// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_RESOURCES_H
#define LCS_RESOURCES_H

#include "daemon_state.h"

void cleanup_local_vips_without_lease(daemon_state_t *st);
void enter_conflict_state(daemon_state_t *st, int vip_idx, uint64_t epoch,
                          const char *reason);
int activate_local_resource(daemon_state_t *st, int vip_idx, uint64_t epoch,
                            int epoll_fd);
void release_local_resource(daemon_state_t *st, int vip_idx, int epoll_fd);
void graceful_shutdown_resources(daemon_state_t *st, int epoll_fd);
void auto_place(daemon_state_t *st, int epoll_fd);
void maintain_owned_leases(daemon_state_t *st, int epoll_fd);
void process_resource_hooks(daemon_state_t *st, int epoll_fd);

#endif
