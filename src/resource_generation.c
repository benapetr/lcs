// SPDX-License-Identifier: GPL-3.0-or-later
#include "peer.h"
#include "config_reload.h"
#include "lease.h"
#include "move.h"
#include <stdlib.h>
#include <string.h>

static bool installed_definitions_match(const lcs_config_value_t *value)
{
    if (g_state.cfg.resource_schema_version != LCS_RESOURCE_DYNAMIC_SCHEMA_VERSION)
        return false;
    char *document = NULL, error[256];
    size_t length;
    if (lcs_resource_config_encode(&g_state.cfg, value->revision, &document, &length, error, sizeof(error)))
        return false;
    bool matches = length == value->length && !memcmp(document, value->document, length);
    free(document);
    return matches;
}

int peer_install_resource_generation(int epoll_fd, const lcs_config_value_t *value)
{
    const lcs_config_runtime_t *runtime = &g_state.config_runtime;
    if (!value || !value->revision || !runtime->base || config_reload_in_progress() ||
        !lcs_config_barrier_valid(&runtime->barrier, &runtime->activation, &runtime->agreement, &runtime->application) ||
        (!lcs_config_value_equal(value, &runtime->application.pending) &&
         !lcs_config_value_equal(value, &runtime->application.applied)))
        return -1;
    lcs_resource_generation_t generation = { .revision = value->revision };
    memcpy(generation.membership, runtime->agreement.membership, sizeof(generation.membership));
    memcpy(generation.digest, value->digest, sizeof(generation.digest));
    if (g_state.resource_generation_enabled)
    {
        if (generation.revision < g_state.resource_generation.revision)
            return -1;
        if (generation.revision == g_state.resource_generation.revision)
            return lcs_resource_generation_equal(&generation, &g_state.resource_generation) ? 0 : -1;
    }
    /* Check every slot, including removed resources, BEFORE changing indices. */
    for (size_t i = 0; i < LCS_MAX_RESOURCES; i++)
    {
        const resource_runtime_t *resource = &g_state.resources[i];
        if (resource->hook.pid > 0 || resource->hook.type || resource->hook.next_type || resource->vip_probe.pid > 0 ||
            resource->vip.pid > 0 || resource->vip.op || resource->vip.next_op || resource->service.pid > 0 ||
            resource->service.op || resource->service.next_op || resource->handoff_pending)
            return 1;
    }
    g_state.resource_generation = generation;
    g_state.resource_generation_enabled = true;
    g_state.resource_generation_admitted = false;
    g_state.voting_ready = false;
    g_state.votes_seen = 0;
    move_cancel_all(epoll_fd, "resource generation changing");
    lease_cancel_all_operations();
    peer_close_all_for_config_reload(epoll_fd);
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        g_state.peers[i].voting_ready = false;
        g_state.peers[i].initial_sync_complete = false;
        g_state.peers[i].resource_generation_known = false;
        g_state.peers[i].resource_generation_matches = false;
    }
    return 0;
}

int peer_admit_resource_generation(void)
{
    const lcs_config_runtime_t *runtime = &g_state.config_runtime;
    lcs_resource_generation_t applied = { .revision = runtime->application.applied.revision };
    memcpy(applied.membership, runtime->agreement.membership, sizeof(applied.membership));
    memcpy(applied.digest, runtime->application.applied.digest, sizeof(applied.digest));
    if (!g_state.resource_generation_enabled || !g_state.config_coordinator.initialized ||
        g_state.config_coordinator.closing || g_state.config_coordinator.failed ||
        g_state.config_coordinator.phase != LCS_CONFIG_COORD_READY || g_state.config_coordinator.storage.pid > 0 ||
        runtime->application.pending.document ||
        !lcs_config_barrier_valid(&runtime->barrier, &runtime->activation, &runtime->agreement, &runtime->application) ||
        !lcs_resource_generation_equal(&applied, &g_state.resource_generation) ||
        !lcs_config_value_equal(&runtime->application.applied, &runtime->agreement.committed) ||
        !installed_definitions_match(&runtime->application.applied))
        return -1;
    if (g_state.resource_generation_admitted)
        return 0;
    g_state.resource_generation_admitted = true;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        g_state.peers[i].state_sync_pending = true;
        g_state.peers[i].next_heartbeat_ms = 0;
    }
    /* Normal lease restart recovery still controls voting_ready. */
    return 0;
}
