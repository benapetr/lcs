// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool allows(const lcs_source_allowlist_t *list, const char *address)
{
    struct sockaddr_storage source = {0};
    struct sockaddr_in *v4 = (struct sockaddr_in *)&source;
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&source;
    if (inet_pton(AF_INET, address, &v4->sin_addr) == 1)
        v4->sin_family = AF_INET;
    else
    {
        assert(inet_pton(AF_INET6, address, &v6->sin6_addr) == 1);
        v6->sin6_family = AF_INET6;
    }
    return lcs_source_allowlist_contains(list, (const struct sockaddr *)&source);
}

static void test_private_ipv4_defaults(void)
{
    lcs_config_t cfg;
    lcs_config_init_defaults(&cfg);
    assert(strcmp(cfg.allow_from, LCS_DEFAULT_ALLOW_FROM) == 0);
    const char *allowed[] = {
        "10.0.0.0", "10.255.255.255", "172.16.0.0", "172.31.255.255",
        "192.168.0.0", "192.168.255.255", "::ffff:192.168.1.2",
    };
    const char *denied[] = {
        "9.255.255.255", "11.0.0.0", "172.15.255.255", "172.32.0.0",
        "192.167.255.255", "192.169.0.0", "8.8.8.8", "127.0.0.1",
        "169.254.1.1", "100.64.0.1", "::1", "fd00::1", "2001:db8::1",
        "::ffff:8.8.8.8", "::ffff:127.0.0.1",
    };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
        assert(allows(&cfg.source_allowlist, allowed[i]));
    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); i++)
        assert(!allows(&cfg.source_allowlist, denied[i]));
}

static void test_custom_policy_and_bypass(void)
{
    lcs_source_allowlist_t list;
    assert(lcs_source_allowlist_parse(&list, " 127.0.0.1/32, 2001:db8::/33, ::1/128 ") == 0);
    assert(allows(&list, "127.0.0.1"));
    assert(!allows(&list, "127.0.0.2"));
    assert(!allows(&list, "10.0.0.1")); /* Explicit lists replace defaults. */
    assert(allows(&list, "2001:db8:7fff::1"));
    assert(!allows(&list, "2001:db8:8000::1"));
    assert(allows(&list, "::1"));
    assert(!allows(&list, "::2"));
    assert(allows(&list, "::ffff:127.0.0.1"));

    assert(lcs_source_allowlist_parse(&list, "192.0.2.129/25") == 0);
    assert(allows(&list, "192.0.2.128"));
    assert(allows(&list, "192.0.2.255"));
    assert(!allows(&list, "192.0.2.127"));
    assert(lcs_source_allowlist_parse(&list, "2001:db8::2/127") == 0);
    assert(allows(&list, "2001:db8::2"));
    assert(allows(&list, "2001:db8::3"));
    assert(!allows(&list, "2001:db8::4"));

    assert(lcs_source_allowlist_parse(&list, "::/0") == 0);
    assert(allows(&list, "2001:db8::1"));
    assert(!allows(&list, "8.8.8.8"));
    assert(lcs_source_allowlist_parse(&list, "0.0.0.0/0") == 0);
    assert(list.disabled);
    assert(allows(&list, "8.8.8.8"));
    assert(allows(&list, "::1"));
    assert(allows(&list, "2001:db8::1"));
}

