// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

char *lcs_trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    if (*s == '\0')
        return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        *end-- = '\0';
    return s;
}

int lcs_parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno || end == s || *lcs_trim(end) != '\0' || v > UINT32_MAX)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

int lcs_parse_u16(const char *s, uint16_t *out)
{
    uint32_t v = 0;
    if (lcs_parse_u32(s, &v) != 0 || v > UINT16_MAX)
        return -1;
    *out = (uint16_t)v;
    return 0;
}

int lcs_valid_name(const char *s)
{
    if (!s || !*s)
        return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    {
        if (!(isalnum(*p) || *p == '_' || *p == '-' || *p == '.'))
            return 0;
    }
    return 1;
}

int lcs_mkdir_parent(const char *path, mode_t mode)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp)
        return 0;
    *slash = '\0';
    if (mkdir(tmp, mode) == 0 || errno == EEXIST)
        return 0;
    return -1;
}

uint64_t lcs_random_u64(void)
{
    uint64_t value = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0)
    {
        ssize_t n = read(fd, &value, sizeof(value));
        close(fd);
        if (n == (ssize_t)sizeof(value) && value != 0)
            return value;
    }
    return ((uint64_t)getpid() << 32) ^ (uint64_t)time(NULL);
}

uint64_t lcs_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return ((uint64_t)ts.tv_sec * 1000ull) + ((uint64_t)ts.tv_nsec / 1000000ull);
}

void lcs_format_duration(uint64_t seconds, char *buf, size_t len)
{
    uint64_t days = seconds / 86400u;
    seconds %= 86400u;
    uint64_t hours = seconds / 3600u;
    seconds %= 3600u;
    uint64_t minutes = seconds / 60u;
    seconds %= 60u;

    if (len == 0)
        return;

    if (days)
    {
        snprintf(buf, len, "%llud %02lluh %02llum %02llus",
                 (unsigned long long)days, (unsigned long long)hours,
                 (unsigned long long)minutes, (unsigned long long)seconds);
    } else if (hours)
    {
        snprintf(buf, len, "%lluh %02llum %02llus",
                 (unsigned long long)hours, (unsigned long long)minutes,
                 (unsigned long long)seconds);
    } else if (minutes)
    {
        snprintf(buf, len, "%llum %02llus", (unsigned long long)minutes, (unsigned long long)seconds);
    } else
    {
        snprintf(buf, len, "%llus", (unsigned long long)seconds);
    }
}
