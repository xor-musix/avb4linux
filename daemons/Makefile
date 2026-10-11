# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
# Build the OpenAvnu daemons with patch overlay.
#
# This directory holds the delta against the pristine OpenAvnu daemons
# (patches/). The upstream sources live in the daemons/OpenAvnu/daemons/
# submodule and are never edited in place. The build stages a fresh
# copy of upstream into build/, applies patches/, and builds everything
# from there.
#
# This follows the same pattern as kernel-module/snd-igb-avb/Makefile.

HERE    := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
UPSTREAM := $(HERE)/OpenAvnu/daemons
PATCHES := $(HERE)/patches
BUILD   := $(HERE)/OpenAvnu/build

.PHONY: all mrpd maap shaper mrpd_clean maap_clean shaper_clean warning-check clean stage

all: mrpd maap shaper

stage:
	@test -f "$(UPSTREAM)/mrpd/mrpd.c" || { \
	  echo "ERROR: $(UPSTREAM) is empty - run: git submodule update --init --recursive"; exit 1; }
	@test -f "$(UPSTREAM)/shaper/src/shaper_daemon.c" || { \
	  echo "ERROR: $(UPSTREAM)/shaper/src/shaper_daemon.c is missing - submodule not fully initialized"; exit 1; }
	rm -rf $(BUILD)/daemons/mrpd $(BUILD)/daemons/common $(BUILD)/daemons/examples/mrp_client $(BUILD)/daemons/shaper
	mkdir -p $(BUILD)/daemons/mrpd
	mkdir -p $(BUILD)/daemons/common
	mkdir -p $(BUILD)/daemons/examples/mrp_client
	mkdir -p $(BUILD)/daemons/shaper/src
	mkdir -p $(BUILD)/daemons/shaper/build
	# Copy pristine upstream sources (following kernel-module pattern)
	# mrpd
	cp $(UPSTREAM)/mrpd/*.c $(UPSTREAM)/mrpd/*.h $(UPSTREAM)/mrpd/Makefile $(BUILD)/daemons/mrpd/
	cp $(UPSTREAM)/common/*.c $(UPSTREAM)/common/*.h $(BUILD)/daemons/common/
	cp $(UPSTREAM)/../examples/mrp_client/*.c $(UPSTREAM)/../examples/mrp_client/*.h $(BUILD)/daemons/examples/mrp_client/
	# shaper
	cp $(UPSTREAM)/shaper/src/*.c $(UPSTREAM)/shaper/src/*.h $(BUILD)/daemons/shaper/src/
	cp $(UPSTREAM)/shaper/Makefile $(BUILD)/daemons/shaper/
	@set -e; cd $(BUILD); for p in $(PATCHES)/*.patch; do \
	  echo "applying $$(basename $$p)"; patch -p1 < "$$p"; done

mrpd: stage
	$(MAKE) -C $(BUILD)/daemons/mrpd mrpd

mrpd_clean:
	rm -rf $(BUILD)/daemons/mrpd

maap:
	$(MAKE) -C $(UPSTREAM)/maap/linux/build

maap_clean:
	$(MAKE) -C $(UPSTREAM)/maap/linux/build clean

shaper: stage
	$(MAKE) -C $(BUILD)/daemons/shaper

shaper_clean:
	rm -rf $(BUILD)/daemons/shaper

# Mirror kernel-module/snd-igb-avb: the baseline script owns the whole build.
# It runs a single `make all` (stage runs exactly once, so no sibling daemon's
# output is clobbered by a re-stage) and gates the captured warnings.
warning-check:
	@$(HERE)/check-warning-baseline.sh

clean:
	rm -rf $(BUILD)

install:
	mkdir -p $(AVBHOME)
	cp $(BUILD)/daemons/mrpd/mrpd $(AVBHOME)
	cp $(UPSTREAM)/maap/linux/build/maap_daemon $(AVBHOME)
	cp $(BUILD)/daemons/shaper/shaper_daemon $(AVBHOME)
