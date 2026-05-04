// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "lease.h"

#include "cluster.h"
#include "log.h"
#include "peer.h"
#include "protocol.h"
#include "util.h"
#include "vip.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

int encode_lease_msg(unsigned char *payload, size_t cap, size_t *len,
                     uint16_t resource_id, uint16_t owner_node,
                     uint64_t epoch, uint64_t lease_id, uint32_t lease_ms,
                     uint64_t sender_instance_id)
{
    lcs_buf_writer_t w;
    lcs_buf_writer_init(&w, payload, cap);
    if (lcs_buf_put_u16(&w, resource_id) != 0 ||
        lcs_buf_put_u16(&w, owner_node) != 0 ||
        lcs_buf_put_u64(&w, epoch) != 0 ||
        lcs_buf_put_u64(&w, lease_id) != 0 ||
        lcs_buf_put_u32(&w, lease_ms) != 0 ||
        lcs_buf_put_u64(&w, sender_instance_id) != 0)
        return -1;
    *len = w.len;
    return 0;
}

int decode_lease_msg(const daemon_state_t *st, const void *payload, size_t len,
                     uint16_t *resource_id, uint16_t *owner_node,
                     uint64_t *epoch, uint64_t *lease_id, uint32_t *lease_ms,
                     uint64_t *sender_instance_id)
{
    lcs_buf_reader_t r;
    lcs_buf_reader_init(&r, payload, len);
    if (lcs_buf_get_u16(&r, resource_id) != 0 ||
        lcs_buf_get_u16(&r, owner_node) != 0 ||
        lcs_buf_get_u64(&r, epoch) != 0 ||
        lcs_buf_get_u64(&r, lease_id) != 0 ||
        lcs_buf_get_u32(&r, lease_ms) != 0 ||
        lcs_buf_get_u64(&r, sender_instance_id) != 0 ||
        *resource_id >= st->cfg.vip_count ||
        *owner_node >= st->cfg.node_count ||
        st->cfg.nodes[*owner_node].role != LCS_NODE_FULL)
        return -1;
    return 0;
}

