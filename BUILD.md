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

Built as part of a QEMU source tree:

```sh
./scripts/integrate.sh /path/to/qemu
cd /path/to/qemu && mkdir -p build && cd build
../configure --target-list=x86_64-softmmu
make -j$(nproc)
```

See [`src/README.md`](src/README.md).

## Host controller runtime dependency

`vbt-controller` attaches to Linux's virtual HCI driver:

```sh
sudo modprobe hci_vhci
sudo ./vbt-controller /tmp/vbt.sock --node-id host-a
```
