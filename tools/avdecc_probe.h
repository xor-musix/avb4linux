// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#ifndef AVDECC_PROBE_H
#define AVDECC_PROBE_H

#include <stdint.h>
#include <stddef.h>

#define AVDECC_DIAG_MAX 64

enum avdecc_diag_kind { DIAG_RATE, DIAG_FORMAT, DIAG_MSRP, DIAG_CLOCK, DIAG_STUCK };

struct avdecc_diag {
   uint64_t eid;
   int      kind;
   int      severe;
   char     scope[64];
   char     cause[128];
};

int  avdecc_format_decode(uint64_t fmt, uint32_t *rate, int *channels);
void avdecc_format_str(uint64_t fmt, char *buf, size_t n);

void avdecc_probe_kick(uint64_t eid);
void avdecc_probe_kick_sid(const uint8_t sid[8]);
void avdecc_probe_note_connect_tx(uint64_t listener_eid, uint16_t listener_uid, uint16_t talker_uid);

void avdecc_probe_poll(int sock, uint64_t now_ms);
void avdecc_probe_handle_aem_response(int sock, const char *frame, int len);
void avdecc_probe_handle_acmp_response(int sock, const char *frame, int len);

int  avdecc_probe_anomalies(struct avdecc_diag *out, int max);

#endif
