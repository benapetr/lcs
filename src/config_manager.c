// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_manager.h"
#include "config_recovery_live.h"
#include "config_peer.h"
#include "config_reload.h"
#include "daemon_state.h"
#include "peer.h"
#include "log.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct
{
    lcs_config_recovery_t recovery;
    lcs_config_recovery_live_t live;
    bool enabled, started, active, failed, stopping;
    lcs_config_recovery_phase_t logged_phase;
    uint64_t retry_ms;
    char error[256];
} manager;

static void fail(const char *message)
{
    if (!manager.failed) lcs_log_error("distributed resource manager failed: %s; retain fences and restart after repair", message);
    manager.failed = true;
    snprintf(manager.error, sizeof(manager.error), "%s", message);
    g_state.config_application_fenced = g_state.config_application_remapping = true;
    g_state.config_application_drain_mask = UINT32_MAX;
    g_state.resource_generation_admitted = g_state.voting_ready = false; g_state.votes_seen = 0;
    lcs_config_owner_network_stop(&g_state.config_owners);
    lcs_config_coordinator_abort(&g_state.config_coordinator, message);
}

int config_manager_prepare(char *err, size_t err_len)
{
    lcs_config_runtime_t *runtime = &g_state.config_runtime;
    if (manager.enabled || !runtime->requires_recovery || !runtime->application.applied.revision ||
        !runtime->base || !lcs_config_barrier_valid(&runtime->barrier, &runtime->activation, &runtime->agreement, &runtime->application) ||
        (g_state.cfg.nodes[g_state.self_index].role == LCS_NODE_FULL && !runtime->application.history[0].document))
    {
        snprintf(err, err_len, "activated durable readiness and known application history are required; revision-zero activation and missing-history recovery remain unavailable");
        return -1;
    }
    manager.enabled = true;
    g_state.config_bootstrap.initialization_locked = true;
    g_state.config_bootstrap.prepared = false; g_state.config_bootstrap_initialized = true;
    lcs_config_coordinator_init(&g_state.config_coordinator, runtime, &g_state.config_readiness);
    if (lcs_config_coordinator_recover(&g_state.config_coordinator) || config_peer_owner_prepare())
    {
        snprintf(err, err_len, "cannot restore configuration coordinator or owner transport");
        return -1;
    }
    if (lcs_config_recovery_live_prepare(&manager.live, &manager.recovery, -1, err, err_len)) return -1;
    int result = lcs_config_recovery_start(&manager.recovery, &g_state.config_coordinator, &g_state.config_owners,
        lcs_config_recovery_live_install, &manager.live);
    manager.started = manager.recovery.coordinator != NULL;
    if (result)
    { snprintf(err, err_len, "cannot start durable configuration recovery: %.180s", manager.recovery.error); return -1; }
    lcs_log_info("resource definitions restored from durable JSON applied=%llu committed=%llu; recovery fences held",
        (unsigned long long)runtime->application.applied.revision, (unsigned long long)runtime->agreement.committed.revision);
    return 0;
}

void config_manager_attach(int epoll_fd) { if (manager.enabled) manager.live.epoll_fd = epoll_fd; }
bool config_manager_enabled(void) { return manager.enabled; }
bool config_manager_recovering(void) { return manager.enabled && !manager.active; }
bool config_manager_failed(void) { return manager.failed; }

void config_manager_diagnostics(lcs_config_status_view_t *view)
{
    if (!manager.enabled) return;
    const lcs_config_application_t *application = &g_state.config_runtime.application;
    const lcs_config_value_t *committed = &g_state.config_runtime.agreement.committed;
    view->authority = LCS_CONFIG_AUTHORITY_JSON;
    view->phase = manager.stopping ? LCS_CONFIG_DIAG_STOPPING :
        manager.failed ? LCS_CONFIG_DIAG_FAILED : manager.active ? LCS_CONFIG_DIAG_ACTIVE :
        manager.recovery.phase == LCS_CONFIG_RECOVERY_BLOCKED ? LCS_CONFIG_DIAG_BLOCKED :
        manager.recovery.phase == LCS_CONFIG_RECOVERY_ADMITTING ? LCS_CONFIG_DIAG_ADMITTING : LCS_CONFIG_DIAG_RECOVERING;
    view->admitted = view->phase == LCS_CONFIG_DIAG_ACTIVE;
    view->committed_revision = committed->revision;
    view->applied_revision = application->applied.revision;
    view->pending_revision = application->pending.revision;
    memcpy(view->committed_digest, committed->digest, sizeof(view->committed_digest));
    memcpy(view->applied_digest, application->applied.digest, sizeof(view->applied_digest));
    memcpy(view->pending_digest, application->pending.digest, sizeof(view->pending_digest));
    if (manager.failed) snprintf(view->error, sizeof(view->error), "%s", manager.error);
    else if (manager.recovery.phase == LCS_CONFIG_RECOVERY_BLOCKED)
        snprintf(view->error, sizeof(view->error), "%s", manager.recovery.error);
}

