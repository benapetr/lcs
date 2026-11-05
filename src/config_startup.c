// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_startup.h"
#include "vip.h"
#include "systemd_service.h"
#include "worker.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static uint32_t all_resources(size_t count)
{
    return count == 32 ? UINT32_MAX : (UINT32_C(1) << count) - 1;
}

static const lcs_config_t *cleanup_config(const lcs_config_startup_t *startup)
{
    return startup->snapshot ? &startup->history_configs[startup->snapshot - 1] : startup->config;
}

static uint32_t *cleanup_completed(lcs_config_startup_t *startup)
{
    return startup->snapshot ? &startup->history_completed[startup->snapshot - 1] : &startup->completed;
}

static void clear_snapshots(lcs_config_startup_t *startup)
{
    free(startup->config); free(startup->history_configs);
    lcs_config_value_clear(&startup->applied); lcs_config_value_clear(&startup->pending);
    for (size_t i = 0; i < LCS_CONFIG_APPLICATION_HISTORY; i++) lcs_config_value_clear(&startup->history[i]);
}

static bool bound(const lcs_config_startup_t *startup)
{
    const lcs_config_runtime_t *runtime = startup->runtime;
    if (!runtime) return false;
    for (size_t i = 0; i < LCS_CONFIG_APPLICATION_HISTORY; i++)
        if ((startup->history[i].document || runtime->application.history[i].document) &&
            !lcs_config_value_equal(&startup->history[i], &runtime->application.history[i]))
            return false;
    return runtime->requires_recovery && runtime->base &&
        lcs_config_barrier_valid(&runtime->barrier, &runtime->activation, &runtime->agreement, &runtime->application) &&
        lcs_config_value_equal(&startup->applied, &runtime->application.applied) &&
        ((!startup->pending.document && !runtime->application.pending.document) ||
            lcs_config_value_equal(&startup->pending, &runtime->application.pending));
}

static int fail(lcs_config_startup_t *startup, const char *message)
{
    startup->phase = LCS_CONFIG_STARTUP_BLOCKED;
    snprintf(startup->error, sizeof(startup->error), "%s", message);
    return -1;
}

int lcs_config_startup_begin(lcs_config_startup_t *startup, const lcs_config_runtime_t *runtime,
                               uint64_t instance, char *err, size_t err_len)
{
    if (!startup || startup->config || startup->pid > 0 || !runtime || !runtime->base || !instance ||
        !runtime->requires_recovery || !runtime->application.applied.document ||
        (!runtime->application.history[0].document && runtime->application.history[1].document) || !lcs_config_barrier_valid(&runtime->barrier,
            &runtime->activation, &runtime->agreement, &runtime->application))
        goto invalid;
    lcs_config_startup_t next = { .runtime = runtime, .instance = instance, .phase = LCS_CONFIG_STARTUP_NEXT };
    next.config = malloc(sizeof(*next.config));
    uint64_t revision;
    if (!next.config || lcs_config_value_copy(&next.applied, &runtime->application.applied) ||
        lcs_config_value_copy(&next.pending, &runtime->application.pending) ||
        lcs_resource_config_decode(next.applied.document, next.applied.length, runtime->base,
            next.config, &revision, err, err_len) || revision != next.applied.revision)
    {
        clear_snapshots(&next);
        goto invalid;
    }
    if (runtime->application.history[0].document)
    {
        next.history_configs = calloc(LCS_CONFIG_APPLICATION_HISTORY, sizeof(*next.history_configs));
        if (!next.history_configs) { clear_snapshots(&next); goto invalid; }
    }
    for (size_t i = 0; i < LCS_CONFIG_APPLICATION_HISTORY; i++)
        if (runtime->application.history[i].document &&
            (lcs_config_value_copy(&next.history[i], &runtime->application.history[i]) ||
             lcs_resource_config_decode(next.history[i].document, next.history[i].length, runtime->base,
                &next.history_configs[i], &revision, err, err_len) || revision != next.history[i].revision))
        { clear_snapshots(&next); goto invalid; }
    int self = lcs_config_self_index(next.config);
    if (self < 0)
    {
        clear_snapshots(&next);
        goto invalid;
    }
    next.quorum_only = next.config->nodes[self].role == LCS_NODE_QUORUM_ONLY;
    if (next.quorum_only || (!next.config->resource_count && !next.history[0].document))
        next.phase = LCS_CONFIG_STARTUP_DONE;
    *startup = next;
    return 0;
invalid:
    if (err && err_len)
        snprintf(err, err_len, "startup cleanup requires recovered frozen authority and an unchanged valid catalog");
    return -1;
}

