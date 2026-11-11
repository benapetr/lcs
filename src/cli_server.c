// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "cli_server.h"

#include "cluster.h"
#include "config_reload.h"
#include "config_peer.h"
#include "config_manager.h"
#include "config_export_protocol.h"
#include "config_request_status_protocol.h"
#include "config_mutation_protocol.h"
#include "config_mutation_diagnostic.h"
#include "log.h"
#include "move.h"
#include "peer.h"
#include "protocol.h"
#include "resources.h"
#include "epoll_util.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

static bool cli_server_definitions_enabled(void);

static uint32_t cli_server_epoll_id(int slot_idx)
{
    return LCS_EPOLL_CLI_SERVER_BASE + (uint32_t)slot_idx;
}

int cli_server_index_from_epoll_id(uint32_t id)
{
    if (id < LCS_EPOLL_CLI_SERVER_BASE ||
        id >= LCS_EPOLL_CLI_SERVER_BASE + LCS_CLI_SERVER_MAX)
        return -1;
    return (int)(id - LCS_EPOLL_CLI_SERVER_BASE);
}

static int cli_server_update_epoll(int epoll_fd, int slot_idx, const cli_server_runtime_t *client)
{
    if (!client->active || client->fd < 0)
        return -1;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLRDHUP | EPOLLERR | EPOLLHUP;
    if (!client->close_after_flush && !client->mutation_waiting) ev.events |= EPOLLIN;
    if (client->out_len > client->out_off)
        ev.events |= EPOLLOUT;
    ev.data.u32 = cli_server_epoll_id(slot_idx);
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->fd, &ev);
}

static void cli_server_free_buffers(cli_server_runtime_t *client)
{
    free(client->inbuf);
    free(client->outbuf);
    free(client->export_document);
    free(client->import_document);
    client->inbuf = NULL;
    client->outbuf = NULL;
    client->export_document = NULL;
    client->import_document = NULL;
}

static int cli_server_alloc_buffers(cli_server_runtime_t *client)
{
    client->inbuf = malloc(LCS_CLI_SERVER_INBUF_SIZE);
    if (!client->inbuf)
        return -1;
    client->outbuf = malloc(LCS_CLI_SERVER_OUTBUF_SIZE);
    if (!client->outbuf)
    {
        cli_server_free_buffers(client);
        return -1;
    }
    return 0;
}

static void cli_server_close_slot(int epoll_fd, int slot_idx, const char *reason)
{
    cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
    if (!client->active)
        return;
    uint64_t cli_server_id = client->id;
    lcs_log_debug3("closing CLI connection slot=%d fd=%d reason=%s in=%zu out=%zu",
                   slot_idx, client->fd, reason ? reason : "-",
                   client->in_len,
                   client->out_len > client->out_off ?
                   client->out_len - client->out_off : 0);
    if (client->fd >= 0)
    {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
        close(client->fd);
    }
    cli_server_free_buffers(client);
    memset(client, 0, sizeof(*client));
    client->fd = -1;
    move_cancel_cli_server(slot_idx, cli_server_id);
}

static int cli_server_queue_frame(int epoll_fd, int slot_idx, uint16_t type, uint32_t seq, const void *payload, uint32_t length)
{
    cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
    if (!client->active || !client->outbuf || length > LCS_MAX_FRAME)
        return -1;
    size_t frame_len = LCS_FRAME_HEADER_SIZE + (size_t)length;
    size_t queued = client->out_len - client->out_off;
    if (frame_len > LCS_CLI_SERVER_OUTBUF_SIZE - queued)
        return -1;
    if (client->out_off && queued)
        memmove(client->outbuf, client->outbuf + client->out_off, queued);
    client->out_off = 0;
    client->out_len = queued;

    lcs_frame_header_t wire;
    wire.magic = htonl(LCS_PROTO_MAGIC);
    wire.type = htons(type);
    wire.flags = 0;
    wire.length = htonl(length);
    wire.seq = htonl(seq);
    memcpy(client->outbuf + client->out_len, &wire, sizeof(wire));
    client->out_len += sizeof(wire);
    if (length)
    {
        memcpy(client->outbuf + client->out_len, payload, length);
        client->out_len += length;
    }
    return cli_server_update_epoll(epoll_fd, slot_idx, client);
}

static void cli_server_queue_error(int epoll_fd, int slot_idx, uint32_t seq, const char *msg)
{
    unsigned char payload[256];
    size_t len = 0;
    if (lcs_encode_simple_resp(payload, sizeof(payload), &len, -1, msg) == 0)
        cli_server_queue_frame(epoll_fd, slot_idx, LCS_MSG_ERROR, seq, payload, (uint32_t)len);
}

