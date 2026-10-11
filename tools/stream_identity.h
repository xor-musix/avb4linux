// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#ifndef STREAM_IDENTITY_H_
#define STREAM_IDENTITY_H_

#include <stdint.h>
#include "../kernel-module/snd-igb-avb/src/avb-config.h"

struct avb_stream_id_cfg {
    uint64_t local_mac;
    uint64_t peer_mac;
    uint64_t dest_mac;
    uint64_t stream_id;
    uint64_t peer_entity_id;
};

extern int g_num_streams;

extern struct avb_stream_id_cfg g_streams[MAX_STREAMS];

#endif
