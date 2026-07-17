# qemu-vbt — Virtual Bluetooth (BLE) Medium for QEMU

A virtual Bluetooth Low Energy medium for QEMU guests: a userspace
fan-out hub plus a QEMU virtual BLE controller device, so a fleet of
50–100 VMs can **advertise** BLE services, **discover** and **pair** with
each other, and **use** those services (GATT) — all over one simulated
2.4 GHz broadcast domain, with no real radios.

It is the Bluetooth sibling of [`qemu-vwifi`](../qemu-vwifi) (virtual
802.11) and [`qemu-ath9k`](../qemu-ath9k) (a QEMU WiFi device model), and
reuses their architecture: a **channel-aware medium hub** that only models
RF fan-out, while each node carries a full controller and the unmodified
host stack does the real work.

## Why this design works

On real silicon the split is: **controller** (Link Layer / baseband, in the
chip) ↔ HCI ↔ **host stack** (L2CAP, ATT/GATT, SMP pairing, in the OS).

qemu-vbt keeps that split intact:

```
   guest: bluetoothd / bluetoothctl / gatttool        ← unmodified BlueZ
        │  HCI
   guest: virtio_bt driver                            ← stock in-tree driver
        │  virtqueues
 ┌──────┴───────────────────────────────────────────┐
 │  QEMU  -device vbt-virtio  (per VM)               │
 │    • BLE controller: HCI + Link Layer             │
 │    • bridges LL PDUs to the medium socket         │
 └──────┬───────────────────────────────────────────┘
        │  Unix socket, length-prefixed wire protocol (vbt.h)
     vbt-medium            ← the shared BLE broadcast domain (this repo)
        │
   ┌────┼─────────┬──────────────┐
   │    │         │              │
 QEMU  QEMU      QEMU      vbt-controller
 VM-A  VM-B      VM-C     (host /dev/vhci bridge)
```

Because the medium only fans out Link-Layer PDUs and the **controller lives
in the device**, everything above HCI — service advertisement, GATT
discovery, and SMP pairing/bonding — runs end-to-end between two real
BlueZ stacks, exactly as it would over the air. The hub never needs to
understand L2CAP, ATT, or SMP.

## Components

| File / Binary | Description |
|---|---|
| `vbt.h` | Shared wire-protocol header (device + hub + controller) |
| `vbt_medium.c` → `vbt-medium` | Userspace medium hub: fans out advertising PDUs, routes connection data point-to-point (CONNECT_IND snoop), models path loss |
| `vbt_ll.c` / `vbt_ll.h` | Portable BLE controller core: HCI LE command engine + Link-Layer state machine, transport-agnostic |
| `vbt_controller.c` → `vbt-controller` | Host bridge: attaches the controller core to Linux `hci_vhci` (`/dev/vhci`) ↔ medium. Runnable on the host and inside VMs |
| `src/` | QEMU `vbt-virtio` device model (integrate into a QEMU tree) |
| `tests/harness.py` | Userspace regression harness for the hub |
| `scripts/` | Lab bring-up / inspection helpers |

## Building

### Userspace (hub + host controller)

No kernel headers or QEMU tree required:

```bash
make                 # builds vbt-medium and vbt-controller
make test            # runs tests/harness.py against ./vbt-medium
```

### QEMU device model

The `vbt-virtio` device is compiled as part of a QEMU source tree:

```bash
./scripts/integrate.sh /path/to/qemu
cd /path/to/qemu && mkdir -p build && cd build
../configure --target-list=x86_64-softmmu
make -j$(nproc)
```

See [`src/README.md`](src/README.md) for details and current verification
status.

## Usage

### 1. Start the hub

```bash
# Data socket only
./vbt-medium /tmp/vbt.sock

# With a runtime control socket (recommended)
./vbt-medium /tmp/vbt.sock -c /tmp/vbt.ctl

# Also accept inter-hub TCP bridges (multi-host)
./vbt-medium /tmp/vbt.sock -c /tmp/vbt.ctl -t 6550
```

```
Usage: ./vbt-medium <unix-socket-path> [options]
  -c <path>        Control socket path (runtime commands)
  -t <port>        TCP listen port for inter-hub bridges
  -u <host:port>   Connect to an upstream hub (repeatable)
  -C <path>        Initial config file (commands run at startup)
```

The data socket is mode 0666 (any user can attach a VM); the control
socket is 0600.

### 2a. Attach VMs (QEMU device)

```bash
qemu-system-x86_64 -machine q35 -m 512 \
  -drive file=vm.qcow2,format=qcow2 \
  -device vbt-virtio,medium=/tmp/vbt.sock,node_id=vm-a \
  -nographic
```

