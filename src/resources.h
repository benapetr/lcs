// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_RESOURCES_H
#define LCS_RESOURCES_H

#include "daemon_state.h"

void resources_cleanup_local_vips_without_lease(daemon_state_t *st);
void resources_enter_conflict_state(daemon_state_t *st, int vip_idx, uint64_t epoch, const char *reason);
int  resources_activate_local(daemon_state_t *st, int vip_idx, uint64_t epoch, int epoll_fd);
void resources_release_local(daemon_state_t *st, int vip_idx, int epoll_fd);
void resources_graceful_shutdown(daemon_state_t *st, int epoll_fd);
void resources_auto_place(daemon_state_t *st, int epoll_fd);
void resources_maintain_owned_leases(daemon_state_t *st, int epoll_fd);
void resources_process_hooks(daemon_state_t *st, int epoll_fd);

#endif
