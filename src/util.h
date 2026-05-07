// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_UTIL_H
#define LCS_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

char *lcs_trim(char *s);
int lcs_parse_u32(const char *s, uint32_t *out);
int lcs_parse_u16(const char *s, uint16_t *out);
int lcs_valid_name(const char *s);
int lcs_mkdir_parent(const char *path, mode_t mode);
uint64_t lcs_random_u64(void);
uint64_t lcs_now_ms(void);
void lcs_format_duration(uint64_t seconds, char *buf, size_t len);

#endif
