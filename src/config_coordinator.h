// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_COORDINATOR_H
#define LCS_CONFIG_COORDINATOR_H

#include "config_readiness.h"
#include "config_agreement_worker.h"

typedef enum
{
    LCS_CONFIG_COORD_IDLE = 0,
    LCS_CONFIG_COORD_COLLECTING,
    LCS_CONFIG_COORD_PERSISTING,
    LCS_CONFIG_COORD_READY,
    LCS_CONFIG_COORD_VOTING,
    LCS_CONFIG_COORD_APPLYING,
    LCS_CONFIG_COORD_PROJECTING,
    LCS_CONFIG_COORD_REQUESTING,
    LCS_CONFIG_COORD_ACCEPT_RESERVING,
    LCS_CONFIG_COORD_CANCELLING,
    LCS_CONFIG_COORD_STOPPED,
    LCS_CONFIG_COORD_FAILED,
} lcs_config_coordinator_phase_t;

typedef struct
{
    lcs_config_agreement_worker_t storage; /* One writer for all configuration jobs. */
    lcs_config_runtime_t *runtime;
    lcs_config_readiness_t *readiness;
    lcs_config_barrier_round_t persisted_round;
    uint64_t requester_instance;
    uint64_t attempt;
    uint64_t completed_votes; /* Process-local completion counter, not durable request history. */
    int last_vote_result;
    uint64_t completed_applications;
    int last_application_result;
    uint64_t completed_projections;
    int last_projection_result;
    uint64_t completed_requests; /* Local storage completions, not client outcomes. */
    int last_request_result;
    lcs_config_value_t accepting; /* Owned across reservation; cleared before voting/shutdown. */
    uint64_t accepting_ballot;
    char accepting_parent[LCS_CONFIG_DIGEST_SIZE];
    uint64_t projecting_revision, projected_revision;
    char projecting_digest[LCS_CONFIG_DIGEST_SIZE], projected_digest[LCS_CONFIG_DIGEST_SIZE];
    const void *application_owner; /* Reconciler holds voting through admission. */
    lcs_config_coordinator_phase_t phase;
    bool initialized;
    bool closing;
    bool failed;
    char error[256];
} lcs_config_coordinator_t;

/* Fresh object only; stop before releasing references/storage, then process
 * cancellation until can_close returns true.
 * Reinitialization after failure requires disk recovery, not stale handle reuse. */
void lcs_config_coordinator_init(lcs_config_coordinator_t *coordinator,
                                  lcs_config_runtime_t *runtime,
                                  lcs_config_readiness_t *readiness);
/* Attach an explicitly started readiness attempt. No automatic initialization,
 * file I/O, authority switch or backend work occurs here. Returns 1 if busy. */
int lcs_config_coordinator_start(lcs_config_coordinator_t *coordinator);
/* After runtime_open, adopt an existing valid durable barrier without collecting
 * readiness again or I/O. Application may lag for sync recovery; votes stay gated. */
int lcs_config_coordinator_recover(lcs_config_coordinator_t *coordinator);
/* Explicit local PREPARE/ACCEPT admission through the SAME writer. A durable
 * barrier and caught-up application are mandatory; ACCEPT requires schema 2 and
 * the exact successor rules (activation preserves frozen definitions). Caller
 * still verifies the transport proposer/ballot identity and quorum policy.
 * Returns 0 started, 1 busy, -1 rejected before any write. Completion advances
 * completed_votes and reports last_vote_result; the network acceptor uses these
 * same gated operations. ACCEPT also requires healthy existing request history;
 * it persists/checks the selected identity before acceptance. A matching known
 * identity permits protocol recovery, not a new client proposal. The vote counter
 * advances only after acceptance or a definitive rejection; reservation alone
 * cannot generate a positive ACK. Stop retains child handles until reaped. */
int lcs_config_coordinator_start_vote(lcs_config_coordinator_t *coordinator,
                                       const lcs_config_agreement_job_t *job,
                                       char *err, size_t err_len);
/* Learn only a validated immediate schema-2 successor with a configured voting
 * majority. A duplicate exact current decision may be acknowledged while local
 * application is behind. Learning never applies backends or enables authority. */
int lcs_config_coordinator_start_learn(lcs_config_coordinator_t *coordinator,
                                       const lcs_config_agreement_job_t *job,
                                       char *err, size_t err_len);
/* Catch up committed metadata, including revision gaps, with majority evidence.
 * Requires the existing durable barrier; application/pending cleanup are retained.
 * It does not grant a vote while application is behind or authorize initialization. */
int lcs_config_coordinator_start_checkpoint(lcs_config_coordinator_t *coordinator,
                                            const lcs_config_agreement_job_t *job,
                                            char *err, size_t err_len);
/* Catalog-only application writes through the same worker. The reconciler must
 * hold application_owner before BEGIN/FINISH. FINISH records the exact pending
 * digest after the caller's backend/owner/install barriers. No backend I/O here. */
int lcs_config_coordinator_start_application(lcs_config_coordinator_t *coordinator,
                                             const void *owner, const lcs_config_agreement_job_t *job,
                                             char *err, size_t err_len);
/* Derived projection only. Requires the existing durable barrier and excludes
 * all other configuration writers, but does not change authority or application.
 * Completion records the exact projected revision/digest, separately from votes
 * and catalogs. A completed I/O error leaves agreement/catalog handles healthy. */
int lcs_config_coordinator_start_projection(lcs_config_coordinator_t *coordinator,
                                            lcs_config_bootstrap_t *bootstrap,
                                            char *err, size_t err_len);
/* Request metadata through the SAME writer. OPEN recovers existing history only;
 * initialization authorization is deliberately not inferred here. RESERVE for a
 * new ID requires voting eligibility and a valid dynamic successor. Known IDs
 * can return EXISTS/CONFLICT while application lags; that never resubmits them.
 * OBSERVE records the current learned decision even during application recovery.
 * A durable barrier is mandatory for every action. Completion uses its own
 * counter/result and never claims voting, application, admission or client success.
 * Failure/cancellation retains ordinary poisoning and child-reaping rules. */
int lcs_config_coordinator_start_requests(lcs_config_coordinator_t *coordinator,
                                            const lcs_config_agreement_job_t *job,
                                            char *err, size_t err_len);
/* Nonblocking: collect -> shared barrier writer -> durable READY. Returns 1
 * while proof is eligible, -1 for terminal failure, 0 for pending/stopped/idle.
 * Never publish a result for a replaced readiness attempt. */
int lcs_config_coordinator_process(lcs_config_coordinator_t *coordinator, uint64_t now_ms);
void lcs_config_coordinator_stop(lcs_config_coordinator_t *coordinator);
void lcs_config_coordinator_abort(lcs_config_coordinator_t *coordinator, const char *reason);
bool lcs_config_coordinator_can_close(const lcs_config_coordinator_t *coordinator);
/* Local configuration-vote prerequisite only; normal quorum, recovery, wire
 * admission and application rules remain required. Does not start a vote. */
bool lcs_config_coordinator_can_vote(const lcs_config_coordinator_t *coordinator);
#endif
