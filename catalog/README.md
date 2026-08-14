# BLE device catalog + fleet generator

`ble_device_catalog.json` is a catalog of BLE peripheral **profiles** — 25
generic archetypes (heart-rate strap, environmental sensor, smart lock,
iBeacon, …) and 25 specific devices (AirTag, RuuviTag, Mi Band, Dexcom
G7, …). [`scripts/ble_fleet.py`](../scripts/ble_fleet.py) reads it and
spins up profile-accurate simulated peripherals on the `vbt-medium` hub,
so a central (a guest's `virtio_bt hci0`, or a `vbt-controller`) sees a
populated, realistic BLE environment to scan, discover, connect to, and
read — the BLE analogue of qemu-vwifi's multi-node `medium_test`.

Each simulated peripheral is its own process by default (like a
pseudohost): independently killable, inspectable, and a distinct node on
the medium. `--in-process` instead runs the whole fleet in one asyncio
loop, which scales to hundreds of devices with far less memory.

## Quick start

```bash
./vbt-medium /tmp/vbt.sock -c /tmp/vbt.ctl            # hub

python3 scripts/ble_fleet.py --list                  # browse the catalog
python3 scripts/ble_fleet.py --scenario beacons      # iBeacon/Eddystone/Ruuvi/AirTag/…
python3 scripts/ble_fleet.py --generic --count 10    # 10 random generic devices
python3 scripts/ble_fleet.py --id generic_hr_chest_strap --id apple_airtag -v
python3 scripts/ble_fleet.py --all --in-process       # everything, one process
```

Watch it on the wire with [`scripts/medium_dump.py`](../scripts/medium_dump.py),
or point a real central at the medium and scan.

## Selection

| Flag | Selects |
|---|---|
| `--all` | every device in the catalog (default if nothing else is given) |
| `--generic` / `--specific` | one whole class |
| `--id ID` (repeatable) | specific devices by id |
| `--grep RE` | ids/labels matching a regex |
| `--scenario NAME` | a preset mix: `beacons`, `medical`, `security-ladder`, `stress`, `office` |
| `--count N` | a random N-sample of the current selection |
| `--dup K` | K instances of each selected device (fleet density) |
| `--seed S` | deterministic addresses/names/values |

## What the generator models

* **Advertising** — flags, local name (`{n}`/`{4hex}` tokens expanded),
  advertised 16-bit service UUIDs, appearance, and manufacturer/service
  data, packed into the 31-byte AD budget with overflow spilled into
  `SCAN_RSP`. Manufacturer-data encoders: **iBeacon**, **Eddystone-UID**
  (service data `0xFEAA`), **RuuviTag RAWv2** (format 5, big-endian, with
  a live sequence counter), an Apple offline-finding approximation, and a
  generic company payload with a rotating (correlatable) counter.
* **Address & privacy** — `public` (OUI-attributable), `random_static`,
  `rpa`, and `nrpa`, with address **rotation** on `privacy_rotation_s`. A
  few profiles deliberately leak a correlatable counter across rotations
  (Ruuvi sequence, generic counter), per the catalog's privacy notes.
* **GATT** — a database built from each profile's detailed services, plus
  synthesized standard services it lists as discoverable but doesn't spell
  out (GAP, GATT, DIS, Battery). Served over ATT: MTU exchange, primary
  service discovery (Read By Group Type), characteristic discovery (Read
  By Type), descriptor discovery (Find Information), reads, CCCD writes,
  and **notifications** — with per-characteristic value generators (heart
  rate at a per-heartbeat cadence, decaying battery, ESS temperature/
  humidity/pressure with their different scalings).
* **Security** — a device whose profile requires encryption
  (`min_level > 1`) returns **Insufficient Authentication** on protected
  characteristic reads. Realistic behavior; it just can't be *satisfied*
  here, because…

## What it does NOT model (and why)

* **SMP pairing / bonding.** There's no crypto: a pairing attempt is
  declined with `SMP Pairing Failed`. For real bonded pairing, attach a
  real BlueZ stack to the medium with `vbt-controller`.
* **Exact vendor payloads** for entries the catalog marks `medium`/`low`
  confidence (AirTag/AirPods/SmartTag/MiBeacon internal structures). These
  are plausible approximations; capture from real hardware if you need
  byte-exact vendor frames. Spec-defined 16-bit UUIDs are exact.
* **Extended advertising / Coded PHY / 2M PHY throughput.** The medium and
  sim speak legacy 1M advertising; the `stress` scenario's devices are
  placeholders for that future work, not faithful PHY models.
* **BR/EDR (Classic)** — out of scope for the whole project.

See the catalog's own `simulator_implementation_notes` and per-device
`nuances` fields for the behaviors that matter; the generator implements
the important ones (payload budget, privacy rotation, HR cadence, battery
decay, security gating) and documents the rest as future refinements.
