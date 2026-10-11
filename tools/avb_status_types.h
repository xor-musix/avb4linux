// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#ifndef AVB_STATUS_TYPES_H
#define AVB_STATUS_TYPES_H

enum talker_state { TK_IDLE, TK_ADVERTISING, TK_READY, TK_FAILED };
enum listener_rsv { LS_IDLE, LS_ASKING, LS_READY, LS_ASKFAILED, LS_FAILED };

#endif