static void cli_server_queue_status(int epoll_fd, int slot_idx, uint32_t seq)
{
    unsigned char payload[LCS_MAX_FRAME];
    lcs_buf_writer_t w;
    lcs_buf_writer_init(&w, payload, sizeof(payload));
    uint64_t now = lcs_now_ms();
    uint64_t membership_seconds = g_state.membership_since_ms && now >= g_state.membership_since_ms ?
                                  (now - g_state.membership_since_ms) / 1000u : 0;
    if (lcs_encode_status_header(&w, (uint16_t)g_state.cfg.node_count,
                                 (uint16_t)g_state.cfg.resource_count,
                                 (uint16_t)g_state.self_index,
                                 (uint16_t)g_state.quorum_needed,
                                 (uint16_t)g_state.votes_seen,
                                 cluster_has_quorum() ? 1 : 0,
                                 membership_seconds,
                                 g_state.effective_protocol_version) != 0)
    {
        cli_server_queue_error(epoll_fd, slot_idx, seq, "failed to encode status header");
        return;
    }
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if (lcs_encode_status_node(&w, (uint16_t)i,
                                   (uint16_t)g_state.cfg.nodes[i].role,
                                   (uint8_t)cluster_node_state(i),
                                   i == (size_t)g_state.self_index ? 1 : 0,
                                   g_state.cfg.nodes[i].name) != 0)
        {
            cli_server_queue_error(epoll_fd, slot_idx, seq,  "failed to encode status node");
            return;
        }
    }
    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        uint16_t owner = g_state.resources[i].owner_node < 0 ?
                         UINT16_MAX : (uint16_t)g_state.resources[i].owner_node;
        const char *group = g_state.cfg.resources[i].group_idx >= 0 ?
                            g_state.cfg.groups[g_state.cfg.resources[i].group_idx].name : "";
        const char *home_node = g_state.cfg.resources[i].home_node_idx >= 0 ?
                                g_state.cfg.nodes[g_state.cfg.resources[i].home_node_idx].name : "";
        const char *type = lcs_resource_type_name(g_state.cfg.resources[i].type);
        if (lcs_encode_status_resource(&w, (uint16_t)i, owner,
                                  g_state.resources[i].epoch,
                                  g_state.resources[i].lease_id,
                                  (uint8_t)g_state.resources[i].state,
                                  g_state.cfg.resources[i].name,
                                  g_state.cfg.resources[i].address,
                                  g_state.cfg.resources[i].interface,
                                  group,
                                  g_state.cfg.resources[i].priority,
                                  home_node,
                                  type,
                                  g_state.cfg.resources[i].systemd_unit,
                                  g_state.resources[i].home_blocked ? 1 : 0,
                                  g_state.resources[i].disabled ? 1 : 0,
                                  g_state.resources[i].conflict_reason) != 0)
        {
            cli_server_queue_error(epoll_fd, slot_idx, seq, "failed to encode status resource");
            return;
        }
    }
    cli_server_queue_frame(epoll_fd, slot_idx, LCS_MSG_STATUS_RESP, seq, payload, (uint32_t)w.len);
}

static void cli_server_queue_simple_response(int epoll_fd, int slot_idx, uint16_t type, uint32_t seq, int32_t status, const char *message)
{
    unsigned char payload[256];
    size_t len = 0;
    if (lcs_encode_simple_resp(payload, sizeof(payload), &len, status, message) == 0)
        cli_server_queue_frame(epoll_fd, slot_idx, type, seq, payload, (uint32_t)len);
}

void cli_server_complete_move(int epoll_fd, int slot_idx, uint64_t cli_server_id, uint32_t seq, int32_t status, const char *message)
{
    if (slot_idx < 0 || slot_idx >= LCS_CLI_SERVER_MAX)
        return;
    cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
    if (!client->active || client->id != cli_server_id)
        return;
    cli_server_queue_simple_response(epoll_fd, slot_idx, LCS_MSG_MOVE_RESP, seq, status, message);
    client->close_after_flush = true;
}

static void cli_server_queue_clear_conflict(int epoll_fd, int slot_idx, uint32_t seq, const void *payload, uint32_t len)
{
    char vip_name[LCS_NAME_MAX + 1];
    int32_t status = -1;
    char message[128] = "";
    if (lcs_decode_clear_conflict_req(payload, len, vip_name, sizeof(vip_name)) != 0)
    {
        snprintf(message, sizeof(message), "invalid clear-conflict request");
    } else if (config_reload_in_progress())
    {
        snprintf(message, sizeof(message), "cluster configuration reload is in progress");
    } else if (!cluster_has_quorum())
    {
        snprintf(message, sizeof(message), "majority quorum is not available");
    } else
    {
        int resource_idx = lcs_config_resource_index(&g_state.cfg, vip_name);
        if (resource_idx < 0)
        {
            snprintf(message, sizeof(message), "unknown VIP");
        } else
        {
            resource_runtime_t *res = &g_state.resources[resource_idx];
            if (res->state != LCS_RES_CONFLICT)
            {
                status = 0;
                snprintf(message, sizeof(message), "VIP is not in conflict state");
            } else
            {
                res->epoch++;
                res->owner_node = -1;
                res->owner_instance_id = 0;
                res->state = LCS_RES_STOPPED;
                res->lease_id = 0;
                res->lease_deadline_ms = 0;
                res->renew_after_ms = 0;
                res->next_activation_attempt_ms = 0;
                res->conflict_reason[0] = '\0';
                peer_broadcast_state_sync(epoll_fd);
                status = 0;
                snprintf(message, sizeof(message), "conflict cleared");
                lcs_log_info("admin cleared conflict for resource %s epoch=%llu", g_state.cfg.resources[resource_idx].name, (unsigned long long)res->epoch);
            }
        }
    }
    cli_server_queue_simple_response(epoll_fd, slot_idx, LCS_MSG_CLEAR_CONFLICT_RESP, seq, status, message);
}

