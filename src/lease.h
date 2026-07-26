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

int lease_decode_msg(const void *payload, size_t len,
                     uint16_t *resource_id, uint16_t *owner_node,
                     uint64_t *epoch, uint64_t *lease_id, uint32_t *lease_ms,
                     uint64_t *sender_instance_id);

int  lease_accept_message(uint16_t type, const void *payload, size_t len, int source_node_idx);
int  lease_apply_commit(const void *payload, size_t len, int source_node_idx, int epoll_fd);

// Record/release this daemon's own voter promise.  Move operations use the
// same grant rules as normal automatic acquisition.
int  lease_grant_local_acquire(int resource_idx, int owner_idx, uint64_t epoch,
                               uint64_t lease_id, uint64_t deadline_ms);
void lease_grant_local_release(int resource_idx, int owner_idx, uint64_t epoch,
                               uint64_t lease_id);

// Announce ownership only after a majority acquisition or renewal completes.
void lease_broadcast_commit(int epoll_fd, int resource_idx, int owner_idx,
                            uint64_t epoch, uint64_t lease_id,
                            uint64_t deadline_ms);

// Begin an asynchronous majority lease acquisition for a resource. The caller
// provides the proposed owner, new epoch, and lease ID; the lease subsystem
// asks peers for LCS_MSG_LEASE_REQ votes and later activates the resource only
// if a quorum acknowledges the same lease.
int  lease_start_acquire(int resource_idx, int owner_idx, uint64_t epoch, uint64_t lease_id, int epoll_fd);

// Begin an asynchronous lease renewal for a resource currently owned by this daemon
// instance. Peers must acknowledge the existing epoch and lease ID before the
// local lease deadline is extended.
int  lease_start_renew(int resource_idx, int epoll_fd);

bool lease_operation_active(int resource_idx);

// Drop any in-flight acquire, renew, or release operation for the resource. Used
// when higher-level resource state changes make the outstanding operation stale.
void lease_cancel_operations(int resource_idx);
void lease_cancel_all_operations(void);

void lease_process_operations(int epoll_fd);

// Broadcast a lease release to peers so a majority can forget an old ownership
// record. Used after failed acquisition, local shutdown/drop, and controlled
// handoff so stale votes do not continue to protect an old owner.
void lease_release_majority(int resource_idx, int owner_idx, uint64_t epoch, uint64_t lease_id, int epoll_fd);

// Handle a controlled handoff request from the target node. The current owner
// validates that the request matches its active epoch and lease ID, removes the
// resource locally, and confirms release before the target is allowed to activate.
int  lease_handle_owner_release_request(const void *payload, size_t len, int source_node_idx, int epoll_fd);

// Expire remote ownership records whose lease deadline has passed. This only
// clears local cluster state for leases owned by other daemon instances; it does
// not remove local resources.
void lease_expire_remote(void);

#endif
