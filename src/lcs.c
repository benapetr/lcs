// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "common.h"
#include "protocol.h"
#include "config_status_protocol.h"
#include "config_export_protocol.h"
#include "config_request_status_protocol.h"
#include "util.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static bool colors_enabled;

#define COLOR_RESET "\033[0m"
#define COLOR_BOLD "\033[1m"
#define COLOR_RED "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"

static const char *color(const char *code)
{
    return colors_enabled ? code : "";
}

static const char *node_color(uint8_t state)
{
    return color(state == LCS_NODE_ONLINE ? COLOR_GREEN :
                 state == LCS_NODE_RECOVERING ? COLOR_YELLOW : COLOR_RED);
}

static const char *resource_color(uint8_t state, bool disabled)
{
    if (state == LCS_RES_CONFLICT || state == LCS_RES_STOP_FAILED)
        return color(COLOR_RED);
    if (disabled || state == LCS_RES_STARTING || state == LCS_RES_STOPPING)
        return color(COLOR_YELLOW);
    return color(state == LCS_RES_ACTIVE ? COLOR_GREEN : COLOR_RED);
}

static void usage(FILE *out)
{
    fprintf(out, "usage: lcs [--version]\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] [--json] status\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] [--json] nrpe\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] [--json] reload\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] [--json] config status\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] [--json] config request-status ID\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] [--json] config export [committed|applied]\n");
    fprintf(out, "       lcs [-c CONFIG|--config CONFIG] [--json] config validate FILE|-\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] [--json] resource list\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] [--json] resource move RESOURCE NODE\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] [--json] resource start RESOURCE\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] [--json] resource stop RESOURCE\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] [--json] resource clear-conflict RESOURCE\n");
    fprintf(out, "\nColors are enabled on terminals. Use --no-colors to disable them.\n");
}

static int connect_socket(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        close(fd);
        fprintf(stderr, "lcs: failed to connect to %s: %s\n", path, strerror(errno));
        return -1;
    }
    return fd;
}

static const char *role_name(uint16_t role)
{
    return role == LCS_NODE_FULL ? "full-member" : "quorum-only";
}

static const char *node_state_name(uint8_t state)
{
    switch ((lcs_node_state_t)state)
    {
        case LCS_NODE_ONLINE:
            return "online";
        case LCS_NODE_RECOVERING:
            return "recovering";
        default:
            return "offline";
    }
}

static void json_string(FILE *out, const char *value)
{
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)value; *p; p++)
    {
        switch (*p)
        {
            case '"':
                fputs("\\\"", out);
                break;
            case '\\':
                fputs("\\\\", out);
                break;
            case '\b':
                fputs("\\b", out);
                break;
            case '\f':
                fputs("\\f", out);
                break;
            case '\n':
                fputs("\\n", out);
                break;
            case '\r':
                fputs("\\r", out);
                break;
            case '\t':
                fputs("\\t", out);
                break;
            default:
                if (*p < 0x20)
                    fprintf(out, "\\u%04x", *p);
                else
                    fputc(*p, out);
                break;
        }
    }
    fputc('"', out);
}

static void print_simple_response_json(bool ok, const char *message)
{
    printf("{\"ok\":%s,\"message\":", ok ? "true" : "false");
    json_string(stdout, message);
    printf("}\n");
}

typedef struct
{
    uint16_t id;
    uint16_t role;
    uint8_t state;
    uint8_t self;
    char name[LCS_NAME_MAX + 1];
} status_node_t;

typedef struct
{
    uint16_t id;
    uint16_t owner_node;
    uint64_t epoch;
    uint64_t lease_id;
    uint8_t state;
    char name[LCS_NAME_MAX + 1];
    char address[LCS_ADDR_MAX + 1];
    char interface[LCS_NAME_MAX + 1];
    char group[LCS_NAME_MAX + 1];
    char home_node[LCS_NAME_MAX + 1];
    char resource_type[LCS_NAME_MAX + 1];
    char systemd_unit[LCS_NAME_MAX + 1];
    char reason[LCS_REASON_MAX + 1];
    uint32_t priority;
    uint8_t home_blocked;
    uint8_t disabled;
} status_resource_t;

typedef struct
{
    uint16_t node_count;
    uint16_t resource_count;
    uint16_t self_node;
    uint16_t quorum_needed;
    uint16_t votes_seen;
    uint64_t membership_seconds;
    uint8_t has_quorum;
    uint16_t effective_protocol;
    status_node_t nodes[LCS_MAX_NODES];
    status_resource_t resources[LCS_MAX_RESOURCES];
} status_snapshot_t;

static const char *status_owner_name(const status_snapshot_t *status, const status_resource_t *resource, char node_names[LCS_MAX_NODES][LCS_NAME_MAX + 1])
{
    if (resource->owner_node != UINT16_MAX && resource->owner_node < status->node_count)
        return node_names[resource->owner_node];
    return NULL;
}

static void status_node_names(const status_snapshot_t *status, char node_names[LCS_MAX_NODES][LCS_NAME_MAX + 1])
{
    memset(node_names, 0, sizeof(char) * LCS_MAX_NODES * (LCS_NAME_MAX + 1));
    for (uint16_t i = 0; i < status->node_count; i++)
    {
        const status_node_t *node = &status->nodes[i];
        snprintf(node_names[node->id], LCS_NAME_MAX + 1, "%s", node->name);
    }
}

