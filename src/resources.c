// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "resources.h"

#include "cluster.h"
#include "group.h"
#include "lease.h"
#include "log.h"
#include "move.h"
#include "peer.h"
#include "systemd_service.h"
#include "util.h"
#include "vip.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void resources_release_local_internal(int resource_idx, int epoll_fd, bool allow_hooks);

static uint64_t resources_vip_probe_timeout_ms(void)
{
    uint64_t count = g_state.cfg.probe_count ? g_state.cfg.probe_count : 1u;
    uint64_t per_probe = g_state.cfg.probe_timeout_ms ?
                         g_state.cfg.probe_timeout_ms : 300u;
    uint64_t timeout = count * per_probe + 1000u;
    return timeout < 5000u ? 5000u : timeout;
}

static void resources_clear_vip_probe(resource_runtime_t *res)
{
    memset(&res->vip_probe, 0, sizeof(res->vip_probe));
}

static void resources_cancel_vip_probe(resource_runtime_t *res)
{
    if (res->vip_probe.pid <= 0)
        return;
    if (!res->vip_probe.kill_sent)
    {
        lcs_vip_probe_cancel(res->vip_probe.pid);
        lcs_log_debug("requested asynchronous VIP probe cancellation pid=%ld", (long)res->vip_probe.pid);
    }
    res->vip_probe.deadline_ms = 0;
    res->vip_probe.kill_sent = true;
    res->vip_probe.discard_result = true;
}

static int resources_start_vip_probe(int resource_idx, uint64_t epoch, uint64_t lease_id, int epoll_fd)
{
    resource_runtime_t *res = &g_state.resources[resource_idx];
    if (res->vip_probe.pid > 0)
        return -1;
    pid_t pid = -1;
    if (lcs_vip_conflict_check_async(&g_state.cfg, &g_state.cfg.resources[resource_idx], &pid) != 0)
        return -1;
    res->vip_probe.pid = pid;
    res->vip_probe.deadline_ms = lcs_now_ms() + resources_vip_probe_timeout_ms();
    res->vip_probe.epoch = epoch;
    res->vip_probe.lease_id = lease_id;
    res->vip_probe.kill_sent = false;
    res->vip_probe.discard_result = false;
    res->state = LCS_RES_STARTING;
    lcs_log_debug("started asynchronous VIP conflict probe resource=%s pid=%ld", g_state.cfg.resources[resource_idx].name, (long)pid);
    peer_broadcast_state_sync(epoll_fd);
    return 0;
}

uint32_t resources_service_operation_timeout_ms(void)
{
    uint32_t timeout = g_state.cfg.hook_timeout_ms;
    return timeout < 5000u ? 5000u : timeout;
}

static void resources_clear_service_operation(resource_runtime_t *res)
{
    res->service.pid = 0;
    res->service.op = LCS_SERVICE_OP_NONE;
    res->service.next_op = LCS_SERVICE_OP_NONE;
    res->service.deadline_ms = 0;
    res->service.kill_sent = false;
    res->service.epoch = 0;
    res->service.lease_id = 0;
    res->service.stop_post_hook = false;
    res->service.handoff = false;
    res->service.handoff_source_node = -1;
    res->service.handoff_response_seq = 0;
}

static void resources_cancel_service_operation(resource_runtime_t *res, resource_service_op_type_t next_op)
{
    if (res->service.pid <= 0)
    {
        resources_clear_service_operation(res);
        return;
    }
    if (!res->service.kill_sent)
    {
        lcs_systemd_service_cancel(res->service.pid);
        lcs_log_debug("requested asynchronous systemd worker cancellation pid=%ld", (long)res->service.pid);
    }
    res->service.op = LCS_SERVICE_OP_CANCELLING;
    res->service.next_op = next_op;
    res->service.deadline_ms = 0;
    res->service.kill_sent = true;
}

static int resources_start_service_operation(int resource_idx, resource_service_op_type_t type)
{
    resource_runtime_t *res = &g_state.resources[resource_idx];
    const lcs_resource_config_t *resource = &g_state.cfg.resources[resource_idx];
    if (res->service.pid > 0 || res->service.op != LCS_SERVICE_OP_NONE)
        return -1;

    pid_t pid = -1;
    int rc;
    if (type == LCS_SERVICE_OP_START)
        rc = lcs_systemd_service_start_async(resource, &pid);
    else if (type == LCS_SERVICE_OP_HEALTH)
        rc = lcs_systemd_service_check_async(resource, &pid);
    else
        rc = lcs_systemd_service_stop_async(resource, &pid);
    if (rc != 0)
        return -1;

    res->service.pid = pid;
    res->service.op = type;
    res->service.next_op = LCS_SERVICE_OP_NONE;
    res->service.deadline_ms = lcs_now_ms() + resources_service_operation_timeout_ms();
    res->service.kill_sent = false;
    lcs_log_debug("started asynchronous systemd operation resource=%s op=%u pid=%ld", resource->name, (unsigned)type, (long)pid);
    return 0;
}

static const char *resources_hook_name(resource_hook_type_t type)
{
    switch (type)
    {
        case LCS_HOOK_PRE_START:
            return "pre-start";
        case LCS_HOOK_POST_START:
            return "post-start";
        case LCS_HOOK_PRE_STOP:
            return "pre-stop";
        case LCS_HOOK_POST_STOP:
            return "post-stop";
        default:
            return "none";
    }
}

static const char *resources_hook_path(const lcs_resource_config_t *resource, resource_hook_type_t type)
{
    switch (type)
    {
        case LCS_HOOK_PRE_START:
            return resource->pre_start;
        case LCS_HOOK_POST_START:
            return resource->post_start;
        case LCS_HOOK_PRE_STOP:
            return resource->pre_stop;
        case LCS_HOOK_POST_STOP:
            return resource->post_stop;
        default:
            return "";
    }
}

static const char *resource_kind(const lcs_resource_config_t *res)
{
    switch (res->type)
    {
        case LCS_RESOURCE_VIP:
            return "VIP";
        case LCS_RESOURCE_SERVICE:
            return "service";
        default:
            return "unknown resource";
    }
}

static int resource_start_local(const lcs_resource_config_t *res)
{
    switch (res->type)
    {
        case LCS_RESOURCE_VIP:
            return lcs_vip_add(res);
        case LCS_RESOURCE_SERVICE:
            lcs_log_warn("synchronous service start requested unexpectedly for %s", res->name);
            return -1;
        default:
            lcs_log_warn("cannot start resource %s: unknown resource type %u", res->name, (unsigned)res->type);
            return -1;
    }
}

int resources_stop_local_backend(const lcs_resource_config_t *res)
{
    switch (res->type)
    {
        case LCS_RESOURCE_VIP:
            return lcs_vip_del(res);
        case LCS_RESOURCE_SERVICE:
            lcs_log_warn("synchronous service stop requested unexpectedly for %s", res->name);
            return -1;
        default:
            lcs_log_warn("cannot stop resource %s: unknown resource type %u", res->name, (unsigned)res->type);
            return -1;
    }
}

static int resource_is_local_active(const lcs_resource_config_t *res)
{
    switch (res->type)
    {
        case LCS_RESOURCE_VIP:
            return 1;
        case LCS_RESOURCE_SERVICE:
            return -1;
        default:
            lcs_log_warn("cannot inspect resource %s: unknown resource type %u", res->name, (unsigned)res->type);
            return -1;
    }
}

static void resource_announce(const lcs_resource_config_t *res)
{
    switch (res->type)
    {
        case LCS_RESOURCE_VIP:
            if (lcs_vip_announce(&g_state.cfg, res) != 0)
            {
                lcs_log_warn("failed to send VIP announcement for %s on %s", res->address, res->interface);
            }
            return;
        case LCS_RESOURCE_SERVICE:
            return;
        default:
            lcs_log_warn("cannot announce resource %s: unknown resource type %u", res->name, (unsigned)res->type);
            return;
    }
}

