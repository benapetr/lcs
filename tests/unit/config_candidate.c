// SPDX-License-Identifier: GPL-3.0-or-later

#include "config_candidate.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static lcs_config_t base, proposed, output, before;
static char error[256];

static void rejected(void)
{
    memset(&output, 0x5b, sizeof(output));
    before = output;
    *error = '\0';
    assert(lcs_config_candidate_prepare(&base, &proposed, &output,
                                        error, sizeof(error)) == -1);
    assert(*error && memcmp(&output, &before, sizeof(output)) == 0);
}

#define STATIC_CHANGE(statement) do { proposed = base; statement; rejected(); } while (0)

int main(void)
{
    char path[] = "/tmp/lcs-config-candidate.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    assert(f);
    assert(fputs("[cluster]\nname = test\nnode = node1\n"
                 "[node node1]\naddress = 127.0.0.1\nrole = full-member\n"
                 "[node node2]\naddress = 127.0.0.2\nrole = quorum-only\n"
                 "[group web]\ntype = keep-together\nmode = best-effort\n"
                 "[vip vip]\naddress = 192.0.2.10/32\ninterface = eth0\n"
                 "[service app]\nsystemd_unit = app.service\n"
                 "depends_on = vip\ngroup = web\n", f) >= 0);
    assert(fclose(f) == 0);
    if (lcs_config_load(path, &base, error, sizeof(error))) { fprintf(stderr, "%s\n", error); abort(); }
    assert(unlink(path) == 0);
    assert(lcs_config_candidate_prepare(&base, &base, &output, error, sizeof(error)) == 0);
    assert(lcs_config_resources_equal(&base, &output));

    STATIC_CHANGE(strcpy(proposed.cluster_name, "other"));
    STATIC_CHANGE(strcpy(proposed.self_name, "node2"));
    STATIC_CHANGE(proposed.node_count = 1);
    STATIC_CHANGE(proposed.nodes[1].role = LCS_NODE_FULL);
    STATIC_CHANGE(strcpy(proposed.nodes[1].address, "127.0.0.3"));
    STATIC_CHANGE(proposed.nodes[1].port++);
#define STRING(field, value) STATIC_CHANGE(strcpy(proposed.field, value))
    STRING(bind_address, "127.0.0.9");
    STRING(metrics_bind_address, "127.0.0.9");
    STRING(socket_path, "/tmp/other.sock");
    STRING(pidfile_path, "/tmp/other.pid");
    STRING(secret, "other");
    STRING(allow_from, "0.0.0.0/0");
#undef STRING
#define NUMBER(field) STATIC_CHANGE(proposed.field++)
    NUMBER(port);
    NUMBER(metrics_port);
    NUMBER(lease_ms);
    NUMBER(renew_ms);
    NUMBER(peer_timeout_ms);
    NUMBER(probe_count);
    NUMBER(probe_timeout_ms);
    NUMBER(hook_timeout_ms);
#undef NUMBER
    STATIC_CHANGE(proposed.peers_only = !base.peers_only);
    STATIC_CHANGE(proposed.syslog_enabled = !base.syslog_enabled);
    STATIC_CHANGE(proposed.metrics_enabled = !base.metrics_enabled);
    STATIC_CHANGE(proposed.vip_backend = base.vip_backend == LCS_VIP_BACKEND_NETLINK ?
                  LCS_VIP_BACKEND_IP : LCS_VIP_BACKEND_NETLINK);

    proposed = base;
    strcpy(proposed.resources[0].depends_on_names[0], "missing");
    rejected();
    proposed = base;
    proposed.resource_count = LCS_MAX_RESOURCES + 1;
    rejected();

    /* Adding an earlier-sorting resource must recompute dependency indices;
     * the active input and its existing identities remain untouched. */
    proposed = base;
    proposed.resources[2] = proposed.resources[1];
    strcpy(proposed.resources[2].name, "aaa");
    strcpy(proposed.resources[2].address, "192.0.2.11/32");
    proposed.resource_count++;
    before = base;
    assert(lcs_config_candidate_prepare(&base, &proposed, &proposed, error, sizeof(error)) == 0);
    assert(memcmp(&base, &before, sizeof(base)) == 0);
    assert(strcmp(proposed.resources[0].name, "aaa") == 0);
    assert(proposed.resources[1].depends_on_idx[0] == 2);
    assert(lcs_config_backend_equal(&base.resources[0], &proposed.resources[1]));
    assert(!lcs_config_backend_equal(&base.resources[1], &proposed.resources[0]));
    assert(!lcs_config_resources_equal(&base, &proposed));

    /* Legacy no-op comparison uses effective values. Dynamic agreement uses
     * the full canonical JSON digest, including explicit/default intent. */
    proposed = base;
    proposed.resources[0].priority++;
    assert(!lcs_config_resources_equal(&base, &proposed));
    assert(lcs_config_backend_equal(&base.resources[0], &proposed.resources[0]));
    proposed = base;
    strcpy(proposed.resources[0].post_stop, "/bin/true");
    assert(!lcs_config_resources_equal(&base, &proposed));
    assert(lcs_config_backend_equal(&base.resources[0], &proposed.resources[0]));
    puts("shared configuration candidate unit tests passed");
    return 0;
}
