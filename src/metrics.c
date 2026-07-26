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
    uint64_t membership_seconds = g_state.membership_since_ms && now >= g_state.membership_since_ms ?
                                  (now - g_state.membership_since_ms) / 1000u : 0;
    metrics_append(body, cap, &len, "# HELP lcs_cluster_membership_seconds Seconds since this node's observed online/offline cluster membership last changed.\n");
    metrics_append(body, cap, &len, "# TYPE lcs_cluster_membership_seconds gauge\n");
    metrics_append(body, cap, &len, "lcs_cluster_membership_seconds{cluster=\"%s\"} %llu\n",
                   cluster, (unsigned long long)membership_seconds);

    metrics_append(body, cap, &len, "# TYPE lcs_node_online gauge\n");
    metrics_append(body, cap, &len, "# TYPE lcs_node_voting_ready gauge\n");
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        const char *role = g_state.cfg.nodes[i].role == LCS_NODE_FULL ?
                           "full-member" : "quorum-only";
        metrics_append(body, cap, &len,
                       "lcs_node_online{cluster=\"%s\",node=\"%s\",role=\"%s\"} %u\n",
                       cluster, g_state.cfg.nodes[i].name, role,
                       cluster_node_is_online(i) ? 1u : 0u);
        metrics_append(body, cap, &len,
                       "lcs_node_voting_ready{cluster=\"%s\",node=\"%s\",role=\"%s\"} %u\n",
                       cluster, g_state.cfg.nodes[i].name, role,
                       cluster_node_state(i) == LCS_NODE_ONLINE ? 1u : 0u);
    }

    metrics_append(body, cap, &len, "# TYPE lcs_resource_state gauge\n");
    metrics_append(body, cap, &len, "# TYPE lcs_resource_owner gauge\n");
    metrics_append(body, cap, &len, "# TYPE lcs_resource_epoch gauge\n");
    metrics_append(body, cap, &len, "# TYPE lcs_resource_lease_remaining_seconds gauge\n");
    metrics_append(body, cap, &len, "# TYPE lcs_resource_conflict gauge\n");
    metrics_append(body, cap, &len, "# TYPE lcs_resource_stop_failed gauge\n");
    metrics_append(body, cap, &len, "# TYPE lcs_resource_priority gauge\n");
    metrics_append(body, cap, &len, "# TYPE lcs_resource_failovers_total counter\n");
    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        const lcs_resource_config_t *resource = &g_state.cfg.resources[i];
        const resource_runtime_t *res = &g_state.resources[i];
        const char *type = lcs_resource_type_name(resource->type);
        const char *group = resource->group_idx >= 0 ?
                            g_state.cfg.groups[resource->group_idx].name : "";
        metrics_append(body, cap, &len,
                       "lcs_resource_state{cluster=\"%s\",resource=\"%s\",type=\"%s\",state=\"%s\"} 1\n",
                       cluster, resource->name, type, lcs_resource_state_name(res->state));
        for (size_t n = 0; n < g_state.cfg.node_count; n++)
        {
            metrics_append(body, cap, &len,
                           "lcs_resource_owner{cluster=\"%s\",resource=\"%s\",type=\"%s\",node=\"%s\"} %u\n",
                           cluster, resource->name, type, g_state.cfg.nodes[n].name,
                           res->owner_node == (int)n ? 1u : 0u);
        }
        double remaining = 0.0;
        if (res->lease_deadline_ms > now)
            remaining = (double)(res->lease_deadline_ms - now) / 1000.0;
            
        metrics_append(body, cap, &len,
                       "lcs_resource_epoch{cluster=\"%s\",resource=\"%s\",type=\"%s\"} %llu\n",
                       cluster, resource->name, type,
                       (unsigned long long)res->epoch);
        metrics_append(body, cap, &len,
                       "lcs_resource_lease_remaining_seconds{cluster=\"%s\",resource=\"%s\",type=\"%s\"} %.3f\n",
                       cluster, resource->name, type, remaining);
        metrics_append(body, cap, &len,
                       "lcs_resource_conflict{cluster=\"%s\",resource=\"%s\",type=\"%s\"} %u\n",
                       cluster, resource->name, type,
                       res->state == LCS_RES_CONFLICT ? 1u : 0u);
        metrics_append(body, cap, &len,
                       "lcs_resource_stop_failed{cluster=\"%s\",resource=\"%s\",type=\"%s\"} %u\n",
                       cluster, resource->name, type,
                       res->state == LCS_RES_STOP_FAILED ? 1u : 0u);
        metrics_append(body, cap, &len,
                       "lcs_resource_priority{cluster=\"%s\",resource=\"%s\",type=\"%s\",group=\"%s\"} %u\n",
                       cluster, resource->name, type, group,
                       resource->priority);
        metrics_append(body, cap, &len,
                       "lcs_resource_failovers_total{cluster=\"%s\",resource=\"%s\",type=\"%s\"} %llu\n",
                       cluster, resource->name, type,
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