static void cli_server_queue_resource_control(int epoll_fd, int slot_idx, uint16_t resp_type, uint32_t seq, const void *payload, uint32_t len, bool disabled)
{
    char resource_name[LCS_NAME_MAX + 1];
    int32_t status = -1;
    char message[128] = "";
    if (lcs_decode_resource_req(payload, len, resource_name, sizeof(resource_name)) != 0)
    {
        snprintf(message, sizeof(message), "invalid resource request");
    } else if (!disabled && config_reload_in_progress())
    {
        snprintf(message, sizeof(message), "cluster configuration reload is in progress");
    } else if (!cluster_has_quorum())
    {
        snprintf(message, sizeof(message), "majority quorum is not available");
    } else
    {
        int resource_idx = lcs_config_resource_index(&g_state.cfg, resource_name);
        if (resource_idx < 0)
        {
            snprintf(message, sizeof(message), "unknown resource");
        } else if (resources_set_disabled(resource_idx, disabled, epoll_fd, message, sizeof(message)) == 0)
        {
            status = 0;
        }
    }
    cli_server_queue_simple_response(epoll_fd, slot_idx, resp_type, seq, status, message);
}

static void cli_server_queue_reload(int epoll_fd, int slot_idx, uint32_t seq, uint32_t len)
{
    int32_t status = -1;
    const char *message;
    if (len == 0)
        peer_dns_refresh_request();
    if (len != 0)
        message = "invalid configuration reload request";
    else if (config_reload_definitions_frozen())
        message = "resource definitions are frozen for distributed configuration";
    else if (!config_reload_supported_by_online_quorum() &&
             !config_reload_catchup_available())
        message = "the current online quorum does not support resource reload";
    else if (g_state.config_reload.agreed)
        message = "configuration reload candidate is already agreement-locked";
    else
    {
        config_reload_request();
        status = 0;
        message = "configuration reload requested";
    }
    cli_server_queue_simple_response(epoll_fd, slot_idx, LCS_MSG_RELOAD_RESP, seq, status, message);
}

static int cli_server_queue_export_chunk(int epoll_fd, int slot_idx)
{
    cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
    if (!client->export_document || client->export_offset >= client->export_length) return 0;
    size_t size = client->export_length - client->export_offset, length;
    if (size > LCS_MAX_FRAME - LCS_CONFIG_EXPORT_HEADER_SIZE) size = LCS_MAX_FRAME - LCS_CONFIG_EXPORT_HEADER_SIZE;
    lcs_config_export_chunk_t chunk = {.authority = client->export_authority, .selection = client->export_selection,
        .revision = client->export_revision, .total = (uint32_t)client->export_length, .offset = (uint32_t)client->export_offset};
    unsigned char payload[LCS_MAX_FRAME];
    if (lcs_config_export_chunk_encode(&chunk, client->export_document + client->export_offset, size,
        payload, sizeof(payload), &length) || cli_server_queue_frame(epoll_fd, slot_idx,
        LCS_MSG_CONFIG_EXPORT_CHUNK, client->export_seq, payload, (uint32_t)length)) return -1;
    client->export_offset += size; return 1;
}

static int cli_server_queue_export(int epoll_fd, int slot_idx, const lcs_frame_header_t *header, const void *payload)
{
    uint8_t selection; char error[256];
    cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
    if (lcs_config_export_request_decode(payload, header->length, &selection))
    { cli_server_queue_error(epoll_fd, slot_idx, header->seq, "invalid configuration export request"); return 0; }
    client->export_selection = selection; client->export_seq = header->seq;
    if (g_state.resource_generation_enabled)
    {
        const lcs_config_value_t *value = selection == LCS_CONFIG_EXPORT_APPLIED ?
            &g_state.config_runtime.application.applied : &g_state.config_runtime.agreement.committed;
        if (!value->document || !value->revision || !value->length || value->length > LCS_RESOURCE_DOCUMENT_MAX)
        { cli_server_queue_error(epoll_fd, slot_idx, header->seq, "distributed snapshot is not ready for export"); return 0; }
        client->export_document = malloc(value->length);
        if (client->export_document) memcpy(client->export_document, value->document, value->length);
        client->export_length = value->length; client->export_revision = value->revision; client->export_authority = 1;
    }
    else if (lcs_resource_config_encode(&g_state.cfg, 0, &client->export_document, &client->export_length, error, sizeof(error)))
    { cli_server_queue_error(epoll_fd, slot_idx, header->seq, error); return 0; }
    if (!client->export_document)
    { cli_server_queue_error(epoll_fd, slot_idx, header->seq, "cannot allocate configuration export"); return 0; }
    /* Fixed deadline and one queued frame bound slow-reader memory/work. Pin a
     * complete private copy so concurrent learning/remapping cannot mix values. */
    client->deadline_ms = lcs_now_ms() + 10000u;
    return cli_server_queue_export_chunk(epoll_fd, slot_idx) < 0 ? -1 : 0;
}

