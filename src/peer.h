// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_PEER_H
#define LCS_PEER_H

#include "daemon_state.h"

#include <stdint.h>
#include <sys/epoll.h>

/* Globals defined in lcsd.c */
extern int g_peer_listener_fd;

void handshake_expire(int epoll_fd);
void handshake_close(int epoll_fd, int slot_idx, const char *reason);
void peer_close_connection(int epoll_fd, int node_idx, bool mark_offline, const char *reason);
int peer_rpc_async(int epoll_fd, int node_idx, uint16_t req_type,
                   const void *req_payload, uint32_t req_len,
                   uint16_t expected_type, unsigned char *resp_payload,
                   size_t resp_cap, uint32_t *resp_len, uint32_t timeout_ms,
                   peer_rpc_callback_t callback, void *callback_ctx);
int peer_queue_simple_resp(int epoll_fd, int node_idx,
                           uint32_t seq, uint16_t type, int32_t status,
                           const char *message);
void peer_broadcast_state_sync(int epoll_fd);
void peer_pump_epoll_event(int epoll_fd, const struct epoll_event *ev);
void peer_poll(int epoll_fd);

#endif
