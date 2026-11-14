// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_projection.h"
#include <stdio.h>
#include <string.h>

void lcs_config_projection_init(lcs_config_projection_t *projection,
                                  lcs_config_coordinator_t *coordinator, lcs_config_bootstrap_t *bootstrap)
{
    memset(projection, 0, sizeof(*projection));
    projection->coordinator = coordinator;
    projection->bootstrap = bootstrap;
}

int lcs_config_projection_step(void *context, const lcs_config_value_t *committed, char *err, size_t err_len)
{
    lcs_config_projection_t *projection = context;
    lcs_config_coordinator_t *coordinator = projection->coordinator;
    if (!coordinator || !coordinator->initialized || coordinator->closing || coordinator->failed || !committed ||
        !lcs_config_value_equal(committed, &coordinator->runtime->agreement.committed))
    {
        if (err && err_len)
            snprintf(err, err_len, "projection requires the current learned value and healthy coordinator");
        return -1;
    }
    if (projection->completion)
    {
        if (coordinator->completed_projections < projection->completion)
            return 0;
        bool failed = coordinator->completed_projections != projection->completion || coordinator->last_projection_result;
        projection->completion = 0;
        if (failed)
        {
            if (err && err_len)
                snprintf(err, err_len, "projection write failed or completion changed: %s", coordinator->error);
            return -1;
        }
    }
    if (coordinator->phase != LCS_CONFIG_COORD_READY || coordinator->storage.pid > 0)
        return 0;
    if (coordinator->projected_revision == committed->revision &&
        !strcmp(coordinator->projected_digest, committed->digest))
        return 1;
    int result = lcs_config_coordinator_start_projection(coordinator, projection->bootstrap, err, err_len);
    if (result < 0)
        return -1;
    if (!result)
        projection->completion = coordinator->completed_projections + 1;
    return 0;
}
