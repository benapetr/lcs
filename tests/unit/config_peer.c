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
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include "util.h"

daemon_state_t g_state;
static bool reload_busy;
static unsigned queued;
static bool freeze_backpressure, stop_freeze_sync;
static unsigned freezes_queued;
static lcs_config_freeze_message_t last_freeze[LCS_MAX_NODES];
int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    if (stop_freeze_sync) { stop_freeze_sync = false; raise(SIGSTOP); }
    return __real_fsync(fd);
}
int peer_queue_config_freeze(int epoll_fd, int node, uint16_t type, const void *payload, uint32_t length)
{
    (void)epoll_fd;
    if (freeze_backpressure) return 1;
    lcs_config_freeze_message_t message;
    assert(!lcs_config_freeze_decode(payload, length, &message));
    assert(message.kind == (type == LCS_MSG_CONFIG_FREEZE_QUERY ? LCS_FREEZE_QUERY : LCS_FREEZE_REPLY));
    last_freeze[node] = message; freezes_queued++;
    return 0;
}

bool config_reload_in_progress(void) { return reload_busy; }
int lcs_config_live_freeze_drain(int epoll_fd, char *err, size_t err_len)
{
    (void)epoll_fd; (void)err; (void)err_len;
    g_state.config_freeze_drained = true; return 1; /* Backend drain is tested in config_reconcile. */
}
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

int peer_queue_config_transaction(int epoll_fd, int node, uint16_t type, const void *payload, uint32_t length)
{
    (void)epoll_fd; (void)node; (void)type; (void)payload; (void)length;
    return 1;
}

