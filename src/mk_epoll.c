/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Monkey HTTP Server
 *  ==================
 *  Copyright 2001-2014 Monkey Software LLC <eduardo@monkey.io>
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#include <monkey/monkey.h>
#include <monkey/mk_connection.h>
#include <monkey/mk_socket.h>
#include <monkey/mk_clock.h>
#include <monkey/mk_request.h>
#include <monkey/mk_config.h>
#include <monkey/mk_scheduler.h>
#include <monkey/mk_epoll.h>
#include <monkey/mk_utils.h>
#include <monkey/mk_macros.h>
#include <monkey/mk_linuxtrace.h>
#include <monkey/mk_scheduler.h>

static __thread struct epoll_state_index mk_epoll_state_k;

/*
 * Initialize the epoll state index per worker thread, every index struct contains
 * a fixed array of epoll_state entries and two mk_list to represent an available and
 * busy queue for each entry
 */
int mk_epoll_state_worker_init()
{
    int i;
    struct epoll_state *es;
    struct epoll_state_index *index = &mk_epoll_state_k;

    memset(index, '\0', sizeof(struct epoll_state_index));
    index->size  = config->worker_capacity;

    index->rb_queue = RB_ROOT;
    mk_list_init(&index->busy_queue);
    mk_list_init(&index->av_queue);

    for (i = 0; i < index->size; i++) {
        es = mk_mem_malloc_z(sizeof(struct epoll_state));
        mk_list_add(&es->_head, &index->av_queue);
    }

    return 0;
}

/* Release all entries in the epoll states */
int mk_epoll_state_worker_exit()
{
    struct mk_list *head;
    struct mk_list *tmp;
    struct epoll_state *es;
    struct epoll_state_index *index = &mk_epoll_state_k;

    /* Remove information being held by the available queue */
    mk_list_foreach_safe(head, tmp, &index->av_queue) {
        es = mk_list_entry(head, struct epoll_state, _head);
        mk_mem_free(es);
    }

    /* Remove information being held by the busy queue */
    mk_list_foreach_safe(head, tmp, &index->busy_queue) {
        es = mk_list_entry(head, struct epoll_state, _head);
        mk_mem_free(es);
    }

    return 0;
}

struct epoll_state *mk_epoll_state_get(int fd)
{
  	struct rb_node *node;
    struct epoll_state *es_entry;
    const struct epoll_state_index *index = &mk_epoll_state_k;

    node = index->rb_queue.rb_node;
  	while (node) {
  		es_entry = container_of(node, struct epoll_state, _rb_head);
		if (fd < es_entry->fd)
  			node = node->rb_left;
		else if (fd > es_entry->fd)
  			node = node->rb_right;
		else {
            MK_LT_EPOLL_STATE(fd, es_entry->mode, "GET");
  			return es_entry;
        }
	}

    MK_LT_EPOLL_STATE(fd, 0, "GET: NOT FOUND");
	return NULL;
}

