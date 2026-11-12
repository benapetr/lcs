// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_peer.h"
#include "config_reload.h"
#include "config_candidate.h"
#include "daemon_state.h"
#include "log.h"
#include "peer.h"
#include "util.h"

#include <stdlib.h>
#include <stdio.h>
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

int config_peer_propose_client(uint64_t id, const lcs_config_value_t *requested, const char *client_digest)
{
    if (config_reload_in_progress() || !config_peer_configuration_vote_ready())
        return -1;
    if (!g_state.config_transactions.instance)
        lcs_config_transactions_init(&g_state.config_transactions, &g_state.config_coordinator, g_state.instance_id);
    uint64_t instances[LCS_MAX_NODES] = {0};
    instances[g_state.self_index] = g_state.instance_id;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        const peer_runtime_t *peer = &g_state.peers[i];
        if ((int)i != g_state.self_index && peer->fd >= 0 && peer->conn_state == LCS_PEER_ESTABLISHED &&
            peer->protocol_version >= LCS_PROTO_FEATURE_CONFIG_TRANSACTIONS)
            instances[i] = peer->instance_id;
    }
    return lcs_config_transactions_propose_client(&g_state.config_transactions, instances, id, requested, client_digest, lcs_now_ms());
}

int config_peer_propose(uint64_t id, const lcs_config_value_t *requested)
{
    return config_peer_propose_client(id, requested, "");
}

static int send_owner(void *context, size_t node, const void *payload, size_t length)
{
    return peer_queue_config_owner(*(int *)context, (int)node, payload, (uint32_t)length);
}

int config_peer_owner_prepare(void)
{
    lcs_config_owner_network_t *network = &g_state.config_owners;
    const lcs_config_t *base = g_state.config_runtime.base ? g_state.config_runtime.base : &g_state.cfg;
    const lcs_config_application_t *application = g_state.config_runtime.base ? &g_state.config_runtime.application : NULL;
    char membership[LCS_CONFIG_DIGEST_SIZE];
    if (g_state.config_coordinator.closing || g_state.config_coordinator.failed ||
        config_reload_in_progress() || !g_state.instance_id || g_state.self_index < 0 ||
        lcs_config_agreement_membership(base, membership))
        return -1;
    if (network->instance && network->instance == g_state.instance_id && network->base == base &&
        network->application == application && !strcmp(network->membership, membership))
        return network->closing ? -1 : 0;
    if (network->instance && (network->round.active || network->receipt.sealed || network->receipt.failed || network->retired || network->closing))
    {
        lcs_config_owner_network_revoke(network);
        return -1;
    }
    return lcs_config_owner_network_init(network, base, application, (size_t)g_state.self_index, g_state.instance_id);
}

static void process_owners(int epoll_fd)
{
    if (!g_state.config_owners.instance)
        return;
    if (config_peer_owner_prepare())
    {
        lcs_config_owner_network_revoke(&g_state.config_owners);
        return;
    }
    uint64_t instances[LCS_MAX_NODES] = {0};
    instances[g_state.self_index] = g_state.instance_id;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        const peer_runtime_t *peer = &g_state.peers[i];
        if ((int)i != g_state.self_index && peer->fd >= 0 && peer->conn_state == LCS_PEER_ESTABLISHED &&
            peer->protocol_version >= LCS_PROTO_FEATURE_CONFIG_OWNERS)
            instances[i] = peer->instance_id;
    }
    lcs_config_owner_network_process(&g_state.config_owners, instances, lcs_now_ms(), send_owner, &epoll_fd);
}

