// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_live.h"
#include "config_candidate.h"
#include "config_reload.h"
#include "cluster.h"
#include "lease.h"
#include "move.h"
#include "peer.h"
#include "resources.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool definitions_equal(const lcs_config_t *a, const lcs_config_t *b, uint64_t revision)
{
    char *left = NULL, *right = NULL, error[256];
    size_t left_length, right_length;
    bool equal = !lcs_resource_config_encode(a, revision, &left, &left_length, error, sizeof(error)) &&
        !lcs_resource_config_encode(b, revision, &right, &right_length, error, sizeof(error)) &&
        left_length == right_length && !memcmp(left, right, left_length);
    free(left); free(right);
    return equal;
}

static bool value_matches(const lcs_config_t *config, const lcs_config_value_t *value)
{
    char *document = NULL, error[256]; size_t length;
    if (lcs_resource_config_encode(config, value->revision, &document, &length, error, sizeof(error)))
        return false;
    bool equal = value->length == length && !memcmp(value->document, document, length);
    free(document);
    return equal;
}

static int bind_attempt(lcs_config_live_t *live, const lcs_config_reconcile_view_t *view, char *err, size_t length)
{
    if (live->bound && live->attempt == view->attempt)
        return live->revision == view->value->revision && !strcmp(live->digest, view->value->digest) ? 0 : -1;
    if ((live->bound && view->attempt <= live->attempt) || !view->attempt ||
        !lcs_config_value_equal(view->value, &g_state.config_runtime.application.pending) ||
        !definitions_equal(&g_state.cfg, view->old_config, g_state.config_runtime.application.applied.revision) ||
        !value_matches(&g_state.cfg, &g_state.config_runtime.application.applied) ||
        !value_matches(view->target, view->value) ||
        lcs_config_candidate_validate(&g_state.cfg, view->target, err, length))
    {
        snprintf(err, length, "live definitions/attempt differ from durable application catalog");
        return -1;
    }
    live->bound = true; live->installed = live->synchronizing = false;
    live->attempt = view->attempt; live->revision = view->value->revision;
    memcpy(live->digest, view->value->digest, sizeof(live->digest));
    g_state.config_application_fenced = true;
    g_state.config_application_remapping = false;
    g_state.config_application_drain_mask = view->plan->drain;
    move_cancel_all(live->epoll_fd, "resource definitions are being reconciled");
    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
        if (view->plan->drain & (UINT32_C(1) << i))
            lease_cancel_operations((int)i);
    return 0;
}

static int provider_result(int result, char *err, size_t length)
{
    if (result > 1)
    {
        snprintf(err, length, "invalid application barrier completion status; keep resources fenced");
        return -1;
    }
    return result;
}

static int publish_current(lcs_config_live_t *live, char *err, size_t length)
{
    return provider_result(live->project(live->managed_projection ? &live->projection : live->barrier_context,
        &g_state.config_runtime.agreement.committed, err, length), err, length);
}

static int quiesce(void *context, const lcs_config_reconcile_view_t *view, char *err, size_t length)
{
    lcs_config_live_t *live = context;
    if (bind_attempt(live, view, err, length))
        return -1;
    return publish_current(live, err, length);
}

static bool workers_busy(const resource_runtime_t *resource)
{
    return resource->hook.pid > 0 || resource->hook.type || resource->hook.next_type || resource->vip_probe.pid > 0 ||
        resource->vip.pid > 0 || resource->vip.op || resource->vip.next_op || resource->service.pid > 0 ||
        resource->service.op || resource->service.next_op || resource->handoff_pending;
}

static bool locally_owned(size_t index)
{
    return g_state.resources[index].owner_node == g_state.self_index &&
        g_state.resources[index].owner_instance_id == g_state.instance_id;
}

static bool dependent_busy(const lcs_config_reconcile_view_t *view, size_t index)
{
    for (size_t i = 0; i < view->old_config->resource_count; i++)
        for (size_t d = 0; d < view->old_config->resources[i].depends_on_count; d++)
            if (view->old_config->resources[i].depends_on_idx[d] == (int)index &&
                (locally_owned(i) || workers_busy(&g_state.resources[i])))
                return true;
    return false;
}

