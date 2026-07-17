#!/bin/bash
# integrate.sh — Patch the virtio-bluetooth (vbt) device into a QEMU tree
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Usage:
#   ./scripts/integrate.sh /path/to/qemu-source
#
# What this does:
#   1. Copies the device source + shared controller core into
#      hw/bluetooth/ inside the QEMU tree
#   2. Adds subdir('bluetooth') to hw/meson.build
#   3. Adds `source bluetooth/Kconfig` to hw/Kconfig
#
# Idempotent: running twice will not duplicate entries.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SRC_DIR="${PROJECT_ROOT}/src"

if [ $# -ne 1 ]; then
    echo "Usage: $0 /path/to/qemu-source-tree" >&2
    exit 1
fi
QEMU_DIR="$1"

if [ ! -f "${QEMU_DIR}/meson.build" ] || [ ! -d "${QEMU_DIR}/hw" ]; then
    echo "ERROR: ${QEMU_DIR} does not look like a QEMU source tree." >&2
    exit 1
fi

echo "=== Integrating virtio-bluetooth (vbt) into ${QEMU_DIR} ==="

TARGET_DIR="${QEMU_DIR}/hw/bluetooth"
mkdir -p "${TARGET_DIR}"

# Device source + build files live in src/ ...
cp -v "${SRC_DIR}/vbt_virtio.c"  "${TARGET_DIR}/"
cp -v "${SRC_DIR}/meson.build"   "${TARGET_DIR}/"
cp -v "${SRC_DIR}/Kconfig"       "${TARGET_DIR}/"
# ... the controller core and headers are shared with the userspace tools,
# so they come from the repo root (single source of truth).
cp -v "${PROJECT_ROOT}/vbt_ll.c" "${TARGET_DIR}/"
cp -v "${PROJECT_ROOT}/vbt_ll.h" "${TARGET_DIR}/"
cp -v "${PROJECT_ROOT}/vbt.h"    "${TARGET_DIR}/"
echo "   Sources copied to ${TARGET_DIR}/"

# ---- Patch hw/meson.build ----
HW_MESON="${QEMU_DIR}/hw/meson.build"
if grep -q "subdir('bluetooth')" "${HW_MESON}" 2>/dev/null; then
    echo "   hw/meson.build already references bluetooth — skipping"
else
    echo "" >> "${HW_MESON}"
    echo "# Virtual BLE controller (qemu-vbt)" >> "${HW_MESON}"
    echo "subdir('bluetooth')" >> "${HW_MESON}"
    echo "   Patched ${HW_MESON}"
fi

# ---- Patch hw/Kconfig ----
HW_KCONFIG="${QEMU_DIR}/hw/Kconfig"
if grep -q "bluetooth/Kconfig" "${HW_KCONFIG}" 2>/dev/null; then
    echo "   hw/Kconfig already sources bluetooth/Kconfig — skipping"
else
    echo "" >> "${HW_KCONFIG}"
    echo "source bluetooth/Kconfig" >> "${HW_KCONFIG}"
    echo "   Patched ${HW_KCONFIG}"
fi

cat <<EOF

=== Integration complete ===

Next steps:
  cd ${QEMU_DIR}
  mkdir -p build && cd build
  ../configure --target-list=x86_64-softmmu
  make -j\$(nproc)

Then attach a guest:
  ./qemu-system-x86_64 -machine q35 -m 512 \\
    -drive file=vm.qcow2,format=qcow2 \\
    -device virtio-bluetooth-pci,medium=/tmp/vbt.sock,node_id=vm-a -nographic
EOF
