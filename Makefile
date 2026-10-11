# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2019 Ralf Beck
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
# Top-level build for avb4linux.
#
# All upstream code lives in git submodules whose sources are never edited:
# the kernel module, gptp and shaper are built from staged copies with this
# repo's patch overlay applied (kernel-module/snd-igb-avb/build,
# daemons/gptp-build, daemons/OpenAvnu/build).
#
#   kernel-module/igb_avb      submodule: pristine Intel/Avnu igb_avb driver
#   kernel-module/snd-igb-avb  overlay:   patches + ALSA/AVTP -> snd_igb_avb.ko
#   daemons/OpenAvnu           submodule: mrpd, maap, shaper
#   daemons/gptp               submodule: gPTP daemon (daemon_cl) + patch overlay
#   jdksavdecc-c               submodule: AVDECC library (built on demand for avb-user)

OPENAVNU := daemons/OpenAvnu/daemons
# gptp is built from a staged, patched copy of the daemons/gptp submodule
# (daemons/check-gptp-baseline.sh stages into gptp-build/ and applies
# daemons/gptp-patches/); the submodule itself is never edited in place.
GPTP     := daemons/gptp-build/linux/build
KMOD     := kernel-module/snd-igb-avb
AVBHOME  ?= $(HOME)/.avb
JDKSAVDECC := jdksavdecc-c/libjdksavdecc-c.a

.PHONY: all daemons mrpd maap shaper gptp snd_igb_avb avb-user jdksavdecc clean install help

all: snd_igb_avb daemons gptp avb-user

help:
	@echo 'Targets: all, snd_igb_avb, daemons (mrpd maap shaper gptp), avb-user, jdksavdecc, clean, install'

snd_igb_avb:
	$(MAKE) -C $(KMOD) warning-check

daemons:
	$(MAKE) -C daemons warning-check
gptp:
	@daemons/check-gptp-baseline.sh

# CMake >= 4 rejects jdksavdecc-c's old cmake_minimum_required without the
# policy floor; 3.10 also avoids the "< 3.10" deprecation warning.  Built only
# when the library is missing.
jdksavdecc: $(JDKSAVDECC)
$(JDKSAVDECC):
	cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.10 -S jdksavdecc-c -B jdksavdecc-c
	$(MAKE) -C jdksavdecc-c jdksavdecc-c

avb-user: $(JDKSAVDECC)
	$(MAKE) -C tools

clean:
	$(MAKE) -C $(KMOD) clean
	$(MAKE) -C daemons clean
	rm -rf daemons/gptp-build
	$(MAKE) -C tools clean

install:
	mkdir -p $(AVBHOME)
	cp $(KMOD)/build/snd_igb_avb.ko $(AVBHOME)
	cp daemons/OpenAvnu/build/daemons/mrpd/mrpd $(AVBHOME)
	cp $(GPTP)/obj/daemon_cl $(AVBHOME)
	cp $(OPENAVNU)/maap/linux/build/maap_daemon $(AVBHOME)
	cp daemons/OpenAvnu/build/daemons/shaper/shaper_daemon $(AVBHOME)
	cp tools/avb-user $(AVBHOME)
# per-machine tuning hook: seeded once, never overwritten (user edits it)
	[ -e $(AVBHOME)/avb-tuning.sh ] || cp avb_tuning.example.sh $(AVBHOME)/avb-tuning.sh
	cp avb_up.sh avb_down.sh utils/avb_nm_unmanage.sh /usr/local/bin