int config_peer_receive_owner(int epoll_fd, int node, const void *payload, size_t length)
{
    if (node < 0 || (size_t)node >= g_state.cfg.node_count || node == g_state.self_index ||
        g_state.cfg.nodes[node].role != LCS_NODE_FULL || g_state.cfg.nodes[g_state.self_index].role != LCS_NODE_FULL)
        return -1;
    const peer_runtime_t *peer = &g_state.peers[node];
    if (peer->fd < 0 || peer->conn_state != LCS_PEER_ESTABLISHED || !peer->instance_id ||
        peer->protocol_version < LCS_PROTO_FEATURE_CONFIG_OWNERS || config_peer_owner_prepare())
        return -1;
    int result = lcs_config_owner_network_receive(&g_state.config_owners, (size_t)node, peer->instance_id,
        payload, length, lcs_now_ms());
    if (!result)
        process_owners(epoll_fd);
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
    g_state.config_initialization_requested = false;
    g_state.config_initialization.failed = true;
    g_state.config_coordinator.closing = true;
    lcs_config_coordinator_stop(&g_state.config_coordinator);
    lcs_config_transactions_close(&g_state.config_transactions);
    lcs_config_owner_network_stop(&g_state.config_owners);
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
    if (g_state.config_initialization_auto_resume && before == LCS_CONFIG_COORD_COLLECTING &&
        !state->round.sealed && (state->round.failed || now >= state->deadline_ms) && durable_readiness_available() &&
        !lcs_config_coordinator_reset_readiness(coordinator, now))
    {
        before = coordinator->phase;
        g_state.config_readiness_retry_ms = now + 1000u;
    }
    if (coordinator->initialized && !coordinator->closing &&
        (before == LCS_CONFIG_COORD_COLLECTING || before == LCS_CONFIG_COORD_PERSISTING) &&
        !durable_readiness_available())
        lcs_config_coordinator_abort(coordinator, "local definitions or reload state changed during readiness");
    (void)lcs_config_coordinator_process(coordinator, now);
    if (before == LCS_CONFIG_COORD_PERSISTING && coordinator->phase == LCS_CONFIG_COORD_READY)
        lcs_log_debug("all-member configuration readiness persisted; configuration vote prerequisites satisfied");
    else if (coordinator->phase != before && coordinator->phase == LCS_CONFIG_COORD_FAILED)
        lcs_log_error("configuration storage coordinator failed: %s", coordinator->error);
    if (g_state.config_initialization_auto_resume && coordinator->phase == LCS_CONFIG_COORD_IDLE &&
        !coordinator->closing && !coordinator->failed && now >= g_state.config_readiness_retry_ms &&
        lcs_config_runtime_initialization_prepared(&g_state.config_runtime))
    {
        g_state.config_readiness_retry_ms = now + 1000u;
        (void)config_peer_readiness_start();
    }
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

static bool freeze_instances(uint64_t instances[LCS_MAX_NODES])
{
    memset(instances, 0, sizeof(uint64_t) * LCS_MAX_NODES);
    if (g_state.self_index < 0 || !g_state.instance_id) return false;
    instances[g_state.self_index] = g_state.instance_id;
    bool complete = true;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if ((int)i == g_state.self_index) continue;
        const peer_runtime_t *peer = &g_state.peers[i];
        if (peer->fd >= 0 && peer->conn_state == LCS_PEER_ESTABLISHED && peer->instance_id &&
            peer->protocol_version >= LCS_PROTO_FEATURE_CONFIG_FREEZE) instances[i] = peer->instance_id;
        else complete = false;
    }
    return complete;
}

