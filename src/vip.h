// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_VIP_H
#define LCS_VIP_H

#include "config.h"

void lcs_vip_set_backend(lcs_vip_backend_t backend);
int lcs_vip_add(const lcs_vip_config_t *vip);
int lcs_vip_del(const lcs_vip_config_t *vip);
int lcs_vip_announce(const lcs_config_t *cfg, const lcs_vip_config_t *vip);
/* Returns 1 for confirmed conflict, 0 for no conflict, -1 for probe failure. */
int lcs_vip_conflict_check(const lcs_config_t *cfg, const lcs_vip_config_t *vip);

#endif
