// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef LCS_CONFIG_AGREEMENT_H
#define LCS_CONFIG_AGREEMENT_H

#include "config_store.h"
#include "resource_config.h"

#define LCS_CONFIG_DIGEST_SIZE 65
#define LCS_CONFIG_REQUEST_MAX 63
#define LCS_CONFIG_JOURNAL_MAX (LCS_RESOURCE_DOCUMENT_MAX * 2u + 16384u)

typedef struct
{
    char request_id[LCS_CONFIG_REQUEST_MAX + 1];
    char digest[LCS_CONFIG_DIGEST_SIZE];
    char *document;
    size_t length;
    uint64_t revision;
} lcs_config_value_t;

/* Durable acceptor state for the next configuration revision. This core does
 * synchronous I/O and must run in a storage worker, never the lease event loop.
 * A successful return is the durability boundary before replying to a peer.
 * Storage failure poisons the handle: reopen/recover before further voting. */
typedef struct
{
    lcs_config_store_t *store; /* borrowed, exclusively locked by the caller */
    const lcs_config_t *base; /* borrowed static settings, immutable for handle lifetime */
    char membership[LCS_CONFIG_DIGEST_SIZE];
    char member[LCS_NAME_MAX + 1];
    lcs_config_value_t committed;
    char parent_digest[LCS_CONFIG_DIGEST_SIZE];
    uint64_t committed_ballot;
    uint32_t committed_voters;
    uint64_t promised;
    uint64_t accepted_ballot;
    lcs_config_value_t accepted;
    bool poisoned;
} lcs_config_agreement_t;

enum
{
    LCS_CONFIG_AGREED = 0,
    LCS_CONFIG_REJECTED = 1,
    LCS_CONFIG_STALE = 2,
};

void lcs_config_value_clear(lcs_config_value_t *value);
int lcs_config_value_init(lcs_config_value_t *value, const lcs_config_t *base,
                          const char *request_id, const char *document, size_t length,
                          char *err, size_t err_len);
int lcs_config_value_copy(lcs_config_value_t *dst, const lcs_config_value_t *src);
bool lcs_config_value_equal(const lcs_config_value_t *a, const lcs_config_value_t *b);

/* Opening a never-initialized journal must be explicitly authorized by bootstrap
 * coordination. Missing journal is an error when allow_initialize is false.
 * Existing records are bound to both membership/timing and the local member.
 * base must be validated and use canonical sorted node names. */
int lcs_config_agreement_open(lcs_config_agreement_t *agreement,
                              lcs_config_store_t *store, const lcs_config_t *base,
                              bool allow_initialize, char *err, size_t err_len);
void lcs_config_agreement_close(lcs_config_agreement_t *agreement);
/* Local worker handoff only; these records must never be imported as peer
 * decision evidence. Import verifies local identity and journal checksum. */
int lcs_config_agreement_export(const lcs_config_agreement_t *agreement,
                                char **document, size_t *length,
                                char *err, size_t err_len);
int lcs_config_agreement_import(lcs_config_agreement_t *agreement,
                                const char *document, size_t length,
                                char *err, size_t err_len);

/* Ballots encode a monotonically increasing counter and sorted proposer index.
 * The caller must persist its own prepare before transmitting that ballot. */
uint64_t lcs_config_agreement_next_ballot(const lcs_config_agreement_t *agreement,
                                         uint64_t observed);
bool lcs_config_agreement_ballot_valid(const lcs_config_agreement_t *agreement,
                                      uint64_t ballot);
int lcs_config_agreement_prepare(lcs_config_agreement_t *agreement,
                                 uint64_t revision, const char *parent_digest,
                                 uint64_t ballot, char *err, size_t err_len);
int lcs_config_agreement_accept(lcs_config_agreement_t *agreement,
                                const char *parent_digest, uint64_t ballot,
                                const lcs_config_value_t *value,
                                char *err, size_t err_len);
/* The voter mask is evidence collected by the correct, authenticated transport
 * coordinator, not a cryptographic certificate. This is a non-Byzantine protocol.
 * Learning a decision changes metadata only, never starts/stops resources. */
int lcs_config_agreement_learn(lcs_config_agreement_t *agreement,
                               const char *parent_digest, uint64_t ballot,
                               uint32_t voters, const lcs_config_value_t *value,
                               char *err, size_t err_len);

/* A proposer collects distinct phase-one promises for one exact slot/ballot.
 * Its phase-two value must be the highest-ballot accepted value reported by the
 * promise quorum, or the requested value if none was previously accepted. */
typedef struct
{
    uint64_t revision;
    uint64_t ballot;
    char parent_digest[LCS_CONFIG_DIGEST_SIZE];
    uint32_t promises;
    uint32_t acknowledgements;
    uint64_t highest_accepted;
    lcs_config_value_t requested;
    lcs_config_value_t recovered;
    bool sealed;
    bool failed;
} lcs_config_proposal_t;

int lcs_config_proposal_init(lcs_config_proposal_t *proposal,
                             const lcs_config_agreement_t *local, uint64_t ballot,
                             const lcs_config_value_t *requested);
void lcs_config_proposal_clear(lcs_config_proposal_t *proposal);
int lcs_config_proposal_promise(lcs_config_proposal_t *proposal,
                                const lcs_config_agreement_t *membership,
                                size_t member, uint64_t revision,
                                const char *parent_digest, uint64_t promised,
                                uint64_t accepted_ballot,
                                const lcs_config_value_t *accepted);
const lcs_config_value_t *lcs_config_proposal_seal(lcs_config_proposal_t *proposal,
                                                 const lcs_config_agreement_t *membership);
int lcs_config_proposal_ack(lcs_config_proposal_t *proposal,
                            const lcs_config_agreement_t *membership, size_t member,
                            uint64_t ballot, const lcs_config_value_t *value);
bool lcs_config_proposal_chosen(const lcs_config_proposal_t *proposal,
                                const lcs_config_agreement_t *membership);

#endif
