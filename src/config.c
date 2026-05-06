// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "config.h"

#include "util.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>

typedef enum
{
    SEC_NONE,
    SEC_CLUSTER,
    SEC_NODE,
    SEC_GROUP,
    SEC_VIP,
} section_type_t;

typedef struct
{
    section_type_t type;
    int index;
} section_t;

static void set_err(char *err, size_t err_len, unsigned line, const char *msg)
{
    if (err && err_len)
    {
        if (line)
        {
            snprintf(err, err_len, "line %u: %s", line, msg);
        } else {
            snprintf(err, err_len, "%s", msg);
        }
    }
}

void lcs_config_init_defaults(lcs_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->port = LCS_DEFAULT_PORT;
    cfg->metrics_port = LCS_DEFAULT_METRICS_PORT;
    cfg->lease_ms = LCS_DEFAULT_LEASE_MS;
    cfg->renew_ms = LCS_DEFAULT_RENEW_MS;
    cfg->peer_timeout_ms = LCS_DEFAULT_PEER_TIMEOUT_MS;
    cfg->probe_count = LCS_DEFAULT_PROBE_COUNT;
    cfg->probe_timeout_ms = LCS_DEFAULT_PROBE_TIMEOUT_MS;
    cfg->hook_timeout_ms = LCS_DEFAULT_HOOK_TIMEOUT_MS;
    cfg->syslog_enabled = true;
    cfg->metrics_enabled = true;
    cfg->vip_backend = LCS_VIP_BACKEND_IP;
    snprintf(cfg->metrics_bind_address, sizeof(cfg->metrics_bind_address), "%s", LCS_DEFAULT_METRICS_BIND);
    snprintf(cfg->socket_path, sizeof(cfg->socket_path), "%s", LCS_DEFAULT_SOCKET_PATH);
    snprintf(cfg->pidfile_path, sizeof(cfg->pidfile_path), "%s", LCS_DEFAULT_PIDFILE_PATH);
}

static int set_string(char *dst, size_t dst_len, const char *value)
{
    if (strlen(value) >= dst_len)
        return -1;

    snprintf(dst, dst_len, "%s", value);
    return 0;
}

static int parse_role(const char *value, lcs_node_role_t *role)
{
    if (strcmp(value, "full-member") == 0)
    {
        *role = LCS_NODE_FULL;
        return 0;
    }
    if (strcmp(value, "quorum-only") == 0)
    {
        *role = LCS_NODE_QUORUM_ONLY;
        return 0;
    }
    return -1;
}

static int parse_bool(const char *value, bool *out)
{
    if (strcmp(value, "true") == 0 || strcmp(value, "yes") == 0 || strcmp(value, "on") == 0 || strcmp(value, "1") == 0)
    {
        *out = true;
        return 0;
    }

    if (strcmp(value, "false") == 0 || strcmp(value, "no") == 0 || strcmp(value, "off") == 0 || strcmp(value, "0") == 0)
    {
        *out = false;
        return 0;
    }
    return -1;
}

static int parse_vip_backend(const char *value, lcs_vip_backend_t *backend)
{
    if (strcmp(value, "ip") == 0)
    {
        *backend = LCS_VIP_BACKEND_IP;
        return 0;
    }

    if (strcmp(value, "netlink") == 0)
    {
        *backend = LCS_VIP_BACKEND_NETLINK;
        return 0;
    }
    return -1;
}

static int parse_group_type(const char *value, lcs_group_type_t *type)
{
    if (strcmp(value, "keep-together") == 0)
    {
        *type = LCS_GROUP_KEEP_TOGETHER;
        return 0;
    }
    if (strcmp(value, "anti-affinity") == 0)
    {
        *type = LCS_GROUP_ANTI_AFFINITY;
        return 0;
    }
    return -1;
}

static int parse_group_mode(const char *value, lcs_group_mode_t *mode)
{
    if (strcmp(value, "strict") == 0)
    {
        *mode = LCS_GROUP_MODE_STRICT;
        return 0;
    }
    if (strcmp(value, "best-effort") == 0)
    {
        *mode = LCS_GROUP_MODE_BEST_EFFORT;
        return 0;
    }
    return -1;
}