static void resources_clear_hook(resource_runtime_t *res)
{
    memset(&res->hook, 0, sizeof(res->hook));
}

static void resources_cancel_hook(int resource_idx)
{
    resource_runtime_t *res = &g_state.resources[resource_idx];
    if (res->hook.pid <= 0)
        return;

    lcs_log_warn("cancelling %s hook for resource %s pid=%ld", resources_hook_name(res->hook.type), g_state.cfg.resources[resource_idx].name, (long)res->hook.pid);
    kill(res->hook.pid, SIGKILL);
    waitpid(res->hook.pid, NULL, 0);
    resources_clear_hook(res);
}

static int resources_start_hook(int resource_idx, resource_hook_type_t type, uint64_t epoch, uint64_t lease_id)
{
    resource_runtime_t *res = &g_state.resources[resource_idx];
    const lcs_resource_config_t *resource = &g_state.cfg.resources[resource_idx];
    const char *path = resources_hook_path(resource, type);
    if (!*path)
        return 1;

    if (res->hook.pid > 0)
    {
        lcs_log_warn("cannot start %s hook for resource %s: %s hook pid=%ld still running",
                     resources_hook_name(type), resource->name, resources_hook_name(res->hook.type),
                     (long)res->hook.pid);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        lcs_log_warn("failed to fork %s hook for resource %s: %s", resources_hook_name(type), resource->name, strerror(errno));
        return -1;
    }
    if (pid == 0)
    {
        char epoch_buf[32];
        char lease_buf[32];
        char timeout_buf[32];
        snprintf(epoch_buf, sizeof(epoch_buf), "%llu", (unsigned long long)epoch);
        snprintf(lease_buf, sizeof(lease_buf), "%llu", (unsigned long long)lease_id);
        snprintf(timeout_buf, sizeof(timeout_buf), "%u", g_state.cfg.hook_timeout_ms);
        setenv("LCS_CLUSTER", g_state.cfg.cluster_name, 1);
        setenv("LCS_NODE", g_state.cfg.nodes[g_state.self_index].name, 1);
        setenv("LCS_RESOURCE", resource->name, 1);
        setenv("LCS_RESOURCE_TYPE", lcs_resource_type_name(resource->type), 1);
        setenv("LCS_SYSTEMD_UNIT", resource->systemd_unit, 1);
        setenv("LCS_VIP", resource->name, 1);
        setenv("LCS_ADDRESS", resource->address, 1);
        setenv("LCS_INTERFACE", resource->interface, 1);
        setenv("LCS_EVENT", resources_hook_name(type), 1);
        setenv("LCS_EPOCH", epoch_buf, 1);
        setenv("LCS_LEASE_ID", lease_buf, 1);
        setenv("LCS_HOOK_TIMEOUT_MS", timeout_buf, 1);
        execl(path, path, (char *)NULL);
        _exit(127);
    }

    res->hook.pid = pid;
    res->hook.type = type;
    res->hook.deadline_ms = lcs_now_ms() + g_state.cfg.hook_timeout_ms;
    res->hook.epoch = epoch;
    res->hook.lease_id = lease_id;
    lcs_log_info("started %s hook for resource %s pid=%ld path=%s timeout_ms=%u", resources_hook_name(type), resource->name, (long)pid, path, g_state.cfg.hook_timeout_ms);
    return 0;
}

static void resources_clear_local_lease(resource_runtime_t *res, uint64_t epoch)
{
    res->epoch = epoch;
    res->owner_node = -1;
    res->owner_instance_id = 0;
    res->state = LCS_RES_STOPPED;
    res->lease_id = 0;
    res->lease_deadline_ms = 0;
    res->renew_after_ms = 0;
    res->conflict_reason[0] = '\0';
}

static bool resource_locally_owned_active(int resource_idx)
{
    if (resource_idx < 0 || (size_t)resource_idx >= g_state.cfg.resource_count)
        return false;

    const resource_runtime_t *res = &g_state.resources[resource_idx];
    return res->owner_node == g_state.self_index &&
           res->owner_instance_id == g_state.instance_id &&
           res->state == LCS_RES_ACTIVE;
}

static bool resource_locally_owned_running(int resource_idx)
{
    if (resource_idx < 0 || (size_t)resource_idx >= g_state.cfg.resource_count)
        return false;

    const resource_runtime_t *res = &g_state.resources[resource_idx];
    return res->owner_node == g_state.self_index &&
           res->owner_instance_id == g_state.instance_id &&
           (res->state == LCS_RES_ACTIVE ||
            res->state == LCS_RES_STARTING ||
            res->state == LCS_RES_STOPPING);
}

static bool resource_dependencies_active_locally(int resource_idx)
{
    const lcs_resource_config_t *resource = &g_state.cfg.resources[resource_idx];
    for (size_t i = 0; i < resource->depends_on_count; i++)
    {
        int dep_idx = resource->depends_on_idx[i];
        if (!resource_locally_owned_active(dep_idx))
            return false;
    }
    return true;
}

static bool resources_release_local_dependents(int resource_idx, int epoll_fd, bool allow_hooks)
{
    bool pending = false;
    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        if ((int)i == resource_idx)
            continue;

        const lcs_resource_config_t *candidate = &g_state.cfg.resources[i];
        bool depends = false;
        for (size_t d = 0; d < candidate->depends_on_count; d++)
        {
            if (candidate->depends_on_idx[d] == resource_idx)
            {
                depends = true;
                break;
            }
        }
        if (!depends || !resource_locally_owned_running((int)i))
            continue;

        lcs_log_info("releasing dependent resource %s before %s", candidate->name, g_state.cfg.resources[resource_idx].name);
        resources_release_local_internal((int)i, epoll_fd, allow_hooks);
        if (resource_locally_owned_running((int)i))
            pending = true;
    }
    return pending;
}

static void resources_mark_local_active(int resource_idx, uint64_t epoch, uint64_t lease_id, int epoll_fd)
{
    resource_runtime_t *res = &g_state.resources[resource_idx];
    const lcs_resource_config_t *resource = &g_state.cfg.resources[resource_idx];
    res->state = LCS_RES_ACTIVE;
    res->next_activation_attempt_ms = 0;
    res->service.next_health_ms = lcs_now_ms() + 1000u;
    resource_announce(resource);
    if (res->failover_pending)
    {
        res->failover_count++;
        res->failover_pending = false;
        lcs_log_info("counted failover for resource %s total=%llu", resource->name, (unsigned long long)res->failover_count);
    }
    lcs_log_info("activated %s %s on %s epoch=%llu", resource_kind(resource), resource->name, g_state.cfg.nodes[g_state.self_index].name, (unsigned long long)epoch);
    peer_broadcast_state_sync(epoll_fd);
    resources_start_hook(resource_idx, LCS_HOOK_POST_START, epoch, lease_id);
}

static int resources_complete_local_activation(int resource_idx, uint64_t epoch, uint64_t lease_id, int epoll_fd)
{
    resource_runtime_t *res = &g_state.resources[resource_idx];
    uint64_t now = lcs_now_ms();
    const lcs_resource_config_t *resource = &g_state.cfg.resources[resource_idx];
    if (resource->type == LCS_RESOURCE_SERVICE)
    {
        res->state = LCS_RES_STARTING;
        res->service.epoch = epoch;
        res->service.lease_id = lease_id;
        if (resources_start_service_operation(resource_idx, LCS_SERVICE_OP_START) == 0)
        {
            peer_broadcast_state_sync(epoll_fd);
            return 0;
        }
        lcs_log_warn("auto-place failed service %s: failed to start asynchronous systemd operation", resource->name);
        lease_release_majority(resource_idx, g_state.self_index, epoch, lease_id, epoll_fd);
        resources_clear_local_lease(res, epoch);
        res->next_activation_attempt_ms = now + lcs_jittered_delay_ms(g_state.cfg.lease_ms);
        return -1;
    }
    if (resource->type == LCS_RESOURCE_VIP)
    {
        if (resources_start_vip_probe(resource_idx, epoch, lease_id, epoll_fd) == 0)
            return 0;
        lcs_log_warn("auto-place failed VIP %s: failed to start asynchronous conflict probe", resource->name);
        lease_release_majority(resource_idx, g_state.self_index, epoch, lease_id, epoll_fd);
        resources_clear_local_lease(res, epoch);
        res->next_activation_attempt_ms = now + lcs_jittered_delay_ms(g_state.cfg.lease_ms);
        return -1;
    }
    lcs_log_warn("auto-place failed resource %s: unknown resource type %u", resource->name, (unsigned)resource->type);
    return -1;
}