int config_peer_resume_frozen(char *err, size_t err_len)
{
    lcs_config_runtime_t *runtime = &g_state.config_runtime;
    const lcs_config_activation_t *activation = &runtime->activation;
    bool frozen = activation->phase == LCS_CONFIG_ACTIVATION_FROZEN &&
        !runtime->agreement.store && !runtime->agreement.poisoned && !runtime->agreement.committed.document &&
        !runtime->agreement.accepted.document && !runtime->agreement.promised && !runtime->application.store &&
        !runtime->application.poisoned && !runtime->application.applied.document && !runtime->application.pending.document &&
        !runtime->requests.store && !runtime->requests.count && !runtime->requests.poisoned && !runtime->barrier.store && !runtime->barrier.poisoned;
    bool pristine = !g_state.config_coordinator.initialized && !g_state.config_coordinator.closing &&
        !g_state.config_coordinator.failed && !g_state.config_coordinator.storage.pid && !g_state.config_status.serial &&
        !g_state.resource_generation_enabled && !g_state.voting_ready && !g_state.votes_seen && g_state.instance_id &&
        !config_reload_in_progress() && !g_state.config_bootstrap.worker_pid && !g_state.config_bootstrap.refresh_pending;
    for (size_t i = 0; i < g_state.cfg.node_count; i++) if (g_state.peers[i].fd >= 0) pristine = false;
    for (size_t i = 0; i < LCS_HANDSHAKE_MAX; i++) if (g_state.handshakes[i].active) pristine = false;
    for (size_t i = 0; i < LCS_CLI_SERVER_MAX; i++) if (g_state.cli_servers[i].active) pristine = false;
    for (size_t i = 0; i < LCS_LEASE_OP_MAX; i++) if (g_state.lease_ops[i].active) pristine = false;
    for (size_t i = 0; i < LCS_MOVE_OP_MAX; i++) if (g_state.moves[i].active) pristine = false;
    for (size_t i = 0; i < LCS_MAX_RESOURCES; i++)
    {
        const resource_runtime_t *resource = &g_state.resources[i];
        const lease_grant_t *grant = &g_state.lease_grants[i];
        if (resource->state != LCS_RES_STOPPED || resource->owner_instance_id || resource->epoch || resource->lease_id ||
            resource->lease_deadline_ms || resource->renew_after_ms || resource->disabled_generation || resource->home_generation || resource->failover_count ||
            resource->vip.pid > 0 || resource->vip.op || resource->vip.next_op || resource->service.pid > 0 ||
            resource->service.op || resource->service.next_op || resource->handoff_pending || resource->startup_cleanup_failed ||
            grant->active || grant->owner_instance_id || grant->epoch || grant->lease_id || grant->deadline_ms || grant->promised_epoch) pristine = false;
    }
    if (!pristine || !runtime->requires_recovery || !runtime->base || activation->base != runtime->base ||
        activation->store != &g_state.config_bootstrap.state || (!frozen && !lcs_config_runtime_initialization_prepared(runtime) &&
        !lcs_config_runtime_activation_pending(runtime)) ||
        activation->poisoned || !activation->requests_required || !activation->baseline.document ||
        g_state.self_index != lcs_config_self_index(runtime->base))
    { if (err && err_len) snprintf(err, err_len, "frozen startup requires pristine pre-listener state and an exact recovered freeze"); return -1; }
    char *document = NULL; size_t length; lcs_config_value_t value = {0};
    int result = lcs_config_candidate_validate(&g_state.cfg, runtime->base, err, err_len);
    if (!result) result = lcs_resource_config_encode(runtime->base, 0, &document, &length, err, err_len);
    if (!result) result = lcs_config_value_init(&value, runtime->base, "bootstrap", document, length, err, err_len);
    if (!result && !lcs_config_value_equal(&value, &activation->baseline))
    { if (err && err_len) snprintf(err, err_len, "frozen startup baseline differs from recovered definitions"); result = -1; }
    free(document); lcs_config_value_clear(&value);
    if (result) return -1;
    /* Fence before installing recovered indices. Cleanup must use the retained
     * baseline; no backend from the newly read INI has run yet. */
    g_state.config_application_fenced = g_state.config_application_remapping = true;
    g_state.config_application_drain_mask = UINT32_MAX;
    g_state.resource_generation_admitted = false;
    g_state.cfg = *runtime->base;
    memset(g_state.resources, 0, sizeof(g_state.resources));
    memset(g_state.lease_grants, 0, sizeof(g_state.lease_grants));
    for (size_t i = 0; i < LCS_MAX_RESOURCES; i++)
    { g_state.resources[i].owner_node = -1; g_state.lease_grants[i].owner_node = -1; }
    g_state.config_bootstrap.initialization_locked = true;
    g_state.config_bootstrap.prepared = false; g_state.config_bootstrap_initialized = true;
    lcs_config_coordinator_init(&g_state.config_coordinator, runtime, &g_state.config_readiness);
    if (lcs_config_runtime_activation_pending(runtime) && lcs_config_coordinator_recover(&g_state.config_coordinator))
    { if (err && err_len) snprintf(err, err_len, "cannot restore durable readiness coordinator"); return -1; }
    return 0;
}