static int start_hook(lcs_config_startup_t *startup, const char *path, const char *event)
{
    const lcs_config_t *cfg = cleanup_config(startup);
    const lcs_resource_config_t *resource = &cfg->resources[startup->index];
    pid_t parent = getpid(), child = fork();
    if (child < 0)
        return -1;
    if (!child)
    {
        lcs_worker_bind_parent(parent);
        (void)setpgid(0, 0);
        (void)close_range(3, ~0u, 0);
        char revision[32], timeout[32];
        snprintf(revision, sizeof(revision), "%llu", (unsigned long long)(startup->snapshot ? startup->history[startup->snapshot - 1].revision : startup->applied.revision));
        snprintf(timeout, sizeof(timeout), "%u", cfg->hook_timeout_ms);
        setenv("LCS_CLUSTER", cfg->cluster_name, 1);
        setenv("LCS_NODE", cfg->self_name, 1);
        setenv("LCS_RESOURCE", resource->name, 1);
        setenv("LCS_RESOURCE_TYPE", lcs_resource_type_name(resource->type), 1);
        setenv("LCS_SYSTEMD_UNIT", resource->systemd_unit, 1);
        setenv("LCS_VIP", resource->name, 1);
        setenv("LCS_ADDRESS", resource->address, 1);
        setenv("LCS_INTERFACE", resource->interface, 1);
        setenv("LCS_EVENT", event, 1);
        setenv("LCS_EPOCH", "0", 1); setenv("LCS_LEASE_ID", "0", 1);
        setenv("LCS_RECOVERY", "1", 1); setenv("LCS_CONFIG_REVISION", revision, 1);
        setenv("LCS_HOOK_TIMEOUT_MS", timeout, 1);
        lcs_worker_exec_hook(path);
    }
    (void)setpgid(child, child);
    startup->pid = child; startup->hook_worker = true;
    return 0;
}

static void cancel_worker(lcs_config_startup_t *startup)
{
    if (startup->pid <= 0 || startup->killed)
        return;
    if (startup->hook_worker)
    {
        if (kill(-startup->pid, SIGKILL))
            (void)kill(startup->pid, SIGKILL);
    }
    else if (startup->service_worker)
        lcs_systemd_service_cancel(startup->pid);
    else
        lcs_vip_operation_cancel(startup->pid);
    startup->killed = true;
}

static int collect_worker(lcs_config_startup_t *startup, int *result)
{
    if (!startup->hook_worker)
        return startup->service_worker ? lcs_systemd_service_collect(startup->pid, result) :
            lcs_vip_operation_collect(startup->pid, result);
    int status;
    pid_t got;
    do { got = waitpid(startup->pid, &status, WNOHANG); } while (got < 0 && errno == EINTR);
    if (!got) return 0;
    if (got < 0) return -1;
    *result = WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
    return 1;
}