int accept_lease_message(daemon_state_t *st, uint16_t type, const void *payload,
                         size_t len, int source_node_idx)
{
    uint16_t resource_id, owner_node;
    uint64_t epoch, lease_id, sender_instance_id;
    uint32_t lease_ms;
    if (decode_lease_msg(st, payload, len, &resource_id, &owner_node, &epoch,
                         &lease_id, &lease_ms, &sender_instance_id) != 0)
    {
        lcs_log_debug("rejecting lease message type=%u from %s: invalid payload length=%zu",
                      type, node_name_or_none(st, source_node_idx), len);
        return -1;
    }
    if (source_node_idx >= 0 &&
        ((size_t)source_node_idx >= st->cfg.node_count ||
         sender_instance_id != st->peers[source_node_idx].instance_id))
    {
        lcs_log_debug("rejecting lease message type=%u from %s: instance mismatch sender=%llu expected=%llu",
                      type, node_name_or_none(st, source_node_idx),
                      (unsigned long long)sender_instance_id,
                      source_node_idx >= 0 && (size_t)source_node_idx < st->cfg.node_count ?
                      (unsigned long long)st->peers[source_node_idx].instance_id : 0ull);
        return -1;
    }
    resource_runtime_t *res = &st->resources[resource_id];
    if (res->state == LCS_RES_CONFLICT)
    {
        lcs_log_debug("rejecting lease message type=%u for VIP %s from %s: local conflict state",
                      type, st->cfg.vips[resource_id].name, node_name_or_none(st, source_node_idx));
        return -1;
    }
    if (type == LCS_MSG_LEASE_RELEASE)
    {
        if (epoch < res->epoch)
        {
            lcs_log_debug("rejecting lease release for VIP %s from %s: stale epoch=%llu local_epoch=%llu",
                          st->cfg.vips[resource_id].name, node_name_or_none(st, source_node_idx),
                          (unsigned long long)epoch, (unsigned long long)res->epoch);
            return -1;
        }
        if (epoch == res->epoch && res->lease_id != 0 && lease_id != res->lease_id)
        {
            lcs_log_debug("rejecting lease release for VIP %s from %s: lease_id mismatch got=%llu local=%llu",
                          st->cfg.vips[resource_id].name, node_name_or_none(st, source_node_idx),
                          (unsigned long long)lease_id, (unsigned long long)res->lease_id);
            return -1;
        }
        if (epoch == res->epoch && res->owner_instance_id != 0 &&
            sender_instance_id != res->owner_instance_id)
        {
            lcs_log_debug("rejecting lease release for VIP %s from %s: owner instance mismatch got=%llu local=%llu",
                          st->cfg.vips[resource_id].name, node_name_or_none(st, source_node_idx),
                          (unsigned long long)sender_instance_id,
                          (unsigned long long)res->owner_instance_id);
            return -1;
        }
        if (res->owner_node == (int)owner_node || epoch > res->epoch)
        {
            if (res->owner_node == st->self_index &&
                res->owner_instance_id == st->instance_id &&
                res->state == LCS_RES_ACTIVE)
                lcs_vip_del(&st->cfg.vips[resource_id]);
            res->epoch = epoch;
            res->lease_id = 0;
            res->owner_node = -1;
            res->owner_instance_id = 0;
            res->state = LCS_RES_STOPPED;
            res->lease_deadline_ms = 0;
            res->renew_after_ms = 0;
            res->conflict_reason[0] = '\0';
        }
        return 0;
    }
    if (type == LCS_MSG_LEASE_RENEW)
    {
        if (epoch != res->epoch || lease_id != res->lease_id ||
            res->owner_node != (int)owner_node ||
            sender_instance_id != res->owner_instance_id)
        {
            lcs_log_debug("rejecting lease renew for VIP %s from %s: got epoch=%llu lease=%llu owner=%s instance=%llu local epoch=%llu lease=%llu owner=%s instance=%llu",
                          st->cfg.vips[resource_id].name, node_name_or_none(st, source_node_idx),
                          (unsigned long long)epoch, (unsigned long long)lease_id,
                          node_name_or_none(st, owner_node),
                          (unsigned long long)sender_instance_id,
                          (unsigned long long)res->epoch, (unsigned long long)res->lease_id,
                          node_name_or_none(st, res->owner_node),
                          (unsigned long long)res->owner_instance_id);
            return -1;
        }
    } else if (type == LCS_MSG_LEASE_REQ)
    {
        if (epoch < res->epoch)
        {
            lcs_log_debug("rejecting lease request for VIP %s from %s: stale epoch=%llu local_epoch=%llu",
                          st->cfg.vips[resource_id].name, node_name_or_none(st, source_node_idx),
                          (unsigned long long)epoch, (unsigned long long)res->epoch);
            return -1;
        }
        if (epoch == res->epoch && res->owner_node >= 0 &&
            (res->owner_node != (int)owner_node || res->lease_id != lease_id ||
             res->owner_instance_id != sender_instance_id))
        {
            lcs_log_debug("rejecting lease request for VIP %s from %s: epoch collision got owner=%s lease=%llu instance=%llu local owner=%s lease=%llu instance=%llu",
                          st->cfg.vips[resource_id].name, node_name_or_none(st, source_node_idx),
                          node_name_or_none(st, owner_node), (unsigned long long)lease_id,
                          (unsigned long long)sender_instance_id,
                          node_name_or_none(st, res->owner_node), (unsigned long long)res->lease_id,
                          (unsigned long long)res->owner_instance_id);
            return -1;
        }
    } else
        return -1;
    if (res->owner_node == st->self_index &&
        res->owner_instance_id == st->instance_id &&
        owner_node != (uint16_t)st->self_index &&
        res->state == LCS_RES_ACTIVE)
        lcs_vip_del(&st->cfg.vips[resource_id]);
    uint64_t now = lcs_now_ms();
    res->epoch = epoch;
    res->lease_id = lease_id;
    res->owner_node = owner_node;
    res->owner_instance_id = sender_instance_id;
    if (type != LCS_MSG_LEASE_RENEW ||
        (res->state != LCS_RES_STARTING && res->state != LCS_RES_STOPPING))
        res->state = LCS_RES_ACTIVE;
    res->lease_deadline_ms = now + lease_ms;
    res->renew_after_ms = now + (lease_ms / 2u);
    res->conflict_reason[0] = '\0';
    return 0;
}

int send_peer_lease(daemon_state_t *st, int node_idx, uint16_t type,
                    int vip_idx, int owner_idx, uint64_t epoch, uint64_t lease_id,
                    int epoll_fd)
{
    unsigned char req[LCS_MAX_FRAME];
    unsigned char resp[LCS_MAX_FRAME];
    size_t req_len = 0;
    uint32_t resp_len = 0;
    int32_t status = -1;
    char _msg[128];
    if (encode_lease_msg(req, sizeof(req), &req_len, (uint16_t)vip_idx,
                         (uint16_t)owner_idx, epoch, lease_id, st->cfg.lease_ms,
                         st->instance_id) != 0 ||
        peer_rpc(st, epoll_fd, node_idx, type, req, (uint32_t)req_len, LCS_MSG_LEASE_ACK,
                 resp, sizeof(resp), &resp_len, st->cfg.peer_timeout_ms) != 0 ||
        lcs_decode_simple_resp(resp, resp_len, &status, _msg, sizeof(_msg)) != 0)
    {
        lcs_log_debug("lease message type=%u VIP=%s epoch=%llu lease=%llu to %s failed before status decode",
                      type, st->cfg.vips[vip_idx].name, (unsigned long long)epoch,
                      (unsigned long long)lease_id, st->cfg.nodes[node_idx].name);
        return -1;
    }
    if (status != 0)
    {
        lcs_log_debug("lease message type=%u VIP=%s epoch=%llu lease=%llu to %s returned status=%d",
                      type, st->cfg.vips[vip_idx].name, (unsigned long long)epoch,
                      (unsigned long long)lease_id, st->cfg.nodes[node_idx].name, status);
    }
    return status == 0 ? 0 : -1;
}