static void cli_server_queue_config_status(int epoll_fd, int slot_idx, uint32_t seq, uint32_t length)
{
    lcs_config_status_view_t view;
    unsigned char payload[LCS_MAX_FRAME];
    size_t encoded;
    if (length || config_peer_view(&view))
    {
        cli_server_queue_error(epoll_fd, slot_idx, seq, "cannot prepare configuration status");
        return;
    }
    config_manager_diagnostics(&view);
    view.dynamic_commands = view.admitted && cli_server_definitions_enabled();
    if (lcs_config_status_view_encode(&view, payload, sizeof(payload), &encoded))
    {
        cli_server_queue_error(epoll_fd, slot_idx, seq, "cannot prepare configuration status");
        return;
    }
    cli_server_queue_frame(epoll_fd, slot_idx, LCS_MSG_CONFIG_STATUS_RESP, seq, payload, (uint32_t)encoded);
}

static void cli_server_queue_request_status(int epoll_fd, int slot_idx, const lcs_frame_header_t *hdr,
                                              const unsigned char *payload)
{
    char id[LCS_CONFIG_REQUEST_MAX + 1]; lcs_config_request_view_t view;
    unsigned char response[LCS_CONFIG_REQUEST_STATUS_SIZE]; size_t length;
    if (lcs_config_request_id_decode(payload, hdr->length, id) ||
        lcs_config_requests_query(&g_state.config_runtime.requests, id, &view) ||
        lcs_config_request_view_encode(&view, response, sizeof(response), &length))
    { cli_server_queue_error(epoll_fd, slot_idx, hdr->seq, "invalid configuration request-status query"); return; }
    cli_server_queue_frame(epoll_fd, slot_idx, LCS_MSG_CONFIG_REQUEST_STATUS_RESP, hdr->seq, response, (uint32_t)length);
}

/* Definition admission is separate from current quorum and transient writer
 * availability. Known descriptor retries remain readable while edits are fenced. */
static bool cli_server_definitions_enabled(void)
{
    return config_manager_enabled() && !config_manager_recovering() && !config_manager_failed() &&
        !g_state.config_runtime.requires_recovery && g_state.resource_generation_enabled &&
        g_state.self_index >= 0 && g_state.cfg.nodes[g_state.self_index].role == LCS_NODE_FULL &&
        g_state.config_runtime.requests.store && !g_state.config_runtime.requests.poisoned;
}
static int cli_server_mutation_result(int epoll_fd, int slot_idx, uint32_t seq, const char *id,
                                        uint8_t result, const char *reason)
{
    lcs_config_mutation_result_t response = {.result = result}; size_t length;
    if (lcs_config_requests_query(&g_state.config_runtime.requests, id, &response.request)) return -1;
    /* A rejected submission says nothing about an earlier request with this ID. */
    if (result == LCS_CONFIG_MUTATION_REJECTED)
    { memset(&response.request, 0, sizeof(response.request)); strcpy(response.request.id, id); }
    snprintf(response.reason, sizeof(response.reason), "%s", reason);
    unsigned char payload[LCS_CONFIG_MUTATION_RESPONSE_SIZE];
    cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
    client->mutation_waiting = false; client->close_after_flush = true;
    client->deadline_ms = lcs_now_ms() + 5000;
    if (lcs_config_mutation_result_encode(&response, payload, sizeof(payload), &length)) return -1;
    return cli_server_queue_frame(epoll_fd, slot_idx, LCS_MSG_CONFIG_MUTATION_RESP, seq, payload, (uint32_t)length);
}
static void cli_server_explain_request(const lcs_config_request_view_t *view, char reason[256])
{
    lcs_config_status_view_t status = {0}; config_manager_diagnostics(&status);
    lcs_config_mutation_diagnostic_t diagnostic = {.current = &g_state.config_runtime.agreement.committed,
        .application_target = config_manager_error_target(),
        .application_error = status.error, .storage_failed = config_manager_failed() || g_state.config_coordinator.failed,
        .storage_error = *g_state.config_coordinator.error ? g_state.config_coordinator.error : status.error};
    lcs_config_mutation_explain(view, &diagnostic, reason);
}

