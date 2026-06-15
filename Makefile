# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

# reac-aes67 — REAC -> AES67 bridge
#
# Native build + tests:        make test
# Build the daemon CLI:        make            (-> build/reac-aes67)
# Cross-tool verification:     make verify      (C decoder vs Python reference)
# Cross-compile for OpenWrt:   make CROSS=aarch64-openwrt-linux-musl-  (mediatek/filogic)
#                          or  make CROSS=mipsel-openwrt-linux-musl-   (ramips/mt7621)
#
# The decode/clock/pcap/rtp/pipeline core is dependency-free so it builds and
# tests anywhere (native x86_64, OpenWrt aarch64/mips). Live AF_PACKET capture + UDP
# send land in a later commit.

CC      ?= cc
CROSS   ?=
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
# libreac: a sibling checkout of github.com/FreeREAC/libreac (native build compiles
# its source in; the OpenWrt build links the libreac package via DEPENDS).
LIBREAC ?= ../libreac
SRCDIR   = src
TESTDIR  = tests
BUILDDIR = build
# LIBREAC_SYSTEM=1 links the installed libreac via pkg-config (-lreac, system
# header) instead of compiling the vendored sibling source in. Used by the Fedora
# rpm. The default native build and the OpenWrt build leave it unset and compile
# $(LIBREAC)/src/reac.c in, so they are unaffected.
LDLIBS  ?= -lm
ifeq ($(LIBREAC_SYSTEM),1)
  INC         = -I$(SRCDIR) $(shell pkg-config --cflags libreac)
  LIBREAC_SRC =
  LDLIBS     += $(shell pkg-config --libs libreac)
else
  INC         = -I$(SRCDIR) -I$(LIBREAC)/include
  LIBREAC_SRC = $(LIBREAC)/src/reac.c
endif

CORE_SRCS = $(SRCDIR)/reac_decode.c $(SRCDIR)/media_clock.c $(SRCDIR)/pcap_source.c \
            $(SRCDIR)/rtp_l24.c $(SRCDIR)/plc.c $(SRCDIR)/pipeline.c \
            $(SRCDIR)/aes67_send.c $(SRCDIR)/reac_capture.c $(SRCDIR)/sdp.c $(SRCDIR)/sap.c \
            $(SRCDIR)/ubus_stats.c $(SRCDIR)/tone.c $(SRCDIR)/ptp_clock.c \
            $(SRCDIR)/packetizer.c $(LIBREAC_SRC)
TEST_SRCS = $(wildcard $(TESTDIR)/test_*.c)
TEST_BINS = $(patsubst $(TESTDIR)/%.c,$(BUILDDIR)/%,$(TEST_SRCS))
DAEMON    = $(BUILDDIR)/reac-aes67

# Test fixture: a real REAC capture, vendored here so the build is self-contained
# (the canonical copy lives in reac-tools). Override REAC_FIXTURE to use another.
REAC_FIXTURE ?= tests/fixtures/real_reac_stream.pcap

.PHONY: all test verify clean

all: $(DAEMON)
	@echo "Built $(DAEMON). Run: make test ; make verify"

$(BUILDDIR)/.dir:
	@mkdir -p $(BUILDDIR)
	@touch $@

$(DAEMON): $(SRCDIR)/main.c $(CORE_SRCS) | $(BUILDDIR)/.dir
	$(CROSS)$(CC) $(CFLAGS) $(INC) -o $@ $(SRCDIR)/main.c $(CORE_SRCS) $(LDLIBS)

# Each test binary links the whole core (small) for simplicity.
# Pattern restricted to test_% so it never collides with the daemon name.
$(BUILDDIR)/test_%: $(TESTDIR)/test_%.c $(CORE_SRCS) | $(BUILDDIR)/.dir
	$(CROSS)$(CC) $(CFLAGS) $(INC) -o $@ $< $(CORE_SRCS) $(LDLIBS)

test: $(TEST_BINS) $(DAEMON)
	@fail=0; \
	for t in $(TEST_BINS); do \
		echo "== $$t =="; \
		REAC_FIXTURE=$(REAC_FIXTURE) $$t || fail=1; \
	done; \
	echo "== tests/test_cli_sdp.sh =="; \
	sh $(TESTDIR)/test_cli_sdp.sh $(DAEMON) >/dev/null && echo "ok   cli-sdp" || fail=1; \
	if [ $$fail -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "TESTS FAILED"; exit 1; fi

# Cross-tool verification: the C decoder must agree, sample-for-sample, with an
# independent Python reference decode of the same real capture.
verify: $(DAEMON)
	python3 $(TESTDIR)/cross_verify.py $(DAEMON) $(REAC_FIXTURE)

clean:
	rm -rf $(BUILDDIR)
