// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_peer.h"
#include "daemon_state.h"
#include "peer.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

daemon_state_t g_state;
static bool reload_busy;
static unsigned queued;

bool config_reload_in_progress(void) { return reload_busy; }
void lcs_log_debug(const char *fmt, ...) { (void)fmt; }
void lcs_log_error(const char *fmt, ...) { (void)fmt; }
int peer_queue_config_status(int epoll_fd, int node, const void *payload, uint32_t length)
{
    (void)epoll_fd;
    const peer_runtime_t *peer = &g_state.peers[node];
    if (peer->fd < 0 || peer->conn_state != LCS_PEER_ESTABLISHED ||
        peer->protocol_version < LCS_PROTO_FEATURE_CONFIG_STATUS)
        return 1;
    lcs_config_status_t status;
    assert(lcs_config_status_decode(payload, length, &status) == 0);
    assert(lcs_config_status_equal(&status, &g_state.config_status));
    queued++;
    return 0;
}

static void initialize(void)
{
    memset(&g_state, 0, sizeof(g_state));
    lcs_config_init_defaults(&g_state.cfg);
    strcpy(g_state.cfg.cluster_name, "config-status");
    strcpy(g_state.cfg.self_name, "node0");
    g_state.cfg.node_count = 3;
    for (size_t i = 0; i < 3; i++)
    {
        snprintf(g_state.cfg.nodes[i].name, sizeof(g_state.cfg.nodes[i].name), "node%zu", i);
        strcpy(g_state.cfg.nodes[i].address, "127.0.0.1");
        g_state.cfg.nodes[i].port = 3322;
        g_state.cfg.nodes[i].role = i == 2 ? LCS_NODE_QUORUM_ONLY : LCS_NODE_FULL;
        g_state.peers[i].fd = -1;
    }
    g_state.cfg.resource_count = 1;
    g_state.cfg.resources[0].type = LCS_RESOURCE_VIP;
    strcpy(g_state.cfg.resources[0].name, "vip");
    strcpy(g_state.cfg.resources[0].address, "192.0.2.10/32");
    strcpy(g_state.cfg.resources[0].interface, "eth0");
    char error[256];
    assert(lcs_config_finalize(&g_state.cfg, error, sizeof(error)) == 0);
    g_state.config_bootstrap_initialized = true;
    g_state.config_bootstrap.prepared = true;
    g_state.config_reload.active_generation = 1;
    g_state.resources[0].epoch = 123;
    g_state.lease_grants[0].promised_epoch = 456;
    assert(config_peer_refresh() == 0);
    assert(g_state.config_status.serial == 1 && g_state.config_status.prepared);
}

static void establish(size_t node, unsigned protocol)
{
    g_state.peers[node].fd = (int)node + 10;
    g_state.peers[node].conn_state = LCS_PEER_ESTABLISHED;
    g_state.peers[node].online = true;
    g_state.peers[node].protocol_version = protocol;
}

