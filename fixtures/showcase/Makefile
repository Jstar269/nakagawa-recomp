TARGET = showcase_scene_app
OBJS = main.o

INCDIR =
CFLAGS = -O2 -G0 -Wall -Wextra
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBDIR =
LDFLAGS =
SHOWCASE_LIBS = -lpspgu -lpspgum -lpspaudio -lpsputility -lpspuser -lpspkernel -lm

BUILD_PRX = 1
PSP_FW_VERSION = 660
PSPSDK = $(shell psp-config --pspsdk-path)

include $(PSPSDK)/lib/build.mak

LIBS += $(SHOWCASE_LIBS)
