// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "group.h"

#include "cluster.h"
#include "daemon_state.h"
#include "lease.h"
#include "log.h"
#include "move.h"

static int group_normal_target(void)
{
    return cluster_first_online_full_member();
}

static bool group_same(int vip_idx, int other_idx)
{
    if (vip_idx < 0 || other_idx < 0 || (size_t)vip_idx >= g_state.cfg.vip_count || (size_t)other_idx >= g_state.cfg.vip_count)
        return false;

    int group_idx = g_state.cfg.vips[vip_idx].group_idx;
    return group_idx >= 0 && group_idx == g_state.cfg.vips[other_idx].group_idx;
}

static bool group_has_waiting_higher_priority_vip(int vip_idx)
{
    const lcs_vip_config_t *vip = &g_state.cfg.vips[vip_idx];
    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
    {
        if ((int)i == vip_idx || !group_same(vip_idx, (int)i))
            continue;

        const lcs_vip_config_t *other_vip = &g_state.cfg.vips[i];
        const resource_runtime_t *other_res = &g_state.resources[i];
        if (other_vip->priority <= vip->priority)
            continue;
        if (other_res->owner_node >= 0 || other_res->state == LCS_RES_CONFLICT)
            continue;
        return true;
    }
    return false;
}

static int group_active_owner(int vip_idx)
{
    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
    {
        if (!group_same(vip_idx, (int)i))
            continue;

        const resource_runtime_t *res = &g_state.resources[i];
        if (res->owner_node >= 0)
            return res->owner_node;
    }
    return -1;
}

static bool group_node_hosts_member(int vip_idx, int node_idx)
{
    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
    {
        if ((int)i == vip_idx || !group_same(vip_idx, (int)i))
            continue;

        const resource_runtime_t *res = &g_state.resources[i];
        if (res->owner_node == node_idx)
            return true;
    }
    return false;
}

static bool group_vip_at_unblocked_home(int vip_idx)
{
    const lcs_vip_config_t *vip = &g_state.cfg.vips[vip_idx];
    const resource_runtime_t *res = &g_state.resources[vip_idx];
    return vip->home_node_idx >= 0 &&
           !res->home_blocked &&
           res->owner_node == vip->home_node_idx &&
           cluster_node_is_online((size_t)vip->home_node_idx);
}

static int group_first_unused_online_full_member(int vip_idx)
{
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if (g_state.cfg.nodes[i].role != LCS_NODE_FULL || !cluster_node_is_online(i))
            continue;
        if (!group_node_hosts_member(vip_idx, (int)i))
            return (int)i;
    }
    return -1;
}

static int group_keep_together_target(int vip_idx)
{
    int owner = group_active_owner(vip_idx);
    if (owner >= 0)
        return owner;
    return group_normal_target();
}

static int group_anti_affinity_target(int vip_idx)
{
    const lcs_vip_config_t *vip = &g_state.cfg.vips[vip_idx];
    const lcs_group_config_t *group = &g_state.cfg.groups[vip->group_idx];
    int unused = group_first_unused_online_full_member(vip_idx);
    if (unused >= 0)
        return unused;
    if (group->mode == LCS_GROUP_MODE_STRICT)
        return -1;
    return group_normal_target();
}

int group_auto_place_target(int vip_idx)
{
    if (vip_idx < 0 || (size_t)vip_idx >= g_state.cfg.vip_count)
        return -1;

    const lcs_vip_config_t *vip = &g_state.cfg.vips[vip_idx];
    const resource_runtime_t *res = &g_state.resources[vip_idx];
    if (vip->home_node_idx >= 0 &&
        !res->home_blocked &&
        cluster_node_is_online((size_t)vip->home_node_idx))
        return vip->home_node_idx;

    if (vip->group_idx < 0)
        return group_normal_target();

    if (group_has_waiting_higher_priority_vip(vip_idx))
        return -1;

    const lcs_group_config_t *group = &g_state.cfg.groups[vip->group_idx];
    switch (group->type)
    {
        case LCS_GROUP_KEEP_TOGETHER:
            return group_keep_together_target(vip_idx);
        case LCS_GROUP_ANTI_AFFINITY:
            return group_anti_affinity_target(vip_idx);
        default:
            return group_normal_target();
    }
}

int group_move_anchor_vip(int vip_idx)
{
    if (vip_idx < 0 || (size_t)vip_idx >= g_state.cfg.vip_count)
        return vip_idx;

    const lcs_vip_config_t *vip = &g_state.cfg.vips[vip_idx];
    if (vip->group_idx < 0)
        return vip_idx;

    const lcs_group_config_t *group = &g_state.cfg.groups[vip->group_idx];
    if (group->type != LCS_GROUP_KEEP_TOGETHER)
        return vip_idx;

    int anchor = vip_idx;
    uint32_t anchor_priority = vip->priority;
    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
    {
        const lcs_vip_config_t *candidate = &g_state.cfg.vips[i];
        if (candidate->group_idx != vip->group_idx)
            continue;
        if (candidate->priority > anchor_priority)
        {
            anchor = (int)i;
            anchor_priority = candidate->priority;
        }
    }
    return anchor;
}

