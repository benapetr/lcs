// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "resources.h"

#include "cluster.h"
#include "group.h"
#include "lease.h"
#include "log.h"
#include "move.h"
#include "peer.h"
#include "util.h"
#include "vip.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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

static const char *resources_hook_path(const lcs_vip_config_t *vip, resource_hook_type_t type)
{
    switch (type)
    {
        case LCS_HOOK_PRE_START:
            return vip->pre_start;
        case LCS_HOOK_POST_START:
            return vip->post_start;
        case LCS_HOOK_PRE_STOP:
            return vip->pre_stop;
        case LCS_HOOK_POST_STOP:
            return vip->post_stop;
        default:
            return "";
    }
}

static void resources_clear_hook(resource_runtime_t *res)
{
    res->hook_pid = 0;
    res->hook_type = LCS_HOOK_NONE;
    res->hook_deadline_ms = 0;
    res->hook_epoch = 0;
    res->hook_lease_id = 0;
}

static void resources_cancel_hook(int vip_idx)
{
    resource_runtime_t *res = &g_state.resources[vip_idx];
    if (res->hook_pid <= 0)
        return;

    lcs_log_warn("cancelling %s hook for VIP %s pid=%ld",
                 resources_hook_name(res->hook_type), g_state.cfg.vips[vip_idx].name,
                 (long)res->hook_pid);
    kill(res->hook_pid, SIGKILL);
    waitpid(res->hook_pid, NULL, 0);
    resources_clear_hook(res);
}