int config_manager_has_historical_vips(char *err, size_t err_len)
{
    if (!manager.enabled || g_state.cfg.nodes[g_state.self_index].role != LCS_NODE_FULL) return 0;
    lcs_config_t *cfg = malloc(sizeof(*cfg));
    if (!cfg) { snprintf(err, err_len, "cannot inspect historical VIP capabilities"); return -1; }
    int result = 0;
    for (size_t i = 0; i < LCS_CONFIG_APPLICATION_HISTORY && !result; i++)
    {
        const lcs_config_value_t *value = &g_state.config_runtime.application.history[i]; uint64_t revision;
        if (!value->document) continue;
        if (lcs_resource_config_decode(value->document, value->length, g_state.config_runtime.base, cfg, &revision, err, err_len)) result = -1;
        else for (size_t j = 0; j < cfg->resource_count; j++) if (cfg->resources[j].type == LCS_RESOURCE_VIP) { result = 1; break; }
    }
    free(cfg); return result;
}

static int send_owner(void *context, size_t node, const void *payload, size_t length)
{ return peer_queue_config_owner(*(int *)context, (int)node, payload, (uint32_t)length); }

void config_manager_process(int epoll_fd)
{
    if (!manager.enabled || manager.failed || manager.stopping) return;
    lcs_config_runtime_t *runtime = &g_state.config_runtime;
    if (manager.active)
    {
        /* Live mutation/application is a separate unfinished manager path. Do
         * not grant old-definition leases after learning unsupported new state. */
        if (runtime->application.pending.document || !lcs_config_value_equal(&runtime->application.applied, &runtime->agreement.committed))
            fail("new committed definitions require restart recovery; live application is not enabled");
        return;
    }
    uint64_t instances[LCS_MAX_NODES] = {0}, now = lcs_now_ms();
    instances[g_state.self_index] = g_state.instance_id;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        const peer_runtime_t *peer = &g_state.peers[i];
        if ((int)i != g_state.self_index && peer->fd >= 0 && peer->conn_state == LCS_PEER_ESTABLISHED &&
            peer->protocol_version >= LCS_PROTO_FEATURE_CONFIG_OWNERS) instances[i] = peer->instance_id;
    }
    int result = lcs_config_recovery_process(&manager.recovery, instances, now, send_owner, &epoll_fd);
    if (manager.recovery.phase == LCS_CONFIG_RECOVERY_FAILED) { fail(manager.recovery.error); return; }
    if (manager.recovery.phase != manager.logged_phase)
    {
        if (manager.recovery.phase == LCS_CONFIG_RECOVERY_BLOCKED)
            lcs_log_warn("distributed resource recovery blocked: %s; retrying with fresh evidence after repair", manager.recovery.error);
        manager.logged_phase = manager.recovery.phase;
    }
    if (manager.recovery.phase == LCS_CONFIG_RECOVERY_BLOCKED && now >= manager.retry_ms)
    {
        manager.retry_ms = now + 1000u;
        (void)lcs_config_recovery_retry(&manager.recovery, now);
    }
    else if (manager.recovery.phase == LCS_CONFIG_RECOVERY_READY)
    {
        char error[256];
        if (lcs_config_recovery_live_begin_admission(&manager.live, error, sizeof(error)))
        {
            /* A checkpoint writer may own the coordinator between recovery
             * steps. Wait for it rather than treating busy storage as proof. */
            if (g_state.config_coordinator.phase == LCS_CONFIG_COORD_READY && g_state.config_coordinator.storage.pid <= 0)
                fail(error);
        }
    }
    else if (manager.recovery.phase == LCS_CONFIG_RECOVERY_COMPLETE)
    {
        manager.active = true;
        lcs_log_info("distributed resource recovery admitted revision=%llu; normal lease placement enabled",
            (unsigned long long)runtime->application.applied.revision);
        if (lcs_config_recovery_close(&manager.recovery)) { fail("cannot release completed recovery controller"); return; }
        manager.started = false;
    }
    else if (result < 0 && manager.recovery.phase != LCS_CONFIG_RECOVERY_BLOCKED)
        fail("unexpected recovery controller result");
}

void config_manager_stop(void)
{
    if (!manager.enabled) return;
    manager.stopping = true;
    if (manager.started) lcs_config_recovery_stop(&manager.recovery);
    else lcs_config_coordinator_stop(&g_state.config_coordinator);
}
void config_manager_shutdown_process(void)
{
    if (manager.enabled && manager.started)
        (void)lcs_config_recovery_process(&manager.recovery, NULL, lcs_now_ms(), NULL, NULL);
}
bool config_manager_stopped(void)
{
    return !manager.enabled || (lcs_config_coordinator_can_close(&g_state.config_coordinator) &&
        (!manager.started || manager.recovery.phase == LCS_CONFIG_RECOVERY_STOPPED));
}
int config_manager_close(void)
{
    if (!config_manager_stopped()) return -1;
    if (manager.started && lcs_config_recovery_close(&manager.recovery)) return -1;
    memset(&manager, 0, sizeof(manager)); return 0;
}