static int resources_begin_service_stop(int resource_idx, resource_service_op_type_t type, uint64_t epoch, uint64_t lease_id, bool post_hook)
{
    resource_runtime_t *res = &g_state.resources[resource_idx];
    res->service.epoch = epoch;
    res->service.lease_id = lease_id;
    res->service.stop_post_hook = post_hook;
    res->state = LCS_RES_STOPPING;
    if (res->service.pid > 0)
    {
        resources_cancel_service_operation(res, type);
        return 0;
    }
    if (resources_start_service_operation(resource_idx, type) != 0)
        return -1;
    return 0;
}

static void resources_release_local_internal(int resource_idx, int epoll_fd, bool allow_hooks)
{
    resource_runtime_t *res = &g_state.resources[resource_idx];
    uint64_t old_epoch = res->epoch;
    uint64_t release_epoch = old_epoch + 1;
    uint64_t old_lease_id = res->lease_id;
    bool locally_owned = res->owner_node == g_state.self_index && res->owner_instance_id == g_state.instance_id;

    resources_cancel_vip_probe(res);

    if (!locally_owned)
    {
        resources_clear_local_lease(res, release_epoch);
        return;
    }

    if (resources_release_local_dependents(resource_idx, epoll_fd, allow_hooks))
        return;

    if (!allow_hooks)
        resources_cancel_hook(resource_idx);
    else if (res->hook.pid > 0 && res->hook.type != LCS_HOOK_PRE_STOP)
        resources_cancel_hook(resource_idx);
    if (allow_hooks && res->state == LCS_RES_ACTIVE && *g_state.cfg.resources[resource_idx].pre_stop)
    {
        res->state = LCS_RES_STOPPING;
        if (resources_start_hook(resource_idx, LCS_HOOK_PRE_STOP, res->epoch, res->lease_id) == 0)
            return;
        lcs_log_warn("continuing resource %s stop without pre-stop hook",  g_state.cfg.resources[resource_idx].name);
    }

    if (g_state.cfg.resources[resource_idx].type == LCS_RESOURCE_SERVICE &&
        (res->state == LCS_RES_ACTIVE ||
         res->state == LCS_RES_STARTING ||
         res->state == LCS_RES_STOPPING ||
         res->state == LCS_RES_STOP_FAILED))
    {
        if (res->service.op == LCS_SERVICE_OP_STOP || res->service.op == LCS_SERVICE_OP_ROLLBACK_STOP)
            return;
        if (resources_begin_service_stop(resource_idx, LCS_SERVICE_OP_STOP, old_epoch, old_lease_id, allow_hooks) != 0)
            resources_enter_stop_failed_state(resource_idx, release_epoch, "failed to start asynchronous systemd stop; service may still be running", epoll_fd);
        return;
    }

    if (res->state == LCS_RES_ACTIVE || res->state == LCS_RES_STOPPING || res->state == LCS_RES_STOP_FAILED)
    {
        if (resources_stop_local_backend(&g_state.cfg.resources[resource_idx]) != 0)
        {
            resources_enter_stop_failed_state(resource_idx, release_epoch, "local resource stop failed; node may still be running resource", epoll_fd);
            return;
        }
    }

    lease_release_majority(resource_idx, g_state.self_index, old_epoch, old_lease_id, epoll_fd);
    resources_clear_local_lease(res, release_epoch);
    if (allow_hooks)
        resources_start_hook(resource_idx, LCS_HOOK_POST_STOP, release_epoch, old_lease_id);
}

static void resources_clear_volatile_state_after_quorum_loss(int epoll_fd)
{
    lcs_log_warn("quorum lost; dropping local resources and clearing volatile cluster state");

    move_cancel_all(epoll_fd, "majority quorum is not available");

    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        uint64_t epoch = g_state.resources[i].epoch;
        if (g_state.lease_grants[i].promised_epoch > epoch)
            epoch = g_state.lease_grants[i].promised_epoch;
        uint64_t failover_count = g_state.resources[i].failover_count;
        uint64_t home_generation = g_state.resources[i].home_generation;
        uint64_t disabled_generation = g_state.resources[i].disabled_generation;
        bool home_blocked = g_state.resources[i].home_blocked;
        bool disabled = g_state.resources[i].disabled;
        if (g_state.resources[i].owner_node == g_state.self_index &&
            g_state.resources[i].owner_instance_id == g_state.instance_id)
        {
            resources_release_local_internal((int)i, epoll_fd, false);
            if (g_state.resources[i].owner_node == g_state.self_index &&
                g_state.resources[i].owner_instance_id == g_state.instance_id)
            {
                g_state.resources[i].failover_count = failover_count;
                g_state.resources[i].home_generation = home_generation;
                g_state.resources[i].disabled_generation = disabled_generation;
                g_state.resources[i].home_blocked = home_blocked;
                g_state.resources[i].disabled = disabled;
                continue;
            }
        }
        resources_cancel_hook((int)i);
        resources_cancel_vip_probe(&g_state.resources[i]);
        resource_vip_probe_runtime_t vip_probe =
            g_state.resources[i].vip_probe;
        memset(&g_state.resources[i], 0, sizeof(g_state.resources[i]));
        g_state.resources[i].vip_probe = vip_probe;
        g_state.resources[i].owner_node = -1;
        g_state.resources[i].state = LCS_RES_STOPPED;
        g_state.resources[i].epoch = epoch;
        g_state.resources[i].failover_count = failover_count;
        g_state.resources[i].home_generation = home_generation;
        g_state.resources[i].disabled_generation = disabled_generation;
        g_state.resources[i].home_blocked = home_blocked;
        g_state.resources[i].disabled = disabled;
    }

    lease_cancel_all_operations();
    g_state.no_quorum_state_cleared = true;
}

static uint64_t resources_next_epoch(uint64_t epoch)
{
    return epoch == UINT64_MAX ? UINT64_MAX : epoch + 1;
}

static int resources_finish_startup_cleanup(int resource_idx, bool success)
{
    resource_runtime_t *res = &g_state.resources[resource_idx];
    const lcs_resource_config_t *resource = &g_state.cfg.resources[resource_idx];
    if (success)
    {
        if (res->startup_cleanup_failed)
        {
            res->epoch = resources_next_epoch(res->epoch);
            if (res->epoch > g_state.lease_grants[resource_idx].promised_epoch)
                g_state.lease_grants[resource_idx].promised_epoch = res->epoch;
            res->owner_node = -1;
            res->owner_instance_id = 0;
            res->state = LCS_RES_STOPPED;
            res->lease_id = 0;
            res->lease_deadline_ms = 0;
            res->renew_after_ms = 0;
            res->conflict_reason[0] = '\0';
            res->startup_cleanup_failed = false;
            res->startup_cleanup_broadcast_pending = true;
            lcs_log_info("startup cleanup recovered for %s %s; resource is confirmed inactive",
                         resource_kind(resource), resource->name);
        } else
        {
            lcs_log_info("startup cleanup verified %s %s inactive",
                         resource_kind(resource), resource->name);
        }
        res->next_startup_cleanup_attempt_ms = 0;
        return 0;
    }

    if (!res->startup_cleanup_failed)
    {
        res->epoch = resources_next_epoch(res->epoch);
        res->owner_node = g_state.self_index;
        res->owner_instance_id = g_state.instance_id;
        res->state = LCS_RES_STOP_FAILED;
        res->lease_id = 0;
        res->lease_deadline_ms = 0;
        res->renew_after_ms = 0;
        res->startup_cleanup_failed = true;
        res->startup_cleanup_broadcast_pending = true;
        snprintf(res->conflict_reason, sizeof(res->conflict_reason),
                 "startup cleanup failed; local resource may still be active");
        lcs_log_warn("startup cleanup failed for %s %s; node remains recovering and resource is blocked cluster-wide until cleanup succeeds or the node is fenced",
                     resource_kind(resource), resource->name);
    }
    res->next_startup_cleanup_attempt_ms = lcs_now_ms() + 1000u;
    return -1;
}

