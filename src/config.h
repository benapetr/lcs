// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_CONFIG_H
#define LCS_CONFIG_H

#include "common.h"

#include <netdb.h>

// Node
typedef struct
{
    char name[LCS_NAME_MAX + 1];
    lcs_node_role_t role;
    char address[LCS_ADDR_MAX + 1];
    uint16_t port;
} lcs_node_config_t;

// VIP
typedef struct
{
    char name[LCS_NAME_MAX + 1];
    char address[LCS_ADDR_MAX + 1];
    char interface[LCS_NAME_MAX + 1];
    char pre_start[LCS_PATH_MAX + 1];
    char post_start[LCS_PATH_MAX + 1];
    char pre_stop[LCS_PATH_MAX + 1];
    char post_stop[LCS_PATH_MAX + 1];
} lcs_vip_config_t;

typedef struct
{
    char cluster_name[LCS_NAME_MAX + 1];
    char self_name[LCS_NAME_MAX + 1];
    char bind_address[LCS_ADDR_MAX + 1];
    char metrics_bind_address[LCS_ADDR_MAX + 1];
    char socket_path[LCS_PATH_MAX + 1];
    char pidfile_path[LCS_PATH_MAX + 1];
    char secret[LCS_NAME_MAX + 1];
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
    lcs_vip_backend_t vip_backend;
    size_t node_count;
    size_t vip_count;
    lcs_node_config_t nodes[LCS_MAX_NODES];
    lcs_vip_config_t vips[LCS_MAX_VIPS];
} lcs_config_t;

void      lcs_config_init_defaults(lcs_config_t *cfg);
int       lcs_config_load(const char *path, lcs_config_t *cfg, char *err, size_t err_len);
int       lcs_config_self_index(const lcs_config_t *cfg);
int       lcs_config_node_index(const lcs_config_t *cfg, const char *name);
int       lcs_config_vip_index(const lcs_config_t *cfg, const char *name);
int       lcs_config_validate(const lcs_config_t *cfg, char *err, size_t err_len);
uint32_t  lcs_config_quorum(const lcs_config_t *cfg);

#endif
