// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef LCS_CONFIG_AGREEMENT_WORKER_H
#define LCS_CONFIG_AGREEMENT_WORKER_H

#include "config_application.h"
#include "config_requests.h"
#include "config_bootstrap.h"
#include "config_runtime.h"
#include "config_initialization.h"
#include <sys/types.h>

typedef enum
{
    LCS_CONFIG_PREPARE = 1,
    LCS_CONFIG_ACCEPT = 2,
    LCS_CONFIG_LEARN = 3,
    LCS_CONFIG_APPLICATION_BEGIN = 4,
    LCS_CONFIG_APPLICATION_FINISH = 5,
    LCS_CONFIG_BARRIER = 6,
    LCS_CONFIG_CHECKPOINT = 7,
    LCS_CONFIG_PROJECTION = 8,
    LCS_CONFIG_REQUEST_RESERVE = 9,
    LCS_CONFIG_REQUEST_OBSERVE = 10,
    LCS_CONFIG_REQUEST_OPEN = 11,
    LCS_CONFIG_REQUEST_APPLY = 12,
} lcs_config_agreement_action_t;

typedef struct
{
    lcs_config_agreement_action_t action;
    const lcs_config_application_t *admission_catalog; /* Borrowed until start returns. */
    const lcs_config_request_admission_t *admission; /* Trusted sealed same-process receipt. */
    bool initialize_requests; /* Explicit pristine-only authorization for OPEN. */
    char client_digest[LCS_CONFIG_DIGEST_SIZE]; /* Origin descriptor for RESERVE; empty for peer intent. */
    uint64_t revision;
    char parent_digest[LCS_CONFIG_DIGEST_SIZE];
    uint64_t ballot;
    uint32_t voters;
    char application_digest[LCS_CONFIG_DIGEST_SIZE]; /* Exact pending value for FINISH. */
    const lcs_config_value_t *value; /* borrowed until start returns */
    lcs_config_bootstrap_t *projection; /* Locked snapshot destination, borrowed until worker reaped. */
    const lcs_config_barrier_round_t *barrier_round; /* Same-host sealed readiness only. */
} lcs_config_agreement_job_t;

typedef struct
{
    pid_t pid;
    int pipe_fd;
    unsigned char *buffer;
    size_t used;
    bool eof;
    bool cancelled;
    bool checkpoint; /* Selects the explicit monotonic checkpoint import. */
    lcs_config_requests_t *requests; /* Borrowed, or private OPEN candidate until reaped. */
    lcs_config_requests_t *requests_target; /* Fresh borrowed OPEN destination. */
    lcs_config_agreement_t *request_agreement; /* Original handle required for poll/cancel. */
    lcs_config_application_t *application; /* Borrowed until completion/cancellation is reaped. */
    lcs_config_runtime_t *initialization_target; /* Borrowed until reaped. */
    lcs_config_runtime_t *initialization_candidate; /* Private unpublished pristine state. */
    lcs_config_runtime_t *barrier_target; /* Borrowed, unchanged until reaped. */
    lcs_config_barrier_t barrier_candidate; /* Unpublished until successful child exit. */
} lcs_config_agreement_worker_t;

void lcs_config_agreement_worker_init(lcs_config_agreement_worker_t *worker);
/* Same serialized writer as agreement/catalog jobs. Returns 1 while this worker
 * or an INI refresh is busy, -1 for preflight failure, 0 for started. Call ONLY
 * after coordinated authorization and while INI reload is excluded. This is a
 * low-level storage primitive; automatic upgrade uses the guarded round API below.
 * Capability advertisements do not authorize initialization. runtime must be fresh/empty.
 * Once started, bootstrap refresh stays locked, even on failure/cancellation;
 * retain all handles/storage until the child is reaped, then recover from disk.
 * poll/cancel use &runtime->agreement. Success publishes only pristine revision
 * zero plus an already-durable empty request journal, NOT activation authority
 * or permission to vote. New initialization uses format-2 intent; legacy intents,
 * lost prepared journals and nonempty reservations require normal recovery,
 * never a synthetic empty-history acknowledgement. No parent-side I/O. */
int lcs_config_agreement_worker_start_initialization(lcs_config_agreement_worker_t *worker,
                                                      lcs_config_runtime_t *runtime,
                                                      lcs_config_bootstrap_t *bootstrap,
                                                      const lcs_config_t *cfg,
                                                      bool allow_initialize,
                                                      char *err, size_t err_len);
/* Automatic first initialization must use this gate, not the low-level boolean
 * authorization primitive above. Revalidate exact effective definitions, reload
 * generation and all admitted upgrade-capable instances against an explicit
 * freeze round before forking. Busy/preflight failure never consumes the round;
 * successful fork consumes it and permanently locks local INI refresh. */