inline struct epoll_state *mk_epoll_state_set(int fd, uint8_t mode,
                                              unsigned int behavior,
                                              uint32_t events)
{
    int i;
    struct epoll_state_index *index = &mk_epoll_state_k;
    struct epoll_state *es_entry = NULL, *es_tmp;

    /*
     * Lets check if we are in the thread context, if dont, this can be the
     * situation when the file descriptor is new and comes from the parent
     * server loop and is just being assigned to the worker thread
     */
    if (mk_unlikely(index->size <= 0)) {
        return NULL;
    }

    /* Lookup entry */
    es_entry = mk_epoll_state_get(fd);

    /* Add new entry to the list */
    if (!es_entry) {
        /* check if we have available slots */
        if (mk_list_is_empty(&index->av_queue) == 0) {

            /* We need to grow the epoll_states list */
            es_tmp = mk_mem_malloc(sizeof(struct epoll_state) * MK_EPOLL_STATE_INDEX_CHUNK);
            for (i = 0; i < MK_EPOLL_STATE_INDEX_CHUNK; i++) {
                mk_list_add(&es_tmp[i]._head, &index->av_queue);
            }
            MK_TRACE("state index grow from %i to %i\n",
                     index->size, index->size + MK_EPOLL_STATE_INDEX_CHUNK);
            index->size += MK_EPOLL_STATE_INDEX_CHUNK;
        }

        /* New entry */
        es_entry = mk_list_entry_first(&index->av_queue, struct epoll_state, _head);
        es_entry->fd       = fd;
        es_entry->mode     = mode;
        es_entry->behavior = behavior;
        es_entry->events   = events;

        /* Unlink from available queue and link to busy queue */
        mk_list_del(&es_entry->_head);
        mk_list_add(&es_entry->_head, &index->busy_queue);

        /* Red-Black tree insert routine */
        struct rb_node **new = &(index->rb_queue.rb_node);
        struct rb_node *parent = NULL;

        /* Figure out where to put new node */
        while (*new) {
            struct epoll_state *this = container_of(*new, struct epoll_state, _rb_head);

            parent = *new;
            if (es_entry->fd < this->fd)
                new = &((*new)->rb_left);
            else if (es_entry->fd > this->fd)
                new = &((*new)->rb_right);
            else {
                break;
            }
        }

        /* Add new node and rebalance tree. */
        rb_link_node(&es_entry->_rb_head, parent, new);
        rb_insert_color(&es_entry->_rb_head, &index->rb_queue);

        MK_LT_EPOLL_STATE(fd, es_entry->mode, "SET: NEW");
        return es_entry;
    }

    /*
     * Sleep mode: the sleep mode disable the events in the epoll queue so the Kernel
     * will not trigger any events, when mode == MK_EPOLL_SLEEP, the epoll_state events
     * keeps the previous events state which can be used in the MK_EPOLL_WAKEUP routine.
     *
     * So we just touch the events and behavior state fields if mode != MK_EPOLL_SLEEP.
     */
    if (mode != MK_EPOLL_SLEEP) {
        es_entry->events   = events;
        es_entry->behavior = behavior;
    }

    /* Update current mode */
    es_entry->mode = mode;
    MK_LT_EPOLL_STATE(fd, es_entry->mode, "SET: CHANGE");
    return es_entry;
}

static int mk_epoll_state_del(int fd)
{
    struct epoll_state *es_entry;
    struct epoll_state_index *index = &mk_epoll_state_k;

    es_entry = mk_epoll_state_get(fd);
    if (es_entry) {
        rb_erase(&es_entry->_rb_head, &index->rb_queue);
        mk_list_del(&es_entry->_head);
        mk_list_add(&es_entry->_head, &index->av_queue);

        MK_LT_EPOLL_STATE(fd, es_entry->mode, "DELETE");
        return 0;
    }

    MK_LT_EPOLL_STATE(fd, 0, "DELETE: NOT FOUND");
    return -1;
}

static int mk_epoll_timeout_set(int efd, int exp)
{
    int ret;
    int timer_fd;
    struct itimerspec its;
    struct epoll_event event = {0, {0}};

    /* expiration interval */
    its.it_interval.tv_sec  = exp;
    its.it_interval.tv_nsec = 0;

    /* initial expiration */
    its.it_value.tv_sec  = time(NULL) + exp;
    its.it_value.tv_nsec = 0;

    timer_fd = timerfd_create(CLOCK_REALTIME, 0);
    if (timer_fd == -1) {
        perror("timerfd");
        exit(EXIT_FAILURE);
    }

    ret = timerfd_settime(timer_fd, TFD_TIMER_ABSTIME, &its, NULL);
    if (ret < 0) {
        perror("timerfd_settime");
        exit(EXIT_FAILURE);
    }

    /* register the timer into the epoll queue */
    event.data.fd = timer_fd;
    event.events  = EPOLLIN;
    ret = epoll_ctl(efd, EPOLL_CTL_ADD, timer_fd, &event);
    if (ret < 0) {
        perror("epoll_ctl");
        exit(EXIT_FAILURE);
    }

    return timer_fd;
}

int mk_epoll_create()
{
    int efd;

    efd = epoll_create1(EPOLL_CLOEXEC);
    if (efd == -1) {
        mk_libc_error("epoll_create");
    }
    return efd;
}

