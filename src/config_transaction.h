// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_TRANSACTION_H
#define LCS_CONFIG_TRANSACTION_H

#include "config_coordinator.h"
#include "config_transfer.h"

#define LCS_CONFIG_TX_FORMAT 1
#define LCS_CONFIG_TX_PREFIX 18u
#define LCS_CONFIG_TX_CONTROL_MAX 256u

enum { LCS_CONFIG_TX_PREPARE = 1, LCS_CONFIG_TX_PROMISE = 2,
       LCS_CONFIG_TX_ACK = 3, LCS_CONFIG_TX_LEARNED = 4,
       LCS_CONFIG_TX_SYNC_QUERY = 5, LCS_CONFIG_TX_SYNC_STATUS = 6 };
enum { LCS_CONFIG_TX_OK = 0, LCS_CONFIG_TX_REJECTED = 1, LCS_CONFIG_TX_STALE = 2,
       LCS_CONFIG_TX_NOT_READY = 3, LCS_CONFIG_TX_BUSY = 4, LCS_CONFIG_TX_UNKNOWN = 5 };

typedef struct
{
    uint8_t kind, result;
    uint64_t sender_instance, target_instance, id, revision, ballot, promised, accepted_ballot;
    char membership[LCS_CONFIG_DIGEST_SIZE];
    char parent[LCS_CONFIG_DIGEST_SIZE];
    char value[LCS_CONFIG_DIGEST_SIZE]; /* Empty when no accepted value is reported. */
} lcs_config_tx_control_t;

int lcs_config_tx_encode(const lcs_config_tx_control_t *control, void *payload, size_t cap, size_t *length);
int lcs_config_tx_decode(const void *payload, size_t length, lcs_config_tx_control_t *control);
/* Prefix for BEGIN/CHUNK/END. Existing bounded transfer codecs form the body. */
int lcs_config_tx_wrap(uint64_t sender, uint64_t target, const void *body, size_t body_length,
                        void *payload, size_t cap, size_t *length);
int lcs_config_tx_unwrap(const void *payload, size_t length, uint64_t *sender, uint64_t *target,
                          const void **body, size_t *body_length);

/* Transport adapter callbacks must use an admitted configured peer and wire-6
 * session. Queue returns zero only after copying the complete payload. */
typedef int (*lcs_config_tx_send_t)(void *context, size_t node, uint16_t type,
                                    const void *payload, size_t length);

typedef struct
{
    uint64_t instance;
    lcs_config_transfer_t incoming;
    lcs_config_tx_control_t response;
    lcs_config_transfer_metadata_t outgoing_metadata;
    lcs_config_value_t outgoing;
    uint32_t offset;
    uint8_t stage;
    uint64_t deadline_ms;
} lcs_config_tx_peer_t;

typedef enum
{
    LCS_CONFIG_PROPOSER_IDLE = 0,
    LCS_CONFIG_PROPOSER_RESERVING,
    LCS_CONFIG_PROPOSER_PREPARING,
    LCS_CONFIG_PROPOSER_PROMISES,
    LCS_CONFIG_PROPOSER_ACCEPTING,
    LCS_CONFIG_PROPOSER_ACKS,
    LCS_CONFIG_PROPOSER_LEARNING,
    LCS_CONFIG_PROPOSER_DISTRIBUTING,
    LCS_CONFIG_PROPOSER_COMPLETE,
    LCS_CONFIG_PROPOSER_UNKNOWN,
} lcs_config_proposer_phase_t;

typedef struct
{
    uint64_t instance;
    lcs_config_tx_control_t promise;
    lcs_config_transfer_t incoming;
    uint32_t offset;
    uint8_t stage;
    bool sent;
} lcs_config_proposer_peer_t;

typedef struct
{
    lcs_config_proposal_t proposal;
    lcs_config_proposer_peer_t peers[LCS_MAX_NODES];
    uint64_t id, deadline_ms, completion;
    lcs_config_proposer_phase_t phase;
} lcs_config_network_proposal_t;

