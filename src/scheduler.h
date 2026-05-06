// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_SCHEDULER_H
#define LCS_SCHEDULER_H

#include "daemon_state.h"

typedef struct
{
    int epoll_fd;
    int local_fd;
    int metrics_fd;
} scheduler_t;

int scheduler_run_once(const scheduler_t *sched);
void scheduler_exec_subsystems(const scheduler_t *sched);

#endif
