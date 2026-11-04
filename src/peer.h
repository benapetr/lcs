// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_PEER_H
#define LCS_PEER_H

#include "daemon_state.h"

#include <stdint.h>
#include <sys/epoll.h>

/* Globals defined in lcsd.c */
extern int g_peer_listener_fd;

int peer_resolve_configured_addresses(char *error, size_t error_len);
void peer_dns_refresh_request(void);
void peer_dns_refresh_process(void);
void peer_dns_refresh_cancel(void);
uint16_t peer_effective_protocol_version(void);
void handshake_expire(int epoll_fd);
void handshake_close(int epoll_fd, int slot_idx, const char *reason);
void peer_close_connection(int epoll_fd, int node_idx, bool mark_offline, const char *reason);
int peer_rpc_async(int epoll_fd, int node_idx, uint16_t req_type,
                   const void *req_payload, uint32_t req_len,
                   uint16_t expected_type, unsigned char *resp_payload,
                   size_t resp_cap, uint32_t *resp_len, uint32_t timeout_ms,
                   peer_rpc_callback_t callback, void *callback_ctx);
void peer_detach_rpcs_by_context(void *callback_ctx);
int peer_queue_simple_resp(int epoll_fd, int node_idx,
                           uint32_t seq, uint16_t type, int32_t status,
                           const char *message);
/* Returns 1 when the session does not support metadata, 0 queued, -1 busy/error. */
int peer_queue_config_status(int epoll_fd, int node_idx, const void *payload, uint32_t length);
int peer_queue_config_readiness(int epoll_fd, int node_idx, uint16_t type,
                                 const void *payload, uint32_t length);
int peer_queue_config_transaction(int epoll_fd, int node_idx, uint16_t type,
                                   const void *payload, uint32_t length);
/* Fixed resource-generation envelope admission, before dispatch/RPC completion.
 * Returns 0 decoded/pass-through, 1 stale/ineligible (drop without dispatch), -1
 * malformed or session context changed. Config traffic remains available. */
int peer_decode_resource_frame(int node_idx, const lcs_frame_header_t *wire, const void *payload,
                                 lcs_frame_header_t *inner, const void **body);
/* Future install adapter only, BEFORE index remapping: target must be the durable
 * pending/applied value, workers drained, and INI reload excluded. Closes
 * sessions and cancels peer/lease/move operations; fences generation voting until
 * the exact applied marker is durable, live cfg matches it, and admit is explicitly
 * called. Install returns 1 while indexed workers remain, -1 rejected, 0 installed. */
int peer_install_resource_generation(int epoll_fd, const lcs_config_value_t *value);
int peer_admit_resource_generation(void);
void peer_broadcast_state_sync(int epoll_fd);
void peer_broadcast_lease_commit(int epoll_fd, const void *payload, uint32_t len);
void peer_broadcast_config_reload(int epoll_fd);
void peer_broadcast_config_reload_commit(int epoll_fd, const void *payload,
                                         uint32_t len);
void peer_close_all_for_config_reload(int epoll_fd);
void peer_pump_epoll_event(int epoll_fd, const struct epoll_event *ev);
void peer_poll(int epoll_fd);

#endif