static int cli_server_mutation(int epoll_fd, int slot_idx, const lcs_frame_header_t *hdr, const unsigned char *payload,
                                  const char *import_document, size_t import_length)
{
    lcs_config_mutation_t *request = malloc(sizeof(*request));
    if (!request) return -1;
    if (lcs_config_mutation_decode(payload, hdr->length, request) ||
        (!import_document && strcmp(request->argv[0], "resource")))
    { free(request); cli_server_queue_error(epoll_fd, slot_idx, hdr->seq, "invalid bounded definition mutation");
      g_state.cli_servers[slot_idx].close_after_flush = true; return 0; }
    lcs_config_runtime_t *runtime = &g_state.config_runtime;
    const lcs_config_request_record_t *known = lcs_config_requests_find(&runtime->requests, request->id);
    uint8_t result = LCS_CONFIG_MUTATION_REJECTED; char error[256] = "definition mutation is unavailable";
    if (!config_manager_enabled() || !runtime->base)
        snprintf(error, sizeof(error), "dynamic definitions require an activated JSON cluster; automatic upgrade activation is not available yet");
    else if (!runtime->requests.store || runtime->requests.poisoned)
    {
        result = LCS_CONFIG_MUTATION_UNKNOWN;
        if (config_manager_failed() || g_state.config_coordinator.failed)
        { lcs_config_request_view_t view = {0}; lcs_config_requests_query(&runtime->requests, request->id, &view); cli_server_explain_request(&view, error); }
        else snprintf(error, sizeof(error), "request history unavailable; recover it before submitting or retrying definitions");
    }
    else
    {
        uint64_t revision = known ? known->base_revision : runtime->agreement.committed.revision;
        const char *parent = known ? known->base_digest : runtime->agreement.committed.digest;
        char descriptor[LCS_CONFIG_DIGEST_SIZE];
        if ((request->revision_set && request->revision != revision) ||
            (request->digest_set && strcmp(request->digest, parent)))
            snprintf(error, sizeof(error), "expected configuration base conflicts; refresh configuration status (known IDs retain their original base)");
        else if (import_document ?
            lcs_config_requests_import_client_digest(runtime->base, request->id, revision, parent, import_document,
                import_length, descriptor, error, sizeof(error)) :
            lcs_config_requests_client_digest(runtime->base, request->id, revision, parent,
                request->argc, request->argv, descriptor, error, sizeof(error))) { /* Invalid descriptor. */ }
        else if (known)
        {
            if (!known->client_digest[0])
            { result = LCS_CONFIG_MUTATION_UNKNOWN; snprintf(error, sizeof(error), "client descriptor unknown on this node; query request-status or retry on the original coordinator"); }
            else if (strcmp(known->client_digest, descriptor))
                snprintf(error, sizeof(error), "request ID conflicts with different arguments; query the original request separately");
            else
            {
                result = known->applied ? LCS_CONFIG_MUTATION_APPLIED : known->committed ? LCS_CONFIG_MUTATION_PENDING : LCS_CONFIG_MUTATION_UNKNOWN;
                lcs_config_request_view_t view = {0};
                lcs_config_requests_query(&runtime->requests, request->id, &view);
                cli_server_explain_request(&view, error);
            }
        }
        else if (g_state.config_transactions.proposer.proposal.requested.document &&
            !strcmp(g_state.config_transactions.proposer.proposal.requested.request_id, request->id))
        { result = LCS_CONFIG_MUTATION_UNKNOWN; snprintf(error, sizeof(error), "original reservation is in progress; query this request ID without resubmitting"); }
        else if (runtime->requests.count == LCS_CONFIG_REQUEST_HISTORY_MAX)
            snprintf(error, sizeof(error), "request history is full; retain it and recover/expand retention before submitting new definitions");
        else if (g_state.self_index < 0 || g_state.cfg.nodes[g_state.self_index].role != LCS_NODE_FULL)
            snprintf(error, sizeof(error), "quorum-only nodes cannot coordinate definition edits; submit on a full member");
        else if (config_manager_failed() || g_state.config_coordinator.failed)
        {
            lcs_config_status_view_t status = {0}; config_manager_diagnostics(&status);
            snprintf(error, sizeof(error), "submission not admitted; configuration recovery required: %.120s. Retain state, repair and restart",
                *g_state.config_coordinator.error ? g_state.config_coordinator.error : status.error);
        }
        else if (!cli_server_definitions_enabled() || config_manager_applying() ||
            g_state.config_application_remapping || config_reload_in_progress())
        {
            lcs_config_status_view_t status = {0}; config_manager_diagnostics(&status);
            if (*status.error)
                snprintf(error, sizeof(error), "submission not admitted; local application blocked: %.120s. Inspect config status; repair before retrying",
                    status.error);
            else snprintf(error, sizeof(error), "definition edits are fenced during recovery, application or reload; inspect config status before retrying");
        }
        else if (!cluster_has_quorum()) snprintf(error, sizeof(error), "no quorum; restore peer connectivity before submitting definitions");
        else if (!config_peer_configuration_vote_ready())
            snprintf(error, sizeof(error), "configuration coordinator is busy or not admitted; refresh status and retry");
        else
        {
            lcs_config_t *configs = calloc(2, sizeof(*configs)); char *document = NULL; size_t length = 0;
            lcs_config_value_t value = {0}; uint64_t decoded;
            if (!configs) snprintf(error, sizeof(error), "cannot allocate definition candidate");
            else if (lcs_resource_config_decode(runtime->agreement.committed.document, runtime->agreement.committed.length,
                runtime->base, &configs[0], &decoded, error, sizeof(error)) || decoded != revision ||
                (import_document ? lcs_resource_config_import(&configs[0], revision, revision, import_document, import_length,
                    &configs[1], error, sizeof(error)) : lcs_resource_config_edit(&configs[0], revision, revision,
                    request->argc, request->argv, &configs[1], error, sizeof(error))) ||
                lcs_resource_config_encode(&configs[1], revision + 1, &document, &length, error, sizeof(error)) ||
                lcs_config_value_init(&value, runtime->base, request->id, document, length, error, sizeof(error))) { /* No submission. */ }
            else if (g_state.config_transactions.proposer.id == UINT64_MAX ||
                config_peer_propose_client(g_state.config_transactions.proposer.id + 1, &value, descriptor))
                snprintf(error, sizeof(error), "proposal not admitted: coordinator busy, history full or wire-6 quorum unavailable; inspect config status and restore compatible peers before retrying");
            else
            {
                cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
                client->mutation_waiting = true; strcpy(client->mutation_id, request->id); client->mutation_seq = hdr->seq;
                client->deadline_ms = lcs_now_ms() + 30000;
                lcs_config_value_clear(&value); free(document); free(configs); free(request);
                return cli_server_update_epoll(epoll_fd, slot_idx, client);
            }
            lcs_config_value_clear(&value); free(document); free(configs);
        }
    }
    int status = cli_server_mutation_result(epoll_fd, slot_idx, hdr->seq, request->id, result, error);
    free(request); return status;
}
static int cli_server_import(int epoll_fd, int slot_idx, const lcs_frame_header_t *hdr, const unsigned char *payload)
{
    cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
    if (hdr->type == LCS_MSG_CONFIG_IMPORT_BEGIN)
    {
        if (client->import_document || client->mutation_waiting) return -1;
        lcs_config_mutation_t *request = malloc(sizeof(*request)); uint32_t total;
        if (!request) return -1;
        int result = lcs_config_import_begin_decode(payload, hdr->length, request, &total);
        free(request); if (result) return -1;
        client->import_document = malloc(total);
        if (!client->import_document) return -1;
        client->import_total = total; client->import_used = 0; client->import_seq = hdr->seq;
        client->import_metadata_length = hdr->length - 6u;
        memcpy(client->import_metadata, payload + 6, client->import_metadata_length);
        client->deadline_ms = lcs_now_ms() + 10000u; /* Fixed upload deadline, never extended by chunks. */
        return 0;
    }
    uint32_t offset; const void *data; size_t size;
    if (!client->import_document || lcs_now_ms() >= client->deadline_ms || hdr->seq != client->import_seq ||
        lcs_config_import_chunk_decode(payload, hdr->length, &offset, &data, &size) ||
        offset != client->import_used || size > client->import_total - client->import_used) return -1;
    memcpy(client->import_document + offset, data, size); client->import_used += (uint32_t)size;
    if (client->import_used < client->import_total) return 0;
    /* Compilation/reservation starts ONLY after the exact complete upload.
     * Socket closure before this point has no consensus or journal side effect. */
    char *document = client->import_document; client->import_document = NULL;
    lcs_frame_header_t request_header = {.type = LCS_MSG_CONFIG_MUTATION_REQ, .seq = client->import_seq,
        .length = (uint32_t)client->import_metadata_length};
    int result = cli_server_mutation(epoll_fd, slot_idx, &request_header, client->import_metadata, document, client->import_total);
    free(document); client->import_total = client->import_used = 0; client->import_metadata_length = 0;
    return result;
}

