// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_peer.h"
#include "config_reload.h"
#include "daemon_state.h"
#include "log.h"
#include "peer.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

_Static_assert(LCS_CONFIG_STATUS_DIGEST_SIZE == LCS_CONFIG_DIGEST_SIZE, "digest format mismatch");

static int send_transaction(void *context, size_t node, uint16_t type, const void *payload, size_t length)
{
    return peer_queue_config_transaction(*(int *)context, (int)node, type, payload, (uint32_t)length);
}

int config_peer_receive_transaction(int epoll_fd, int node, uint16_t type, const void *payload, size_t length)
{
    if (node < 0 || (size_t)node >= g_state.cfg.node_count || node == g_state.self_index)
        return -1;
    const peer_runtime_t *peer = &g_state.peers[node];
    if (peer->fd < 0 || peer->conn_state != LCS_PEER_ESTABLISHED || !peer->instance_id ||
        peer->protocol_version < LCS_PROTO_FEATURE_CONFIG_TRANSACTIONS)
        return -1;
    if (!g_state.config_transactions.instance)
        lcs_config_transactions_init(&g_state.config_transactions, &g_state.config_coordinator, g_state.instance_id);
    if (config_reload_in_progress() && g_state.config_coordinator.initialized)
        return -1;
    int result = lcs_config_transactions_receive(&g_state.config_transactions, (size_t)node,
        peer->instance_id, type, payload, length, lcs_now_ms());
    if (!result)
        lcs_config_transactions_process(&g_state.config_transactions, lcs_now_ms(), send_transaction, &epoll_fd);
    return result;
}

static bool durable_readiness_available(void)
{
    const lcs_config_runtime_t *runtime = &g_state.config_runtime;
    return g_state.config_bootstrap_initialized && g_state.config_bootstrap.initialization_locked &&
        !g_state.config_coordinator.closing &&
        !config_reload_in_progress() && runtime->base && runtime->base == runtime->activation.base &&
        lcs_config_barrier_local_ready(&runtime->activation, &runtime->agreement, &runtime->application) &&
        !strcmp(g_state.config_status.membership, runtime->activation.membership) &&
        !strcmp(g_state.config_status.baseline, runtime->activation.baseline.digest);
}

int config_peer_readiness_start(void)
{
    lcs_config_coordinator_t *coordinator = &g_state.config_coordinator;
    if (coordinator->closing || coordinator->failed)
        return -1;
    if (coordinator->initialized && (coordinator->storage.pid > 0 || coordinator->phase != LCS_CONFIG_COORD_IDLE))
        return 1;
    if (g_state.config_readiness.active && !g_state.config_readiness.round.failed &&
        !g_state.config_readiness.round.sealed)
        return 1;
    if (config_peer_refresh() || !durable_readiness_available() || g_state.config_runtime.barrier.store ||
        g_state.config_runtime.barrier.poisoned || g_state.config_readiness_attempt == UINT64_MAX)
        return -1;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        const peer_runtime_t *peer = &g_state.peers[i];
        if ((int)i != g_state.self_index && (peer->fd < 0 || peer->conn_state != LCS_PEER_ESTABLISHED ||
            peer->protocol_version < LCS_PROTO_FEATURE_CONFIG_READINESS || !peer->instance_id))
            return 1;
    }
    char error[256];
    lcs_config_runtime_t *runtime = &g_state.config_runtime;
    int result = lcs_config_readiness_begin(&g_state.config_readiness, &runtime->activation,
        &runtime->agreement, &runtime->application, g_state.instance_id,
        ++g_state.config_readiness_attempt, lcs_now_ms(), error, sizeof(error));
    if (!result)
    {
        if (!coordinator->initialized)
            lcs_config_coordinator_init(coordinator, runtime, &g_state.config_readiness);
        result = lcs_config_coordinator_start(coordinator);
        if (result)
            lcs_config_coordinator_abort(coordinator, "cannot attach readiness attempt to the configuration writer");
    }
    if (!result)
        g_state.config_readiness_next_send_ms = 0;
    return result;
}

void config_peer_stop(void)
{
    g_state.config_coordinator.closing = true;
    lcs_config_coordinator_stop(&g_state.config_coordinator);
    lcs_config_transactions_close(&g_state.config_transactions);
}

bool config_peer_stopped(void)
{
    return lcs_config_coordinator_can_close(&g_state.config_coordinator);
}

void config_peer_shutdown_process(void)
{
    (void)lcs_config_coordinator_process(&g_state.config_coordinator, lcs_now_ms());
}

bool config_peer_configuration_vote_ready(void)
{
    return g_state.config_bootstrap.initialization_locked && !config_reload_in_progress() &&
        lcs_config_coordinator_can_vote(&g_state.config_coordinator);
}

