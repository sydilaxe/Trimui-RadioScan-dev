# RadioScan build — TrimUI Brick, Brick Hammer and Brick Pro (TrimUI OS and MustardOS)
#   make                 cross-compile for the TrimUI with zig (aarch64, glibc 2.28, against the device's SDL2)
#                        -> build/trimui/radioscan
#   make desktop         a build for this computer (needs the SDL2 dev files) -> build/desktop/radioscan
#   make package         the TrimUI build + build/RadioScan-trimui.zip, to unzip at the root of the SD card
#                        (it holds Apps/RadioScan), and build/RadioScan-mustardos.muxapp for MustardOS
#                        (the same binary)
#   make install SD=/path/to/card [OS=mustardos]   copy the app onto a mounted TrimUI card
#   make version         print the version: RS_VERSION in src/update.h, or VERSION=x.y.z when given
#                        (CI passes the release tag's version, so a release is built as v<tag>)
#
# zig: ZIG=/path/to/zig, or `pip install ziglang==0.14.1` (used as `python3 -m ziglang`)

ZIG      ?= $(shell command -v zig 2>/dev/null || echo python3 -m ziglang)
ZTARGET  := aarch64-linux-gnu.2.28
CPU      := cortex_a53
APP      := radioscan
APPDIR   := Apps/RadioScan
SDL_INC  := third_party/SDL2-2.30.9-headers
SRC      := src/main.c src/perflog.c src/dsp.c src/stream.c src/net.c src/stations.c src/sync.c src/aclock.c src/update.c
HDR      := $(wildcard src/*.h)
MBED_DIR := third_party/mbedtls
MBED_SRC := $(wildcard $(MBED_DIR)/library/*.c)
VERSION  := $(shell sed -n 's/^\#define RS_VERSION "\([^"]*\)".*/\1/p' src/update.h)

WARN     := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CFLAGS   ?= -O2
CFLAGS   += -std=gnu11 $(WARN) -I$(MBED_DIR)/include -DRS_VERSION='"$(VERSION)"'
MBED_CFLAGS := -O2 -I$(MBED_DIR)/include -I$(MBED_DIR)/library

# --- the TrimUI build
DEV_CC   = $(ZIG) cc -target $(ZTARGET) -mcpu=$(CPU)
DEV_OBJ  := $(patsubst src/%.c,build/trimui/%.o,$(SRC))
DEV_MBED := $(patsubst $(MBED_DIR)/library/%.c,build/trimui/mbedtls/%.o,$(MBED_SRC))

all: build/trimui/$(APP)

build/trimui/$(APP): $(DEV_OBJ) build/trimui/libmbedtls.a build/sdl2/libSDL2-2.0.so.0
	$(DEV_CC) -o $@ $(DEV_OBJ) build/trimui/libmbedtls.a build/sdl2/libSDL2-2.0.so.0 -lm -lpthread \
		-Wl,--allow-shlib-undefined -s

build/trimui/%.o: src/%.c $(HDR) | build/trimui
	$(DEV_CC) $(CFLAGS) -DRS_DEVICE -I$(SDL_INC) -c -o $@ $<

build/trimui/libmbedtls.a: $(DEV_MBED)
	$(ZIG) ar rcs $@ $^

build/trimui/mbedtls/%.o: $(MBED_DIR)/library/%.c | build/trimui/mbedtls
	$(DEV_CC) $(MBED_CFLAGS) -c -o $@ $<

# a stand-in for the device's libSDL2-2.0.so.0, to link against (TrimUI OS has the real one)
build/sdl2/libSDL2-2.0.so.0: tools/mksdlstub.py src/main.c
	@mkdir -p build/sdl2
	python3 tools/mksdlstub.py build/sdl2/sdl2_stub.c src/main.c
	$(ZIG) cc -target $(ZTARGET) -shared -Wl,-soname,libSDL2-2.0.so.0 -o $@ build/sdl2/sdl2_stub.c

# --- a desktop build, for trying it and for the tests
HOST_CC  ?= cc
HOST_OBJ := $(patsubst src/%.c,build/desktop/%.o,$(SRC))
HOST_MBED := $(patsubst $(MBED_DIR)/library/%.c,build/desktop/mbedtls/%.o,$(MBED_SRC))
# GCC 15 stopped zeroing a whole union for `= {0}`, which Mbed TLS 3.6.2 relies on (TLS 1.3 fails without this)
HOST_MBED_CFLAGS := $(MBED_CFLAGS) $(shell $(HOST_CC) -fzero-init-padding-bits=unions -E -x c /dev/null >/dev/null 2>&1 && echo -fzero-init-padding-bits=unions)

desktop: build/desktop/$(APP)

build/desktop/$(APP): $(HOST_OBJ) build/desktop/libmbedtls.a
	$(HOST_CC) -o $@ $(HOST_OBJ) build/desktop/libmbedtls.a $(shell sdl2-config --libs) -lpthread -lm

build/desktop/%.o: src/%.c $(HDR) | build/desktop
	$(HOST_CC) $(CFLAGS) $(shell sdl2-config --cflags | sed 's|-I\([^ ]*\)/SDL2|-I\1|') -c -o $@ $<

build/desktop/libmbedtls.a: $(HOST_MBED)
	ar rcs $@ $^

build/desktop/mbedtls/%.o: $(MBED_DIR)/library/%.c | build/desktop/mbedtls
	$(HOST_CC) $(HOST_MBED_CFLAGS) -c -o $@ $<

build/trimui build/trimui/mbedtls build/desktop build/desktop/mbedtls:
	mkdir -p $@

# --- the SD card package: Apps/RadioScan, as TrimUI OS lists apps
package: build/RadioScan-trimui.zip build/RadioScan-mustardos.muxapp

build/RadioScan-trimui.zip: build/trimui/$(APP) $(shell find $(APPDIR) -type f)
	rm -rf build/sd $@
	mkdir -p build/sd/Apps
	cp -r $(APPDIR) build/sd/Apps/
	rm -f build/sd/$(APPDIR)/settings.cfg build/sd/$(APPDIR)/log.txt build/sd/$(APPDIR)/sysinfo.txt build/sd/$(APPDIR)/$(APP)
	cp build/trimui/$(APP) build/sd/$(APPDIR)/$(APP)
	cp $(MBED_DIR)/LICENSE build/sd/$(APPDIR)/LICENSE-mbedtls.txt
	echo $(VERSION) > build/sd/$(APPDIR)/VERSION # read by install.sh to tell whether the TrimUI is up to date
	chmod +x build/sd/$(APPDIR)/launch.sh build/sd/$(APPDIR)/sysinfo.sh build/sd/$(APPDIR)/$(APP)
	cd build/sd && zip -qr ../RadioScan-trimui.zip Apps
	@echo "$@ ready: unzip it at the root of the TrimUI's SD card"

# MustardOS: a .muxapp is a zip that its Archive Manager unpacks into the application folder (MUOS/application on
# the card), and the folder's name is the name in the Applications list. The same app folder and binary, plus
# mux_launch.sh (how MustardOS starts an app, and how the app knows to update from this package), wake_screen.sh
# (the Brick Pro's screen can stay dark otherwise) and the list glyph. Keep mux_launch.sh out of the TrimUI OS zip.
MUX := mustardos
build/RadioScan-mustardos.muxapp: build/RadioScan-trimui.zip $(shell find $(MUX) -type f)
	rm -rf build/mux $@
	mkdir -p build/mux/RadioScan
	cp -r build/sd/$(APPDIR)/. build/mux/RadioScan/
	cp -r $(MUX)/. build/mux/RadioScan/
	chmod +x build/mux/RadioScan/mux_launch.sh build/mux/RadioScan/wake_screen.sh
	cd build/mux && zip -qr ../RadioScan-mustardos.muxapp RadioScan
	@echo "$@ ready: copy it into ARCHIVE on the MustardOS card and install it from the Archive Manager"

OS ?= trimui
ifeq ($(OS),mustardos)
INSTALL_DIR = $(SD)/MUOS/application/RadioScan
INSTALL_ROOT = $(SD)/MUOS
INSTALL_FROM = build/mux/RadioScan
else
INSTALL_DIR = $(SD)/$(APPDIR)
INSTALL_ROOT = $(SD)/Apps
INSTALL_FROM = build/sd/$(APPDIR)
endif
install: package
	@test -n "$(SD)" || (echo "usage: make install SD=/path/to/mounted/sdcard [OS=mustardos]" && exit 1)
	@test -d "$(INSTALL_ROOT)" || (echo "$(INSTALL_ROOT) not found - is this the $(OS) card?" && exit 1)
	mkdir -p "$(INSTALL_DIR)"
	cp -r $(INSTALL_FROM)/. "$(INSTALL_DIR)/"
	sync

version:
	@echo $(VERSION)

clean:
	rm -rf build

.PHONY: all desktop package install version clean