static void test_metadata_and_reload(void)
{
    initialize();
    establish(1, 6);
    establish(2, 5);
    config_peer_process(-1);
    assert(queued == 1 && g_state.peers[1].config_status_sent == 1);
    config_peer_process(-1);
    assert(queued == 1); /* Stable metadata does not flood healthy sessions. */
    unsigned char payload[256];
    size_t length;
    assert(lcs_config_status_encode(&g_state.config_status, payload, sizeof(payload), &length) == 0);
    assert(config_peer_receive(2, payload, length) == -1); /* Negotiated wire 5. */
    assert(config_peer_receive(1, payload, length) == 0);
    assert(config_peer_receive(1, payload, length) == 0); /* Exact retry. */
    lcs_config_status_view_t view;
    assert(config_peer_view(&view) == 0);
    assert(lcs_config_status_node_matches(&view, 0) && lcs_config_status_node_matches(&view, 1));
    assert(!view.nodes[2].known && !lcs_config_status_node_matches(&view, 2));
    lcs_config_status_t changed = g_state.config_status;
    changed.prepared = 0;
    assert(lcs_config_status_encode(&changed, payload, sizeof(payload), &length) == 0);
    assert(config_peer_receive(1, payload, length) == -1); /* Equal serial conflict. */
    changed.serial++;
    assert(lcs_config_status_encode(&changed, payload, sizeof(payload), &length) == 0);
    assert(config_peer_receive(1, payload, length) == 0);
    assert(!g_state.peers[1].config_status.prepared);
    changed = g_state.config_status;
    assert(lcs_config_status_encode(&changed, payload, sizeof(payload), &length) == 0);
    assert(config_peer_receive(1, payload, length) == 0); /* Ignore older metadata. */
    assert(!g_state.peers[1].config_status.prepared);
    changed.serial += 2;
    changed.membership[0] = changed.membership[0] == 'a' ? 'b' : 'a';
    assert(lcs_config_status_encode(&changed, payload, sizeof(payload), &length) == 0);
    assert(config_peer_receive(1, payload, length) == -1);
    reload_busy = true;
    assert(config_peer_refresh() == 0 && !g_state.config_status.prepared);
    g_state.config_bootstrap.prepared = false;
    reload_busy = false;
    strcpy(g_state.cfg.resources[0].address, "192.0.2.20/32");
    g_state.config_reload.active_generation++;
    assert(config_peer_refresh() == 0 && !g_state.config_status.prepared);
    char digest[65];
    strcpy(digest, g_state.config_status.baseline);
    assert(strcmp(digest, view.nodes[0].status.baseline));
    g_state.config_bootstrap.prepared = true;
    assert(config_peer_refresh() == 0 && g_state.config_status.prepared);
    assert(!strcmp(g_state.config_status.baseline, digest));
    assert(lcs_config_status_encode(&g_state.config_status, payload, sizeof(payload), &length) == 0);
    assert(config_peer_receive(1, payload, length) == 0);
    establish(2, 6);
    assert(config_peer_receive(2, payload, length) == 0);
    assert(config_peer_view(&view) == 0 && view.nodes[2].role == LCS_NODE_QUORUM_ONLY);
    for (size_t i = 0; i < 3; i++)
        assert(lcs_config_status_node_matches(&view, i));
    g_state.peers[1].conn_state = LCS_PEER_DISCONNECTED;
    assert(config_peer_view(&view) == 0 && !view.nodes[1].known && !view.nodes[1].protocol);
    assert(!lcs_config_status_node_matches(&view, 1));
    /* A failed cache rebuild must withdraw the old prepared advertisement. */
    g_state.config_reload.active_generation++;
    g_state.cfg.resource_count = LCS_MAX_RESOURCES + 1;
    assert(config_peer_refresh() == -1 && !g_state.config_status.prepared);
    g_state.cfg.resource_count = 1;
    assert(config_peer_refresh() == 0 && g_state.config_status.prepared);
    assert(g_state.resources[0].epoch == 123 && g_state.lease_grants[0].promised_epoch == 456);
}

static void test_strict_codecs(void)
{
    initialize();
    unsigned char payload[8192], saved[8192];
    size_t length;
    lcs_config_status_t decoded;
    assert(lcs_config_status_encode(&g_state.config_status, payload, sizeof(payload), &length) == 0);
    for (size_t i = 0; i < length; i++)
        assert(lcs_config_status_decode(payload, i, &decoded) == -1);
    assert(lcs_config_status_decode(payload, length, &decoded) == 0);
    assert(lcs_config_status_equal(&decoded, &g_state.config_status));
    memcpy(saved, payload, length);
    payload[length] = 0;
    assert(lcs_config_status_decode(payload, length + 1, &decoded) == -1);
    payload[1] = 2;
    assert(lcs_config_status_decode(payload, length, &decoded) == -1);
    memcpy(payload, saved, length);
    payload[10] = 2; /* prepared must be boolean. */
    assert(lcs_config_status_decode(payload, length, &decoded) == -1);
    memcpy(payload, saved, length);
    payload[11] = 'A'; /* Canonical lowercase SHA-256 only. */
    assert(lcs_config_status_decode(payload, length, &decoded) == -1);
    memcpy(payload, saved, length);
    payload[75] = 'a'; /* Missing fixed-string terminator. */
    assert(lcs_config_status_decode(payload, length, &decoded) == -1);
    lcs_config_status_view_t view, roundtrip;
    assert(config_peer_view(&view) == 0);
    assert(lcs_config_status_view_encode(&view, payload, sizeof(payload), &length) == 0);
    assert(lcs_config_status_view_decode(payload, length, &roundtrip) == 0);
    assert(roundtrip.count == 3 && roundtrip.self == 0 && roundtrip.nodes[0].known);
    for (size_t i = 0; i < length; i++)
        assert(lcs_config_status_view_decode(payload, i, &roundtrip) == -1);
    memcpy(saved, payload, length);
    payload[6 + 6] = 'x'; /* Nonzero padding after node0's terminator. */
    assert(lcs_config_status_view_decode(payload, length, &roundtrip) == -1);
    memcpy(payload, saved, length);
    payload[length] = 0;
    assert(lcs_config_status_view_decode(payload, length + 1, &roundtrip) == -1);
    view.nodes[1].known = 1;
    assert(lcs_config_status_view_encode(&view, payload, sizeof(payload), &length) == -1);
    view.nodes[1].known = 0;
    strcpy(view.nodes[1].name, view.nodes[0].name);
    assert(lcs_config_status_view_encode(&view, payload, sizeof(payload), &length) == -1);
}

int main(void)
{
    test_metadata_and_reload();
    test_strict_codecs();
    puts("configuration peer metadata unit tests passed");
    return 0;
}