void *mk_epoll_init(int server_fd, int efd, int max_events)
{
    int i, fd, ret = -1;
    int num_fds;
    int remote_fd;
    int timeout_fd;
    uint64_t val = 0;
    struct epoll_event *events;
    struct sched_list_node *sched;

    /* Get thread conf */
    sched = mk_sched_get_thread_conf();

    timeout_fd = mk_epoll_timeout_set(efd, config->timeout);
    events = mk_mem_malloc_z(max_events * sizeof(struct epoll_event));

    pthread_mutex_lock(&mutex_worker_init);
    sched->initialized = 1;
    pthread_mutex_unlock(&mutex_worker_init);

    while (1) {
        ret = -1;
        num_fds = epoll_wait(efd, events, max_events, MK_EPOLL_WAIT_TIMEOUT);

        for (i = 0; i < num_fds; i++) {
            fd = events[i].data.fd;

            if (events[i].events & EPOLLIN) {
                MK_LT_EPOLL(fd, "EPOLLIN");
                MK_TRACE("[FD %i] EPoll Event READ", fd);

                /* Check if we have a worker signal */
                if (mk_unlikely(fd == sched->signal_channel)) {
                    ret = read(fd, &val, sizeof(val));
                    if (ret > 0) {
                        if (val == MK_SCHEDULER_SIGNAL_DEADBEEF) {
                            mk_sched_sync_counters();
                            continue;
                        }
                        else if (val == MK_SCHEDULER_SIGNAL_FREE_ALL) {
                            mk_sched_worker_free();
                            mk_mem_free(events);
                            return NULL;
                        }
                    }
                }
                else if (mk_unlikely(fd == timeout_fd)) {
                    MK_LT_EPOLL(0, "TIMEOUT CHECK");
                    ret = read(fd, &val, sizeof(val));
                    if (ret < 0) {
                        perror("read");
                    }
                    else {
                        mk_sched_check_timeouts(sched);
                    }
                    continue;
                }

                /* New connection under MK_SCHEDULER_REUSEPORT mode */
                if (fd == server_fd) {

                    /* Make sure the worker have enough slots */
                    if (mk_sched_check_capacity(sched) == -1) {
                        continue;
                    }

                    remote_fd = mk_socket_accept(server_fd);
                    if (mk_unlikely(remote_fd == -1)) {
#ifdef TRACE
                        MK_TRACE("Could not accept connection");
#endif
                        continue;
                    }
#ifdef TRACE
                    MK_TRACE("New connection arrived: FD %i", remote_fd);
#endif
                    /* Register new connection into the scheduler */
                    ret = mk_sched_add_client_reuseport(remote_fd, sched);
                    if (ret == -1) {
                        mk_warn("Server over capacity");
                        close(remote_fd);
                        continue;
                    }
                    mk_sched_register_client(remote_fd, sched);
                    fd = remote_fd;
                }
                ret = mk_conn_read(fd);
            }
            else if (events[i].events & EPOLLOUT) {
                MK_LT_EPOLL(fd, "EPOLLOUT");
                MK_TRACE("[FD %i] EPoll Event WRITE", fd);

                ret = mk_conn_write(fd);
            }
            else if (events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
#ifdef LINUX_TRACE
                if (events[i].events & (EPOLLHUP))   MK_LT_EPOLL(fd, "EPOLLHUP");
                if (events[i].events & (EPOLLERR))   MK_LT_EPOLL(fd, "EPOLLERR");
                if (events[i].events & (EPOLLRDHUP)) MK_LT_EPOLL(fd, "EPOLLRDHUP");
#endif
                MK_TRACE("[FD %i] EPoll Event EPOLLHUP/EPOLLERR/EPOLLRDHUP", fd);
                mk_conn_close(fd, MK_EP_SOCKET_CLOSED);
                ret = 0;
            }

            if (ret < 0) {
                MK_LT_EPOLL(fd, "FORCED CLOSE");
                MK_TRACE("[FD %i] Epoll Event FORCE CLOSE | ret = %i", fd, ret);
                mk_conn_close(fd, MK_EP_SOCKET_CLOSED);
            }
        }
    }

    return NULL;
}