int config_peer_freeze_start(void)
{
    lcs_config_runtime_t *runtime = &g_state.config_runtime;
    lcs_config_coordinator_t *coordinator = &g_state.config_coordinator;
    if (g_state.self_index < 0 ||
        coordinator->closing || coordinator->failed || coordinator->storage.pid > 0 ||
        config_reload_in_progress() || !g_state.config_bootstrap.initialization_locked ||
        runtime->activation.phase != LCS_CONFIG_ACTIVATION_FROZEN || runtime->activation.poisoned ||
        !runtime->base || !runtime->activation.store || g_state.config_initialization_attempt == UINT64_MAX) return -1;
    uint64_t now = lcs_now_ms(), instances[LCS_MAX_NODES];
    if (!freeze_instances(instances)) return 1;
    if (g_state.config_initialization.active && !g_state.config_initialization.failed &&
        !g_state.config_initialization.consumed && now < g_state.config_initialization.deadline_ms) return 1;
    char error[256];
    int result = lcs_config_initialization_begin(&g_state.config_initialization, runtime->base,
        instances, g_state.config_reload.active_generation, ++g_state.config_initialization_attempt,
        now, error, sizeof(error));
    if (!result) g_state.config_initialization_next_send_ms = 0;
    return result;
}

int config_peer_initialization_start(void)
{
    if (g_state.config_initialization_requested ||
        g_state.config_coordinator.phase == LCS_CONFIG_COORD_INITIALIZING) return 1;
    int result = config_peer_freeze_start();
    if (result < 0) return -1;
    if (result == 1)
    {
        uint64_t instances[LCS_MAX_NODES];
        if (!freeze_instances(instances) || !g_state.config_initialization.active ||
            g_state.config_coordinator.closing || g_state.config_coordinator.failed ||
            g_state.config_coordinator.storage.pid > 0 || config_reload_in_progress() ||
            lcs_config_initialization_poll(&g_state.config_initialization, instances,
                g_state.config_reload.active_generation, lcs_now_ms(), NULL, 0) < 0) return 1;
    }
    g_state.config_initialization_requested = true;
    return 0;
}

/* This function is called only for a current admitted query or its bounded
 * waiter. It never turns advertisements into initialization authorization. */
static uint8_t freeze_status(const lcs_config_freeze_message_t *query)
{
    lcs_config_coordinator_t *coordinator = &g_state.config_coordinator;
    const lcs_config_runtime_t *runtime = &g_state.config_runtime;
    if (coordinator->closing || coordinator->failed || config_reload_in_progress() || config_peer_refresh())
        return LCS_INITIALIZATION_BUSY;
    const char *membership = runtime->activation.store ? runtime->activation.membership : g_state.config_status.membership;
    const char *baseline = runtime->activation.store ? runtime->activation.baseline.digest : g_state.config_status.baseline;
    if (strcmp(query->receipt.membership, membership) || strcmp(query->receipt.baseline, baseline))
        return LCS_INITIALIZATION_MISMATCH;
    if (runtime->activation.store && (runtime->activation.phase == LCS_CONFIG_ACTIVATION_FROZEN ||
        lcs_config_runtime_initialization_prepared(runtime)) &&
        !runtime->activation.poisoned && g_state.config_bootstrap.initialization_locked && !coordinator->storage.pid)
        return LCS_INITIALIZATION_FROZEN;
    uint64_t instances[LCS_MAX_NODES];
    if (!freeze_instances(instances) || g_state.resource_generation_enabled || runtime->requires_recovery ||
        runtime->base || !g_state.config_bootstrap_initialized || !g_state.config_bootstrap.prepared ||
        g_state.config_bootstrap.initialization_locked || g_state.config_bootstrap.refresh_pending ||
        g_state.config_bootstrap.worker_pid > 0) return LCS_INITIALIZATION_BUSY;
    if (!coordinator->initialized)
        lcs_config_coordinator_init(coordinator, &g_state.config_runtime, &g_state.config_readiness);
    char error[256];
    if (lcs_config_coordinator_start_freeze(coordinator, &g_state.config_bootstrap, &g_state.cfg, error, sizeof(error)) < 0)
        lcs_log_debug("configuration freeze preflight refused: %s", error);
    return LCS_INITIALIZATION_BUSY;
}

