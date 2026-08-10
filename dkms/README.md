# virtio_bt out-of-tree DKMS package

If your guest kernel was built **without** `CONFIG_BT_VIRTIO`, the
`virtio-bluetooth-pci` device shows up on the PCI bus but nothing binds to
it (`modinfo virtio_bt` is empty, no `hci0`). This package builds the
upstream `virtio_bt` driver out-of-tree with DKMS so the device gets a
native controller — no full guest-kernel rebuild.

`virtio_bt` is a small, self-contained leaf driver, which is why this
works cleanly.

## What it does

`stage.sh` obtains the `virtio_bt.c` source **matching your kernel**,
installs a DKMS package under `/usr/src/virtio-bt-1.0/`, and runs
`dkms add/build/install`. It intentionally does **not** vendor a copy of
the driver: version-matching the source to your kernel's Bluetooth/virtio
API is what keeps it building, so the source comes from your kernel tree
(or a matching download).

## Prerequisites (in the guest)

- `dkms` and a compiler toolchain (`apt install dkms build-essential` /
  `dnf install dkms kernel-devel`)
- kernel headers for the running kernel (`linux-headers-$(uname -r)`)
- the Bluetooth core — `CONFIG_BT` (the `bluetooth` module) must be present
  (`modprobe bluetooth`); `virtio_bt` depends on its exported symbols
- kernel ≥ 5.13 (when `virtio_bt`, `VIRTIO_ID_BLUETOOTH`, and
  `uapi/linux/virtio_bt.h` landed) — the uapi header ships in the
  kernel-headers package even when the driver itself was not compiled

## Usage

Pick whichever source you have. Exact-version sources are best:

```bash
# Debian/Ubuntu — install the matching kernel source, then auto-detect:
sudo apt-get install linux-source-$(uname -r | cut -d- -f1-2)
sudo ./stage.sh                       # finds /usr/src/linux-source-*.tar.*

# From a kernel source tree you already have:
sudo ./stage.sh --kernel-src /usr/src/linux

# From a single file you extracted yourself:
sudo ./stage.sh --source /path/to/virtio_bt.c

# Last resort — download the matching version from kernel.org:
sudo ./stage.sh --download
```

Then confirm:

```bash
lsmod | grep virtio_bt
ls /sys/class/bluetooth/            # expect hci0
hciconfig -a
dmesg | grep -i -e virtio_bt -e bluetooth | tail
```

`bluetoothctl` should now see the adapter, and you can `scan on` / `pair` /
`connect` peers on the medium exactly as with a real controller.

## Build on one box, install on another

If you stage on a build host and deploy to a different (often minimal or
air-gapped) target, use `--pack` to produce a portable artifact instead of
installing locally. Pick the format by what the **target** has:

| Target has… | Use | Build host needs |
|---|---|---|
| toolchain + kernel headers | `--pack tarball` (or `deb`/`rpm`) | just the driver source |
| **no** toolchain | `--pack ko` (or `bin`) | the **target kernel's** headers |

Source packages rebuild on the target (and keep rebuilding across its
kernel upgrades via DKMS); prebuilt packages are locked to one exact
kernel version.

```bash
# --- on the build box ---
# Source package — target compiles it (portable across the target's kernels):
sudo ./stage.sh --kernel-src /usr/src/linux --pack tarball --out ./out
sudo ./stage.sh --kernel-src /usr/src/linux --pack deb     --out ./out   # or rpm

# Prebuilt module for a target with no compiler — must match its exact
# kernel, so install THAT kernel's headers here first:
sudo ./stage.sh --source ./virtio_bt.c --pack ko \
     --kver 6.1.0-18-amd64 --out ./out
```

`--pack` never touches the build box's running kernel; it just drops the
artifact in `--out` and prints the exact install commands. On the target:

```bash
# source tarball:
sudo dkms ldtarball virtio-bt-1.0.dkms.tar.gz
sudo dkms install virtio-bt/1.0            # builds against the target's headers
sudo modprobe virtio_bt

# .deb / .rpm (DKMS rebuilds on install):
sudo apt install ./virtio-bt-dkms_1.0_all.deb     # or: dnf install ./...rpm
sudo modprobe virtio_bt

# prebuilt .ko (target kernel must equal --kver):
sudo install -D virtio_bt-<kver>.ko /lib/modules/<kver>/updates/virtio_bt.ko
sudo depmod -a <kver> && sudo modprobe virtio_bt
```

Prerequisites still apply on whichever box does the compiling: `dkms`,
kernel headers, and the Bluetooth core (`CONFIG_BT`). `--pack deb`/`rpm`
additionally need `dpkg-dev` / `rpmbuild` on the build box.

## Kernel upgrades

The package is registered `AUTOINSTALL=yes`, so DKMS rebuilds it on kernel
updates. `virtio_bt`'s API is stable across most releases, so this usually
just works. If a build fails after a major kernel jump, re-run `stage.sh`
to refresh the source from the *new* kernel:

```bash
sudo dkms remove virtio-bt/1.0 --all
sudo ./stage.sh --kernel-src /usr/src/linux   # (or your matching source)
```

## Uninstall

```bash
sudo modprobe -r virtio_bt
sudo dkms remove virtio-bt/1.0 --all
sudo rm -rf /usr/src/virtio-bt-1.0
```

## If you can't get the driver built

The medium is transport-agnostic, so you don't strictly need `virtio_bt`.
The `vbt-controller` daemon (repo root) attaches to `/dev/vhci`
(`hci_vhci`, near-universally built) and bridges to the medium — run it
inside the guest, tunneling to the host hub over a stock `vhost-vsock-pci`
or `virtio-serial-pci`. Same result, a more common guest dependency.
