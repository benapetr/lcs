// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_EPOLL_UTIL_H
#define LCS_EPOLL_UTIL_H

#include <stdint.h>

int add_epoll_fd(int epoll_fd, int fd, uint32_t id);
int add_epoll_fd_events(int epoll_fd, int fd, uint32_t id, uint32_t events);
int set_fd_nonblocking(int fd);

#endif
