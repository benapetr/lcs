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
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] status\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] nrpe\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] resource list\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] resource move RESOURCE NODE\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] resource start RESOURCE\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] resource stop RESOURCE\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] resource clear-conflict RESOURCE\n");
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

static const char *state_name(uint8_t state)
{
    switch (state)
    {
        case LCS_RES_STOPPED:
            return "stopped";
        case LCS_RES_ACTIVE:
            return "active";
        case LCS_RES_CONFLICT:
            return "conflict";
        case LCS_RES_STARTING:
            return "starting";
        case LCS_RES_STOPPING:
            return "stopping";
        default:
            return "unknown";
    }
}

typedef struct
{
    uint16_t id;
    uint16_t role;
    uint8_t online;
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
} status_vip_t;

typedef struct
{
    uint16_t node_count;
    uint16_t vip_count;
    uint16_t self_node;
    uint16_t quorum_needed;
    uint16_t votes_seen;
    uint64_t membership_seconds;
    uint8_t has_quorum;
    status_node_t nodes[LCS_MAX_NODES];
    status_vip_t vips[LCS_MAX_VIPS];
} status_snapshot_t;

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
    if (lcs_decode_status_header(&r, &status->node_count, &status->vip_count,
                                 &status->self_node, &status->quorum_needed,
                                 &status->votes_seen, &status->has_quorum,
                                 &status->membership_seconds) != 0 ||
        status->node_count > LCS_MAX_NODES ||
        status->vip_count > LCS_MAX_VIPS)
    {
        fprintf(stderr, "lcs: invalid status response header\n");
        return 1;
    }
    for (uint16_t i = 0; i < status->node_count; i++)
    {
        status_node_t *node = &status->nodes[i];
        if (lcs_decode_status_node(&r, &node->id, &node->role,
                                   &node->online, &node->self,
                                   node->name, sizeof(node->name)) != 0 ||
            node->id >= status->node_count)
        {
            fprintf(stderr, "lcs: invalid status node entry\n");
            return 1;
        }
    }
    for (uint16_t i = 0; i < status->vip_count; i++)
    {
        status_vip_t *vip = &status->vips[i];
        if (lcs_decode_status_vip(&r, &vip->id, &vip->owner_node,
                                  &vip->epoch, &vip->lease_id, &vip->state,
                                  vip->name, sizeof(vip->name),
                                  vip->address, sizeof(vip->address),
                                  vip->interface, sizeof(vip->interface),
                                  vip->group, sizeof(vip->group),
                                  &vip->priority,
                                  vip->home_node, sizeof(vip->home_node),
                                  vip->resource_type, sizeof(vip->resource_type),
                                  vip->systemd_unit, sizeof(vip->systemd_unit),
                                  &vip->home_blocked,
                                  &vip->disabled,
                                  vip->reason,
                                  sizeof(vip->reason)) != 0 ||
            vip->id >= status->vip_count)
        {
            fprintf(stderr, "lcs: invalid status VIP entry\n");
            return 1;
        }
    }
    if (r.off != r.len) {
        fprintf(stderr, "lcs: trailing bytes in status response\n");
        return 1;
    }
    return 0;
}

static int cmd_status(const char *socket_path)
{
    status_snapshot_t status;
    if (fetch_status(socket_path, &status) != 0)
        return 1;

    char membership_for[64];
    lcs_format_duration(status.membership_seconds, membership_for,
                        sizeof(membership_for));
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
        printf("  %s role=%s online=%s%s\n", node->name, role_name(node->role),
               node->online ? "yes" : "no", node->self ? " (self)" : "");
    }
    printf("VIPs\n");
    for (uint16_t i = 0; i < status.vip_count; i++)
    {
        status_vip_t *vip = &status.vips[i];
        const char *owner = "-";
        if (vip->owner_node != UINT16_MAX && vip->owner_node < status.node_count)
            owner = node_names[vip->owner_node];

        if (strcmp(vip->resource_type, "service") == 0)
        {
            printf("  %s type=service unit=%s state=%s owner=%s epoch=%llu",
                   vip->name, vip->systemd_unit, state_name(vip->state),
                   owner, (unsigned long long)vip->epoch);
        } else
        {
            printf("  %s %s dev=%s state=%s owner=%s epoch=%llu",
                   vip->name, vip->address, vip->interface, state_name(vip->state),
                   owner, (unsigned long long)vip->epoch);
        }
        if (*vip->group)
            printf(" group=%s priority=%u", vip->group, vip->priority);
        if (*vip->home_node)
            printf(" home=%s%s", vip->home_node, vip->home_blocked ? " blocked=yes" : "");
        if (vip->disabled)
            printf(" disabled=yes");
        printf("\n");
        if (vip->state == LCS_RES_CONFLICT && *vip->reason) {
            printf("    conflict: %s\n", vip->reason);
        }
    }
    return 0;
}