static int resources_try_startup_cleanup(int resource_idx)
{
    resource_runtime_t *res = &g_state.resources[resource_idx];
    const lcs_resource_config_t *resource = &g_state.cfg.resources[resource_idx];
    if (resource->type == LCS_RESOURCE_SERVICE)
    {
        if (res->service.op == LCS_SERVICE_OP_STARTUP_CLEANUP)
            return -1;
        if (res->service.op != LCS_SERVICE_OP_NONE ||
            resources_start_service_operation(resource_idx,
                                               LCS_SERVICE_OP_STARTUP_CLEANUP) != 0)
            return resources_finish_startup_cleanup(resource_idx, false);
        res->next_startup_cleanup_attempt_ms = 0;
        return -1;
    }
    return resources_finish_startup_cleanup(
        resource_idx, resources_stop_local_backend(resource) == 0);
}

void resources_begin_startup_cleanup(void)
{
    if (g_state.cfg.nodes[g_state.self_index].role != LCS_NODE_FULL)
    {
        lcs_log_debug("skipping local resource cleanup on quorum-only node");
        return;
    }
    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
        (void)resources_try_startup_cleanup((int)i);
}

void resources_progress_startup_cleanup(int epoll_fd)
{
    if (g_state.cfg.nodes[g_state.self_index].role != LCS_NODE_FULL)
        return;

    uint64_t now = lcs_now_ms();
    bool broadcast = false;
    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        resource_runtime_t *res = &g_state.resources[i];
        if (res->startup_cleanup_failed &&
            (!res->next_startup_cleanup_attempt_ms ||
             now >= res->next_startup_cleanup_attempt_ms))
            (void)resources_try_startup_cleanup((int)i);
        if (res->startup_cleanup_broadcast_pending)
        {
            res->startup_cleanup_broadcast_pending = false;
            broadcast = true;
        }
    }
    if (broadcast)
        peer_broadcast_state_sync(epoll_fd);
}

bool resources_startup_cleanup_complete(void)
{
    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        if (g_state.resources[i].startup_cleanup_failed ||
            g_state.resources[i].service.op == LCS_SERVICE_OP_STARTUP_CLEANUP)
            return false;
    }
    return true;
}

bool resources_preserve_startup_cleanup_failure(int resource_idx, uint64_t incoming_epoch)
{
    resource_runtime_t *res = &g_state.resources[resource_idx];
    if (!res->startup_cleanup_failed)
        return false;

    if (incoming_epoch >= res->epoch)
    {
        res->epoch = resources_next_epoch(incoming_epoch);
        if (res->epoch > g_state.lease_grants[resource_idx].promised_epoch)
            g_state.lease_grants[resource_idx].promised_epoch = res->epoch;
        res->startup_cleanup_broadcast_pending = true;
    }
    res->owner_node = g_state.self_index;
    res->owner_instance_id = g_state.instance_id;
    res->state = LCS_RES_STOP_FAILED;
    res->lease_id = 0;
    res->lease_deadline_ms = 0;
    res->renew_after_ms = 0;
    snprintf(res->conflict_reason, sizeof(res->conflict_reason),
             "startup cleanup failed; local resource may still be active");
    return true;
}

void resources_enter_conflict_state(int resource_idx, uint64_t epoch, const char *reason)
{
    resource_runtime_t *res = &g_state.resources[resource_idx];
    lease_cancel_operations(resource_idx);
    resources_cancel_vip_probe(res);
    if (res->owner_node == g_state.self_index && res->owner_instance_id == g_state.instance_id && res->state == LCS_RES_ACTIVE)
        resources_stop_local_backend(&g_state.cfg.resources[resource_idx]);
    res->epoch = epoch > res->epoch ? epoch : res->epoch + 1;
    res->owner_node = -1;
    res->owner_instance_id = 0;
    res->state = LCS_RES_CONFLICT;
    res->lease_id = 0;
    res->lease_deadline_ms = 0;
    res->renew_after_ms = 0;
    res->next_activation_attempt_ms = 0;
    snprintf(res->conflict_reason, sizeof(res->conflict_reason), "%s", reason ? reason : "VIP conflict detected");
    lcs_log_warn("resource %s entered conflict state at epoch=%llu: %s; admin clear required", g_state.cfg.resources[resource_idx].name, (unsigned long long)res->epoch, res->conflict_reason);
}

void resources_enter_stop_failed_state(int resource_idx, uint64_t epoch, const char *reason, int epoll_fd)
{
    resource_runtime_t *res = &g_state.resources[resource_idx];
    lease_cancel_operations(resource_idx);
    resources_cancel_hook(resource_idx);
    resources_cancel_vip_probe(res);
    resources_cancel_service_operation(res, LCS_SERVICE_OP_NONE);
    res->epoch = epoch > res->epoch ? epoch : res->epoch + 1;
    res->state = LCS_RES_STOP_FAILED;
    res->lease_deadline_ms = 0;
    res->renew_after_ms = 0;
    res->next_activation_attempt_ms = 0;
    snprintf(res->conflict_reason, sizeof(res->conflict_reason), "%s", reason ? reason : "local resource stop failed; node may still be running resource");
    lcs_log_warn("resource %s entered stop_failed state at epoch=%llu: %s; retry stop or fence node",
                 g_state.cfg.resources[resource_idx].name, (unsigned long long)res->epoch,
                 res->conflict_reason);
    if (epoll_fd >= 0)
        peer_broadcast_state_sync(epoll_fd);
}

int resources_begin_state_replacement(int resource_idx, int owner_node, uint64_t owner_instance_id, lcs_resource_state_t state, uint64_t epoch, uint64_t lease_id, uint64_t deadline_ms, const char *reason, int epoll_fd)
{
    if (g_state.cfg.resources[resource_idx].type != LCS_RESOURCE_SERVICE)
    {
        resources_cancel_vip_probe(&g_state.resources[resource_idx]);
        return 0;
    }

    resource_runtime_t *res = &g_state.resources[resource_idx];
    lease_cancel_operations(resource_idx);
    resources_cancel_hook(resource_idx);
    res->service.replacement.owner_node = owner_node;
    res->service.replacement.owner_instance_id = owner_instance_id;
    res->service.replacement.state = state;
    res->service.replacement.epoch = epoch;
    res->service.replacement.lease_id = lease_id;
    res->service.replacement.deadline_ms = deadline_ms;
    snprintf(res->service.replacement.reason, sizeof(res->service.replacement.reason), "%s", reason ? reason : "");
    res->state = LCS_RES_STOPPING;
    if (res->service.pid > 0)
    {
        resources_cancel_service_operation(res, LCS_SERVICE_OP_STATE_REPLACE);
        return 1;
    }
    if (resources_start_service_operation(resource_idx, LCS_SERVICE_OP_STATE_REPLACE) != 0)
    {
        resources_enter_stop_failed_state(resource_idx, epoch + 1, "failed to start asynchronous systemd stop while replacing local ownership", epoll_fd);
        return -1;
    }
    return 1;
}

