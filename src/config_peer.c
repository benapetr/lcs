// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_peer.h"
#include "config_reload.h"
#include "daemon_state.h"
#include "log.h"
#include "peer.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

_Static_assert(LCS_CONFIG_STATUS_DIGEST_SIZE == LCS_CONFIG_DIGEST_SIZE, "digest format mismatch");

static int withdraw_preparation(void)
{
    if (g_state.config_status.prepared && g_state.config_status.serial != UINT64_MAX)
    {
        g_state.config_status.prepared = 0;
        g_state.config_status.serial++;
    }
    return -1;
}

int config_peer_refresh(void)
{
    lcs_config_status_t next = g_state.config_status;
    bool changed = !next.serial;
    if (!next.serial || g_state.config_status_generation != g_state.config_reload.active_generation)
    {
        char error[256], *document = NULL;
        size_t length;
        lcs_config_value_t value = {0};
        if (lcs_config_agreement_membership(&g_state.cfg, next.membership) ||
            lcs_resource_config_encode(&g_state.cfg, 0, &document, &length, error, sizeof(error)))
            return withdraw_preparation();
        int result = lcs_config_value_init(&value, &g_state.cfg, "bootstrap", document,
                                           length, error, sizeof(error));
        free(document);
        if (result)
            return withdraw_preparation();
        changed = changed || strcmp(next.baseline, value.digest) != 0;
        memcpy(next.baseline, value.digest, sizeof(next.baseline));
        lcs_config_value_clear(&value);
    }
    bool prepared = g_state.config_bootstrap_initialized && g_state.config_bootstrap.prepared &&
        !config_reload_in_progress() && !g_state.config_runtime.requires_recovery;
    changed = changed || next.prepared != prepared;
    next.prepared = prepared;
    if (changed)
    {
        if (next.serial == UINT64_MAX)
            return withdraw_preparation();
        next.serial++;
    }
    g_state.config_status = next;
    g_state.config_status_generation = g_state.config_reload.active_generation;
    return 0;
}

void config_peer_process(int epoll_fd)
{
    if (lcs_now_ms() < g_state.config_status_retry_ms)
        return;
    if (config_peer_refresh())
    {
        lcs_log_error("cannot prepare configuration peer status; retrying without activation readiness");
        g_state.config_status_retry_ms = lcs_now_ms() + 1000u;
    }
    else
        g_state.config_status_retry_ms = 0;
    if (!g_state.config_status.serial)
        return;
    unsigned char payload[256];
    size_t length;
    if (lcs_config_status_encode(&g_state.config_status, payload, sizeof(payload), &length))
        return;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        peer_runtime_t *peer = &g_state.peers[i];
        if ((int)i == g_state.self_index || peer->config_status_sent == g_state.config_status.serial)
            continue;
        if (!peer_queue_config_status(epoll_fd, (int)i, payload, (uint32_t)length))
            peer->config_status_sent = g_state.config_status.serial;
    }
}

int config_peer_receive(int node, const void *payload, size_t length)
{
    if (node < 0 || (size_t)node >= g_state.cfg.node_count || node == g_state.self_index)
        return -1;
    peer_runtime_t *peer = &g_state.peers[node];
    lcs_config_status_t next;
    if (peer->fd < 0 || peer->conn_state != LCS_PEER_ESTABLISHED ||
        peer->protocol_version < LCS_PROTO_FEATURE_CONFIG_STATUS ||
        lcs_config_status_decode(payload, length, &next) ||
        strcmp(next.membership, g_state.config_status.membership))
        return -1;
    if (peer->config_status_known)
    {
        if (next.serial < peer->config_status.serial)
            return 0;
        if (next.serial == peer->config_status.serial)
            return lcs_config_status_equal(&next, &peer->config_status) ? 0 : -1;
    }
    peer->config_status = next;
    peer->config_status_known = true;
    lcs_log_debug("peer %s resource bootstrap prepared=%s matching=%s serial=%llu",
        g_state.cfg.nodes[node].name, next.prepared ? "true" : "false",
        !strcmp(next.baseline, g_state.config_status.baseline) ? "true" : "false",
        (unsigned long long)next.serial);
    return 0;
}

int config_peer_view(lcs_config_status_view_t *view)
{
    if (config_peer_refresh())
        return -1;
    memset(view, 0, sizeof(*view));
    view->count = (uint16_t)g_state.cfg.node_count;
    view->self = (uint16_t)g_state.self_index;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        lcs_config_status_node_t *node = &view->nodes[i];
        const peer_runtime_t *peer = &g_state.peers[i];
        memcpy(node->name, g_state.cfg.nodes[i].name, sizeof(node->name));
        node->role = (uint16_t)g_state.cfg.nodes[i].role;
        if ((int)i == g_state.self_index)
        {
            node->connected = node->known = 1;
            node->protocol = LCS_PEER_PROTO_VERSION;
            node->status = g_state.config_status;
        }
        else
        {
            node->connected = peer->online && peer->fd >= 0 && peer->conn_state == LCS_PEER_ESTABLISHED;
            node->protocol = node->connected ? peer->protocol_version : 0;
            node->known = node->connected && peer->protocol_version >= LCS_PROTO_FEATURE_CONFIG_STATUS &&
                          peer->config_status_known;
            if (node->known)
                node->status = peer->config_status;
        }
    }
    return 0;
}
