// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_manager.h"
#include "config_recovery_live.h"
#include "config_live.h"
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
    lcs_config_reconcile_t application;
    lcs_config_live_t application_live;
    bool application_initialized, applying;
    bool enabled, started, active, failed, stopping;
    lcs_config_recovery_phase_t logged_phase;
    lcs_config_reconcile_phase_t logged_application_phase;
    uint64_t retry_ms, outcome_completion;
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
    bool activation = lcs_config_runtime_activation_chosen(runtime);
    if (manager.enabled || !runtime->requires_recovery || (!runtime->application.applied.revision && !activation) ||
        !runtime->base || !lcs_config_barrier_valid(&runtime->barrier, &runtime->activation, &runtime->agreement, &runtime->application) ||
        (g_state.cfg.nodes[g_state.self_index].role == LCS_NODE_FULL && !activation && !runtime->application.history[0].document))
    {
        snprintf(err, err_len, "chosen dynamic authority, durable readiness and known application history are required");
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
bool config_manager_applying(void)
{
    const lcs_config_runtime_t *runtime = &g_state.config_runtime;
    return manager.enabled && manager.active && (manager.applying || runtime->application.pending.document ||
        !lcs_config_value_equal(&runtime->application.applied, &runtime->agreement.committed));
}
bool config_manager_failed(void) { return manager.failed; }

void config_manager_diagnostics(lcs_config_status_view_t *view)
{
    if (!manager.enabled) return;
    const lcs_config_application_t *application = &g_state.config_runtime.application;
    const lcs_config_value_t *committed = &g_state.config_runtime.agreement.committed;
    view->authority = LCS_CONFIG_AUTHORITY_JSON;
    view->phase = manager.stopping ? LCS_CONFIG_DIAG_STOPPING :
        manager.failed ? LCS_CONFIG_DIAG_FAILED : config_manager_applying() ?
        (manager.application.phase == LCS_CONFIG_RECONCILE_BLOCKED ? LCS_CONFIG_DIAG_BLOCKED :
         manager.application.phase == LCS_CONFIG_RECONCILE_ADMIT && !application->pending.document &&
         lcs_config_value_equal(&application->applied, committed) ? LCS_CONFIG_DIAG_ADMITTING : LCS_CONFIG_DIAG_RECOVERING) :
        manager.active ? LCS_CONFIG_DIAG_ACTIVE :
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
    else if (manager.applying && manager.application.phase == LCS_CONFIG_RECONCILE_BLOCKED)
        snprintf(view->error, sizeof(view->error), "%s", manager.application.error);
    else if (manager.recovery.phase == LCS_CONFIG_RECOVERY_BLOCKED)
        snprintf(view->error, sizeof(view->error), "%s", manager.recovery.error);
}

const lcs_config_value_t *config_manager_error_target(void)
{
    if (!manager.enabled || manager.failed) return NULL;
    if (manager.applying && manager.application.phase == LCS_CONFIG_RECONCILE_BLOCKED)
        return &manager.application.value;
    if (!manager.active && manager.recovery.phase == LCS_CONFIG_RECOVERY_BLOCKED)
        return &manager.recovery.target;
    return NULL;
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

/* Admission has actually completed before this historical receipt is written.
 * A crash before its durable publication leaves the client outcome unknown; an
 * applied catalog or a later revision never fabricates the missing receipt. */
static int record_admission(bool live_application)
{
    lcs_config_coordinator_t *coordinator = &g_state.config_coordinator;
    if (!g_state.config_runtime.requests.store) return 1; /* Older catalog, no invented history. */
    if (coordinator->failed || coordinator->closing) { fail("request admission persistence requires recovery"); return -1; }
    if (manager.outcome_completion)
    {
        if (coordinator->completed_requests < manager.outcome_completion) return 0;
        if (coordinator->completed_requests != manager.outcome_completion ||
            (coordinator->last_request_result != LCS_CONFIG_REQUEST_RECORDED &&
             coordinator->last_request_result != LCS_CONFIG_REQUEST_EXISTS))
        { fail("cannot durably record admitted request; retain decision and recover"); return -1; }
        manager.outcome_completion = 0; return 1;
    }
    lcs_config_request_admission_t receipt;
    int proof = live_application ? lcs_config_reconcile_admission_receipt(&manager.application, &receipt) :
        lcs_config_recovery_live_admission_receipt(&manager.live, &receipt);
    if (proof) { fail("completed application lacks exact final admission receipt"); return -1; }
    lcs_config_agreement_job_t job = {.action = LCS_CONFIG_REQUEST_APPLY,
        .admission_catalog = &g_state.config_runtime.application, .admission = &receipt};
    char error[256];
    int started = lcs_config_coordinator_start_requests(coordinator, &job, error, sizeof(error));
    if (started > 0) return 0;
    if (started < 0) { fail("cannot start admitted-request persistence"); return -1; }
    manager.outcome_completion = coordinator->completed_requests + 1; return 0;
}

static void process_application(uint64_t now)
{
    lcs_config_coordinator_t *coordinator = &g_state.config_coordinator;
    if (coordinator->failed || coordinator->closing)
    { fail(*coordinator->error ? coordinator->error : "live configuration coordinator lost eligibility"); return; }
    if (!manager.applying)
    {
        if (!config_manager_applying() || coordinator->phase != LCS_CONFIG_COORD_READY || coordinator->storage.pid > 0) return;
        if (!manager.application_initialized || lcs_config_reconcile_start(&manager.application))
        { fail("cannot start live configuration application"); return; }
        manager.applying = true;
        lcs_log_info("distributed resource application started applied=%llu committed=%llu",
            (unsigned long long)g_state.config_runtime.application.applied.revision,
            (unsigned long long)g_state.config_runtime.agreement.committed.revision);
    }
    (void)lcs_config_reconcile_process(&manager.application);
    if (manager.application.phase == LCS_CONFIG_RECONCILE_RECOVERY)
    { fail(manager.application.error); return; }
    if (manager.application.phase != manager.logged_application_phase)
    {
        if (manager.application.phase == LCS_CONFIG_RECONCILE_BLOCKED)
            lcs_log_warn("distributed resource application blocked: %s; cleanup failures require repair and restart", manager.application.error);
        manager.logged_application_phase = manager.application.phase;
    }
    if (manager.application.phase == LCS_CONFIG_RECONCILE_BLOCKED && now >= manager.retry_ms)
    {
        manager.retry_ms = now + 1000u;
        lcs_config_owner_network_t *network = &g_state.config_owners;
        /* A transport retry retains pinned local proof. Failed cleanup never
         * becomes success merely because its timer expired. */
        if (network->receipt.failed) return;
        if ((network->round.active && network->round.failed) || (network->release.active && network->release.failed))
            if (lcs_config_owner_network_retry(network, now)) return;
        (void)lcs_config_reconcile_retry(&manager.application);
    }
    else if (manager.application.phase == LCS_CONFIG_RECONCILE_DONE)
    {
        if (record_admission(true) != 1) return;
        manager.applying = false;
        lcs_log_info("distributed resource application admitted revision=%llu; normal lease placement enabled",
            (unsigned long long)g_state.config_runtime.application.applied.revision);
    }
}

static void attach_activation(int epoll_fd)
{
    if (manager.enabled || !lcs_config_runtime_activation_chosen(&g_state.config_runtime)) return;
    lcs_config_coordinator_t *coordinator = &g_state.config_coordinator;
    if (!coordinator->initialized || coordinator->runtime != &g_state.config_runtime ||
        coordinator->phase != LCS_CONFIG_COORD_READY || coordinator->storage.pid > 0 || coordinator->application_owner) return;
    char error[256];
    int result = lcs_config_recovery_live_attach_activation(&manager.live, &manager.recovery, epoll_fd, error, sizeof(error));
    if (result > 0) return;
    manager.enabled = true;
    if (result < 0) { fail(error); return; }
    if (config_peer_owner_prepare()) { fail("cannot attach activation owner transport"); return; }
    result = lcs_config_recovery_start(&manager.recovery, coordinator, &g_state.config_owners,
        lcs_config_recovery_live_install, &manager.live);
    manager.started = manager.recovery.coordinator != NULL;
    if (result) { fail("cannot acquire live activation application hold"); return; }
    lcs_log_info("chosen resource activation handed to recovery without restart; resource fences held");
}

void config_manager_process(int epoll_fd)
{
    attach_activation(epoll_fd);
    if (!manager.enabled || manager.failed || manager.stopping) return;
    lcs_config_runtime_t *runtime = &g_state.config_runtime;
    if (manager.active)
    {
        process_application(lcs_now_ms());
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
        if (record_admission(false) != 1) return;
        manager.active = true;
        lcs_log_info("distributed resource recovery admitted revision=%llu; normal lease placement enabled",
            (unsigned long long)runtime->application.applied.revision);
        if (lcs_config_recovery_close(&manager.recovery)) { fail("cannot release completed recovery controller"); return; }
        manager.started = false;
        if (lcs_config_live_init_with_owner_network(&manager.application_live, epoll_fd, &g_state.config_coordinator,
            &g_state.config_bootstrap, &g_state.config_owners))
        { fail("cannot prepare live application after startup admission"); return; }
        lcs_config_reconcile_init(&manager.application, &g_state.config_coordinator, lcs_config_live_ops(), &manager.application_live);
        manager.application_initialized = true;
    }
    else if (result < 0 && manager.recovery.phase != LCS_CONFIG_RECOVERY_BLOCKED)
        fail("unexpected recovery controller result");
}

void config_manager_stop(void)
{
    if (!manager.enabled) return;
    manager.stopping = true;
    if (manager.started) lcs_config_recovery_stop(&manager.recovery);
    else if (manager.application_initialized) lcs_config_reconcile_stop(&manager.application);
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
    if (manager.application_initialized && lcs_config_reconcile_close(&manager.application)) return -1;
    memset(&manager, 0, sizeof(manager)); return 0;
}
