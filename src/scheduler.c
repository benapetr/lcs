// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "scheduler.h"

#include "lease.h"
#include "local_client.h"
#include "log.h"
#include "metrics.h"
#include "move.h"
#include "peer.h"
#include "resources.h"

#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

static int scheduler_is_peer_event(uint32_t event_id)
{
    return event_id == LCS_EPOLL_PEER || (event_id >= LCS_EPOLL_PEER_CONN_BASE && event_id < LCS_EPOLL_HANDSHAKE_BASE + LCS_HANDSHAKE_MAX);
}

static void scheduler_handle_metrics_clients(const scheduler_t *sched)
{
    if (sched->metrics_fd < 0)
        return;
    for (;;)
    {
        int client = accept4(sched->metrics_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (client < 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                lcs_log_debug("metrics accept failed: %s", strerror(errno));
            break;
        }
        lcs_metrics_handle_client(client);
        close(client);
    }
}

static void scheduler_dispatch_event(const scheduler_t *sched, const struct epoll_event *ev)
{
    uint32_t event_id = ev->data.u32;
    if (scheduler_is_peer_event(event_id))
    {
        peer_pump_epoll_event(sched->epoll_fd, ev);
        return;
    }
    if (client_index_from_epoll_id(event_id) >= 0)
    {
        client_pump_epoll_event(sched->epoll_fd, ev);
        return;
    }

    if (!(ev->events & EPOLLIN))
        return;
        
    if (event_id == LCS_EPOLL_LOCAL)
        client_accept(sched->epoll_fd, sched->local_fd);
    else if (event_id == LCS_EPOLL_METRICS)
        scheduler_handle_metrics_clients(sched);
}

void scheduler_exec_subsystems(const scheduler_t *sched)
{
    peer_poll(sched->epoll_fd);
    handshake_expire(sched->epoll_fd);
    client_expire(sched->epoll_fd);
    move_process(sched->epoll_fd);
    lease_process_operations(sched->epoll_fd);
    lease_expire_remote();
    resources_process_hooks(sched->epoll_fd);
    resources_maintain_owned_leases(sched->epoll_fd);
    resources_auto_place(sched->epoll_fd);
}

int scheduler_run_once(const scheduler_t *sched)
{
    struct epoll_event events[64];

    // Check if any of the open sockets has buffered data waiting for ingestion
    int rc = epoll_wait(sched->epoll_fd, events, 64, LCS_DEFAULT_LOOP_TIMEOUT_MS);
    if (rc < 0)
    {
        if (errno == EINTR)
            return 0;
        lcs_log_error("epoll_wait failed: %s", strerror(errno));
        return -1;
    }

    // Process all incoming frames
    for (int i = 0; i < rc; i++)
        scheduler_dispatch_event(sched, &events[i]);

    scheduler_exec_subsystems(sched);
    return 0;
}
