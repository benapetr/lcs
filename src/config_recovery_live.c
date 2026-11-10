// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_recovery_live.h"
#include "config_candidate.h"
#include "config_reload.h"
#include "cluster.h"
#include "peer.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int reject(char *err, size_t length, const char *message)
{
    if (err && length) snprintf(err, length, "%s", message);
    return -1;
}

static bool matches(const lcs_config_t *cfg, const lcs_config_value_t *value)
{
    char *document = NULL, error[256]; size_t length;
    if (!value || !value->document || lcs_resource_config_encode(cfg, value->revision, &document, &length, error, sizeof(error)))
        return false;
    bool equal = value->length == length && !memcmp(value->document, document, length);
    free(document); return equal;
}

static bool work_busy(void)
{
    for (size_t i = 0; i < LCS_MAX_RESOURCES; i++)
    {
        const resource_runtime_t *resource = &g_state.resources[i];
        if (resource->hook.pid > 0 || resource->hook.type || resource->hook.next_type || resource->vip_probe.pid > 0 ||
            resource->vip.pid > 0 || resource->vip.op || resource->vip.next_op || resource->service.pid > 0 ||
            resource->service.op || resource->service.next_op || resource->handoff_pending) return true;
    }
    for (size_t i = 0; i < LCS_LEASE_OP_MAX; i++) if (g_state.lease_ops[i].active) return true;
    for (size_t i = 0; i < LCS_MOVE_OP_MAX; i++) if (g_state.moves[i].active) return true;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
        for (size_t j = 0; j < LCS_MAX_PEER_RPC_INFLIGHT; j++)
            if (g_state.peers[i].in_flight[j].active && lcs_resource_frame_type(g_state.peers[i].in_flight[j].req_type)) return true;
    return false;
}

static bool pristine(void)
{
    if (work_busy()) return false;
    for (size_t i = 0; i < g_state.cfg.node_count; i++) if (g_state.peers[i].fd >= 0) return false;
    for (size_t i = 0; i < LCS_HANDSHAKE_MAX; i++) if (g_state.handshakes[i].active) return false;
    for (size_t i = 0; i < LCS_CLI_SERVER_MAX; i++) if (g_state.cli_servers[i].active) return false;
    for (size_t i = 0; i < LCS_MAX_RESOURCES; i++)
    {
        const resource_runtime_t *resource = &g_state.resources[i];
        const lease_grant_t *grant = &g_state.lease_grants[i];
        if (resource->state != LCS_RES_STOPPED || resource->owner_instance_id || resource->epoch || resource->lease_id ||
            resource->lease_deadline_ms || resource->renew_after_ms || resource->disabled_generation || resource->home_generation ||
            resource->startup_cleanup_failed || resource->failover_count || grant->active || grant->owner_instance_id ||
            grant->epoch || grant->lease_id || grant->deadline_ms || grant->promised_epoch) return false;
    }
    return true;
}

static bool projection_storage_ready(const lcs_config_runtime_t *runtime)
{
    const lcs_config_bootstrap_t *bootstrap = &g_state.config_bootstrap;
    return runtime->agreement.store && bootstrap->initialization_locked && bootstrap->worker_pid <= 0 &&
        !bootstrap->refresh_pending && bootstrap->snapshots.directory_fd >= 0 &&
        bootstrap->state.directory_fd == runtime->agreement.store->directory_fd &&
        bootstrap->state.lock_fd == runtime->agreement.store->lock_fd && bootstrap->state.lock_fd >= 0 &&
        memchr(bootstrap->snapshot_name, 0, sizeof(bootstrap->snapshot_name)) && *bootstrap->snapshot_name;
}

static int publish_current(lcs_config_recovery_live_t *live, char *err, size_t err_len)
{
    if (live->projection.coordinator != &g_state.config_coordinator || live->projection.bootstrap != &g_state.config_bootstrap ||
        !projection_storage_ready(&g_state.config_runtime))
        return reject(err, err_len, "recovery projection lost its frozen shared-writer storage binding");
    return lcs_config_projection_step(&live->projection, &g_state.config_runtime.agreement.committed, err, err_len);
}