static size_t group_member_count(int group_idx)
{
    size_t count = 0;
    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
    {
        if (g_state.cfg.vips[i].group_idx == group_idx)
            count++;
    }
    return count;
}

static size_t group_full_member_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if (g_state.cfg.nodes[i].role == LCS_NODE_FULL)
            count++;
    }
    return count;
}

void group_log_strict_warnings(void)
{
    size_t full_members = group_full_member_count();
    for (size_t i = 0; i < g_state.cfg.group_count; i++)
    {
        const lcs_group_config_t *group = &g_state.cfg.groups[i];
        if (group->mode != LCS_GROUP_MODE_STRICT)
            continue;

        size_t members = group_member_count((int)i);
        if (members == 0)
            continue;

        if (full_members == 0)
        {
            lcs_log_warn("strict group %s has %zu VIPs but no full-member nodes; VIPs cannot be placed", group->name, members);
            continue;
        }

        if (group->type == LCS_GROUP_ANTI_AFFINITY && members > full_members)
        {
            lcs_log_warn("strict anti-affinity group %s has %zu VIPs but only %zu full-member nodes; lower-priority VIPs may remain stopped",
                         group->name, members, full_members);
        }
    }
}

static int group_active_member_count_on_node(int group_idx, int node_idx)
{
    int count = 0;
    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
    {
        const lcs_vip_config_t *vip = &g_state.cfg.vips[i];
        const resource_runtime_t *res = &g_state.resources[i];
        if (vip->group_idx == group_idx &&
            res->owner_node == node_idx &&
            res->state == LCS_RES_ACTIVE)
            count++;
    }
    return count;
}

static int group_keep_together_rebalance(int epoll_fd, int group_idx)
{
    int target = -1;
    uint32_t target_priority = 0;
    int candidate = -1;
    uint32_t candidate_priority = 0;

    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
    {
        const lcs_vip_config_t *vip = &g_state.cfg.vips[i];
        const resource_runtime_t *res = &g_state.resources[i];
        if (vip->group_idx != group_idx ||
            res->state != LCS_RES_ACTIVE ||
            res->owner_node < 0)
            continue;

        if (target < 0 || vip->priority > target_priority)
        {
            target = res->owner_node;
            target_priority = vip->priority;
        }
    }

    if (target < 0 || !cluster_node_is_online((size_t)target))
        return 0;

    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
    {
        const lcs_vip_config_t *vip = &g_state.cfg.vips[i];
        const resource_runtime_t *res = &g_state.resources[i];
        if (vip->group_idx != group_idx ||
            res->state != LCS_RES_ACTIVE ||
            res->owner_node < 0 ||
            res->owner_node == target ||
            group_vip_at_unblocked_home((int)i))
            continue;

        if (candidate < 0 || vip->priority < candidate_priority)
        {
            candidate = (int)i;
            candidate_priority = vip->priority;
        }
    }

    if (candidate < 0)
        return 0;
    return move_start_internal(epoll_fd, candidate, target, "group keep-together rebalance");
}

static int group_anti_affinity_rebalance(int epoll_fd, int group_idx)
{
    int candidate = -1;
    uint32_t candidate_priority = 0;
    int target = -1;

    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
    {
        const lcs_vip_config_t *vip = &g_state.cfg.vips[i];
        const resource_runtime_t *res = &g_state.resources[i];
        if (vip->group_idx != group_idx ||
            res->state != LCS_RES_ACTIVE ||
            res->owner_node < 0 ||
            group_vip_at_unblocked_home((int)i) ||
            group_active_member_count_on_node(group_idx, res->owner_node) < 2)
            continue;

        int unused = group_first_unused_online_full_member((int)i);
        if (unused < 0)
            continue;

        if (candidate < 0 || vip->priority < candidate_priority)
        {
            candidate = (int)i;
            candidate_priority = vip->priority;
            target = unused;
        }
    }

    if (candidate < 0 || target < 0)
        return 0;
    return move_start_internal(epoll_fd, candidate, target, "group anti-affinity rebalance");
}

void group_rebalance(int epoll_fd)
{
    if (!cluster_has_quorum() || move_any_active())
        return;

    int coordinator = cluster_first_online_full_member();
    if (coordinator != g_state.self_index)
        return;

    for (size_t i = 0; i < g_state.cfg.group_count; i++)
    {
        const lcs_group_config_t *group = &g_state.cfg.groups[i];
        int rc = 0;
        if (group->type == LCS_GROUP_KEEP_TOGETHER)
            rc = group_keep_together_rebalance(epoll_fd, (int)i);
        else if (group->type == LCS_GROUP_ANTI_AFFINITY)
            rc = group_anti_affinity_rebalance(epoll_fd, (int)i);

        if (rc != 0 || move_any_active())
            return;
    }
}
