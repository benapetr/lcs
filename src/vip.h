// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_VIP_H
#define LCS_VIP_H

#include "config.h"

#include <sys/types.h>

void lcs_vip_set_backend(lcs_vip_backend_t backend);
int lcs_vip_add(const lcs_resource_config_t *vip);
int lcs_vip_del(const lcs_resource_config_t *vip);
int lcs_vip_announce(const lcs_config_t *cfg, const lcs_resource_config_t *vip);
int lcs_vip_conflict_check_async(const lcs_config_t *cfg,
                                 const lcs_resource_config_t *vip,
                                 pid_t *pid);
/* Returns 0 while pending, 1 when complete, and -1 on waitpid failure. */
int lcs_vip_probe_collect(pid_t pid, int *result);
/* Requests termination; collection and reaping remain nonblocking. */
void lcs_vip_probe_cancel(pid_t pid);

#endif
