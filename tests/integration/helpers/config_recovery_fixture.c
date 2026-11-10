// SPDX-License-Identifier: GPL-3.0-or-later
/* Test-only interrupted activated-state generator. Not an activation command. */
#include "config_barrier.h"
#include "config_requests.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char error[256];
#define CHECK(call) do { if ((call) != 0) { fprintf(stderr, "%s: %s\n", #call, error); return 1; } } while (0)

int main(int argc, char **argv)
{
    bool advance = argc == 3 && !strcmp(argv[2], "advance");
    bool reserved = argc == 3 && !strcmp(argv[2], "reserved");
    bool history = reserved || (argc == 3 && !strcmp(argv[2], "history"));
    if (argc != 2 && !advance && !history) return 2;
    lcs_config_t *cfg = calloc(5, sizeof(*cfg));
    if (!cfg) return 1;
    CHECK(lcs_config_load(argv[1], &cfg[0], error, sizeof(error)));
    char directory[4096]; snprintf(directory, sizeof(directory), "%s.state", argv[1]);
    lcs_config_store_t store;
    lcs_config_activation_t activation = {0};
    lcs_config_agreement_t agreement = {0};
    lcs_config_application_t application = {0};
    lcs_config_barrier_round_t round = {0};
    lcs_config_barrier_t barrier = {0};
    CHECK(lcs_config_store_open(&store, directory, error, sizeof(error)));
    CHECK(lcs_config_activation_open(&activation, &store, &cfg[0], &cfg[1], !history, error, sizeof(error)));
    CHECK(lcs_config_activation_resume(&activation, &agreement, &application, error, sizeof(error)));
    if (history)
    {
        lcs_config_requests_t requests = {0};
        CHECK(lcs_config_requests_open(&requests, &store, &cfg[1], true, error, sizeof(error)));
        CHECK(lcs_config_requests_observe(&requests, &agreement, error, sizeof(error)));
        if (reserved)
        {
            uint64_t previous; char *document; size_t length;
            CHECK(lcs_resource_config_decode(agreement.committed.document, agreement.committed.length,
                &cfg[1], &cfg[2], &previous, error, sizeof(error)));
            CHECK(lcs_resource_config_assign_identities(&cfg[2], previous, &cfg[2], &cfg[3], error, sizeof(error)));
            CHECK(lcs_resource_config_encode(&cfg[3], previous + 1, &document, &length, error, sizeof(error)));
            lcs_config_value_t pending = {0};
            CHECK(lcs_config_value_init(&pending, &cfg[1], "pending-request", document, length, error, sizeof(error)));
            CHECK(lcs_config_requests_reserve(&requests, &agreement, previous, agreement.committed.digest,
                &pending, error, sizeof(error)));
            lcs_config_value_clear(&pending); free(document);
        }
        lcs_config_requests_close(&requests);
        goto done;
    }
    if (advance)
    {
        /* A stopped quorum-only fixture relays a pre-decided checkpoint when
         * it rejoins. Never edit a running store or expose a test daemon command. */
        if (strcmp(activation.member, "node3") || agreement.committed.revision < 3 || agreement.committed.revision >= 8) return 2;
        CHECK(lcs_config_barrier_open(&barrier, &activation, &agreement, &application, NULL, error, sizeof(error)));
    }
    else
    {
        CHECK(lcs_config_barrier_begin(&round, &activation, &agreement, &application, error, sizeof(error)));
        for (size_t i = 0; i < cfg[0].node_count; i++)
            CHECK(lcs_config_barrier_ready(&round, i, round.membership, round.baseline, error, sizeof(error)));
        CHECK(lcs_config_barrier_seal(&round, error, sizeof(error)));
        CHECK(lcs_config_barrier_open(&barrier, &activation, &agreement, &application, &round, error, sizeof(error)));
    }
    unsigned begin = advance ? (unsigned)agreement.committed.revision + 1 : 1;
    for (unsigned revision = begin; revision <= (advance ? begin : 3); revision++)
    {
        uint64_t previous;
        CHECK(lcs_resource_config_decode(agreement.committed.document, agreement.committed.length,
            &cfg[1], &cfg[2], &previous, error, sizeof(error)));
        cfg[3] = cfg[2];
        if (revision > 1 && revision <= 4) snprintf(cfg[3].resources[0].address, sizeof(cfg[3].resources[0].address),
            "127.0.0.%u/32", 199 + revision);
        if (revision == 5)
        {
            cfg[3].resources[1] = cfg[3].resources[0];
            strcpy(cfg[3].resources[1].name, "vip2"); strcpy(cfg[3].resources[1].address, "127.0.0.204/32");
            cfg[3].resource_count = 2;
        }
        if (revision == 6) { cfg[3].resources[0].priority = 66; cfg[3].resources[0].priority_set = true; }
        if (revision >= 7) cfg[3].resource_count = revision == 7 ? 1 : 0;
        CHECK(lcs_resource_config_assign_identities(&cfg[2], previous, &cfg[3], &cfg[4], error, sizeof(error)));
        char *document; size_t length; char request[32];
        CHECK(lcs_resource_config_encode(&cfg[4], revision, &document, &length, error, sizeof(error)));
        snprintf(request, sizeof(request), "recovery-%u", revision);
        lcs_config_value_t value = {0};
        CHECK(lcs_config_value_init(&value, &cfg[1], request, document, length, error, sizeof(error)));
        CHECK(lcs_config_agreement_learn(&agreement, agreement.committed.digest, revision * 32, 5,
            &value, error, sizeof(error)));
        /* node1: applied 2, pending 3; node2: applied 1, pending 2;
         * quorum-only node3: applied 3. All have learned revision 3. */
        bool second = !strcmp(activation.member, "node2");
        bool witness = !strcmp(activation.member, "node3");
        if (!advance && (!second || revision <= 2))
        {
            CHECK(lcs_config_application_begin(&application, &agreement, error, sizeof(error)));
            if (witness || revision == 1 || (!second && revision == 2))
                CHECK(lcs_config_application_finish(&application, value.digest, error, sizeof(error)));
        }
        lcs_config_value_clear(&value); free(document);
    }
done:
    lcs_config_barrier_close(&barrier);
    lcs_config_application_close(&application);
    lcs_config_agreement_close(&agreement);
    lcs_config_activation_close(&activation);
    lcs_config_store_close(&store); free(cfg);
    return 0;
}
