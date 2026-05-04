// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_DAEMON_STATE_H
#define LCS_DAEMON_STATE_H

#include "config.h"
#include "protocol.h"

#include <sys/types.h>

#define LCS_FRAME_HEADER_SIZE ((size_t)sizeof(lcs_frame_header_t))
#define LCS_PEER_INBUF_SIZE   (LCS_FRAME_HEADER_SIZE + LCS_MAX_FRAME)
#define LCS_PEER_OUTBUF_SIZE  ((LCS_FRAME_HEADER_SIZE + LCS_MAX_FRAME) * 4u)
#define LCS_HANDSHAKE_MAX     (LCS_MAX_NODES * 2)

typedef enum
{
    LCS_PEER_DISCONNECTED = 0,
    LCS_PEER_CONNECTING,
    LCS_PEER_HELLO_SENT,
    LCS_PEER_ESTABLISHED,
} peer_conn_state_t;

typedef enum
{
    LCS_HOOK_NONE = 0,
    LCS_HOOK_PRE_START,
    LCS_HOOK_POST_START,
    LCS_HOOK_PRE_STOP,
    LCS_HOOK_POST_STOP,
} resource_hook_type_t;

typedef struct
{
    lcs_resource_state_t state;
    int owner_node;
    uint64_t owner_instance_id;
    uint64_t epoch;
    uint64_t lease_id;
    uint64_t lease_deadline_ms;
    uint64_t renew_after_ms;
    uint64_t next_activation_attempt_ms;
    uint64_t failover_count;
    bool failover_pending;
    pid_t hook_pid;
    resource_hook_type_t hook_type;
    uint64_t hook_deadline_ms;
    uint64_t hook_epoch;
    uint64_t hook_lease_id;
    char conflict_reason[LCS_REASON_MAX + 1];
} resource_runtime_t;

typedef struct
{
    bool online;
    int fd;
    peer_conn_state_t conn_state;
    bool outbound;
    bool pending_active;
    bool pending_done;
    int pending_status;
    uint32_t pending_seq;
    uint16_t pending_expected_type;
    unsigned char *pending_resp_payload;
    size_t pending_resp_cap;
    uint32_t *pending_resp_len;
    uint64_t instance_id;
    uint64_t last_seen_ms;
    uint64_t next_sync_ms;
    uint64_t connect_deadline_ms;
    uint32_t hello_seq;
    uint32_t backoff_ms;
    uint32_t seen_request_seqs[LCS_SEQ_CACHE_SIZE];
    size_t seen_request_pos;
    unsigned char *inbuf;
    size_t in_len;
    unsigned char *outbuf;
    size_t out_off;
    size_t out_len;
} peer_runtime_t;

typedef struct
{
    bool active;
    int fd;
    uint64_t deadline_ms;
    unsigned char *inbuf;
    size_t in_len;
    unsigned char *outbuf;
    size_t out_off;
    size_t out_len;
    int node_idx;
    uint64_t instance_id;
} inbound_handshake_t;

typedef struct
{
    lcs_config_t cfg;
    int self_index;
    uint64_t instance_id;
    uint32_t quorum_needed;
    uint32_t votes_seen;
    peer_runtime_t peers[LCS_MAX_NODES];
    inbound_handshake_t handshakes[LCS_HANDSHAKE_MAX];
    resource_runtime_t resources[LCS_MAX_VIPS];
} daemon_state_t;

enum
{
    LCS_EPOLL_LOCAL            = 1,
    LCS_EPOLL_PEER             = 2,
    LCS_EPOLL_METRICS          = 3,
    LCS_EPOLL_PEER_CONN_BASE   = 1000,
    LCS_EPOLL_HANDSHAKE_BASE   = 5000,
};

enum
{
    LCS_HELLO_MODE_PERSISTENT = 1,
};

#endif
