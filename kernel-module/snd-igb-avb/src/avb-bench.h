/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de> */
/* avb-bench.h -- AVB RX throughput-measurement support.
 *
 * This is the kernel half of the throughput-test/ tool: it lets a single i210
 * be measured for "how many AVB listener streams can it sink with zero drops?"
 * by capturing ALL AVTP on RX-q1 (any MAC, no MSRP/MAC config) and counting the
 * received packet rate, while drops are read independently via ethtool.
 *
 * It is INERT in production: every entry point keys off the avb_rx_bench module
 * parameter (default 0). Load with `avb_rx_bench=1` to enter measurement mode.
 * The production audio path (narrow flex filter, live 8-ch relay) is untouched
 * when the parameter is 0 -- alsa.c carries only four small guarded calls into
 * the functions below, so the measurement code stays out of the data path.
 */

#ifndef AVB_BENCH_H
#define AVB_BENCH_H

#include "igb.h"

extern int avb_rx_bench;

void avb_bench_setup_filter(struct igb_adapter *adapter, u8 *filter_mask);

void avb_bench_count(const u8 *stream_id);

void avb_bench_init(struct igb_adapter *adapter);
void avb_bench_exit(void);

#endif