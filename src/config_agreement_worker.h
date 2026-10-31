// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef LCS_CONFIG_AGREEMENT_WORKER_H
#define LCS_CONFIG_AGREEMENT_WORKER_H

#include "config_application.h"
#include "config_bootstrap.h"
#include "config_runtime.h"
#include <sys/types.h>

typedef enum
{
    LCS_CONFIG_PREPARE = 1,
    LCS_CONFIG_ACCEPT = 2,
    LCS_CONFIG_LEARN = 3,
    LCS_CONFIG_APPLICATION_BEGIN = 4,
    LCS_CONFIG_APPLICATION_FINISH = 5,
    LCS_CONFIG_BARRIER = 6,
} lcs_config_agreement_action_t;

typedef struct
{
    lcs_config_agreement_action_t action;
    uint64_t revision;
    char parent_digest[LCS_CONFIG_DIGEST_SIZE];
    uint64_t ballot;
    uint32_t voters;
    char application_digest[LCS_CONFIG_DIGEST_SIZE]; /* Exact pending value for FINISH. */
    const lcs_config_value_t *value; /* borrowed until start returns */
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
    lcs_config_application_t *application; /* Borrowed until completion/cancellation is reaped. */
    lcs_config_runtime_t *initialization_target; /* Borrowed until reaped. */
    lcs_config_runtime_t *initialization_candidate; /* Private unpublished pristine state. */
    lcs_config_runtime_t *barrier_target; /* Borrowed, unchanged until reaped. */
    lcs_config_barrier_t barrier_candidate; /* Unpublished until successful child exit. */
} lcs_config_agreement_worker_t;

void lcs_config_agreement_worker_init(lcs_config_agreement_worker_t *worker);
/* Same serialized writer as agreement/catalog jobs. Returns 1 while this worker
 * or an INI refresh is busy, -1 for preflight failure, 0 for started. Call ONLY
 * after coordinated authorization and while INI reload is excluded. Capability
 * advertisements do not authorize initialization. runtime must be fresh/empty.
 * Once started, bootstrap refresh stays locked, even on failure/cancellation;
 * retain all handles/storage until the child is reaped, then recover from disk.
 * poll/cancel use &runtime->agreement. Success publishes only pristine revision
 * zero, NOT activation authority or permission to vote. No parent-side I/O. */
int lcs_config_agreement_worker_start_initialization(lcs_config_agreement_worker_t *worker,
                                                      lcs_config_runtime_t *runtime,
                                                      lcs_config_bootstrap_t *bootstrap,
                                                      const lcs_config_t *cfg,
                                                      bool allow_initialize,
                                                      char *err, size_t err_len);
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
