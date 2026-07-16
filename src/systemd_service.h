// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_SYSTEMD_SERVICE_H
#define LCS_SYSTEMD_SERVICE_H

#include "config.h"

int lcs_systemd_service_start(const lcs_vip_config_t *res);
int lcs_systemd_service_stop(const lcs_vip_config_t *res);
int lcs_systemd_service_is_active(const lcs_vip_config_t *res);

#endif
