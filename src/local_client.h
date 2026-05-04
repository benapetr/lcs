// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_LOCAL_CLIENT_H
#define LCS_LOCAL_CLIENT_H

#include "daemon_state.h"

#include <stdint.h>

void send_error(int fd, uint32_t seq, const char *msg);
int compute_move_response(daemon_state_t *st, const void *payload, uint32_t len,
                          int epoll_fd, int32_t *status, char *message,
                          size_t message_len);
void handle_client(int fd, daemon_state_t *st, int epoll_fd);

#endif
