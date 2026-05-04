// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_METRICS_H
#define LCS_METRICS_H

#include "daemon_state.h"

void lcs_metrics_handle_client(int fd, const daemon_state_t *st);

#endif
