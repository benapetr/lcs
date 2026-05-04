// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_CLUSTER_H
#define LCS_CLUSTER_H

#include "daemon_state.h"

#include <stddef.h>

bool node_is_online(const daemon_state_t *st, size_t node_idx);
int first_online_full_member(const daemon_state_t *st);
const char *node_name_or_none(const daemon_state_t *st, int node_idx);
int has_quorum(const daemon_state_t *st);
void recompute_votes(daemon_state_t *st);
uint32_t peer_handshake_timeout_ms(const daemon_state_t *st);

int encode_state(const daemon_state_t *st, unsigned char *payload, size_t cap, size_t *len);
int apply_state(daemon_state_t *st, const void *payload, size_t len, int source_node_idx);

#endif
