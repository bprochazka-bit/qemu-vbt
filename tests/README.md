# Tests

Two userspace test suites, both run by `make test`. Neither needs a
kernel, QEMU, or BlueZ.

## `test_ll.c` — controller core (`test-ll`)

Wires **two `vbt_ll` controller cores** together through an in-process
2-node medium (cross-delivery) and drives them over HCI exactly as a host
stack would, asserting on the HCI events each emits. This is the closest
thing to an end-to-end BLE session without real silicon:

1. HCI reset / init, `Read BD_ADDR`
2. peripheral advertises, central scans → **LE Advertising Report**
3. `LE Create Connection` → `CONNECT_IND` → **LE Connection Complete** on
   both sides, with correct roles
4. ACL data both directions, intact, with **Number Of Completed Packets**
5. link-layer encryption handshake → **Encryption Change** on both sides
6. `Disconnect` → **Disconnection Complete** on both sides

Because the same `vbt_ll` core powers the QEMU device and the
`vbt-controller` host bridge, this exercises the real controller code
paths those use.

## `harness.py` — medium hub

Spawns `./vbt-medium` on temporary sockets and drives mock nodes that
speak the wire protocol directly, covering routing and propagation:

* advertising PDUs fan out to every other node (and never echo to the
  sender)
* `CONNECT_IND` snooping builds a per-Access-Address connection, after
  which data PDUs route point-to-point (a third node sees nothing)
* a **promiscuous monitor** (`scripts/medium_dump.py`'s mode) does see that
  point-to-point connection data, and `STATS` reports it
* per-link `SET_LOSS` / `SET_RSSI` overrides and position-based path loss
  drop PDUs as expected
* the control socket reports peers, connections, and stats
* a 60-node fan-out smoke test

## Not covered here

The QEMU `virtio-bluetooth` transport glue (`src/vbt_virtio.c`) needs a
QEMU build + a guest to exercise; see [`../src/README.md`](../src/README.md).
Real SMP pairing and GATT are exercised by running two guests (or two
`vbt-controller` hosts) with BlueZ — they ride through as ACL data, which
the tests above confirm is carried intact.