static void empty_tables(void)
{
    memset(g_state.resources, 0, sizeof(g_state.resources));
    memset(g_state.lease_grants, 0, sizeof(g_state.lease_grants));
    for (size_t i = 0; i < LCS_MAX_RESOURCES; i++)
    { g_state.resources[i].owner_node = -1; g_state.lease_grants[i].owner_node = -1; }
}

int lcs_config_recovery_live_prepare(lcs_config_recovery_live_t *live,
                                       lcs_config_recovery_t *recovery, int epoll_fd,
                                       char *err, size_t err_len)
{
    const lcs_config_runtime_t *runtime = &g_state.config_runtime;
    if (!live || live->prepared || !recovery || recovery->coordinator || recovery->phase != LCS_CONFIG_RECOVERY_IDLE ||
        !runtime->requires_recovery || !runtime->base || !projection_storage_ready(runtime) || !runtime->application.applied.revision || !g_state.instance_id ||
        g_state.self_index != lcs_config_self_index(runtime->base) || config_reload_in_progress() ||
        g_state.resource_generation_enabled || g_state.voting_ready || g_state.config_reload.active_generation == UINT64_MAX ||
        !lcs_config_barrier_valid(&runtime->barrier, &runtime->activation, &runtime->agreement, &runtime->application) || !pristine())
        return reject(err, err_len, "recovery preparation requires pristine pre-listener state and activated durable authority");
    lcs_config_t *applied = malloc(sizeof(*applied)); uint64_t revision;
    if (!applied) return reject(err, err_len, "cannot allocate authoritative recovery definitions");
    int result = lcs_resource_config_decode(runtime->application.applied.document, runtime->application.applied.length,
        runtime->base, applied, &revision, err, err_len);
    if (!result && (revision != runtime->application.applied.revision || applied->resource_schema_version != LCS_RESOURCE_DYNAMIC_SCHEMA_VERSION ||
        !matches(applied, &runtime->application.applied) || lcs_config_candidate_validate(&g_state.cfg, applied, err, err_len))) result = -1;
    if (result) { free(applied); return -1; }
    /* Fence before any cancellation callback can inspect resource state. */
    g_state.config_application_fenced = g_state.config_application_remapping = true;
    g_state.config_application_drain_mask = UINT32_MAX;
    result = peer_install_resource_generation(epoll_fd, &runtime->application.applied);
    if (result) { free(applied); return reject(err, err_len, "cannot establish fenced recovery generation"); }
    empty_tables(); g_state.cfg = *applied; free(applied);
    g_state.config_reload.active_generation++;
    g_state.quorum_needed = lcs_config_quorum(&g_state.cfg);
    g_state.voting_not_before_ms = lcs_now_ms() + (uint64_t)g_state.cfg.lease_ms + g_state.cfg.peer_timeout_ms;
    *live = (lcs_config_recovery_live_t){.recovery = recovery, .epoll_fd = epoll_fd, .prepared = true};
    lcs_config_projection_init(&live->projection, &g_state.config_coordinator, &g_state.config_bootstrap);
    return 0;
}

