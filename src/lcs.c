// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "common.h"
#include "protocol.h"
#include "util.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void usage(FILE *out)
{
    fprintf(out, "usage: lcs [--version]\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] [--json] status\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] [--json] nrpe\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] [--json] resource list\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] [--json] resource move RESOURCE NODE\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] [--json] resource start RESOURCE\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] [--json] resource stop RESOURCE\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] [--json] resource clear-conflict RESOURCE\n");
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

    printf("{\"cluster\":{\"quorum\":%s,\"votes_seen\":%u,\"quorum_needed\":%u,\"membership_seconds\":%llu},",
           status->has_quorum ? "true" : "false",
           status->votes_seen, status->quorum_needed,
           (unsigned long long)status->membership_seconds);

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
                                 &status->membership_seconds) != 0 ||
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
    printf("Cluster\n");
    printf("  quorum: %s (%u votes, need %u, membership for %s)\n",
           status.has_quorum ? "yes" : "no", status.votes_seen,
           status.quorum_needed, membership_for);
    printf("Nodes\n");
    char node_names[LCS_MAX_NODES][LCS_NAME_MAX + 1];
    memset(node_names, 0, sizeof(node_names));
    for (uint16_t i = 0; i < status.node_count; i++)
    {
        status_node_t *node = &status.nodes[i];
        snprintf(node_names[node->id], sizeof(node_names[node->id]), "%s", node->name);
        printf("  %s role=%s state=%s%s\n", node->name, role_name(node->role), node_state_name(node->state), node->self ? " (self)" : "");
    }
    printf("Resources\n");
    for (uint16_t i = 0; i < status.resource_count; i++)
    {
        status_resource_t *resource = &status.resources[i];
        const char *owner = "-";
        if (resource->owner_node != UINT16_MAX && resource->owner_node < status.node_count)
            owner = node_names[resource->owner_node];

        if (strcmp(resource->resource_type, "service") == 0)
        {
            printf("  %s type=service unit=%s state=%s owner=%s epoch=%llu",
                   resource->name, resource->systemd_unit, lcs_resource_state_name((lcs_resource_state_t)resource->state),
                   owner, (unsigned long long)resource->epoch);
        } else
        {
            printf("  %s %s dev=%s state=%s owner=%s epoch=%llu",
                   resource->name, resource->address, resource->interface, lcs_resource_state_name((lcs_resource_state_t)resource->state),
                   owner, (unsigned long long)resource->epoch);
        }
        if (*resource->group)
            printf(" group=%s priority=%u", resource->group, resource->priority);
        if (*resource->home_node)
            printf(" home=%s%s", resource->home_node, resource->home_blocked ? " blocked=yes" : "");
        if (resource->disabled)
            printf(" disabled=yes");
        printf("\n");

        if ((resource->state == LCS_RES_CONFLICT || resource->state == LCS_RES_STOP_FAILED) && *resource->reason)
            printf("    %s: %s\n", resource->state == LCS_RES_CONFLICT ? "conflict" : "stop_failed", resource->reason);
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

        printf("%s type=%s state=%s owner=%s",
               resource->name, *resource->resource_type ? resource->resource_type : "vip",
               lcs_resource_state_name((lcs_resource_state_t)resource->state), owner);
        if (*resource->address)
            printf(" address=%s dev=%s", resource->address, resource->interface);
        if (*resource->systemd_unit)
            printf(" unit=%s", resource->systemd_unit);
        if (resource->disabled)
            printf(" disabled=yes");
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
    bool json_output = false;
    bool show_version = false;
    int opt;
    static const struct option long_opts[] = {
        { "socket", required_argument, NULL, 's' },
        { "json", no_argument, NULL, 'j' },
        { "version", no_argument, NULL, 'V' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    while ((opt = getopt_long(argc, argv, "s:jhV", long_opts, NULL)) != -1)
    {
        switch (opt)
        {
            case 's':
                socket_path = optarg;
                break;
            case 'j':
                json_output = true;
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
    if (strcmp(cmd, "resource") == 0)
        return cmd_resource(socket_path, argc, argv, optind, json_output);
    usage(stderr);
    return 2;
}