static int cmd_resource_list(const char *socket_path)
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

    for (uint16_t i = 0; i < status.vip_count; i++)
    {
        status_vip_t *vip = &status.vips[i];
        const char *owner = "-";
        if (vip->owner_node != UINT16_MAX && vip->owner_node < status.node_count)
            owner = node_names[vip->owner_node];

        printf("%s type=%s state=%s owner=%s",
               vip->name, *vip->resource_type ? vip->resource_type : "vip",
               state_name(vip->state), owner);
        if (*vip->address)
            printf(" address=%s dev=%s", vip->address, vip->interface);
        if (*vip->systemd_unit)
            printf(" unit=%s", vip->systemd_unit);
        if (vip->disabled)
            printf(" disabled=yes");
        if (*vip->group)
            printf(" group=%s priority=%u", vip->group, vip->priority);
        if (*vip->home_node)
            printf(" home=%s%s", vip->home_node, vip->home_blocked ? " home_blocked=yes" : "");
        if (vip->state == LCS_RES_CONFLICT && *vip->reason)
            printf(" conflict=\"%s\"", vip->reason);
        printf("\n");
    }
    return 0;
}

static int cmd_nrpe(const char *socket_path)
{
    status_snapshot_t status;
    if (fetch_status(socket_path, &status) != 0)
    {
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
        if (status.nodes[i].online)
            online_nodes++;
    }
    for (uint16_t i = 0; i < status.vip_count; i++)
    {
        status_vip_t *vip = &status.vips[i];
        if (vip->disabled)
        {
            disabled_resources++;
            continue;
        }
        if (vip->state == LCS_RES_ACTIVE)
        {
            active_resources++;
            continue;
        }
        down_resources++;
        int n = snprintf(down_detail + down_len, sizeof(down_detail) - down_len,
                         "%s%s=%s", down_len ? "," : "", vip->name,
                         state_name(vip->state));
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

    printf("%s - quorum=%s votes=%u/%u need=%u membership_for=%s nodes=%u/%u resources=%u/%u active",
           state, status.has_quorum ? "yes" : "no", status.votes_seen,
           status.node_count, status.quorum_needed, membership_for,
           online_nodes, status.node_count,
           active_resources, status.vip_count);
    if (disabled_resources > 0)
        printf(" disabled=%u", disabled_resources);
    if (down_resources > 0)
        printf(" down=%s", down_detail);
    printf("\n");
    return rc;
}

static int cmd_clear_conflict(const char *socket_path, const char *vip)
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
        fprintf(stderr, "lcs: %s\n", message);
        return 1;
    }
    printf("%s\n", message);
    return 0;
}

static int cmd_resource_control(const char *socket_path, const char *resource,
                                uint16_t req_type, uint16_t resp_type,
                                const char *action)
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
        fprintf(stderr, "lcs: invalid resource %s response: got message type %u, expected %u or %u\n",
                action, hdr.type, resp_type, LCS_MSG_ERROR);
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
        fprintf(stderr, "lcs: %s\n", message);
        return 1;
    }
    printf("%s\n", message);
    return 0;
}

static int cmd_move(const char *socket_path, const char *vip, const char *node)
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
        fprintf(stderr, "lcs: %s\n", message);
        return 1;
    }
    printf("%s\n", message);
    return 0;
}

static int cmd_resource(const char *socket_path, int argc, char **argv, int optind)
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
        return cmd_resource_list(socket_path);
    }
    if (strcmp(cmd, "move") == 0)
    {
        if (optind + 2 != argc)
        {
            usage(stderr);
            return 2;
        }
        return cmd_move(socket_path, argv[optind], argv[optind + 1]);
    }
    if (strcmp(cmd, "start") == 0)
    {
        if (optind + 1 != argc)
        {
            usage(stderr);
            return 2;
        }
        return cmd_resource_control(socket_path, argv[optind],
                                    LCS_MSG_RESOURCE_START_REQ,
                                    LCS_MSG_RESOURCE_START_RESP,
                                    "start");
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
                                    "stop");
    }
    if (strcmp(cmd, "clear-conflict") == 0)
    {
        if (optind + 1 != argc)
        {
            usage(stderr);
            return 2;
        }
        return cmd_clear_conflict(socket_path, argv[optind]);
    }

    usage(stderr);
    return 2;
}

int main(int argc, char **argv)
{
    const char *socket_path = LCS_DEFAULT_SOCKET_PATH;
    int opt;
    static const struct option long_opts[] = {
        { "socket", required_argument, NULL, 's' },
        { "version", no_argument, NULL, 'V' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    while ((opt = getopt_long(argc, argv, "s:hV", long_opts, NULL)) != -1)
    {
        switch (opt)
        {
            case 's':
                socket_path = optarg;
                break;
            case 'V':
                printf("lcs %s\n", LCS_VERSION);
                return 0;
            case 'h':
                usage(stdout);
                return 0;
            default:
                usage(stderr);
                return 2;
        }
    }
    if (optind >= argc)
    {
        usage(stderr);
        return 2;
    }
    const char *cmd = argv[optind++];
    if (strcmp(cmd, "status") == 0)
    {
        if (optind != argc)
        {
            usage(stderr);
            return 2;
        }
        return cmd_status(socket_path);
    }
    if (strcmp(cmd, "nrpe") == 0)
    {
        if (optind != argc)
        {
            usage(stderr);
            return 2;
        }
        return cmd_nrpe(socket_path);
    }
    if (strcmp(cmd, "resource") == 0)
        return cmd_resource(socket_path, argc, argv, optind);
    usage(stderr);
    return 2;
}
