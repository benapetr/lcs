// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "metrics.h"

#include "cluster.h"
#include "daemon_state.h"
#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int metrics_append(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
    if (*len >= cap)
        return -1;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *len)
        return -1;

    *len += (size_t)n;
    return 0;
}

static const char *resource_state_name(lcs_resource_state_t state)
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

static void write_best_effort(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len)
    {
        ssize_t n = write(fd, p, len);
        if (n > 0)
        {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
}

void lcs_metrics_handle_client(int fd)
{
    char req[512];
    ssize_t req_len = read(fd, req, sizeof(req));
    (void)req_len;

    size_t cap = 128 * 1024;
    char *body = malloc(cap);
    if (!body)
        return;
    size_t len = 0;
    uint64_t now = lcs_now_ms();
    const char *cluster = *g_state.cfg.cluster_name ? g_state.cfg.cluster_name : "default";

    metrics_append(body, cap, &len, "# HELP lcs_cluster_quorum Whether this node currently sees cluster quorum.\n");
    metrics_append(body, cap, &len, "# TYPE lcs_cluster_quorum gauge\n");
    metrics_append(body, cap, &len, "lcs_cluster_quorum{cluster=\"%s\"} %u\n", cluster, cluster_has_quorum() ? 1u : 0u);
    metrics_append(body, cap, &len, "# TYPE lcs_cluster_votes_seen gauge\n");
    metrics_append(body, cap, &len, "lcs_cluster_votes_seen{cluster=\"%s\"} %u\n", cluster, g_state.votes_seen);
    metrics_append(body, cap, &len, "# TYPE lcs_cluster_votes_needed gauge\n");
    metrics_append(body, cap, &len, "lcs_cluster_votes_needed{cluster=\"%s\"} %u\n", cluster, g_state.quorum_needed);

    metrics_append(body, cap, &len, "# TYPE lcs_node_online gauge\n");
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        const char *role = g_state.cfg.nodes[i].role == LCS_NODE_FULL ?
                           "full-member" : "quorum-only";
        metrics_append(body, cap, &len,
                       "lcs_node_online{cluster=\"%s\",node=\"%s\",role=\"%s\"} %u\n",
                       cluster, g_state.cfg.nodes[i].name, role,
                       cluster_node_is_online(i) ? 1u : 0u);
    }

    metrics_append(body, cap, &len, "# TYPE lcs_vip_state gauge\n");
    metrics_append(body, cap, &len, "# TYPE lcs_vip_owner gauge\n");
    metrics_append(body, cap, &len, "# TYPE lcs_vip_epoch gauge\n");
    metrics_append(body, cap, &len, "# TYPE lcs_vip_lease_remaining_seconds gauge\n");
    metrics_append(body, cap, &len, "# TYPE lcs_vip_conflict gauge\n");
    metrics_append(body, cap, &len, "# TYPE lcs_vip_priority gauge\n");
    metrics_append(body, cap, &len, "# TYPE lcs_vip_failovers_total counter\n");
    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
    {
        const resource_runtime_t *res = &g_state.resources[i];
        const char *group = g_state.cfg.vips[i].group_idx >= 0 ?
                            g_state.cfg.groups[g_state.cfg.vips[i].group_idx].name : "";
        metrics_append(body, cap, &len,
                       "lcs_vip_state{cluster=\"%s\",vip=\"%s\",state=\"%s\"} 1\n",
                       cluster, g_state.cfg.vips[i].name, resource_state_name(res->state));
        for (size_t n = 0; n < g_state.cfg.node_count; n++)
        {
            metrics_append(body, cap, &len,
                           "lcs_vip_owner{cluster=\"%s\",vip=\"%s\",node=\"%s\"} %u\n",
                           cluster, g_state.cfg.vips[i].name, g_state.cfg.nodes[n].name,
                           res->owner_node == (int)n ? 1u : 0u);
        }
        double remaining = 0.0;
        if (res->lease_deadline_ms > now)
            remaining = (double)(res->lease_deadline_ms - now) / 1000.0;
            
        metrics_append(body, cap, &len,
                       "lcs_vip_epoch{cluster=\"%s\",vip=\"%s\"} %llu\n",
                       cluster, g_state.cfg.vips[i].name,
                       (unsigned long long)res->epoch);
        metrics_append(body, cap, &len,
                       "lcs_vip_lease_remaining_seconds{cluster=\"%s\",vip=\"%s\"} %.3f\n",
                       cluster, g_state.cfg.vips[i].name, remaining);
        metrics_append(body, cap, &len,
                       "lcs_vip_conflict{cluster=\"%s\",vip=\"%s\"} %u\n",
                       cluster, g_state.cfg.vips[i].name,
                       res->state == LCS_RES_CONFLICT ? 1u : 0u);
        metrics_append(body, cap, &len,
                       "lcs_vip_priority{cluster=\"%s\",vip=\"%s\",group=\"%s\"} %u\n",
                       cluster, g_state.cfg.vips[i].name, group,
                       g_state.cfg.vips[i].priority);
        metrics_append(body, cap, &len,
                       "lcs_vip_failovers_total{cluster=\"%s\",vip=\"%s\"} %llu\n",
                       cluster, g_state.cfg.vips[i].name,
                       (unsigned long long)res->failover_count);
    }

    char header[256];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.0 200 OK\r\n"
                              "Content-Type: text/plain; version=0.0.4\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n\r\n", len);
    if (header_len > 0)
        write_best_effort(fd, header, (size_t)header_len);

    if (len)
        write_best_effort(fd, body, len);

    free(body);
}