typedef enum
{
    LCS_CONFIG_SYNC_IDLE = 0,
    LCS_CONFIG_SYNC_QUERYING,
    LCS_CONFIG_SYNC_RECEIVING,
    LCS_CONFIG_SYNC_PERSISTING,
    LCS_CONFIG_SYNC_CURRENT,
    LCS_CONFIG_SYNC_COMPLETE,
    LCS_CONFIG_SYNC_UNKNOWN,
} lcs_config_sync_phase_t;

typedef struct
{
    lcs_config_tx_control_t query, status;
    uint64_t deadline_ms, completion, next_ms, serial;
    size_t node, cursor;
    lcs_config_sync_phase_t phase;
    bool sent;
} lcs_config_sync_t;

typedef struct
{
    lcs_config_coordinator_t *coordinator;
    uint64_t instance;
    lcs_config_tx_peer_t peers[LCS_MAX_NODES];
    lcs_config_tx_control_t pending;
    uint64_t completion;
    size_t pending_node, cursor;
    uint8_t pending_kind;
    bool active, reply_pending;
    lcs_config_network_proposal_t proposer;
    lcs_config_sync_t sync;
} lcs_config_transactions_t;

void lcs_config_transactions_init(lcs_config_transactions_t *transactions,
                                    lcs_config_coordinator_t *coordinator, uint64_t instance);
/* PREPARE and complete ACCEPT/LEARN values submit only through the coordinator.
 * No storage reads/syncs in the receiver. Non-ready nodes reject before allocation.
 * Replies are counted only for an explicitly started, instance-bound proposal. */
int lcs_config_transactions_receive(lcs_config_transactions_t *transactions, size_t node,
                                     uint64_t peer_instance, uint16_t type,
                                     const void *payload, size_t length, uint64_t now_ms);
/* Explicit internal proposal entry point; does not initialize/activate a cluster.
 * instances contains only admitted wire-6 sessions (zero for unavailable peers),
 * including this daemon's own instance. IDs must strictly increase per daemon.
 * A healthy opened request history and an unused request ID are mandatory.
 * The original requested identity is reserved through the shared coordinator
 * before local PREPARE; both are durable before any proposal frames are sent.
 * Known IDs are status/recovery cases, never permission to submit again, even
 * with a new transport ID. Missing history is not initialized here. Completion means
 * the chosen value was learned locally; it can be a recovered earlier request.
 * UNKNOWN requires a new ballot/recovery, never assumes the request was aborted.
 * No application, CLI mutation, automatic retry or reconnect catch-up here. */
int lcs_config_transactions_propose(lcs_config_transactions_t *transactions,
                                     const uint64_t instances[LCS_MAX_NODES],
                                     uint64_t id, const lcs_config_value_t *requested, uint64_t now_ms);
/* Origin submission variant: reserve a canonical client descriptor digest in
 * the SAME durable write as original intent, before PREPARE. Empty retains the
 * internal/peer behavior. This evidence stays local; it is not sent to peers.
 * Known IDs still require status lookup, never another proposal. */
int lcs_config_transactions_propose_client(lcs_config_transactions_t *transactions,
                                            const uint64_t instances[LCS_MAX_NODES],
                                            uint64_t id, const lcs_config_value_t *requested,
                                            const char *client_digest, uint64_t now_ms);
/* Selected phase-two value; a decision is known only in DISTRIBUTING/COMPLETE. */
const lcs_config_value_t *lcs_config_transactions_selected(const lcs_config_transactions_t *transactions);
/* Pull one admitted peer's latest committed checkpoint, including revision gaps.
 * Uses the existing readiness barrier and writer, retaining applied/pending state.
 * A strictly increasing session-local ID is required; no initialization occurs. */
int lcs_config_transactions_sync(lcs_config_transactions_t *transactions, size_t node,
                                  uint64_t peer_instance, uint64_t id, uint64_t now_ms);
/* Caller advances the shared coordinator first. At most nine queued frames per
 * call; bounded snapshot output retries do not extend the original deadline. */
void lcs_config_transactions_process(lcs_config_transactions_t *transactions, uint64_t now_ms,
                                      lcs_config_tx_send_t send, void *context);
/* Disconnect drops session buffers/replies, but NEVER cancels a durable write. */
void lcs_config_transactions_disconnect(lcs_config_transactions_t *transactions, size_t node);
void lcs_config_transactions_close(lcs_config_transactions_t *transactions);
#endif