static void print_status_json(const status_snapshot_t *status)
{
    char node_names[LCS_MAX_NODES][LCS_NAME_MAX + 1];
    status_node_names(status, node_names);

    printf("{\"cluster\":{\"quorum\":%s,\"votes_seen\":%u,\"quorum_needed\":%u,\"membership_seconds\":%llu,\"effective_protocol\":%u,\"effective_protocol_release\":",
           status->has_quorum ? "true" : "false",
           status->votes_seen, status->quorum_needed,
           (unsigned long long)status->membership_seconds,
           status->effective_protocol);
    const char *protocol_release =
        lcs_peer_protocol_release(status->effective_protocol);
    if (protocol_release)
        json_string(stdout, protocol_release);
    else
        printf("null");
    printf("},");

    printf("\"nodes\":[");
    for (uint16_t i = 0; i < status->node_count; i++)
    {
        const status_node_t *node = &status->nodes[i];
        if (i)
            printf(",");
        printf("{\"id\":%u,\"name\":", node->id);
        json_string(stdout, node->name);
        printf(",\"role\":");
        json_string(stdout, role_name(node->role));
        printf(",\"state\":");
        json_string(stdout, node_state_name(node->state));
        printf(",\"self\":%s}", node->self ? "true" : "false");
    }
    printf("],\"resources\":[");
    for (uint16_t i = 0; i < status->resource_count; i++)
    {
        const status_resource_t *resource = &status->resources[i];
        const char *owner = status_owner_name(status, resource, node_names);
        if (i)
            printf(",");
        printf("{\"id\":%u,\"name\":", resource->id);
        json_string(stdout, resource->name);
        printf(",\"type\":");
        json_string(stdout, *resource->resource_type ? resource->resource_type : "vip");
        printf(",\"state\":");
        json_string(stdout, lcs_resource_state_name((lcs_resource_state_t)resource->state));
        printf(",\"owner\":");
        if (owner)
            json_string(stdout, owner);
        else
            printf("null");
        printf(",\"epoch\":%llu,\"lease_id\":%llu", (unsigned long long)resource->epoch, (unsigned long long)resource->lease_id);
        if (*resource->address)
        {
            printf(",\"address\":");
            json_string(stdout, resource->address);
            printf(",\"interface\":");
            json_string(stdout, resource->interface);
        }
        if (*resource->systemd_unit)
        {
            printf(",\"systemd_unit\":");
            json_string(stdout, resource->systemd_unit);
        }
        if (*resource->group)
        {
            printf(",\"group\":");
            json_string(stdout, resource->group);
            printf(",\"priority\":%u", resource->priority);
        }
        if (*resource->home_node)
        {
            printf(",\"home_node\":");
            json_string(stdout, resource->home_node);
            printf(",\"home_blocked\":%s", resource->home_blocked ? "true" : "false");
        }
        printf(",\"disabled\":%s", resource->disabled ? "true" : "false");
        if ((resource->state == LCS_RES_CONFLICT ||
             resource->state == LCS_RES_STOP_FAILED) && *resource->reason)
        {
            printf(",\"reason\":");
            json_string(stdout, resource->reason);
        }
        printf("}");
    }
    printf("]}\n");
}

static int fetch_status(const char *socket_path, status_snapshot_t *status)
{
    int fd = connect_socket(socket_path);
    if (fd < 0)
        return 1;

    uint32_t seq = lcs_next_seq();
    if (lcs_write_frame(fd, LCS_MSG_STATUS_REQ, seq, NULL, 0) != 0)
    {
        fprintf(stderr, "lcs: failed to send status request\n");
        close(fd);
        return 1;
    }
    unsigned char payload[LCS_MAX_FRAME];
    lcs_frame_header_t hdr;
    int read_rc = lcs_read_frame(fd, &hdr, payload, sizeof(payload));
    if (read_rc <= 0)
    {
        fprintf(stderr, "lcs: invalid status response: %s\n", lcs_protocol_error());
        close(fd);
        return 1;
    }
    if (hdr.type != LCS_MSG_STATUS_RESP)
    {
        fprintf(stderr, "lcs: invalid status response: got message type %u, expected %u\n", hdr.type, LCS_MSG_STATUS_RESP);
        close(fd);
        return 1;
    }
    close(fd);
    lcs_buf_reader_t r;
    lcs_buf_reader_init(&r, payload, hdr.length);
    memset(status, 0, sizeof(*status));
    if (lcs_decode_status_header(&r, &status->node_count, &status->resource_count,
                                 &status->self_node, &status->quorum_needed,
                                 &status->votes_seen, &status->has_quorum,
                                 &status->membership_seconds,
                                 &status->effective_protocol) != 0 ||
        status->node_count > LCS_MAX_NODES ||
        status->resource_count > LCS_MAX_RESOURCES)
    {
        fprintf(stderr, "lcs: invalid status response header\n");
        return 1;
    }
    for (uint16_t i = 0; i < status->node_count; i++)
    {
        status_node_t *node = &status->nodes[i];
        if (lcs_decode_status_node(&r, &node->id, &node->role, &node->state, &node->self, node->name, sizeof(node->name)) != 0 || node->id >= status->node_count || node->state > LCS_NODE_ONLINE)
        {
            fprintf(stderr, "lcs: invalid status node entry\n");
            return 1;
        }
    }
    for (uint16_t i = 0; i < status->resource_count; i++)
    {
        status_resource_t *resource = &status->resources[i];
        if (lcs_decode_status_resource(&r, &resource->id, &resource->owner_node,
                                  &resource->epoch, &resource->lease_id, &resource->state,
                                  resource->name, sizeof(resource->name),
                                  resource->address, sizeof(resource->address),
                                  resource->interface, sizeof(resource->interface),
                                  resource->group, sizeof(resource->group),
                                  &resource->priority,
                                  resource->home_node, sizeof(resource->home_node),
                                  resource->resource_type, sizeof(resource->resource_type),
                                  resource->systemd_unit, sizeof(resource->systemd_unit),
                                  &resource->home_blocked,
                                  &resource->disabled,
                                  resource->reason,
                                  sizeof(resource->reason)) != 0 ||
            resource->id >= status->resource_count)
        {
            fprintf(stderr, "lcs: invalid status resource entry\n");
            return 1;
        }
    }
    if (r.off != r.len) {
        fprintf(stderr, "lcs: trailing bytes in status response\n");
        return 1;
    }
    return 0;
}

