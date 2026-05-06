// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_LOCAL_CLIENT_H
#define LCS_LOCAL_CLIENT_H

#include "daemon_state.h"

#include <stdint.h>
#include <sys/epoll.h>

void client_complete_move(int epoll_fd, int slot_idx, uint64_t client_id, uint32_t seq, int32_t status, const char *message);
void client_accept(int epoll_fd, int listen_fd);
void client_pump_epoll_event(int epoll_fd, const struct epoll_event *ev);
void client_expire(int epoll_fd);
void client_close_all(int epoll_fd);
int  client_index_from_epoll_id(uint32_t id);

#endif