int resources_activate_acquired_local(int resource_idx, uint64_t epoch, uint64_t lease_id, int epoll_fd)
{
    resource_runtime_t *res = &g_state.resources[resource_idx];
    uint64_t now = lcs_now_ms();
    if (!resource_dependencies_active_locally(resource_idx))
    {
        lcs_log_warn("auto-place failed %s %s: dependencies are not active locally", resource_kind(&g_state.cfg.resources[resource_idx]), g_state.cfg.resources[resource_idx].name);
        lease_release_majority(resource_idx, g_state.self_index, epoch, lease_id, epoll_fd);
        resources_clear_local_lease(res, epoch);
        res->next_activation_attempt_ms = now + lcs_jittered_delay_ms(g_state.cfg.lease_ms);
        return -1;
    }
    if (*g_state.cfg.resources[resource_idx].pre_start)
    {
        res->state = LCS_RES_STARTING;
        if (resources_start_hook(resource_idx, LCS_HOOK_PRE_START, epoch, lease_id) == 0)
        {
            peer_broadcast_state_sync(epoll_fd);
            return 0;
        }
        lcs_log_warn("auto-place failed resource %s: failed to start pre-start hook", g_state.cfg.resources[resource_idx].name);
        lease_release_majority(resource_idx, g_state.self_index, epoch, lease_id, epoll_fd);
        resources_clear_local_lease(res, epoch);
        res->next_activation_attempt_ms = now + lcs_jittered_delay_ms(g_state.cfg.lease_ms);
        return -1;
    }
    return resources_complete_local_activation(resource_idx, epoch, lease_id, epoll_fd);
}

int resources_activate_local(int resource_idx, uint64_t epoch, int epoll_fd)
{
    if (g_state.cfg.nodes[g_state.self_index].role != LCS_NODE_FULL)
    {
        lcs_log_debug("refusing to activate resource %s on non-full-member node", g_state.cfg.resources[resource_idx].name);
        return -1;
    }
    resource_runtime_t *res = &g_state.resources[resource_idx];
    uint64_t now = lcs_now_ms();
    if (res->disabled)
    {
        lcs_log_debug("refusing to activate resource %s because it is administratively stopped", g_state.cfg.resources[resource_idx].name);
        return -1;
    }
    if (res->state == LCS_RES_CONFLICT || res->state == LCS_RES_STOP_FAILED)
    {
        lcs_log_warn("refusing to activate resource %s because it is in %s state", g_state.cfg.resources[resource_idx].name, lcs_resource_state_name(res->state));
        return -1;
    }
    if (res->next_activation_attempt_ms && now < res->next_activation_attempt_ms)
    {
        lcs_log_debug2("auto-place skip resource %s: activation retry backoff %llu ms remaining", g_state.cfg.resources[resource_idx].name, (unsigned long long)(res->next_activation_attempt_ms - now));
        return -1;
    }
    if (!resource_dependencies_active_locally(resource_idx))
    {
        lcs_log_debug2("auto-place skip resource %s: dependencies are not active locally", g_state.cfg.resources[resource_idx].name);
        return -1;
    }
    uint64_t lease_id = lcs_random_u64();
    if (lease_start_acquire(resource_idx, g_state.self_index, epoch, lease_id, epoll_fd) != 0)
    {
        lcs_log_debug2("auto-place failed resource %s: could not start majority lease acquire for epoch=%llu", g_state.cfg.resources[resource_idx].name, (unsigned long long)epoch);
        res->next_activation_attempt_ms = now + lcs_jittered_delay_ms(g_state.cfg.renew_ms);
        return -1;
    }
    return 0;
}

void resources_release_local(int resource_idx, int epoll_fd)
{
    resources_release_local_internal(resource_idx, epoll_fd, true);
}

int resources_release_for_handoff(int resource_idx, uint64_t epoch, uint64_t lease_id, int source_node_idx, uint32_t response_seq, int epoll_fd)
{
    resource_runtime_t *res = &g_state.resources[resource_idx];

    if (resources_release_local_dependents(resource_idx, epoll_fd, false))
        return -1;

    resources_cancel_hook(resource_idx);
    if (g_state.cfg.resources[resource_idx].type == LCS_RESOURCE_SERVICE)
    {
        if (resources_begin_service_stop(resource_idx, LCS_SERVICE_OP_STOP, epoch, lease_id, true) != 0)
        {
            resources_enter_stop_failed_state(resource_idx, epoch + 1, "failed to start asynchronous systemd stop during handoff", epoll_fd);
            return -1;
        }
        res->service.handoff = true;
        res->service.handoff_source_node = source_node_idx;
        res->service.handoff_response_seq = response_seq;
        return 1;
    }
    if (resources_stop_local_backend(&g_state.cfg.resources[resource_idx]) != 0)
    {
        resources_enter_stop_failed_state(resource_idx, epoch + 1, "local resource stop failed during handoff; node may still be running resource", epoll_fd);
        return -1;
    }

    res->owner_node = -1;
    res->owner_instance_id = 0;
    res->state = LCS_RES_STOPPED;
    res->lease_id = 0;
    res->lease_deadline_ms = 0;
    res->renew_after_ms = 0;
    res->conflict_reason[0] = '\0';
    res->next_activation_attempt_ms = lcs_now_ms() + lcs_jittered_delay_ms(g_state.cfg.lease_ms);
    lease_cancel_operations(resource_idx);
    resources_start_hook(resource_idx, LCS_HOOK_POST_STOP, epoch, lease_id);
    return 0;
}

void resources_drop_local(int resource_idx, int epoll_fd)
{
    resources_release_local_internal(resource_idx, epoll_fd, false);
}

void resources_begin_graceful_shutdown(int epoll_fd)
{
    move_cancel_all(epoll_fd, "daemon is shutting down");
    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        resource_runtime_t *res = &g_state.resources[i];
        if (res->owner_node == g_state.self_index && res->owner_instance_id == g_state.instance_id)
        {
            res->shutdown_release_required = true;
            res->shutdown_release_confirmed = false;
        }
    }
    resources_progress_graceful_shutdown(epoll_fd);
}

void resources_progress_graceful_shutdown(int epoll_fd)
{
    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        resource_runtime_t *res = &g_state.resources[i];
        if (res->shutdown_release_required &&
            res->owner_node == g_state.self_index &&
            res->owner_instance_id == g_state.instance_id &&
            res->hook.pid <= 0 &&
            res->state != LCS_RES_STOPPING &&
            res->state != LCS_RES_STOP_FAILED)
        {
            lcs_log_info("releasing resource %s before shutdown",
                         g_state.cfg.resources[i].name);
            resources_release_local((int)i, epoll_fd);
        }
    }
}

bool resources_graceful_shutdown_complete(void)
{
    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        const resource_runtime_t *res = &g_state.resources[i];
        if (!res->shutdown_release_required)
            continue;
        if (res->hook.pid > 0 ||
            (res->owner_node == g_state.self_index &&
             res->owner_instance_id == g_state.instance_id) ||
            !res->shutdown_release_confirmed)
            return false;
    }
    return true;
}

void resources_finish_graceful_shutdown(void)
{
    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        resources_cancel_hook((int)i);
        resources_cancel_vip_probe(&g_state.resources[i]);
        resources_cancel_service_operation(&g_state.resources[i], LCS_SERVICE_OP_NONE);
    }
}

static void resources_start_activation_rollback(int resource_idx, uint64_t epoch, uint64_t lease_id, int epoll_fd)
{
    if (resources_begin_service_stop(resource_idx, LCS_SERVICE_OP_ROLLBACK_STOP, epoch, lease_id, false) == 0)
    {
        lcs_log_warn("service %s start was not confirmed; stopping it before releasing lease", g_state.cfg.resources[resource_idx].name);
        return;
    }
    resources_enter_stop_failed_state(resource_idx, epoch + 1, "service start was not confirmed and rollback stop could not be started", epoll_fd);
}

