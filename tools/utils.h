// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <string.h>

#include "../include/stream_id.h"

static inline uint64_t array8_to_uint64(const uint8_t *val)
{
    uint64_t ret = 0;
    for (unsigned i = 0; i < 8; i++)
        ret = (ret << 8) | val[i];
    return ret;
}

uint64_t parse_mac_u64(const char *s, uint64_t fallback);
uint64_t array6_to_uint64(const char *val);
uint64_t get_time_in_ms(void);

#endif