static int send_freeze(int epoll_fd, size_t node, const lcs_config_freeze_message_t *message)
{
    unsigned char payload[LCS_CONFIG_FREEZE_SIZE]; size_t length;
    if (lcs_config_freeze_encode(message, payload, sizeof(payload), &length)) return -1;
    return peer_queue_config_freeze(epoll_fd, (int)node,
        message->kind == LCS_FREEZE_QUERY ? LCS_MSG_CONFIG_FREEZE_QUERY : LCS_MSG_CONFIG_FREEZE_REPLY,
        payload, (uint32_t)length);
}

static void process_freeze(int epoll_fd)
{
    uint64_t now = lcs_now_ms();
    /* A durable freeze is the prerequisite, never version advertisements alone.
     * Unit/internal collectors may remain explicit; ordinary daemon operation
     * resumes each member's preparation after collecting fresh all-member proof. */
    if (g_state.config_initialization_auto_resume && !g_state.config_initialization_requested &&
        g_state.config_runtime.activation.phase == LCS_CONFIG_ACTIVATION_FROZEN &&
        !g_state.config_coordinator.closing && !g_state.config_coordinator.failed &&
        g_state.config_coordinator.storage.pid <= 0 && now >= g_state.config_initialization_retry_ms)
    {
        g_state.config_initialization_retry_ms = now + 1000u;
        (void)config_peer_initialization_start();
    }
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if (!g_state.config_freeze_waiters[i].pending) continue;
        const peer_runtime_t *peer = &g_state.peers[i];
        if (now >= g_state.config_freeze_waiters[i].deadline_ms || g_state.config_coordinator.closing ||
            (peer->instance_id && peer->instance_id != g_state.config_freeze_waiters[i].query.receipt.requester_instance))
        { g_state.config_freeze_waiters[i].pending = false; continue; }
        if (peer->fd < 0 || peer->conn_state != LCS_PEER_ESTABLISHED ||
            peer->protocol_version < LCS_PROTO_FEATURE_CONFIG_FREEZE ||
            peer->instance_id != g_state.config_freeze_waiters[i].query.receipt.requester_instance ||
            now < g_state.config_freeze_waiters[i].next_send_ms) continue;
        lcs_config_freeze_message_t reply = g_state.config_freeze_waiters[i].query;
        reply.kind = LCS_FREEZE_REPLY; reply.receipt.status = freeze_status(&reply);
        g_state.config_freeze_waiters[i].next_send_ms = now + 250u;
        if (!send_freeze(epoll_fd, i, &reply) && reply.receipt.status != LCS_INITIALIZATION_BUSY)
            g_state.config_freeze_waiters[i].pending = false;
    }
    lcs_config_initialization_round_t *round = &g_state.config_initialization;
    if (!round->active || round->failed || round->consumed)
    { g_state.config_initialization_requested = false; return; }
    uint64_t instances[LCS_MAX_NODES]; (void)freeze_instances(instances);
    if (g_state.config_coordinator.closing || config_reload_in_progress())
    { round->failed = true; g_state.config_initialization_requested = false; return; }
    int ready = lcs_config_initialization_poll(round, instances, g_state.config_reload.active_generation, now, NULL, 0);
    if (ready < 0) { g_state.config_initialization_requested = false; return; }
    if (ready == 1)
    {
        if (g_state.config_initialization_requested)
        {
            char error[256] = {0};
            int result = lcs_config_coordinator_start_initialization(&g_state.config_coordinator,
                &g_state.config_bootstrap, &g_state.cfg, round, instances,
                g_state.config_reload.active_generation, now, error, sizeof(error));
            if (result <= 0) g_state.config_initialization_requested = false;
            if (result < 0)
            { round->failed = true; lcs_log_error("coordinated local initialization refused: %s", error); }
        }
        return;
    }
    if (now < g_state.config_initialization_next_send_ms) return;
    g_state.config_initialization_next_send_ms = now + 250u;
    for (size_t i = 0; i < round->count; i++)
    {
        if (i == round->self) continue;
        lcs_config_freeze_message_t query = {.kind = LCS_FREEZE_QUERY};
        if (!lcs_config_initialization_query(round, i, instances[i], now, &query.receipt, NULL, 0))
            (void)send_freeze(epoll_fd, i, &query);
    }
}