static int drain(void *context, const lcs_config_reconcile_view_t *view, char *err, size_t length)
{
    lcs_config_live_t *live = context;
    if (bind_attempt(live, view, err, length) ||
        !definitions_equal(&g_state.cfg, view->old_config, g_state.config_runtime.application.applied.revision))
        return -1;
    bool pending = false;
    for (size_t i = 0; i < view->old_config->resource_count; i++)
    {
        if (!(view->plan->drain & (UINT32_C(1) << i)))
            continue;
        resource_runtime_t *resource = &g_state.resources[i];
        if (resource->state == LCS_RES_STOP_FAILED || resource->startup_cleanup_failed)
        {
            if (live->owner_revoke)
                live->owner_revoke(live->barrier_context);
            snprintf(err, length, "resource %s cleanup failed; confirm cleanup or fencing before retry", view->old_config->resources[i].name);
            return -1;
        }
        if (locally_owned(i))
        {
            pending = true;
            if (!dependent_busy(view, i) && resource->state != LCS_RES_STOPPING)
                resources_release_local((int)i, live->epoll_fd);
        }
        if (workers_busy(resource))
            pending = true;
    }
    return pending ? 0 : 1;
}

int lcs_config_live_freeze_drain(int epoll_fd, char *err, size_t err_len)
{
    const lcs_config_runtime_t *runtime = &g_state.config_runtime;
    g_state.config_freeze_drained = false;
    if (!runtime->requires_recovery || !runtime->base ||
        runtime->activation.base != runtime->base || !runtime->activation.store || runtime->activation.poisoned ||
        runtime->activation.phase != LCS_CONFIG_ACTIVATION_FROZEN || !runtime->activation.requests_required ||
        !g_state.config_bootstrap.initialization_locked || config_reload_in_progress() ||
        g_state.resource_generation_enabled || g_state.self_index != lcs_config_self_index(runtime->base) ||
        !definitions_equal(&g_state.cfg, runtime->base, 0) || !value_matches(&g_state.cfg, &runtime->activation.baseline))
    { snprintf(err, err_len, "resource drain requires an exact durable frozen INI baseline"); return -1; }
    if (err && err_len) err[0] = 0;
    /* Every member must have a durable reload lock before any owner drains.
     * Until then exact existing leases may renew, but new grants are refused. */
    if (!g_state.config_freeze_locks_confirmed) return 0;
    /* Exclude placement, acquisition, renewal, handoff and incoming resource
     * traffic before cancelling anything. Ordinary backend callbacks still run. */
    g_state.config_application_fenced = g_state.config_application_remapping = true;
    g_state.config_application_drain_mask = UINT32_MAX;
    g_state.resource_generation_admitted = g_state.voting_ready = false;
    if (!g_state.config_freeze_draining)
    {
        g_state.config_freeze_draining = true;
        move_cancel_all(epoll_fd, "resource definitions are being frozen");
        lease_cancel_all_operations();
    }
    bool pending = false;
    bool quorum_only = g_state.cfg.nodes[g_state.self_index].role == LCS_NODE_QUORUM_ONLY;
    for (size_t i = 0; i < LCS_MAX_RESOURCES; i++)
    {
        resource_runtime_t *resource = &g_state.resources[i];
        if (i < g_state.cfg.resource_count && (resource->state == LCS_RES_STOP_FAILED || resource->startup_cleanup_failed))
        { snprintf(err, err_len, "resource %s cleanup failed during freeze; repair cleanup and restart", g_state.cfg.resources[i].name); return -1; }
        if (i < g_state.cfg.resource_count && resource->state == LCS_RES_CONFLICT)
        { snprintf(err, err_len, "resource %s ownership conflict prevents freeze drain; repair and restart", g_state.cfg.resources[i].name); return -1; }
        if (i < g_state.cfg.resource_count && locally_owned(i))
        {
            pending = true;
            if (quorum_only)
            { snprintf(err, err_len, "quorum-only member has unexpected local resource ownership"); return -1; }
            /* Reuse the live drain's dependency predicate with the whole old
             * generation fenced; a child must finish before its parent stops. */
            lcs_config_reconcile_view_t view = {.old_config = runtime->base};
            if (!dependent_busy(&view, i) && resource->state != LCS_RES_STOPPING)
                resources_release_local((int)i, epoll_fd);
        }
        if (resource->state != LCS_RES_STOPPED || workers_busy(resource) || resource->owner_instance_id || resource->lease_id ||
            resource->lease_deadline_ms || resource->renew_after_ms || g_state.lease_grants[i].active)
            pending = true;
    }
    for (size_t i = 0; i < LCS_LEASE_OP_MAX; i++) if (g_state.lease_ops[i].active) pending = true;
    for (size_t i = 0; i < LCS_MOVE_OP_MAX; i++) if (g_state.moves[i].active) pending = true;
    if (pending) return 0;
    g_state.config_freeze_drained = true;
    if (err && err_len) err[0] = 0;
    return 1;
}