int lcs_config_agreement_worker_start_initialization_round(lcs_config_agreement_worker_t *worker,
    lcs_config_runtime_t *runtime, lcs_config_bootstrap_t *bootstrap, const lcs_config_t *cfg,
    lcs_config_initialization_round_t *round, const uint64_t instances[LCS_MAX_NODES],
    uint64_t generation, uint64_t now_ms, char *err, size_t err_len);
/* Persist explicit all-member readiness through the SAME writer. runtime must
 * retain pristine initialization; an existing durable barrier is recovered via
 * runtime_open, not overwritten. poll/cancel use &runtime->agreement. Successful
 * publication permits the separate configuration vote gate, not JSON authority.
 * All runtime handles remain unchanged until reaping; failures poison them. */
int lcs_config_agreement_worker_start_barrier(lcs_config_agreement_worker_t *worker,
                                              lcs_config_runtime_t *runtime,
                                              const lcs_config_barrier_round_t *round,
                                              char *err, size_t err_len);
/* Returns 0 for started, 1 for busy, -1 for failure before start. All journal
 * work occurs in the child; the parent publishes nothing until poll completes. */
int lcs_config_agreement_worker_start(lcs_config_agreement_worker_t *worker,
                                      lcs_config_agreement_t *agreement,
                                      const lcs_config_agreement_job_t *job,
                                      char *err, size_t err_len);
/* Shares the SAME worker with agreement jobs: only one catalog/journal writer
 * may run at once. BEGIN persists cleanup obligations; FINISH records caller-
 * completed backend reconciliation. Neither operation touches backends itself.
 * application and agreement must remain alive and unchanged until poll returns. */
int lcs_config_agreement_worker_start_application(lcs_config_agreement_worker_t *worker,
                                                  lcs_config_agreement_t *agreement,
                                                  lcs_config_application_t *application,
                                                  const lcs_config_agreement_job_t *job,
                                                  char *err, size_t err_len);
/* Explicit recover/open through the same writer. The destination must be fresh;
 * parent-side file I/O is forbidden. Child reopens and compares disk agreement
 * before recovering history. Initialization authorization is pristine-only and
 * must come from the coordinated upgrade path, never capability advertisements.
 * Missing history after promises/acceptances/learning is never initialized.
 * Pristine state alone is insufficient authorization: it cannot exclude lost
 * reservations before PREPARE. Recovery must not infer permission from absence.
 * Failure/cancellation poisons destination and agreement until disk recovery.
 * This worker is not automatically scheduled by the daemon and does not submit
 * proposals or confer outcome authority. */
int lcs_config_agreement_worker_open_requests(lcs_config_agreement_worker_t *worker,
                                               lcs_config_agreement_t *agreement,
                                               lcs_config_requests_t *requests,
                                               bool allow_initialize, char *err, size_t err_len);
/* Reserve/observe through the SAME serialized writer. History must already be
 * explicitly opened and bound to this agreement's store/static base. RESERVE
 * uses revision/parent_digest as expected base and value as requested successor.
 * OBSERVE records only the current learned majority decision. APPLY records an
 * exact completed-admission receipt using the borrowed admission catalog; it never
 * infers completion from catalog FINISH. Keep all handles/catalogs unchanged
 * until reaped. Success publishes only journal facts, not client success.
 * Uncertain persistence/cancellation poisons both handles; recover after reaping.
 * This API does not initialize missing history or submit agreement proposals. */
int lcs_config_agreement_worker_start_requests(lcs_config_agreement_worker_t *worker,
                                               lcs_config_agreement_t *agreement,
                                               lcs_config_requests_t *requests,
                                               const lcs_config_agreement_job_t *job,
                                               char *err, size_t err_len);
/* Publish exactly the current learned schema-2 document through the SAME worker.
 * The bootstrap state descriptors must match agreement's exclusively locked store;
 * INI refresh must be permanently excluded and its worker already reaped. Keep
 * both snapshot/state handles alive until poll/cancellation finishes. A completed
 * projection I/O failure does not poison the authoritative journal; interrupted
 * or malformed worker handoff retains the ordinary conservative recovery rule. */
int lcs_config_agreement_worker_start_projection(lcs_config_agreement_worker_t *worker,
                                                 lcs_config_agreement_t *agreement,
                                                 lcs_config_bootstrap_t *bootstrap,
                                                 char *err, size_t err_len);
/* Returns 0 for pending/idle, 1 for completion with operation_result, -1 for a
 * failed worker/uncertain persistence (agreement is poisoned in that case). */
int lcs_config_agreement_worker_poll(lcs_config_agreement_worker_t *worker,
                                     lcs_config_agreement_t *agreement,
                                     int *operation_result,
                                     char *err, size_t err_len);
void lcs_config_agreement_worker_cancel(lcs_config_agreement_worker_t *worker,
                                        lcs_config_agreement_t *agreement);
/* After cancellation/failure, poll until pid becomes zero before reopening the
 * journal. SIGKILL delivery alone is not evidence that old I/O has stopped. */

#endif