int config_peer_receive_freeze(int epoll_fd, int node, uint16_t type, const void *payload, size_t length)
{
    if (node < 0 || (size_t)node >= g_state.cfg.node_count || node == g_state.self_index) return -1;
    const peer_runtime_t *peer = &g_state.peers[node];
    lcs_config_freeze_message_t message;
    if (peer->fd < 0 || peer->conn_state != LCS_PEER_ESTABLISHED || !peer->instance_id ||
        peer->protocol_version < LCS_PROTO_FEATURE_CONFIG_FREEZE || lcs_config_freeze_decode(payload, length, &message)) return -1;
    if (type == LCS_MSG_CONFIG_FREEZE_REPLY && message.kind == LCS_FREEZE_REPLY)
    {
        if (message.receipt.responder_instance != peer->instance_id) return -1;
        if (message.receipt.requester_instance != g_state.instance_id) return 0;
        int result = lcs_config_initialization_receipt(&g_state.config_initialization, (size_t)node,
            peer->instance_id, &message.receipt, lcs_now_ms(), NULL, 0);
        return result < 0 ? -1 : 0;
    }
    if (type != LCS_MSG_CONFIG_FREEZE_QUERY || message.kind != LCS_FREEZE_QUERY ||
        message.receipt.requester_instance != peer->instance_id || message.receipt.responder_instance != g_state.instance_id) return -1;
    uint64_t now = lcs_now_ms();
    lcs_config_freeze_message_t *previous = &g_state.config_freeze_waiters[node].query;
    if (previous->receipt.requester_instance == peer->instance_id)
    {
        if (message.receipt.attempt < previous->receipt.attempt) return 0;
        if (message.receipt.attempt == previous->receipt.attempt)
        {
            if (strcmp(message.receipt.membership, previous->receipt.membership) ||
                strcmp(message.receipt.baseline, previous->receipt.baseline)) return -1;
            if (now >= g_state.config_freeze_waiters[node].deadline_ms) return 0;
        }
        else g_state.config_freeze_waiters[node].deadline_ms = now + LCS_CONFIG_INITIALIZATION_TIMEOUT_MS;
    }
    else g_state.config_freeze_waiters[node].deadline_ms = now + LCS_CONFIG_INITIALIZATION_TIMEOUT_MS;
    *previous = message;
    g_state.config_freeze_waiters[node].pending = true;
    g_state.config_freeze_waiters[node].next_send_ms = 0;
    process_freeze(epoll_fd);
    return 0;
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
        const lcs_config_t *definition_base = g_state.resource_generation_enabled && g_state.config_runtime.base ?
            g_state.config_runtime.base : &g_state.cfg;
        char error[256], *document = NULL;
        size_t length;
        lcs_config_value_t value = {0};
        if (lcs_config_agreement_membership(definition_base, next.membership) ||
            lcs_resource_config_encode(definition_base, 0, &document, &length, error, sizeof(error)))
            return withdraw_preparation();
        int result = lcs_config_value_init(&value, definition_base, "bootstrap", document,
                                           length, error, sizeof(error));
        free(document);
        if (result)
            return withdraw_preparation();
        changed = changed || strcmp(next.baseline, value.digest) != 0;
        memcpy(next.baseline, value.digest, sizeof(next.baseline));
        lcs_config_value_clear(&value);
    }
    bool prepared = !g_state.resource_generation_enabled && g_state.config_bootstrap_initialized && g_state.config_bootstrap.prepared &&
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