static int cli_server_mutation_poll(int epoll_fd, int slot_idx, uint64_t now)
{
    cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
    lcs_config_request_view_t view;
    if (lcs_config_requests_query(&g_state.config_runtime.requests, client->mutation_id, &view)) return -1;
    if (view.state == LCS_CONFIG_REQUEST_APPLIED)
        return cli_server_mutation_result(epoll_fd, slot_idx, client->mutation_seq, client->mutation_id,
            LCS_CONFIG_MUTATION_APPLIED, "decision durable and local configuration admission completed");
    /* Transport completion can precede request observation/admission, and a
     * timed-out transport can leave a durable worker still running. Neither
     * closes this client's bounded application wait. */
    if (now < client->deadline_ms && !config_manager_failed() && !g_state.config_coordinator.failed) return 0;
    char reason[256]; cli_server_explain_request(&view, reason);
    return cli_server_mutation_result(epoll_fd, slot_idx, client->mutation_seq, client->mutation_id,
        view.state == LCS_CONFIG_REQUEST_COMMITTED ? LCS_CONFIG_MUTATION_PENDING : LCS_CONFIG_MUTATION_UNKNOWN, reason);
}

static int cli_server_process_frame(int epoll_fd, int slot_idx, const lcs_frame_header_t *hdr, const unsigned char *payload)
{
    cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
    if (client->import_document && hdr->type != LCS_MSG_CONFIG_IMPORT_CHUNK) return -1;
    if (hdr->type == LCS_MSG_CONFIG_IMPORT_BEGIN || hdr->type == LCS_MSG_CONFIG_IMPORT_CHUNK)
        return cli_server_import(epoll_fd, slot_idx, hdr, payload);
    if (hdr->type == LCS_MSG_CONFIG_MUTATION_REQ) return cli_server_mutation(epoll_fd, slot_idx, hdr, payload, NULL, 0);
    if ((g_state.config_runtime.requires_recovery || config_manager_applying() ||
         (g_state.resource_generation_enabled && g_state.config_application_remapping)) &&
        hdr->type != LCS_MSG_STATUS_REQ && hdr->type != LCS_MSG_CONFIG_STATUS_REQ && hdr->type != LCS_MSG_CONFIG_EXPORT_REQ &&
        hdr->type != LCS_MSG_CONFIG_REQUEST_STATUS_REQ)
    {
        cli_server_queue_error(epoll_fd, slot_idx, hdr->seq, config_manager_applying() ?
            "resource configuration application is active; local mutations are fenced" :
            "resource configuration recovery is active; local mutations are fenced");
        client->close_after_flush = true; return 0;
    }
    switch (hdr->type)
    {
        case LCS_MSG_CONFIG_REQUEST_STATUS_REQ:
            cli_server_queue_request_status(epoll_fd, slot_idx, hdr, payload);
            break;
        case LCS_MSG_CONFIG_EXPORT_REQ:
            if (cli_server_queue_export(epoll_fd, slot_idx, hdr, payload)) return -1;
            break;
        case LCS_MSG_CONFIG_STATUS_REQ:
            cli_server_queue_config_status(epoll_fd, slot_idx, hdr->seq, hdr->length);
            break;
        case LCS_MSG_STATUS_REQ:
            cli_server_queue_status(epoll_fd, slot_idx, hdr->seq);
            break;
        case LCS_MSG_MOVE_REQ:
            move_start_cli_server(epoll_fd, slot_idx, hdr->seq, payload, hdr->length);
            return 0;
        case LCS_MSG_CLEAR_CONFLICT_REQ:
            cli_server_queue_clear_conflict(epoll_fd, slot_idx, hdr->seq, payload, hdr->length);
            break;
        case LCS_MSG_RESOURCE_START_REQ:
            cli_server_queue_resource_control(epoll_fd, slot_idx, LCS_MSG_RESOURCE_START_RESP, hdr->seq, payload, hdr->length, false);
            break;
        case LCS_MSG_RESOURCE_STOP_REQ:
            cli_server_queue_resource_control(epoll_fd, slot_idx, LCS_MSG_RESOURCE_STOP_RESP, hdr->seq, payload, hdr->length, true);
            break;
        case LCS_MSG_RELOAD_REQ:
            cli_server_queue_reload(epoll_fd, slot_idx, hdr->seq, hdr->length);
            break;
        default:
            cli_server_queue_error(epoll_fd, slot_idx, hdr->seq, "unsupported local CLI message");
            break;
    }
    client->close_after_flush = true;
    return 0;
}