int lcs_config_recovery_live_install(void *context, const lcs_config_reconcile_view_t *view,
                                       char *err, size_t err_len)
{
    lcs_config_recovery_live_t *live = context;
    lcs_config_recovery_t *recovery = live ? live->recovery : NULL;
    const lcs_config_runtime_t *runtime = &g_state.config_runtime;
    if (!live || !live->prepared || !recovery || recovery->phase != LCS_CONFIG_RECOVERY_INSTALL ||
        recovery->coordinator != &g_state.config_coordinator || recovery->coordinator->runtime != runtime ||
        recovery->coordinator->application_owner != recovery || recovery->coordinator->closing || recovery->coordinator->failed ||
        recovery->install != lcs_config_recovery_live_install || recovery->context != live ||
        !view || !view->value || !view->target || !view->old_config || !view->plan || view->attempt != recovery->attempt ||
        !view->attempt || !lcs_config_value_equal(view->value, &recovery->target) || !runtime->requires_recovery ||
        !g_state.config_application_fenced || !g_state.config_application_remapping || !g_state.resource_generation_enabled ||
        g_state.resource_generation_admitted ||
        g_state.voting_ready || !lcs_config_startup_verified(recovery->cleanup, runtime, g_state.instance_id) ||
        !matches(view->target, view->value) || view->target->resource_schema_version != LCS_RESOURCE_DYNAMIC_SCHEMA_VERSION)
        return reject(err, err_len, "recovery installation lacks exact cleanup, catalog, attempt or global fences");
    if (!recovery->quorum_only && !recovery->current_install)
    {
        lcs_config_owner_scope_t scope;
        if (!recovery->network || lcs_config_owner_scope(&runtime->application, &scope, err, err_len) ||
            !lcs_config_owner_scope_equal(&scope, &recovery->network->round.scope))
            return reject(err, err_len, "recovery installation owner scope does not match the catalog");
        int proof = lcs_config_owner_poll(&recovery->network->round, lcs_now_ms());
        if (proof < 0) return reject(err, err_len, "recovery installation owner proof was invalidated");
        if (!proof) return 0;
    }
    if (!recovery->quorum_only && recovery->current_install &&
        (!recovery->network || !recovery->network->retired || recovery->network->receipt.sealed ||
        recovery->network->retired_scope.target_revision != runtime->application.applied.revision ||
        strcmp(recovery->network->retired_scope.target_value, runtime->application.applied.digest)))
        return reject(err, err_len, "recovery current-table installation requires completed receipt retirement");
    if (work_busy()) return 0;
    if (live->attempt && view->attempt <= live->attempt)
    {
        if (view->attempt != live->attempt || strcmp(live->digest, view->value->digest) || !matches(&g_state.cfg, view->value) ||
            g_state.resource_generation.revision != view->value->revision || strcmp(g_state.resource_generation.digest, view->value->digest) ||
            strcmp(g_state.resource_generation.membership, runtime->agreement.membership))
            return reject(err, err_len, "stale or conflicting recovery installation attempt");
        return publish_current(live, err, err_len);
    }
    if (!matches(&g_state.cfg, &runtime->application.applied) || !matches(view->old_config, &runtime->application.applied) ||
        lcs_config_candidate_validate(&g_state.cfg, view->target, err, err_len) || g_state.config_reload.active_generation == UINT64_MAX)
        return reject(err, err_len, "recovery runtime table differs from its durable old generation");
    for (size_t i = 0; i < view->target->resource_count; i++)
    {
        int old = lcs_config_resource_index(view->old_config, view->target->resources[i].name);
        if (old >= 0 && (view->old_config->resources[old].birth_revision != view->target->resources[i].birth_revision ||
            !lcs_config_backend_equal(&view->old_config->resources[old], &view->target->resources[i]))) old = -1;
        if (view->plan->new_to_old[i] != old) return reject(err, err_len, "recovery incarnation remapping plan is inconsistent");
    }
    int published = publish_current(live, err, err_len);
    if (published != 1) return published;
    struct tables { resource_runtime_t resources[LCS_MAX_RESOURCES]; lease_grant_t grants[LCS_MAX_RESOURCES]; };
    struct tables *old = malloc(sizeof(*old));
    if (!old) return reject(err, err_len, "cannot retain old recovery runtime tables");
    int result = peer_install_resource_generation(live->epoll_fd, view->value);
    if (result) { free(old); return result > 0 ? 0 : reject(err, err_len, "cannot install recovery resource generation"); }
    /* Generation cancellation callbacks run against the old indices first. */
    memcpy(old->resources, g_state.resources, sizeof(old->resources));
    memcpy(old->grants, g_state.lease_grants, sizeof(old->grants));
    empty_tables();
    for (size_t i = 0; i < view->target->resource_count; i++)
    {
        int previous = view->plan->new_to_old[i];
        if (previous < 0) continue;
        const resource_runtime_t *prior = &old->resources[previous];
        g_state.resources[i].epoch = prior->epoch;
        g_state.resources[i].failover_count = prior->failover_count;
        g_state.resources[i].home_generation = prior->home_generation;
        g_state.resources[i].home_blocked = prior->home_blocked;
        g_state.resources[i].disabled_generation = prior->disabled_generation;
        g_state.resources[i].disabled = prior->disabled;
        g_state.lease_grants[i] = old->grants[previous];
    }
    free(old); g_state.cfg = *view->target;
    g_state.config_reload.active_generation++;
    live->attempt = view->attempt; memcpy(live->digest, view->value->digest, sizeof(live->digest));
    return 1;
}