static void process_checkpoint_sync(void)
{
    lcs_config_coordinator_t *coordinator = &g_state.config_coordinator;
    if (!coordinator->initialized || coordinator->closing || coordinator->failed ||
        coordinator->phase != LCS_CONFIG_COORD_READY || !g_state.config_bootstrap.initialization_locked ||
        config_reload_in_progress())
        return;
    const lcs_config_runtime_t *runtime = coordinator->runtime;
    if (!lcs_config_barrier_valid(&runtime->barrier, &runtime->activation, &runtime->agreement, &runtime->application))
        return;
    if (!g_state.config_transactions.instance)
        lcs_config_transactions_init(&g_state.config_transactions, coordinator, g_state.instance_id);
    lcs_config_sync_t *sync = &g_state.config_transactions.sync;
    uint64_t now = lcs_now_ms();
    if ((sync->phase >= LCS_CONFIG_SYNC_QUERYING && sync->phase <= LCS_CONFIG_SYNC_PERSISTING) ||
        now < sync->next_ms || sync->serial == UINT64_MAX)
        return;
    sync->next_ms = now + 1000u;
    for (size_t scanned = 0; scanned < g_state.cfg.node_count; scanned++)
    {
        size_t node = sync->cursor++ % g_state.cfg.node_count;
        const peer_runtime_t *peer = &g_state.peers[node];
        if ((int)node == g_state.self_index || peer->fd < 0 || peer->conn_state != LCS_PEER_ESTABLISHED ||
            peer->protocol_version < LCS_PROTO_FEATURE_CONFIG_TRANSACTIONS || !peer->instance_id)
            continue;
        if (!lcs_config_transactions_sync(&g_state.config_transactions, node, peer->instance_id, sync->serial + 1, now))
            break;
    }
}

static void process_activation(void)
{
    lcs_config_runtime_t *runtime = &g_state.config_runtime;
    lcs_config_coordinator_t *coordinator = &g_state.config_coordinator;
    uint64_t now = lcs_now_ms();
    if (!g_state.config_activation_auto_resume || !lcs_config_runtime_activation_pending(runtime) ||
        !config_peer_configuration_vote_ready() || !g_state.config_application_fenced ||
        !g_state.config_application_remapping || g_state.resource_generation_enabled ||
        g_state.cfg.nodes[g_state.self_index].role != LCS_NODE_FULL ||
        coordinator->phase != LCS_CONFIG_COORD_READY || coordinator->storage.pid > 0 ||
        coordinator->application_owner || runtime->requests.count >= LCS_CONFIG_REQUEST_HISTORY_MAX) return;
    if (!g_state.config_activation_retry_ms)
    { g_state.config_activation_retry_ms = now + 1000u; return; }
    if (now < g_state.config_activation_retry_ms) return;
    for (size_t i = 0; i < (size_t)g_state.self_index; i++)
    {
        const peer_runtime_t *peer = &g_state.peers[i];
        if (g_state.cfg.nodes[i].role == LCS_NODE_FULL && peer->fd >= 0 && peer->conn_state == LCS_PEER_ESTABLISHED &&
            peer->instance_id && peer->protocol_version >= LCS_PROTO_FEATURE_CONFIG_TRANSACTIONS) return;
    }
    uint32_t available = UINT32_C(1) << g_state.self_index;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
        if (g_state.peers[i].fd >= 0 && g_state.peers[i].conn_state == LCS_PEER_ESTABLISHED &&
            g_state.peers[i].instance_id && g_state.peers[i].protocol_version >= LCS_PROTO_FEATURE_CONFIG_TRANSACTIONS)
            available |= UINT32_C(1) << i;
    if (!lcs_config_agreement_has_quorum(&runtime->agreement, available)) return;
    lcs_config_transactions_t *transactions = &g_state.config_transactions;
    uint64_t serial = g_state.config_activation_attempt > transactions->proposer.id ?
        g_state.config_activation_attempt : transactions->proposer.id;
    if (serial == UINT64_MAX) return;
    lcs_config_t *candidate = malloc(sizeof(*candidate)); char *document = NULL; size_t length;
    lcs_config_value_t value = {0}; char error[256], request[LCS_CONFIG_REQUEST_MAX + 1];
    g_state.config_activation_attempt = ++serial;
    g_state.config_activation_retry_ms = now + LCS_CONFIG_TRANSFER_TIMEOUT_MS + 1000u;
    snprintf(request, sizeof(request), "activation-%016llx-%016llx", (unsigned long long)g_state.instance_id,
        (unsigned long long)serial);
    if (candidate && !lcs_resource_config_assign_identities(runtime->base, 0, runtime->base, candidate, error, sizeof(error)) &&
        !lcs_resource_config_encode(candidate, 1, &document, &length, error, sizeof(error)) &&
        !lcs_config_value_init(&value, runtime->base, request, document, length, error, sizeof(error)) &&
        !config_peer_propose(serial, &value))
        lcs_log_debug("automatic resource activation proposed request=%s; waiting for majority decision", request);
    lcs_config_value_clear(&value); free(document); free(candidate);
}

