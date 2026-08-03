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
#define LCS_CLI_SERVER_MAX 32
#define LCS_CLI_SERVER_INBUF_SIZE  LCS_PEER_INBUF_SIZE
#define LCS_CLI_SERVER_OUTBUF_SIZE LCS_PEER_INBUF_SIZE
#define LCS_MOVE_OP_MAX 16
#define LCS_MOVE_REQ_PAYLOAD_SIZE ((LCS_NAME_MAX + 1u) * 2u)
#define LCS_MOVE_RESP_PAYLOAD_SIZE 256u
#define LCS_LEASE_OP_MAX LCS_MAX_RESOURCES
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

typedef enum
{
    LCS_SERVICE_OP_NONE = 0,
    LCS_SERVICE_OP_START,
    LCS_SERVICE_OP_STOP,
    LCS_SERVICE_OP_ROLLBACK_STOP,
    LCS_SERVICE_OP_HEALTH,
    LCS_SERVICE_OP_STARTUP_CLEANUP,
    LCS_SERVICE_OP_STATE_REPLACE,
} resource_service_op_type_t;

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
    uint64_t home_generation;
    uint64_t disabled_generation;
    bool failover_pending;
    bool home_blocked;
    bool disabled;
    bool shutdown_release_required;
    bool shutdown_release_confirmed;
    bool startup_cleanup_failed;
    bool startup_cleanup_broadcast_pending;
    uint64_t next_startup_cleanup_attempt_ms;
    pid_t hook_pid;
    resource_hook_type_t hook_type;
    uint64_t hook_deadline_ms;
    uint64_t hook_epoch;
    uint64_t hook_lease_id;
    pid_t service_pid;
    resource_service_op_type_t service_op;
    uint64_t service_deadline_ms;
    uint64_t service_epoch;
    uint64_t service_lease_id;
    uint64_t next_service_health_ms;
    bool service_stop_post_hook;
    bool service_handoff;
    int service_handoff_source_node;
    uint32_t service_handoff_response_seq;
    int service_replace_owner_node;
    uint64_t service_replace_owner_instance_id;
    lcs_resource_state_t service_replace_state;
    uint64_t service_replace_epoch;
    uint64_t service_replace_lease_id;
    uint64_t service_replace_deadline_ms;
    char service_replace_reason[LCS_REASON_MAX + 1];
    char conflict_reason[LCS_REASON_MAX + 1];
} resource_runtime_t;

/* Private voter promise.  This must never be serialized as resource state. */
typedef struct
{
    bool active;
    int owner_node;
    uint64_t owner_instance_id;
    uint64_t epoch;
    uint64_t lease_id;
    uint64_t deadline_ms;
    uint64_t promised_epoch;
} lease_grant_t;

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
    bool detached;
    unsigned char detached_resp[LCS_LEASE_RESP_PAYLOAD_SIZE];
    uint32_t detached_resp_len;
} peer_rpc_runtime_t;

typedef struct
{
    bool online;
    bool voting_ready;
    bool initial_sync_complete;
    int fd;
    peer_conn_state_t conn_state;
    bool outbound;
    peer_rpc_runtime_t in_flight[LCS_MAX_PEER_RPC_INFLIGHT];
    uint64_t instance_id;
    uint64_t last_seen_ms;
    uint64_t next_connect_attempt_ms;
    uint64_t next_heartbeat_ms;
    bool state_sync_pending;
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
    bool voting_ready;
    bool reject_after_flush;
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
} cli_server_runtime_t;

typedef enum
{
    LCS_MOVE_ORIGIN_NONE = 0,
    LCS_MOVE_ORIGIN_CLI_SERVER,
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
    int cli_server_slot;
    uint64_t cli_server_id;
    uint32_t cli_server_seq;
    int source_node_idx;
    uint32_t peer_seq;
    int resource_idx;
    int target_idx;
    int old_owner_idx;
    uint64_t old_epoch;
    uint64_t old_lease_id;
    uint64_t epoch;
    uint64_t lease_id;
    uint64_t grant_deadline_ms;
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
    int resource_idx;
    int owner_idx;
    uint64_t epoch;
    uint64_t lease_id;
    uint64_t grant_deadline_ms;
    uint64_t deadline_ms;
    int votes;
    int pending_rpcs;
    bool release_notify;
    bool release_notified;
    int release_response_node;
    uint32_t release_response_seq;
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
    bool voting_ready;
    uint64_t voting_not_before_ms;
    uint32_t quorum_needed;
    uint32_t votes_seen;
    uint64_t started_ms;
    uint64_t next_placement_ms;
    uint64_t membership_mask;
    uint64_t membership_since_ms;
    bool had_quorum;
    bool no_quorum_state_cleared;
    peer_runtime_t peers[LCS_MAX_NODES];
    inbound_handshake_t handshakes[LCS_HANDSHAKE_MAX];
    cli_server_runtime_t cli_servers[LCS_CLI_SERVER_MAX];
    uint64_t next_cli_server_id;
    uint64_t next_move_id;
    move_runtime_t moves[LCS_MOVE_OP_MAX];
    uint64_t next_lease_op_id;
    lease_runtime_t lease_ops[LCS_LEASE_OP_MAX];
    resource_runtime_t resources[LCS_MAX_RESOURCES];
    lease_grant_t lease_grants[LCS_MAX_RESOURCES];
} daemon_state_t;

extern daemon_state_t g_state;

enum
{
    LCS_EPOLL_CLI_SERVER_LISTENER            = 1,
    LCS_EPOLL_PEER             = 2,
    LCS_EPOLL_METRICS          = 3,
    LCS_EPOLL_PEER_CONN_BASE   = 1000,
    LCS_EPOLL_HANDSHAKE_BASE   = 5000,
    LCS_EPOLL_CLI_SERVER_BASE = 9000,
};

enum
{
    LCS_HELLO_MODE_PERSISTENT = 1,
};

#endif
