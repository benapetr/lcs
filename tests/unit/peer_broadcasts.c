// SPDX-License-Identifier: GPL-3.0-or-later

#include "peer.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdarg.h>
#include <string.h>

daemon_state_t g_state;
static unsigned char output[4][LCS_PEER_OUTBUF_SIZE];
static int snapshot_count;

int cluster_encode_state(unsigned char *payload, size_t cap, size_t *len)
{
    assert(cap >= 1);
    payload[0] = 42;
    *len = 1;
    snapshot_count++;
    return 0;
}

void lcs_log_debug(const char *fmt, ...)
{
    (void)fmt;
}

void lcs_log_debug3(const char *fmt, ...)
{
    (void)fmt;
}

static void test_broadcasts_wait_for_handshake(void)
{
    memset(&g_state, 0, sizeof(g_state));
    g_state.cfg.node_count = 4;
    g_state.self_index = 0;
    const peer_conn_state_t states[] = {
        LCS_PEER_DISCONNECTED, LCS_PEER_CONNECTING, LCS_PEER_HELLO_SENT,
    };
    unsigned char commit[] = { 1, 2, 3 };

    for (size_t state = 0; state < sizeof(states) / sizeof(states[0]); state++)
    {
        snapshot_count = 0;
        for (size_t i = 0; i < g_state.cfg.node_count; i++)
        {
            peer_runtime_t *peer = &g_state.peers[i];
            peer->fd = i == 3 ? -1 : (int)i;
            peer->conn_state = i == 2 ? LCS_PEER_ESTABLISHED : states[state];
            peer->outbuf = output[i];
            peer->out_len = peer->out_off = 0;
            peer->state_sync_pending = true;
        }

        /* A renewal on a healthy session must not leak onto a new socket,
         * either ahead of HELLO or while HELLO_ACK is still outstanding. */
        peer_broadcast_lease_commit(-1, commit, sizeof(commit));
        peer_broadcast_state_sync(-1);
        assert(g_state.peers[0].out_len == 0);
        assert(g_state.peers[1].out_len == 0);
        assert(g_state.peers[1].state_sync_pending);
        assert(g_state.peers[3].out_len == 0);
        assert(snapshot_count == 1);

        peer_runtime_t *established = &g_state.peers[2];
        lcs_frame_header_t wire;
        memcpy(&wire, established->outbuf, sizeof(wire));
        assert(ntohs(wire.type) == LCS_MSG_LEASE_COMMIT);
        assert(ntohl(wire.length) == sizeof(commit));
        assert(memcmp(established->outbuf + sizeof(wire), commit, sizeof(commit)) == 0);
        memcpy(&wire, established->outbuf + sizeof(wire) + sizeof(commit), sizeof(wire));
        assert(ntohs(wire.type) == LCS_MSG_STATE_SYNC_REQ);
        assert(!established->state_sync_pending);

        /* Once that same session finishes HELLO, broadcasts resume. */
        g_state.peers[1].conn_state = LCS_PEER_ESTABLISHED;
        peer_broadcast_lease_commit(-1, commit, sizeof(commit));
        assert(g_state.peers[1].out_len == sizeof(wire) + sizeof(commit));
    }
}

