// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "resources.h"

#include "cluster.h"
#include "lease.h"
#include "log.h"
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

static void resources_cancel_hook(daemon_state_t *st, int vip_idx)
{
    resource_runtime_t *res = &st->resources[vip_idx];
    if (res->hook_pid <= 0)
        return;

    lcs_log_warn("cancelling %s hook for VIP %s pid=%ld",
                 resources_hook_name(res->hook_type), st->cfg.vips[vip_idx].name,
                 (long)res->hook_pid);
    kill(res->hook_pid, SIGKILL);
    waitpid(res->hook_pid, NULL, 0);
    resources_clear_hook(res);
}

static int resources_start_hook(daemon_state_t *st, int vip_idx, resource_hook_type_t type, uint64_t epoch, uint64_t lease_id)
{
    resource_runtime_t *res = &st->resources[vip_idx];
    const lcs_vip_config_t *vip = &st->cfg.vips[vip_idx];
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
        snprintf(timeout_buf, sizeof(timeout_buf), "%u", st->cfg.hook_timeout_ms);
        setenv("LCS_CLUSTER", st->cfg.cluster_name, 1);
        setenv("LCS_NODE", st->cfg.nodes[st->self_index].name, 1);
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
    res->hook_deadline_ms = lcs_now_ms() + st->cfg.hook_timeout_ms;
    res->hook_epoch = epoch;
    res->hook_lease_id = lease_id;
    lcs_log_info("started %s hook for VIP %s pid=%ld path=%s timeout_ms=%u",
                 resources_hook_name(type), vip->name, (long)pid, path, st->cfg.hook_timeout_ms);
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

static int resources_complete_local_activation(daemon_state_t *st, int vip_idx, uint64_t epoch,
                                     uint64_t lease_id, int epoll_fd)
{
    resource_runtime_t *res = &st->resources[vip_idx];
    uint64_t now = lcs_now_ms();
    int conflict_rc = lcs_vip_conflict_check(&st->cfg, &st->cfg.vips[vip_idx]);
    if (conflict_rc > 0)
    {
        lease_release_majority(st, vip_idx, st->self_index, epoch, lease_id, epoll_fd);
        resources_enter_conflict_state(st, vip_idx, epoch + 1, "VIP answered conflict probe before activation");
        peer_broadcast_state_sync(st, epoll_fd);
        return -1;
    }
    if (conflict_rc < 0)
    {
        lcs_log_warn("auto-place failed VIP %s: conflict probe failed", st->cfg.vips[vip_idx].name);
        lease_release_majority(st, vip_idx, st->self_index, epoch, lease_id, epoll_fd);
        resources_clear_local_lease(res, epoch);
        res->next_activation_attempt_ms = now + st->cfg.lease_ms;
        return -1;
    }
    if (lcs_vip_add(&st->cfg.vips[vip_idx]) != 0)
    {
        lcs_log_warn("auto-place failed VIP %s: failed to add %s on %s",
                     st->cfg.vips[vip_idx].name,
                     st->cfg.vips[vip_idx].address,
                     st->cfg.vips[vip_idx].interface);
        lease_release_majority(st, vip_idx, st->self_index, epoch, lease_id, epoll_fd);
        resources_clear_local_lease(res, epoch);
        res->next_activation_attempt_ms = now + st->cfg.lease_ms;
        return -1;
    }
    res->state = LCS_RES_ACTIVE;
    res->next_activation_attempt_ms = 0;
    if (lcs_vip_announce(&st->cfg, &st->cfg.vips[vip_idx]) != 0)
    {
        lcs_log_warn("failed to send VIP announcement for %s on %s",
                     st->cfg.vips[vip_idx].address,
                     st->cfg.vips[vip_idx].interface);
    }
    if (res->failover_pending)
    {
        res->failover_count++;
        res->failover_pending = false;
        lcs_log_info("counted failover for VIP %s total=%llu",
                     st->cfg.vips[vip_idx].name,
                     (unsigned long long)res->failover_count);
    }
    lcs_log_info("activated VIP %s on %s epoch=%llu",
                 st->cfg.vips[vip_idx].name, st->cfg.nodes[st->self_index].name,
                 (unsigned long long)epoch);
    resources_start_hook(st, vip_idx, LCS_HOOK_POST_START, epoch, lease_id);
    return 0;
}

static void resources_release_local_internal(daemon_state_t *st, int vip_idx, int epoll_fd, bool allow_hooks)
{
    resource_runtime_t *res = &st->resources[vip_idx];
    uint64_t release_epoch = res->epoch + 1;
    uint64_t old_lease_id = res->lease_id;
    bool locally_owned = res->owner_node == st->self_index && res->owner_instance_id == st->instance_id;

    if (!locally_owned)
    {
        resources_clear_local_lease(res, release_epoch);
        return;
    }

    if (!allow_hooks)
        resources_cancel_hook(st, vip_idx);
    else if (res->hook_pid > 0 && res->hook_type != LCS_HOOK_PRE_STOP)
        resources_cancel_hook(st, vip_idx);
    if (allow_hooks && res->state == LCS_RES_ACTIVE && *st->cfg.vips[vip_idx].pre_stop)
    {
        res->state = LCS_RES_STOPPING;
        if (resources_start_hook(st, vip_idx, LCS_HOOK_PRE_STOP, res->epoch, res->lease_id) == 0)
            return;
        lcs_log_warn("continuing VIP %s stop without pre-stop hook",  st->cfg.vips[vip_idx].name);
    }

    if (res->state == LCS_RES_ACTIVE || res->state == LCS_RES_STOPPING)
        lcs_vip_del(&st->cfg.vips[vip_idx]);

    lease_release_majority(st, vip_idx, st->self_index, release_epoch, old_lease_id, epoll_fd);
    resources_clear_local_lease(res, release_epoch);
    if (allow_hooks)
        resources_start_hook(st, vip_idx, LCS_HOOK_POST_STOP, release_epoch, old_lease_id);
}

void resources_cleanup_local_vips_without_lease(daemon_state_t *st)
{
    if (st->cfg.nodes[st->self_index].role != LCS_NODE_FULL)
    {
        lcs_log_debug("skipping local VIP cleanup on quorum-only node");
        return;
    }
    for (size_t i = 0; i < st->cfg.vip_count; i++)
    {
        if (st->resources[i].owner_node != st->self_index ||
            st->resources[i].owner_instance_id != st->instance_id ||
            st->resources[i].state != LCS_RES_ACTIVE)
            lcs_vip_del(&st->cfg.vips[i]);
    }
}

void resources_enter_conflict_state(daemon_state_t *st, int vip_idx, uint64_t epoch,
                          const char *reason)
{
    resource_runtime_t *res = &st->resources[vip_idx];
    if (res->owner_node == st->self_index &&
        res->owner_instance_id == st->instance_id &&
        res->state == LCS_RES_ACTIVE)
        lcs_vip_del(&st->cfg.vips[vip_idx]);
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
                 st->cfg.vips[vip_idx].name, (unsigned long long)res->epoch,
                 res->conflict_reason);
}