static lcs_config_owner_message_t last_owner;
static unsigned owners_queued;
static bool owner_backpressure;
int peer_queue_config_owner(int epoll_fd, int node, const void *payload, uint32_t length)
{
    (void)epoll_fd; (void)node;
    if (owner_backpressure)
        return 1;
    assert(!lcs_config_owner_decode(payload, length, &last_owner));
    owners_queued++;
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

static void test_dynamic_status_codec(void)
{
    initialize();
    lcs_config_status_view_t view, decoded;
    unsigned char payload[8192], saved[8192]; size_t length;
    assert(!config_peer_view(&view));
    assert(!lcs_config_status_view_encode(&view, payload, sizeof(payload), &length));
    assert(payload[0] == 0 && payload[1] == 1); /* Legacy response stays byte-compatible. */
    view.authority = LCS_CONFIG_AUTHORITY_JSON;
    view.phase = LCS_CONFIG_DIAG_BLOCKED;
    view.committed_revision = 3; view.applied_revision = 1; view.pending_revision = 2;
    memset(view.committed_digest, 'a', 64); view.committed_digest[64] = 0;
    memset(view.applied_digest, 'b', 64); view.applied_digest[64] = 0;
    memset(view.pending_digest, 'c', 64); view.pending_digest[64] = 0;
    strcpy(view.error, "cleanup failed: \"old\" backend\nrepair required");
    assert(!lcs_config_status_view_encode(&view, payload, sizeof(payload), &length));
    assert(payload[0] == 0 && payload[1] == 2);
    assert(!lcs_config_status_view_decode(payload, length, &decoded));
    assert(decoded.authority == LCS_CONFIG_AUTHORITY_JSON && !decoded.admitted);
    assert(decoded.committed_revision == 3 && decoded.applied_revision == 1 && decoded.pending_revision == 2);
    assert(!strcmp(decoded.error, view.error) && !strcmp(decoded.pending_digest, view.pending_digest));
    memcpy(saved, payload, length);
    for (size_t i = 0; i < length; i++) assert(lcs_config_status_view_decode(payload, i, &decoded) < 0);
    payload[length] = 0;
    assert(lcs_config_status_view_decode(payload, length + 1, &decoded) < 0);
    payload[1] = 1; /* An older decoder's format cannot reinterpret dynamic authority. */
    assert(lcs_config_status_view_decode(payload, length, &decoded) < 0);
    memcpy(payload, saved, length);
    size_t extension = length - (4 + 24 + 3 * LCS_CONFIG_STATUS_DIGEST_SIZE + 256);
    payload[extension] = LCS_CONFIG_AUTHORITY_INI;
    assert(lcs_config_status_view_decode(payload, length, &decoded) < 0);
    memcpy(payload, saved, length); payload[extension + 1] = 1;
    assert(lcs_config_status_view_decode(payload, length, &decoded) < 0); /* Blocked is not admitted. */
    memcpy(payload, saved, length); payload[length - 1] = 'x';
    assert(lcs_config_status_view_decode(payload, length, &decoded) < 0); /* Canonical padding. */
    view.phase = LCS_CONFIG_DIAG_ACTIVE; view.admitted = 1;
    assert(lcs_config_status_view_encode(&view, payload, sizeof(payload), &length) < 0);
    view.applied_revision = 3; view.pending_revision = 0; view.pending_digest[0] = 0; view.error[0] = 0;
    assert(lcs_config_status_view_encode(&view, payload, sizeof(payload), &length) < 0); /* Digest mismatch. */
    strcpy(view.applied_digest, view.committed_digest);
    assert(!lcs_config_status_view_encode(&view, payload, sizeof(payload), &length));
    assert(!lcs_config_status_view_decode(payload, length, &decoded));
    assert(decoded.admitted && !decoded.pending_revision && !*decoded.error);
    assert(!strcmp(lcs_config_status_phase_name(decoded.phase), "active"));
    view.dynamic_commands = 1;
    assert(!lcs_config_status_view_encode(&view, payload, sizeof(payload), &length) && payload[1] == 3);
    assert(!lcs_config_status_view_decode(payload, length, &decoded) && decoded.dynamic_commands);
    payload[length - 1] = 0;
    assert(lcs_config_status_view_decode(payload, length, &decoded) < 0);
    view.admitted = 0; view.phase = LCS_CONFIG_DIAG_BLOCKED;
    assert(lcs_config_status_view_encode(&view, payload, sizeof(payload), &length) < 0);
}

static lcs_config_store_t readiness_store;
static char readiness_directory[80];

static void prepare_durable_runtime_mode(bool required)
{
    char error[256];
    strcpy(readiness_directory, "/tmp/lcs-ready-peer.XXXXXX");
    assert(mkdtemp(readiness_directory));
    assert(!lcs_config_store_open(&readiness_store, readiness_directory, error, sizeof(error)));
    lcs_config_runtime_t *runtime = &g_state.config_runtime;
    runtime->base = malloc(sizeof(*runtime->base));
    assert(runtime->base);
    if (required)
        assert(!lcs_config_activation_open_requests(&runtime->activation, &readiness_store, &g_state.cfg,
                                        runtime->base, true, error, sizeof(error)));
    else
        assert(!lcs_config_activation_open(&runtime->activation, &readiness_store, &g_state.cfg,
                                        runtime->base, true, error, sizeof(error)));
    assert(!lcs_config_activation_resume(&runtime->activation, &runtime->agreement,
                                          &runtime->application, error, sizeof(error)));
    if (required) assert(!lcs_config_requests_recover(&runtime->requests, &runtime->agreement, false, error, sizeof(error)));
    runtime->requires_recovery = true;
    g_state.config_bootstrap.initialization_locked = true;
    assert(!config_peer_refresh());
    assert(!g_state.config_status.prepared); /* Diagnostic preparation is withdrawn. */
}

static void prepare_durable_runtime(void) { prepare_durable_runtime_mode(false); }

static void cleanup_durable_runtime(void)
{
    bool persisted = g_state.config_runtime.barrier.store != NULL;
    config_peer_stop();
    for (unsigned i = 0; i < 5000 && !config_peer_stopped(); i++)
    {
        config_peer_shutdown_process();
        usleep(1000);
    }
    assert(config_peer_stopped() && !config_peer_configuration_vote_ready());
    lcs_config_runtime_close(&g_state.config_runtime);
    const char *names[] = {"activation.json", "agreement.json", "application.json", "lock"};
    if (!faccessat(readiness_store.directory_fd, "requests.json", F_OK, 0))
        assert(!unlinkat(readiness_store.directory_fd, "requests.json", 0));
    if (persisted)
        assert(!unlinkat(readiness_store.directory_fd, "barrier.json", 0));
    else
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
    assert(!config_peer_configuration_vote_ready());
    config_peer_process(-1);
    assert(g_state.config_coordinator.phase == LCS_CONFIG_COORD_PERSISTING);
    assert(g_state.config_coordinator.storage.pid > 0 && !config_peer_stopped());
    assert(config_peer_readiness_start() == 1);
    for (unsigned i = 0; i < 5000 && !config_peer_configuration_vote_ready(); i++)
    {
        config_peer_process(-1);
        usleep(1000);
    }
    assert(config_peer_configuration_vote_ready() && g_state.config_runtime.barrier.ready == 7);
    assert(!g_state.config_coordinator.storage.pid && g_state.config_runtime.requires_recovery);
    assert(g_state.resources[0].epoch == 123 && g_state.lease_grants[0].promised_epoch == 456);
    cleanup_durable_runtime();
}

static void test_owner_transport_admission(void)
{
    initialize(); g_state.instance_id = 100; g_state.self_index = 0;
    establish(1, 6); g_state.peers[1].instance_id = 200;
    establish(2, 6); g_state.peers[2].instance_id = 300;
    lcs_config_owner_message_t query = { .kind = LCS_CONFIG_OWNER_QUERY, .sender_instance = 200,
        .target_instance = 100, .id = 1, .scope = { .old_revision = 0, .target_revision = 1, .drain = 1 } };
    strcpy(query.scope.membership, g_state.config_status.membership);
    memset(query.scope.old_value, 'a', 64); memset(query.scope.target_value, 'b', 64);
    unsigned char payload[LCS_CONFIG_OWNER_MESSAGE_SIZE]; size_t length;
    assert(!lcs_config_owner_encode(&query, payload, sizeof(payload), &length));
    owners_queued = 0; owner_backpressure = true;
    assert(!config_peer_receive_owner(-1, 1, payload, length));
    assert(!owners_queued && g_state.config_owners.pending_replies == 2);
    assert(!g_state.config_runtime.base && !g_state.config_owners.receipt.sealed && !g_state.config_owners.round.active);
    owner_backpressure = false; config_peer_process(-1);
    assert(owners_queued == 1 && last_owner.result == LCS_CONFIG_OWNER_PENDING);
    assert(last_owner.sender_instance == 100 && last_owner.target_instance == 200);
    query.kind = LCS_CONFIG_OWNER_APPLIED_QUERY;
    assert(!lcs_config_owner_encode(&query, payload, sizeof(payload), &length));
    assert(!config_peer_receive_owner(-1, 1, payload, length));
    assert(last_owner.kind == LCS_CONFIG_OWNER_APPLIED_REPLY && last_owner.result == LCS_CONFIG_OWNER_PENDING);
    assert(!g_state.config_owners.receipt.sealed && !g_state.config_owners.release.active);
    query.kind = LCS_CONFIG_OWNER_QUERY;
    assert(!lcs_config_owner_encode(&query, payload, sizeof(payload), &length));
    assert(config_peer_receive_owner(-1, 2, payload, length) < 0); /* Quorum only. */
    g_state.peers[1].protocol_version = 5;
    assert(config_peer_receive_owner(-1, 1, payload, length) < 0);
    g_state.peers[1].protocol_version = 6;
    g_state.peers[1].instance_id = 201;
    assert(config_peer_receive_owner(-1, 1, payload, length) < 0);
    g_state.peers[1].instance_id = 200;
    query.target_instance++; assert(!lcs_config_owner_encode(&query, payload, sizeof(payload), &length));
    assert(config_peer_receive_owner(-1, 1, payload, length) < 0);
    query.target_instance--; query.scope.membership[0] = query.scope.membership[0] == 'a' ? 'b' : 'a';
    assert(!lcs_config_owner_encode(&query, payload, sizeof(payload), &length));
    assert(config_peer_receive_owner(-1, 1, payload, length) < 0);
    assert(!g_state.config_owners.receipt.sealed && !g_state.config_owners.round.active);
    config_peer_stop(); assert(g_state.config_owners.closing);
    assert(config_peer_receive_owner(-1, 1, payload, length) < 0);
}

static lcs_config_freeze_message_t freeze_query(uint64_t attempt)
{
    lcs_config_freeze_message_t message = {.kind = LCS_FREEZE_QUERY,
        .receipt = {.requester_instance = 200, .responder_instance = 100, .attempt = attempt}};
    strcpy(message.receipt.membership, g_state.config_status.membership);
    strcpy(message.receipt.baseline, g_state.config_status.baseline);
    return message;
}

static int receive_freeze(lcs_config_freeze_message_t *message, int node)
{
    unsigned char payload[LCS_CONFIG_FREEZE_SIZE]; size_t length;
    assert(!lcs_config_freeze_encode(message, payload, sizeof(payload), &length));
    return config_peer_receive_freeze(-1, node,
        message->kind == LCS_FREEZE_QUERY ? LCS_MSG_CONFIG_FREEZE_QUERY : LCS_MSG_CONFIG_FREEZE_REPLY, payload, length);
}

static void test_freeze_codec(void)
{
    initialize();
    lcs_config_freeze_message_t message = freeze_query(1), decoded;
    unsigned char bytes[LCS_CONFIG_FREEZE_SIZE + 1], saved[LCS_CONFIG_FREEZE_SIZE]; size_t length;
    assert(!lcs_config_freeze_encode(&message, bytes, sizeof(bytes), &length) && length == LCS_CONFIG_FREEZE_SIZE);
    memcpy(saved, bytes, length);
    assert(!lcs_config_freeze_decode(bytes, length, &decoded));
    assert(decoded.receipt.responder_instance == 100 && !decoded.receipt.status);
    for (size_t i = 0; i < length; i++) assert(lcs_config_freeze_decode(bytes, i, &decoded) < 0);
    assert(lcs_config_freeze_decode(bytes, length + 1, &decoded) < 0);
    const size_t offsets[] = {1, 2, 27, 28, 92, 93, 157};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    {
        memcpy(bytes, saved, length); bytes[offsets[i]] = 255;
        assert(lcs_config_freeze_decode(bytes, length, &decoded) < 0);
    }
    for (size_t offset = 3; offset < 27; offset += 8)
    { memcpy(bytes, saved, length); memset(bytes + offset, 0, 8); assert(lcs_config_freeze_decode(bytes, length, &decoded) < 0); }
    message.receipt.responder_instance = 0;
    assert(lcs_config_freeze_encode(&message, bytes, sizeof(bytes), &length) < 0);
    message = freeze_query(1); message.kind = LCS_FREEZE_REPLY;
    for (unsigned status = 1; status <= 3; status++)
    {
        message.receipt.status = status;
        assert(!lcs_config_freeze_encode(&message, bytes, sizeof(bytes), &length));
        assert(!lcs_config_freeze_decode(bytes, length, &decoded));
    }
}

static void test_freeze_peer_admission(void)
{
    initialize(); g_state.self_index = 0; g_state.instance_id = 100;
    establish(1, 6); establish(2, 5);
    g_state.peers[1].instance_id = 200; g_state.peers[2].instance_id = 300;
    freezes_queued = 0;
    lcs_config_freeze_message_t query = freeze_query(1);
    assert(!receive_freeze(&query, 1));
    assert(freezes_queued == 1 && last_freeze[1].receipt.status == LCS_INITIALIZATION_BUSY);
    assert(!g_state.config_coordinator.storage.pid && !g_state.config_bootstrap.initialization_locked);
    uint64_t deadline = g_state.config_freeze_waiters[1].deadline_ms;
    assert(!receive_freeze(&query, 1) && g_state.config_freeze_waiters[1].deadline_ms == deadline);
    query.receipt.baseline[0] ^= 1;
    assert(receive_freeze(&query, 1) < 0); /* Same attempt cannot change its identity. */
    query.receipt.attempt = 2;
    assert(!receive_freeze(&query, 1) && last_freeze[1].receipt.status == LCS_INITIALIZATION_MISMATCH);
    query = freeze_query(3); query.receipt.responder_instance++;
    assert(receive_freeze(&query, 1) < 0);
    query = freeze_query(3); query.receipt.requester_instance++;
    assert(receive_freeze(&query, 1) < 0);
    query = freeze_query(3); g_state.peers[1].protocol_version = 5;
    assert(receive_freeze(&query, 1) < 0); g_state.peers[1].protocol_version = 6;
    query.receipt.requester_instance = 300; assert(receive_freeze(&query, 2) < 0);
    /* A blocked/queued INI reload prevents even starting the worker. */
    query = freeze_query(3); establish(2, 6); reload_busy = true;
    assert(!receive_freeze(&query, 1) && last_freeze[1].receipt.status == LCS_INITIALIZATION_BUSY);
    assert(!g_state.config_coordinator.initialized && !g_state.config_bootstrap.initialization_locked);
    reload_busy = false;
    g_state.config_freeze_waiters[1].deadline_ms = 0; /* Expiry never starts a late write. */
    assert(!receive_freeze(&query, 1)); config_peer_process(-1);
    assert(!g_state.config_coordinator.initialized && !g_state.config_freeze_waiters[1].pending);
}

static void test_durable_freeze_exchange(void)
{
    initialize(); g_state.self_index = 0; g_state.instance_id = 100;
    establish(1, 6); establish(2, 6);
    g_state.peers[1].instance_id = 200; g_state.peers[2].instance_id = 300;
    strcpy(readiness_directory, "/tmp/lcs-freeze-peer.XXXXXX"); assert(mkdtemp(readiness_directory));
    char error[256]; assert(!lcs_config_store_open(&readiness_store, readiness_directory, error, sizeof(error)));
    g_state.config_bootstrap.state = readiness_store;
    stop_freeze_sync = true; freezes_queued = 0;
    lcs_config_freeze_message_t query = freeze_query(1);
    assert(!receive_freeze(&query, 1));
    pid_t child = g_state.config_coordinator.storage.pid; assert(child > 0);
    assert(last_freeze[1].receipt.status == LCS_INITIALIZATION_BUSY && !g_state.config_runtime.activation.store);
    int status = 0; pid_t result = 0;
    for (unsigned i = 0; i < 5000 && !result; i++)
    { result = waitpid(child, &status, WNOHANG | WUNTRACED); if (!result) usleep(1000); }
    assert(result == child && WIFSTOPPED(status));
    assert(g_state.config_bootstrap.initialization_locked && g_state.config_runtime.requires_recovery);
    assert(!kill(child, SIGCONT)); stop_freeze_sync = false;
    /* Output backpressure and a replaced requester cannot publish a stale ACK. */
    freeze_backpressure = true; g_state.peers[1].instance_id = 201;
    for (unsigned i = 0; i < 5000 && g_state.config_coordinator.storage.pid; i++) { config_peer_process(-1); usleep(1000); }
    assert(!g_state.config_coordinator.storage.pid && g_state.config_coordinator.completed_freezes == 1);
    assert(g_state.config_runtime.activation.phase == LCS_CONFIG_ACTIVATION_FROZEN);
    assert(!g_state.config_freeze_waiters[1].pending && !config_peer_configuration_vote_ready());
    assert(g_state.resources[0].epoch == 123 && g_state.lease_grants[0].promised_epoch == 456);
    query.receipt.requester_instance = 201; query.receipt.attempt = 2;
    assert(!receive_freeze(&query, 1) && g_state.config_freeze_waiters[1].pending);
    freeze_backpressure = false; g_state.config_freeze_waiters[1].next_send_ms = 0;
    config_peer_process(-1);
    assert(last_freeze[1].receipt.status == LCS_INITIALIZATION_FROZEN && !g_state.config_freeze_waiters[1].pending);
    assert(!config_peer_freeze_start());
    config_peer_process(-1);
    assert(last_freeze[1].kind == LCS_FREEZE_QUERY && last_freeze[2].kind == LCS_FREEZE_QUERY);
    lcs_config_freeze_message_t replies[2] = {last_freeze[1], last_freeze[2]};
    for (size_t i = 0; i < 2; i++)
    { replies[i].kind = LCS_FREEZE_REPLY; replies[i].receipt.status = LCS_INITIALIZATION_FROZEN; assert(!receive_freeze(&replies[i], (int)i + 1)); }
    assert(g_state.config_initialization.sealed && !g_state.config_runtime.agreement.store);
    /* A sealed freeze collection alone still cannot initialize or activate. */
    g_state.config_freeze_waiters[1].deadline_ms = 0; config_peer_process(-1);
    assert(g_state.config_runtime.activation.phase == LCS_CONFIG_ACTIVATION_FROZEN);
    config_peer_stop();
    for (unsigned i = 0; i < 5000 && !config_peer_stopped(); i++) { config_peer_shutdown_process(); usleep(1000); }
    assert(config_peer_stopped()); lcs_config_runtime_close(&g_state.config_runtime);
    const char *absent[] = {"agreement.json", "application.json", "requests.json", "barrier.json"};
    for (size_t i = 0; i < sizeof(absent) / sizeof(absent[0]); i++) assert(faccessat(readiness_store.directory_fd, absent[i], F_OK, 0) == -1 && errno == ENOENT);
    assert(!unlinkat(readiness_store.directory_fd, "activation.json", 0)); assert(!unlinkat(readiness_store.directory_fd, "lock", 0));
    lcs_config_store_close(&readiness_store); assert(!rmdir(readiness_directory));
}

static void test_freeze_failure_and_shutdown(void)
{
    for (unsigned mode = 0; mode < 2; mode++)
    {
        initialize(); g_state.self_index = 0; g_state.instance_id = 100;
        establish(1, 6); establish(2, 6);
        g_state.peers[1].instance_id = 200; g_state.peers[2].instance_id = 300;
        strcpy(readiness_directory, "/tmp/lcs-freeze-failure.XXXXXX"); assert(mkdtemp(readiness_directory));
        char error[256]; assert(!lcs_config_store_open(&readiness_store, readiness_directory, error, sizeof(error)));
        g_state.config_bootstrap.state = readiness_store; stop_freeze_sync = true;
        lcs_config_freeze_message_t query = freeze_query(1);
        assert(!receive_freeze(&query, 1));
        pid_t child = g_state.config_coordinator.storage.pid; assert(child > 0);
        int status = 0; pid_t result = 0;
        for (unsigned i = 0; i < 5000 && !result; i++)
        { result = waitpid(child, &status, WNOHANG | WUNTRACED); if (!result) usleep(1000); }
        assert(result == child && WIFSTOPPED(status));
        if (mode) config_peer_stop(); else assert(!kill(child, SIGKILL));
        stop_freeze_sync = false;
        for (unsigned i = 0; i < 5000 && !config_peer_stopped(); i++)
        { if (mode) config_peer_shutdown_process(); else config_peer_process(-1); usleep(1000); }
        assert(config_peer_stopped() && g_state.config_bootstrap.initialization_locked);
        assert(g_state.config_runtime.requires_recovery && g_state.config_runtime.activation.poisoned);
        assert(!g_state.config_coordinator.completed_freezes && last_freeze[1].receipt.status == LCS_INITIALIZATION_BUSY);
        config_peer_process(-1); assert(!g_state.config_freeze_waiters[1].pending);
        assert(g_state.resources[0].epoch == 123 && g_state.lease_grants[0].promised_epoch == 456);
        lcs_config_runtime_close(&g_state.config_runtime);
        assert(faccessat(readiness_store.directory_fd, "activation.json", F_OK, 0) == -1 && errno == ENOENT);
        assert(!unlinkat(readiness_store.directory_fd, ".activation.json.tmp", 0));
        assert(!unlinkat(readiness_store.directory_fd, "lock", 0));
        lcs_config_store_close(&readiness_store); assert(!rmdir(readiness_directory));
    }
}

static void test_frozen_startup_and_status(void)
{
    initialize(); g_state.self_index = 0; g_state.instance_id = 100;
    strcpy(readiness_directory, "/tmp/lcs-frozen-startup.XXXXXX"); assert(mkdtemp(readiness_directory));
    char error[256]; assert(!lcs_config_store_open(&readiness_store, readiness_directory, error, sizeof(error)));
    g_state.config_bootstrap.state = readiness_store;
    lcs_config_activation_t activation = {0}; lcs_config_t *base = malloc(sizeof(*base)); assert(base);
    assert(!lcs_config_activation_freeze(&activation, &readiness_store, &g_state.cfg, base, error, sizeof(error)));
    lcs_config_activation_close(&activation); free(base);
    /* Later valid INI edits include an added resource and a changed backend. */
    g_state.cfg.resources[1] = g_state.cfg.resources[0]; strcpy(g_state.cfg.resources[1].name, "extra");
    strcpy(g_state.cfg.resources[1].address, "192.0.2.30/32");
    strcpy(g_state.cfg.resources[0].address, "192.0.2.20/32"); g_state.cfg.resource_count = 2;
    assert(!lcs_config_finalize(&g_state.cfg, error, sizeof(error)));
    assert(!lcs_config_runtime_open(&g_state.config_runtime, &g_state.config_bootstrap.state, &g_state.cfg, error, sizeof(error)));
    g_state.config_status.serial = 0; g_state.config_status_generation = 0;
    assert(config_peer_resume_frozen(error, sizeof(error)) < 0); /* Existing epochs/grants may never be reset. */
    assert(g_state.resources[0].epoch == 123 && g_state.cfg.resource_count == 2);
    memset(g_state.resources, 0, sizeof(g_state.resources)); memset(g_state.lease_grants, 0, sizeof(g_state.lease_grants));
    establish(1, 6); assert(config_peer_resume_frozen(error, sizeof(error)) < 0); g_state.peers[1].fd = -1;
    assert(!config_peer_resume_frozen(error, sizeof(error)));
    assert(g_state.cfg.resource_count == 1 && !strcmp(g_state.cfg.resources[0].name, "vip"));
    assert(!strcmp(g_state.cfg.resources[0].address, "192.0.2.10/32"));
    assert(g_state.config_application_fenced && g_state.config_application_remapping && resource_config_is_fenced(0));
    assert(!g_state.voting_ready && !g_state.resource_generation_enabled && !g_state.resource_generation_admitted);
    assert(g_state.config_bootstrap.initialization_locked && !g_state.config_bootstrap.prepared);
    assert(g_state.config_coordinator.initialized && g_state.config_coordinator.phase == LCS_CONFIG_COORD_IDLE);
    assert(!g_state.config_runtime.agreement.store && !g_state.config_runtime.requests.store && !config_peer_configuration_vote_ready());
    establish(1, 6); establish(2, 6); g_state.peers[1].instance_id = 200; g_state.peers[2].instance_id = 300;
    queued = 0; config_peer_process(-1); assert(queued == 2);
    uint64_t serial = g_state.config_status.serial;
    config_peer_process(-1); assert(queued == 2);
    g_state.config_status_keepalive_ms = 0; config_peer_process(-1);
    assert(queued == 4 && g_state.config_status.serial == serial && !g_state.config_status.prepared);
    assert(!g_state.config_initialization.active && !g_state.config_coordinator.storage.pid);
    lcs_config_status_view_t view, decoded;
    assert(!config_peer_view(&view) && view.phase == LCS_CONFIG_DIAG_FROZEN && !view.admitted);
    unsigned char payload[4096], saved[4096]; size_t length;
    assert(!lcs_config_status_view_encode(&view, payload, sizeof(payload), &length) && payload[1] == 4);
    memcpy(saved, payload, length);
    assert(!lcs_config_status_view_decode(payload, length, &decoded));
    assert(decoded.authority == LCS_CONFIG_AUTHORITY_INI && decoded.phase == LCS_CONFIG_DIAG_FROZEN);
    assert(!decoded.dynamic_commands && !decoded.committed_revision && !strcmp(lcs_config_status_phase_name(decoded.phase), "frozen"));
    for (size_t i = 0; i < length; i++) assert(lcs_config_status_view_decode(payload, i, &decoded) < 0);
    for (unsigned format = 1; format <= 3; format++)
    { memcpy(payload, saved, length); payload[1] = format; assert(lcs_config_status_view_decode(payload, length, &decoded) < 0); }
    view.admitted = 1; assert(lcs_config_status_view_encode(&view, payload, sizeof(payload), &length) < 0); view.admitted = 0;
    view.dynamic_commands = 1; assert(lcs_config_status_view_encode(&view, payload, sizeof(payload), &length) < 0); view.dynamic_commands = 0;
    view.nodes[view.self].status.prepared = 1; assert(lcs_config_status_view_encode(&view, payload, sizeof(payload), &length) < 0);
    view.nodes[view.self].status.prepared = 0; view.committed_revision = 1;
    assert(lcs_config_status_view_encode(&view, payload, sizeof(payload), &length) < 0);
    assert(config_peer_resume_frozen(error, sizeof(error)) < 0); /* Not a live reconfiguration entry point. */
    config_peer_stop(); assert(config_peer_stopped()); lcs_config_runtime_close(&g_state.config_runtime);
    assert(!unlinkat(readiness_store.directory_fd, "activation.json", 0)); assert(!unlinkat(readiness_store.directory_fd, "lock", 0));
    lcs_config_store_close(&readiness_store); assert(!rmdir(readiness_directory));
}

static void test_coordinated_local_initialization(void)
{
    initialize(); g_state.self_index = 0; g_state.instance_id = 100;
    establish(1, 6); establish(2, 6); g_state.peers[1].instance_id = 200; g_state.peers[2].instance_id = 300;
    g_state.config_initialization_auto_resume = true; config_peer_process(-1);
    assert(!g_state.config_runtime.requires_recovery && !g_state.config_initialization.active &&
        !g_state.config_coordinator.initialized && !g_state.config_bootstrap.initialization_locked);
    assert(g_state.resources[0].epoch == 123 && g_state.lease_grants[0].promised_epoch == 456);
    for (unsigned self = 0; self <= 2; self += 2)
    {
        initialize(); g_state.self_index = (int)self; g_state.instance_id = 100 + self * 100;
        snprintf(g_state.cfg.self_name, sizeof(g_state.cfg.self_name), "node%u", self);
        strcpy(readiness_directory, "/tmp/lcs-initialize-peer.XXXXXX"); assert(mkdtemp(readiness_directory));
        char error[256]; assert(!lcs_config_store_open(&readiness_store, readiness_directory, error, sizeof(error)));
        g_state.config_bootstrap.state = readiness_store;
        lcs_config_activation_t activation = {0}; lcs_config_t *base = malloc(sizeof(*base)); assert(base);
        assert(!lcs_config_activation_freeze(&activation, &readiness_store, &g_state.cfg, base, error, sizeof(error)));
        lcs_config_activation_close(&activation); free(base);
        assert(!lcs_config_runtime_open(&g_state.config_runtime, &g_state.config_bootstrap.state, &g_state.cfg, error, sizeof(error)));
        memset(g_state.resources, 0, sizeof(g_state.resources)); memset(g_state.lease_grants, 0, sizeof(g_state.lease_grants));
        g_state.config_status.serial = 0; assert(!config_peer_resume_frozen(error, sizeof(error)));
        const lcs_config_t *retained = g_state.config_runtime.base;
        for (size_t i = 0; i < 3; i++) if (i != self) { establish(i, 6); g_state.peers[i].instance_id = 100 + i * 100; }
        assert(config_peer_initialization_start() == 1); /* Scheduler must drain first. */
        config_peer_process(-1);
        assert(!config_peer_initialization_start()); config_peer_process(-1);
        assert(g_state.config_initialization_requested && !g_state.config_coordinator.storage.pid);
        size_t pending = 0;
        for (size_t i = 0; i < 3; i++)
        {
            if (i == self) continue;
            lcs_config_freeze_message_t reply = last_freeze[i];
            assert(reply.kind == LCS_FREEZE_QUERY); reply.kind = LCS_FREEZE_REPLY; reply.receipt.status = LCS_INITIALIZATION_FROZEN;
            if (!pending)
            {
                pending = i + 1; reply.receipt.attempt++;
                assert(!receive_freeze(&reply, (int)i)); /* Stale replies cannot complete a majority/all-member round. */
            }
            else assert(!receive_freeze(&reply, (int)i));
        }
        config_peer_process(-1); assert(!g_state.config_coordinator.storage.pid && !g_state.config_runtime.agreement.store);
        size_t node = pending - 1;
        lcs_config_freeze_message_t reply = last_freeze[node]; reply.kind = LCS_FREEZE_REPLY; reply.receipt.status = LCS_INITIALIZATION_FROZEN;
        assert(!receive_freeze(&reply, (int)node)); assert(g_state.config_initialization.sealed);
        stop_freeze_sync = true; config_peer_process(-1);
        pid_t child = g_state.config_coordinator.storage.pid; assert(child > 0);
        assert(g_state.config_coordinator.phase == LCS_CONFIG_COORD_INITIALIZING && g_state.config_initialization.consumed);
        assert(!g_state.config_initialization_requested && !g_state.config_runtime.agreement.store);
        int status; pid_t result = 0;
        for (unsigned i = 0; i < 5000 && !result; i++)
        { result = waitpid(child, &status, WNOHANG | WUNTRACED); if (!result) usleep(1000); }
        assert(result == child && WIFSTOPPED(status)); assert(config_peer_initialization_start() == 1);
        assert(!kill(child, SIGCONT)); stop_freeze_sync = false;
        for (unsigned i = 0; i < 5000 && g_state.config_coordinator.storage.pid; i++) { config_peer_process(-1); usleep(1000); }
        assert(!g_state.config_coordinator.storage.pid && g_state.config_coordinator.completed_initializations == 1);
        assert(g_state.config_coordinator.phase == LCS_CONFIG_COORD_IDLE && lcs_config_runtime_initialization_prepared(&g_state.config_runtime));
        assert(g_state.config_runtime.base == retained && !config_peer_configuration_vote_ready());
        assert(g_state.config_application_remapping && g_state.config_application_fenced && !g_state.resource_generation_enabled);
        /* A later participant (including a quorum-only requester) can still
         * confirm this durable freeze while local preparation remains pristine. */
        node = self == 0 ? 2 : 0;
        lcs_config_freeze_message_t query = freeze_query(55);
        query.receipt.requester_instance = g_state.peers[node].instance_id;
        query.receipt.responder_instance = g_state.instance_id;
        assert(!receive_freeze(&query, (int)node) && last_freeze[node].receipt.status == LCS_INITIALIZATION_FROZEN);
        g_state.config_runtime.requests.count = 1; query.receipt.attempt++;
        assert(!receive_freeze(&query, (int)node) && last_freeze[node].receipt.status == LCS_INITIALIZATION_BUSY);
        g_state.config_runtime.requests.count = 0;
        assert(config_peer_initialization_start() < 0); /* Never reset already prepared state. */
        config_peer_stop(); assert(config_peer_stopped()); lcs_config_runtime_close(&g_state.config_runtime);
        memset(&g_state.config_coordinator, 0, sizeof(g_state.config_coordinator));
        memset(&g_state.config_readiness, 0, sizeof(g_state.config_readiness));
        memset(&g_state.config_initialization, 0, sizeof(g_state.config_initialization));
        g_state.config_status.serial = 0; g_state.config_status_generation = 0; g_state.instance_id++;
        for (size_t i = 0; i < 3; i++) g_state.peers[i].fd = -1;
        strcpy(g_state.cfg.resources[0].address, "192.0.2.99/32");
        assert(!lcs_config_runtime_open(&g_state.config_runtime, &g_state.config_bootstrap.state, &g_state.cfg, error, sizeof(error)));
        assert(lcs_config_runtime_initialization_prepared(&g_state.config_runtime));
        assert(!config_peer_resume_frozen(error, sizeof(error)));
        assert(!strcmp(g_state.cfg.resources[0].address, "192.0.2.10/32"));
        config_peer_process(-1); assert(!g_state.config_coordinator.storage.pid && !g_state.config_coordinator.completed_initializations);
        config_peer_stop(); assert(config_peer_stopped()); lcs_config_runtime_close(&g_state.config_runtime);
        const char *names[] = {"activation.json", "agreement.json", "application.json", "requests.json", "lock"};
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) assert(!unlinkat(readiness_store.directory_fd, names[i], 0));
        assert(faccessat(readiness_store.directory_fd, "barrier.json", F_OK, 0) < 0 && errno == ENOENT);
        lcs_config_store_close(&readiness_store); assert(!rmdir(readiness_directory));
    }
}

