# Cross-builds feederd with the toolchain + sysroot of a finished Thingino build
# (uClibc, libmosquitto from the image; libstdc++ is linked statically).
#
#   make THINGINO=/path/to/thingino-firmware            build ./feederd
#   make THINGINO=/path/to/thingino-firmware overlay    copy it into the Thingino user overlay
#   make install FEEDER=root@<feeder-ip>                dev install over ssh (no rebuild of the image)

CAMERA      ?= princess_feeder_t31l_sc2336_atbm6012bx
THINGINO    ?=
# Buildroot output dir of that camera, e.g. $(THINGINO)/output/master/<camera>-3.10.14-uclibc
THINGINO_OUT ?= $(firstword $(wildcard $(THINGINO)/output/*/$(CAMERA)-*))
OVERLAY     ?= $(THINGINO)/user/$(CAMERA)/overlay
FEEDER      ?=

CROSS    = $(THINGINO_OUT)/host/bin/mipsel-linux-
CXX      = $(CROSS)g++
STRIP    = $(CROSS)strip
CXXFLAGS ?= -Os -Wall -Wextra -std=c++17 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections
LDFLAGS  ?= -static-libstdc++ -Wl,--gc-sections
LDLIBS   := -lmosquitto

feederd: src/feederd.cpp
	@test -n "$(THINGINO)" || { echo "error: set THINGINO=/path/to/thingino-firmware"; exit 1; }
	@test -x "$(CXX)" || { echo "error: no toolchain at $(CXX). Build the Thingino image once first (make CAMERA=$(CAMERA)), or set THINGINO_OUT."; exit 1; }
	$(CXX) $(CXXFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@
	$(STRIP) $@
	@ls -l $@

# Bake feederd into the image: after this, run `make repack` (or `make`) in the
# Thingino tree and flash / `make ota` the result. The config is not copied:
# edit $(OVERLAY)/etc/feederd.conf yourself.
overlay: feederd
	@test -d "$(OVERLAY)" || { echo "error: $(OVERLAY) does not exist (copy thingino/user from this repo first)"; exit 1; }
	install -D -m 755 feederd $(OVERLAY)/usr/sbin/feederd
	install -D -m 755 files/S96feederd $(OVERLAY)/etc/init.d/S96feederd
	@echo "now run 'make repack' in $(THINGINO) and flash or 'make ota IP=...'"

# Dev loop: push a new binary to a running feeder. The writable overlay keeps
# it across reboots, but a full OTA or reflash wipes it unless it's baked in.
install: feederd
	@test -n "$(FEEDER)" || { echo "error: set FEEDER=root@<feeder-ip>"; exit 1; }
	ssh $(FEEDER) '/etc/init.d/S96feederd stop 2>/dev/null; true'
	cat feederd | ssh $(FEEDER) 'cat > /usr/sbin/feederd && chmod 755 /usr/sbin/feederd'
	cat files/S96feederd | ssh $(FEEDER) 'cat > /etc/init.d/S96feederd && chmod 755 /etc/init.d/S96feederd'
	ssh $(FEEDER) 'test -f /etc/feederd.conf || echo "warning: no /etc/feederd.conf on the feeder"; /etc/init.d/S96feederd start'

clean:
	rm -f feederd

.PHONY: overlay install clean