static void resources_finish_service_stop(int resource_idx, bool handoff, int handoff_source, uint32_t handoff_seq, uint64_t epoch, uint64_t lease_id, bool post_hook, int epoll_fd)
{
    resource_runtime_t *res = &g_state.resources[resource_idx];
    if (handoff)
    {
        res->owner_node = -1;
        res->owner_instance_id = 0;
        res->state = LCS_RES_STOPPED;
        res->lease_id = 0;
        res->lease_deadline_ms = 0;
        res->renew_after_ms = 0;
        res->conflict_reason[0] = '\0';
        res->next_activation_attempt_ms = lcs_now_ms() + lcs_jittered_delay_ms(g_state.cfg.lease_ms);
        lease_cancel_operations(resource_idx);
        if (post_hook)
            resources_start_hook(resource_idx, LCS_HOOK_POST_STOP, epoch, lease_id);
        if (lease_complete_owner_release(resource_idx, g_state.self_index, epoch, lease_id, handoff_source, handoff_seq, epoll_fd) < 0)
            (void)peer_queue_simple_resp(epoll_fd, handoff_source, handoff_seq, LCS_MSG_OWNER_RELEASE_RESP, -1, "release quorum operation could not be started");
        return;
    }

    lease_release_majority(resource_idx, g_state.self_index, epoch, lease_id, epoll_fd);
    resources_clear_local_lease(res, resources_next_epoch(epoch));
    if (post_hook)
        resources_start_hook(resource_idx, LCS_HOOK_POST_STOP, resources_next_epoch(epoch), lease_id);
}

static bool resources_activation_lease_current(const resource_runtime_t *res, uint64_t epoch, uint64_t lease_id, uint64_t now)
{
    return res->owner_node == g_state.self_index &&
           res->owner_instance_id == g_state.instance_id &&
           res->state == LCS_RES_STARTING &&
           res->epoch == epoch &&
           res->lease_id == lease_id &&
           res->lease_deadline_ms > now &&
           cluster_has_quorum();
}

void resources_process_vip_operations(int epoll_fd)
{
    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        resource_runtime_t *res = &g_state.resources[i];
        if (res->vip_probe.pid <= 0)
            continue;

        uint64_t now = lcs_now_ms();
        bool timed_out = res->vip_probe.deadline_ms &&
                         now >= res->vip_probe.deadline_ms;
        int result = -1;
        int collect_rc = lcs_vip_probe_collect(res->vip_probe.pid, &result);
        if (collect_rc == 0)
        {
            if (timed_out && !res->vip_probe.kill_sent)
            {
                lcs_log_warn("asynchronous VIP conflict probe timed out resource=%s pid=%ld; requesting cancellation", g_state.cfg.resources[i].name, (long)res->vip_probe.pid);
                lcs_vip_probe_cancel(res->vip_probe.pid);
                res->vip_probe.kill_sent = true;
                res->vip_probe.deadline_ms = 0;
            }
            continue;
        }

        uint64_t epoch = res->vip_probe.epoch;
        uint64_t lease_id = res->vip_probe.lease_id;
        bool discard = res->vip_probe.discard_result;
        bool killed = res->vip_probe.kill_sent;
        if (collect_rc < 0)
        {
            lcs_log_warn("failed to collect VIP conflict probe resource=%s pid=%ld", g_state.cfg.resources[i].name, (long)res->vip_probe.pid);
            result = -1;
        } else if (timed_out || killed)
        {
            result = -1;
        }
        resources_clear_vip_probe(res);
        if (discard)
            continue;

        now = lcs_now_ms();
        bool still_current = resources_activation_lease_current(res, epoch, lease_id, now);
        if (!still_current)
        {
            if (res->owner_node == g_state.self_index && res->owner_instance_id == g_state.instance_id && res->epoch == epoch && res->lease_id == lease_id)
            {
                lcs_log_warn("discarding VIP probe result for %s because its activation lease is no longer valid", g_state.cfg.resources[i].name);
                lease_release_majority((int)i, g_state.self_index, epoch, lease_id, epoll_fd);
                resources_clear_local_lease(res, epoch);
                res->next_activation_attempt_ms = now + lcs_jittered_delay_ms(g_state.cfg.lease_ms);
            }
            continue;
        }
        if (result > 0)
        {
            lease_release_majority((int)i, g_state.self_index, epoch, lease_id, epoll_fd);
            resources_enter_conflict_state((int)i, epoch + 1, "VIP answered conflict probe before activation");
            peer_broadcast_state_sync(epoll_fd);
            continue;
        }
        if (result < 0)
        {
            lcs_log_warn("auto-place failed VIP %s: conflict probe failed", g_state.cfg.resources[i].name);
            lease_release_majority((int)i, g_state.self_index, epoch, lease_id, epoll_fd);
            resources_clear_local_lease(res, epoch);
            res->next_activation_attempt_ms = now + lcs_jittered_delay_ms(g_state.cfg.lease_ms);
            continue;
        }

        if (resource_start_local(&g_state.cfg.resources[i]) != 0)
        {
            lcs_log_warn("auto-place failed VIP %s: failed to add address", g_state.cfg.resources[i].name);
            lease_release_majority((int)i, g_state.self_index, epoch, lease_id, epoll_fd);
            resources_clear_local_lease(res, epoch);
            res->next_activation_attempt_ms = lcs_now_ms() + lcs_jittered_delay_ms(g_state.cfg.lease_ms);
            continue;
        }

        now = lcs_now_ms();
        if (!resources_activation_lease_current(res, epoch, lease_id, now))
        {
            lcs_log_warn("removing VIP %s because its activation lease expired while adding the address", g_state.cfg.resources[i].name);
            (void)resources_stop_local_backend(&g_state.cfg.resources[i]);
            lease_release_majority((int)i, g_state.self_index, epoch, lease_id, epoll_fd);
            resources_clear_local_lease(res, epoch);
            res->next_activation_attempt_ms = now + lcs_jittered_delay_ms(g_state.cfg.lease_ms);
            continue;
        }
        resources_mark_local_active((int)i, epoch, lease_id, epoll_fd);
    }
}

