// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_SYSTEMD_SERVICE_H
#define LCS_SYSTEMD_SERVICE_H

#include "config.h"

#include <sys/types.h>

/*
 * Systemd D-Bus operations run in worker processes so a slow system manager
 * can never block the daemon's lease, peer, or hook event loop.
 */
int lcs_systemd_service_start_async(const lcs_resource_config_t *res,
                                    pid_t *pid);
int lcs_systemd_service_stop_async(const lcs_resource_config_t *res,
                                   pid_t *pid);
int lcs_systemd_service_check_async(const lcs_resource_config_t *res,
                                    pid_t *pid);

/* Returns 0 while pending, 1 when complete, and -1 on waitpid failure. */
int lcs_systemd_service_collect(pid_t pid, int *result);
/* Requests termination; collection and reaping remain nonblocking. */
void lcs_systemd_service_cancel(pid_t pid);

#endif