static int parse_section(lcs_config_t *cfg, char *name, section_t *sec, char *err, size_t err_len, unsigned line)
{
    char *end = strchr(name, ']');
    if (!end || end[1] != '\0')
    {
        set_err(err, err_len, line, "malformed section header");
        return -1;
    }

    *end = '\0';
    char *body = lcs_trim(name + 1);
    if (strcmp(body, "cluster") == 0)
    {
        sec->type = SEC_CLUSTER;
        sec->index = -1;
        return 0;
    }

    if (strncmp(body, "node ", 5) == 0)
    {
        char *node_name = lcs_trim(body + 5);
        if (!lcs_valid_name(node_name))
        {
            set_err(err, err_len, line, "invalid node name");
            return -1;
        }
        if (cfg->node_count >= LCS_MAX_NODES)
        {
            set_err(err, err_len, line, "too many nodes");
            return -1;
        }
        if (lcs_config_node_index(cfg, node_name) >= 0)
        {
            set_err(err, err_len, line, "duplicate node section");
            return -1;
        }
        int idx = (int)cfg->node_count++;
        cfg->nodes[idx].port = cfg->port;
        set_string(cfg->nodes[idx].name, sizeof(cfg->nodes[idx].name), node_name);
        sec->type = SEC_NODE;
        sec->index = idx;
        return 0;
    }

    if (strncmp(body, "group ", 6) == 0)
    {
        char *group_name = lcs_trim(body + 6);
        if (!lcs_valid_name(group_name))
        {
            set_err(err, err_len, line, "invalid group name");
            return -1;
        }
        if (cfg->group_count >= LCS_MAX_GROUPS)
        {
            set_err(err, err_len, line, "too many groups");
            return -1;
        }
        if (lcs_config_group_index(cfg, group_name) >= 0)
        {
            set_err(err, err_len, line, "duplicate group section");
            return -1;
        }
        int idx = (int)cfg->group_count++;
        set_string(cfg->groups[idx].name, sizeof(cfg->groups[idx].name), group_name);
        sec->type = SEC_GROUP;
        sec->index = idx;
        return 0;
    }

    if (strncmp(body, "vip ", 4) == 0)
    {
        char *vip_name = lcs_trim(body + 4);
        if (!lcs_valid_name(vip_name))
        {
            set_err(err, err_len, line, "invalid vip name");
            return -1;
        }
        if (cfg->vip_count >= LCS_MAX_VIPS)
        {
            set_err(err, err_len, line, "too many VIPs");
            return -1;
        }
        if (lcs_config_vip_index(cfg, vip_name) >= 0)
        {
            set_err(err, err_len, line, "duplicate vip section");
            return -1;
        }
        int idx = (int)cfg->vip_count++;
        set_string(cfg->vips[idx].name, sizeof(cfg->vips[idx].name), vip_name);
        cfg->vips[idx].group_idx = -1;
        sec->type = SEC_VIP;
        sec->index = idx;
        return 0;
    }

    set_err(err, err_len, line, "unknown section");
    return -1;
}

static int valid_ip_or_host(const char *value)
{
    struct in_addr a4;
    struct in6_addr a6;
    if (inet_pton(AF_INET, value, &a4) == 1 || inet_pton(AF_INET6, value, &a6) == 1)
        return 1;

    return lcs_valid_name(value);
}

static int valid_vip_cidr(const char *value)
{
    char buf[LCS_ADDR_MAX + 1];
    if (set_string(buf, sizeof(buf), value) != 0)
        return 0;

    char *slash = strrchr(buf, '/');
    if (!slash)
        return 0;

    *slash++ = '\0';
    uint32_t prefix = 0;
    if (lcs_parse_u32(slash, &prefix) != 0)
        return 0;

    struct in_addr a4;
    struct in6_addr a6;
    if (inet_pton(AF_INET, buf, &a4) == 1)
        return prefix <= 32;

    if (inet_pton(AF_INET6, buf, &a6) == 1)
        return prefix <= 128;

    return 0;
}

static int valid_socket_path(const char *value)
{
    struct sockaddr_un addr;
    return value && value[0] == '/' && strlen(value) < sizeof(addr.sun_path);
}

static int valid_pidfile_path(const char *value)
{
    return !*value || (value[0] == '/' && strlen(value) <= LCS_PATH_MAX);
}

static int valid_interface_name(const char *value)
{
    if (!value || !*value || strlen(value) >= IFNAMSIZ)
        return 0;

    for (const unsigned char *p = (const unsigned char *)value; *p; p++)
    {
        if (!(isalnum(*p) || *p == '_' || *p == '-' || *p == '.' || *p == ':'))
            return 0;
    }
    return 1;
}