static int cmd_status(const char *socket_path, bool json_output)
{
    status_snapshot_t status;
    if (fetch_status(socket_path, &status) != 0)
        return 1;

    if (json_output)
    {
        print_status_json(&status);
        return 0;
    }

    char membership_for[64];
    lcs_format_duration(status.membership_seconds, membership_for, sizeof(membership_for));
    printf("%sCluster%s\n", color(COLOR_BOLD), color(COLOR_RESET));
    printf("  quorum: %s%s%s (%u votes, need %u, membership for %s)\n",
           color(status.has_quorum ? COLOR_GREEN : COLOR_RED),
           status.has_quorum ? "yes" : "no", color(COLOR_RESET), status.votes_seen,
           status.quorum_needed, membership_for);
    const char *protocol_release =
        lcs_peer_protocol_release(status.effective_protocol);
    if (protocol_release)
        printf("  protocol: %s (wire %u)\n", protocol_release,
               status.effective_protocol);
    else
        printf("  protocol: wire %u\n", status.effective_protocol);
    printf("%sNodes%s\n", color(COLOR_BOLD), color(COLOR_RESET));
    char node_names[LCS_MAX_NODES][LCS_NAME_MAX + 1];
    memset(node_names, 0, sizeof(node_names));
    for (uint16_t i = 0; i < status.node_count; i++)
    {
        status_node_t *node = &status.nodes[i];
        snprintf(node_names[node->id], sizeof(node_names[node->id]), "%s", node->name);
        printf("  %s role=%s state=%s%s%s%s\n", node->name, role_name(node->role),
               node_color(node->state), node_state_name(node->state), color(COLOR_RESET),
               node->self ? " (self)" : "");
    }
    printf("%sResources%s\n", color(COLOR_BOLD), color(COLOR_RESET));
    for (uint16_t i = 0; i < status.resource_count; i++)
    {
        status_resource_t *resource = &status.resources[i];
        const char *owner = "-";
        if (resource->owner_node != UINT16_MAX && resource->owner_node < status.node_count)
            owner = node_names[resource->owner_node];

        if (strcmp(resource->resource_type, "service") == 0)
        {
            printf("  %s type=service unit=%s state=%s%s%s owner=%s epoch=%llu",
                   resource->name, resource->systemd_unit, resource_color(resource->state, resource->disabled),
                   lcs_resource_state_name((lcs_resource_state_t)resource->state), color(COLOR_RESET),
                   owner, (unsigned long long)resource->epoch);
        } else
        {
            printf("  %s %s dev=%s state=%s%s%s owner=%s epoch=%llu",
                   resource->name, resource->address, resource->interface, resource_color(resource->state, resource->disabled),
                   lcs_resource_state_name((lcs_resource_state_t)resource->state), color(COLOR_RESET),
                   owner, (unsigned long long)resource->epoch);
        }
        if (*resource->group)
            printf(" group=%s priority=%u", resource->group, resource->priority);
        if (*resource->home_node)
            printf(" home=%s%s", resource->home_node, resource->home_blocked ? " blocked=yes" : "");
        if (resource->disabled)
            printf(" %sdisabled=yes%s", color(COLOR_YELLOW), color(COLOR_RESET));
        printf("\n");

        if ((resource->state == LCS_RES_CONFLICT || resource->state == LCS_RES_STOP_FAILED) && *resource->reason)
            printf("    %s%s: %s%s\n", color(COLOR_RED),
                   resource->state == LCS_RES_CONFLICT ? "conflict" : "stop_failed",
                   resource->reason, color(COLOR_RESET));
    }
    return 0;
}

static int cmd_resource_list(const char *socket_path, bool json_output)
{
    status_snapshot_t status;
    if (fetch_status(socket_path, &status) != 0)
        return 1;

    char node_names[LCS_MAX_NODES][LCS_NAME_MAX + 1];
    memset(node_names, 0, sizeof(node_names));
    for (uint16_t i = 0; i < status.node_count; i++)
    {
        status_node_t *node = &status.nodes[i];
        snprintf(node_names[node->id], sizeof(node_names[node->id]), "%s", node->name);
    }

    if (json_output)
    {
        printf("{\"resources\":[");
        for (uint16_t i = 0; i < status.resource_count; i++)
        {
            status_resource_t *resource = &status.resources[i];
            const char *owner = status_owner_name(&status, resource, node_names);
            if (i)
                printf(",");
            printf("{\"name\":");
            json_string(stdout, resource->name);
            printf(",\"type\":");
            json_string(stdout, *resource->resource_type ? resource->resource_type : "vip");
            printf(",\"state\":");
            json_string(stdout, lcs_resource_state_name((lcs_resource_state_t)resource->state));
            printf(",\"owner\":");
            if (owner)
                json_string(stdout, owner);
            else
                printf("null");
            if (*resource->address)
            {
                printf(",\"address\":");
                json_string(stdout, resource->address);
                printf(",\"interface\":");
                json_string(stdout, resource->interface);
            }
            if (*resource->systemd_unit)
            {
                printf(",\"systemd_unit\":");
                json_string(stdout, resource->systemd_unit);
            }
            printf(",\"disabled\":%s", resource->disabled ? "true" : "false");
            if (*resource->group)
            {
                printf(",\"group\":");
                json_string(stdout, resource->group);
                printf(",\"priority\":%u", resource->priority);
            }
            if (*resource->home_node)
            {
                printf(",\"home_node\":");
                json_string(stdout, resource->home_node);
                printf(",\"home_blocked\":%s", resource->home_blocked ? "true" : "false");
            }
            if ((resource->state == LCS_RES_CONFLICT ||
                 resource->state == LCS_RES_STOP_FAILED) && *resource->reason)
            {
                printf(",\"reason\":");
                json_string(stdout, resource->reason);
            }
            printf("}");
        }
        printf("]}\n");
        return 0;
    }

    for (uint16_t i = 0; i < status.resource_count; i++)
    {
        status_resource_t *resource = &status.resources[i];
        const char *owner = "-";
        if (resource->owner_node != UINT16_MAX && resource->owner_node < status.node_count)
            owner = node_names[resource->owner_node];

        printf("%s type=%s state=%s%s%s owner=%s",
               resource->name, *resource->resource_type ? resource->resource_type : "vip",
               resource_color(resource->state, resource->disabled),
               lcs_resource_state_name((lcs_resource_state_t)resource->state), color(COLOR_RESET), owner);
        if (*resource->address)
            printf(" address=%s dev=%s", resource->address, resource->interface);
        if (*resource->systemd_unit)
            printf(" unit=%s", resource->systemd_unit);
        if (resource->disabled)
            printf(" %sdisabled=yes%s", color(COLOR_YELLOW), color(COLOR_RESET));
        if (*resource->group)
            printf(" group=%s priority=%u", resource->group, resource->priority);
        if (*resource->home_node)
            printf(" home=%s%s", resource->home_node, resource->home_blocked ? " home_blocked=yes" : "");
        if ((resource->state == LCS_RES_CONFLICT || resource->state == LCS_RES_STOP_FAILED) && *resource->reason)
            printf(" reason=\"%s\"", resource->reason);
        printf("\n");
    }
    return 0;
}