static void process_readiness(int epoll_fd)
{
    lcs_config_readiness_t *state = &g_state.config_readiness;
    lcs_config_coordinator_t *coordinator = &g_state.config_coordinator;
    uint64_t now = lcs_now_ms();
    lcs_config_coordinator_phase_t before = coordinator->phase;
    if (coordinator->initialized && !coordinator->closing &&
        (before == LCS_CONFIG_COORD_COLLECTING || before == LCS_CONFIG_COORD_PERSISTING) &&
        !durable_readiness_available())
        lcs_config_coordinator_abort(coordinator, "local definitions or reload state changed during readiness");
    (void)lcs_config_coordinator_process(coordinator, now);
    if (before == LCS_CONFIG_COORD_PERSISTING && coordinator->phase == LCS_CONFIG_COORD_READY)
        lcs_log_debug("all-member configuration readiness persisted; configuration vote prerequisites satisfied");
    else if (coordinator->phase != before && coordinator->phase == LCS_CONFIG_COORD_FAILED)
        lcs_log_error("configuration storage coordinator failed: %s", coordinator->error);
    if (coordinator->initialized && coordinator->phase != LCS_CONFIG_COORD_COLLECTING)
        return;
    if (!state->active || lcs_config_readiness_complete(state))
        return;
    if (!durable_readiness_available())
        state->round.failed = true;
    if (lcs_config_readiness_poll(state, now) || now < g_state.config_readiness_next_send_ms)
        return;
    g_state.config_readiness_next_send_ms = now + 250u;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        const peer_runtime_t *peer = &g_state.peers[i];
        if ((int)i == g_state.self_index || peer->fd < 0 || peer->conn_state != LCS_PEER_ESTABLISHED ||
            peer->protocol_version < LCS_PROTO_FEATURE_CONFIG_READINESS || !peer->instance_id)
            continue;
        lcs_config_readiness_message_t query;
        unsigned char payload[LCS_CONFIG_READINESS_MAX];
        size_t length;
        if (!lcs_config_readiness_query(state, i, peer->instance_id, now, &query) &&
            !lcs_config_readiness_encode(&query, payload, sizeof(payload), &length))
            (void)peer_queue_config_readiness(epoll_fd, (int)i, LCS_MSG_CONFIG_READINESS_QUERY,
                                               payload, (uint32_t)length);
    }
}

int config_peer_receive_readiness(int epoll_fd, int node, uint16_t type,
                                   const void *payload, size_t length)
{
    if (node < 0 || (size_t)node >= g_state.cfg.node_count || node == g_state.self_index)
        return -1;
    const peer_runtime_t *peer = &g_state.peers[node];
    lcs_config_readiness_message_t message;
    if (peer->fd < 0 || peer->conn_state != LCS_PEER_ESTABLISHED ||
        peer->protocol_version < LCS_PROTO_FEATURE_CONFIG_READINESS || !peer->instance_id ||
        lcs_config_readiness_decode(payload, length, &message))
        return -1;
    if (type == LCS_MSG_CONFIG_READINESS_REPLY && message.kind == LCS_READINESS_REPLY)
    {
        if (message.responder_instance != peer->instance_id)
            return -1;
        if (g_state.config_readiness.active && !durable_readiness_available() &&
            !lcs_config_readiness_complete(&g_state.config_readiness))
            g_state.config_readiness.round.failed = true;
        char error[256];
        int result = lcs_config_readiness_reply(&g_state.config_readiness, (size_t)node,
            peer->instance_id, lcs_now_ms(), &message, error, sizeof(error));
        return result < 0 ? -1 : 0;
    }
    if (type != LCS_MSG_CONFIG_READINESS_QUERY || message.kind != LCS_READINESS_QUERY ||
        message.requester_instance != peer->instance_id)
        return -1;
    const lcs_config_activation_t *activation = &g_state.config_runtime.activation;
    const char *membership = activation->store ? activation->membership : g_state.config_status.membership;
    const char *baseline = activation->store ? activation->baseline.digest : g_state.config_status.baseline;
    message.kind = LCS_READINESS_REPLY;
    message.responder_instance = g_state.instance_id;
    message.status = strcmp(message.membership, membership) || strcmp(message.baseline, baseline) ?
        LCS_READINESS_MISMATCH : durable_readiness_available() ? LCS_READINESS_READY : LCS_READINESS_NOT_READY;
    unsigned char response[LCS_CONFIG_READINESS_MAX];
    size_t response_length;
    if (lcs_config_readiness_encode(&message, response, sizeof(response), &response_length))
        return -1;
    return peer_queue_config_readiness(epoll_fd, node, LCS_MSG_CONFIG_READINESS_REPLY,
                                        response, (uint32_t)response_length) ? -1 : 0;
}

static int withdraw_preparation(void)
{
    if (g_state.config_status.prepared && g_state.config_status.serial != UINT64_MAX)
    {
        g_state.config_status.prepared = 0;
        g_state.config_status.serial++;
    }
    return -1;
}