int acquire_majority_lease(daemon_state_t *st, int vip_idx, int owner_idx,
                           uint64_t epoch, uint64_t lease_id, int epoll_fd)
{
    if (!has_quorum(st))
        return -1;
    int votes = 1;
    bool acked[LCS_MAX_NODES] = {0};
    for (size_t i = 0; i < st->cfg.node_count; i++)
    {
        if ((int)i == st->self_index)
            continue;
        if (send_peer_lease(st, (int)i, LCS_MSG_LEASE_REQ, vip_idx, owner_idx,
                            epoch, lease_id, epoll_fd) == 0)
        {
            acked[i] = true;
            votes++;
            lcs_log_debug("lease request for VIP %s epoch=%llu acked by %s",
                          st->cfg.vips[vip_idx].name, (unsigned long long)epoch,
                          st->cfg.nodes[i].name);
        } else
        {
            lcs_log_debug("lease request for VIP %s epoch=%llu rejected or failed by %s",
                          st->cfg.vips[vip_idx].name, (unsigned long long)epoch,
                          st->cfg.nodes[i].name);
        }
    }
    if ((uint32_t)votes >= st->quorum_needed)
    {
        resource_runtime_t *res = &st->resources[vip_idx];
        uint64_t now = lcs_now_ms();
        res->epoch = epoch;
        res->lease_id = lease_id;
        res->owner_node = owner_idx;
        res->owner_instance_id = st->instance_id;
        res->state = LCS_RES_ACTIVE;
        res->lease_deadline_ms = now + st->cfg.lease_ms;
        res->renew_after_ms = now + st->cfg.renew_ms;
        res->conflict_reason[0] = '\0';
        lcs_log_debug("lease acquired for VIP %s epoch=%llu votes=%d need=%u",
                      st->cfg.vips[vip_idx].name, (unsigned long long)epoch,
                      votes, st->quorum_needed);
        return 0;
    }
    lcs_log_debug("lease acquire failed for VIP %s epoch=%llu votes=%d need=%u",
                  st->cfg.vips[vip_idx].name, (unsigned long long)epoch,
                  votes, st->quorum_needed);
    for (size_t i = 0; i < st->cfg.node_count; i++)
    {
        if (acked[i])
            send_peer_lease(st, (int)i, LCS_MSG_LEASE_RELEASE, vip_idx, owner_idx,
                            epoch, lease_id, epoll_fd);
    }
    return -1;
}

void release_majority_lease(daemon_state_t *st, int vip_idx, int owner_idx,
                             uint64_t epoch, uint64_t lease_id, int epoll_fd)
{
    for (size_t i = 0; i < st->cfg.node_count; i++)
    {
        if ((int)i == st->self_index)
            continue;
        send_peer_lease(st, (int)i, LCS_MSG_LEASE_RELEASE, vip_idx, owner_idx,
                        epoch, lease_id, epoll_fd);
    }
}

int handle_owner_release_request(daemon_state_t *st, const void *payload, size_t len,
                                 int source_node_idx)
{
    uint16_t resource_id, owner_node;
    uint64_t epoch, lease_id, sender_instance_id;
    uint32_t lease_ms;
    if (decode_lease_msg(st, payload, len, &resource_id, &owner_node, &epoch,
                         &lease_id, &lease_ms, &sender_instance_id) != 0)
        return -1;

    (void)lease_ms;
    if (source_node_idx >= 0 && ((size_t)source_node_idx >= st->cfg.node_count ||
                                  sender_instance_id != st->peers[source_node_idx].instance_id))
        return -1;

    if (owner_node != (uint16_t)st->self_index)
        return -1;

    resource_runtime_t *res = &st->resources[resource_id];
    if (res->owner_node == -1 && res->state == LCS_RES_STOPPED && res->epoch >= epoch)
        return 0;

    if (res->owner_node != st->self_index ||
        res->owner_instance_id != st->instance_id ||
        res->state != LCS_RES_ACTIVE ||
        res->epoch != epoch ||
        res->lease_id != lease_id)
        return -1;

    if (lcs_vip_del(&st->cfg.vips[resource_id]) != 0)
        return -1;

    res->owner_node = -1;
    res->owner_instance_id = 0;
    res->state = LCS_RES_STOPPED;
    res->lease_id = 0;
    res->lease_deadline_ms = 0;
    res->renew_after_ms = 0;
    res->conflict_reason[0] = '\0';
    res->next_activation_attempt_ms = lcs_now_ms() + st->cfg.lease_ms;
    lcs_log_info("released VIP %s for controlled handoff at epoch=%llu",
                 st->cfg.vips[resource_id].name, (unsigned long long)epoch);
    return 0;
}