static int cmd_nrpe(const char *socket_path, bool json_output)
{
    status_snapshot_t status;
    if (fetch_status(socket_path, &status) != 0)
    {
        if (json_output)
            printf("{\"state\":\"UNKNOWN\",\"ok\":false,\"message\":\"failed to read local lcsd status\"}\n");
        else
            printf("UNKNOWN - failed to read local lcsd status\n");
        return 3;
    }

    uint16_t online_nodes = 0;
    uint16_t down_resources = 0;
    uint16_t active_resources = 0;
    uint16_t disabled_resources = 0;
    char down_detail[512] = "";
    size_t down_len = 0;
    for (uint16_t i = 0; i < status.node_count; i++)
    {
        if (status.nodes[i].state == LCS_NODE_ONLINE)
            online_nodes++;
    }
    for (uint16_t i = 0; i < status.resource_count; i++)
    {
        status_resource_t *resource = &status.resources[i];
        if (resource->disabled)
        {
            disabled_resources++;
            continue;
        }
        if (resource->state == LCS_RES_ACTIVE)
        {
            active_resources++;
            continue;
        }
        down_resources++;
        int n = snprintf(down_detail + down_len, sizeof(down_detail) - down_len,
                         "%s%s=%s", down_len ? "," : "", resource->name,
                         lcs_resource_state_name((lcs_resource_state_t)resource->state));
        if (n > 0 && (size_t)n < sizeof(down_detail) - down_len)
            down_len += (size_t)n;
    }

    char membership_for[64];
    lcs_format_duration(status.membership_seconds, membership_for,
                        sizeof(membership_for));
    const char *state = "OK";
    int rc = 0;
    if (!status.has_quorum || down_resources > 0)
    {
        state = "CRITICAL";
        rc = 2;
    } else if (online_nodes < status.node_count)
    {
        state = "WARNING";
        rc = 1;
    }

    if (json_output)
    {
        printf("{\"state\":");
        json_string(stdout, state);
        printf(",\"exit_code\":%d,\"quorum\":%s,\"votes_seen\":%u,\"node_count\":%u,\"quorum_needed\":%u,\"membership_seconds\":%llu,\"online_nodes\":%u,\"active_resources\":%u,\"resource_count\":%u,\"disabled_resources\":%u,\"down_resources\":%u",
               rc, status.has_quorum ? "true" : "false", status.votes_seen,
               status.node_count, status.quorum_needed,
               (unsigned long long)status.membership_seconds,
               online_nodes, active_resources, status.resource_count,
               disabled_resources, down_resources);
        if (down_resources > 0)
        {
            printf(",\"down_detail\":");
            json_string(stdout, down_detail);
        }
        printf("}\n");
        return rc;
    }

    printf("%s - quorum=%s votes=%u/%u need=%u membership_for=%s nodes=%u/%u resources=%u/%u active",
           state, status.has_quorum ? "yes" : "no", status.votes_seen,
           status.node_count, status.quorum_needed, membership_for,
           online_nodes, status.node_count,
           active_resources, status.resource_count);
    if (disabled_resources > 0)
        printf(" disabled=%u", disabled_resources);
    if (down_resources > 0)
        printf(" down=%s", down_detail);
    printf("\n");
    return rc;
}

static int cmd_clear_conflict(const char *socket_path, const char *vip, bool json_output)
{
    int fd = connect_socket(socket_path);
    if (fd < 0)
        return 1;

    unsigned char req[LCS_MAX_FRAME];
    size_t req_len = 0;
    if (lcs_encode_clear_conflict_req(req, sizeof(req), &req_len, vip) != 0)
    {
        fprintf(stderr, "lcs: failed to encode clear-conflict request\n");
        close(fd);
        return 1;
    }
    uint32_t seq = lcs_next_seq();
    if (lcs_write_frame(fd, LCS_MSG_CLEAR_CONFLICT_REQ, seq, req, (uint32_t)req_len) != 0)
    {
        fprintf(stderr, "lcs: failed to send clear-conflict request\n");
        close(fd);
        return 1;
    }
    unsigned char payload[LCS_MAX_FRAME];
    lcs_frame_header_t hdr;
    int read_rc = lcs_read_frame(fd, &hdr, payload, sizeof(payload));
    if (read_rc <= 0)
    {
        fprintf(stderr, "lcs: invalid clear-conflict response: %s\n", lcs_protocol_error());
        close(fd);
        return 1;
    }
    if (hdr.type != LCS_MSG_CLEAR_CONFLICT_RESP && hdr.type != LCS_MSG_ERROR)
    {
        fprintf(stderr, "lcs: invalid clear-conflict response: got message type %u, expected %u or %u\n", hdr.type, LCS_MSG_CLEAR_CONFLICT_RESP, LCS_MSG_ERROR);
        close(fd);
        return 1;
    }
    close(fd);
    int32_t status = -1;
    char message[128];
    if (lcs_decode_simple_resp(payload, hdr.length, &status, message, sizeof(message)) != 0)
    {
        fprintf(stderr, "lcs: invalid clear-conflict response payload\n");
        return 1;
    }
    if (status != 0)
    {
        if (json_output)
            print_simple_response_json(false, message);
        else
            fprintf(stderr, "lcs: %s\n", message);
        return 1;
    }
    if (json_output)
        print_simple_response_json(true, message);
    else
        printf("%s\n", message);
    return 0;
}

