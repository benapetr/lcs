// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_MOVE_H
#define LCS_MOVE_H

#include "daemon_state.h"

#include <stdint.h>

// This handles the VIP move operation (usually initiated by lcs move via CLU)

int move_start_local_client(int epoll_fd, int local_slot,
                            uint32_t client_seq, const void *payload,
                            uint32_t len);
int move_start_peer_request(int epoll_fd, int source_node_idx,
                            uint32_t peer_seq, const void *payload,
                            uint32_t len);
void move_process(int epoll_fd);
void move_cancel_local_client(int local_slot, uint64_t local_client_id);

#endif
