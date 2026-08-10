#!/usr/bin/env bash
# stage.sh — stage the upstream virtio_bt driver and build it via DKMS.
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The virtio_bt driver is a small, self-contained leaf driver. When a guest
# kernel ships without CONFIG_BT_VIRTIO, this script extracts the driver
# source *matching that kernel* and builds it out-of-tree with DKMS, so the
# virtio-bluetooth-pci device gets a native hci0.
#
# It does NOT vendor a copy of the driver — version-matching the source to
# your kernel is what keeps it building. Point it at a source, or let it
# find/download one:
#
#   sudo ./stage.sh --kernel-src /usr/src/linux         # a kernel source tree
#   sudo ./stage.sh --source /path/to/virtio_bt.c       # an exact file
#   sudo ./stage.sh --tarball /usr/src/linux-source-6.1.tar.xz
#   sudo ./stage.sh                                      # auto-detect, else hint
#   sudo ./stage.sh --download                           # fetch from kernel.org
#
# Run as root. Requires: dkms, kernel headers for the running kernel, and
# the Bluetooth core (CONFIG_BT, i.e. the `bluetooth` module).

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKG_NAME="virtio-bt"
PKG_VER="1.0"

KVER="$(uname -r)"
SRC_FILE=""
KERNEL_SRC=""
TARBALL=""
DOWNLOAD=0

die()  { echo "stage.sh: error: $*" >&2; exit 1; }
info() { echo "stage.sh: $*"; }

usage() {
    # Print the leading comment block (after the shebang), stopping at the
    # first non-comment line.
    awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next } NR>1 { exit }' \
        "${BASH_SOURCE[0]}"
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --source)      SRC_FILE="${2:?}"; shift 2 ;;
        --kernel-src)  KERNEL_SRC="${2:?}"; shift 2 ;;
        --tarball)     TARBALL="${2:?}"; shift 2 ;;
        --download)    DOWNLOAD=1; shift ;;
        --kver)        KVER="${2:?}"; shift 2 ;;
        --version)     PKG_VER="${2:?}"; shift 2 ;;
        -h|--help)     usage 0 ;;
        *)             die "unknown argument: $1 (see --help)" ;;
    esac
done

[ "$(id -u)" = "0" ] || die "must run as root (sudo)"
command -v dkms >/dev/null 2>&1 || die "dkms not installed (apt/dnf install dkms)"

KBUILD="/lib/modules/${KVER}/build"
[ -d "${KBUILD}" ] || die "no kernel headers for ${KVER} at ${KBUILD} — install linux-headers-${KVER}"

# --- Sanity: the Bluetooth core and the uapi header the driver needs ------
if ! [ -e "${KBUILD}/include/uapi/linux/virtio_bt.h" ] \
   && ! [ -e /usr/include/linux/virtio_bt.h ]; then
    info "WARNING: uapi/linux/virtio_bt.h not found in the kernel headers."
    info "         Your kernel may predate virtio_bt (needs >= 5.13)."
fi
if ! modinfo bluetooth >/dev/null 2>&1 && \
   ! grep -qE '^CONFIG_BT=(y|m)' "${KBUILD}/.config" 2>/dev/null; then
    info "WARNING: Bluetooth core (CONFIG_BT / 'bluetooth' module) not detected."
    info "         virtio_bt depends on it; load/enable it or the module won't load."
fi

# --- Resolve the driver source into a temp file ---------------------------
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT
OUT="${TMP}/virtio_bt.c"

resolve_from_tree() {   # $1 = kernel source dir
    local f="$1/drivers/bluetooth/virtio_bt.c"
    [ -f "$f" ] && { cp "$f" "${OUT}"; info "using ${f}"; return 0; }
    return 1
}

resolve_from_tarball() {   # $1 = linux-source tarball
    local tb="$1"
    [ -f "$tb" ] || return 1
    info "extracting virtio_bt.c from ${tb}"
    if tar -xf "$tb" -O --wildcards '*/drivers/bluetooth/virtio_bt.c' \
            > "${OUT}" 2>/dev/null && [ -s "${OUT}" ]; then
        return 0
    fi
    return 1
}