static int cmd_config_validate(const char *config_path, const char *input_path, bool json_output)
{
    lcs_config_t *configs = calloc(2, sizeof(*configs));
    char *document = NULL, error[256] = ""; uint64_t revision = 0;
    size_t length = 0; FILE *input = NULL; bool valid = false;
    if (!configs) { snprintf(error, sizeof(error), "cannot allocate validation context"); goto result; }
    if (lcs_config_load(config_path, &configs[0], error, sizeof(error))) goto result;
    input = !strcmp(input_path, "-") ? stdin : fopen(input_path, "rb");
    if (!input) { snprintf(error, sizeof(error), "cannot open resource JSON: %s", strerror(errno)); goto result; }
    document = malloc(LCS_RESOURCE_DOCUMENT_MAX + 1u);
    if (!document) { snprintf(error, sizeof(error), "cannot allocate resource JSON input"); goto result; }
    while (length < LCS_RESOURCE_DOCUMENT_MAX + 1u)
    {
        size_t count = fread(document + length, 1, LCS_RESOURCE_DOCUMENT_MAX + 1u - length, input);
        length += count;
        if (ferror(input)) { snprintf(error, sizeof(error), "cannot read resource JSON input"); goto result; }
        if (!count) break;
    }
    valid = !lcs_resource_config_decode(document, length, &configs[0], &configs[1], &revision, error, sizeof(error));
result:
    if (input && input != stdin) fclose(input);
    if (json_output)
    {
        printf("{\"valid\":%s", valid ? "true" : "false");
        if (valid)
            printf(",\"schema_version\":%u,\"revision\":%llu,\"resource_count\":%zu,\"group_count\":%zu",
                configs[1].resource_schema_version, (unsigned long long)revision, configs[1].resource_count, configs[1].group_count);
        else { printf(",\"error\":"); json_string(stdout, error); }
        printf("}\n");
    }
    else if (valid)
        printf("Resource configuration is valid: schema=%u revision=%llu resources=%zu groups=%zu\n",
            configs[1].resource_schema_version, (unsigned long long)revision, configs[1].resource_count, configs[1].group_count);
    else fprintf(stderr, "lcs: %s\n", error);
    free(document); free(configs);
    if (fflush(stdout)) return 1;
    return valid ? 0 : 1;
}

static int cmd_config_export(const char *socket_path, uint8_t selection)
{
    int fd = connect_socket(socket_path);
    if (fd < 0) return 1;
    unsigned char payload[LCS_MAX_FRAME]; size_t length;
    uint32_t seq = lcs_next_seq(), received = 0;
    char *document = NULL;
    lcs_config_export_chunk_t first = {0};
    if (lcs_config_export_request_encode(selection, payload, sizeof(payload), &length) ||
        lcs_write_frame(fd, LCS_MSG_CONFIG_EXPORT_REQ, seq, payload, (uint32_t)length)) goto invalid;
    do
    {
        lcs_frame_header_t header;
        if (lcs_read_frame(fd, &header, payload, sizeof(payload)) <= 0 || header.seq != seq) goto invalid;
        if (header.type == LCS_MSG_ERROR)
        {
            int32_t status; char message[256];
            if (lcs_decode_simple_resp(payload, header.length, &status, message, sizeof(message))) goto invalid;
            fprintf(stderr, "lcs: %s\n", message); free(document); close(fd); return 1;
        }
        lcs_config_export_chunk_t chunk; const void *data; size_t size;
        if (header.type != LCS_MSG_CONFIG_EXPORT_CHUNK ||
            lcs_config_export_chunk_decode(payload, header.length, &chunk, &data, &size) ||
            chunk.selection != selection || chunk.offset != received) goto invalid;
        if (!document)
        {
            first = chunk; document = malloc(first.total);
            if (!document) goto invalid;
        }
        if (chunk.total != first.total || chunk.revision != first.revision || chunk.authority != first.authority) goto invalid;
        memcpy(document + received, data, size); received += (uint32_t)size;
    } while (received < first.total);
    close(fd);
    int failed = fwrite(document, 1, first.total, stdout) != first.total || fflush(stdout) != 0;
    free(document); return failed ? 1 : 0;
invalid:
    fprintf(stderr, "lcs: invalid or incomplete configuration export response\n");
    free(document); close(fd); return 1;
}

static int cmd_config_request_status(const char *socket_path, const char *id, bool json_output)
{
    unsigned char payload[LCS_MAX_FRAME]; size_t length;
    if (lcs_config_request_id_encode(id, payload, sizeof(payload), &length))
    { fprintf(stderr, "lcs: invalid request ID (use 1-63 letters, digits, underscores, dots or hyphens)\n"); return 2; }
    int fd = connect_socket(socket_path); if (fd < 0) return 1;
    uint32_t seq = lcs_next_seq(); lcs_frame_header_t header;
    int failed = lcs_write_frame(fd, LCS_MSG_CONFIG_REQUEST_STATUS_REQ, seq, payload, (uint32_t)length) ||
        lcs_read_frame(fd, &header, payload, sizeof(payload)) <= 0;
    close(fd);
    if (failed) { fprintf(stderr, "lcs: request-status query failed: %s\n", lcs_protocol_error()); return 1; }
    if (header.seq != seq) { fprintf(stderr, "lcs: mismatched request-status response\n"); return 1; }
    if (header.type == LCS_MSG_ERROR)
    {
        int32_t status; char message[256];
        if (!lcs_decode_simple_resp(payload, header.length, &status, message, sizeof(message))) fprintf(stderr, "lcs: %s\n", message);
        else fprintf(stderr, "lcs: invalid request-status error\n");
        return 1;
    }
    lcs_config_request_view_t view;
    if (header.type != LCS_MSG_CONFIG_REQUEST_STATUS_RESP ||
        lcs_config_request_view_decode(payload, header.length, &view) || strcmp(view.id, id))
    { fprintf(stderr, "lcs: invalid request-status response\n"); return 1; }
    const char *state = view.state == LCS_CONFIG_REQUEST_APPLIED ? "applied" : view.state == LCS_CONFIG_REQUEST_COMMITTED ? "committed" :
        view.state == LCS_CONFIG_REQUEST_RESERVED ? "reserved" : "unknown";
    const char *history = view.history == LCS_CONFIG_HISTORY_AVAILABLE ? "available" :
        view.history == LCS_CONFIG_HISTORY_RECOVERY ? "recovery-required" : "unavailable";
    if (json_output)
    {
        printf("{\"scope\":\"local\",\"request_id\":"); json_string(stdout, view.id);
        printf(",\"history\":\"%s\",\"state\":\"%s\",\"base_revision\":", history, state);
        if (view.state) printf("%llu,\"value_revision\":%llu,\"base_digest\":\"%s\",\"value_digest\":\"%s\"",
            (unsigned long long)view.base_revision, (unsigned long long)(view.base_revision + 1), view.base_digest, view.value_digest);
        else printf("null,\"value_revision\":null,\"base_digest\":null,\"value_digest\":null");
        puts("}");
    }
    else
    {
        printf("request: %s\nscope: local\nhistory: %s\nstate: %s%s\n", view.id, history, state,
            view.state == LCS_CONFIG_REQUEST_RESERVED ? " (outcome unknown)" : "");
        if (view.state) printf("expected base: %llu %s\nvalue: %llu %s\n",
            (unsigned long long)view.base_revision, view.base_digest, (unsigned long long)(view.base_revision + 1), view.value_digest);
    }
    return fflush(stdout) ? 1 : 0;
}

