// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_OWNER_BARRIER_H
#define LCS_CONFIG_OWNER_BARRIER_H

#include "config_application.h"

#define LCS_CONFIG_OWNER_FORMAT 1
#define LCS_CONFIG_OWNER_MESSAGE_SIZE 243u
#define LCS_CONFIG_OWNER_TIMEOUT_MS 10000u

enum { LCS_CONFIG_OWNER_QUERY = 1, LCS_CONFIG_OWNER_REPLY = 2,
       LCS_CONFIG_OWNER_APPLIED_QUERY = 3, LCS_CONFIG_OWNER_APPLIED_REPLY = 4 };
enum { LCS_CONFIG_OWNER_STOPPED = 1, LCS_CONFIG_OWNER_PENDING = 2, LCS_CONFIG_OWNER_FAILED = 3 };

typedef struct
{
    uint64_t old_revision, target_revision;
    uint32_t drain;
    char membership[LCS_CONFIG_DIGEST_SIZE];
    char old_value[LCS_CONFIG_DIGEST_SIZE], target_value[LCS_CONFIG_DIGEST_SIZE];
} lcs_config_owner_scope_t;

typedef struct
{
    uint8_t kind, result;
    uint64_t sender_instance, target_instance, id;
    lcs_config_owner_scope_t scope;
} lcs_config_owner_message_t;

typedef struct
{
    lcs_config_owner_scope_t scope;
    uint64_t instance;
    bool sealed, failed;
} lcs_config_owner_receipt_t;

typedef struct
{
    lcs_config_owner_scope_t scope;
    uint64_t instance, id, deadline_ms;
    uint64_t expected_instance[LCS_MAX_NODES];
    size_t node_count, self;
    uint32_t required, queried, stopped;
    bool active, sealed, failed;
} lcs_config_owner_round_t;

/* Derive the exact old/pending transition and dependency drain mask from the
 * durable catalog. No I/O and no backend/lease safety is inferred by this call. */
int lcs_config_owner_scope(const lcs_config_application_t *application,
                             lcs_config_owner_scope_t *scope, char *err, size_t err_len);
bool lcs_config_owner_scope_equal(const lcs_config_owner_scope_t *a, const lcs_config_owner_scope_t *b);
int lcs_config_owner_encode(const lcs_config_owner_message_t *message,
                              void *payload, size_t capacity, size_t *length);
int lcs_config_owner_decode(const void *payload, size_t length, lcs_config_owner_message_t *message);

/* Zero-initialize receipt and round objects before first use.
 * Proof primitive used by the explicit peer network adapter; automatic
 * application is not enabled. Seal ONLY after local cleanup including hooks/workers succeeds,
 * restart cleanup is known, and old-generation grants/reactivation are fenced.
 * These guarantees must outlive every consumer of this receipt. The primitive
 * never releases a pinned scope. The network adapter retires it only after all
 * full members confirm exact durable application and retains completion history.
 * There is no restart reuse or timeout release. A later cleanup failure revokes
 * the current receipt; startup reconstruction still requires integration. */
int lcs_config_owner_receipt_seal(lcs_config_owner_receipt_t *receipt,
                                    const lcs_config_owner_scope_t *scope, uint64_t instance);
void lcs_config_owner_receipt_revoke(lcs_config_owner_receipt_t *receipt);
/* Caller first authenticates configured full-member source and wire capability.
 * A query does not start cleanup, initialize state or change the pinned scope.
 * Empty receipts answer PENDING; conflicting/failed receipts answer FAILED. */
int lcs_config_owner_answer(const lcs_config_owner_receipt_t *receipt, uint64_t self_instance,
                              uint64_t peer_instance, const lcs_config_owner_message_t *query,
                              lcs_config_owner_message_t *reply);

/* Fresh round or explicit retry after failure/completion; IDs strictly increase
 * within this daemon. All configured FULL members are required, including self;
 * unavailable members (instance 0) remain required. Quorum-only nodes cannot
 * originate/count as owner proof. The caller supplies only admitted instances.
 * There is deliberately no majority, gossip or elapsed-lease shortcut. */
int lcs_config_owner_begin(lcs_config_owner_round_t *round, const lcs_config_t *base,
                             const lcs_config_owner_scope_t *scope, size_t self,
                             const uint64_t instances[LCS_MAX_NODES], uint64_t id, uint64_t now_ms);
/* Register a locally sealed receipt; self is never automatically acknowledged. */
int lcs_config_owner_local(lcs_config_owner_round_t *round, const lcs_config_owner_receipt_t *receipt);
/* MUST observe session changes and recheck poll immediately before installation,
 * even after completion. Losing/changing a known full-member instance fails
 * the round; a new daemon cannot inherit its predecessor's stop evidence. */
int lcs_config_owner_observe(lcs_config_owner_round_t *round, size_t node, uint64_t instance);
/* 0 query produced, 1 unnecessary, -1 failed/expired. Retry sends cannot extend
 * the deadline; the transport must copy messages before returning to its caller. */
int lcs_config_owner_query(lcs_config_owner_round_t *round, size_t node, uint64_t instance,
                             uint64_t now_ms, lcs_config_owner_message_t *query);
/* 0 matching reply, 1 unsolicited/stale, -1 conflicting/failed/expired proof.
 * Delivery on a configured authenticated session is a caller prerequisite. */
int lcs_config_owner_reply(lcs_config_owner_round_t *round, size_t node, uint64_t instance,
                             uint64_t now_ms, const lcs_config_owner_message_t *reply);
/* 1 all full members explicitly stopped, 0 pending, -1 failed/expired. */
int lcs_config_owner_poll(lcs_config_owner_round_t *round, uint64_t now_ms);
#endif
