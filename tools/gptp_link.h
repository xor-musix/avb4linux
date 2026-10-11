// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#ifndef GPTP_LINK_H
#define GPTP_LINK_H

#include <stddef.h>
#include <stdint.h>

int  gptp_link_init(int ifindex);
void gptp_link_cleanup(void);
void gptp_link_poll(uint64_t now_ms);
int  gptp_link_carrier(void);

int  gptp_link_text(char *buf, size_t n);

#endif
