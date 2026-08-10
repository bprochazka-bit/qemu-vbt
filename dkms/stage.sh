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
# Build on one box, install on another (--pack produces a portable artifact
# and does NOT install locally; copy it to the target — see the printed
# install commands and dkms/README.md):
#
#   sudo ./stage.sh --kernel-src /usr/src/linux --pack tarball   # source; target rebuilds
#   sudo ./stage.sh --kernel-src /usr/src/linux --pack deb       # or: rpm
#   sudo ./stage.sh --source virtio_bt.c --pack ko \
#        --kver 6.1.0-18-amd64                                   # prebuilt .ko (no toolchain on target)
#
# Run as root. For a local build/install (no --pack) or a prebuilt --pack
# (ko/bin), you need dkms + kernel headers for the target kernel and the
# Bluetooth core (CONFIG_BT). Source packaging (tarball/deb/rpm) needs only
# the driver source + dkms/dpkg-dev/rpmbuild.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKG_NAME="virtio-bt"
PKG_VER="1.0"

KVER="$(uname -r)"
SRC_FILE=""
KERNEL_SRC=""
TARBALL=""
DOWNLOAD=0
PACK=""
OUTDIR="$(pwd)"

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
        --pack)        PACK="${2:?}"; shift 2 ;;
        --out)         OUTDIR="${2:?}"; shift 2 ;;
        -h|--help)     usage 0 ;;
        *)             die "unknown argument: $1 (see --help)" ;;
    esac
done

[ "$(id -u)" = "0" ] || die "must run as root (sudo)"
command -v dkms >/dev/null 2>&1 || die "dkms not installed (apt/dnf install dkms)"

case "${PACK}" in
    ""|tarball|bin|deb|rpm|ko) ;;
    *) die "unknown --pack format: ${PACK} (tarball|bin|deb|rpm|ko)" ;;
esac

# We only need kernel headers when we actually compile: a local build/install
# (no --pack) or a prebuilt package (--pack ko|bin). Source packaging
# (tarball/deb/rpm) ships source and defers the build to the target box.
WILL_BUILD=0
case "${PACK}" in
    ""|bin|ko) WILL_BUILD=1 ;;
esac

KBUILD="/lib/modules/${KVER}/build"
if [ "${WILL_BUILD}" = "1" ] && [ ! -d "${KBUILD}" ]; then
    die "no kernel headers for ${KVER} at ${KBUILD} — install linux-headers-${KVER}
     (for --pack ko|bin targeting another kernel, install that kernel's headers here,
      or use --pack tarball|deb|rpm to build on the target instead)"
fi

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

# --- Stage the DKMS source tree into /usr/src -----------------------------
DEST="/usr/src/${PKG_NAME}-${PKG_VER}"
info "staging DKMS source tree into ${DEST}"
rm -rf "${DEST}"
mkdir -p "${DEST}"
cp "${OUT}"            "${DEST}/virtio_bt.c"
cp "${HERE}/Makefile" "${DEST}/Makefile"
sed "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"${PKG_VER}\"/" \
    "${HERE}/dkms.conf" > "${DEST}/dkms.conf"

# Fresh registration.
dkms status "${PKG_NAME}/${PKG_VER}" 2>/dev/null | grep -q . && \
    dkms remove "${PKG_NAME}/${PKG_VER}" --all >/dev/null 2>&1 || true
info "dkms add"; dkms add "${PKG_NAME}/${PKG_VER}"

# Newest artifact matching a glob, searched across the dirs dkms may use.
find_art() {
    find /var/lib/dkms "${OUTDIR}" "$(pwd)" -maxdepth 8 -name "$1" \
        -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -1 | cut -d' ' -f2-
}

# --- Local build + install (no --pack) ------------------------------------
if [ -z "${PACK}" ]; then
    info "dkms build"   ; dkms build   "${PKG_NAME}/${PKG_VER}" -k "${KVER}"
    info "dkms install" ; dkms install "${PKG_NAME}/${PKG_VER}" -k "${KVER}"
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
    exit 0
fi

# --- Package a portable artifact for another box (no local install) -------
mkdir -p "${OUTDIR}"
info "packaging '${PACK}' into ${OUTDIR} (not installing on this box)"
case "${PACK}" in
    tarball)
        dkms mktarball "${PKG_NAME}/${PKG_VER}" --source-only
        art="$(find_art "${PKG_NAME}-${PKG_VER}*.dkms.tar.gz")" ;;
    bin)
        dkms build "${PKG_NAME}/${PKG_VER}" -k "${KVER}"
        dkms mktarball "${PKG_NAME}/${PKG_VER}" -k "${KVER}" --binaries-only
        art="$(find_art "${PKG_NAME}-${PKG_VER}*.dkms.tar.gz")" ;;
    deb)
        command -v dpkg-deb >/dev/null 2>&1 || die "--pack deb needs dpkg-dev"
        dkms mkdeb "${PKG_NAME}/${PKG_VER}" --source-only
        art="$(find_art "*${PKG_NAME}*_*.deb")" ;;
    rpm)
        command -v rpmbuild >/dev/null 2>&1 || die "--pack rpm needs rpm-build/rpmbuild"
        dkms mkrpm "${PKG_NAME}/${PKG_VER}" --source-only
        art="$(find_art "*${PKG_NAME}*.rpm")" ;;
    ko)
        dkms build "${PKG_NAME}/${PKG_VER}" -k "${KVER}"
        art="$(find_art 'virtio_bt.ko*')" ;;
esac

[ -n "${art:-}" ] && [ -e "${art}" ] || \
    die "packaging produced no artifact (see the dkms output above)"

if [ "${PACK}" = "ko" ]; then
    # Keep the ".ko" plus any compression suffix (.ko / .ko.zst / .ko.xz).
    dst="${OUTDIR}/virtio_bt-${KVER}.ko${art##*.ko}"
else
    dst="${OUTDIR}/$(basename "${art}")"
fi
[ "${art}" = "${dst}" ] || cp "${art}" "${dst}"
info "artifact ready: ${dst}"

echo ""
echo "Copy ${dst##*/} to the target box, then install there:"
case "${PACK}" in
    tarball|bin)
        echo "  sudo dkms ldtarball ${dst##*/}"
        [ "${PACK}" = "tarball" ] && \
          echo "  sudo dkms install ${PKG_NAME}/${PKG_VER}      # builds against the target's headers"
        echo "  sudo modprobe virtio_bt" ;;
    deb)
        echo "  sudo apt install ./${dst##*/}                 # dkms rebuilds on install"
        echo "  sudo modprobe virtio_bt" ;;
    rpm)
        echo "  sudo dnf install ./${dst##*/}                 # dkms rebuilds on install"
        echo "  sudo modprobe virtio_bt" ;;
    ko)
        echo "  # target kernel MUST be exactly ${KVER}"
        echo "  sudo install -D ${dst##*/} /lib/modules/${KVER}/updates/virtio_bt.ko"
        echo "  sudo depmod -a ${KVER} && sudo modprobe virtio_bt" ;;
esac