static int apply_key(lcs_config_t *cfg, section_t sec, char *key, char *value, char *err, size_t err_len, unsigned line)
{
    if (sec.type == SEC_NONE)
    {
        set_err(err, err_len, line, "key outside section");
        return -1;
    }
    if (sec.type == SEC_CLUSTER)
    {
        if (strcmp(key, "name") == 0)
            return set_string(cfg->cluster_name, sizeof(cfg->cluster_name), value);

        if (strcmp(key, "node") == 0)
            return set_string(cfg->self_name, sizeof(cfg->self_name), value);

        if (strcmp(key, "bind") == 0)
            return set_string(cfg->bind_address, sizeof(cfg->bind_address), value);

        if (strcmp(key, "socket") == 0)
            return set_string(cfg->socket_path, sizeof(cfg->socket_path), value);

        if (strcmp(key, "pidfile") == 0)
            return set_string(cfg->pidfile_path, sizeof(cfg->pidfile_path), value);

        if (strcmp(key, "secret") == 0)
            return set_string(cfg->secret, sizeof(cfg->secret), value);

        if (strcmp(key, "port") == 0)
            return lcs_parse_u16(value, &cfg->port);

        if (strcmp(key, "metrics") == 0)
            return parse_bool(value, &cfg->metrics_enabled);

        if (strcmp(key, "metrics_bind") == 0)
            return set_string(cfg->metrics_bind_address, sizeof(cfg->metrics_bind_address), value);

        if (strcmp(key, "metrics_port") == 0)
            return lcs_parse_u16(value, &cfg->metrics_port);

        if (strcmp(key, "lease_ms") == 0)
            return lcs_parse_u32(value, &cfg->lease_ms);

        if (strcmp(key, "renew_ms") == 0)
            return lcs_parse_u32(value, &cfg->renew_ms);

        if (strcmp(key, "peer_timeout_ms") == 0)
            return lcs_parse_u32(value, &cfg->peer_timeout_ms);

        if (strcmp(key, "probe_count") == 0)
            return lcs_parse_u32(value, &cfg->probe_count);

        if (strcmp(key, "probe_timeout_ms") == 0)
            return lcs_parse_u32(value, &cfg->probe_timeout_ms);

        if (strcmp(key, "hook_timeout_ms") == 0)
            return lcs_parse_u32(value, &cfg->hook_timeout_ms);

        if (strcmp(key, "syslog") == 0)
            return parse_bool(value, &cfg->syslog_enabled);

        if (strcmp(key, "vip_backend") == 0)
            return parse_vip_backend(value, &cfg->vip_backend);

    } else if (sec.type == SEC_NODE)
    {
        lcs_node_config_t *node = &cfg->nodes[sec.index];
        if (strcmp(key, "role") == 0)
            return parse_role(value, &node->role);

        if (strcmp(key, "address") == 0)
            return set_string(node->address, sizeof(node->address), value);

        if (strcmp(key, "port") == 0)
            return lcs_parse_u16(value, &node->port);
    } else if (sec.type == SEC_GROUP)
    {
        lcs_group_config_t *group = &cfg->groups[sec.index];
        if (strcmp(key, "type") == 0)
            return parse_group_type(value, &group->type);

        if (strcmp(key, "mode") == 0)
            return parse_group_mode(value, &group->mode);
    } else if (sec.type == SEC_VIP)
    {
        lcs_vip_config_t *vip = &cfg->vips[sec.index];
        if (strcmp(key, "group") == 0)
            return set_string(vip->group_name, sizeof(vip->group_name), value);

        if (strcmp(key, "priority") == 0)
        {
            if (lcs_parse_u32(value, &vip->priority) != 0 || vip->priority == 0)
                return -1;
            vip->priority_set = true;
            return 0;
        }

        if (strcmp(key, "address") == 0)
            return set_string(vip->address, sizeof(vip->address), value);

        if (strcmp(key, "interface") == 0)
            return set_string(vip->interface, sizeof(vip->interface), value);

        if (strcmp(key, "pre_start") == 0)
            return set_string(vip->pre_start, sizeof(vip->pre_start), value);

        if (strcmp(key, "post_start") == 0)
            return set_string(vip->post_start, sizeof(vip->post_start), value);

        if (strcmp(key, "pre_stop") == 0)
            return set_string(vip->pre_stop, sizeof(vip->pre_stop), value);

        if (strcmp(key, "post_stop") == 0)
            return set_string(vip->post_stop, sizeof(vip->post_stop), value);
    }
    set_err(err, err_len, line, "unknown key");
    return -1;
}

