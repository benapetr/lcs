// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_CONFIG_H
#define LCS_CONFIG_H

#include "common.h"
#include "source_allowlist.h"

#include <netdb.h>

// Node
typedef struct
{
    char name[LCS_NAME_MAX + 1];
    lcs_node_role_t role;
    char address[LCS_ADDR_MAX + 1];
    uint16_t port;
    bool address_set;
    struct sockaddr_storage numeric_address;
} lcs_node_config_t;

// Resource group
typedef struct
{
    char name[LCS_NAME_MAX + 1];
    lcs_group_type_t type;
    lcs_group_mode_t mode;
} lcs_group_config_t;

// Resource
typedef struct
{
    lcs_resource_type_t type;
    char name[LCS_NAME_MAX + 1];
    char group_name[LCS_NAME_MAX + 1];
    char home_node_name[LCS_NAME_MAX + 1];
    char systemd_unit[LCS_NAME_MAX + 1];
    char depends_on_names[LCS_MAX_RESOURCE_DEPS][LCS_NAME_MAX + 1];
    char address[LCS_ADDR_MAX + 1];
    char interface[LCS_NAME_MAX + 1];
    char interface_original[LCS_ADDR_MAX + 1];
    char pre_start[LCS_PATH_MAX + 1];
    char post_start[LCS_PATH_MAX + 1];
    char pre_stop[LCS_PATH_MAX + 1];
    char post_stop[LCS_PATH_MAX + 1];
    int group_idx;
    int home_node_idx;
    int depends_on_idx[LCS_MAX_RESOURCE_DEPS];
    size_t depends_on_count;
    uint32_t priority;
    bool priority_set;
    bool interface_normalized;
    uint64_t birth_revision; /* Dynamic backend incarnation; zero for INI/bootstrap. */
} lcs_resource_config_t;

typedef struct
{
    char cluster_name[LCS_NAME_MAX + 1];
    char self_name[LCS_NAME_MAX + 1];
    char bind_address[LCS_ADDR_MAX + 1];
    char metrics_bind_address[LCS_ADDR_MAX + 1];
    char socket_path[LCS_PATH_MAX + 1];
    char pidfile_path[LCS_PATH_MAX + 1];
    char secret[LCS_NAME_MAX + 1];
    char allow_from[LCS_ALLOW_FROM_MAX + 1];
    lcs_source_allowlist_t source_allowlist;
    uint16_t port;
    uint16_t metrics_port;
    uint32_t lease_ms;
    uint32_t renew_ms;
    uint32_t peer_timeout_ms;
    uint32_t probe_count;
    uint32_t probe_timeout_ms;
    uint32_t hook_timeout_ms;
    bool syslog_enabled;
    bool metrics_enabled;
    bool peers_only;
    lcs_vip_backend_t vip_backend;
    size_t node_count;
    size_t group_count;
    size_t resource_count;
    uint32_t resource_schema_version; /* 0/1: legacy shadow; 2: incarnation snapshot. */
    lcs_node_config_t nodes[LCS_MAX_NODES];
    lcs_group_config_t groups[LCS_MAX_GROUPS];
    lcs_resource_config_t resources[LCS_MAX_RESOURCES];
} lcs_config_t;

void      lcs_config_init_defaults(lcs_config_t *cfg);
int       lcs_config_load(const char *path, lcs_config_t *cfg, char *err, size_t err_len);
int       lcs_config_self_index(const lcs_config_t *cfg);
int       lcs_config_node_index(const lcs_config_t *cfg, const char *name);
int       lcs_config_group_index(const lcs_config_t *cfg, const char *name);
int       lcs_config_resource_index(const lcs_config_t *cfg, const char *name);
int       lcs_config_validate(lcs_config_t *cfg, char *err, size_t err_len);
/* Canonical ordering followed by the common INI/JSON candidate validation. */
int       lcs_config_finalize(lcs_config_t *cfg, char *err, size_t err_len);
uint32_t  lcs_config_quorum(const lcs_config_t *cfg);
uint64_t  lcs_config_voting_fingerprint(const lcs_config_t *cfg);
uint64_t  lcs_config_full_fingerprint(const lcs_config_t *cfg);
bool      lcs_config_source_is_peer(const lcs_config_t *cfg, const struct sockaddr *source);

#endif
