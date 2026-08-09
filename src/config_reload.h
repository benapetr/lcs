// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef LCS_CONFIG_RELOAD_H
#define LCS_CONFIG_RELOAD_H

#include "daemon_state.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Cluster-coordinated resource configuration reload
 * --------------------------------------------------
 *
 * A reload is a small distributed state machine, not an immediate replacement
 * of g_state.cfg.  SIGHUP calls config_reload_request(); receiving another
 * node's announcement may request the same local load automatically.  Each
 * node reads its own config file into g_state.config_reload.candidate, validates
 * it, and continues using the active config until the protocol commits.
 *
 * The normal flow is:
 *
 *   load -> announce -> agree -> drain -> ready -> commit -> remap
 *
 * 1. The requester snapshots itself and every established peer into a fixed
 *    participant set.  The set must contain quorum, at least one full member,
 *    and only nodes supporting reload; members already offline are excluded.
 * 2. Every participant loads the candidate and announces identical voting/full
 *    fingerprints and the same participant set.  Agreement is acknowledged by
 *    every participant before draining starts.  This locks the candidate so
 *    two different proposals cannot partially drain the cluster.
 * 3. Each node stops only locally owned resources that disappear or whose
 *    backend identity changes (VIP address/interface, type, or systemd unit).
 *    Unchanged resources keep running and continue renewing their leases.
 * 4. Once every participant is ready, the lowest-index participant broadcasts
 *    the commit.  A short delay lets nonblocking peer queues deliver it before
 *    connections close.  Losing a participant before this decision aborts the
 *    attempt; a retry captures the new online set.
 * 5. Runtime resources and voter grants are remapped by resource name, never by
 *    their old sorted array index.  Unchanged resources preserve ownership,
 *    epochs, leases, and administrative state; new/replaced resources start
 *    stopped.  Peer connections are then rebuilt using the new schema.
 *
 * A rejected/withdrawn candidate never changes the active config.  HELLO also
 * carries a commit proof so participants can converge after a partially
 * delivered commit.  An excluded node retains its old active configuration and
 * receives a non-voting commit proof when it reconnects.  Deploy the committed
 * file and run `lcs reload`; it drains its old local resources, adopts the
 * committed schema, and rejoins through normal recovery without a restart.
 *
 * Scope and invariants:
 *
 * - This protocol changes resources and groups only.  Membership, node roles,
 *   peer addresses, quorum timing, listeners, and daemon-wide settings remain
 *   immutable and require a restart or a future joint-consensus protocol.
 * - Every node online when the proposal starts must participate, and that fixed
 *   set must contain quorum.  Nodes already offline do not block commit.
 * - When an excluded full member could retain an old changed resource, commit
 *   waits a full lease interval before the new backend may become active.
 * - Reload messages are allowed only when every online participant negotiated
 *   LCS_PROTO_FEATURE_RESOURCE_RELOAD.  The global effective version may be
 *   lower because offline nodes conservatively contribute the minimum version.
 * - Call config_reload_process() from the scheduler after asynchronous backend
 *   operations have progressed, so drain completion is observed without ever
 *   blocking the event loop.
 */

/* Initialize reload state and remember the path each node reloads locally. */
void config_reload_init(const char *config_path);

/* Request candidate loading; safe work is deferred to config_reload_process(). */
void config_reload_request(void);

/* Advance agreement, drain, and commit from the daemon scheduler. */
void config_reload_process(int epoll_fd);

/* True while a candidate is loading or participating in the protocol. */
bool config_reload_in_progress(void);

/* True when the current online set has quorum and every online member supports
 * the coordinated reload transaction.  Offline members are not participants. */
bool config_reload_supported_by_online_quorum(void);

/* True after a committed peer proved that this node's active schema is stale. */
bool config_reload_catchup_available(void);

/* Wire helpers used by peer.c for candidate announcements and commit messages. */
int config_reload_encode_announcement(void *payload, size_t cap, size_t *len);
int config_reload_handle_announcement(const void *payload, size_t len, int source_node_idx);
int config_reload_handle_commit(const void *payload, size_t len, int source_node_idx);

#endif
