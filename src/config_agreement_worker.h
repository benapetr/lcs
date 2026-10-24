// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef LCS_CONFIG_AGREEMENT_WORKER_H
#define LCS_CONFIG_AGREEMENT_WORKER_H

#include "config_agreement.h"
#include <sys/types.h>

typedef enum
{
    LCS_CONFIG_PREPARE = 1,
    LCS_CONFIG_ACCEPT = 2,
    LCS_CONFIG_LEARN = 3,
} lcs_config_agreement_action_t;

typedef struct
{
    lcs_config_agreement_action_t action;
    uint64_t revision;
    char parent_digest[LCS_CONFIG_DIGEST_SIZE];
    uint64_t ballot;
    uint32_t voters;
    const lcs_config_value_t *value; /* borrowed until start returns */
} lcs_config_agreement_job_t;

typedef struct
{
    pid_t pid;
    int pipe_fd;
    unsigned char *buffer;
    size_t used;
    bool eof;
    bool cancelled;
} lcs_config_agreement_worker_t;

void lcs_config_agreement_worker_init(lcs_config_agreement_worker_t *worker);
/* Returns 0 for started, 1 for busy, -1 for failure before start. All journal
 * work occurs in the child; the parent publishes nothing until poll completes. */
int lcs_config_agreement_worker_start(lcs_config_agreement_worker_t *worker,
                                      lcs_config_agreement_t *agreement,
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