static int owners(void *context, const lcs_config_reconcile_view_t *view, char *err, size_t length)
{
    lcs_config_live_t *live = context;
    if (bind_attempt(live, view, err, length))
        return -1;
    int drained = drain(context, view, err, length);
    if (drained != 1)
        return drained;
    return provider_result(live->owner_barrier(live->barrier_context, view, err, length), err, length);
}

static int install(void *context, const lcs_config_reconcile_view_t *view, char *err, size_t length)
{
    lcs_config_live_t *live = context;
    if (bind_attempt(live, view, err, length))
        return -1;
    /* Learning may advance while drain/owner proof waits. Publish the latest
     * decision again before any table install, including idempotent retries. */
    int projected = publish_current(live, err, length);
    if (projected != 1) return projected;
    if (live->installed)
        return definitions_equal(&g_state.cfg, view->target, view->value->revision) ? 1 : -1;
    if (g_state.cfg.nodes[g_state.self_index].role == LCS_NODE_FULL)
    {
        /* Collection may have completed before a disconnect or cleanup fault.
         * Recheck the owner provider at the final pre-install boundary. */
        int safe = owners(context, view, err, length);
        if (safe != 1)
            return safe;
    }
    if (g_state.config_reload.active_generation == UINT64_MAX)
        return -1;
    for (size_t i = 0; i < view->target->resource_count; i++)
    {
        int previous = lcs_config_resource_index(view->old_config, view->target->resources[i].name);
        if (previous >= 0 && (view->old_config->resources[previous].birth_revision != view->target->resources[i].birth_revision ||
            !lcs_config_backend_equal(&view->old_config->resources[previous], &view->target->resources[i])))
            previous = -1;
        if (view->plan->new_to_old[i] != previous)
        { snprintf(err, length, "runtime remapping plan does not match durable incarnation identities"); return -1; }
    }
    g_state.config_application_remapping = true;
    struct snapshot { resource_runtime_t resources[LCS_MAX_RESOURCES]; lease_grant_t grants[LCS_MAX_RESOURCES]; };
    struct snapshot *old = malloc(sizeof(*old));
    if (!old)
    { snprintf(err, length, "out of memory retaining resource runtime table"); return -1; }
    int result = peer_install_resource_generation(live->epoll_fd, view->value);
    if (result)
    { free(old); return result > 0 ? 0 : -1; }
    /* Cancellation callbacks finish against OLD indices before copying/remapping. */
    memcpy(old->resources, g_state.resources, sizeof(old->resources));
    memcpy(old->grants, g_state.lease_grants, sizeof(old->grants));
    memset(g_state.resources, 0, sizeof(g_state.resources));
    memset(g_state.lease_grants, 0, sizeof(g_state.lease_grants));
    uint32_t fence = 0;
    for (size_t i = 0; i < view->target->resource_count; i++)
    {
        int previous = view->plan->new_to_old[i];
        if (previous >= 0)
        {
            g_state.resources[i] = old->resources[previous];
            g_state.lease_grants[i] = old->grants[previous];
        }
        else
        {
            g_state.resources[i].state = LCS_RES_STOPPED;
            g_state.resources[i].owner_node = -1;
            g_state.lease_grants[i].owner_node = -1;
        }
        if (previous < 0 || (view->plan->drain & (UINT32_C(1) << previous)))
            fence |= UINT32_C(1) << i;
    }
    free(old);
    g_state.cfg = *view->target;
    g_state.config_application_drain_mask = fence;
    g_state.config_reload.active_generation++;
    g_state.next_placement_ms = lcs_now_ms() + 500u;
    live->installed = true;
    return 1;
}