static void cancel_admission(void *context)
{
    lcs_config_recovery_live_t *live = context;
    g_state.config_application_fenced = g_state.config_application_remapping = true;
    g_state.config_application_drain_mask = UINT32_MAX;
    g_state.resource_generation_admitted = false;
    g_state.voting_ready = false; g_state.votes_seen = 0;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        g_state.peers[i].voting_ready = false;
        g_state.peers[i].initial_sync_complete = false;
        g_state.peers[i].resource_generation_matches = false;
        g_state.peers[i].next_heartbeat_ms = 0;
    }
    live->synchronizing = false;
}

static bool generation_matches_applied(void)
{
    const lcs_config_runtime_t *runtime = &g_state.config_runtime;
    return g_state.resource_generation_enabled &&
        g_state.resource_generation.revision == runtime->application.applied.revision &&
        !strcmp(g_state.resource_generation.digest, runtime->application.applied.digest) &&
        !strcmp(g_state.resource_generation.membership, runtime->agreement.membership) &&
        matches(&g_state.cfg, &runtime->application.applied);
}

static bool projection_matches_applied(const lcs_config_recovery_live_t *live)
{
    const lcs_config_runtime_t *runtime = &g_state.config_runtime;
    return !live->projection.completion && projection_storage_ready(runtime) &&
        g_state.config_coordinator.projected_revision == runtime->application.applied.revision &&
        !strcmp(g_state.config_coordinator.projected_digest, runtime->application.applied.digest);
}

