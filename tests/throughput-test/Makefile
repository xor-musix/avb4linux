# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
# Build the AVTP blaster (runs on the load-source box / second NIC).
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra

avtp-blaster: avtp-blaster.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f avtp-blaster

.PHONY: clean
