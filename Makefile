# Makefile for qemu-vbt — Virtual Bluetooth (BLE) medium
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Targets:
#   make            — build userspace binaries (vbt-medium, vbt-controller)
#   make userspace  — same as `make`
#   make test       — run tests/harness.py against ./vbt-medium
#   make install    — install userspace binaries (honors PREFIX, DESTDIR)
#   make clean      — remove build artifacts
#
# The QEMU device model in src/ is built as part of a QEMU source tree;
# see src/README.md and scripts/integrate.sh. It is NOT built here.

CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2
LDLIBS_MEDIUM := -lm

USERSPACE_BINS := vbt-medium vbt-controller

all: userspace
userspace: $(USERSPACE_BINS)

vbt-medium: vbt_medium.c vbt.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS_MEDIUM)

vbt-controller: vbt_controller.c vbt_ll.c vbt_ll.h vbt.h
	$(CC) $(CFLAGS) -o $@ vbt_controller.c vbt_ll.c

# Controller-core unit test (no kernel / QEMU needed)
test-ll: tests/test_ll.c vbt_ll.c vbt_ll.h vbt.h
	$(CC) $(CFLAGS) -o $@ tests/test_ll.c vbt_ll.c

# Install destination (DESTDIR for staged/packaged installs).
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
INSTALL ?= install

install: $(USERSPACE_BINS)
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 0755 $(USERSPACE_BINS) $(DESTDIR)$(BINDIR)

uninstall:
	rm -f $(addprefix $(DESTDIR)$(BINDIR)/,$(USERSPACE_BINS))

test: vbt-medium test-ll
	./test-ll
	python3 tests/harness.py

clean:
	rm -f $(USERSPACE_BINS) test-ll

.PHONY: all userspace install uninstall test test-ll clean
