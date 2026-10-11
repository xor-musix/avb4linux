# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
# Build the 256-channel audio generator/checker for 32 talkers scale test.
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  ?= -lm

all: audio_check_256ch

audio_check_256ch: audio_check_256ch.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f audio_check_256ch

.PHONY: all clean