int lcs_config_startup_process(lcs_config_startup_t *startup, uint64_t now_ms)
{
    if (!startup || !startup->config)
        return -1;
    if (!bound(startup))
    {
        startup->invalid = true;
        cancel_worker(startup);
    }
    if (startup->pid > 0)
    {
        bool timed_out = now_ms >= startup->deadline_ms;
        if (timed_out) cancel_worker(startup);
        int result = -1, collected = collect_worker(startup, &result);
        if (!collected) return 0;
        bool failed = collected < 0 || result || startup->killed || timed_out;
        startup->pid = 0; startup->killed = false;
        startup->hook_worker = startup->service_worker = false;
        if (collected < 0) startup->invalid = true;
        if (startup->closing || startup->invalid)
            goto terminal;
        if (startup->phase == LCS_CONFIG_STARTUP_PRE_STOP)
        {
            startup->pre_failed = failed; /* Still remove the backend on pre-stop failure. */
            startup->phase = LCS_CONFIG_STARTUP_BACKEND;
        }
        else if (failed)
            return fail(startup, "startup backend removal or post-stop hook failed; cleanup/fencing repair required");
        else
            startup->phase = startup->phase == LCS_CONFIG_STARTUP_BACKEND ? LCS_CONFIG_STARTUP_POST_STOP : LCS_CONFIG_STARTUP_NEXT;
        if (startup->phase == LCS_CONFIG_STARTUP_NEXT)
        {
            if (startup->pre_failed)
                return fail(startup, "startup pre-stop hook failed; backend removed but cleanup requires repair");
            *cleanup_completed(startup) |= UINT32_C(1) << startup->index;
        }
        return 0;
    }
    if (startup->closing || startup->invalid)
        goto terminal;
    if (startup->phase == LCS_CONFIG_STARTUP_DONE)
        return 1;
    if (startup->phase == LCS_CONFIG_STARTUP_BLOCKED || startup->phase == LCS_CONFIG_STARTUP_RECOVERY ||
        startup->phase == LCS_CONFIG_STARTUP_STOPPED)
        return -1;
    const lcs_config_t *cfg = cleanup_config(startup);
    uint32_t completed = *cleanup_completed(startup);
    if (startup->phase == LCS_CONFIG_STARTUP_NEXT)
    {
        if (completed == all_resources(cfg->resource_count))
        {
            if (startup->snapshot < LCS_CONFIG_APPLICATION_HISTORY && startup->history[startup->snapshot].document)
            { startup->snapshot++; return 0; }
            startup->phase = LCS_CONFIG_STARTUP_DONE; return 1;
        }
        for (size_t i = 0; i < cfg->resource_count; i++)
        {
            if (completed & (UINT32_C(1) << i)) continue;
            bool dependent = false;
            for (size_t j = 0; j < cfg->resource_count; j++)
                if (!(completed & (UINT32_C(1) << j)))
                    for (size_t d = 0; d < cfg->resources[j].depends_on_count; d++)
                        if (cfg->resources[j].depends_on_idx[d] == (int)i) dependent = true;
            if (dependent) continue;
            startup->index = i; startup->pre_failed = false; startup->phase = LCS_CONFIG_STARTUP_PRE_STOP;
            return 0;
        }
        startup->invalid = true;
        goto terminal;
    }
    const lcs_resource_config_t *resource = &cfg->resources[startup->index];
    uint64_t timeout = cfg->hook_timeout_ms;
    if (startup->phase == LCS_CONFIG_STARTUP_BACKEND)
    {
        startup->service_worker = resource->type == LCS_RESOURCE_SERVICE;
        int result = startup->service_worker ? lcs_systemd_service_stop_async(resource, &startup->pid) :
            resource->type == LCS_RESOURCE_VIP ? lcs_vip_operation_async(cfg, resource, LCS_VIP_WORKER_DEL, &startup->pid) : -1;
        if (result)
            return fail(startup, "cannot start startup backend cleanup worker");
        if (timeout < 5000u) timeout = 5000u;
    }
    else
    {
        bool pre = startup->phase == LCS_CONFIG_STARTUP_PRE_STOP;
        const char *path = pre ? resource->pre_stop : resource->post_stop;
        if (!*path)
        {
            if (pre) startup->phase = LCS_CONFIG_STARTUP_BACKEND;
            else if (startup->pre_failed) return fail(startup, "startup pre-stop hook failed; backend removed but cleanup requires repair");
            else { *cleanup_completed(startup) |= UINT32_C(1) << startup->index; startup->phase = LCS_CONFIG_STARTUP_NEXT; }
            return 0;
        }
        if (start_hook(startup, path, pre ? "pre-stop" : "post-stop"))
        {
            if (!pre) return fail(startup, "cannot start startup post-stop hook");
            startup->pre_failed = true; startup->phase = LCS_CONFIG_STARTUP_BACKEND;
            return 0;
        }
    }
    startup->deadline_ms = now_ms > UINT64_MAX - timeout ? UINT64_MAX : now_ms + timeout;
    return 0;
terminal:
    startup->phase = startup->closing ? LCS_CONFIG_STARTUP_STOPPED : LCS_CONFIG_STARTUP_RECOVERY;
    snprintf(startup->error, sizeof(startup->error), "startup cleanup cancelled or catalog changed; recover before admission");
    return -1;
}

int lcs_config_startup_retry(lcs_config_startup_t *startup)
{
    if (!startup || startup->pid > 0 || startup->phase != LCS_CONFIG_STARTUP_BLOCKED || startup->closing || !bound(startup))
        return -1;
    startup->phase = LCS_CONFIG_STARTUP_PRE_STOP;
    startup->pre_failed = false; startup->error[0] = 0;
    return 0;
}