static int admit(void *context, const lcs_config_reconcile_view_t *view, char *err, size_t err_len)
{
    lcs_config_recovery_live_t *live = context;
    lcs_config_recovery_t *recovery = live->recovery;
    if (!live->prepared || live->admitted || recovery->phase != LCS_CONFIG_RECOVERY_ADMITTING ||
        recovery->coordinator != &g_state.config_coordinator || !g_state.config_runtime.requires_recovery ||
        !g_state.config_application_fenced || g_state.config_application_drain_mask != UINT32_MAX ||
        !lcs_config_value_equal(view->value, &g_state.config_runtime.application.applied) ||
        !g_state.quorum_needed || g_state.quorum_needed != lcs_config_quorum(&g_state.cfg) ||
        !generation_matches_applied() || !projection_matches_applied(live))
        return reject(err, err_len, "admission generation, projection or recovery fences changed");
    if (work_busy()) return 0;
    if (!live->synchronizing)
    {
        if (!g_state.config_application_remapping || g_state.resource_generation_admitted || g_state.voting_ready)
            return reject(err, err_len, "admission must begin from a fully fenced generation");
        for (size_t i = 0; i < g_state.cfg.node_count; i++)
        {
            g_state.peers[i].voting_ready = false;
            g_state.peers[i].initial_sync_complete = false;
            g_state.peers[i].resource_generation_matches = false;
        }
        if (peer_admit_resource_generation()) return reject(err, err_len, "cannot enable matching-generation restart sync");
        g_state.config_application_remapping = false;
        live->synchronizing = true;
    }
    if (!g_state.resource_generation_admitted || g_state.config_application_remapping)
        return reject(err, err_len, "matching-generation restart sync lost admission");
    cluster_update_recovery_state(); cluster_recompute_votes();
    uint32_t synchronized = 1, voters = cluster_local_voting_ready() ? 1 : 0;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if ((int)i == g_state.self_index) continue;
        const peer_runtime_t *peer = &g_state.peers[i];
        if (peer->fd < 0 || peer->conn_state != LCS_PEER_ESTABLISHED || !peer->instance_id ||
            peer->instance_id != recovery->network->instances[i] || peer->protocol_version < LCS_PROTO_FEATURE_RESOURCE_GENERATION ||
            !peer->initial_sync_complete || !peer->resource_generation_known || !peer->resource_generation_matches ||
            !lcs_resource_generation_equal(&peer->resource_generation, &g_state.resource_generation) || !cluster_node_is_online(i)) continue;
        synchronized++; if (peer->voting_ready) voters++;
    }
    return lcs_now_ms() >= g_state.voting_not_before_ms && synchronized >= g_state.quorum_needed &&
        voters >= g_state.quorum_needed && cluster_local_voting_ready() && cluster_has_quorum() ? 1 : 0;
}

static void complete_admission(void *context)
{
    lcs_config_recovery_live_t *live = context;
    g_state.config_application_fenced = false; g_state.config_application_drain_mask = 0;
    live->synchronizing = false; live->admitted = true;
}

int lcs_config_recovery_live_admission_receipt(const lcs_config_recovery_live_t *live,
                                                lcs_config_request_admission_t *receipt)
{
    if (!live || !receipt || !live->prepared || !live->admitted || !live->attempt || !live->recovery ||
        live->recovery->phase != LCS_CONFIG_RECOVERY_COMPLETE || live->recovery->attempt != live->attempt ||
        live->recovery->coordinator != &g_state.config_coordinator || g_state.config_coordinator.failed ||
        g_state.config_coordinator.closing || g_state.config_coordinator.application_owner ||
        g_state.config_coordinator.phase != LCS_CONFIG_COORD_READY || g_state.config_coordinator.storage.pid > 0 ||
        g_state.config_runtime.application.pending.document ||
        !lcs_config_value_equal(&g_state.config_runtime.application.applied, &g_state.config_runtime.agreement.committed) ||
        strcmp(live->digest, g_state.config_runtime.application.applied.digest)) return -1;
    *receipt = (lcs_config_request_admission_t){.issuer = live, .attempt = live->attempt,
        .revision = g_state.config_runtime.application.applied.revision};
    memcpy(receipt->digest, live->digest, sizeof(receipt->digest)); return 0;
}

int lcs_config_recovery_live_begin_admission(lcs_config_recovery_live_t *live, char *err, size_t err_len)
{
    if (!live || !live->prepared || live->admitted || live->synchronizing || !live->recovery ||
        live->recovery->coordinator != &g_state.config_coordinator ||
        live->recovery->install != lcs_config_recovery_live_install || live->recovery->context != live ||
        live->attempt != live->recovery->attempt || strcmp(live->digest, g_state.config_runtime.application.applied.digest) ||
        !g_state.config_application_fenced || !g_state.config_application_remapping ||
        g_state.config_application_drain_mask != UINT32_MAX || g_state.resource_generation_admitted || g_state.voting_ready ||
        !generation_matches_applied() || !projection_matches_applied(live) ||
        lcs_config_recovery_begin_admission(live->recovery, admit, cancel_admission, complete_admission, live))
        return reject(err, err_len, "recovery is not ready for explicit generation/restart admission");
    return 0;
}