static void test_initialization_interruption(void)
{
    for (unsigned cancel = 0; cancel < 2; cancel++)
    {
        initialize(); g_state.self_index = 0; g_state.instance_id = 100;
        strcpy(readiness_directory, "/tmp/lcs-initialize-failure.XXXXXX"); assert(mkdtemp(readiness_directory));
        char error[256]; assert(!lcs_config_store_open(&readiness_store, readiness_directory, error, sizeof(error)));
        g_state.config_bootstrap.state = readiness_store;
        lcs_config_activation_t activation = {0}; lcs_config_t *base = malloc(sizeof(*base)); assert(base);
        assert(!lcs_config_activation_freeze(&activation, &readiness_store, &g_state.cfg, base, error, sizeof(error)));
        lcs_config_activation_close(&activation); free(base);
        assert(!lcs_config_runtime_open(&g_state.config_runtime, &g_state.config_bootstrap.state, &g_state.cfg, error, sizeof(error)));
        memset(g_state.resources, 0, sizeof(g_state.resources)); memset(g_state.lease_grants, 0, sizeof(g_state.lease_grants));
        g_state.config_status.serial = 0; assert(!config_peer_resume_frozen(error, sizeof(error)));
        establish(1, 6); establish(2, 6); g_state.peers[1].instance_id = 200; g_state.peers[2].instance_id = 300;
        config_peer_process(-1);
        assert(!config_peer_initialization_start()); config_peer_process(-1);
        g_state.peers[1].instance_id++; config_peer_process(-1);
        assert(!g_state.config_initialization_requested && g_state.config_initialization.failed && !g_state.config_coordinator.storage.pid);
        assert(!config_peer_initialization_start()); config_peer_process(-1);
        for (size_t node = 1; node <= 2; node++)
        {
            lcs_config_freeze_message_t reply = last_freeze[node]; reply.kind = LCS_FREEZE_REPLY;
            reply.receipt.status = LCS_INITIALIZATION_FROZEN; assert(!receive_freeze(&reply, (int)node));
        }
        stop_freeze_sync = true; config_peer_process(-1);
        pid_t child = g_state.config_coordinator.storage.pid; assert(child > 0);
        int status; pid_t result = 0;
        for (unsigned i = 0; i < 5000 && !result; i++)
        { result = waitpid(child, &status, WNOHANG | WUNTRACED); if (!result) usleep(1000); }
        assert(result == child && WIFSTOPPED(status));
        if (cancel) config_peer_stop(); else assert(!kill(child, SIGKILL));
        stop_freeze_sync = false;
        for (unsigned i = 0; i < 5000 && !config_peer_stopped(); i++)
        { if (cancel) config_peer_shutdown_process(); else config_peer_process(-1); usleep(1000); }
        assert(config_peer_stopped() && !g_state.config_coordinator.completed_initializations);
        assert(g_state.config_runtime.activation.poisoned && g_state.config_bootstrap.initialization_locked && g_state.config_application_remapping);
        assert(!g_state.config_initialization_requested && config_peer_initialization_start() < 0);
        lcs_config_runtime_close(&g_state.config_runtime);
        assert(!lcs_config_runtime_open(&g_state.config_runtime, &readiness_store, &g_state.cfg, error, sizeof(error)));
        assert(g_state.config_runtime.activation.phase == LCS_CONFIG_ACTIVATION_FROZEN && !g_state.config_runtime.agreement.store);
        lcs_config_runtime_close(&g_state.config_runtime);
        const char *names[] = {"activation.json", ".activation.json.tmp", "lock"};
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) assert(!unlinkat(readiness_store.directory_fd, names[i], 0));
        lcs_config_store_close(&readiness_store); assert(!rmdir(readiness_directory));
    }
}