static int compare_node_config(const void *a, const void *b)
{
    const lcs_node_config_t *na = a;
    const lcs_node_config_t *nb = b;
    return strcmp(na->name, nb->name);
}

static int compare_group_config(const void *a, const void *b)
{
    const lcs_group_config_t *ga = a;
    const lcs_group_config_t *gb = b;
    return strcmp(ga->name, gb->name);
}

static int compare_vip_config(const void *a, const void *b)
{
    const lcs_vip_config_t *va = a;
    const lcs_vip_config_t *vb = b;
    return strcmp(va->name, vb->name);
}

static void sort_config(lcs_config_t *cfg)
{
    qsort(cfg->nodes, cfg->node_count, sizeof(cfg->nodes[0]), compare_node_config);
    qsort(cfg->groups, cfg->group_count, sizeof(cfg->groups[0]), compare_group_config);
    qsort(cfg->vips, cfg->vip_count, sizeof(cfg->vips[0]), compare_vip_config);
}

static void strip_inline_comment(char *line)
{
    for (char *p = line; *p; p++)
    {
        if (*p == '#' || *p == ';')
        {
            *p = '\0';
            return;
        }
    }
}

int lcs_config_load(const char *path, lcs_config_t *cfg, char *err, size_t err_len)
{
    lcs_config_init_defaults(cfg);
    FILE *f = fopen(path, "r");
    if (!f)
    {
        set_err(err, err_len, 0, "failed to open config");
        return -1;
    }
    char line_buf[512];
    unsigned line_no = 0;
    section_t sec = { SEC_NONE, -1 };
    while (fgets(line_buf, sizeof(line_buf), f))
    {
        line_no++;
        strip_inline_comment(line_buf);
        char *line = lcs_trim(line_buf);
        if (*line == '\0' || *line == '#' || *line == ';')
            continue;

        if (*line == '[')
        {
            if (parse_section(cfg, line, &sec, err, err_len, line_no) != 0)
            {
                fclose(f);
                return -1;
            }
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq)
        {
            set_err(err, err_len, line_no, "expected key = value");
            fclose(f);
            return -1;
        }
        *eq = '\0';
        char *key = lcs_trim(line);
        char *value = lcs_trim(eq + 1);
        if (*key == '\0' || *value == '\0')
        {
            set_err(err, err_len, line_no, "empty key or value");
            fclose(f);
            return -1;
        }
        if (apply_key(cfg, sec, key, value, err, err_len, line_no) != 0)
        {
            if (err && !*err)
                set_err(err, err_len, line_no, "invalid value");

            fclose(f);
            return -1;
        }
    }
    fclose(f);
    sort_config(cfg);
    return lcs_config_validate(cfg, err, err_len);
}

int lcs_config_self_index(const lcs_config_t *cfg)
{
    return lcs_config_node_index(cfg, cfg->self_name);
}