int resources_activate_acquired_local(daemon_state_t *st, int vip_idx,
                                      uint64_t epoch, uint64_t lease_id,
                                      int epoll_fd)
{
    resource_runtime_t *res = &st->resources[vip_idx];
    uint64_t now = lcs_now_ms();
    if (*st->cfg.vips[vip_idx].pre_start)
    {
        res->state = LCS_RES_STARTING;
        if (resources_start_hook(st, vip_idx, LCS_HOOK_PRE_START, epoch, lease_id) == 0)
            return 0;
        lcs_log_warn("auto-place failed VIP %s: failed to start pre-start hook",
                     st->cfg.vips[vip_idx].name);
        lease_release_majority(st, vip_idx, st->self_index, epoch, lease_id,
                               epoll_fd);
        resources_clear_local_lease(res, epoch);
        res->next_activation_attempt_ms = now + st->cfg.lease_ms;
        return -1;
    }
    return resources_complete_local_activation(st, vip_idx, epoch, lease_id, epoll_fd);
}

int resources_activate_local(daemon_state_t *st, int vip_idx, uint64_t epoch, int epoll_fd)
{
    if (st->cfg.nodes[st->self_index].role != LCS_NODE_FULL)
    {
        lcs_log_debug("refusing to activate VIP %s on non-full-member node",
                      st->cfg.vips[vip_idx].name);
        return -1;
    }
    resource_runtime_t *res = &st->resources[vip_idx];
    uint64_t now = lcs_now_ms();
    if (res->state == LCS_RES_CONFLICT)
    {
        lcs_log_warn("refusing to activate VIP %s because it is in conflict state",
                     st->cfg.vips[vip_idx].name);
        return -1;
    }
    if (res->next_activation_attempt_ms && now < res->next_activation_attempt_ms)
    {
        lcs_log_debug2("auto-place skip VIP %s: activation retry backoff %llu ms remaining",
                       st->cfg.vips[vip_idx].name,
                       (unsigned long long)(res->next_activation_attempt_ms - now));
        return -1;
    }
    uint64_t lease_id = lcs_random_u64();
    if (lease_start_acquire(st, vip_idx, st->self_index, epoch, lease_id,
                            epoll_fd) != 0)
    {
        lcs_log_debug2("auto-place failed VIP %s: could not start majority lease acquire for epoch=%llu",
                       st->cfg.vips[vip_idx].name, (unsigned long long)epoch);
        res->next_activation_attempt_ms = now + st->cfg.renew_ms;
        return -1;
    }
    return 0;
}