static void test_invalid_policies_are_atomic(void)
{
    lcs_source_allowlist_t list;
    assert(lcs_source_allowlist_parse(&list, LCS_DEFAULT_ALLOW_FROM) == 0);
    lcs_source_allowlist_t before = list;
    const char *invalid[] = {
        "", " ", ",", "10.0.0.0/8,", ",10.0.0.0/8", "10.0.0.0/8,,::1/128",
        "localhost/32", "10.0.0.1", "10.0.0.0/", "10.0.0.0/-1", "10.0.0.0/+8",
        "10.0.0.0/33", "10.0.0.0/8garbage", "10.0.0.0/8/8", "::/129",
        "::1%lo/128", "0.0.0.0/0,invalid", "10.0.0.0/999999999999999999999",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
    {
        assert(lcs_source_allowlist_parse(&list, invalid[i]) != 0);
        assert(memcmp(&before, &list, sizeof(list)) == 0);
    }
    char excessive[LCS_ALLOW_FROM_MAX + 2];
    memset(excessive, '1', sizeof(excessive) - 1);
    excessive[sizeof(excessive) - 1] = '\0';
    assert(lcs_source_allowlist_parse(&list, excessive) != 0);
    excessive[0] = '\0';
    for (int i = 0; i < LCS_SOURCE_NETWORK_MAX + 1; i++)
        strcat(excessive, i ? ",10.0.0.0/8" : "10.0.0.0/8");
    assert(lcs_source_allowlist_parse(&list, excessive) != 0);
    assert(memcmp(&before, &list, sizeof(list)) == 0);
}

static void test_config_validation(void)
{
    char path[] = "/tmp/lcs-allowlist-config.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    const char *policies[] = { NULL, "127.0.0.0/8,::1/128", "0.0.0.0/0", "", "bad/24" };
    for (size_t i = 0; i < sizeof(policies) / sizeof(policies[0]); i++)
    {
        FILE *file = fopen(path, "w");
        assert(file);
        fputs("[cluster]\nname=test\nnode=node1\n", file);
        if (policies[i])
            fprintf(file, "allow_from=%s\n", policies[i]);
        fputs("[node node1]\nrole=quorum-only\naddress=127.0.0.1\n", file);
        assert(fclose(file) == 0);
        lcs_config_t cfg;
        char error[256] = {0};
        int rc = lcs_config_load(path, &cfg, error, sizeof(error));
        if (i < 3)
        {
            assert(rc == 0);
            assert(allows(&cfg.source_allowlist, "127.0.0.1") == (i != 0));
        } else
        {
            assert(rc != 0);
            assert(strstr(error, i == 3 ? "empty key or value" : "allow_from"));
        }
    }
    unlink(path);
}

static void test_peers_only_validation(void)
{
    const struct
    {
        bool enabled;
        const char *local_address;
        const char *remote_address;
        bool valid;
    } cases[] = {
        { false, NULL, "node2.example.test", true },
        { true, "127.0.0.1", "127.0.0.2", true },
        { true, "::1", "2001:db8::2", true },
        { true, "127.0.0.1", "2001:db8::2", true },
        { true, NULL, "127.0.0.2", false },
        { true, "127.0.0.1", NULL, false },
        { true, "127.0.0.1", "node2.example.test", false },
        { true, "localhost", "127.0.0.2", false },
        { true, "127.0.0.1", "127.0.0.2/32", false },
        { true, "127.0.0.1", "127.0.0.2:3322", false },
    };
    char path[] = "/tmp/lcs-peers-only-config.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        FILE *file = fopen(path, "w");
        assert(file);
        fprintf(file, "[cluster]\nname=test\nnode=node1\npeers_only=%s\n"
                      "[node node1]\nrole=full-member\n",
                      cases[i].enabled ? "true" : "false");
        if (cases[i].local_address)
            fprintf(file, "address=%s\n", cases[i].local_address);
        fputs("[node node2]\nrole=quorum-only\n", file);
        if (cases[i].remote_address)
            fprintf(file, "address=%s\n", cases[i].remote_address);
        assert(fclose(file) == 0);
        lcs_config_t cfg;
        char error[256] = {0};
        int rc = lcs_config_load(path, &cfg, error, sizeof(error));
        if (cases[i].valid)
        {
            assert(rc == 0);
            assert(cfg.peers_only == cases[i].enabled);
        } else
        {
            assert(rc != 0);
            assert(strstr(error, "peers_only"));
        }
    }
    unlink(path);
}

static void test_peer_source_matching(void)
{
    lcs_config_t cfg;
    lcs_config_init_defaults(&cfg);
    assert(!cfg.peers_only);
    strcpy(cfg.self_name, "node1");
    cfg.node_count = 3;
    const char *ips[] = { "127.0.0.1", "192.0.2.2", "2001:db8::3" };
    for (size_t i = 0; i < cfg.node_count; i++)
    {
        snprintf(cfg.nodes[i].name, sizeof(cfg.nodes[i].name), "node%zu", i + 1);
        assert(lcs_source_parse_ip(ips[i], &cfg.nodes[i].numeric_address) == 0);
    }
    const char *sources[] = {
        "127.0.0.1", "192.0.2.2", "2001:db8::3", "192.0.2.4", "::ffff:192.0.2.2",
    };
    const bool expected[] = { false, true, true, false, true };
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++)
    {
        struct sockaddr_storage source;
        assert(lcs_source_parse_ip(sources[i], &source) == 0);
        assert(lcs_config_source_is_peer(&cfg, (const struct sockaddr *)&source) == expected[i]);
    }
    struct sockaddr_storage a, b;
    assert(lcs_source_parse_ip("2001:db8::3", &a) == 0);
    assert(lcs_source_parse_ip("2001:0db8:0:0:0:0:0:3", &b) == 0);
    assert(lcs_source_same_ip((const struct sockaddr *)&a, (const struct sockaddr *)&b));
    assert(lcs_source_parse_ip("2001:db8::4", &b) == 0);
    assert(!lcs_source_same_ip((const struct sockaddr *)&a, (const struct sockaddr *)&b));
    assert(lcs_source_parse_ip("localhost", &a) != 0);
    assert(!lcs_config_source_is_peer(&cfg, (const struct sockaddr *)&a));
}

int main(void)
{
    test_private_ipv4_defaults();
    test_custom_policy_and_bypass();
    test_invalid_policies_are_atomic();
    test_config_validation();
    test_peers_only_validation();
    test_peer_source_matching();
    return 0;
}