void resources_process_service_operations(int epoll_fd)
{
    uint64_t now = lcs_now_ms();
    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        resource_runtime_t *res = &g_state.resources[i];
        if (res->service.pid <= 0 || res->service.op == LCS_SERVICE_OP_NONE)
            continue;

        bool timed_out = res->service.deadline_ms && now >= res->service.deadline_ms;
        int result = -1;
        int collect_rc = lcs_systemd_service_collect(res->service.pid, &result);
        if (collect_rc == 0)
        {
            if (timed_out && !res->service.kill_sent)
            {
                lcs_log_warn("asynchronous systemd operation timed out resource=%s op=%u pid=%ld; requesting cancellation",
                             g_state.cfg.resources[i].name,
                             (unsigned)res->service.op,
                             (long)res->service.pid);
                lcs_systemd_service_cancel(res->service.pid);
                res->service.kill_sent = true;
                res->service.deadline_ms = 0;
            }
            continue;
        }
        if (collect_rc < 0)
        {
            lcs_log_warn("failed to collect systemd worker resource=%s pid=%ld", g_state.cfg.resources[i].name, (long)res->service.pid);
            result = -1;
        } else if (timed_out || res->service.kill_sent)
        {
            result = -1;
        }

        resource_service_op_type_t type = res->service.op;
        if (type == LCS_SERVICE_OP_CANCELLING)
        {
            resource_service_op_type_t next_type = res->service.next_op;
            bool handoff = res->service.handoff;
            int handoff_source = res->service.handoff_source_node;
            uint32_t handoff_seq = res->service.handoff_response_seq;
            uint64_t replacement_epoch = res->service.replacement.epoch;
            res->service.pid = 0;
            res->service.op = LCS_SERVICE_OP_NONE;
            res->service.next_op = LCS_SERVICE_OP_NONE;
            res->service.deadline_ms = 0;
            res->service.kill_sent = false;
            if (next_type == LCS_SERVICE_OP_NONE)
            {
                resources_clear_service_operation(res);
                continue;
            }
            if (resources_start_service_operation((int)i, next_type) == 0)
                continue;

            resources_enter_stop_failed_state(
                (int)i,
                next_type == LCS_SERVICE_OP_STATE_REPLACE ?
                replacement_epoch + 1 : res->service.epoch + 1,
                next_type == LCS_SERVICE_OP_STATE_REPLACE ?
                "failed to start asynchronous systemd stop while replacing local ownership" :
                "failed to start asynchronous systemd stop after cancelling superseded operation",
                epoll_fd);
            if (handoff)
                (void)peer_queue_simple_resp(epoll_fd, handoff_source, handoff_seq, LCS_MSG_OWNER_RELEASE_RESP, -1, "owner could not start service stop");

            continue;
        }

        uint64_t epoch = res->service.epoch;
        uint64_t lease_id = res->service.lease_id;
        bool post_hook = res->service.stop_post_hook;
        bool handoff = res->service.handoff;
        int handoff_source = res->service.handoff_source_node;
        uint32_t handoff_seq = res->service.handoff_response_seq;
        resources_clear_service_operation(res);

        if (type == LCS_SERVICE_OP_STARTUP_CLEANUP)
        {
            (void)resources_finish_startup_cleanup((int)i, result == 0);
            continue;
        }
        if (type == LCS_SERVICE_OP_HEALTH)
        {
            res->service.next_health_ms = now + 1000u;
            if (result == 1)
                continue;
            if (result < 0)
            {
                lcs_log_warn("owned service %s health check failed; keeping lease for retry", g_state.cfg.resources[i].name);
                continue;
            }
            if (res->owner_node == g_state.self_index && res->owner_instance_id == g_state.instance_id && res->state == LCS_RES_ACTIVE)
            {
                lcs_log_warn("owned service %s is no longer active; releasing for failover", g_state.cfg.resources[i].name);
                res->failover_pending = true;
                lease_release_majority((int)i, g_state.self_index, res->epoch, res->lease_id, epoll_fd);
                resources_clear_local_lease(res, resources_next_epoch(res->epoch));
                peer_broadcast_state_sync(epoll_fd);
            }
            continue;
        }
        if (type == LCS_SERVICE_OP_START)
        {
            bool still_current = res->owner_node == g_state.self_index &&
                                 res->owner_instance_id == g_state.instance_id &&
                                 res->state == LCS_RES_STARTING &&
                                 res->epoch == epoch &&
                                 res->lease_id == lease_id &&
                                 res->lease_deadline_ms > now &&
                                 cluster_has_quorum();
            if (result == 0 && still_current)
                resources_mark_local_active((int)i, epoch, lease_id, epoll_fd);
            else
                resources_start_activation_rollback((int)i, epoch, lease_id, epoll_fd);
            continue;
        }
        if (type == LCS_SERVICE_OP_STATE_REPLACE)
        {
            if (result != 0)
            {
                resources_enter_stop_failed_state((int)i,
                                                  res->service.replacement.epoch + 1,
                                                  "systemd service stop failed while replacing local ownership",
                                                  epoll_fd);
                continue;
            }
            res->owner_node = res->service.replacement.owner_node;
            res->owner_instance_id = res->service.replacement.owner_instance_id;
            res->state = res->service.replacement.state;
            res->epoch = res->service.replacement.epoch;
            res->lease_id = res->service.replacement.lease_id;
            res->lease_deadline_ms = res->service.replacement.deadline_ms;
            res->renew_after_ms = 0;
            snprintf(res->conflict_reason, sizeof(res->conflict_reason), "%s", res->service.replacement.reason);
            peer_broadcast_state_sync(epoll_fd);
            continue;
        }
        if (result != 0)
        {
            resources_enter_stop_failed_state((int)i, epoch + 1,
                                             type == LCS_SERVICE_OP_ROLLBACK_STOP ?
                                             "service start was not confirmed and rollback stop failed" :
                                             "asynchronous systemd service stop failed; service may still be running",
                                             epoll_fd);
            if (handoff)
                (void)peer_queue_simple_resp(epoll_fd, handoff_source, handoff_seq, LCS_MSG_OWNER_RELEASE_RESP, -1, "owner could not confirm service stop");
            continue;
        }
        resources_finish_service_stop((int)i, handoff, handoff_source, handoff_seq, epoch, lease_id, post_hook, epoll_fd);
    }
}

int resources_set_disabled(int resource_idx, bool disabled, int epoll_fd, char *message, size_t message_len)
{
    if (resource_idx < 0 || (size_t)resource_idx >= g_state.cfg.resource_count)
    {
        snprintf(message, message_len, "unknown resource");
        return -1;
    }

    resource_runtime_t *res = &g_state.resources[resource_idx];
    bool retry_stop_failed = disabled &&
                             res->disabled &&
                             res->owner_node == g_state.self_index &&
                             res->owner_instance_id == g_state.instance_id &&
                             res->state == LCS_RES_STOP_FAILED;
    if (res->disabled == disabled && !retry_stop_failed)
    {
        snprintf(message, message_len, "resource already %s", disabled ? "stopped" : "started");
        return 0;
    }
    if (!disabled && res->state == LCS_RES_STOP_FAILED)
    {
        snprintf(message, message_len, "resource stop failed; retry stop or fence node before starting");
        return -1;
    }

    uint64_t now = lcs_now_ms();
    res->disabled = disabled;
    res->disabled_generation = now > res->disabled_generation ?
                               now : res->disabled_generation + 1;
    res->next_activation_attempt_ms = 0;
    if (disabled)
    {
        lcs_log_info("%s resource %s", retry_stop_failed ? "admin retrying stop for" : "admin stopped", g_state.cfg.resources[resource_idx].name);
        if (res->owner_node == g_state.self_index &&
            res->owner_instance_id == g_state.instance_id &&
            (res->state == LCS_RES_ACTIVE ||
             res->state == LCS_RES_STARTING ||
             res->state == LCS_RES_STOPPING ||
             res->state == LCS_RES_STOP_FAILED))
            resources_release_local(resource_idx, epoll_fd);
        snprintf(message, message_len, "%s", retry_stop_failed ? "resource stop retry requested" : "resource stop requested");
    } else
    {
        lcs_log_info("admin started resource %s", g_state.cfg.resources[resource_idx].name);
        snprintf(message, message_len, "resource start requested");
    }
    peer_broadcast_state_sync(epoll_fd);
    return 0;
}

void resources_auto_place(int epoll_fd)
{
    uint64_t now = lcs_now_ms();
    if (g_state.started_ms && now - g_state.started_ms < LCS_STARTUP_AUTOPLACE_DELAY_MS)
    {
        lcs_log_debug4("auto-place skip: startup settle delay");
        return;
    }
    if (!cluster_has_quorum())
    {
        lcs_log_debug4("auto-place skip: quorum is not available (%u votes, need %u)", g_state.votes_seen, g_state.quorum_needed);
        return;
    }
    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        resource_runtime_t *res = &g_state.resources[i];
        if (res->disabled)
        {
            lcs_log_debug4("auto-place skip resource %s: resource is administratively stopped", g_state.cfg.resources[i].name);
            continue;
        }
        if (res->owner_node >= 0)
        {
            lcs_log_debug4("auto-place skip resource %s: owner is %s state=%u epoch=%llu",
                           g_state.cfg.resources[i].name,
                           cluster_node_name_or_none(res->owner_node),
                           (unsigned)res->state,
                           (unsigned long long)res->epoch);
            continue;
        }
        if (res->state == LCS_RES_CONFLICT || res->state == LCS_RES_STOP_FAILED)
        {
            lcs_log_debug4("auto-place skip resource %s: resource is in %s state epoch=%llu",
                           g_state.cfg.resources[i].name, lcs_resource_state_name(res->state),
                           (unsigned long long)res->epoch);
            continue;
        }
        if (lease_operation_active((int)i))
        {
            lcs_log_debug4("auto-place skip resource %s: lease operation already pending", g_state.cfg.resources[i].name);
            continue;
        }
        int target = group_auto_place_target((int)i);
        if (target < 0)
        {
            lcs_log_debug4("auto-place skip resource %s: group policy has no valid target", g_state.cfg.resources[i].name);
            continue;
        }
        if (target != g_state.self_index)
        {
            lcs_log_debug4("auto-place skip resource %s: selected full-member is %s, self is %s",
                           g_state.cfg.resources[i].name,
                           cluster_node_name_or_none(target),
                           g_state.cfg.nodes[g_state.self_index].name);
            continue;
        }
        lcs_log_debug2("auto-place try resource %s on %s current_epoch=%llu next_epoch=%llu",
                       g_state.cfg.resources[i].name,
                       g_state.cfg.nodes[g_state.self_index].name,
                       (unsigned long long)res->epoch,
                       (unsigned long long)(res->epoch + 1));
        resources_activate_local((int)i, res->epoch + 1, epoll_fd);
    }
}

