/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2019, Ralf Beck
 * Copyright (c) 2026, mnemonic powerplay <mnemonic@xor-musix.de>
 */
#define MAX_STREAMS 32
#define CHANNELS_PER_STREAM 8
#define MAX_PCM_CHANNELS (MAX_STREAMS * CHANNELS_PER_STREAM)

#define MAAP_LOCALLY_ADMINISTRATORED_BASE 0x91e0f000fe00ul

#define ALIGN_MAX_FRAMES 96
