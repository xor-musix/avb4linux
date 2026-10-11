/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
/*
 * Copyright (c) 2019, Ralf Beck
 * Copyright (c) 2026, mnemonic powerplay <mnemonic@xor-musix.de>
 */
#ifndef STREAM_ID_H_
#define STREAM_ID_H_

#if defined(__KERNEL__)

#include <linux/types.h>

#define STREAM_ID_LEN 8

#define AVB_STREAM_ID(mac, uid) (((u64)(mac) << 16) | ((uid) & 0xffff))

static inline u64 array6_to_u64(const u8 *val)
{
    u64 r = 0;
    for (unsigned i = 0; i < 6; i++) {
        r = (r << 8) | val[i];
    }
    return r;
}

static inline u64 array8_to_u64(const u8 *val)
{
    u64 r = 0;
    for (unsigned i = 0; i < 8; i++) {
        r = (r << 8) | val[i];
    }
    return r;
}

static inline void u64_to_array6(u64 src, u8 *dest)
{
    memset(dest, 0, 6);
    dest[0] = (src >> (8*5)) & 0xff;
    dest[1] = (src >> (8*4)) & 0xff;
    dest[2] = (src >> (8*3)) & 0xff;
    dest[3] = (src >> (8*2)) & 0xff;
    dest[4] = (src >> (8*1)) & 0xff;
    dest[5] = (src >> (8*0)) & 0xff;
}

static inline void u64_to_array8(u64 src, u8 *dest)
{
    memset(dest, 0, 8);
    dest[0] = (src >> (8*7)) & 0xff;
    dest[1] = (src >> (8*6)) & 0xff;
    dest[2] = (src >> (8*5)) & 0xff;
    dest[3] = (src >> (8*4)) & 0xff;
    dest[4] = (src >> (8*3)) & 0xff;
    dest[5] = (src >> (8*2)) & 0xff;
    dest[6] = (src >> (8*1)) & 0xff;
    dest[7] = (src >> (8*0)) & 0xff;
}

#else

#include <stdint.h>
#include <string.h>

#define STREAM_ID_LEN 8

#define AVB_STREAM_ID(mac, uid) (((uint64_t)(mac) << 16) | ((uid) & 0xffff))

static inline uint64_t array6_to_uint64(const char *val)
{
    uint64_t r = 0;
    for (unsigned i = 0; i < 6; i++) {
        r = (r << 8) | (uint8_t)val[i];
    }
    return r;
}

static inline void uint64_to_array6(uint64_t src, uint8_t *dest)
{
    memset(dest, 0, 6);
    dest[0] = (src >> (8*5)) & 0xff;
    dest[1] = (src >> (8*4)) & 0xff;
    dest[2] = (src >> (8*3)) & 0xff;
    dest[3] = (src >> (8*2)) & 0xff;
    dest[4] = (src >> (8*1)) & 0xff;
    dest[5] = (src >> (8*0)) & 0xff;
}

static inline void uint64_to_array8(uint64_t src, uint8_t *dest)
{
    memset(dest, 0, 8);
    dest[0] = (src >> (8*7)) & 0xff;
    dest[1] = (src >> (8*6)) & 0xff;
    dest[2] = (src >> (8*5)) & 0xff;
    dest[3] = (src >> (8*4)) & 0xff;
    dest[4] = (src >> (8*3)) & 0xff;
    dest[5] = (src >> (8*2)) & 0xff;
    dest[6] = (src >> (8*1)) & 0xff;
    dest[7] = (src >> (8*0)) & 0xff;
}

static inline uint64_t mac_to_entity_id(uint64_t mac)
{
    return ((mac & 0xffffff000000) << 16) | (0xfffe000000) | (mac & 0xffffff);
}

static inline uint64_t mac_to_controller_id(uint64_t mac)
{
    return ((mac & 0xffffff000000) << 16) | (0xfefe000000) | (mac & 0xffffff);
}

static inline uint64_t mac_to_grandmaster_id(uint64_t mac)
{
    return ((mac & 0xffffff000000) << 16) | (0xfffe000000) | (mac & 0xffffff);
}

static inline uint64_t mac_to_stream_id(uint64_t mac, uint64_t guid)
{
    return AVB_STREAM_ID(mac, guid);
}

#endif

#endif