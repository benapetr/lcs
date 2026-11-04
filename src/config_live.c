// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_live.h"
#include "config_candidate.h"
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
    live->bound = true; live->installed = false;
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

static int quiesce(void *context, const lcs_config_reconcile_view_t *view, char *err, size_t length)
{
    lcs_config_live_t *live = context;
    if (bind_attempt(live, view, err, length))
        return -1;
    return provider_result(live->project(live->barrier_context,
        &g_state.config_runtime.agreement.committed, err, length), err, length);
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
    if (live->installed)
        return definitions_equal(&g_state.cfg, view->target, view->value->revision) ? 1 : -1;
    /* Quorum-only nodes skip quiesce, so project before installing their table. */
    if (g_state.cfg.nodes[g_state.self_index].role == LCS_NODE_QUORUM_ONLY)
    {
        int projected = provider_result(live->project(live->barrier_context,
            &g_state.config_runtime.agreement.committed, err, length), err, length);
        if (projected != 1)
            return projected;
    }
    if (g_state.cfg.nodes[g_state.self_index].role == LCS_NODE_FULL)
    {
        int drained = drain(context, view, err, length);
        if (drained != 1)
            return drained;
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
        peer_admit_resource_generation())
    { snprintf(err, length, "applied resource generation is not ready for admission"); return -1; }
    g_state.config_application_remapping = false; /* Allow the new generation's restart state sync. */
    if (!cluster_local_voting_ready() || !cluster_has_quorum())
        return 0;
    g_state.config_application_fenced = false;
    g_state.config_application_drain_mask = 0;
    return 1;
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

const lcs_config_reconcile_ops_t *lcs_config_live_ops(void)
{
    static const lcs_config_reconcile_ops_t ops = {quiesce, drain, owners, install, admit};
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
