// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_CLUSTER_H
#define LCS_CLUSTER_H

#include "daemon_state.h"

#include <stddef.h>

bool        cluster_node_is_online(size_t node_idx);
int         cluster_first_online_full_member(void);
const char *cluster_node_name_or_none(int node_idx);
int         cluster_has_quorum(void);
void        cluster_recompute_votes(void);

// Decode and merge a peer's cluster-state snapshot (mix of VIP ownership, lease epochs, conflict state, and counters)
// received over the peer network protocol.

// Returns 0 on success, -1 on invalid payload or if the sender's instance ID doesn't match
// the expected one for the source node index.
int cluster_apply_state(const void *payload, size_t len, int source_node_idx);

// Encode this daemon's current cluster-state snapshot for transmission to
// peers. Used for periodic state sync and state-sync replies.

// Returns 0 on success, -1 if the payload buffer is too small.
int cluster_encode_state(unsigned char *payload, size_t cap, size_t *len);

#endif