static int cli_server_parse_frames(int epoll_fd, int slot_idx)
{
    cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
    size_t off = 0;
    while (client->in_len - off >= LCS_FRAME_HEADER_SIZE)
    {
        lcs_frame_header_t wire;
        memcpy(&wire, client->inbuf + off, sizeof(wire));
        lcs_frame_header_t hdr;
        hdr.magic = ntohl(wire.magic);
        hdr.type = ntohs(wire.type);
        hdr.flags = ntohs(wire.flags);
        hdr.length = ntohl(wire.length);
        hdr.seq = ntohl(wire.seq);
        if (hdr.magic != LCS_PROTO_MAGIC || hdr.flags != 0 || hdr.length > LCS_MAX_FRAME)
            return -1;

        size_t frame_len = LCS_FRAME_HEADER_SIZE + (size_t)hdr.length;
        if (client->in_len - off < frame_len)
            break;

        if (cli_server_process_frame(epoll_fd, slot_idx, &hdr, client->inbuf + off + LCS_FRAME_HEADER_SIZE) != 0)
            return -1;
        off += frame_len;
        if (client->close_after_flush || client->mutation_waiting)
            break;
    }
    if (off)
    {
        size_t remaining = client->in_len - off;
        if (remaining)
            memmove(client->inbuf, client->inbuf + off, remaining);
            
        client->in_len = remaining;
    }
    return 0;
}