void resources_release_local(daemon_state_t *st, int vip_idx, int epoll_fd)
{
    resources_release_local_internal(st, vip_idx, epoll_fd, true);
}

void resources_drop_local(daemon_state_t *st, int vip_idx, int epoll_fd)
{
    resources_release_local_internal(st, vip_idx, epoll_fd, false);
}

void resources_graceful_shutdown(daemon_state_t *st, int epoll_fd)
{
    for (size_t i = 0; i < st->cfg.vip_count; i++)
    {
        resource_runtime_t *res = &st->resources[i];
        if (res->owner_node == st->self_index &&
            res->owner_instance_id == st->instance_id)
        {
            lcs_log_info("releasing VIP %s before shutdown", st->cfg.vips[i].name);
            resources_release_local(st, (int)i, epoll_fd);
        }
    }
    uint64_t deadline = lcs_now_ms() + st->cfg.hook_timeout_ms + 100u;
    for (;;)
    {
        bool pending = false;
        resources_process_hooks(st, epoll_fd);
        for (size_t i = 0; i < st->cfg.vip_count; i++)
        {
            if (st->resources[i].hook_pid > 0)
                pending = true;
        }
        if (!pending || lcs_now_ms() >= deadline)
            break;
        usleep(10000);
    }
    for (size_t i = 0; i < st->cfg.vip_count; i++)
        resources_cancel_hook(st, (int)i);
}

void resources_auto_place(daemon_state_t *st, int epoll_fd)
{
    if (!has_quorum(st))
    {
        lcs_log_debug2("auto-place skip: quorum is not available (%u votes, need %u)", st->votes_seen, st->quorum_needed);
        return;
    }
    int target = first_online_full_member(st);
    if (target != st->self_index)
    {
        lcs_log_debug2("auto-place skip: selected online full-member is %s, self is %s",
                       node_name_or_none(st, target),
                       st->cfg.nodes[st->self_index].name);
        return;
    }
    for (size_t i = 0; i < st->cfg.vip_count; i++)
    {
        resource_runtime_t *res = &st->resources[i];
        if (res->owner_node >= 0)
        {
            lcs_log_debug2("auto-place skip VIP %s: owner is %s state=%u epoch=%llu",
                           st->cfg.vips[i].name,
                           node_name_or_none(st, res->owner_node),
                           (unsigned)res->state,
                           (unsigned long long)res->epoch);
            continue;
        }
        if (res->state == LCS_RES_CONFLICT)
        {
            lcs_log_debug2("auto-place skip VIP %s: resource is in conflict state epoch=%llu",
                           st->cfg.vips[i].name, (unsigned long long)res->epoch);
            continue;
        }
        if (lease_operation_active(st, (int)i))
        {
            lcs_log_debug2("auto-place skip VIP %s: lease operation already pending",
                           st->cfg.vips[i].name);
            continue;
        }
        lcs_log_debug2("auto-place try VIP %s on %s current_epoch=%llu next_epoch=%llu",
                       st->cfg.vips[i].name,
                       st->cfg.nodes[st->self_index].name,
                       (unsigned long long)res->epoch,
                       (unsigned long long)(res->epoch + 1));
        resources_activate_local(st, (int)i, res->epoch + 1, epoll_fd);
    }
}

