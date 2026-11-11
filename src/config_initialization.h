// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_INITIALIZATION_H
#define LCS_CONFIG_INITIALIZATION_H

#include "config_agreement.h"

#define LCS_CONFIG_INITIALIZATION_TIMEOUT_MS 10000u

enum { LCS_INITIALIZATION_FROZEN = 1, LCS_INITIALIZATION_BUSY, LCS_INITIALIZATION_MISMATCH };

/* Same-process collection state, never a wire/disk proof. The future upgrade
 * transport must obtain explicit freeze acknowledgements, not translate version
 * advertisements into receipts. Acknowledging a freeze must exclude INI reload
 * until durable initialization/recovery takes over; a collector timeout does NOT
 * authorize the responder to thaw. The durable FROZEN storage primitive exists;
 * its explicit peer exchange and fenced startup resumption exist, but automatic
 * initialization/activation coordination is not implemented.
 * begin requires the caller to exclude local reload, including queued requests. */
typedef struct
{
    uint64_t requester_instance, attempt, responder_instance;
    uint8_t status;
    char membership[LCS_CONFIG_DIGEST_SIZE], baseline[LCS_CONFIG_DIGEST_SIZE];
} lcs_config_initialization_receipt_t;

typedef struct
{
    char membership[LCS_CONFIG_DIGEST_SIZE], baseline[LCS_CONFIG_DIGEST_SIZE];
    uint64_t instances[LCS_MAX_NODES];
    uint64_t attempt, local_generation, deadline_ms;
    uint32_t required, frozen, queried;
    size_t self, count;
    bool active, sealed, failed, consumed;
} lcs_config_initialization_round_t;

/* instances contains only currently admitted, upgrade-capable sessions, zero
 * otherwise (including disconnected/unknown/old binaries). Require EVERY member,
 * including quorum-only. A fresh attempt is strictly increasing per instance. */
int lcs_config_initialization_begin(lcs_config_initialization_round_t *round,
    const lcs_config_t *cfg, const uint64_t instances[LCS_MAX_NODES],
    uint64_t generation, uint64_t attempt, uint64_t now_ms, char *err, size_t err_len);
/* Register a query before queuing it to this exact admitted instance. The
 * returned same-process query has status zero (never valid as a receipt).
 * Returns 1 if already confirmed, 0 if a query should be sent, -1 on failure. */
int lcs_config_initialization_query(lcs_config_initialization_round_t *round,
    size_t node, uint64_t admitted_instance, uint64_t now_ms,
    lcs_config_initialization_receipt_t *query, char *err, size_t err_len);
/* Returns 1 for stale/unsolicited receipts, 0 for current evidence, -1 for
 * conflicting evidence/expiry. A current BUSY stays pending; withdrawal of a
 * previously confirmed freeze fails the whole round, including a sealed round. */
int lcs_config_initialization_receipt(lcs_config_initialization_round_t *round,
    size_t node, uint64_t admitted_instance,
    const lcs_config_initialization_receipt_t *receipt, uint64_t now_ms,
    char *err, size_t err_len);
/* Revalidate all session instances and the exact local reload generation before
 * starting storage. Returns 1 when complete, 0 pending, -1 invalid/expired.
 * Disconnect/restart/reload invalidates even a sealed round. No deadline renewal. */
int lcs_config_initialization_poll(lcs_config_initialization_round_t *round,
    const uint64_t instances[LCS_MAX_NODES], uint64_t generation, uint64_t now_ms,
    char *err, size_t err_len);
/* Final worker gate also recomputes the effective definition/membership binding.
 * The worker consumes the round only after fork succeeds and keeps its permanent
 * local reload lock even on cancellation/failure. This is not activation. */
int lcs_config_initialization_authorize(lcs_config_initialization_round_t *round,
    const lcs_config_t *cfg, const uint64_t instances[LCS_MAX_NODES],
    uint64_t generation, uint64_t now_ms, char *err, size_t err_len);

#endif
