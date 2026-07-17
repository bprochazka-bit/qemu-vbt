# QEMU virtio-bluetooth device (`virtio-bluetooth-pci`)

This directory holds the QEMU device model that gives a guest a virtual
BLE controller attached to the [`vbt-medium`](../README.md) hub. The
guest binds it with the stock in-tree `virtio_bt` driver and BlueZ sees a
normal `hci0`.

## Files

| File | Role |
|---|---|
| `vbt_virtio.c` | The `virtio-bluetooth` device + `virtio-bluetooth-pci` binding: virtqueue transport, medium socket bridge, timers |
| `meson.build` | Compiled at `hw/bluetooth/meson.build` (builds `vbt_virtio.c` + `vbt_ll.c`) |
| `Kconfig` | `CONFIG_VBT_VIRTIO` |

The controller brains — the HCI command engine and Link-Layer state
machine — are **not** here: they are the repo's [`vbt_ll.c`](../vbt_ll.c)
/ [`vbt_ll.h`](../vbt_ll.h) and [`vbt.h`](../vbt.h), shared verbatim with
the `vbt-controller` host bridge so there is one implementation. The
`integrate.sh` script copies those from the repo root next to
`vbt_virtio.c` at integration time, so the QEMU build is self-contained
while the source of truth stays single.

## How it maps to the hardware split

```
 guest BlueZ (L2CAP / ATT / GATT / SMP)      ← unmodified
      │ HCI packets
 guest virtio_bt driver                       ← stock in-tree driver
      │ tx vq (cmd/ACL out) · rx vq (evt/ACL in)
 ┌────┴──────────────────────────────────────┐
 │ virtio-bluetooth-pci (this device)         │
 │   tx handler → vbt_ll_hci_from_host()      │
 │   vbt_ll hci_to_host cb → rx vq → guest    │
 │   vbt_ll pdu_to_medium cb → medium socket  │
 │   medium socket → vbt_ll_pdu_from_medium() │
 └────┬──────────────────────────────────────┘
      │ Unix socket, vbt wire protocol
   vbt-medium
```

Each HCI packet on a virtqueue is H4-framed (leading packet-type byte),
which is exactly what `vbt_ll` consumes and produces, so the transport is
a thin copy in each direction.

## Build

```bash
./scripts/integrate.sh /path/to/qemu
cd /path/to/qemu && mkdir -p build && cd build
../configure --target-list=x86_64-softmmu
make -j$(nproc)
```

`integrate.sh` creates `hw/bluetooth/`, copies the sources, and wires
`subdir('bluetooth')` into `hw/meson.build` and `source bluetooth/Kconfig`
into `hw/Kconfig`. It is idempotent.

## Run

```bash
# start the hub
./vbt-medium /tmp/vbt.sock -c /tmp/vbt.ctl

# a peripheral VM and a central VM on the same medium
qemu-system-x86_64 -machine q35 -m 512 -drive file=peri.qcow2,format=qcow2 \
  -device virtio-bluetooth-pci,medium=/tmp/vbt.sock,node_id=peri-a -nographic
qemu-system-x86_64 -machine q35 -m 512 -drive file=cent.qcow2,format=qcow2 \
  -device virtio-bluetooth-pci,medium=/tmp/vbt.sock,node_id=cent-b -nographic
```

Device properties:

| Property | Default | Meaning |
|---|---|---|
| `medium` | (none) | Path to the `vbt-medium` Unix socket. Without it the controller runs but hears no peers. |
| `node_id` | `vbt-vm` | Identity registered with the hub (shown in `LIST_PEERS`). |
| `bdaddr` | derived | Public device address `xx:xx:xx:xx:xx:xx`; derived from `node_id` if unset. |

Inside the guests, use BlueZ normally (`bluetoothctl advertise on`,
`scan on`, `pair`, `gatttool`).

## Verification status

* The wrapped controller core (`vbt_ll`) is unit-tested end-to-end in
  [`tests/test_ll.c`](../tests/test_ll.c) — advertise, discover, connect,
  ACL both ways, encryption, disconnect (16/16).
* The `vbt-medium` routing/propagation is covered by
  [`tests/harness.py`](../tests/harness.py) (14/14).
* This transport glue (`vbt_virtio.c`) targets current QEMU virtio APIs
  and is verified by building it into a QEMU tree and attaching guests; it
  is not compiled by the repo's Makefile (which builds only the userspace
  tools). API names occasionally shift between QEMU releases — if a symbol
  differs in your target tree (e.g. `virtio_init` arity, property macros),
  adjust to that release. It is written against the QEMU 9.x/10.x virtio
  device conventions.

## Why virtio-bluetooth (and not USB/UART)

`virtio_bt` is the modern, in-tree Linux transport and needs no guest-side
custom driver — the guest just needs `CONFIG_BT_VIRTIO=y` (or the
`virtio_bt` module). Emulating a USB `btusb` dongle or an H4 UART would
work too but is heavier for the same result. The medium and wire protocol
are transport-independent, so a USB/UART front-end could be added later
without touching `vbt_ll` or `vbt-medium`.