int config_peer_refresh(void)
{
    lcs_config_status_t next = g_state.config_status;
    bool changed = !next.serial;
    if (!next.serial || g_state.config_status_generation != g_state.config_reload.active_generation)
    {
        char error[256], *document = NULL;
        size_t length;
        lcs_config_value_t value = {0};
        if (lcs_config_agreement_membership(&g_state.cfg, next.membership) ||
            lcs_resource_config_encode(&g_state.cfg, 0, &document, &length, error, sizeof(error)))
            return withdraw_preparation();
        int result = lcs_config_value_init(&value, &g_state.cfg, "bootstrap", document,
                                           length, error, sizeof(error));
        free(document);
        if (result)
            return withdraw_preparation();
        changed = changed || strcmp(next.baseline, value.digest) != 0;
        memcpy(next.baseline, value.digest, sizeof(next.baseline));
        lcs_config_value_clear(&value);
    }
    bool prepared = g_state.config_bootstrap_initialized && g_state.config_bootstrap.prepared &&
        !config_reload_in_progress() && !g_state.config_runtime.requires_recovery;
    changed = changed || next.prepared != prepared;
    next.prepared = prepared;
    if (changed)
    {
        if (next.serial == UINT64_MAX)
            return withdraw_preparation();
        next.serial++;
    }
    g_state.config_status = next;
    g_state.config_status_generation = g_state.config_reload.active_generation;
    return 0;
}

void config_peer_process(int epoll_fd)
{
    process_readiness(epoll_fd);
    lcs_config_transactions_process(&g_state.config_transactions, lcs_now_ms(), send_transaction, &epoll_fd);
    if (lcs_now_ms() < g_state.config_status_retry_ms)
        return;
    if (config_peer_refresh())
    {
        lcs_log_error("cannot prepare configuration peer status; retrying without activation readiness");
        g_state.config_status_retry_ms = lcs_now_ms() + 1000u;
    }
    else
        g_state.config_status_retry_ms = 0;
    if (!g_state.config_status.serial)
        return;
    unsigned char payload[256];
    size_t length;
    if (lcs_config_status_encode(&g_state.config_status, payload, sizeof(payload), &length))
        return;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        peer_runtime_t *peer = &g_state.peers[i];
        if ((int)i == g_state.self_index || peer->config_status_sent == g_state.config_status.serial)
            continue;
        if (!peer_queue_config_status(epoll_fd, (int)i, payload, (uint32_t)length))
            peer->config_status_sent = g_state.config_status.serial;
    }
}

int config_peer_receive(int node, const void *payload, size_t length)
{
    if (node < 0 || (size_t)node >= g_state.cfg.node_count || node == g_state.self_index)
        return -1;
    peer_runtime_t *peer = &g_state.peers[node];
    lcs_config_status_t next;
    if (peer->fd < 0 || peer->conn_state != LCS_PEER_ESTABLISHED ||
        peer->protocol_version < LCS_PROTO_FEATURE_CONFIG_STATUS ||
        lcs_config_status_decode(payload, length, &next) ||
        strcmp(next.membership, g_state.config_status.membership))
        return -1;
    if (peer->config_status_known)
    {
        if (next.serial < peer->config_status.serial)
            return 0;
        if (next.serial == peer->config_status.serial)
            return lcs_config_status_equal(&next, &peer->config_status) ? 0 : -1;
    }
    peer->config_status = next;
    peer->config_status_known = true;
    lcs_log_debug("peer %s resource bootstrap prepared=%s matching=%s serial=%llu",
        g_state.cfg.nodes[node].name, next.prepared ? "true" : "false",
        !strcmp(next.baseline, g_state.config_status.baseline) ? "true" : "false",
        (unsigned long long)next.serial);
    return 0;
}

int config_peer_view(lcs_config_status_view_t *view)
{
    if (config_peer_refresh())
        return -1;
    memset(view, 0, sizeof(*view));
    view->count = (uint16_t)g_state.cfg.node_count;
    view->self = (uint16_t)g_state.self_index;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        lcs_config_status_node_t *node = &view->nodes[i];
        const peer_runtime_t *peer = &g_state.peers[i];
        memcpy(node->name, g_state.cfg.nodes[i].name, sizeof(node->name));
        node->role = (uint16_t)g_state.cfg.nodes[i].role;
        if ((int)i == g_state.self_index)
        {
            node->connected = node->known = 1;
            node->protocol = LCS_PEER_PROTO_VERSION;
            node->status = g_state.config_status;
        }
        else
        {
            node->connected = peer->online && peer->fd >= 0 && peer->conn_state == LCS_PEER_ESTABLISHED;
            node->protocol = node->connected ? peer->protocol_version : 0;
            node->known = node->connected && peer->protocol_version >= LCS_PROTO_FEATURE_CONFIG_STATUS &&
                          peer->config_status_known;
            if (node->known)
                node->status = peer->config_status;
        }
    }
    return 0;
}
