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

#include "monkey.h"
#include "mk_http.h"
#include "mk_plugin.h"
#include "mk_macros.h"
#include "mk_stats.h"
#include "mk_scheduler.h"

int mk_conn_read(int socket, struct sched_list_node *__sched)
{
    int ret;
    struct client_session *cs;

    MK_TRACE("[FD %i] Connection Handler / read", socket);

    STATS_COUNTER_START_NO_SCHED(mk_conn_read);

    /* Plugin hook */
    ret = mk_plugin_event_read(socket, __sched);

    switch(ret) {
    case MK_PLUGIN_RET_EVENT_OWNED:
        STATS_COUNTER_STOP_NO_SCHED(mk_conn_read);
        return MK_PLUGIN_RET_CONTINUE;
    case MK_PLUGIN_RET_EVENT_CLOSE:
        STATS_COUNTER_STOP_NO_SCHED(mk_conn_read);
        return -1;
    case MK_PLUGIN_RET_EVENT_CONTINUE:
        break; /* just return controller to invoker */
    }

    cs = mk_session_get(socket, __sched);
    if (!cs) {
        /* Check if is this a new connection for the Scheduler */
        if (!mk_sched_get_connection(__sched, socket)) {
            MK_TRACE("[FD %i] Registering new connection");
            if (mk_sched_register_client(socket, __sched) == -1) {
                MK_TRACE("[FD %i] Close requested", socket);
                STATS_COUNTER_STOP_NO_SCHED(mk_conn_read);
                return -1;
            }
            /*
             * New connections are not registered yet into the epoll
             * event state list, we need to do it manually
             */
            mk_epoll_state_set(socket,
                               MK_EPOLL_READ,
                               MK_EPOLL_LEVEL_TRIGGERED,
                               (EPOLLERR | EPOLLHUP | EPOLLRDHUP | EPOLLIN));
            STATS_COUNTER_STOP_NO_SCHED(mk_conn_read);
            return 0;
        }

        /* Create session for the client */
        MK_TRACE("[FD %i] Create session", socket);
        cs = mk_session_create(socket, __sched);
        if (!cs) {
            STATS_COUNTER_STOP_NO_SCHED(mk_conn_read);
            return -1;
        }
    }

    /* Read incomming data */
    ret = mk_handler_read(socket, cs, __sched);
    if (ret > 0) {
        if (mk_http_pending_request(cs, __sched) == 0) {
            mk_epoll_change_mode(__sched->epoll_fd,
                                 socket, MK_EPOLL_WRITE, MK_EPOLL_LEVEL_TRIGGERED);
        }
        else if (cs->body_length + 1 >= (unsigned int) config->max_request_size) {
            /*
             * Request is incomplete and our buffer is full,
             * close connection
             */
            mk_session_remove(socket, __sched);
            STATS_COUNTER_STOP_NO_SCHED(mk_conn_read);
            return -1;
        }
        else {
            MK_TRACE("[FD %i] waiting for pending data", socket);
        }
    }

    STATS_COUNTER_STOP_NO_SCHED(mk_conn_read);
    return ret;
}

int mk_conn_write(int socket, struct sched_list_node *__sched)
{
    int ret = -1;
    struct client_session *cs;
    struct sched_connection *conx;

    STATS_COUNTER_START_NO_SCHED(mk_conn_write);

    MK_TRACE("[FD %i] Connection Handler / write", socket);

    /* Plugin hook */
    ret = mk_plugin_event_write(socket, __sched);
    switch(ret) {
    case MK_PLUGIN_RET_EVENT_OWNED:
        STATS_COUNTER_STOP_NO_SCHED(mk_conn_write);
        return MK_PLUGIN_RET_CONTINUE;
    case MK_PLUGIN_RET_EVENT_CLOSE:
        STATS_COUNTER_STOP_NO_SCHED(mk_conn_write);
        return -1;
    case MK_PLUGIN_RET_EVENT_CONTINUE:
        break; /* just return controller to invoker */
    }

    MK_TRACE("[FD %i] Normal connection write handling", socket);

    conx = mk_sched_get_connection(__sched, socket);
    if (!conx) {
        MK_TRACE("[FD %i] Registering new connection");
        if (mk_sched_register_client(socket, __sched) == -1) {
            MK_TRACE("[FD %i] Close requested", socket);
            STATS_COUNTER_STOP_NO_SCHED(mk_conn_write);
            return -1;
        }

        mk_epoll_change_mode(__sched->epoll_fd, socket,
                             MK_EPOLL_READ, MK_EPOLL_LEVEL_TRIGGERED);
        STATS_COUNTER_STOP_NO_SCHED(mk_conn_write);
        return 0;
    }

    mk_sched_update_conn_status(__sched, socket, MK_SCHEDULER_CONN_PROCESS);

    /* Get node from schedule list node which contains
     * the information regarding to the current client/socket
     */
    cs = mk_session_get(socket, __sched);
    if (!cs) {
        /* This is a ghost connection that doesn't exist anymore.
         * Closing it could accidentally close some other thread's
         * socket, so pass it to remove_client that checks it's ours.
         */
        mk_sched_remove_client(__sched, socket);
        STATS_COUNTER_STOP_NO_SCHED(mk_conn_write);
        return 0;
    }

    ret = mk_handler_write(socket, cs, __sched);

    /* if ret < 0, means that some error
     * happened in the writer call, in the
     * other hand, 0 means a successful request
     * processed, if ret > 0 means that some data
     * still need to be send.
     */
    if (ret < 0) {
        mk_request_free_list(cs);
        mk_session_remove(socket, __sched);
        STATS_COUNTER_STOP_NO_SCHED(mk_conn_write);
        return -1;
    }
    else if (ret == 0) {
        STATS_COUNTER_STOP_NO_SCHED(mk_conn_write);
        return mk_http_request_end(socket, __sched);
    }
    else if (ret > 0) {
        STATS_COUNTER_STOP_NO_SCHED(mk_conn_write);
        return 0;
    }

    /* avoid to make gcc cry :_( */
    STATS_COUNTER_STOP_NO_SCHED(mk_conn_write);
    return -1;
}

int mk_conn_close(int socket, int event)
{
    struct sched_list_node *sched;

    MK_TRACE("[FD %i] Connection Handler, closed", socket);

    /*
     * Remove the socket from the scheduler and make sure
     * to disable all notifications.
     */
    sched = mk_sched_get_thread_conf();
    mk_sched_remove_client(sched, socket);

    /* Plugin hook: this is a wrap-workaround to do not
     * break plugins until the whole interface events and
     * return values are re-worked.
     */
    if (event == MK_EP_SOCKET_CLOSED) {
        mk_plugin_event_close(socket);
    }
    else if (event == MK_EP_SOCKET_ERROR) {
        mk_plugin_event_error(socket);
    }
    else if (event == MK_EP_SOCKET_TIMEOUT) {
        mk_plugin_event_timeout(socket);
    }

    return 0;
}
