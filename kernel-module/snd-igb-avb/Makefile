# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
# Build the single "snd_igb_avb" kernel module.
#
# This directory holds ONLY the AVB-audio additions (src/) and the delta against
# the pristine Intel/Avnu igb_avb driver (patches/). The upstream sources live in
# the ../igb_avb submodule and are never edited in place. The build stages a fresh
# copy of upstream into build/, overlays src/ + patches/, and compiles everything
# into one module: build/snd_igb_avb.ko
#
# Targets: modules (default), clean, install

KDIR    ?= /lib/modules/$(shell uname -r)/build
HERE    := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
UPSTREAM := $(HERE)/../igb_avb/kmod
SRC     := $(HERE)/src
PATCHES := $(HERE)/patches
BUILD   := $(HERE)/build

MODULE  := snd_igb_avb

.PHONY: modules warning-check stage clean install

# ccflags-y is the kernel build system way to add compiler flags
# Users can pass: make EXTRA_CFLAGS="-DDEBUG_TICK_COST"
# Or directly: make ccflags-y="-DDEBUG_TICK_COST"

modules: stage
	$(MAKE) -C $(KDIR) M=$(BUILD) modules ccflags-y="$(ccflags-y) $(EXTRA_CFLAGS)"
	@echo "** built $(BUILD)/$(MODULE).ko"

warning-check:
	@$(HERE)/check-warning-baseline.sh

# Reconstruct a buildable tree from pristine upstream + our overlay.
stage:
	@test -f $(UPSTREAM)/igb_main.c || { \
	  echo "ERROR: $(UPSTREAM) is empty - run: git submodule update --init"; exit 1; }
	rm -rf $(BUILD)
	mkdir -p $(BUILD)
	cp $(UPSTREAM)/*.c $(UPSTREAM)/*.h $(UPSTREAM)/Makefile $(BUILD)/
	cp $(SRC)/*.c $(SRC)/*.h $(BUILD)/
	# Copy shared headers from include/ to build/
	cp $(HERE)/../../include/stream_id.h $(BUILD)/
	@set -e; cd $(BUILD); for p in $(PATCHES)/*.patch; do \
	  echo "applying $$(basename $$p)"; patch -p1 < $$p; done
	# Emit one module named "$(MODULE)" while keeping upstream internals untouched.
	sed -i 's/^DRIVER_NAME=igb_avb/DRIVER_NAME=$(MODULE)/' $(BUILD)/Makefile

clean:
	rm -rf $(BUILD)

install: modules
	$(MAKE) -C $(KDIR) M=$(BUILD) modules_install
