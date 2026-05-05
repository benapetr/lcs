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
#define LCS_MAX_PEER_RPC_INFLIGHT 8
#define LCS_LOCAL_CLIENT_MAX 32
#define LCS_LOCAL_CLIENT_INBUF_SIZE  LCS_PEER_INBUF_SIZE
#define LCS_LOCAL_CLIENT_OUTBUF_SIZE LCS_PEER_INBUF_SIZE
#define LCS_MOVE_OP_MAX 16
#define LCS_MOVE_REQ_PAYLOAD_SIZE ((LCS_NAME_MAX + 1u) * 2u)
#define LCS_MOVE_RESP_PAYLOAD_SIZE 256u
#define LCS_LEASE_OP_MAX LCS_MAX_VIPS
#define LCS_LEASE_RESP_PAYLOAD_SIZE 256u

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

typedef void (*peer_rpc_callback_t)(void *ctx, int status, const unsigned char *payload, uint32_t len);

typedef struct
{
    bool active;
    bool done;
    int status;
    int peer_idx;
    uint32_t seq;
    uint16_t req_type;
    uint16_t expected_type;
    uint64_t deadline_ms;
    unsigned char *resp_payload;
    size_t resp_cap;
    uint32_t *resp_len;
    peer_rpc_callback_t callback;
    void *callback_ctx;
} peer_rpc_runtime_t;

typedef struct
{
    bool online;
    int fd;
    peer_conn_state_t conn_state;
    bool outbound;
    peer_rpc_runtime_t in_flight[LCS_MAX_PEER_RPC_INFLIGHT];
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
    bool active;
    int fd;
    uint64_t id;
    uint64_t deadline_ms;
    unsigned char *inbuf;
    size_t in_len;
    unsigned char *outbuf;
    size_t out_off;
    size_t out_len;
    bool close_after_flush;
} local_client_runtime_t;

typedef enum
{
    LCS_MOVE_ORIGIN_NONE = 0,
    LCS_MOVE_ORIGIN_LOCAL_CLIENT,
    LCS_MOVE_ORIGIN_PEER,
} move_origin_t;

typedef enum
{
    LCS_MOVE_PHASE_NONE = 0,
    LCS_MOVE_PHASE_WAIT_TARGET,
    LCS_MOVE_PHASE_PREPARE_TARGET,
    LCS_MOVE_PHASE_WAIT_OWNER_RELEASE,
    LCS_MOVE_PHASE_WAIT_OLD_LEASE_EXPIRY,
    LCS_MOVE_PHASE_WAIT_LEASE,
    LCS_MOVE_PHASE_RELEASE_FAILED_LEASE,
} move_phase_t;

struct move_runtime;

typedef struct
{
    struct move_runtime *move;
    uint64_t move_id;
    int node_idx;
} move_rpc_context_t;

typedef struct move_runtime
{
    bool active;
    uint64_t id;
    move_origin_t origin;
    move_phase_t phase;
    uint64_t deadline_ms;
    uint64_t wait_until_ms;
    int local_client_slot;
    uint64_t local_client_id;
    uint32_t client_seq;
    int source_node_idx;
    uint32_t peer_seq;
    int vip_idx;
    int target_idx;
    int old_owner_idx;
    uint64_t old_epoch;
    uint64_t old_lease_id;
    uint64_t epoch;
    uint64_t lease_id;
    int votes;
    int pending_rpcs;
    bool peer_done;
    int peer_status;
    unsigned char peer_resp[LCS_MOVE_RESP_PAYLOAD_SIZE];
    uint32_t peer_resp_len;
    bool rpc_done[LCS_MAX_NODES];
    int rpc_status[LCS_MAX_NODES];
    bool lease_acked[LCS_MAX_NODES];
    unsigned char rpc_resp[LCS_MAX_NODES][LCS_MOVE_RESP_PAYLOAD_SIZE];
    uint32_t rpc_resp_len[LCS_MAX_NODES];
    move_rpc_context_t rpc_ctx[LCS_MAX_NODES];
    int32_t final_status;
    char final_message[128];
} move_runtime_t;

typedef enum
{
    LCS_LEASE_OP_NONE = 0,
    LCS_LEASE_OP_ACQUIRE,
    LCS_LEASE_OP_RENEW,
    LCS_LEASE_OP_RELEASE,
} lease_op_type_t;

struct lease_runtime;

typedef struct
{
    struct lease_runtime *op;
    uint64_t op_id;
    int node_idx;
} lease_rpc_context_t;

typedef struct lease_runtime
{
    bool active;
    uint64_t id;
    lease_op_type_t type;
    int vip_idx;
    int owner_idx;
    uint64_t epoch;
    uint64_t lease_id;
    uint64_t deadline_ms;
    int votes;
    int pending_rpcs;
    bool rpc_done[LCS_MAX_NODES];
    int rpc_status[LCS_MAX_NODES];
    bool acked[LCS_MAX_NODES];
    unsigned char rpc_resp[LCS_MAX_NODES][LCS_LEASE_RESP_PAYLOAD_SIZE];
    uint32_t rpc_resp_len[LCS_MAX_NODES];
    lease_rpc_context_t rpc_ctx[LCS_MAX_NODES];
} lease_runtime_t;

typedef struct
{
    lcs_config_t cfg;
    int self_index;
    uint64_t instance_id;
    uint32_t quorum_needed;
    uint32_t votes_seen;
    peer_runtime_t peers[LCS_MAX_NODES];
    inbound_handshake_t handshakes[LCS_HANDSHAKE_MAX];
    local_client_runtime_t local_clients[LCS_LOCAL_CLIENT_MAX];
    uint64_t next_local_client_id;
    uint64_t next_move_id;
    move_runtime_t moves[LCS_MOVE_OP_MAX];
    uint64_t next_lease_op_id;
    lease_runtime_t lease_ops[LCS_LEASE_OP_MAX];
    resource_runtime_t resources[LCS_MAX_VIPS];
} daemon_state_t;

enum
{
    LCS_EPOLL_LOCAL            = 1,
    LCS_EPOLL_PEER             = 2,
    LCS_EPOLL_METRICS          = 3,
    LCS_EPOLL_PEER_CONN_BASE   = 1000,
    LCS_EPOLL_HANDSHAKE_BASE   = 5000,
    LCS_EPOLL_LOCAL_CLIENT_BASE = 9000,
};

enum
{
    LCS_HELLO_MODE_PERSISTENT = 1,
};

#endif
