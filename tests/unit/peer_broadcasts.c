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
        assert(!peer->out_len);
    }
    peer->conn_state = LCS_PEER_ESTABLISHED;
    peer->protocol_version = 5;
    assert(peer_queue_config_status(-1, 1, payload, sizeof(payload)) == 1);
    assert(!peer->out_len);
    peer->protocol_version = 6;
    assert(peer_queue_config_status(-1, 1, payload, sizeof(payload)) == 0);
    lcs_frame_header_t header;
    memcpy(&header, peer->outbuf, sizeof(header));
    assert(ntohs(header.type) == LCS_MSG_CONFIG_BOOTSTRAP_STATUS);
}

int main(void)
{
    test_broadcasts_wait_for_handshake();
    test_config_status_feature_gate();
    return 0;
}