void config_peer_process(int epoll_fd)
{
    process_readiness(epoll_fd);
    process_freeze(epoll_fd);
    process_owners(epoll_fd);
    lcs_config_transactions_process(&g_state.config_transactions, lcs_now_ms(), send_transaction, &epoll_fd);
    process_activation();
    process_checkpoint_sync();
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
    /* Cold frozen nodes retain resource-frame fences, including heartbeats.
     * Repeat advisory metadata to maintain the independent coordination channel;
     * these frames never advertise voting readiness or authorize activation. */
    uint64_t now = lcs_now_ms();
    bool keepalive = g_state.config_application_remapping && !g_state.resource_generation_enabled &&
        g_state.config_runtime.activation.store && !g_state.config_runtime.activation.poisoned &&
        (g_state.config_runtime.activation.phase == LCS_CONFIG_ACTIVATION_FROZEN ||
         lcs_config_runtime_initialization_prepared(&g_state.config_runtime) ||
         lcs_config_runtime_activation_pending(&g_state.config_runtime) ||
         lcs_config_runtime_activation_chosen(&g_state.config_runtime)) && now >= g_state.config_status_keepalive_ms;
    if (keepalive)
    {
        uint64_t interval = g_state.cfg.peer_timeout_ms / 3u;
        g_state.config_status_keepalive_ms = now + (interval ? interval : 1u);
    }
    unsigned char payload[256];
    size_t length;
    if (lcs_config_status_encode(&g_state.config_status, payload, sizeof(payload), &length))
        return;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        peer_runtime_t *peer = &g_state.peers[i];
        if ((int)i == g_state.self_index || (!keepalive && peer->config_status_sent == g_state.config_status.serial))
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
    if (g_state.config_runtime.activation.store && !g_state.config_runtime.activation.poisoned &&
        (g_state.config_runtime.activation.phase == LCS_CONFIG_ACTIVATION_FROZEN ||
         lcs_config_runtime_initialization_prepared(&g_state.config_runtime) ||
         lcs_config_runtime_activation_pending(&g_state.config_runtime)))
    {
        view->phase = LCS_CONFIG_DIAG_FROZEN;
        snprintf(view->error, sizeof(view->error), g_state.config_runtime.activation.phase == LCS_CONFIG_ACTIVATION_FROZEN ?
            "resource definitions are frozen; awaiting coordinated initialization" :
            lcs_config_runtime_activation_pending(&g_state.config_runtime) ?
            "resource definitions are frozen; local initialization prepared and all-member readiness persisted; awaiting activation" :
            "resource definitions are frozen; local initialization prepared, awaiting all-member readiness");
    }
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