static int cmd_config_status(const char *socket_path, bool json_output)
{
    int fd = connect_socket(socket_path);
    if (fd < 0)
        return 1;
    unsigned char payload[LCS_MAX_FRAME];
    lcs_frame_header_t header;
    uint32_t seq = lcs_next_seq();
    if (lcs_write_frame(fd, LCS_MSG_CONFIG_STATUS_REQ, seq, NULL, 0) ||
        lcs_read_frame(fd, &header, payload, sizeof(payload)) <= 0)
    {
        fprintf(stderr, "lcs: configuration status request failed: %s\n", lcs_protocol_error());
        close(fd);
        return 1;
    }
    close(fd);
    if (header.type == LCS_MSG_ERROR)
    {
        int32_t status;
        char message[256];
        if (!lcs_decode_simple_resp(payload, header.length, &status, message, sizeof(message)))
            fprintf(stderr, "lcs: %s\n", message);
        else
            fprintf(stderr, "lcs: invalid configuration status error\n");
        return 1;
    }
    lcs_config_status_view_t view;
    if (header.type != LCS_MSG_CONFIG_STATUS_RESP || header.seq != seq ||
        lcs_config_status_view_decode(payload, header.length, &view))
    {
        fprintf(stderr, "lcs: invalid configuration status response\n");
        return 1;
    }
    unsigned prepared = 0, matching = 0;
    for (size_t i = 0; i < view.count; i++)
    {
        prepared += view.nodes[i].known && view.nodes[i].status.prepared;
        matching += lcs_config_status_node_matches(&view, i);
    }
    const lcs_config_status_t *local = &view.nodes[view.self].status;
    if (json_output)
    {
        printf("{\"authority\":\"%s\",\"dynamic_commands\":false,\"activation_ready\":false,\"prepared\":%s,",
               view.authority == LCS_CONFIG_AUTHORITY_JSON ? "json" : "ini", local->prepared ? "true" : "false");
        if (view.authority == LCS_CONFIG_AUTHORITY_JSON)
        {
            printf("\"application_state\":"); json_string(stdout, lcs_config_status_phase_name(view.phase));
            printf(",\"admitted\":%s,\"committed_revision\":%llu,\"committed_digest\":",
                view.admitted ? "true" : "false", (unsigned long long)view.committed_revision);
            json_string(stdout, view.committed_digest);
            printf(",\"applied_revision\":%llu,\"applied_digest\":", (unsigned long long)view.applied_revision);
            json_string(stdout, view.applied_digest);
            printf(",\"pending_revision\":");
            if (view.pending_revision) printf("%llu", (unsigned long long)view.pending_revision); else printf("null");
            printf(",\"pending_digest\":");
            if (view.pending_revision) json_string(stdout, view.pending_digest); else printf("null");
            printf(",\"error\":");
            if (*view.error) json_string(stdout, view.error); else printf("null");
            putchar(',');
        }
        printf("\"membership\":"); json_string(stdout, local->membership);
        printf(",\"baseline_digest\":"); json_string(stdout, local->baseline);
        printf(",\"prepared_members\":%u,\"matching_members\":%u,\"configured_members\":%u,\"nodes\":[",
               prepared, matching, view.count);
        for (size_t i = 0; i < view.count; i++)
        {
            const lcs_config_status_node_t *node = &view.nodes[i];
            if (i) putchar(',');
            printf("{\"name\":"); json_string(stdout, node->name);
            printf(",\"role\":"); json_string(stdout, role_name(node->role));
            printf(",\"self\":%s,\"connected\":%s,\"protocol\":%u,\"metadata_known\":%s,\"prepared\":",
                   i == view.self ? "true" : "false", node->connected ? "true" : "false",
                   node->protocol, node->known ? "true" : "false");
            printf("%s,\"matches\":%s,\"baseline_digest\":",
                   node->known ? (node->status.prepared ? "true" : "false") : "null",
                   lcs_config_status_node_matches(&view, i) ? "true" : "false");
            if (node->known) json_string(stdout, node->status.baseline); else printf("null");
            putchar('}');
        }
        printf("]}\n");
    }
    else
    {
        printf("Resource configuration: %s\n", view.authority == LCS_CONFIG_AUTHORITY_JSON ? "JSON" : "INI");
        if (view.authority == LCS_CONFIG_AUTHORITY_JSON)
        {
            printf("Application: %s; admitted: %s\nCommitted: %llu %s\nApplied: %llu %s\n",
                lcs_config_status_phase_name(view.phase), view.admitted ? "yes" : "no",
                (unsigned long long)view.committed_revision, view.committed_digest,
                (unsigned long long)view.applied_revision, view.applied_digest);
            if (view.pending_revision) printf("Pending: %llu %s\n", (unsigned long long)view.pending_revision, view.pending_digest);
            else printf("Pending: none\n");
            if (*view.error) printf("Recovery error: %s\n", view.error);
        }
        printf("Prepared snapshots: %u/%u; matching local snapshot: %u/%u\n", prepared, view.count, matching, view.count);
        printf("Dynamic commands: unavailable\n");
        for (size_t i = 0; i < view.count; i++)
        {
            const lcs_config_status_node_t *node = &view.nodes[i];
            printf("%s role=%s connected=%s protocol=%u prepared=%s matching=%s\n",
                   node->name, role_name(node->role), node->connected ? "yes" : "no", node->protocol,
                   node->known ? (node->status.prepared ? "yes" : "no") : "unknown",
                   lcs_config_status_node_matches(&view, i) ? "yes" : "no");
        }
    }
    return 0;
}

