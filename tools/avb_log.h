// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#ifndef AVB_LOG_H
#define AVB_LOG_H

#include <stddef.h>
#include <stdint.h>
#include <syslog.h>

enum avb_layer { L_SYS, L_GPTP, L_MSRP, L_AVTP, L_AVDECC, L_FQTSS, L_AECP, L_DIAG };

void avb_log_init(void);
void avb_log_close(void);

void avb_log(enum avb_layer layer, int level, const char *fmt, ...)
     __attribute__((format(printf, 3, 4)));

void avb_log_last(char *buf, size_t buflen);

const char *avb_layer_tag(enum avb_layer layer);

struct avb_diag {
   int      on;
   uint64_t since_ms;
   char     body[320];
};

void avb_diag_update(struct avb_diag *d, int cond, uint64_t now_ms, uint64_t hold_ms,
                     const char *body);

#endif