int lcs_config_node_index(const lcs_config_t *cfg, const char *name)
{
    for (size_t i = 0; i < cfg->node_count; i++)
    {
        if (strcmp(cfg->nodes[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

int lcs_config_group_index(const lcs_config_t *cfg, const char *name)
{
    for (size_t i = 0; i < cfg->group_count; i++)
    {
        if (strcmp(cfg->groups[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

int lcs_config_vip_index(const lcs_config_t *cfg, const char *name)
{
    for (size_t i = 0; i < cfg->vip_count; i++)
    {
        if (strcmp(cfg->vips[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

static int validate_groups_and_assign_vips(lcs_config_t *cfg, char *err, size_t err_len)
{
    for (size_t i = 0; i < cfg->group_count; i++)
    {
        const lcs_group_config_t *group = &cfg->groups[i];
        if (group->type != LCS_GROUP_KEEP_TOGETHER &&
            group->type != LCS_GROUP_ANTI_AFFINITY)
        {
            set_err(err, err_len, 0, "group type is required");
            return -1;
        }
        if (group->mode != LCS_GROUP_MODE_STRICT &&
            group->mode != LCS_GROUP_MODE_BEST_EFFORT)
        {
            set_err(err, err_len, 0, "group mode is required");
            return -1;
        }
    }

    for (size_t i = 0; i < cfg->vip_count; i++)
    {
        lcs_vip_config_t *vip = &cfg->vips[i];
        if (!vip->priority_set)
            vip->priority = (uint32_t)i + 1u;
        if (*vip->group_name)
        {
            int group_idx = lcs_config_group_index(cfg, vip->group_name);
            if (group_idx < 0)
            {
                set_err(err, err_len, 0, "vip references unknown group");
                return -1;
            }
            vip->group_idx = group_idx;
        } else {
            vip->group_idx = -1;
        }
    }

    for (size_t i = 0; i < cfg->vip_count; i++)
    {
        const lcs_vip_config_t *a = &cfg->vips[i];
        if (a->group_idx < 0)
            continue;
        for (size_t j = i + 1; j < cfg->vip_count; j++)
        {
            const lcs_vip_config_t *b = &cfg->vips[j];
            if (a->group_idx == b->group_idx && a->priority == b->priority)
            {
                set_err(err, err_len, 0, "duplicate vip priority in group");
                return -1;
            }
        }
    }
    return 0;
}

int lcs_config_validate(lcs_config_t *cfg, char *err, size_t err_len)
{
    if (!lcs_valid_name(cfg->cluster_name))
    {
        set_err(err, err_len, 0, "cluster name is required and must be a valid name");
        return -1;
    }
    if (!*cfg->self_name)
    {
        set_err(err, err_len, 0, "cluster node is required");
        return -1;
    }
    if (cfg->node_count == 0)
    {
        set_err(err, err_len, 0, "at least one node is required");
        return -1;
    }
    if (lcs_config_self_index(cfg) < 0)
    {
        set_err(err, err_len, 0, "cluster node does not match any node section");
        return -1;
    }
    if (!valid_socket_path(cfg->socket_path))
    {
        set_err(err, err_len, 0, "cluster socket path must be absolute and fit sockaddr_un");
        return -1;
    }
    if (!valid_pidfile_path(cfg->pidfile_path))
    {
        set_err(err, err_len, 0, "cluster pidfile path must be empty or absolute");
        return -1;
    }
    if (cfg->metrics_enabled)
    {
        if (!valid_ip_or_host(cfg->metrics_bind_address))
        {
            set_err(err, err_len, 0, "metrics bind address is invalid");
            return -1;
        }
        if (cfg->metrics_port == 0)
        {
            set_err(err, err_len, 0, "metrics port cannot be zero");
            return -1;
        }
    }
    for (size_t i = 0; i < cfg->node_count; i++)
    {
        const lcs_node_config_t *node = &cfg->nodes[i];
        if (node->role != LCS_NODE_FULL && node->role != LCS_NODE_QUORUM_ONLY)
        {
            set_err(err, err_len, 0, "node role is required");
            return -1;
        }
        if (!valid_ip_or_host(node->address))
        {
            set_err(err, err_len, 0, "node address is invalid");
            return -1;
        }
        if (node->port == 0)
        {
            set_err(err, err_len, 0, "node port cannot be zero");
            return -1;
        }
    }
    if (validate_groups_and_assign_vips(cfg, err, err_len) != 0)
        return -1;

    for (size_t i = 0; i < cfg->vip_count; i++)
    {
        const lcs_vip_config_t *vip = &cfg->vips[i];
        if (!valid_vip_cidr(vip->address))
        {
            set_err(err, err_len, 0, "vip address must be IPv4/IPv6 CIDR");
            return -1;
        }
        if (!valid_interface_name(vip->interface))
        {
            set_err(err, err_len, 0, "vip interface is invalid or too long");
            return -1;
        }
        if (!valid_pidfile_path(vip->pre_start) ||
            !valid_pidfile_path(vip->post_start) ||
            !valid_pidfile_path(vip->pre_stop) ||
            !valid_pidfile_path(vip->post_stop))
        {
            set_err(err, err_len, 0, "vip hook paths must be empty or absolute");
            return -1;
        }
    }
    if (cfg->hook_timeout_ms == 0)
    {
        set_err(err, err_len, 0, "hook_timeout_ms cannot be zero");
        return -1;
    }
    if (cfg->renew_ms >= cfg->lease_ms)
    {
        set_err(err, err_len, 0, "renew_ms must be lower than lease_ms");
        return -1;
    }
    return 0;
}

uint32_t lcs_config_quorum(const lcs_config_t *cfg)
{
    return (uint32_t)(cfg->node_count / 2u) + 1u;
}
