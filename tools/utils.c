// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

uint64_t parse_mac_u64(const char *s, uint64_t fallback)
{
    const char *orig = s;
    uint64_t v = 0;
    int n = 0;

    if (!s || !*s)
        return fallback;

    for (; *s; s++) {
        int d;

        if (*s == ':' || *s == '-')
            continue;
        if (*s >= '0' && *s <= '9')
            d = *s - '0';
        else if (*s >= 'a' && *s <= 'f')
            d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F')
            d = *s - 'A' + 10;
        else {
            n = -1;
            break;
        }
        v = (v << 4) | d;
        n++;
    }

    if (n != 12) {
        fprintf(stderr, "invalid MAC \"%s\", using default\n", orig);
        return fallback;
    }

    return v;
}

uint64_t get_time_in_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}