static void test_automatic_activation_proposal(void)
{
    initialize(); g_state.self_index = 0; g_state.instance_id = 100;
    prepare_durable_runtime_mode(true);
    lcs_config_runtime_t *runtime = &g_state.config_runtime;
    lcs_config_barrier_round_t round; char error[256];
    assert(!lcs_config_barrier_begin(&round, &runtime->activation, &runtime->agreement, &runtime->application, error, sizeof(error)));
    for (size_t i = 0; i < 3; i++) assert(!lcs_config_barrier_ready(&round, i, round.membership, round.baseline, error, sizeof(error)));
    assert(!lcs_config_barrier_seal(&round, error, sizeof(error)));
    assert(!lcs_config_barrier_open(&runtime->barrier, &runtime->activation, &runtime->agreement, &runtime->application, &round, error, sizeof(error)));
    lcs_config_coordinator_init(&g_state.config_coordinator, runtime, &g_state.config_readiness);
    assert(!lcs_config_coordinator_recover(&g_state.config_coordinator));
    lcs_config_transactions_init(&g_state.config_transactions, &g_state.config_coordinator, 100);
    g_state.config_transactions.sync.next_ms = UINT64_MAX;
    g_state.config_activation_auto_resume = true;
    g_state.config_application_fenced = g_state.config_application_remapping = true;
    g_state.config_application_drain_mask = UINT32_MAX;
    config_peer_process(-1); assert(!g_state.config_coordinator.storage.pid && !runtime->requests.count);
    establish(1, 5); g_state.peers[1].instance_id = 200; g_state.config_activation_retry_ms = 1;
    config_peer_process(-1); assert(!runtime->requests.count && !g_state.config_activation_attempt);
    establish(1, 6); g_state.config_application_fenced = false;
    config_peer_process(-1); assert(!g_state.config_activation_attempt);
    g_state.config_application_fenced = true; g_state.cfg.nodes[0].role = LCS_NODE_QUORUM_ONLY;
    config_peer_process(-1); assert(!g_state.config_activation_attempt); g_state.cfg.nodes[0].role = LCS_NODE_FULL;
    config_peer_process(-1);
    assert(g_state.config_activation_attempt == 1 && g_state.config_coordinator.storage.pid > 0);
    const lcs_config_value_t *requested = &g_state.config_transactions.proposer.proposal.requested;
    assert(requested->revision == 1 && strstr(requested->request_id, "activation-") == requested->request_id);
    char first[LCS_CONFIG_REQUEST_MAX + 1]; strcpy(first, requested->request_id);
    lcs_config_t *candidate = malloc(sizeof(*candidate)); uint64_t revision; assert(candidate);
    assert(!lcs_resource_config_decode(requested->document, requested->length, runtime->base, candidate, &revision, error, sizeof(error)));
    assert(candidate->resource_schema_version == 2 && !lcs_resource_config_validate_successor(runtime->base, 0, candidate, 1, error, sizeof(error))); free(candidate);
    for (unsigned i = 0; i < 5000; i++)
    {
        config_peer_process(-1);
        if (g_state.config_transactions.proposer.phase == LCS_CONFIG_PROPOSER_PROMISES) break;
        usleep(1000); assert(i < 4999);
    }
    assert(runtime->requests.count == 1 && runtime->agreement.promised && !runtime->agreement.committed.revision);
    uint64_t ballot = runtime->agreement.promised;
    g_state.config_transactions.proposer.deadline_ms = 1; config_peer_process(-1);
    assert(g_state.config_transactions.proposer.phase == LCS_CONFIG_PROPOSER_UNKNOWN);
    assert(lcs_config_requests_find(&runtime->requests, first));
    g_state.config_activation_retry_ms = 1; config_peer_process(-1);
    assert(g_state.config_activation_attempt == 2 && strcmp(first, g_state.config_transactions.proposer.proposal.requested.request_id));
    for (unsigned i = 0; i < 5000; i++)
    {
        config_peer_process(-1);
        if (g_state.config_transactions.proposer.phase == LCS_CONFIG_PROPOSER_PROMISES) break;
        usleep(1000); assert(i < 4999);
    }
    assert(runtime->requests.count == 2 && runtime->agreement.promised > ballot && !runtime->agreement.committed.revision);
    assert(g_state.config_application_fenced && g_state.config_application_remapping && !g_state.resource_generation_admitted);
    cleanup_durable_runtime();
}