int mk_epoll_add(int efd, int fd, int init_mode, unsigned int behavior)
{
    int ret;
    struct epoll_event event = {0, {0}};

    event.data.fd = fd;
    event.events = EPOLLERR | EPOLLHUP | EPOLLRDHUP;

    if (behavior == MK_EPOLL_EDGE_TRIGGERED) {
        event.events |= EPOLLET;
    }

    switch (init_mode) {
    case MK_EPOLL_READ:
        event.events |= EPOLLIN;
        break;
    case MK_EPOLL_WRITE:
        event.events |= EPOLLOUT;
        break;
    case MK_EPOLL_RW:
        event.events |= EPOLLIN | EPOLLOUT;
        break;
    case MK_EPOLL_SLEEP:
        event.events = 0;
        break;
    case MK_EPOLL_HANGUP:
        break;
    }

    /* Add to epoll queue */
    ret = epoll_ctl(efd, EPOLL_CTL_ADD, fd, &event);
    if (mk_unlikely(ret < 0 && errno != EEXIST)) {
        MK_TRACE("[FD %i] epoll_ctl() %s", fd, strerror(errno));
        return ret;
    }

    /* Add to event state list */
    mk_epoll_state_set(fd, init_mode, behavior, event.events);

    return ret;
}

int mk_epoll_del(int efd, int fd)
{
    int ret;

    ret = epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL);
    MK_TRACE("[FD %i] Epoll, remove from QUEUE_FD=%i, ret=%i", fd, efd, ret);

#ifdef TRACE
    if (ret < 0) {
        mk_libc_error("epoll_ctl");
    }
#endif

    /* remove epoll state */
    mk_epoll_state_del(fd);

    return ret;
}

int mk_epoll_change_mode(int efd, int fd, int mode, unsigned int behavior)
{
    int ret;
    struct epoll_event event = {0, {0}};
    struct epoll_state *state;

    event.events = EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    event.data.fd = fd;

    switch (mode) {
    case MK_EPOLL_HANGUP:
        MK_TRACE("[FD %i] Epoll chaning mode to HANGUP", fd);
        break;
    case MK_EPOLL_READ:
        MK_TRACE("[FD %i] EPoll changing mode to READ", fd);
        event.events |= EPOLLIN;
        break;
    case MK_EPOLL_WRITE:
        MK_TRACE("[FD %i] EPoll changing mode to WRITE", fd);
        event.events |= EPOLLOUT;
        break;
    case MK_EPOLL_RW:
        MK_TRACE("[FD %i] Epoll changing mode to READ/WRITE", fd);
        event.events |= EPOLLIN | EPOLLOUT;
        break;
    case MK_EPOLL_SLEEP:
        MK_TRACE("[FD %i] Epoll changing mode to DISABLE", fd);
        event.events = 0;
        break;
    case MK_EPOLL_WAKEUP:
        state = mk_epoll_state_get(fd);
        if (!state) {
            mk_warn("[FD %i] MK_EPOLL_WAKEUP error, invalid connection",
                    fd);
            return -1;
        }
        else if (state->mode == MK_EPOLL_SLEEP) {
            event.events = state->events;
            behavior     = state->behavior;
        }
        else {
            mk_warn("[FD %i] MK_EPOLL_WAKEUP error, current mode is %i",
                    fd, state->mode);
            return -1;
        }
        break;
    }

    if (behavior == MK_EPOLL_EDGE_TRIGGERED) {
        event.events |= EPOLLET;
    }

    /* Update epoll fd events */
    ret = epoll_ctl(efd, EPOLL_CTL_MOD, fd, &event);
#ifdef TRACE
    if (ret < 0) {
        mk_libc_error("epoll_ctl");
        MK_TRACE("[FD %i] epoll_ctl() = %i", fd, ret);
    }
#endif

    /* Update state */
    mk_epoll_state_set(fd, mode, behavior, event.events);
    return ret;
}
