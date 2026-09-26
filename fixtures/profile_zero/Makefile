# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

TARGET = profile_zero_guest
OBJS = main.o

INCDIR =
CFLAGS = -O2 -G0 -Wall -Wextra
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBDIR =
LDFLAGS =
LIBS = -lpspgu -lpspgum -lpspaudio -lpspdisplay -lpspctrl -lpspuser -lpspkernel -lm

PSP_EBOOT_TITLE = Nakagawa Profile Zero Fixture
PSP_EBOOT_ICON = NULL

BUILD_PRX = 1
PSP_FW_VERSION = 660
PSPSDK = $(shell psp-config --pspsdk-path)

include $(PSPSDK)/lib/build.mak
