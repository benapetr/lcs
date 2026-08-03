// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_CLI_SERVER_H
#define LCS_CLI_SERVER_H

#include "daemon_state.h"

#include <stdint.h>
#include <sys/epoll.h>

/*
 * Server for the local lcs command-line interface. lcs connects to lcsd over
 * the configured Unix socket and sends framed status, resource-control,
 * conflict-clear, or move requests. This is separate from peer.c, which
 * handles daemon-to-daemon cluster traffic over TCP.
 *
 * Most requests receive one response and the connection is then closed. Move
 * requests may remain open until the distributed handoff completes.
 */
void cli_server_complete_move(int epoll_fd, int slot_idx, uint64_t cli_server_id, uint32_t seq, int32_t status, const char *message);
void cli_server_accept(int epoll_fd, int listen_fd);
void cli_server_pump_epoll_event(int epoll_fd, const struct epoll_event *ev);
void cli_server_expire(int epoll_fd);
void cli_server_close_all(int epoll_fd);
int  cli_server_index_from_epoll_id(uint32_t id);

#endif
