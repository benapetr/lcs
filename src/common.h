// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_COMMON_H
#define LCS_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LCS_VERSION "1.1.0"

#ifdef HAVE_SYSTEMD
#define LCS_SYSTEMD_SUPPORT 1
#define LCS_WITH_SYSTEMD_VALUE "1"
#else
#define LCS_SYSTEMD_SUPPORT 0
#define LCS_WITH_SYSTEMD_VALUE "0"
#endif

#define LCS_MAX_NODES 32
#define LCS_MAX_RESOURCES 32
#define LCS_MAX_GROUPS 32
#define LCS_MAX_RESOURCE_DEPS 8
#define LCS_NAME_MAX 63
#define LCS_ADDR_MAX 127
#define LCS_PATH_MAX 255
#define LCS_REASON_MAX 127
#define LCS_SEQ_CACHE_SIZE 64

#define LCS_DEFAULT_PORT 3322
#define LCS_DEFAULT_METRICS_PORT 9120
#define LCS_DEFAULT_METRICS_BIND "127.0.0.1"
#define LCS_DEFAULT_LEASE_MS 5000
#define LCS_DEFAULT_RENEW_MS 1000
#define LCS_DEFAULT_PEER_TIMEOUT_MS 5000
#define LCS_DEFAULT_PROBE_COUNT 3
#define LCS_DEFAULT_PROBE_TIMEOUT_MS 300
#define LCS_DEFAULT_HOOK_TIMEOUT_MS 5000
#define LCS_DEFAULT_LOOP_TIMEOUT_MS 100
#define LCS_PLACEMENT_INTERVAL_MS 1000
#define LCS_STARTUP_AUTOPLACE_DELAY_MS 1000
#define LCS_DEFAULT_HANDSHAKE_TIMEOUT_MS 500
#define LCS_DEFAULT_SOCKET_PATH "/run/lcs/lcsd.sock"
#define LCS_DEFAULT_PIDFILE_PATH ""
#define LCS_DEFAULT_CONFIG_PATH "/etc/lcs/lcs.conf"

typedef enum
{
    LCS_NODE_FULL = 1,
    LCS_NODE_QUORUM_ONLY = 2,
} lcs_node_role_t;

typedef enum
{
    LCS_NODE_OFFLINE = 0,
    LCS_NODE_RECOVERING = 1,
    LCS_NODE_ONLINE = 2,
} lcs_node_state_t;

typedef enum
{
    LCS_RES_STOPPED = 0,
    LCS_RES_ACTIVE = 1,
    LCS_RES_CONFLICT = 2,
    LCS_RES_STARTING = 3,
    LCS_RES_STOPPING = 4,
    LCS_RES_STOP_FAILED = 5,
} lcs_resource_state_t;

typedef enum
{
    LCS_RESOURCE_VIP = 1,
    LCS_RESOURCE_SERVICE = 2,
} lcs_resource_type_t;

static inline const char *lcs_resource_type_name(lcs_resource_type_t type)
{
    switch (type)
    {
        case LCS_RESOURCE_VIP:
            return "vip";
        case LCS_RESOURCE_SERVICE:
            return "service";
        default:
            return "unknown";
    }
}

static inline const char *lcs_resource_state_name(lcs_resource_state_t state)
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
        case LCS_RES_STOP_FAILED:
            return "stop_failed";
        default:
            return "unknown";
    }
}

typedef enum
{
    LCS_VIP_BACKEND_IP = 1,
    LCS_VIP_BACKEND_NETLINK = 2,
} lcs_vip_backend_t;

typedef enum
{
    LCS_GROUP_NONE = 0,
    LCS_GROUP_KEEP_TOGETHER = 1,
    LCS_GROUP_ANTI_AFFINITY = 2,
} lcs_group_type_t;

typedef enum
{
    LCS_GROUP_MODE_NONE = 0,
    LCS_GROUP_MODE_STRICT = 1,
    LCS_GROUP_MODE_BEST_EFFORT = 2,
} lcs_group_mode_t;

#endif
