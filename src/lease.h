// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_LEASE_H
#define LCS_LEASE_H

#include "daemon_state.h"

#include <stddef.h>

int lease_encode_msg(unsigned char *payload, size_t cap, size_t *len,
                     uint16_t resource_id, uint16_t owner_node,
                     uint64_t epoch, uint64_t lease_id, uint32_t lease_ms,
                     uint64_t sender_instance_id);

int lease_decode_msg(const daemon_state_t *st, const void *payload, size_t len,
                     uint16_t *resource_id, uint16_t *owner_node,
                     uint64_t *epoch, uint64_t *lease_id, uint32_t *lease_ms,
                     uint64_t *sender_instance_id);

int lease_accept_message(daemon_state_t *st, uint16_t type, const void *payload,
                         size_t len, int source_node_idx);

int lease_start_acquire(daemon_state_t *st, int vip_idx, int owner_idx,
                        uint64_t epoch, uint64_t lease_id, int epoll_fd);
int lease_start_renew(daemon_state_t *st, int vip_idx, int epoll_fd);
bool lease_operation_active(const daemon_state_t *st, int vip_idx);
void lease_cancel_operations(daemon_state_t *st, int vip_idx);
void lease_process_operations(daemon_state_t *st, int epoll_fd);

void lease_release_majority(daemon_state_t *st, int vip_idx, int owner_idx,
                             uint64_t epoch, uint64_t lease_id, int epoll_fd);

int lease_handle_owner_release_request(daemon_state_t *st, const void *payload, size_t len,
                                 int source_node_idx);

void lease_expire_remote(daemon_state_t *st);

#endif