bool lcs_config_startup_verified(const lcs_config_startup_t *startup,
                                   const lcs_config_runtime_t *runtime, uint64_t instance)
{
    if (!startup || !startup->config || startup->runtime != runtime || startup->instance != instance || startup->closing ||
        startup->invalid || startup->pid > 0 || startup->phase != LCS_CONFIG_STARTUP_DONE || !bound(startup))
        return false;
    if (startup->quorum_only) return true;
    if (startup->completed != all_resources(startup->config->resource_count)) return false;
    for (size_t i = 0; i < LCS_CONFIG_APPLICATION_HISTORY; i++)
        if (startup->history[i].document && startup->history_completed[i] != all_resources(startup->history_configs[i].resource_count))
            return false;
    return true;
}

static int completed_scope(lcs_config_startup_t *startup, size_t offset, lcs_config_owner_scope_t *scope)
{
    lcs_config_application_t pair = startup->runtime->application;
    pair.applied = startup->history[offset];
    pair.pending = offset ? startup->history[offset - 1] : startup->applied;
    return lcs_config_owner_scope(&pair, scope, startup->error, sizeof(startup->error));
}

int lcs_config_startup_restore_completed(lcs_config_startup_t *startup,
                                         lcs_config_owner_network_t *network)
{
    if (!startup || !network || !lcs_config_startup_verified(startup, startup->runtime, network->instance) || startup->quorum_only ||
        network->application != &startup->runtime->application || network->base != startup->runtime->base ||
        network->self != (size_t)lcs_config_self_index(startup->config) || network->closing || network->receipt.sealed ||
        network->receipt.failed || network->round.active || network->release.active || network->retired || !startup->history[0].document)
        return -1;
    lcs_config_owner_scope_t scope, older;
    if (completed_scope(startup, 0, &scope) || (startup->history[1].document && completed_scope(startup, 1, &older)))
        return -1;
    lcs_config_owner_receipt_t receipt = {0};
    if (lcs_config_owner_receipt_seal(&receipt, &scope, network->instance)) return -1;
    network->receipt = receipt;
    network->applied = true;
    if (startup->history[1].document)
    { network->retired_scope = older; network->retired = true; }
    network->pending_replies = 0;
    return 0;
}

int lcs_config_startup_restore_pending(lcs_config_startup_t *startup,
                                       lcs_config_owner_network_t *network, uint64_t now_ms)
{
    if (!startup || !network || !lcs_config_startup_verified(startup, startup->runtime, network->instance) ||
        network->application != &startup->runtime->application || network->base != startup->runtime->base || startup->quorum_only ||
        network->self != (size_t)lcs_config_self_index(startup->config) ||
        !startup->pending.document)
        return -1;
    if (startup->history[0].document)
    {
        lcs_config_owner_scope_t latest;
        if (completed_scope(startup, 0, &latest)) return -1;
        if (!network->retired || !lcs_config_owner_scope_equal(&network->retired_scope, &latest)) return 1;
    }
    else if (startup->applied.revision) return -1; /* Version-1 history is unknown. */
    lcs_config_owner_scope_t scope;
    if (lcs_config_owner_scope(network->application, &scope, startup->error, sizeof(startup->error)))
        return -1;
    return lcs_config_owner_network_start(network, &scope, now_ms);
}

static const lcs_config_value_t *known_value(const lcs_config_startup_t *startup,
                                               uint64_t revision, const char *digest)
{
    const lcs_config_value_t *values[] = {&startup->applied, &startup->pending,
        &startup->history[0], &startup->history[1]};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
        if (values[i]->document && values[i]->revision == revision && !strcmp(values[i]->digest, digest))
            return values[i];
    return NULL;
}