static int cmd_reload(const char *socket_path, bool json_output)
{
    int fd = connect_socket(socket_path);
    if (fd < 0)
        return 1;

    uint32_t seq = lcs_next_seq();
    if (lcs_write_frame(fd, LCS_MSG_RELOAD_REQ, seq, NULL, 0) != 0)
    {
        fprintf(stderr, "lcs: failed to send configuration reload request\n");
        close(fd);
        return 1;
    }
    unsigned char payload[LCS_MAX_FRAME];
    lcs_frame_header_t hdr;
    int read_rc = lcs_read_frame(fd, &hdr, payload, sizeof(payload));
    if (read_rc <= 0)
    {
        fprintf(stderr, "lcs: invalid configuration reload response: %s\n",
                lcs_protocol_error());
        close(fd);
        return 1;
    }
    if (hdr.type != LCS_MSG_RELOAD_RESP && hdr.type != LCS_MSG_ERROR)
    {
        fprintf(stderr, "lcs: invalid configuration reload response: got message type %u, expected %u or %u\n",
                hdr.type, LCS_MSG_RELOAD_RESP, LCS_MSG_ERROR);
        close(fd);
        return 1;
    }
    close(fd);

    int32_t status = -1;
    char message[128];
    if (lcs_decode_simple_resp(payload, hdr.length, &status, message,
                               sizeof(message)) != 0)
    {
        fprintf(stderr, "lcs: invalid configuration reload response payload\n");
        return 1;
    }
    if (json_output)
        print_simple_response_json(status == 0, message);
    else if (status == 0)
        printf("%s\n", message);
    else
        fprintf(stderr, "lcs: %s\n", message);
    return status == 0 ? 0 : 1;
}

static int cmd_resource_control(const char *socket_path, const char *resource, uint16_t req_type, uint16_t resp_type, const char *action, bool json_output)
{
    int fd = connect_socket(socket_path);
    if (fd < 0)
        return 1;

    unsigned char req[LCS_MAX_FRAME];
    size_t req_len = 0;
    if (lcs_encode_resource_req(req, sizeof(req), &req_len, resource) != 0)
    {
        fprintf(stderr, "lcs: failed to encode resource %s request\n", action);
        close(fd);
        return 1;
    }
    uint32_t seq = lcs_next_seq();
    if (lcs_write_frame(fd, req_type, seq, req, (uint32_t)req_len) != 0)
    {
        fprintf(stderr, "lcs: failed to send resource %s request\n", action);
        close(fd);
        return 1;
    }
    unsigned char payload[LCS_MAX_FRAME];
    lcs_frame_header_t hdr;
    int read_rc = lcs_read_frame(fd, &hdr, payload, sizeof(payload));
    if (read_rc <= 0)
    {
        fprintf(stderr, "lcs: invalid resource %s response: %s\n", action, lcs_protocol_error());
        close(fd);
        return 1;
    }
    if (hdr.type != resp_type && hdr.type != LCS_MSG_ERROR)
    {
        fprintf(stderr, "lcs: invalid resource %s response: got message type %u, expected %u or %u\n", action, hdr.type, resp_type, LCS_MSG_ERROR);
        close(fd);
        return 1;
    }
    close(fd);
    int32_t status = -1;
    char message[128];
    if (lcs_decode_simple_resp(payload, hdr.length, &status, message, sizeof(message)) != 0)
    {
        fprintf(stderr, "lcs: invalid resource %s response payload\n", action);
        return 1;
    }
    if (status != 0)
    {
        if (json_output)
            print_simple_response_json(false, message);
        else
            fprintf(stderr, "lcs: %s\n", message);
        return 1;
    }
    if (json_output)
        print_simple_response_json(true, message);
    else
        printf("%s\n", message);
    return 0;
}

static int cmd_move(const char *socket_path, const char *vip, const char *node, bool json_output)
{
    int fd = connect_socket(socket_path);
    if (fd < 0)
        return 1;

    unsigned char req[LCS_MAX_FRAME];
    size_t req_len = 0;
    if (lcs_encode_move_req(req, sizeof(req), &req_len, vip, node) != 0)
    {
        fprintf(stderr, "lcs: failed to encode move request\n");
        close(fd);
        return 1;
    }
    uint32_t seq = lcs_next_seq();
    if (lcs_write_frame(fd, LCS_MSG_MOVE_REQ, seq, req, (uint32_t)req_len) != 0)
    {
        fprintf(stderr, "lcs: failed to send move request\n");
        close(fd);
        return 1;
    }
    unsigned char payload[LCS_MAX_FRAME];
    lcs_frame_header_t hdr;
    int read_rc = lcs_read_frame(fd, &hdr, payload, sizeof(payload));
    if (read_rc <= 0)
    {
        fprintf(stderr, "lcs: invalid move response: %s\n", lcs_protocol_error());
        close(fd);
        return 1;
    }
    if (hdr.type != LCS_MSG_MOVE_RESP && hdr.type != LCS_MSG_ERROR)
    {
        fprintf(stderr, "lcs: invalid move response: got message type %u, expected %u or %u\n", hdr.type, LCS_MSG_MOVE_RESP, LCS_MSG_ERROR);
        close(fd);
        return 1;
    }
    close(fd);
    int32_t status = -1;
    char message[128];
    if (lcs_decode_simple_resp(payload, hdr.length, &status, message, sizeof(message)) != 0)
    {
        fprintf(stderr, "lcs: invalid move response payload\n");
        return 1;
    }
    if (status != 0)
    {
        if (json_output)
            print_simple_response_json(false, message);
        else
            fprintf(stderr, "lcs: %s\n", message);
        return 1;
    }
    if (json_output)
        print_simple_response_json(true, message);
    else
        printf("%s\n", message);
    return 0;
}

