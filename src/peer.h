// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_PEER_H
#define LCS_PEER_H

#include "daemon_state.h"

#include <signal.h>
#include <stdint.h>
#include <sys/epoll.h>

/* Globals defined in lcsd.c */
extern volatile sig_atomic_t g_stop;
extern int g_peer_listener_fd;

void handshake_expire(daemon_state_t *st, int epoll_fd);
void handshake_close(daemon_state_t *st, int epoll_fd, int slot_idx, const char *reason);
void peer_close_connection(daemon_state_t *st, int epoll_fd, int node_idx, bool mark_offline, const char *reason);
int peer_rpc(daemon_state_t *st, int epoll_fd, int node_idx, uint16_t req_type,
             const void *req_payload, uint32_t req_len,
             uint16_t expected_type, unsigned char *resp_payload,
             size_t resp_cap, uint32_t *resp_len, uint32_t timeout_ms);
void peer_broadcast_state_sync(daemon_state_t *st, int epoll_fd);
void peer_pump_epoll_event(daemon_state_t *st, int epoll_fd, const struct epoll_event *ev);
void peer_poll(daemon_state_t *st, int epoll_fd);

#endif
