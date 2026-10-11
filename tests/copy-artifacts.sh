#! /bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>

sudo chown "$(id -un):$(id -gn)" ~/tmp/all.pcap
mv ~/tmp/all.pcap ~/tmp/baseline-all-latest.pcap

sudo chown "$(id -un):$(id -gn)" ~/tmp/tap-all.pcap
mv ~/tmp/tap-all.pcap ~/tmp/baseline-tap-all-latest.pcap

sudo chown "$(id -un):$(id -gn)" ~/tmp/avb-loopback.wav
mv ~/tmp/avb-loopback.wav ~/tmp/baseline-avb-loopback-latest.wav

sudo chown "$(id -un):$(id -gn)" ~/tmp/avb-loopback.metrics.txt
mv ~/tmp/avb-loopback.metrics.txt ~/tmp/baseline-avb-loopback-latest.metrics.txt