static int answer_history(void *context, const lcs_config_owner_message_t *query)
{
    lcs_config_startup_t *startup = context;
    if (!startup || !lcs_config_startup_verified(startup, startup->runtime,
        query ? query->target_instance : startup->instance) || startup->quorum_only) return -1;
    if (!query) return 0;
    const lcs_config_owner_scope_t *scope = &query->scope;
    const lcs_config_value_t *old = known_value(startup, scope->old_revision, scope->old_value);
    const lcs_config_value_t *target = known_value(startup, scope->target_revision, scope->target_value);
    /* Pending was never installed by this recovery instance and is not an old
     * backend snapshot. A newer target is not evidence of an older application. */
    if (!old || !target || old == &startup->pending ||
        (target != &startup->applied && target != &startup->pending) ||
        (query->kind == LCS_CONFIG_OWNER_APPLIED_QUERY && target != &startup->applied)) return 0;
    lcs_config_application_t pair = startup->runtime->application;
    pair.applied = *old; pair.pending = *target;
    lcs_config_owner_scope_t exact;
    char error[256];
    if (lcs_config_owner_scope(&pair, &exact, error, sizeof(error))) return 0;
    return lcs_config_owner_scope_equal(scope, &exact) ? 1 : 0;
}

int lcs_config_startup_bind_owner_history(lcs_config_startup_t *startup,
                                           lcs_config_owner_network_t *network)
{
    if (!startup || !network || !lcs_config_startup_verified(startup, startup->runtime, network->instance) ||
        startup->quorum_only || startup->owner_bound || (startup->applied.revision && !startup->history[0].document) ||
        network->closing || network->receipt.failed || network->recovery ||
        network->base != startup->runtime->base || network->application != &startup->runtime->application ||
        network->self != (size_t)lcs_config_self_index(startup->config)) return -1;
    network->recovery = answer_history; network->recovery_context = startup;
    startup->owner_bound = true;
    return 0;
}

static bool retained_scope(lcs_config_startup_t *startup, const lcs_config_owner_network_t *next,
                               const lcs_config_owner_scope_t *scope, bool applied)
{
    if ((next->retired && lcs_config_owner_scope_equal(scope, &next->retired_scope)) ||
        (next->receipt.sealed && (!applied || next->applied) && lcs_config_owner_scope_equal(scope, &next->receipt.scope)))
        return true;
    lcs_config_owner_message_t query = {.kind = applied ? LCS_CONFIG_OWNER_APPLIED_QUERY : LCS_CONFIG_OWNER_QUERY,
        .target_instance = next->instance, .scope = *scope};
    return answer_history(startup, &query) == 1;
}

int lcs_config_startup_refresh_owner_history(lcs_config_startup_t *startup,
                                              lcs_config_owner_network_t *network)
{
    if (!startup || !network || !lcs_config_startup_verified(startup, startup->runtime, network->instance) ||
        startup->quorum_only || startup->owner_bound || network->closing || network->serial == UINT64_MAX ||
        network->base != startup->runtime->base || network->application != &startup->runtime->application ||
        network->self != (size_t)lcs_config_self_index(startup->config)) return -1;
    if (network->recovery && (network->recovery != answer_history || !network->recovery_context ||
        network->recovery_context == startup || ((lcs_config_startup_t *)network->recovery_context)->pid > 0)) return -1;
    lcs_config_owner_network_t next;
    if (lcs_config_owner_network_init(&next, network->base, network->application, network->self, network->instance))
        return -1;
    next.serial = network->serial;
    memcpy(next.instances, network->instances, sizeof(next.instances));
    if ((startup->applied.revision && lcs_config_startup_restore_completed(startup, &next)) ||
        (network->receipt.sealed && !retained_scope(startup, &next, &network->receipt.scope, network->applied)) ||
        (network->retired && !retained_scope(startup, &next, &network->retired_scope, true)) ||
        lcs_config_startup_bind_owner_history(startup, &next)) return -1;
    *network = next;
    return 0;
}

void lcs_config_startup_stop(lcs_config_startup_t *startup)
{
    if (!startup || !startup->config) return;
    startup->closing = true;
    cancel_worker(startup);
    if (startup->pid <= 0) startup->phase = LCS_CONFIG_STARTUP_STOPPED;
}

int lcs_config_startup_close(lcs_config_startup_t *startup)
{
    if (!startup || startup->pid > 0 || (startup->config && startup->phase != LCS_CONFIG_STARTUP_DONE &&
        startup->phase != LCS_CONFIG_STARTUP_STOPPED && startup->phase != LCS_CONFIG_STARTUP_BLOCKED &&
        startup->phase != LCS_CONFIG_STARTUP_RECOVERY))
        return -1;
    clear_snapshots(startup);
    memset(startup, 0, sizeof(*startup));
    return 0;
}
