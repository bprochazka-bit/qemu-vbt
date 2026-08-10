# Build cheatsheet

## Userspace tools (hub + host controller)

No kernel headers or QEMU tree needed:

```sh
make            # vbt-medium, vbt-controller
make test       # test-ll + tests/harness.py
make clean
```

Manual builds:

```sh
gcc -Wall -Wextra -O2 -o vbt-medium      vbt_medium.c -lm
gcc -Wall -Wextra -O2 -o vbt-controller  vbt_controller.c vbt_ll.c
gcc -Wall -Wextra -O2 -o test-ll         tests/test_ll.c vbt_ll.c
```

The userspace tools link no kernel or QEMU headers, so they build
standalone in CI.

## QEMU device (`virtio-bluetooth-pci`)

Built as part of a QEMU source tree. The Makefile orchestrates QEMU's own
Meson build — point it at a QEMU checkout:

```sh
make qemu          QEMU_SRC=/path/to/qemu   # integrate + configure + build
make qemu-test     QEMU_SRC=/path/to/qemu   # is the device in the build?
make qemu-upgrade  QEMU_SRC=/path/to/qemu   # rebuild + reinstall after edits
make qemu-clean    QEMU_SRC=/path/to/qemu
```

Granular steps (`qemu-integrate`, `qemu-configure`, `qemu-build`,
`qemu-install`) and tunables (`QEMU_TARGETS`, `QEMU_PREFIX`,
`QEMU_CONFIGURE_FLAGS`) are in `make help`. Equivalent manual flow:

```sh
./scripts/integrate.sh /path/to/qemu
cd /path/to/qemu && mkdir -p build && cd build
../configure --target-list=x86_64-softmmu && make -j$(nproc)
```

See [`src/README.md`](src/README.md).

## Host controller runtime dependency

`vbt-controller` attaches to Linux's virtual HCI driver:

```sh
sudo modprobe hci_vhci
sudo ./vbt-controller /tmp/vbt.sock --node-id host-a
```
