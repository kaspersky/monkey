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

#define _GNU_SOURCE
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <sys/time.h>
#include <sys/resource.h>

#include <monkey/monkey.h>
#include <monkey/mk_config.h>
#include <monkey/mk_scheduler.h>
#include <monkey/mk_epoll.h>
#include <monkey/mk_socket.h>
#include <monkey/mk_plugin.h>
#include <monkey/mk_utils.h>
#include <monkey/mk_macros.h>

/* Return the number of clients that can be attended
 * at the same time per worker thread
 */
unsigned int mk_server_worker_capacity(unsigned short nworkers)
{
    unsigned int max, avl;
    struct rlimit lim;

    /* Limit by system */
    getrlimit(RLIMIT_NOFILE, &lim);
    max = lim.rlim_cur;

    /* Minimum of fds needed by Monkey:
     * --------------------------------
     * 3 fds: stdin, stdout, stderr
     * 1 fd for main socket server
     * 1 fd for epoll array (per thread)
     * 1 fd for worker logger when writing to FS
     * 2 fd for worker logger pipe
     */

    avl = max - (3 + 1 + nworkers + 1 + 2);

    /* The avl is divided by two as we need to consider
     * a possible additional FD for each plugin working
     * on the same request.
     */
    return ((avl / 2) / nworkers);
}

#ifndef SHAREDLIB

/* Here we launch the worker threads to attend clients */
void mk_server_launch_workers()
{
    int i;
    pthread_t skip;

    /* Launch workers */
    for (i = 0; i < config->workers; i++) {
        mk_sched_launch_thread(config->worker_capacity, &skip, NULL);
    }
}

void mk_server_loop(int server_fd)
{
    int ret;
    int remote_fd;

    /* check balancing mode, for reuse port just stay here forever */
    if (config->scheduler_mode == MK_SCHEDULER_REUSEPORT) {
        while (1) sleep(60);
    }

    /* Activate TCP_DEFER_ACCEPT */
    if (mk_socket_set_tcp_defer_accept(server_fd) != 0) {
            mk_warn("TCP_DEFER_ACCEPT failed");
    }

    /* Rename worker */
    mk_utils_worker_rename("monkey: server");

    mk_info("HTTP Server started");

    while (1) {
        remote_fd = mk_socket_accept(server_fd);

        if (mk_unlikely(remote_fd == -1)) {
            continue;
        }

#ifdef TRACE
        MK_TRACE("New connection arrived: FD %i", remote_fd);

        int i;
        struct sched_list_node *node;

        node = sched_list;
        for (i=0; i < config->workers; i++) {
            MK_TRACE("Worker Status");
            MK_TRACE(" WID %i / conx = %llu", node[i].idx, node[i].accepted_connections - node[i].closed_connections);
        }
#endif

        /* Assign socket to worker thread */
        ret = mk_sched_add_client(remote_fd);
        if (ret == -1) {
            mk_socket_close(remote_fd);
        }
    }
}

#endif // !SHAREDLIB
