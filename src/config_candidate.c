// SPDX-License-Identifier: GPL-3.0-or-later

#include "config_candidate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool same_string(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

bool lcs_config_backend_equal(const lcs_resource_config_t *a, const lcs_resource_config_t *b)
{
    if (a->type != b->type)
        return false;
    if (a->type == LCS_RESOURCE_VIP)
        return same_string(a->address, b->address) &&
               same_string(a->interface, b->interface);
    if (a->type == LCS_RESOURCE_SERVICE)
        return same_string(a->systemd_unit, b->systemd_unit);
    return false;
}

static bool resource_config_equal(const lcs_resource_config_t *a, const lcs_resource_config_t *b)
{
    if (a->type != b->type || !same_string(a->name, b->name) ||
        !same_string(a->group_name, b->group_name) ||
        !same_string(a->home_node_name, b->home_node_name) ||
        !same_string(a->systemd_unit, b->systemd_unit) ||
        !same_string(a->address, b->address) ||
        !same_string(a->interface, b->interface) ||
        !same_string(a->pre_start, b->pre_start) ||
        !same_string(a->post_start, b->post_start) ||
        !same_string(a->pre_stop, b->pre_stop) ||
        !same_string(a->post_stop, b->post_stop) ||
        a->priority != b->priority ||
        a->depends_on_count != b->depends_on_count)
        return false;
    for (size_t i = 0; i < a->depends_on_count; i++)
    {
        if (!same_string(a->depends_on_names[i], b->depends_on_names[i]))
            return false;
    }
    return true;
}

bool lcs_config_resources_equal(const lcs_config_t *a, const lcs_config_t *b)
{
    if (a->group_count != b->group_count || a->resource_count != b->resource_count)
        return false;
    for (size_t i = 0; i < a->group_count; i++)
    {
        if (!same_string(a->groups[i].name, b->groups[i].name) || a->groups[i].type != b->groups[i].type || a->groups[i].mode != b->groups[i].mode)
            return false;
    }
    for (size_t i = 0; i < a->resource_count; i++)
    {
        if (!resource_config_equal(&a->resources[i], &b->resources[i]))
            return false;
    }
    return true;
}

int lcs_config_candidate_validate(const lcs_config_t *active, const lcs_config_t *candidate, char *error, size_t error_len)
{
    if (!same_string(candidate->cluster_name, active->cluster_name) || !same_string(candidate->self_name, active->self_name))
    {
        snprintf(error, error_len, "cluster name and local node identity cannot be reloaded");
        return -1;
    }
    if (candidate->node_count != active->node_count)
    {
        snprintf(error, error_len, "node membership changes require the future joint-consensus protocol");
        return -1;
    }
    for (size_t i = 0; i < active->node_count; i++)
    {
        const lcs_node_config_t *old_node = &active->nodes[i];
        const lcs_node_config_t *new_node = &candidate->nodes[i];
        if (!same_string(old_node->name, new_node->name) ||
            old_node->role != new_node->role ||
            !same_string(old_node->address, new_node->address) ||
            old_node->port != new_node->port)
        {
            snprintf(error, error_len, "node membership, roles, addresses, and ports cannot be reloaded");
            return -1;
        }
    }

#define REQUIRE_UNCHANGED(field) \
    do { if (candidate->field != active->field) { \
        snprintf(error, error_len, #field " cannot be changed by resource reload"); \
        return -1; \
    } } while (0)
#define REQUIRE_STRING_UNCHANGED(field) \
    do { if (!same_string(candidate->field, active->field)) { \
        snprintf(error, error_len, #field " cannot be changed by resource reload"); \
        return -1; \
    } } while (0)

    REQUIRE_STRING_UNCHANGED(bind_address);
    REQUIRE_STRING_UNCHANGED(metrics_bind_address);
    REQUIRE_STRING_UNCHANGED(socket_path);
    REQUIRE_STRING_UNCHANGED(pidfile_path);
    REQUIRE_STRING_UNCHANGED(secret);
    REQUIRE_STRING_UNCHANGED(allow_from);
    REQUIRE_UNCHANGED(peers_only);
    REQUIRE_UNCHANGED(port);
    REQUIRE_UNCHANGED(metrics_port);
    REQUIRE_UNCHANGED(lease_ms);
    REQUIRE_UNCHANGED(renew_ms);
    REQUIRE_UNCHANGED(peer_timeout_ms);
    REQUIRE_UNCHANGED(probe_count);
    REQUIRE_UNCHANGED(probe_timeout_ms);
    REQUIRE_UNCHANGED(hook_timeout_ms);
    REQUIRE_UNCHANGED(syslog_enabled);
    REQUIRE_UNCHANGED(metrics_enabled);
    REQUIRE_UNCHANGED(vip_backend);
#undef REQUIRE_STRING_UNCHANGED
#undef REQUIRE_UNCHANGED
    return 0;
}

/* Validate in a private copy so callers can safely alias input and output and
 * malformed candidates never partially modify the published configuration. */
int lcs_config_candidate_prepare(const lcs_config_t *active,
                                  const lcs_config_t *proposed,
                                  lcs_config_t *candidate,
                                  char *error, size_t error_len)
{
    lcs_config_t *next = malloc(sizeof(*next));
    if (!next)
    {
        snprintf(error, error_len, "out of memory preparing resource candidate");
        return -1;
    }
    *next = *proposed;
    int result = lcs_config_finalize(next, error, error_len);
    if (!result)
        result = lcs_config_candidate_validate(active, next, error, error_len);
    if (!result)
        *candidate = *next;
    free(next);
    return result;
}