Inside the guest, the stock `virtio_bt` driver binds and BlueZ sees an
`hci0` controller:

```bash
# advertise a GATT service (peripheral)
sudo bluetoothctl
[bluetooth]# menu advertise
[bluetooth]# power on
[bluetooth]# advertise on
```

### 2b. Attach the host (vhci controller)

The host can join the same medium as a real BlueZ controller, no QEMU
needed:

```bash
sudo modprobe hci_vhci
sudo ./vbt-controller /tmp/vbt.sock --node-id host-a
# BlueZ now shows a new hciN; use bluetoothctl as usual
```

### 3. Discover, pair, use services

With one node advertising and another scanning, the flows are the normal
BlueZ ones — nothing bespoke:

```bash
# on a scanning/central node
bluetoothctl
[bluetooth]# scan on           # discover advertised devices
[bluetooth]# pair XX:XX:..     # SMP pairing runs end-to-end
[bluetooth]# connect XX:XX:..
gatttool -b XX:XX:.. --characteristics   # use GATT services
```

## Runtime control

When the hub is started with `-c <path>`, a text control socket accepts
commands. Connect with `socat` or `nc`:

```bash
echo LIST_PEERS | socat - UNIX-CONNECT:/tmp/vbt.ctl
```

| Command | Description |
|---|---|
| `LIST_PEERS` | Nodes: role, last channel, learned addresses, position, counters |
| `LIST_CONNS` | Active connections, keyed by Access Address |
| `SET_POS <node> <x> <y> [z]` | Position a node in metres |
| `SET_TXPOWER <node> <dBm>` | Node TX power |
| `SET_SENS <node> <dBm>` | Node RX sensitivity floor |
| `SET_RSSI <a> <b> <dBm>` | Pin a per-link RSSI |
| `SET_LOSS <a> <b> <prob>` | Pin a per-link loss probability (0..1) |
| `CLEAR_LINK <a> <b>` | Clear per-link overrides |
| `SET_PATHLOSS <exponent>` | Global log-distance path-loss exponent |
| `STATS` | Global counters |
| `SAVE_CONFIG <path>` | Snapshot positions/overrides to a file |
| `HELP` | Full command list |

## How the medium routes

* **Advertising PDUs** (`ll_type == VBT_LL_ADV`, Access Address
  `0x8E89BED6`) are broadcast to every other node. Each node's controller
  decides whether it is scanning and whether to surface an
  advertisement, so the hub need not track scan state. The propagation
  model can still drop an advertisement a distant node could not hear.

* **Connection setup.** When an initiator sends `CONNECT_IND` on the
  advertising channel, the hub snoops the PDU, extracts the new **Access
  Address** and the advertiser address, and records a connection linking
  the two endpoints. Device addresses are learned from advertising PDUs,
  so the hub knows which peer owns which address.

* **Connection data** (`ll_type == VBT_LL_DATA`) carries that Access
  Address; the hub routes each data PDU only to the *other* endpoint — a
  virtual point-to-point link. Channel hopping within the connection is
  handled by the endpoints and need not be enforced by the hub.

* **Propagation.** With node positions set (`SET_POS`), a log-distance
  path-loss model computes per-link RSSI and drops PDUs below the
  receiver's sensitivity floor. With no positions set, every node hears
  every other at a default strong RSSI — the right default for VMs on one
  host. Per-link `SET_RSSI` / `SET_LOSS` overrides pin specific links for
  fault-injection tests.

## Scaling to 50–100 VMs

The hub is a single-threaded `poll()` loop with per-peer output buffering
and backpressure accounting, sized (`MAX_PEERS`) well above 100 nodes.
Advertising is the dominant load: N advertisers beaconing at ~10 Hz to N
peers is O(N²) PDUs/s, comfortably within a userspace hub's budget at
N≈100. Connection data is point-to-point, so it scales linearly with the
number of active connections. Position-based path loss further prunes
fan-out once a topology is laid out.

## Status

* **BLE-first.** BR/EDR (Classic) PDU types are reserved in the wire
  protocol's `ll_type` namespace but not yet emitted or filtered.
* The **medium hub** and its routing/propagation logic are covered by
  `tests/harness.py`.
* See [`src/README.md`](src/README.md) for the QEMU device's build and
  verification status.

## Limitations

- BLE only (no BR/EDR Classic yet).
- No regulatory / real-RF modelling — this is a virtual medium.
- The hub must be running for PDUs to flow.
- The propagation model is a coarse log-distance estimate, not a
  MAC-accurate airtime/collision model.
