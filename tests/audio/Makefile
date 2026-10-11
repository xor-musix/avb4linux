# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
# Build the audio loopback oracle (stimulus generator + WAV content checker).
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  ?= -lm

audio_check: audio_check.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f audio_check

.PHONY: clean