static void test_activating_status_codec(void)
{
    initialize(); lcs_config_status_view_t view, decoded;
    assert(!config_peer_view(&view)); view.authority = LCS_CONFIG_AUTHORITY_JSON;
    view.committed_revision = 1; memset(view.committed_digest, 'a', 64); view.committed_digest[64] = 0;
    memset(view.applied_digest, 'b', 64); view.applied_digest[64] = 0;
    strcpy(view.error, "baseline cleanup requires repair");
    const uint16_t phases[] = {LCS_CONFIG_DIAG_RECOVERING, LCS_CONFIG_DIAG_BLOCKED, LCS_CONFIG_DIAG_FAILED, LCS_CONFIG_DIAG_STOPPING};
    unsigned char payload[8192]; size_t length;
    for (size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); i++) for (unsigned pending = 0; pending < 2; pending++)
    {
        view.phase = phases[i]; view.pending_revision = pending;
        strcpy(view.pending_digest, pending ? view.committed_digest : "");
        assert(!lcs_config_status_view_encode(&view, payload, sizeof(payload), &length) && payload[1] == 5);
        assert(!lcs_config_status_view_decode(payload, length, &decoded) && !decoded.applied_revision &&
            decoded.committed_revision == 1 && !decoded.admitted && !decoded.dynamic_commands);
        for (size_t n = 0; n < length; n++) assert(lcs_config_status_view_decode(payload, n, &decoded) < 0);
        payload[1] = 2; assert(lcs_config_status_view_decode(payload, length, &decoded) < 0);
        payload[1] = 4; assert(lcs_config_status_view_decode(payload, length, &decoded) < 0);
    }
    view.committed_revision = 2; assert(lcs_config_status_view_encode(&view, payload, sizeof(payload), &length) < 0);
    view.committed_revision = 1; view.phase = LCS_CONFIG_DIAG_ACTIVE; view.admitted = 1;
    assert(lcs_config_status_view_encode(&view, payload, sizeof(payload), &length) < 0);
    view.phase = LCS_CONFIG_DIAG_ADMITTING; view.admitted = 0;
    assert(lcs_config_status_view_encode(&view, payload, sizeof(payload), &length) < 0);
    view.phase = LCS_CONFIG_DIAG_BLOCKED; view.dynamic_commands = 1;
    assert(lcs_config_status_view_encode(&view, payload, sizeof(payload), &length) < 0);
}