void resources_maintain_owned_leases(daemon_state_t *st, int epoll_fd)
{
    uint64_t now = lcs_now_ms();
    for (size_t i = 0; i < st->cfg.vip_count; i++)
    {
        resource_runtime_t *res = &st->resources[i];
        if (res->owner_node != st->self_index ||
            res->owner_instance_id != st->instance_id ||
            (res->state != LCS_RES_ACTIVE &&
             res->state != LCS_RES_STARTING &&
             res->state != LCS_RES_STOPPING))
            continue;
        if (!has_quorum(st))
        {
            lcs_log_warn("dropping VIP %s because quorum is lost", st->cfg.vips[i].name);
            resources_release_local_internal(st, (int)i, epoll_fd, false);
            continue;
        }
        if (res->lease_deadline_ms && now >= res->lease_deadline_ms)
        {
            lcs_log_warn("dropping VIP %s because local lease expired",
                         st->cfg.vips[i].name);
            resources_release_local_internal(st, (int)i, epoll_fd, false);
            continue;
        }
        if (res->renew_after_ms && now < res->renew_after_ms)
            continue;
        if (lease_operation_active(st, (int)i))
        {
            lcs_log_debug3("renew VIP %s skipped: lease operation already pending",
                           st->cfg.vips[i].name);
            continue;
        }
        if (lease_start_renew(st, (int)i, epoll_fd) != 0)
        {
            if (now + st->cfg.renew_ms >= res->lease_deadline_ms)
            {
                lcs_log_warn("dropping VIP %s because lease renewal could not start",
                             st->cfg.vips[i].name);
                resources_release_local_internal(st, (int)i, epoll_fd, false);
            } else
                res->renew_after_ms = now + st->cfg.renew_ms;
        }
    }
}

void resources_process_hooks(daemon_state_t *st, int epoll_fd)
{
    uint64_t now = lcs_now_ms();
    for (size_t i = 0; i < st->cfg.vip_count; i++)
    {
        resource_runtime_t *res = &st->resources[i];
        if (res->hook_pid <= 0)
            continue;

        int status = 0;
        pid_t rc = waitpid(res->hook_pid, &status, WNOHANG);
        bool done = rc == res->hook_pid || rc < 0;
        bool ok = done && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (!done && res->hook_deadline_ms && now >= res->hook_deadline_ms)
        {
            lcs_log_warn("%s hook for VIP %s timed out; killing pid=%ld",
                         resources_hook_name(res->hook_type), st->cfg.vips[i].name,
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
                         resources_hook_name(res->hook_type), st->cfg.vips[i].name,
                         strerror(errno));
            ok = false;
        }

        resource_hook_type_t type = res->hook_type;
        uint64_t hook_epoch = res->hook_epoch;
        uint64_t hook_lease_id = res->hook_lease_id;
        lcs_log_info("%s hook for VIP %s completed status=%s", resources_hook_name(type), st->cfg.vips[i].name, ok ? "ok" : "failed");
        resources_clear_hook(res);

        if (type == LCS_HOOK_PRE_START)
        {
            bool still_current = res->owner_node == st->self_index &&
                                 res->owner_instance_id == st->instance_id &&
                                 res->state == LCS_RES_STARTING &&
                                 res->epoch == hook_epoch &&
                                 res->lease_id == hook_lease_id;
            if (!ok || !still_current)
            {
                lcs_log_warn("aborting VIP %s activation after pre-start hook status=%s current=%s",
                             st->cfg.vips[i].name, ok ? "ok" : "failed",
                             still_current ? "true" : "false");
                if (still_current)
                {
                    lease_release_majority(st, (int)i, st->self_index, hook_epoch, hook_lease_id, epoll_fd);
                    resources_clear_local_lease(res, hook_epoch);
                    res->next_activation_attempt_ms = now + st->cfg.lease_ms;
                }
                continue;
            }
            resources_complete_local_activation(st, (int)i, hook_epoch, hook_lease_id, epoll_fd);
        } else if (type == LCS_HOOK_PRE_STOP)
        {
            if (!ok)
            {
                lcs_log_warn("pre-stop hook for VIP %s failed; stopping VIP anyway", st->cfg.vips[i].name);
            }
            resources_release_local_internal(st, (int)i, epoll_fd, false);
            resources_start_hook(st, (int)i, LCS_HOOK_POST_STOP, hook_epoch + 1, hook_lease_id);
        } else if (!ok)
        {
            lcs_log_warn("%s hook for VIP %s failed after VIP event", resources_hook_name(type), st->cfg.vips[i].name);
        }
    }
}
