// SPDX-License-Identifier: GPL-3.0-or-later

#include "config_bootstrap.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    char directory[] = "/tmp/lcs-config-bootstrap.XXXXXX";
    assert(mkdtemp(directory));
    char path[512], state[512], snapshot[512], err[256];
    snprintf(path, sizeof(path), "%s/node.conf", directory);
    snprintf(state, sizeof(state), "%s/node.conf.state", directory);
    snprintf(snapshot, sizeof(snapshot), "%s/node.conf.resources.json", directory);
    FILE *file = fopen(path, "w");
    assert(file);
    assert(fputs("[cluster]\nname = test\nnode = node1\n"
                 "[node node1]\nrole = full-member\naddress = 127.0.0.1\n"
                 "[vip vip]\naddress = 192.0.2.10/24\ninterface = eth0\n", file) >= 0);
    assert(fclose(file) == 0);
    lcs_config_t cfg;
    assert(lcs_config_load(path, &cfg, err, sizeof(err)) == 0);
    lcs_config_bootstrap_t bootstrap, other;
    assert(lcs_config_bootstrap_open(&bootstrap, path, &cfg, err, sizeof(err)) == 0);
    assert(lcs_config_bootstrap_open(&other, path, &cfg, err, sizeof(err)) == -1);
    struct stat before, after;
    assert(stat(snapshot, &before) == 0);
    assert(lcs_config_bootstrap_refresh(&bootstrap, &cfg, err, sizeof(err)) == 0);
    assert(stat(snapshot, &after) == 0 && after.st_ino == before.st_ino);
    snprintf(cfg.resources[0].address, sizeof(cfg.resources[0].address), "192.0.2.20/24");
    assert(lcs_config_bootstrap_refresh(&bootstrap, &cfg, err, sizeof(err)) == 0);
    char *text = NULL;
    size_t length;
    assert(lcs_config_store_read(&bootstrap.snapshots, bootstrap.snapshot_name,
        &text, &length, LCS_RESOURCE_DOCUMENT_MAX, err, sizeof(err)) == 0);
    assert(strstr(text, "192.0.2.20/24"));
    free(text);
    snprintf(cfg.resources[0].address, sizeof(cfg.resources[0].address), "192.0.2.30/24");
    lcs_config_bootstrap_request(&bootstrap);
    assert(!bootstrap.prepared);
    assert(lcs_config_bootstrap_process(&bootstrap, &cfg, err, sizeof(err)) == 0);
    assert(bootstrap.worker_pid > 0);
    /* An edit arriving while the first worker runs schedules the latest view. */
    snprintf(cfg.resources[0].address, sizeof(cfg.resources[0].address), "192.0.2.40/24");
    lcs_config_bootstrap_request(&bootstrap);
    for (int attempts = 0; !bootstrap.prepared && attempts < 500; attempts++)
    {
        assert(lcs_config_bootstrap_process(&bootstrap, &cfg, err, sizeof(err)) >= 0);
        usleep(10000);
    }
    assert(bootstrap.prepared);
    assert(lcs_config_store_read(&bootstrap.snapshots, bootstrap.snapshot_name,
        &text, &length, LCS_RESOURCE_DOCUMENT_MAX, err, sizeof(err)) == 0);
    assert(strstr(text, "192.0.2.40/24"));
    free(text);
    lcs_config_bootstrap_close(&bootstrap);
    assert(lcs_config_bootstrap_open(&bootstrap, path, &cfg, err, sizeof(err)) == 0);

    /* Prepared metadata prevents deleted snapshots from resurrecting INI. */
    assert(unlink(snapshot) == 0);
    assert(lcs_config_bootstrap_refresh(&bootstrap, &cfg, err, sizeof(err)) == -1);
    assert(strstr(err, "recover missing state"));
    const char *preparing = "LCS-RESOURCE-BOOTSTRAP-1\ntest\npreparing\n";
    assert(lcs_config_store_write(&bootstrap.state, "bootstrap.state", preparing,
        strlen(preparing), err, sizeof(err)) == 0);
    /* Interrupted first initialization can resume automatically. */
    assert(lcs_config_bootstrap_refresh(&bootstrap, &cfg, err, sizeof(err)) == 0);
    assert(lcs_config_store_write(&bootstrap.state, "bootstrap.state", "active", 6, err, sizeof(err)) == 0);
    assert(lcs_config_bootstrap_refresh(&bootstrap, &cfg, err, sizeof(err)) == -1);
    assert(strstr(err, "unknown authority"));
    assert(lcs_config_store_write(&bootstrap.state, "bootstrap.state", preparing,
        strlen(preparing), err, sizeof(err)) == 0);
    assert(lcs_config_bootstrap_refresh(&bootstrap, &cfg, err, sizeof(err)) == 0);

    /* A future committed revision must never be overwritten from INI. */
    char *advanced;
    assert(lcs_resource_config_encode(&cfg, 1, &advanced, &length, err, sizeof(err)) == 0);
    assert(lcs_config_store_write(&bootstrap.snapshots, bootstrap.snapshot_name,
        advanced, length, err, sizeof(err)) == 0);
    free(advanced);
    assert(lcs_config_bootstrap_refresh(&bootstrap, &cfg, err, sizeof(err)) == -1);
    assert(strstr(err, "non-bootstrap revision"));
    assert(lcs_config_store_write(&bootstrap.snapshots, bootstrap.snapshot_name,
        "{broken", 7, err, sizeof(err)) == 0);
    assert(lcs_config_bootstrap_refresh(&bootstrap, &cfg, err, sizeof(err)) == -1);
    assert(strstr(err, "JSON"));
    assert(unlinkat(bootstrap.state.directory_fd, "bootstrap.state", 0) == 0);
    assert(lcs_config_bootstrap_refresh(&bootstrap, &cfg, err, sizeof(err)) == -1);
    assert(strstr(err, "disagree"));
    assert(unlink(snapshot) == 0);
    assert(unlinkat(bootstrap.state.directory_fd, "lock", 0) == 0);
    lcs_config_bootstrap_close(&bootstrap);
    assert(unlink(path) == 0);
    assert(rmdir(state) == 0);
    assert(rmdir(directory) == 0);
    puts("automatic resource bootstrap unit tests passed");
    return 0;
}
