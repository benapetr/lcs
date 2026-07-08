// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_RESOURCES_H
#define LCS_RESOURCES_H

#include "daemon_state.h"

void resources_cleanup_local_vips_without_lease(void);
void resources_enter_conflict_state(int vip_idx, uint64_t epoch, const char *reason);
int  resources_activate_acquired_local(int vip_idx, uint64_t epoch, uint64_t lease_id, int epoll_fd);
int  resources_activate_local(int vip_idx, uint64_t epoch, int epoll_fd);
void resources_release_local(int vip_idx, int epoll_fd);
int  resources_release_for_handoff(int vip_idx, uint64_t epoch, uint64_t lease_id);
void resources_drop_local(int vip_idx, int epoll_fd);
void resources_graceful_shutdown(int epoll_fd);
int  resources_set_disabled(int vip_idx, bool disabled, int epoll_fd, char *message, size_t message_len);
void resources_auto_place(int epoll_fd);
void resources_home_rebalance(int epoll_fd);
void resources_maintain_owned_leases(int epoll_fd);
void resources_process_hooks(int epoll_fd);

#endif