resolve_download() {
    command -v curl >/dev/null 2>&1 || die "curl needed for --download"
    local num maj tag url
    num="${KVER%%-*}"                       # 6.6.8-arch1 -> 6.6.8
    maj="$(echo "$num" | cut -d. -f1,2)"    # -> 6.6
    for tag in "v${num}" "v${maj}"; do
        for url in \
          "https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/drivers/bluetooth/virtio_bt.c?h=${tag}" \
          "https://raw.githubusercontent.com/torvalds/linux/${tag}/drivers/bluetooth/virtio_bt.c"; do
            info "trying ${url}"
            if curl -fsSL --max-time 30 "$url" -o "${OUT}" 2>/dev/null \
               && grep -q 'VIRTIO_ID_BLUETOOTH\|virtio_bt' "${OUT}"; then
                info "downloaded virtio_bt.c (${tag})"
                return 0
            fi
        done
    done
    return 1
}

got=0
if [ -n "${SRC_FILE}" ]; then
    [ -f "${SRC_FILE}" ] || die "--source ${SRC_FILE} not found"
    cp "${SRC_FILE}" "${OUT}"; info "using ${SRC_FILE}"; got=1
elif [ -n "${KERNEL_SRC}" ]; then
    resolve_from_tree "${KERNEL_SRC}" && got=1 || die "no virtio_bt.c under ${KERNEL_SRC}/drivers/bluetooth/"
elif [ -n "${TARBALL}" ]; then
    resolve_from_tarball "${TARBALL}" && got=1 || die "could not extract from ${TARBALL}"
elif [ "${DOWNLOAD}" = "1" ]; then
    resolve_download && got=1 || die "download failed — try --kernel-src or --tarball"
else
    # Auto-detect common locations.
    if resolve_from_tree "${KBUILD}"; then got=1
    else
        for tb in /usr/src/linux-source-*.tar.* /usr/src/linux-source-*/linux-source-*.tar.*; do
            [ -e "$tb" ] || continue
            resolve_from_tarball "$tb" && { got=1; break; }
        done
    fi
fi

if [ "${got}" != "1" ]; then
    cat >&2 <<EOF
stage.sh: could not locate a matching virtio_bt.c automatically.

Provide one of:
  --kernel-src /path/to/linux        (a kernel source tree)
  --source     /path/to/virtio_bt.c  (an exact file)
  --tarball    /usr/src/linux-source-<ver>.tar.xz
  --download                         (fetch matching version from kernel.org)

On Debian/Ubuntu: apt-get install linux-source-\$(uname -r | cut -d- -f1-2)
then re-run; the tarball in /usr/src is auto-detected.
EOF
    exit 1
fi

grep -q 'virtio_bt' "${OUT}" || die "staged file does not look like virtio_bt.c"

# --- Install the package tree into /usr/src and build with DKMS -----------
DEST="/usr/src/${PKG_NAME}-${PKG_VER}"
info "installing package into ${DEST}"
rm -rf "${DEST}"
mkdir -p "${DEST}"
cp "${OUT}"            "${DEST}/virtio_bt.c"
cp "${HERE}/Makefile" "${DEST}/Makefile"
sed "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"${PKG_VER}\"/" \
    "${HERE}/dkms.conf" > "${DEST}/dkms.conf"

# Remove any prior registration of this version, then add/build/install.
dkms status "${PKG_NAME}/${PKG_VER}" 2>/dev/null | grep -q . && \
    dkms remove "${PKG_NAME}/${PKG_VER}" --all 2>/dev/null || true

info "dkms add"     ; dkms add     "${PKG_NAME}/${PKG_VER}"
info "dkms build"   ; dkms build   "${PKG_NAME}/${PKG_VER}" -k "${KVER}"
info "dkms install" ; dkms install "${PKG_NAME}/${PKG_VER}" -k "${KVER}"

# --- Load it --------------------------------------------------------------
depmod -a "${KVER}"
if modprobe virtio_bt 2>/dev/null; then
    info "virtio_bt loaded."
else
    info "built and installed, but modprobe failed — check: dmesg | tail"
fi

echo ""
info "done. Check for a controller:"
echo "     ls /sys/class/bluetooth/ ; hciconfig -a"
echo "     dmesg | grep -i -e virtio_bt -e bluetooth | tail"
