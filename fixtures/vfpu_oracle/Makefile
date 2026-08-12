# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
#
# Source-owned PSPDEV fixture for the VFPU transcendental hardware oracle
# (Loop A of docs/HARDWARE_ORACLE.md). Contains no game bytes, retail assets,
# firmware, keys, or captures. Build output is ignored; never commit a PRX/PBP.
TARGET = nakagawa_vfpu_oracle
OBJS = vfpu_probe.o
CFLAGS = -O2 -G0 -Wall -Wextra -std=c11
CXXFLAGS = $(CFLAGS)
ASFLAGS = $(CFLAGS)
BUILD_PRX = 1
PSP_FW_VERSION = 500
PSP_EBOOT_TITLE = Nakagawa VFPU Oracle
PSPSDK = $(shell psp-config --pspsdk-path)
ifeq ($(strip $(PSPSDK)),)
$(error PSPDEV/PSPSDK is not available: psp-config --pspsdk-path failed)
endif
include $(PSPSDK)/lib/build.mak
