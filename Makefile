# Makefile for qemu-vbt — Virtual Bluetooth (BLE) medium
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Userspace targets (no QEMU or kernel needed):
#   make            — build userspace binaries (vbt-medium, vbt-controller)
#   make userspace  — same as `make`
#   make test       — run test-ll + tests/harness.py
#   make install    — install userspace binaries (honors PREFIX, DESTDIR)
#   make clean      — remove userspace build artifacts
#
# QEMU device targets (build QEMU with the virtio-bluetooth device;
# require QEMU_SRC=/path/to/qemu):
#   make qemu          — integrate + configure + build QEMU with the device
#   make qemu-upgrade  — re-integrate + rebuild + reinstall (quick iterate)
#   make qemu-test     — confirm the device is present in the built QEMU
#   make qemu-clean    — clean the QEMU build dir
#   (granular: qemu-integrate, qemu-configure, qemu-build, qemu-install)
#
# See `make help`, src/README.md, and scripts/integrate.sh.

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

# The standalone simulated BLE peripheral (scripts/sim_peripheral.py) is a
# stdlib-only Python script. It installs as vbt-sim-peripheral so tooling —
# Nyxus's simulators feature launches it under a systemd unit — can invoke
# it by a stable name on PATH, alongside the compiled userspace binaries.
SIM_PERIPHERAL_SRC := scripts/sim_peripheral.py
SIM_PERIPHERAL_BIN := vbt-sim-peripheral

install: $(USERSPACE_BINS)
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 0755 $(USERSPACE_BINS) $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 0755 $(SIM_PERIPHERAL_SRC) \
		$(DESTDIR)$(BINDIR)/$(SIM_PERIPHERAL_BIN)

uninstall:
	rm -f $(addprefix $(DESTDIR)$(BINDIR)/,$(USERSPACE_BINS))
	rm -f $(DESTDIR)$(BINDIR)/$(SIM_PERIPHERAL_BIN)

test: vbt-medium test-ll
	./test-ll
	python3 tests/harness.py
	python3 -m unittest -v tests.test_sim_config

clean:
	rm -f $(USERSPACE_BINS) test-ll

# ==================================================================
#  QEMU device build orchestration
#
#  These targets drive QEMU's own Meson build to produce a
#  qemu-system-* with the virtio-bluetooth device. They require a QEMU
#  source tree (a git clone of qemu/qemu):
#
#    make qemu          QEMU_SRC=/path/to/qemu     # first-time build
#    make qemu-upgrade  QEMU_SRC=/path/to/qemu     # rebuild + reinstall
#
#  The device sources are copied in from src/ + the repo root by
#  scripts/integrate.sh; only src/vbt_virtio.c and the shared vbt_ll
#  core are compiled into QEMU.
# ==================================================================
QEMU_SRC             ?=
QEMU_TARGETS         ?= x86_64-softmmu
QEMU_PREFIX          ?= /usr/local
QEMU_CONFIGURE_FLAGS ?= --enable-debug
QEMU_BUILD_DIR        = $(QEMU_SRC)/build
QEMU_BINARY           = $(QEMU_BUILD_DIR)/qemu-system-x86_64
QEMU_INSTALLED        = $(QEMU_PREFIX)/bin/qemu-system-x86_64
NPROC                := $(shell nproc 2>/dev/null || echo 4)

check-qemu-src:
ifndef QEMU_SRC
	$(error QEMU_SRC is not set. Usage: make qemu QEMU_SRC=/path/to/qemu)
endif
	@test -f "$(QEMU_SRC)/meson.build" || \
		{ echo "ERROR: $(QEMU_SRC)/meson.build not found — not a QEMU source tree"; exit 1; }

# First-time build: integrate the device, configure, and build. Ordered via
# recursive make so it is correct even under a parallel top-level invocation.
qemu: check-qemu-src
	@$(MAKE) --no-print-directory qemu-integrate QEMU_SRC="$(QEMU_SRC)"
	@$(MAKE) --no-print-directory qemu-configure QEMU_SRC="$(QEMU_SRC)"
	@$(MAKE) --no-print-directory qemu-build     QEMU_SRC="$(QEMU_SRC)"
	@echo ""
	@echo "=== Built $(QEMU_BINARY) ==="
	@echo "  quick check:  make qemu-test QEMU_SRC=$(QEMU_SRC)"
	@echo "  run:          $(QEMU_BINARY) -M q35 -m 512 \\"
	@echo "                  -device virtio-bluetooth-pci,medium=/tmp/vbt.sock,node_id=vm-a ..."
	@echo "  install:      make qemu-upgrade QEMU_SRC=$(QEMU_SRC)"

