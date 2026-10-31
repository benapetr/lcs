// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_READINESS_H
#define LCS_CONFIG_READINESS_H

#include "config_barrier.h"

#define LCS_CONFIG_READINESS_FORMAT 1
#define LCS_CONFIG_READINESS_MAX 192u
#define LCS_CONFIG_READINESS_TIMEOUT_MS 10000u

enum { LCS_READINESS_QUERY = 1, LCS_READINESS_REPLY = 2 };
enum { LCS_READINESS_READY = 1, LCS_READINESS_NOT_READY = 2, LCS_READINESS_MISMATCH = 3 };

typedef struct
{
    uint8_t kind;
    uint64_t requester_instance;
    uint64_t attempt;
    uint64_t responder_instance;
    uint8_t status;
    char membership[LCS_CONFIG_DIGEST_SIZE];
    char baseline[LCS_CONFIG_DIGEST_SIZE];
} lcs_config_readiness_message_t;

typedef struct
{
    lcs_config_barrier_round_t round;
    lcs_config_readiness_message_t query;
    uint64_t expected_instance[LCS_MAX_NODES];
    uint64_t deadline_ms;
    uint32_t sent;
    bool active;
} lcs_config_readiness_t;

int lcs_config_readiness_encode(const lcs_config_readiness_message_t *message,
                                 void *payload, size_t capacity, size_t *length);
int lcs_config_readiness_decode(const void *payload, size_t length,
                                 lcs_config_readiness_message_t *message);
/* The caller excludes INI reload/storage workers before starting, and uses a
 * strictly increasing attempt within its daemon instance. No file I/O here. */
int lcs_config_readiness_begin(lcs_config_readiness_t *state,
                                const lcs_config_activation_t *activation,
                                const lcs_config_agreement_t *agreement,
                                const lcs_config_application_t *application,
                                uint64_t requester_instance, uint64_t attempt, uint64_t now_ms,
                                char *err, size_t err_len);
/* Record the currently admitted peer instance before queuing this query.
 * Returns 1 when no query is needed, -1 on invalid state/expiry, 0 when ready.
 * Retries/reconnections never extend the fixed deadline. */
int lcs_config_readiness_query(lcs_config_readiness_t *state, size_t node,
                                uint64_t peer_instance, uint64_t now_ms,
                                lcs_config_readiness_message_t *query);
/* Returns 1 for stale/unsolicited replies, -1 for expiry or conflicting current
 * evidence, 0 for a matching reply. NOT_READY remains pending. Only an explicit
 * READY from the queried, currently admitted instance counts toward the mask.
 * Matching readiness already recorded is durable evidence across disconnects. */
int lcs_config_readiness_reply(lcs_config_readiness_t *state, size_t node,
                                uint64_t peer_instance, uint64_t now_ms,
                                const lcs_config_readiness_message_t *reply,
                                char *err, size_t err_len);
bool lcs_config_readiness_complete(const lcs_config_readiness_t *state);
/* Advance the deadline even when all unconfirmed peers are disconnected. */
int lcs_config_readiness_poll(lcs_config_readiness_t *state, uint64_t now_ms);
#endif
