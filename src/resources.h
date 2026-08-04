// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_RESOURCES_H
#define LCS_RESOURCES_H

#include "daemon_state.h"

void resources_begin_startup_cleanup(void);
void resources_progress_startup_cleanup(int epoll_fd);
bool resources_startup_cleanup_complete(void);
bool resources_preserve_startup_cleanup_failure(int resource_idx, uint64_t incoming_epoch);
int  resources_stop_local_backend(const lcs_resource_config_t *res);
int  resources_begin_state_replacement(int resource_idx, int owner_node,
                                       uint64_t owner_instance_id,
                                       lcs_resource_state_t state,
                                       uint64_t epoch, uint64_t lease_id,
                                       uint64_t deadline_ms,
                                       const char *reason, int epoll_fd);
void resources_enter_conflict_state(int resource_idx, uint64_t epoch, const char *reason);
void resources_enter_stop_failed_state(int resource_idx, uint64_t epoch, const char *reason, int epoll_fd);
int  resources_activate_acquired_local(int resource_idx, uint64_t epoch, uint64_t lease_id, int epoll_fd);
int  resources_activate_local(int resource_idx, uint64_t epoch, int epoll_fd);
void resources_release_local(int resource_idx, int epoll_fd);
int  resources_release_for_handoff(int resource_idx, uint64_t epoch,
                                   uint64_t lease_id, int source_node_idx,
                                   uint32_t response_seq, int epoll_fd);
void resources_drop_local(int resource_idx, int epoll_fd);
void resources_begin_graceful_shutdown(int epoll_fd);
void resources_progress_graceful_shutdown(int epoll_fd);
bool resources_graceful_shutdown_complete(void);
void resources_finish_graceful_shutdown(void);
int  resources_set_disabled(int resource_idx, bool disabled, int epoll_fd, char *message, size_t message_len);
void resources_auto_place(int epoll_fd);
void resources_home_rebalance(int epoll_fd);
void resources_maintain_owned_leases(int epoll_fd);
void resources_process_hooks(int epoll_fd);
void resources_process_vip_operations(int epoll_fd);
void resources_process_service_operations(int epoll_fd);
uint32_t resources_service_operation_timeout_ms(void);

#endif
