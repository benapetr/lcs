// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_owner_network.h"
#include "util.h"
#include <stdio.h>
#include <string.h>

int lcs_config_owner_network_init(lcs_config_owner_network_t *network, const lcs_config_t *base,
                                   const lcs_config_application_t *application, size_t self, uint64_t instance)
{
    if (!network || !base || !instance || !base->node_count || base->node_count > LCS_MAX_NODES || self >= base->node_count)
        return -1;
    lcs_config_owner_network_t next = { .base = base, .application = application, .self = self, .instance = instance };
    if (lcs_config_agreement_membership(base, next.membership))
        return -1;
    next.instances[self] = instance;
    *network = next;
    return 0;
}

int lcs_config_owner_network_start(lcs_config_owner_network_t *network,
                                    const lcs_config_owner_scope_t *scope, uint64_t now_ms)
{
    lcs_config_owner_scope_t current;
    char error[256];
    if (!network || !network->instance || network->closing || !network->application || !scope ||
        network->serial == UINT64_MAX || network->base->nodes[network->self].role != LCS_NODE_FULL ||
        lcs_config_owner_scope(network->application, &current, error, sizeof(error)) ||
        !lcs_config_owner_scope_equal(&current, scope))
        return -1;
    lcs_config_owner_round_t round = network->round;
    lcs_config_owner_receipt_t receipt = network->receipt;
    if (lcs_config_owner_receipt_seal(&receipt, scope, network->instance) ||
        lcs_config_owner_begin(&round, network->base, scope, network->self, network->instances, network->serial + 1, now_ms) ||
        lcs_config_owner_local(&round, &receipt))
        return -1;
    network->round = round; network->receipt = receipt; network->serial++;
    network->next_query_ms = 0;
    return 0;
}

int lcs_config_owner_network_retry(lcs_config_owner_network_t *network, uint64_t now_ms)
{
    if (!network || !network->round.active || !network->round.failed || network->receipt.failed)
        return -1;
    return lcs_config_owner_network_start(network, &network->receipt.scope, now_ms);
}

void lcs_config_owner_network_revoke(void *context)
{
    lcs_config_owner_network_t *network = context;
    if (!network || !network->instance)
        return;
    lcs_config_owner_receipt_revoke(&network->receipt);
    network->round.failed = true;
    /* A queued STOPPED reply must never escape after local cleanup failed. */
    for (size_t i = 0; i < network->base->node_count; i++)
        if (network->pending_replies & (UINT32_C(1) << i))
            network->replies[i].result = LCS_CONFIG_OWNER_FAILED;
}

int lcs_config_owner_network_step(void *context, const lcs_config_reconcile_view_t *view, char *err, size_t err_len)
{
    lcs_config_owner_network_t *network = context;
    lcs_config_owner_scope_t scope;
    if (!network || network->closing || !view || !view->value || !view->plan || !network->application ||
        !lcs_config_value_equal(view->value, &network->application->pending) ||
        lcs_config_owner_scope(network->application, &scope, err, err_len) || scope.drain != view->plan->drain)
        goto blocked;
    if (!network->round.active)
    {
        if (lcs_config_owner_network_start(network, &scope, lcs_now_ms()))
            goto blocked;
    }
    else if (!lcs_config_owner_scope_equal(&network->round.scope, &scope))
        goto blocked;
    if (lcs_config_owner_local(&network->round, &network->receipt))
        goto blocked;
    int result = lcs_config_owner_poll(&network->round, lcs_now_ms());
    if (result >= 0)
        return result;
blocked:
    if (err && err_len)
        snprintf(err, err_len, "old-owner stop barrier blocked; retain cleanup fences and pinned receipts");
    return -1;
}

int lcs_config_owner_network_receive(lcs_config_owner_network_t *network, size_t node, uint64_t instance,
                                      const void *payload, size_t length, uint64_t now_ms)
{
    lcs_config_owner_message_t message;
    if (!network || !network->instance || network->closing || node >= network->base->node_count || node == network->self ||
        network->base->nodes[node].role != LCS_NODE_FULL || network->base->nodes[network->self].role != LCS_NODE_FULL ||
        !instance || lcs_config_owner_decode(payload, length, &message) || message.sender_instance != instance ||
        message.target_instance != network->instance || strcmp(message.scope.membership, network->membership))
        return -1;
    network->instances[node] = instance;
    if (network->round.active)
        (void)lcs_config_owner_observe(&network->round, node, instance);
    if (message.kind == LCS_CONFIG_OWNER_QUERY)
    {
        if (lcs_config_owner_answer(&network->receipt, network->instance, instance, &message, &network->replies[node]))
            return -1;
        network->pending_replies |= UINT32_C(1) << node;
    }
    else
        (void)lcs_config_owner_reply(&network->round, node, instance, now_ms, &message);
    return 0;
}

void lcs_config_owner_network_process(lcs_config_owner_network_t *network,
                                        const uint64_t instances[LCS_MAX_NODES], uint64_t now_ms,
                                        lcs_config_owner_send_t send, void *context)
{
    if (!network || !network->instance || network->closing || !instances || !send)
        return;
    if (instances[network->self] != network->instance)
    {
        lcs_config_owner_network_revoke(network);
        return;
    }
    for (size_t i = 0; i < network->base->node_count; i++)
    {
        network->instances[i] = instances[i];
        if (network->round.active)
            (void)lcs_config_owner_observe(&network->round, i, instances[i]);
        uint32_t bit = UINT32_C(1) << i;
        if (!(network->pending_replies & bit))
            continue;
        if (!instances[i] || network->replies[i].target_instance != instances[i])
        { network->pending_replies &= ~bit; continue; }
        unsigned char payload[LCS_CONFIG_OWNER_MESSAGE_SIZE]; size_t length;
        if (!lcs_config_owner_encode(&network->replies[i], payload, sizeof(payload), &length) &&
            !send(context, i, payload, length))
            network->pending_replies &= ~bit;
    }
    if (!network->round.active || lcs_config_owner_poll(&network->round, now_ms) || now_ms < network->next_query_ms)
        return;
    network->next_query_ms = now_ms > UINT64_MAX - 250u ? UINT64_MAX : now_ms + 250u;
    for (size_t i = 0; i < network->base->node_count; i++)
    {
        if (i == network->self)
            continue;
        lcs_config_owner_message_t query;
        unsigned char payload[LCS_CONFIG_OWNER_MESSAGE_SIZE]; size_t length;
        if (!lcs_config_owner_query(&network->round, i, instances[i], now_ms, &query) &&
            !lcs_config_owner_encode(&query, payload, sizeof(payload), &length))
            (void)send(context, i, payload, length);
    }
}

void lcs_config_owner_network_disconnect(lcs_config_owner_network_t *network, size_t node)
{
    if (!network || !network->instance || node >= network->base->node_count)
        return;
    network->instances[node] = 0;
    network->pending_replies &= ~(UINT32_C(1) << node);
    if (network->round.active)
        (void)lcs_config_owner_observe(&network->round, node, 0);
}

void lcs_config_owner_network_stop(lcs_config_owner_network_t *network)
{
    if (!network || !network->instance)
        return;
    lcs_config_owner_network_revoke(network);
    network->closing = true;
    network->pending_replies = 0;
}