static void test_automatic_readiness_and_restart(void)
{
    initialize(); g_state.self_index = 0; g_state.instance_id = 100;
    strcpy(readiness_directory, "/tmp/lcs-auto-readiness.XXXXXX"); assert(mkdtemp(readiness_directory));
    char error[256]; assert(!lcs_config_store_open(&readiness_store, readiness_directory, error, sizeof(error)));
    g_state.config_bootstrap.state = readiness_store;
    lcs_config_runtime_t *runtime = &g_state.config_runtime;
    runtime->base = malloc(sizeof(*runtime->base)); assert(runtime->base);
    assert(!lcs_config_activation_open_requests(&runtime->activation, &g_state.config_bootstrap.state,
        &g_state.cfg, runtime->base, true, error, sizeof(error)));
    assert(!lcs_config_activation_resume(&runtime->activation, &runtime->agreement, &runtime->application, error, sizeof(error)));
    lcs_config_runtime_close(runtime);
    assert(!lcs_config_runtime_open(runtime, &g_state.config_bootstrap.state, &g_state.cfg, error, sizeof(error)));
    memset(g_state.resources, 0, sizeof(g_state.resources)); memset(g_state.lease_grants, 0, sizeof(g_state.lease_grants));
    g_state.config_status.serial = 0; assert(!config_peer_resume_frozen(error, sizeof(error)));
    g_state.config_initialization_auto_resume = true;
    establish(1, 6); g_state.peers[1].instance_id = 200; config_peer_process(-1);
    assert(g_state.config_coordinator.phase == LCS_CONFIG_COORD_IDLE && !runtime->barrier.store);
    establish(2, 6); g_state.peers[2].instance_id = 300; g_state.config_readiness_retry_ms = 0;
    config_peer_process(-1); assert(g_state.config_coordinator.phase == LCS_CONFIG_COORD_COLLECTING);
    lcs_config_readiness_message_t old = last_ready[1]; uint64_t attempt = old.attempt;
    assert(lcs_config_coordinator_reset_readiness(&g_state.config_coordinator, lcs_now_ms()) < 0);
    g_state.config_readiness.deadline_ms = lcs_now_ms() - 1; config_peer_process(-1);
    assert(g_state.config_coordinator.phase == LCS_CONFIG_COORD_IDLE && !g_state.config_coordinator.failed &&
        !g_state.config_coordinator.closing && g_state.config_readiness.round.failed && !runtime->barrier.store);
    g_state.config_readiness_retry_ms = 0; config_peer_process(-1);
    assert(g_state.config_readiness.query.attempt > attempt);
    old.kind = LCS_READINESS_REPLY; old.status = LCS_READINESS_READY; old.responder_instance = 200;
    unsigned char payload[LCS_CONFIG_READINESS_MAX]; size_t length;
    assert(!lcs_config_readiness_encode(&old, payload, sizeof(payload), &length));
    assert(!config_peer_receive_readiness(-1, 1, LCS_MSG_CONFIG_READINESS_REPLY, payload, length));
    assert(g_state.config_readiness.round.ready == 1); /* Old attempt cannot be reused. */
    for (size_t node = 1; node <= 2; node++)
    {
        lcs_config_readiness_message_t reply = last_ready[node]; reply.kind = LCS_READINESS_REPLY;
        reply.status = LCS_READINESS_READY; reply.responder_instance = g_state.peers[node].instance_id;
        assert(!lcs_config_readiness_encode(&reply, payload, sizeof(payload), &length));
        assert(!config_peer_receive_readiness(-1, (int)node, LCS_MSG_CONFIG_READINESS_REPLY, payload, length));
        if (node == 1) { config_peer_process(-1); assert(!g_state.config_coordinator.storage.pid && !runtime->barrier.store); }
    }
    stop_freeze_sync = true; config_peer_process(-1);
    pid_t child = g_state.config_coordinator.storage.pid; assert(child > 0 && !runtime->barrier.store);
    assert(g_state.config_coordinator.phase == LCS_CONFIG_COORD_PERSISTING && !config_peer_configuration_vote_ready());
    assert(lcs_config_coordinator_reset_readiness(&g_state.config_coordinator, UINT64_MAX) < 0);
    int status; pid_t result = 0;
    for (unsigned i = 0; i < 5000 && !result; i++)
    { result = waitpid(child, &status, WNOHANG | WUNTRACED); if (!result) usleep(1000); }
    assert(result == child && WIFSTOPPED(status)); assert(!kill(child, SIGCONT)); stop_freeze_sync = false;
    for (unsigned i = 0; i < 5000 && g_state.config_coordinator.storage.pid; i++) { config_peer_process(-1); usleep(1000); }
    assert(lcs_config_runtime_readiness_prepared(runtime) && !lcs_config_runtime_initialization_prepared(runtime));
    assert(config_peer_configuration_vote_ready() && g_state.config_coordinator.phase == LCS_CONFIG_COORD_READY);
    assert(g_state.config_application_remapping && g_state.config_application_fenced && !g_state.resource_generation_enabled && !g_state.voting_ready);
    lcs_config_status_view_t view; assert(!config_peer_view(&view) && view.phase == LCS_CONFIG_DIAG_FROZEN && !view.admitted && strstr(view.error, "readiness persisted"));
    char *record = NULL; size_t bytes;
    assert(!lcs_config_store_read(&readiness_store, "barrier.json", &record, &bytes, LCS_CONFIG_BARRIER_MAX, error, sizeof(error)));
    config_peer_stop(); assert(config_peer_stopped()); lcs_config_runtime_close(runtime);
    memset(&g_state.config_coordinator, 0, sizeof(g_state.config_coordinator));
    memset(&g_state.config_readiness, 0, sizeof(g_state.config_readiness));
    g_state.config_status.serial = 0; g_state.config_status_generation = 0; g_state.instance_id++;
    for (size_t i = 0; i < 3; i++) g_state.peers[i].fd = -1;
    strcpy(g_state.cfg.resources[0].address, "192.0.2.99/32");
    assert(!lcs_config_runtime_open(runtime, &g_state.config_bootstrap.state, &g_state.cfg, error, sizeof(error)));
    assert(lcs_config_runtime_readiness_prepared(runtime)); assert(!config_peer_resume_frozen(error, sizeof(error)));
    config_peer_process(-1);
    assert(config_peer_configuration_vote_ready() && !g_state.config_readiness.active && !g_state.config_coordinator.storage.pid);
    assert(!strcmp(g_state.cfg.resources[0].address, "192.0.2.10/32") && resource_config_is_fenced(0));
    char *again = NULL; size_t again_bytes;
    assert(!lcs_config_store_read(&readiness_store, "barrier.json", &again, &again_bytes, LCS_CONFIG_BARRIER_MAX, error, sizeof(error)));
    assert(bytes == again_bytes && !memcmp(record, again, bytes)); free(record); free(again);
    /* A crash after reservation, PREPARE or ACCEPT must retain that exact
     * undecided activation evidence, never reset it to pristine readiness. */
    lcs_config_t *candidate = malloc(sizeof(*candidate)); assert(candidate);
    assert(!lcs_resource_config_assign_identities(runtime->base, 0, runtime->base, candidate, error, sizeof(error)));
    char *document = NULL; size_t document_length;
    assert(!lcs_resource_config_encode(candidate, 1, &document, &document_length, error, sizeof(error))); free(candidate);
    lcs_config_value_t accepted = {0};
    assert(!lcs_config_value_init(&accepted, runtime->base, "restart-activation", document, document_length, error, sizeof(error))); free(document);
    const char *records[] = {"activation.json", "agreement.json", "application.json", "requests.json", "barrier.json"};
    for (unsigned stage = 0; stage < 3; stage++)
    {
        if (!stage) assert(!lcs_config_requests_reserve(&runtime->requests, &runtime->agreement, 0,
            runtime->agreement.committed.digest, &accepted, error, sizeof(error)));
        if (stage == 1) assert(!lcs_config_agreement_prepare(&runtime->agreement, 1,
            runtime->agreement.committed.digest, 64, error, sizeof(error)));
        if (stage == 2) assert(!lcs_config_agreement_accept(&runtime->agreement,
            runtime->agreement.committed.digest, 64, &accepted, error, sizeof(error)));
        assert(!lcs_config_runtime_readiness_prepared(runtime) && lcs_config_runtime_activation_pending(runtime));
        runtime->requests.poisoned = true; assert(!lcs_config_runtime_activation_pending(runtime)); runtime->requests.poisoned = false;
        runtime->requests.records[0].applied = true; assert(!lcs_config_runtime_activation_pending(runtime)); runtime->requests.records[0].applied = false;
        runtime->barrier.ready = 1; assert(!lcs_config_runtime_activation_pending(runtime)); runtime->barrier.ready = 7;
        if (stage == 2)
        { runtime->requests.count = 0; assert(!lcs_config_runtime_activation_pending(runtime)); runtime->requests.count = 1; }
        char *before[5]; size_t sizes[5];
        for (size_t i = 0; i < 5; i++)
            assert(!lcs_config_store_read(&readiness_store, records[i], &before[i], &sizes[i], LCS_CONFIG_ACTIVATION_MAX, error, sizeof(error)));
        config_peer_stop(); assert(config_peer_stopped()); lcs_config_runtime_close(runtime);
        memset(&g_state.config_coordinator, 0, sizeof(g_state.config_coordinator));
        memset(&g_state.config_readiness, 0, sizeof(g_state.config_readiness));
        g_state.config_status.serial = 0; g_state.config_status_generation = 0; g_state.instance_id++;
        for (size_t i = 0; i < 3; i++) g_state.peers[i].fd = -1;
        assert(!lcs_config_runtime_open(runtime, &g_state.config_bootstrap.state, &g_state.cfg, error, sizeof(error)));
        assert(lcs_config_runtime_activation_pending(runtime) && !lcs_config_runtime_readiness_prepared(runtime));
        assert(!config_peer_resume_frozen(error, sizeof(error))); config_peer_process(-1);
        assert(config_peer_configuration_vote_ready() && !g_state.config_readiness.active && !g_state.config_coordinator.storage.pid);
        assert(runtime->requests.count == 1 && runtime->agreement.promised == (stage ? 64 : 0));
        if (stage == 2) assert(lcs_config_value_equal(&runtime->agreement.accepted, &accepted));
        assert(resource_config_is_fenced(0) && !g_state.voting_ready && !g_state.resource_generation_admitted);
        assert(!config_peer_view(&view) && view.phase == LCS_CONFIG_DIAG_FROZEN && !view.admitted);
        for (size_t i = 0; i < 5; i++)
        {
            char *after; size_t size;
            assert(!lcs_config_store_read(&readiness_store, records[i], &after, &size, LCS_CONFIG_ACTIVATION_MAX, error, sizeof(error)));
            assert(size == sizes[i] && !memcmp(after, before[i], size)); free(after); free(before[i]);
        }
        if (stage) assert(lcs_config_agreement_prepare(&runtime->agreement, 1,
            runtime->agreement.committed.digest, 32, error, sizeof(error)) != 0);
    }
    lcs_config_value_clear(&accepted);
    config_peer_stop(); lcs_config_runtime_close(runtime);
    const char *names[] = {"activation.json", "agreement.json", "application.json", "requests.json", "barrier.json", "lock"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) assert(!unlinkat(readiness_store.directory_fd, names[i], 0));
    lcs_config_store_close(&readiness_store); assert(!rmdir(readiness_directory));
}

int main(void)
{
    test_metadata_and_reload();
    test_strict_codecs();
    test_dynamic_status_codec();
    test_activating_status_codec();
    test_automatic_activation_proposal();
    test_readiness_codecs();
    test_readiness_round();
    test_explicit_peer_readiness();
    test_owner_transport_admission();
    test_freeze_codec();
    test_freeze_peer_admission();
    test_durable_freeze_exchange();
    test_freeze_failure_and_shutdown();
    test_frozen_startup_and_status();
    test_coordinated_local_initialization();
    test_initialization_interruption();
    test_automatic_readiness_and_restart();
    puts("configuration peer metadata and explicit readiness unit tests passed");
    return 0;
}
