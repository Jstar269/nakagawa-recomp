# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

# Nakagawa Font Copy (fontcopy.c): the PSP app that copies the console's own system fonts to its
# Memory Stick folder for the player's font import (docs/SETUP.md, "System fonts"). It needs the
# PSPSDK from pspdev (docs/PSPDEV_LOCAL_VERIFICATION.md) with psp-config on PATH:
#
#   make -C src/psp_companion
#
# builds build/EBOOT.PBP. Copy it to PSP/GAME/NAKAGAWAFONTS/EBOOT.PBP on the Memory Stick.

BUILD_DIR ?= build
TARGET = $(BUILD_DIR)/fontcopy
OBJS = $(BUILD_DIR)/fontcopy.o

$(shell mkdir -p "$(BUILD_DIR)")

INCDIR = ../../fixtures/psp_oracle
CFLAGS = -O2 -G0 -Wall -Wextra -Werror -std=gnu11
LIBS = -lpspdebug -lpspdisplay -lpspge -lpspctrl
BUILD_PRX = 1
PSP_FW_VERSION = 500
PSP_EBOOT = $(BUILD_DIR)/EBOOT.PBP
PSP_EBOOT_SFO = $(BUILD_DIR)/PARAM.SFO
PSP_EBOOT_TITLE = Nakagawa Font Copy
PSP_EBOOT_ICON = NULL
EXTRA_TARGETS = $(PSP_EBOOT)

PSPSDK = $(shell psp-config --pspsdk-path)
ifeq ($(strip $(PSPSDK)),)
$(error PSPDEV/PSPSDK is not available: psp-config --pspsdk-path failed)
endif
include $(PSPSDK)/lib/build.mak

# The link-time import gate (tools/psp_oracle/user_mode_imports.py) runs after
# psp-fixup-imports: user-mode libraries only, no registry call, and exactly these file calls:
# sceIoOpen, sceIoRead, sceIoWrite, sceIoClose, sceIoLseek, sceIoDopen, sceIoDread,
# sceIoDclose and sceIoMkdir, plus sceIoChdir and sceIoGetstat from the PSPSDK C library glue.
# No remove, rename, rmdir, ioctl or devctl call can be linked into the app.
PYTHON ?= python3
IMPORT_GATE = ../../tools/psp_oracle/user_mode_imports.py
IMPORT_GATE_ARGS = --forbid-library sceReg
IMPORT_GATE_ARGS += --exact-library IoFileMgrForUser=0x109F50BC,0x6A638D83,0x42EC03AC,0x810C4BC3,0x27EB27B8,0xB29DDF9C,0xE3EB004C,0xEB092469,0x06A70004,0x55F4717D,0xACE946E8
FIXUP = bash -c 'psp-fixup-imports "$$1" || exit $$?; exec "$(PYTHON)" "$(IMPORT_GATE)" $(IMPORT_GATE_ARGS) "$$1"' --

$(BUILD_DIR)/fontcopy.o: fontcopy.c fontcopy_config.h ../../fixtures/psp_oracle/font_files.h
	$(CC) $(CFLAGS) -c $< -o $@
