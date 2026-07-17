# QEMU virtio-bluetooth (vbt) — Meson build integration
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Placed at hw/bluetooth/meson.build inside the QEMU source tree by
# scripts/integrate.sh. The parent hw/meson.build gets a
# subdir('bluetooth') entry.
#
# The vbt-medium hub and vbt-controller host bridge are standalone tools
# built from the repo's top-level Makefile, not here.

system_ss.add(when: 'CONFIG_VBT_VIRTIO', if_true: files(
  'vbt_virtio.c',
  'vbt_ll.c',
))