static void test_config_status_feature_gate(void)
{
    memset(&g_state, 0, sizeof(g_state));
    g_state.cfg.node_count = 2;
    peer_runtime_t *peer = &g_state.peers[1];
    peer->fd = 1;
    peer->outbuf = output[1];
    unsigned char payload[] = {1};
    const peer_conn_state_t states[] = {LCS_PEER_DISCONNECTED, LCS_PEER_CONNECTING, LCS_PEER_HELLO_SENT};
    peer->protocol_version = 6;
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++)
    {
        peer->conn_state = states[i];
        assert(peer_queue_config_status(-1, 1, payload, sizeof(payload)) == 1);
        assert(peer_queue_config_readiness(-1, 1, LCS_MSG_CONFIG_READINESS_QUERY, payload, sizeof(payload)) == 1);
        assert(peer_queue_config_readiness(-1, 1, LCS_MSG_CONFIG_READINESS_REPLY, payload, sizeof(payload)) == 1);
        assert(peer_queue_config_transaction(-1, 1, LCS_MSG_CONFIG_TX_CONTROL, payload, sizeof(payload)) == 1);
        assert(!peer->out_len);
    }
    peer->conn_state = LCS_PEER_ESTABLISHED;
    peer->protocol_version = 5;
    assert(peer_queue_config_status(-1, 1, payload, sizeof(payload)) == 1);
    assert(peer_queue_config_readiness(-1, 1, LCS_MSG_CONFIG_READINESS_QUERY, payload, sizeof(payload)) == 1);
    assert(peer_queue_config_readiness(-1, 1, LCS_MSG_CONFIG_READINESS_REPLY, payload, sizeof(payload)) == 1);
    assert(peer_queue_config_transaction(-1, 1, LCS_MSG_CONFIG_TX_CONTROL, payload, sizeof(payload)) == 1);
    assert(!peer->out_len);
    peer->protocol_version = 6;
    assert(peer_queue_config_status(-1, 1, payload, sizeof(payload)) == 0);
    lcs_frame_header_t header;
    memcpy(&header, peer->outbuf, sizeof(header));
    assert(ntohs(header.type) == LCS_MSG_CONFIG_BOOTSTRAP_STATUS);
    peer->out_len = 0;
    assert(!peer_queue_config_readiness(-1, 1, LCS_MSG_CONFIG_READINESS_QUERY, payload, sizeof(payload)));
    memcpy(&header, peer->outbuf, sizeof(header));
    assert(ntohs(header.type) == LCS_MSG_CONFIG_READINESS_QUERY);
    peer->out_len = 0;
    assert(!peer_queue_config_readiness(-1, 1, LCS_MSG_CONFIG_READINESS_REPLY, payload, sizeof(payload)));
    memcpy(&header, peer->outbuf, sizeof(header));
    assert(ntohs(header.type) == LCS_MSG_CONFIG_READINESS_REPLY);
    peer->out_len = 0;
    assert(!peer_queue_config_transaction(-1, 1, LCS_MSG_CONFIG_TX_CONTROL, payload, sizeof(payload)));
    memcpy(&header, peer->outbuf, sizeof(header));
    assert(ntohs(header.type) == LCS_MSG_CONFIG_TX_CONTROL);
}

