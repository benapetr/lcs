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
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] move VIP NODE\n");
    fprintf(out, "       lcs [-s SOCKET|--socket SOCKET] clear-conflict VIP\n");
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

static int cmd_status(const char *socket_path)
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
    uint16_t node_count, vip_count, self_node, quorum_needed, votes_seen;
    uint64_t membership_seconds;
    uint8_t has_quorum;
    if (lcs_decode_status_header(&r, &node_count, &vip_count, &self_node,
                                 &quorum_needed, &votes_seen, &has_quorum,
                                 &membership_seconds) != 0 ||
        node_count > LCS_MAX_NODES || vip_count > LCS_MAX_VIPS)
    {
        fprintf(stderr, "lcs: invalid status response header\n");
        return 1;
    }
    char membership_for[64];
    lcs_format_duration(membership_seconds, membership_for, sizeof(membership_for));
    printf("Cluster\n");
    printf("  quorum: %s (%u votes, need %u, membership for %s)\n",
           has_quorum ? "yes" : "no", votes_seen, quorum_needed, membership_for);
    printf("Nodes\n");
    char node_names[LCS_MAX_NODES][LCS_NAME_MAX + 1];
    memset(node_names, 0, sizeof(node_names));
    for (uint16_t i = 0; i < node_count; i++)
    {
        uint16_t id, role;
        uint8_t online, self;
        char name[LCS_NAME_MAX + 1];
        if (lcs_decode_status_node(&r, &id, &role, &online, &self, name, sizeof(name)) != 0 || id >= node_count)
        {
            fprintf(stderr, "lcs: invalid status node entry\n");
            return 1;
        }
        snprintf(node_names[id], sizeof(node_names[id]), "%s", name);
        printf("  %s role=%s online=%s%s\n", name, role_name(role), online ? "yes" : "no", self ? " (self)" : "");
    }
    printf("VIPs\n");
    for (uint16_t i = 0; i < vip_count; i++)
    {
        uint16_t id, owner_node;
        uint64_t epoch, lease_id;
        uint8_t state;
        char name[LCS_NAME_MAX + 1];
        char address[LCS_ADDR_MAX + 1];
        char interface[LCS_NAME_MAX + 1];
        char group[LCS_NAME_MAX + 1];
        char reason[LCS_REASON_MAX + 1];
        uint32_t priority;
        if (lcs_decode_status_vip(&r, &id, &owner_node, &epoch, &lease_id, &state,
                                  name, sizeof(name), address, sizeof(address),
                                  interface, sizeof(interface), group, sizeof(group),
                                  &priority, reason, sizeof(reason)) != 0 ||
            id >= vip_count)
        {
            fprintf(stderr, "lcs: invalid status VIP entry\n");
            return 1;
        }
        const char *owner = "-";
        if (owner_node != UINT16_MAX && owner_node < node_count)
            owner = node_names[owner_node];

        printf("  %s %s dev=%s state=%s owner=%s epoch=%llu",
               name, address, interface, state_name(state), owner,
               (unsigned long long)epoch);
        if (*group)
            printf(" group=%s priority=%u", group, priority);
        printf("\n");
        if (state == LCS_RES_CONFLICT && *reason) {
            printf("    conflict: %s\n", reason);
        }
    }
    if (r.off != r.len) {
        fprintf(stderr, "lcs: trailing bytes in status response\n");
        return 1;
    }
    return 0;
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
    if (strcmp(cmd, "move") == 0)
    {
        if (optind + 2 != argc)
        {
            usage(stderr);
            return 2;
        }
        return cmd_move(socket_path, argv[optind], argv[optind + 1]);
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
