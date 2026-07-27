// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static lcs_config_t base_config(void)
{
    lcs_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.lease_ms = 5000;
    cfg.renew_ms = 1500;
    cfg.peer_timeout_ms = 5000;
    cfg.probe_count = 2;
    cfg.probe_timeout_ms = 250;
    cfg.hook_timeout_ms = 30000;

    cfg.node_count = 2;
    snprintf(cfg.nodes[0].name, sizeof(cfg.nodes[0].name), "node1");
    cfg.nodes[0].role = LCS_NODE_FULL;
    snprintf(cfg.nodes[1].name, sizeof(cfg.nodes[1].name), "node2");
    cfg.nodes[1].role = LCS_NODE_QUORUM_ONLY;

    cfg.group_count = 1;
    snprintf(cfg.groups[0].name, sizeof(cfg.groups[0].name), "frontend");
    cfg.groups[0].type = LCS_GROUP_KEEP_TOGETHER;
    cfg.groups[0].mode = LCS_GROUP_MODE_STRICT;

    cfg.resource_count = 2;
    cfg.resources[0].type = LCS_RESOURCE_SERVICE;
    snprintf(cfg.resources[0].name, sizeof(cfg.resources[0].name), "api");
    snprintf(cfg.resources[0].group_name, sizeof(cfg.resources[0].group_name), "frontend");
    snprintf(cfg.resources[0].systemd_unit, sizeof(cfg.resources[0].systemd_unit), "api.service");
    cfg.resources[0].priority = 2;

    cfg.resources[1].type = LCS_RESOURCE_VIP;
    snprintf(cfg.resources[1].name, sizeof(cfg.resources[1].name), "vip");
    snprintf(cfg.resources[1].group_name, sizeof(cfg.resources[1].group_name), "frontend");
    snprintf(cfg.resources[1].address, sizeof(cfg.resources[1].address), "192.0.2.10/32");
    snprintf(cfg.resources[1].interface, sizeof(cfg.resources[1].interface), "eth0");
    cfg.resources[1].priority = 1;
    cfg.resources[1].depends_on_count = 1;
    cfg.resources[1].depends_on_idx[0] = 0;
    return cfg;
}

int main(void)
{
    lcs_config_t base = base_config();
    uint64_t voting = lcs_config_voting_fingerprint(&base);
    uint64_t full = lcs_config_full_fingerprint(&base);

    lcs_config_t local = base;
    snprintf(local.self_name, sizeof(local.self_name), "node2");
    snprintf(local.bind_address, sizeof(local.bind_address), "127.0.0.2");
    snprintf(local.socket_path, sizeof(local.socket_path), "/tmp/node2.sock");
    snprintf(local.secret, sizeof(local.secret), "different-check");
    local.port = 9001;
    local.metrics_enabled = true;
    local.syslog_enabled = true;
    local.probe_count = 5;
    local.probe_timeout_ms = 900;
    local.hook_timeout_ms = 5000;
    local.nodes[0].port = 9010;
    snprintf(local.nodes[0].address, sizeof(local.nodes[0].address), "node1.example.test");
    snprintf(local.resources[1].interface, sizeof(local.resources[1].interface), "ens18");
    snprintf(local.resources[1].pre_start, sizeof(local.resources[1].pre_start), "/opt/lcs/pre-start");
    assert(lcs_config_voting_fingerprint(&local) == voting);
    assert(lcs_config_full_fingerprint(&local) == full);

    lcs_config_t changed = base;
    changed.lease_ms++;
    assert(lcs_config_voting_fingerprint(&changed) != voting);

    changed = base;
    changed.nodes[1].role = LCS_NODE_FULL;
    assert(lcs_config_voting_fingerprint(&changed) != voting);

    changed = base;
    snprintf(changed.resources[1].name, sizeof(changed.resources[1].name), "vip2");
    assert(lcs_config_voting_fingerprint(&changed) != voting);

    changed = base;
    changed.groups[0].mode = LCS_GROUP_MODE_BEST_EFFORT;
    assert(lcs_config_voting_fingerprint(&changed) == voting);
    assert(lcs_config_full_fingerprint(&changed) != full);

    changed = base;
    snprintf(changed.resources[1].address, sizeof(changed.resources[1].address), "192.0.2.11/32");
    assert(lcs_config_voting_fingerprint(&changed) == voting);
    assert(lcs_config_full_fingerprint(&changed) != full);

    return 0;
}
