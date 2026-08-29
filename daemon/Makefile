# Makefile for building termux-va inside Termux.
#
#   make -C daemon          # build build/termux-va
#   make -C daemon install  # install to $PREFIX/bin/termux-va
#
# The canonical release build is the NDK cross build (scripts/build-ndk.sh,
# NDK 29.0.14206865, same as anland-termux).  This Makefile is a convenience
# for development on-device: Termux's clang already targets bionic.
#
# -lmediandk provides both AMediaCodec_* and AMediaFormat_* symbols.

CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
PREFIX  ?= /data/data/com.termux/files/usr
BINDIR  ?= $(PREFIX)/bin

ROOT    := ..
BUILD   := $(ROOT)/build
TARGET  := $(BUILD)/termux-va

.PHONY: all install clean

all: $(TARGET)

$(TARGET): termux-va.c $(ROOT)/common/tva_protocol.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(ROOT)/common -o $@ termux-va.c -lmediandk -llog -landroid

install: $(TARGET)
	install -Dm700 $(TARGET) $(BINDIR)/termux-va

clean:
	rm -rf $(BUILD)
