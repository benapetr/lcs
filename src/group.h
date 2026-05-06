// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_GROUP_H
#define LCS_GROUP_H

int group_auto_place_target(int vip_idx);
int group_move_anchor_vip(int vip_idx);
void group_log_strict_warnings(void);
void group_rebalance(int epoll_fd);

#endif
