TARGET = nakagawa_minimal_v3
OBJS = main.o

INCDIR =
CFLAGS = -O2 -G0 -Wall -nostdlib
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBDIR =
LDFLAGS = -nostdlib
LIBS = -lpspmodinfo -lpspuser -lpspkernel

EXTRA_CLEAN = EBOOT_SIGNED.PBP

PSP_EBOOT_TITLE = Nakagawa Minimal Diag v3
PSP_EBOOT_ICON = NULL

BUILD_PRX = 1
PSP_FW_VERSION = 500

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

signed: EBOOT.PBP
	ebootsign EBOOT.PBP EBOOT_SIGNED.PBP
