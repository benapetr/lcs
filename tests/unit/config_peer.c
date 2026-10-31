// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_peer.h"
#include "daemon_state.h"
#include "peer.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "util.h"

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

static lcs_config_readiness_message_t last_ready[LCS_MAX_NODES];
static unsigned readiness_queued;
int peer_queue_config_readiness(int epoll_fd, int node, uint16_t type, const void *payload, uint32_t length)
{
    (void)epoll_fd;
    assert(node >= 0 && (size_t)node < g_state.cfg.node_count);
    const peer_runtime_t *peer = &g_state.peers[node];
    if (peer->fd < 0 || peer->conn_state != LCS_PEER_ESTABLISHED ||
        peer->protocol_version < LCS_PROTO_FEATURE_CONFIG_READINESS)
        return 1;
    assert(!lcs_config_readiness_decode(payload, length, &last_ready[node]));
    assert(last_ready[node].kind == (type == LCS_MSG_CONFIG_READINESS_QUERY ? LCS_READINESS_QUERY : LCS_READINESS_REPLY));
    readiness_queued++;
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

static lcs_config_store_t readiness_store;
static char readiness_directory[80];

static void prepare_durable_runtime(void)
{
    char error[256];
    strcpy(readiness_directory, "/tmp/lcs-ready-peer.XXXXXX");
    assert(mkdtemp(readiness_directory));
    assert(!lcs_config_store_open(&readiness_store, readiness_directory, error, sizeof(error)));
    lcs_config_runtime_t *runtime = &g_state.config_runtime;
    runtime->base = malloc(sizeof(*runtime->base));
    assert(runtime->base);
    assert(!lcs_config_activation_open(&runtime->activation, &readiness_store, &g_state.cfg,
                                        runtime->base, true, error, sizeof(error)));
    assert(!lcs_config_activation_resume(&runtime->activation, &runtime->agreement,
                                          &runtime->application, error, sizeof(error)));
    runtime->requires_recovery = true;
    g_state.config_bootstrap.initialization_locked = true;
    assert(!config_peer_refresh());
    assert(!g_state.config_status.prepared); /* Diagnostic preparation is withdrawn. */
}

static void cleanup_durable_runtime(void)
{
    lcs_config_runtime_close(&g_state.config_runtime);
    const char *names[] = {"activation.json", "agreement.json", "application.json", "lock"};
    assert(faccessat(readiness_store.directory_fd, "barrier.json", F_OK, 0) == -1);
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        assert(!unlinkat(readiness_store.directory_fd, names[i], 0));
    lcs_config_store_close(&readiness_store);
    assert(!rmdir(readiness_directory));
}

static lcs_config_readiness_message_t query_message(void)
{
    lcs_config_readiness_message_t message = { .kind = LCS_READINESS_QUERY,
        .requester_instance = 100, .attempt = 10 };
    strcpy(message.membership, g_state.config_status.membership);
    strcpy(message.baseline, g_state.config_status.baseline);
    return message;
}

static void test_readiness_codecs(void)
{
    initialize();
    lcs_config_readiness_message_t message = query_message(), decoded;
    unsigned char payload[LCS_CONFIG_READINESS_MAX + 1];
    size_t length;
    assert(!lcs_config_readiness_encode(&message, payload, sizeof(payload), &length));
    assert(length == 158);
    for (size_t i = 0; i < length; i++)
        assert(lcs_config_readiness_decode(payload, i, &decoded) == -1);
    assert(!lcs_config_readiness_decode(payload, length, &decoded));
    assert(decoded.requester_instance == 100 && decoded.attempt == 10 && decoded.kind == LCS_READINESS_QUERY);
    payload[length] = 0;
    assert(lcs_config_readiness_decode(payload, length + 1, &decoded) == -1);
    payload[length - 1] = 'x';
    assert(lcs_config_readiness_decode(payload, length, &decoded) == -1);
    payload[length - 1] = 0;
    payload[length - 1 - 65] = 'x';
    assert(lcs_config_readiness_decode(payload, length, &decoded) == -1);
    assert(!lcs_config_readiness_encode(&message, payload, sizeof(payload), &length));
    payload[28] = 0; /* An early NUL plus hidden trailing digest bytes is invalid. */
    assert(lcs_config_readiness_decode(payload, length, &decoded) == -1);
    message.status = LCS_READINESS_READY;
    assert(lcs_config_readiness_encode(&message, payload, sizeof(payload), &length) == -1);
    message.kind = LCS_READINESS_REPLY;
    assert(lcs_config_readiness_encode(&message, payload, sizeof(payload), &length) == -1);
    message.responder_instance = 200;
    assert(!lcs_config_readiness_encode(&message, payload, sizeof(payload), &length));
    assert(!lcs_config_readiness_decode(payload, length, &decoded));
    payload[2] = 9;
    assert(lcs_config_readiness_decode(payload, length, &decoded) == -1);
    assert(!lcs_config_status_encode(&g_state.config_status, payload, sizeof(payload), &length));
    assert(lcs_config_readiness_decode(payload, length, &decoded) == -1);
}

static void test_readiness_round(void)
{
    initialize();
    prepare_durable_runtime();
    lcs_config_runtime_t *runtime = &g_state.config_runtime;
    lcs_config_readiness_t state = {0};
    char error[256];
    assert(!lcs_config_readiness_begin(&state, &runtime->activation, &runtime->agreement,
                                       &runtime->application, 100, 1, 1000, error, sizeof(error)));
    lcs_config_readiness_message_t request;
    assert(!lcs_config_readiness_query(&state, 1, 200, 1001, &request));
    assert(lcs_config_readiness_begin(&state, &runtime->activation, &runtime->agreement,
                                      &runtime->application, 100, 2, 1001, error, sizeof(error)) == -1);
    assert(state.query.attempt == 1 && state.deadline_ms == 11000);
    lcs_config_readiness_message_t reply = request;
    reply.kind = LCS_READINESS_REPLY;
    reply.responder_instance = 200;
    reply.status = LCS_READINESS_READY;
    reply.attempt++;
    assert(lcs_config_readiness_reply(&state, 1, 200, 1002, &reply, error, sizeof(error)) == 1);
    reply.attempt--;
    assert(lcs_config_readiness_reply(&state, 2, 200, 1002, &reply, error, sizeof(error)) == 1);
    reply.status = LCS_READINESS_NOT_READY;
    assert(!lcs_config_readiness_reply(&state, 1, 200, 1002, &reply, error, sizeof(error)));
    assert(state.round.ready == 1);
    reply.status = LCS_READINESS_READY;
    assert(!lcs_config_readiness_reply(&state, 1, 200, 1002, &reply, error, sizeof(error)));
    assert(state.round.ready == 3 && !lcs_config_readiness_complete(&state));
    assert(!lcs_config_readiness_query(&state, 2, 300, 1003, &request));
    assert(!lcs_config_readiness_query(&state, 2, 301, 1004, &request));
    reply = request;
    reply.kind = LCS_READINESS_REPLY;
    reply.status = LCS_READINESS_READY;
    reply.responder_instance = 300;
    assert(lcs_config_readiness_reply(&state, 2, 301, 1005, &reply, error, sizeof(error)) == 1);
    reply.responder_instance = 301;
    assert(!lcs_config_readiness_reply(&state, 2, 301, 1005, &reply, error, sizeof(error)));
    assert(lcs_config_readiness_complete(&state) && state.round.ready == 7);
    assert(lcs_config_readiness_poll(&state, UINT64_MAX) == 1); /* Sealed proof does not expire. */
    assert(lcs_config_readiness_begin(&state, &runtime->activation, &runtime->agreement,
                                      &runtime->application, 100, 1, 2000, error, sizeof(error)) == -1);
    assert(!lcs_config_readiness_begin(&state, &runtime->activation, &runtime->agreement,
                                       &runtime->application, 100, 2, 2000, error, sizeof(error)));
    assert(!lcs_config_readiness_query(&state, 1, 200, 11999, &request));
    assert(state.deadline_ms == 12000);
    assert(lcs_config_readiness_poll(&state, 12000) == -1 && state.round.failed);
    assert(!lcs_config_readiness_begin(&state, &runtime->activation, &runtime->agreement,
                                       &runtime->application, 100, 3, 3000, error, sizeof(error)));
    assert(!lcs_config_readiness_query(&state, 1, 200, 3001, &request));
    reply = request;
    reply.kind = LCS_READINESS_REPLY;
    reply.status = LCS_READINESS_READY;
    reply.responder_instance = 200;
    reply.baseline[0] = reply.baseline[0] == 'a' ? 'b' : 'a';
    assert(lcs_config_readiness_reply(&state, 1, 200, 3002, &reply, error, sizeof(error)) == -1);
    assert(state.round.failed);
    cleanup_durable_runtime();
}

static void test_explicit_peer_readiness(void)
{
    initialize();
    g_state.self_index = 0;
    g_state.instance_id = 100;
    establish(1, 6);
    establish(2, 5);
    g_state.peers[1].instance_id = 200;
    g_state.peers[2].instance_id = 300;
    readiness_queued = 0;
    assert(config_peer_readiness_start() == -1); /* INI preparation alone is insufficient. */
    lcs_config_readiness_message_t request = query_message();
    request.requester_instance = 200;
    unsigned char payload[LCS_CONFIG_READINESS_MAX];
    size_t length;
    assert(!lcs_config_readiness_encode(&request, payload, sizeof(payload), &length));
    assert(!config_peer_receive_readiness(-1, 1, LCS_MSG_CONFIG_READINESS_QUERY, payload, length));
    assert(last_ready[1].status == LCS_READINESS_NOT_READY && !g_state.config_runtime.base);
    assert(config_peer_receive_readiness(-1, 2, LCS_MSG_CONFIG_READINESS_QUERY, payload, length) == -1);
    prepare_durable_runtime();
    assert(config_peer_readiness_start() == 1); /* Quorum-only node still negotiated wire 5. */
    g_state.peers[2].protocol_version = 6;
    assert(!config_peer_receive_readiness(-1, 1, LCS_MSG_CONFIG_READINESS_QUERY, payload, length));
    assert(last_ready[1].status == LCS_READINESS_READY);
    reload_busy = true;
    assert(!config_peer_receive_readiness(-1, 1, LCS_MSG_CONFIG_READINESS_QUERY, payload, length));
    assert(last_ready[1].status == LCS_READINESS_NOT_READY);
    reload_busy = false;
    request.requester_instance = 999;
    assert(!lcs_config_readiness_encode(&request, payload, sizeof(payload), &length));
    assert(config_peer_receive_readiness(-1, 1, LCS_MSG_CONFIG_READINESS_QUERY, payload, length) == -1);
    request.requester_instance = 200;
    request.baseline[0] = request.baseline[0] == 'a' ? 'b' : 'a';
    assert(!lcs_config_readiness_encode(&request, payload, sizeof(payload), &length));
    assert(!config_peer_receive_readiness(-1, 1, LCS_MSG_CONFIG_READINESS_QUERY, payload, length));
    assert(last_ready[1].status == LCS_READINESS_MISMATCH);
    assert(!config_peer_readiness_start());
    uint64_t deadline = g_state.config_readiness.deadline_ms;
    assert(config_peer_readiness_start() == 1);
    assert(g_state.config_readiness_attempt == 1 && g_state.config_readiness.deadline_ms == deadline);
    config_peer_process(-1);
    assert(g_state.config_readiness.sent == 6);
    for (size_t node = 1; node < 3; node++)
    {
        lcs_config_readiness_message_t reply = last_ready[node];
        assert(reply.kind == LCS_READINESS_QUERY);
        reply.kind = LCS_READINESS_REPLY;
        reply.responder_instance = g_state.peers[node].instance_id;
        reply.status = LCS_READINESS_READY;
        assert(!lcs_config_readiness_encode(&reply, payload, sizeof(payload), &length));
        assert(config_peer_receive_readiness(-1, (int)node, LCS_MSG_CONFIG_READINESS_QUERY, payload, length) == -1);
        assert(!config_peer_receive_readiness(-1, (int)node, LCS_MSG_CONFIG_READINESS_REPLY, payload, length));
    }
    assert(lcs_config_readiness_complete(&g_state.config_readiness));
    assert(g_state.config_readiness.round.ready == 7);
    assert(!g_state.config_runtime.barrier.store); /* Collection never writes proof or switches authority. */
    assert(!lcs_config_barrier_can_vote(&g_state.config_runtime.barrier, &g_state.config_runtime.activation,
                                      &g_state.config_runtime.agreement, &g_state.config_runtime.application));
    cleanup_durable_runtime();
}

int main(void)
{
    test_metadata_and_reload();
    test_strict_codecs();
    test_readiness_codecs();
    test_readiness_round();
    test_explicit_peer_readiness();
    puts("configuration peer metadata and explicit readiness unit tests passed");
    return 0;
}