static int cmd_resource(const char *socket_path, int argc, char **argv, int optind, bool json_output)
{
    if (optind >= argc)
    {
        usage(stderr);
        return 2;
    }

    const char *cmd = argv[optind++];
    if (strcmp(cmd, "list") == 0)
    {
        if (optind != argc)
        {
            usage(stderr);
            return 2;
        }
        return cmd_resource_list(socket_path, json_output);
    }
    if (strcmp(cmd, "move") == 0)
    {
        if (optind + 2 != argc)
        {
            usage(stderr);
            return 2;
        }
        return cmd_move(socket_path, argv[optind], argv[optind + 1], json_output);
    }
    if (strcmp(cmd, "start") == 0)
    {
        if (optind + 1 != argc)
        {
            usage(stderr);
            return 2;
        }
        return cmd_resource_control(socket_path, argv[optind], LCS_MSG_RESOURCE_START_REQ, LCS_MSG_RESOURCE_START_RESP, "start", json_output);
    }
    if (strcmp(cmd, "stop") == 0)
    {
        if (optind + 1 != argc)
        {
            usage(stderr);
            return 2;
        }
        return cmd_resource_control(socket_path, argv[optind],
                                    LCS_MSG_RESOURCE_STOP_REQ,
                                    LCS_MSG_RESOURCE_STOP_RESP,
                                    "stop",
                                    json_output);
    }
    if (strcmp(cmd, "clear-conflict") == 0)
    {
        if (optind + 1 != argc)
        {
            usage(stderr);
            return 2;
        }
        return cmd_clear_conflict(socket_path, argv[optind], json_output);
    }

    usage(stderr);
    return 2;
}

int main(int argc, char **argv)
{
    const char *socket_path = LCS_DEFAULT_SOCKET_PATH;
    const char *config_path = NULL;
    bool socket_selected = false;
    bool json_output = false;
    bool show_version = false;
    bool no_colors = false;
    int opt;
    static const struct option long_opts[] = {
        { "socket", required_argument, NULL, 's' },
        { "config", required_argument, NULL, 'c' },
        { "json", no_argument, NULL, 'j' },
        { "no-colors", no_argument, NULL, 1000 },
        { "version", no_argument, NULL, 'V' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    while ((opt = getopt_long(argc, argv, "s:c:jhV", long_opts, NULL)) != -1)
    {
        switch (opt)
        {
            case 's':
                socket_path = optarg;
                socket_selected = true;
                break;
            case 'c':
                config_path = optarg;
                break;
            case 'j':
                json_output = true;
                break;
            case 1000:
                no_colors = true;
                break;
            case 'V':
                show_version = true;
                break;
            case 'h':
                usage(stdout);
                return 0;
            default:
                usage(stderr);
                return 2;
        }
    }
    const char *term = getenv("TERM");
    const char *no_color = getenv("NO_COLOR");
    colors_enabled = !no_colors && !json_output && isatty(STDOUT_FILENO) &&
                     !(no_color && *no_color) && !(term && strcmp(term, "dumb") == 0);
    if (show_version)
    {
        if (json_output)
        {
            printf("{\"version\":");
            json_string(stdout, LCS_VERSION);
            printf(",\"build_flags\":{\"WITH_SYSTEMD\":%s}}\n", LCS_SYSTEMD_SUPPORT ? "true" : "false");
        } else
        {
            printf("lcs %s build_flags=WITH_SYSTEMD=%s\n", LCS_VERSION, LCS_WITH_SYSTEMD_VALUE);
        }
        return 0;
    }
    if (optind >= argc)
    {
        usage(stderr);
        return 2;
    }
    const char *cmd = argv[optind++];
    bool validation_command = !strcmp(cmd, "config") && optind < argc && !strcmp(argv[optind], "validate");
    if (config_path && !validation_command)
    { fprintf(stderr, "lcs: --config is only supported for config validate\n"); return 2; }
    if (validation_command)
    {
        if (optind + 2 != argc) { usage(stderr); return 2; }
        if (socket_selected)
        { fprintf(stderr, "lcs: config validate is offline; --socket is not supported\n"); return 2; }
        return cmd_config_validate(config_path ? config_path : LCS_DEFAULT_CONFIG_PATH, argv[optind + 1], json_output);
    }

    /*
     * Temporary compatibility alias for the pre-resource CLI syntax.
     * Remove this entire block once `lcs move` compatibility is retired.
     */
    if (strcmp(cmd, "move") == 0)
    {
        fprintf(stderr, "lcs: warning: 'lcs move' is deprecated; use 'lcs resource move' instead\n");
        if (optind + 2 != argc)
        {
            usage(stderr);
            return 2;
        }
        return cmd_move(socket_path, argv[optind], argv[optind + 1], json_output);
    }
    /* End temporary `lcs move` compatibility alias. */

    if (strcmp(cmd, "status") == 0)
    {
        if (optind != argc)
        {
            usage(stderr);
            return 2;
        }
        return cmd_status(socket_path, json_output);
    }
    if (strcmp(cmd, "nrpe") == 0)
    {
        if (optind != argc)
        {
            usage(stderr);
            return 2;
        }
        return cmd_nrpe(socket_path, json_output);
    }
    if (strcmp(cmd, "reload") == 0)
    {
        if (optind != argc)
        {
            usage(stderr);
            return 2;
        }
        return cmd_reload(socket_path, json_output);
    }
    if (strcmp(cmd, "config") == 0)
    {
        if (optind + 2 == argc && !strcmp(argv[optind], "request-status"))
            return cmd_config_request_status(socket_path, argv[optind + 1], json_output);
        if (optind + 1 == argc && !strcmp(argv[optind], "status"))
            return cmd_config_status(socket_path, json_output);
        if (optind < argc && !strcmp(argv[optind], "export") && optind + 2 >= argc)
        {
            uint8_t selection = LCS_CONFIG_EXPORT_COMMITTED;
            if (optind + 2 == argc)
            {
                if (!strcmp(argv[optind + 1], "applied")) selection = LCS_CONFIG_EXPORT_APPLIED;
                else if (strcmp(argv[optind + 1], "committed")) { usage(stderr); return 2; }
            }
            return cmd_config_export(socket_path, selection);
        }
        usage(stderr);
        return 2;
    }
    if (strcmp(cmd, "resource") == 0)
        return cmd_resource(socket_path, argc, argv, optind, json_output);
    usage(stderr);
    return 2;
}