void resources_home_rebalance(int epoll_fd)
{
    if (!cluster_has_quorum() || move_any_active())
        return;

    int coordinator = cluster_first_online_full_member();
    if (coordinator != g_state.self_index)
        return;

    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        const lcs_resource_config_t *resource = &g_state.cfg.resources[i];
        resource_runtime_t *res = &g_state.resources[i];
        if (resource->home_node_idx < 0 ||
            res->disabled ||
            res->home_blocked ||
            res->state != LCS_RES_ACTIVE ||
            res->owner_node < 0 ||
            res->owner_node == resource->home_node_idx ||
            !cluster_node_is_online((size_t)resource->home_node_idx) ||
            lease_operation_active((int)i) ||
            move_active_for_resource((int)i))
            continue;

        if (move_start_internal(epoll_fd, (int)i, resource->home_node_idx, "home-node rebalance") != 0 || move_any_active())
            return;
    }
}

void resources_maintain_owned_leases(int epoll_fd)
{
    uint64_t now = lcs_now_ms();
    if (!cluster_has_quorum())
    {
        if (g_state.had_quorum && !g_state.no_quorum_state_cleared)
            resources_clear_volatile_state_after_quorum_loss(epoll_fd);
        return;
    }

    g_state.had_quorum = true;
    g_state.no_quorum_state_cleared = false;

    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        resource_runtime_t *res = &g_state.resources[i];
        if (res->disabled &&
            res->owner_node == g_state.self_index &&
            res->owner_instance_id == g_state.instance_id &&
            (res->state == LCS_RES_ACTIVE ||
             res->state == LCS_RES_STARTING ||
             res->state == LCS_RES_STOPPING))
        {
            lcs_log_info("releasing administratively stopped resource %s", g_state.cfg.resources[i].name);
            resources_release_local((int)i, epoll_fd);
            continue;
        }
        if (res->owner_node != g_state.self_index ||
            res->owner_instance_id != g_state.instance_id ||
            (res->state != LCS_RES_ACTIVE &&
             res->state != LCS_RES_STARTING &&
             res->state != LCS_RES_STOPPING))
            continue;
        if (res->lease_deadline_ms && now >= res->lease_deadline_ms)
        {
            lcs_log_warn("dropping resource %s because local lease expired", g_state.cfg.resources[i].name);
            resources_release_local_internal((int)i, epoll_fd, false);
            continue;
        }
        if (g_state.cfg.resources[i].type == LCS_RESOURCE_SERVICE)
        {
            if (res->state == LCS_RES_ACTIVE &&
                res->service.op == LCS_SERVICE_OP_NONE &&
                (!res->service.next_health_ms ||
                 now >= res->service.next_health_ms))
            {
                if (resources_start_service_operation((int)i,
                                                      LCS_SERVICE_OP_HEALTH) != 0)
                    res->service.next_health_ms = now + 1000u;
            }
        } else
        {
            int active_rc = resource_is_local_active(&g_state.cfg.resources[i]);
            if (active_rc == 0)
            {
                res->failover_pending = true;
                resources_release_local_internal((int)i, epoll_fd, true);
                peer_broadcast_state_sync(epoll_fd);
                continue;
            }
        }
        if (res->renew_after_ms && now < res->renew_after_ms)
            continue;
            
        if (lease_operation_active((int)i))
        {
            lcs_log_debug3("renew resource %s skipped: lease operation already pending", g_state.cfg.resources[i].name);
            continue;
        }
        if (lease_start_renew((int)i, epoll_fd) != 0)
        {
            if (now + g_state.cfg.renew_ms >= res->lease_deadline_ms)
            {
                lcs_log_warn("dropping resource %s because lease renewal could not start", g_state.cfg.resources[i].name);
                resources_release_local_internal((int)i, epoll_fd, false);
            } else
            {
                res->renew_after_ms = now + g_state.cfg.renew_ms;
            }
        }
    }
}

void resources_process_hooks(int epoll_fd)
{
    uint64_t now = lcs_now_ms();
    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        resource_runtime_t *res = &g_state.resources[i];
        if (res->hook.pid <= 0)
            continue;

        int status = 0;
        pid_t rc = waitpid(res->hook.pid, &status, WNOHANG);
        bool done = rc == res->hook.pid || rc < 0;
        bool ok = done && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (!done && res->hook.deadline_ms && now >= res->hook.deadline_ms)
        {
            lcs_log_warn("%s hook for resource %s timed out; killing pid=%ld",
                         resources_hook_name(res->hook.type), g_state.cfg.resources[i].name,
                         (long)res->hook.pid);
            kill(res->hook.pid, SIGKILL);
            waitpid(res->hook.pid, &status, 0);
            done = true;
            ok = false;
        }
        if (!done)
            continue;
        if (rc < 0)
        {
            lcs_log_warn("%s hook for resource %s waitpid failed: %s",
                         resources_hook_name(res->hook.type), g_state.cfg.resources[i].name,
                         strerror(errno));
            ok = false;
        }

        resource_hook_type_t type = res->hook.type;
        uint64_t hook_epoch = res->hook.epoch;
        uint64_t hook_lease_id = res->hook.lease_id;
        lcs_log_info("%s hook for resource %s completed status=%s", resources_hook_name(type), g_state.cfg.resources[i].name, ok ? "ok" : "failed");
        resources_clear_hook(res);

        if (type == LCS_HOOK_PRE_START)
        {
            bool still_current = res->owner_node == g_state.self_index &&
                                 res->owner_instance_id == g_state.instance_id &&
                                 res->state == LCS_RES_STARTING &&
                                 res->epoch == hook_epoch &&
                                 res->lease_id == hook_lease_id;
            if (!ok || !still_current)
            {
                lcs_log_warn("aborting resource %s activation after pre-start hook status=%s current=%s",
                             g_state.cfg.resources[i].name, ok ? "ok" : "failed",
                             still_current ? "true" : "false");
                if (still_current)
                {
                    lease_release_majority((int)i, g_state.self_index, hook_epoch, hook_lease_id, epoll_fd);
                    resources_clear_local_lease(res, hook_epoch);
                    res->next_activation_attempt_ms = now + lcs_jittered_delay_ms(g_state.cfg.lease_ms);
                }
                continue;
            }
            resources_complete_local_activation((int)i, hook_epoch, hook_lease_id, epoll_fd);
        } else if (type == LCS_HOOK_PRE_STOP)
        {
            if (!ok)
            {
                lcs_log_warn("pre-stop hook for resource %s failed; stopping VIP anyway", g_state.cfg.resources[i].name);
            }
            resources_release_local_internal((int)i, epoll_fd, false);
            resources_start_hook((int)i, LCS_HOOK_POST_STOP, hook_epoch + 1, hook_lease_id);
        } else if (!ok)
        {
            lcs_log_warn("%s hook for resource %s failed after VIP event", resources_hook_name(type), g_state.cfg.resources[i].name);
        }
    }
}
