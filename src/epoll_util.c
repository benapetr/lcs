// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "epoll_util.h"

#include <fcntl.h>
#include <string.h>
#include <sys/epoll.h>

int add_epoll_fd(int epoll_fd, int fd, uint32_t id)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
    ev.data.u32 = id;
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

int add_epoll_fd_events(int epoll_fd, int fd, uint32_t id, uint32_t events)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.u32 = id;
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

int set_fd_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