static int admit(void *context, const lcs_config_reconcile_view_t *view, char *err, size_t length)
{
    lcs_config_live_t *live = context;
    if (!live->installed || live->attempt != view->attempt || strcmp(live->digest, view->value->digest) ||
        !g_state.config_application_fenced ||
        peer_admit_resource_generation())
    { snprintf(err, length, "applied resource generation is not ready for admission"); return -1; }
    /* Synchronization can make ordinary cluster voting ready before the final
     * checks below. Fence ALL resource grants until the sequencer completes. */
    g_state.config_application_drain_mask = UINT32_MAX;
    if (!live->synchronizing)
    {
        g_state.voting_ready = false; g_state.votes_seen = 0;
        for (size_t i = 0; i < g_state.cfg.node_count; i++)
        {
            g_state.peers[i].voting_ready = false;
            g_state.peers[i].initial_sync_complete = false;
            g_state.peers[i].resource_generation_matches = false;
            g_state.peers[i].state_sync_pending = true;
            g_state.peers[i].next_heartbeat_ms = 0;
        }
        live->synchronizing = true;
    }
    g_state.config_application_remapping = false; /* Allow the new generation's restart state sync. */
    uint64_t now = lcs_now_ms();
    uint32_t synchronized = 1, voters = cluster_local_voting_ready() ? 1 : 0;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if ((int)i == g_state.self_index) continue;
        const peer_runtime_t *peer = &g_state.peers[i];
        if (peer->fd < 0 || peer->conn_state != LCS_PEER_ESTABLISHED || !peer->instance_id || !peer->online ||
            (live->network && (live->network->closing || live->network->instances[i] != peer->instance_id)) ||
            peer->protocol_version < LCS_PROTO_FEATURE_RESOURCE_GENERATION ||
            now < peer->last_seen_ms || now - peer->last_seen_ms > g_state.cfg.peer_timeout_ms ||
            !peer->initial_sync_complete || !peer->resource_generation_known || !peer->resource_generation_matches ||
            !lcs_resource_generation_equal(&peer->resource_generation, &g_state.resource_generation)) continue;
        synchronized++; if (peer->voting_ready) voters++;
    }
    uint32_t needed = (uint32_t)lcs_config_quorum(&g_state.cfg);
    if (now < g_state.voting_not_before_ms || synchronized < needed || voters < needed ||
        !cluster_local_voting_ready() || !cluster_has_quorum())
        return 0;
    return 1;
}

static void cancel_admission(void *context)
{
    lcs_config_live_t *live = context;
    g_state.config_application_fenced = g_state.config_application_remapping = true;
    g_state.config_application_drain_mask = UINT32_MAX;
    g_state.resource_generation_admitted = g_state.voting_ready = false; g_state.votes_seen = 0;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        g_state.peers[i].voting_ready = false;
        g_state.peers[i].initial_sync_complete = false;
        g_state.peers[i].resource_generation_matches = false;
        g_state.peers[i].next_heartbeat_ms = 0;
    }
    live->synchronizing = false;
}

static void complete_admission(void *context)
{
    lcs_config_live_t *live = context;
    g_state.config_application_fenced = false;
    g_state.config_application_drain_mask = 0;
    live->synchronizing = false;
}

int lcs_config_live_init(lcs_config_live_t *live, int epoll_fd,
                           lcs_config_live_projection_t project,
                           lcs_config_reconcile_step_t owner_barrier, void *context)
{
    if (!project || !owner_barrier)
        return -1;
    memset(live, 0, sizeof(*live));
    live->epoll_fd = epoll_fd; live->project = project;
    live->owner_barrier = owner_barrier; live->barrier_context = context;
    return 0;
}

int lcs_config_live_init_with_projection(lcs_config_live_t *live, int epoll_fd,
                                          lcs_config_coordinator_t *coordinator,
                                          lcs_config_bootstrap_t *bootstrap,
                                          lcs_config_reconcile_step_t owner_barrier, void *context)
{
    if (!coordinator || !bootstrap || lcs_config_live_init(live, epoll_fd, lcs_config_projection_step, owner_barrier, context))
        return -1;
    live->managed_projection = true;
    lcs_config_projection_init(&live->projection, coordinator, bootstrap);
    return 0;
}

int lcs_config_live_init_with_owner_network(lcs_config_live_t *live, int epoll_fd,
                                             lcs_config_coordinator_t *coordinator,
                                             lcs_config_bootstrap_t *bootstrap,
                                             lcs_config_owner_network_t *network)
{
    if (!coordinator || !coordinator->runtime || !network || !network->instance || network->closing ||
        network->base != coordinator->runtime->base || network->application != &coordinator->runtime->application ||
        lcs_config_live_init_with_projection(live, epoll_fd, coordinator, bootstrap,
            lcs_config_owner_network_step, network))
        return -1;
    live->owner_revoke = lcs_config_owner_network_revoke;
    live->network = network;
    return 0;
}

const lcs_config_reconcile_ops_t *lcs_config_live_ops(void)
{
    static const lcs_config_reconcile_ops_t ops = {quiesce, drain, owners, install, admit, cancel_admission, complete_admission};
    return &ops;
}

bool lcs_config_live_maintain_leases(int epoll_fd)
{
    if (!g_state.config_application_fenced || (!g_state.config_application_remapping &&
        cluster_local_voting_ready() && cluster_has_quorum()))
        return false;
    uint64_t now = lcs_now_ms();
    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        const resource_runtime_t *resource = &g_state.resources[i];
        if (resource->owner_node == g_state.self_index && resource->owner_instance_id == g_state.instance_id &&
            resource->state != LCS_RES_STOP_FAILED &&
            (!resource->lease_deadline_ms || now >= resource->lease_deadline_ms))
            resources_drop_local((int)i, epoll_fd);
    }
    return true;
}