static int cli_server_flush_output(int epoll_fd, int slot_idx)
{
    cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
    while (client->out_off < client->out_len)
    {
        ssize_t n = write(client->fd, client->outbuf + client->out_off, client->out_len - client->out_off);
        if (n > 0)
        {
            client->out_off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return cli_server_update_epoll(epoll_fd, slot_idx, client);
        return -1;
    }
    client->out_off = 0;
    client->out_len = 0;
    int exported = cli_server_queue_export_chunk(epoll_fd, slot_idx);
    if (exported) return exported < 0 ? -1 : 0;
    if (client->close_after_flush)
        cli_server_close_slot(epoll_fd, slot_idx, "response sent");
    else if (client->active)
        return cli_server_update_epoll(epoll_fd, slot_idx, client);
    return 0;
}

void cli_server_fail_mutations(int epoll_fd)
{
    if (!config_manager_failed() && !g_state.config_coordinator.failed) return;
    /* Fatal storage failure can end the normal scheduler before its next CLI
     * poll. Explain only existing waiters and attempt one bounded nonblocking
     * flush; never accept commands, wait for readers or delay safe shutdown. */
    for (size_t i = 0; i < LCS_CLI_SERVER_MAX; i++)
    {
        cli_server_runtime_t *client = &g_state.cli_servers[i];
        if (!client->active || !client->mutation_waiting) continue;
        if (cli_server_mutation_poll(epoll_fd, (int)i, lcs_now_ms()) ||
            cli_server_flush_output(epoll_fd, (int)i))
            cli_server_close_slot(epoll_fd, (int)i, "failed mutation diagnostic");
    }
}

void cli_server_accept(int epoll_fd, int listen_fd)
{
    for (;;)
    {
        int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                lcs_log_debug("CLI connection accept failed: %s", strerror(errno));
            return;
        }
        int slot_idx = -1;
        for (size_t i = 0; i < LCS_CLI_SERVER_MAX; i++)
        {
            if (!g_state.cli_servers[i].active)
            {
                slot_idx = (int)i;
                break;
            }
        }
        if (slot_idx < 0)
        {
            lcs_log_warn("rejecting CLI connection: connection table full");
            close(fd);
            continue;
        }
        cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
        memset(client, 0, sizeof(*client));
        client->fd = fd;
        client->id = ++g_state.next_cli_server_id;
        if (client->id == 0)
            client->id = ++g_state.next_cli_server_id;
        client->deadline_ms = lcs_now_ms() + g_state.cfg.peer_timeout_ms;
        if (cli_server_alloc_buffers(client) != 0)
        {
            lcs_log_warn("rejecting CLI connection: failed to allocate buffers");
            close(fd);
            memset(client, 0, sizeof(*client));
            client->fd = -1;
            continue;
        }
        client->active = true;
        if (lcs_add_epoll_fd_events(epoll_fd, fd, cli_server_epoll_id(slot_idx), EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP) != 0)
        {
            cli_server_close_slot(epoll_fd, slot_idx, "epoll add failed");
            continue;
        }
        lcs_log_debug3("accepted CLI connection slot=%d fd=%d", slot_idx, fd);
    }
}

void cli_server_pump_epoll_event(int epoll_fd, const struct epoll_event *ev)
{
    int slot_idx = cli_server_index_from_epoll_id(ev->data.u32);
    if (slot_idx < 0)
        return;

    cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
    if (!client->active)
        return;

    if (ev->events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR))
    {
        cli_server_close_slot(epoll_fd, slot_idx, "connection closed");
        return;
    }
    if ((ev->events & EPOLLOUT) && cli_server_flush_output(epoll_fd, slot_idx) != 0)
    {
        cli_server_close_slot(epoll_fd, slot_idx, "write failed");
        return;
    }
    if (!(ev->events & EPOLLIN) || !client->active || client->close_after_flush)
        return;
        
    for (;;)
    {
        if (client->in_len == LCS_CLI_SERVER_INBUF_SIZE)
        {
            cli_server_close_slot(epoll_fd, slot_idx, "input buffer full");
            return;
        }
        ssize_t n = read(client->fd, client->inbuf + client->in_len, LCS_CLI_SERVER_INBUF_SIZE - client->in_len);
        if (n > 0)
        {
            client->in_len += (size_t)n;
            if (cli_server_parse_frames(epoll_fd, slot_idx) != 0)
            {
                cli_server_close_slot(epoll_fd, slot_idx, "invalid frame");
                return;
            }
            if (!client->active || client->out_len > client->out_off)
                return;
            continue;
        }
        if (n == 0)
        {
            cli_server_close_slot(epoll_fd, slot_idx, "connection closed");
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        if (errno == EINTR)
            continue;
        cli_server_close_slot(epoll_fd, slot_idx, "read failed");
        return;
    }
}

void cli_server_expire(int epoll_fd)
{
    uint64_t now = lcs_now_ms();
    for (size_t i = 0; i < LCS_CLI_SERVER_MAX; i++)
    {
        cli_server_runtime_t *client = &g_state.cli_servers[i];
        if (client->active && client->mutation_waiting)
        { if (cli_server_mutation_poll(epoll_fd, (int)i, now)) cli_server_close_slot(epoll_fd, (int)i, "mutation response failed"); }
        else if (client->active && client->deadline_ms && now >= client->deadline_ms)
            cli_server_close_slot(epoll_fd, (int)i, "CLI connection timeout");
    }
}

void cli_server_close_all(int epoll_fd)
{
    for (size_t i = 0; i < LCS_CLI_SERVER_MAX; i++)
    {
        if (g_state.cli_servers[i].active)
            cli_server_close_slot(epoll_fd, (int)i, "shutdown");
    }
}
