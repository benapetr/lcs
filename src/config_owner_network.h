// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_OWNER_NETWORK_H
#define LCS_CONFIG_OWNER_NETWORK_H
#include "config_owner_barrier.h"
#include "config_reconcile.h"

typedef int (*lcs_config_owner_send_t)(void *context, size_t node, const void *payload, size_t length);
typedef struct
{
    const lcs_config_t *base;
    const lcs_config_application_t *application;
    size_t self;
    uint64_t instance, serial, next_query_ms;
    uint64_t instances[LCS_MAX_NODES];
    char membership[LCS_CONFIG_DIGEST_SIZE];
    lcs_config_owner_receipt_t receipt;
    lcs_config_owner_round_t round;
    lcs_config_owner_message_t replies[LCS_MAX_NODES];
    uint32_t pending_replies;
    bool closing;
} lcs_config_owner_network_t;

/* Fresh object only. Borrow the immutable static base and recovered catalog
 * until shutdown. A NULL catalog cannot seal stop proof (legacy nodes).
 * This does not start cleanup, seal stop proof or authorize application. */
int lcs_config_owner_network_init(lcs_config_owner_network_t *network, const lcs_config_t *base,
                                   const lcs_config_application_t *application, size_t self, uint64_t instance);
/* Explicit entry after local drain/fencing; scope must match this local catalog.
 * One pinned scope only. Another scope is blocked until a future distributed
 * receipt-release/history protocol exists; never clear receipts to advance. */
int lcs_config_owner_network_start(lcs_config_owner_network_t *network,
                                    const lcs_config_owner_scope_t *scope, uint64_t now_ms);
/* Reconciliation owner callback, ONLY after local drain. The concrete live
 * initializer supplies the corresponding revoke callback and install recheck. */
int lcs_config_owner_network_step(void *context, const lcs_config_reconcile_view_t *view, char *err, size_t err_len);
/* Explicit retry of a failed collection, preserving the stop receipt and scope.
 * Failed local cleanup is terminal and cannot be repaired by a network retry. */
int lcs_config_owner_network_retry(lcs_config_owner_network_t *network, uint64_t now_ms);
void lcs_config_owner_network_revoke(void *context);
/* Established configured wire-6 source admission is the transport's job.
 * At most one copied response per peer; backpressure retries do not extend
 * collection deadlines. Valid FAILED replies block collection, not the socket. */
int lcs_config_owner_network_receive(lcs_config_owner_network_t *network, size_t node, uint64_t instance,
                                      const void *payload, size_t length, uint64_t now_ms);
void lcs_config_owner_network_process(lcs_config_owner_network_t *network,
                                        const uint64_t instances[LCS_MAX_NODES], uint64_t now_ms,
                                        lcs_config_owner_send_t send, void *context);
void lcs_config_owner_network_disconnect(lcs_config_owner_network_t *network, size_t node);
void lcs_config_owner_network_stop(lcs_config_owner_network_t *network);
#endif