static void test_resource_generation_admission(void)
{
    memset(&g_state, 0, sizeof(g_state));
    g_state.cfg.node_count = 2; g_state.self_index = 0; g_state.instance_id = 100;
    g_state.resource_generation_enabled = g_state.resource_generation_admitted = true;
    g_state.resource_generation.revision = 7;
    memset(g_state.resource_generation.membership, 'a', 64);
    memset(g_state.resource_generation.digest, 'b', 64);
    peer_runtime_t *peer = &g_state.peers[1];
    peer->fd = 1; peer->conn_state = LCS_PEER_ESTABLISHED; peer->protocol_version = 6;
    peer->instance_id = 101; peer->outbuf = output[1];
    unsigned char message[] = {1, 2, 3}, payload[256]; size_t length;
    /* Configuration traffic is independent of resource-generation matching. */
    assert(!peer_queue_config_transaction(-1, 1, LCS_MSG_CONFIG_TX_CONTROL, message, sizeof(message)));
    peer->out_len = 0;
    peer_broadcast_lease_commit(-1, message, sizeof(message));
    assert(!peer->out_len);
    lcs_frame_header_t wire = {.type = LCS_MSG_HEARTBEAT, .seq = 50, .length = 1}, inner;
    const void *body;
    peer->voting_ready = peer->initial_sync_complete = true;
    assert(peer_decode_resource_frame(1, &wire, message, &inner, &body) == 1);
    assert(!peer->voting_ready && !peer->initial_sync_complete);
    assert(!lcs_resource_frame_prefix(&g_state.resource_generation, 101, 100, LCS_MSG_HEARTBEAT,
                                        1, payload, sizeof(payload), &length));
    payload[length++] = 1;
    wire.type = LCS_MSG_RESOURCE_FRAME; wire.length = (uint32_t)length;
    assert(!peer_decode_resource_frame(1, &wire, payload, &inner, &body));
    assert(peer->resource_generation_matches && inner.type == LCS_MSG_HEARTBEAT && inner.seq == 50 && inner.length == 1);
    assert(*(const unsigned char *)body == 1);
    peer_broadcast_lease_commit(-1, message, sizeof(message));
    lcs_frame_header_t output_header;
    memcpy(&output_header, peer->outbuf, sizeof(output_header));
    assert(ntohs(output_header.type) == LCS_MSG_RESOURCE_FRAME);
    lcs_resource_generation_t decoded; uint16_t type; size_t count;
    assert(!lcs_resource_frame_decode(peer->outbuf + sizeof(output_header), ntohl(output_header.length),
        100, 101, &decoded, &type, &body, &count));
    assert(type == LCS_MSG_LEASE_COMMIT && count == sizeof(message) && !memcmp(body, message, count));
    /* Inner replies retain their original RPC type and sequence after validation. */
    assert(!lcs_resource_frame_prefix(&g_state.resource_generation, 101, 100, LCS_MSG_LEASE_ACK,
                                        0, payload, sizeof(payload), &length));
    wire.length = (uint32_t)length;
    assert(!peer_decode_resource_frame(1, &wire, payload, &inner, &body) && inner.type == LCS_MSG_LEASE_ACK);
    assert(inner.seq == wire.seq && !inner.length);
    /* A generation cannot change in place on one socket. */
    lcs_resource_generation_t newer = g_state.resource_generation; newer.revision++;
    assert(!lcs_resource_frame_prefix(&newer, 101, 100, LCS_MSG_LEASE_ACK, 0, payload, sizeof(payload), &length));
    wire.length = (uint32_t)length;
    assert(peer_decode_resource_frame(1, &wire, payload, &inner, &body) < 0);
    /* A different generation on a fresh session leaves the config channel open. */
    peer->resource_generation_known = false;
    peer->voting_ready = peer->initial_sync_complete = true;
    assert(peer_decode_resource_frame(1, &wire, payload, &inner, &body) == 1);
    assert(!peer->resource_generation_matches && !peer->voting_ready && !peer->initial_sync_complete);
    wire.type = LCS_MSG_CONFIG_TX_CONTROL; wire.length = sizeof(message);
    assert(!peer_decode_resource_frame(1, &wire, message, &inner, &body));
    /* Neither legacy local authority nor wire 5 may consume wrapped resource state. */
    peer->resource_generation_known = false; g_state.resource_generation_enabled = false;
    wire.type = LCS_MSG_RESOURCE_FRAME; wire.length = (uint32_t)length;
    assert(peer_decode_resource_frame(1, &wire, payload, &inner, &body) == 1);
    wire.type = LCS_MSG_HEARTBEAT; wire.length = 1;
    assert(peer_decode_resource_frame(1, &wire, message, &inner, &body) == 1);
    wire.type = LCS_MSG_RESOURCE_FRAME; wire.length = (uint32_t)length;
    peer->protocol_version = 5;
    assert(peer_decode_resource_frame(1, &wire, payload, &inner, &body) < 0);
    g_state.resource_generation_enabled = true; g_state.resource_generation_admitted = false;
    peer->protocol_version = 6; peer->out_len = 0;
    peer_broadcast_lease_commit(-1, message, sizeof(message));
    assert(!peer->out_len);
    g_state.resource_generation_admitted = true; g_state.config_application_remapping = true;
    peer->state_sync_pending = true;
    peer_broadcast_state_sync(-1);
    assert(!peer->out_len && peer->state_sync_pending);
}

int main(void)
{
    test_broadcasts_wait_for_handshake();
    test_config_status_feature_gate();
    test_resource_generation_admission();
    return 0;
}
