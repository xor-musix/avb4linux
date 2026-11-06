#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
# Connects to the given $MOTU_DEVICE via `telnet`. 
# The $MOTU_DEVICE must be the ip address or hostname of the MOTU device.
# Use `root` without password to login!

set -e
MOTU_DEVICE=${MOTU_DEVICE:-ulavb}

curl -X POST -v http:/$MOTU_DEVICE/start_telnet  

telnet $MOTU_DEVICE