static int resources_start_hook(int vip_idx, resource_hook_type_t type, uint64_t epoch, uint64_t lease_id)
{
    resource_runtime_t *res = &g_state.resources[vip_idx];
    const lcs_vip_config_t *vip = &g_state.cfg.vips[vip_idx];
    const char *path = resources_hook_path(vip, type);
    if (!*path)
        return 1;

    if (res->hook_pid > 0)
    {
        lcs_log_warn("cannot start %s hook for VIP %s: %s hook pid=%ld still running",
                     resources_hook_name(type), vip->name, resources_hook_name(res->hook_type),
                     (long)res->hook_pid);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        lcs_log_warn("failed to fork %s hook for VIP %s: %s", resources_hook_name(type), vip->name, strerror(errno));
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
        setenv("LCS_VIP", vip->name, 1);
        setenv("LCS_ADDRESS", vip->address, 1);
        setenv("LCS_INTERFACE", vip->interface, 1);
        setenv("LCS_EVENT", resources_hook_name(type), 1);
        setenv("LCS_EPOCH", epoch_buf, 1);
        setenv("LCS_LEASE_ID", lease_buf, 1);
        setenv("LCS_HOOK_TIMEOUT_MS", timeout_buf, 1);
        execl(path, path, (char *)NULL);
        _exit(127);
    }

    res->hook_pid = pid;
    res->hook_type = type;
    res->hook_deadline_ms = lcs_now_ms() + g_state.cfg.hook_timeout_ms;
    res->hook_epoch = epoch;
    res->hook_lease_id = lease_id;
    lcs_log_info("started %s hook for VIP %s pid=%ld path=%s timeout_ms=%u",
                 resources_hook_name(type), vip->name, (long)pid, path, g_state.cfg.hook_timeout_ms);
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

static int resources_complete_local_activation(int vip_idx, uint64_t epoch, uint64_t lease_id, int epoll_fd)
{
    resource_runtime_t *res = &g_state.resources[vip_idx];
    uint64_t now = lcs_now_ms();
    // TODO: VIP conflict probing is intentionally still synchronous. Making ARP/ND
    // probes fully scheduler-driven would require a separate async probe state
    // machine for little practical benefit while probe waits remain short.
    int conflict_rc = lcs_vip_conflict_check(&g_state.cfg, &g_state.cfg.vips[vip_idx]);
    if (conflict_rc > 0)
    {
        lease_release_majority(vip_idx, g_state.self_index, epoch, lease_id, epoll_fd);
        resources_enter_conflict_state(vip_idx, epoch + 1, "VIP answered conflict probe before activation");
        peer_broadcast_state_sync(epoll_fd);
        return -1;
    }
    if (conflict_rc < 0)
    {
        lcs_log_warn("auto-place failed VIP %s: conflict probe failed", g_state.cfg.vips[vip_idx].name);
        lease_release_majority(vip_idx, g_state.self_index, epoch, lease_id, epoll_fd);
        resources_clear_local_lease(res, epoch);
        res->next_activation_attempt_ms = now + g_state.cfg.lease_ms;
        return -1;
    }
    if (lcs_vip_add(&g_state.cfg.vips[vip_idx]) != 0)
    {
        lcs_log_warn("auto-place failed VIP %s: failed to add %s on %s",
                     g_state.cfg.vips[vip_idx].name,
                     g_state.cfg.vips[vip_idx].address,
                     g_state.cfg.vips[vip_idx].interface);
        lease_release_majority(vip_idx, g_state.self_index, epoch, lease_id, epoll_fd);
        resources_clear_local_lease(res, epoch);
        res->next_activation_attempt_ms = now + g_state.cfg.lease_ms;
        return -1;
    }
    res->state = LCS_RES_ACTIVE;
    res->next_activation_attempt_ms = 0;
    if (lcs_vip_announce(&g_state.cfg, &g_state.cfg.vips[vip_idx]) != 0)
    {
        lcs_log_warn("failed to send VIP announcement for %s on %s",
                     g_state.cfg.vips[vip_idx].address,
                     g_state.cfg.vips[vip_idx].interface);
    }
    if (res->failover_pending)
    {
        res->failover_count++;
        res->failover_pending = false;
        lcs_log_info("counted failover for VIP %s total=%llu",
                     g_state.cfg.vips[vip_idx].name,
                     (unsigned long long)res->failover_count);
    }
    lcs_log_info("activated VIP %s on %s epoch=%llu",
                 g_state.cfg.vips[vip_idx].name, g_state.cfg.nodes[g_state.self_index].name,
                 (unsigned long long)epoch);
    peer_broadcast_state_sync(epoll_fd);
    resources_start_hook(vip_idx, LCS_HOOK_POST_START, epoch, lease_id);
    return 0;
}

static void resources_release_local_internal(int vip_idx, int epoll_fd, bool allow_hooks)
{
    resource_runtime_t *res = &g_state.resources[vip_idx];
    uint64_t release_epoch = res->epoch + 1;
    uint64_t old_lease_id = res->lease_id;
    bool locally_owned = res->owner_node == g_state.self_index && res->owner_instance_id == g_state.instance_id;

    if (!locally_owned)
    {
        resources_clear_local_lease(res, release_epoch);
        return;
    }

    if (!allow_hooks)
        resources_cancel_hook(vip_idx);
    else if (res->hook_pid > 0 && res->hook_type != LCS_HOOK_PRE_STOP)
        resources_cancel_hook(vip_idx);
    if (allow_hooks && res->state == LCS_RES_ACTIVE && *g_state.cfg.vips[vip_idx].pre_stop)
    {
        res->state = LCS_RES_STOPPING;
        if (resources_start_hook(vip_idx, LCS_HOOK_PRE_STOP, res->epoch, res->lease_id) == 0)
            return;
        lcs_log_warn("continuing VIP %s stop without pre-stop hook",  g_state.cfg.vips[vip_idx].name);
    }

    if (res->state == LCS_RES_ACTIVE || res->state == LCS_RES_STOPPING)
        lcs_vip_del(&g_state.cfg.vips[vip_idx]);

    lease_release_majority(vip_idx, g_state.self_index, release_epoch, old_lease_id, epoll_fd);
    resources_clear_local_lease(res, release_epoch);
    if (allow_hooks)
        resources_start_hook(vip_idx, LCS_HOOK_POST_STOP, release_epoch, old_lease_id);
}

static void resources_clear_volatile_state_after_quorum_loss(int epoll_fd)
{
    lcs_log_warn("quorum lost; dropping local VIPs and clearing volatile cluster state");

    move_cancel_all(epoll_fd, "majority quorum is not available");

    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
    {
        uint64_t failover_count = g_state.resources[i].failover_count;
        uint64_t home_generation = g_state.resources[i].home_generation;
        bool home_blocked = g_state.resources[i].home_blocked;
        if (g_state.resources[i].owner_node == g_state.self_index &&
            g_state.resources[i].owner_instance_id == g_state.instance_id)
            resources_release_local_internal((int)i, epoll_fd, false);
        resources_cancel_hook((int)i);
        memset(&g_state.resources[i], 0, sizeof(g_state.resources[i]));
        g_state.resources[i].owner_node = -1;
        g_state.resources[i].state = LCS_RES_STOPPED;
        g_state.resources[i].failover_count = failover_count;
        g_state.resources[i].home_generation = home_generation;
        g_state.resources[i].home_blocked = home_blocked;
    }

    lease_cancel_all_operations();
    g_state.no_quorum_state_cleared = true;
}

void resources_cleanup_local_vips_without_lease(void)
{
    if (g_state.cfg.nodes[g_state.self_index].role != LCS_NODE_FULL)
    {
        lcs_log_debug("skipping local VIP cleanup on quorum-only node");
        return;
    }
    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
    {
        if (g_state.resources[i].owner_node != g_state.self_index ||
            g_state.resources[i].owner_instance_id != g_state.instance_id ||
            g_state.resources[i].state != LCS_RES_ACTIVE)
            lcs_vip_del(&g_state.cfg.vips[i]);
    }
}

void resources_enter_conflict_state(int vip_idx, uint64_t epoch, const char *reason)
{
    resource_runtime_t *res = &g_state.resources[vip_idx];
    lease_cancel_operations(vip_idx);
    if (res->owner_node == g_state.self_index &&
        res->owner_instance_id == g_state.instance_id &&
        res->state == LCS_RES_ACTIVE)
        lcs_vip_del(&g_state.cfg.vips[vip_idx]);
    res->epoch = epoch > res->epoch ? epoch : res->epoch + 1;
    res->owner_node = -1;
    res->owner_instance_id = 0;
    res->state = LCS_RES_CONFLICT;
    res->lease_id = 0;
    res->lease_deadline_ms = 0;
    res->renew_after_ms = 0;
    res->next_activation_attempt_ms = 0;
    snprintf(res->conflict_reason, sizeof(res->conflict_reason), "%s",
             reason ? reason : "VIP conflict detected");
    lcs_log_warn("VIP %s entered conflict state at epoch=%llu: %s; admin clear required",
                 g_state.cfg.vips[vip_idx].name, (unsigned long long)res->epoch,
                 res->conflict_reason);
}

int resources_activate_acquired_local(int vip_idx, uint64_t epoch, uint64_t lease_id, int epoll_fd)
{
    resource_runtime_t *res = &g_state.resources[vip_idx];
    uint64_t now = lcs_now_ms();
    if (*g_state.cfg.vips[vip_idx].pre_start)
    {
        res->state = LCS_RES_STARTING;
        if (resources_start_hook(vip_idx, LCS_HOOK_PRE_START, epoch, lease_id) == 0)
            return 0;
        lcs_log_warn("auto-place failed VIP %s: failed to start pre-start hook", g_state.cfg.vips[vip_idx].name);
        lease_release_majority(vip_idx, g_state.self_index, epoch, lease_id, epoll_fd);
        resources_clear_local_lease(res, epoch);
        res->next_activation_attempt_ms = now + g_state.cfg.lease_ms;
        return -1;
    }
    return resources_complete_local_activation(vip_idx, epoch, lease_id, epoll_fd);
}

int resources_activate_local(int vip_idx, uint64_t epoch, int epoll_fd)
{
    if (g_state.cfg.nodes[g_state.self_index].role != LCS_NODE_FULL)
    {
        lcs_log_debug("refusing to activate VIP %s on non-full-member node", g_state.cfg.vips[vip_idx].name);
        return -1;
    }
    resource_runtime_t *res = &g_state.resources[vip_idx];
    uint64_t now = lcs_now_ms();
    if (res->state == LCS_RES_CONFLICT)
    {
        lcs_log_warn("refusing to activate VIP %s because it is in conflict state", g_state.cfg.vips[vip_idx].name);
        return -1;
    }
    if (res->next_activation_attempt_ms && now < res->next_activation_attempt_ms)
    {
        lcs_log_debug2("auto-place skip VIP %s: activation retry backoff %llu ms remaining",
                       g_state.cfg.vips[vip_idx].name,
                       (unsigned long long)(res->next_activation_attempt_ms - now));
        return -1;
    }
    uint64_t lease_id = lcs_random_u64();
    if (lease_start_acquire(vip_idx, g_state.self_index, epoch, lease_id, epoll_fd) != 0)
    {
        lcs_log_debug2("auto-place failed VIP %s: could not start majority lease acquire for epoch=%llu",
                       g_state.cfg.vips[vip_idx].name, (unsigned long long)epoch);
        res->next_activation_attempt_ms = now + g_state.cfg.renew_ms;
        return -1;
    }
    return 0;
}

void resources_release_local(int vip_idx, int epoll_fd)
{
    resources_release_local_internal(vip_idx, epoll_fd, true);
}

int resources_release_for_handoff(int vip_idx, uint64_t epoch, uint64_t lease_id)
{
    resource_runtime_t *res = &g_state.resources[vip_idx];

    resources_cancel_hook(vip_idx);
    if (lcs_vip_del(&g_state.cfg.vips[vip_idx]) != 0)
        return -1;

    res->owner_node = -1;
    res->owner_instance_id = 0;
    res->state = LCS_RES_STOPPED;
    res->lease_id = 0;
    res->lease_deadline_ms = 0;
    res->renew_after_ms = 0;
    res->conflict_reason[0] = '\0';
    res->next_activation_attempt_ms = lcs_now_ms() + g_state.cfg.lease_ms;
    lease_cancel_operations(vip_idx);
    resources_start_hook(vip_idx, LCS_HOOK_POST_STOP, epoch, lease_id);
    return 0;
}

void resources_drop_local(int vip_idx, int epoll_fd)
{
    resources_release_local_internal(vip_idx, epoll_fd, false);
}

void resources_graceful_shutdown(int epoll_fd)
{
    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
    {
        resource_runtime_t *res = &g_state.resources[i];
        if (res->owner_node == g_state.self_index &&
            res->owner_instance_id == g_state.instance_id)
        {
            lcs_log_info("releasing VIP %s before shutdown", g_state.cfg.vips[i].name);
            resources_release_local((int)i, epoll_fd);
        }
    }
    uint64_t deadline = lcs_now_ms() + g_state.cfg.hook_timeout_ms + 100u;
    for (;;)
    {
        bool pending = false;
        resources_process_hooks(epoll_fd);
        for (size_t i = 0; i < g_state.cfg.vip_count; i++)
        {
            if (g_state.resources[i].hook_pid > 0)
                pending = true;
        }
        if (!pending || lcs_now_ms() >= deadline)
            break;
        usleep(10000);
    }
    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
        resources_cancel_hook((int)i);
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
    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
    {
        resource_runtime_t *res = &g_state.resources[i];
        if (res->owner_node >= 0)
        {
            lcs_log_debug4("auto-place skip VIP %s: owner is %s state=%u epoch=%llu",
                           g_state.cfg.vips[i].name,
                           cluster_node_name_or_none(res->owner_node),
                           (unsigned)res->state,
                           (unsigned long long)res->epoch);
            continue;
        }
        if (res->state == LCS_RES_CONFLICT)
        {
            lcs_log_debug4("auto-place skip VIP %s: resource is in conflict state epoch=%llu",
                           g_state.cfg.vips[i].name, (unsigned long long)res->epoch);
            continue;
        }
        if (lease_operation_active((int)i))
        {
            lcs_log_debug4("auto-place skip VIP %s: lease operation already pending", g_state.cfg.vips[i].name);
            continue;
        }
        int target = group_auto_place_target((int)i);
        if (target < 0)
        {
            lcs_log_debug4("auto-place skip VIP %s: group policy has no valid target", g_state.cfg.vips[i].name);
            continue;
        }
        if (target != g_state.self_index)
        {
            lcs_log_debug4("auto-place skip VIP %s: selected full-member is %s, self is %s",
                           g_state.cfg.vips[i].name,
                           cluster_node_name_or_none(target),
                           g_state.cfg.nodes[g_state.self_index].name);
            continue;
        }
        lcs_log_debug2("auto-place try VIP %s on %s current_epoch=%llu next_epoch=%llu",
                       g_state.cfg.vips[i].name,
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

    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
    {
        const lcs_vip_config_t *vip = &g_state.cfg.vips[i];
        resource_runtime_t *res = &g_state.resources[i];
        if (vip->home_node_idx < 0 ||
            res->home_blocked ||
            res->state != LCS_RES_ACTIVE ||
            res->owner_node < 0 ||
            res->owner_node == vip->home_node_idx ||
            !cluster_node_is_online((size_t)vip->home_node_idx) ||
            lease_operation_active((int)i) ||
            move_active_for_vip((int)i))
            continue;

        if (move_start_internal(epoll_fd, (int)i, vip->home_node_idx, "home-node rebalance") != 0 ||
            move_any_active())
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

    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
    {
        resource_runtime_t *res = &g_state.resources[i];
        if (res->owner_node != g_state.self_index ||
            res->owner_instance_id != g_state.instance_id ||
            (res->state != LCS_RES_ACTIVE &&
             res->state != LCS_RES_STARTING &&
             res->state != LCS_RES_STOPPING))
            continue;
        if (res->lease_deadline_ms && now >= res->lease_deadline_ms)
        {
            lcs_log_warn("dropping VIP %s because local lease expired", g_state.cfg.vips[i].name);
            resources_release_local_internal((int)i, epoll_fd, false);
            continue;
        }
        if (res->renew_after_ms && now < res->renew_after_ms)
            continue;
            
        if (lease_operation_active((int)i))
        {
            lcs_log_debug3("renew VIP %s skipped: lease operation already pending", g_state.cfg.vips[i].name);
            continue;
        }
        if (lease_start_renew((int)i, epoll_fd) != 0)
        {
            if (now + g_state.cfg.renew_ms >= res->lease_deadline_ms)
            {
                lcs_log_warn("dropping VIP %s because lease renewal could not start", g_state.cfg.vips[i].name);
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
    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
    {
        resource_runtime_t *res = &g_state.resources[i];
        if (res->hook_pid <= 0)
            continue;

        int status = 0;
        pid_t rc = waitpid(res->hook_pid, &status, WNOHANG);
        bool done = rc == res->hook_pid || rc < 0;
        bool ok = done && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (!done && res->hook_deadline_ms && now >= res->hook_deadline_ms)
        {
            lcs_log_warn("%s hook for VIP %s timed out; killing pid=%ld",
                         resources_hook_name(res->hook_type), g_state.cfg.vips[i].name,
                         (long)res->hook_pid);
            kill(res->hook_pid, SIGKILL);
            waitpid(res->hook_pid, &status, 0);
            done = true;
            ok = false;
        }
        if (!done)
            continue;
        if (rc < 0)
        {
            lcs_log_warn("%s hook for VIP %s waitpid failed: %s",
                         resources_hook_name(res->hook_type), g_state.cfg.vips[i].name,
                         strerror(errno));
            ok = false;
        }

        resource_hook_type_t type = res->hook_type;
        uint64_t hook_epoch = res->hook_epoch;
        uint64_t hook_lease_id = res->hook_lease_id;
        lcs_log_info("%s hook for VIP %s completed status=%s", resources_hook_name(type), g_state.cfg.vips[i].name, ok ? "ok" : "failed");
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
                lcs_log_warn("aborting VIP %s activation after pre-start hook status=%s current=%s",
                             g_state.cfg.vips[i].name, ok ? "ok" : "failed",
                             still_current ? "true" : "false");
                if (still_current)
                {
                    lease_release_majority((int)i, g_state.self_index, hook_epoch, hook_lease_id, epoll_fd);
                    resources_clear_local_lease(res, hook_epoch);
                    res->next_activation_attempt_ms = now + g_state.cfg.lease_ms;
                }
                continue;
            }
            resources_complete_local_activation((int)i, hook_epoch, hook_lease_id, epoll_fd);
        } else if (type == LCS_HOOK_PRE_STOP)
        {
            if (!ok)
            {
                lcs_log_warn("pre-stop hook for VIP %s failed; stopping VIP anyway", g_state.cfg.vips[i].name);
            }
            resources_release_local_internal((int)i, epoll_fd, false);
            resources_start_hook((int)i, LCS_HOOK_POST_STOP, hook_epoch + 1, hook_lease_id);
        } else if (!ok)
        {
            lcs_log_warn("%s hook for VIP %s failed after VIP event", resources_hook_name(type), g_state.cfg.vips[i].name);
        }
    }
}