int request_old_owner_release(daemon_state_t *st, int old_owner_idx, int vip_idx,
                               uint64_t epoch, uint64_t lease_id, int epoll_fd)
{
    unsigned char req[LCS_MAX_FRAME];
    unsigned char resp[LCS_MAX_FRAME];
    size_t req_len = 0;
    uint32_t resp_len = 0;
    int32_t status = -1;
    char _msg[128];
    if (encode_lease_msg(req, sizeof(req), &req_len, (uint16_t)vip_idx,
                         (uint16_t)old_owner_idx, epoch, lease_id, 0,
                         st->instance_id) != 0 ||
        peer_rpc(st, epoll_fd, old_owner_idx, LCS_MSG_OWNER_RELEASE_REQ,
                 req, (uint32_t)req_len, LCS_MSG_OWNER_RELEASE_RESP,
                 resp, sizeof(resp), &resp_len, st->cfg.peer_timeout_ms) != 0 ||
        lcs_decode_simple_resp(resp, resp_len, &status, _msg, sizeof(_msg)) != 0)
        return -1;
    return status == 0 ? 0 : -1;
}

int wait_for_old_lease_expiry(daemon_state_t *st, int vip_idx)
{
    resource_runtime_t *res = &st->resources[vip_idx];
    if (!res->lease_deadline_ms)
        return -1;

    uint64_t now = lcs_now_ms();
    while (now < res->lease_deadline_ms)
    {
        uint64_t remaining_ms = res->lease_deadline_ms - now;
        useconds_t sleep_us = (useconds_t)((remaining_ms > 100 ? 100 : remaining_ms) * 1000u);
        usleep(sleep_us);
        now = lcs_now_ms();
    }
    return 0;
}

int prepare_controlled_handoff(daemon_state_t *st, int vip_idx, int epoll_fd)
{
    resource_runtime_t *res = &st->resources[vip_idx];
    int old_owner = res->owner_node;
    if (old_owner < 0 || old_owner == st->self_index)
        return 0;

    uint64_t old_epoch = res->epoch;
    uint64_t old_lease_id = res->lease_id;
    if (node_is_online(st, (size_t)old_owner) &&
        request_old_owner_release(st, old_owner, vip_idx, old_epoch, old_lease_id, epoll_fd) == 0)
    {
        lcs_log_info("old owner %s confirmed release of VIP %s",
                     st->cfg.nodes[old_owner].name, st->cfg.vips[vip_idx].name);
        return 0;
    }
    lcs_log_info("old owner %s did not confirm release of VIP %s; waiting for lease expiry",
                 node_name_or_none(st, old_owner), st->cfg.vips[vip_idx].name);
    if (wait_for_old_lease_expiry(st, vip_idx) != 0)
        return -1;
    return 0;
}

void expire_remote_leases(daemon_state_t *st)
{
    uint64_t now = lcs_now_ms();
    uint64_t grace_ms = st->cfg.renew_ms ? st->cfg.renew_ms : 1000u;
    for (size_t i = 0; i < st->cfg.vip_count; i++)
    {
        resource_runtime_t *res = &st->resources[i];
        if ((res->state != LCS_RES_ACTIVE &&
             res->state != LCS_RES_STARTING &&
             res->state != LCS_RES_STOPPING) ||
            res->owner_node < 0 ||
            (res->owner_node == st->self_index &&
             res->owner_instance_id == st->instance_id))
            continue;
        if (res->lease_deadline_ms && now < res->lease_deadline_ms + grace_ms)
            continue;
        lcs_log_warn("clearing expired remote lease for VIP %s owner=%s epoch=%llu expired_ms=%llu",
                     st->cfg.vips[i].name,
                     node_name_or_none(st, res->owner_node),
                     (unsigned long long)res->epoch,
                     res->lease_deadline_ms && now > res->lease_deadline_ms ?
                     (unsigned long long)(now - res->lease_deadline_ms) : 0ull);
        res->owner_node = -1;
        res->owner_instance_id = 0;
        res->state = LCS_RES_STOPPED;
        res->lease_id = 0;
        res->lease_deadline_ms = 0;
        res->renew_after_ms = 0;
        res->failover_pending = true;
        res->conflict_reason[0] = '\0';
    }
}
