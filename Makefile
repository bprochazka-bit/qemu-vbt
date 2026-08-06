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

# vhost-user backend — attaches to UNMODIFIED QEMU via vhost-user-device-pci.
# Opt-in: needs QEMU's libvhost-user (not a distro package). Point
# LIBVHOST_USER at its include dir and LIBVHOST_USER_LIB at the built
# archive from your QEMU build, e.g.:
#   make vbt-vhost-user \
#     LIBVHOST_USER=/path/to/qemu/subprojects/libvhost-user \
#     LIBVHOST_USER_LIB=/path/to/qemu/build/subprojects/libvhost-user/libvhost-user.a
LIBVHOST_USER ?=
LIBVHOST_USER_LIB ?=
vbt-vhost-user: vbt_vhost_user.c vbt_ll.c vbt_ll.h vbt.h
	@test -n "$(LIBVHOST_USER)" || { \
	  echo "set LIBVHOST_USER=<qemu>/subprojects/libvhost-user (and LIBVHOST_USER_LIB)"; \
	  exit 1; }
	$(CC) $(CFLAGS) -I$(LIBVHOST_USER) -o $@ vbt_vhost_user.c vbt_ll.c $(LIBVHOST_USER_LIB)

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