qemu-integrate: check-qemu-src
	@echo "=== Integrating virtio-bluetooth device into QEMU ==="
	./scripts/integrate.sh "$(QEMU_SRC)"

qemu-configure: check-qemu-src
	@echo "=== Configuring QEMU ($(QEMU_TARGETS)) ==="
	@mkdir -p "$(QEMU_BUILD_DIR)"
	cd "$(QEMU_BUILD_DIR)" && "$(QEMU_SRC)/configure" \
		--target-list=$(QEMU_TARGETS) \
		--prefix="$(QEMU_PREFIX)" \
		$(QEMU_CONFIGURE_FLAGS)

qemu-build: check-qemu-src
	@test -d "$(QEMU_BUILD_DIR)" || \
		{ echo "ERROR: no build dir — run 'make qemu-configure QEMU_SRC=$(QEMU_SRC)' first"; exit 1; }
	@echo "=== Building QEMU (-j$(NPROC)) ==="
	$(MAKE) -C "$(QEMU_BUILD_DIR)" -j$(NPROC)

# Quick iterate after editing src/vbt_virtio.c (or pulling a newer QEMU):
# re-copy the device sources, rebuild, and reinstall over any existing binary.
qemu-upgrade: check-qemu-src
	@$(MAKE) --no-print-directory qemu-integrate QEMU_SRC="$(QEMU_SRC)"
	@$(MAKE) --no-print-directory qemu-build     QEMU_SRC="$(QEMU_SRC)"
	@echo "=== Reinstalling QEMU into $(QEMU_PREFIX) (overwriting) ==="
	$(MAKE) -C "$(QEMU_BUILD_DIR)" install
	@echo "   installed $(QEMU_INSTALLED)"

qemu-install: check-qemu-src
	@test -x "$(QEMU_BINARY)" || \
		{ echo "ERROR: $(QEMU_BINARY) not found — run 'make qemu' first"; exit 1; }
	@if [ -x "$(QEMU_INSTALLED)" ]; then \
		echo "   $(QEMU_INSTALLED) already exists — use 'make qemu-upgrade' to overwrite"; \
	else \
		$(MAKE) -C "$(QEMU_BUILD_DIR)" install; \
		echo "   installed $(QEMU_INSTALLED)"; \
	fi

# Guestless smoke test: confirm the device linked into the build.
qemu-test: check-qemu-src
	@test -x "$(QEMU_BINARY)" || \
		{ echo "ERROR: $(QEMU_BINARY) not found — run 'make qemu' first"; exit 1; }
	@echo "=== Checking virtio-bluetooth-pci is registered ==="
	@"$(QEMU_BINARY)" -device help 2>&1 | grep -i "virtio-bluetooth" \
		&& echo "OK: device present in $(QEMU_BINARY)" \
		|| { echo "FAIL: virtio-bluetooth not found in this build"; exit 1; }

qemu-clean: check-qemu-src
	@test -d "$(QEMU_BUILD_DIR)" && \
		$(MAKE) -C "$(QEMU_BUILD_DIR)" clean || true

help:
	@echo "qemu-vbt — build targets"
	@echo ""
	@echo "Userspace (no QEMU/kernel needed):"
	@echo "  make                 vbt-medium, vbt-controller"
	@echo "  make test            test-ll + tests/harness.py"
	@echo "  make vbt-vhost-user  vhost-user backend (needs LIBVHOST_USER=...)"
	@echo "  make install         install userspace binaries"
	@echo "  make clean           remove userspace artifacts"
	@echo ""
	@echo "QEMU device (need QEMU_SRC=/path/to/qemu):"
	@echo "  make qemu            integrate + configure + build QEMU w/ the device"
	@echo "  make qemu-upgrade    re-integrate + rebuild + reinstall (quick iterate)"
	@echo "  make qemu-test       confirm the device is present in the build"
	@echo "  make qemu-clean      clean the QEMU build dir"
	@echo ""
	@echo "Variables: QEMU_SRC, QEMU_TARGETS (=$(QEMU_TARGETS)),"
	@echo "  QEMU_PREFIX (=$(QEMU_PREFIX)), QEMU_CONFIGURE_FLAGS (=$(QEMU_CONFIGURE_FLAGS))"

.PHONY: all userspace install uninstall test test-ll clean help \
        check-qemu-src qemu qemu-integrate qemu-configure qemu-build \
        qemu-upgrade qemu-install qemu-test qemu-clean
