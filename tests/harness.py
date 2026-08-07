#!/usr/bin/env python3
"""
Userspace regression harness for vbt-medium (Virtual Bluetooth medium).

Spawns ./vbt-medium on temporary sockets and exercises the routing and
propagation logic that can be checked without a QEMU VM or a kernel:

  * advertising PDUs fan out to every other node (discovery)
  * a scanner does not receive its own advertisement (own-frame is simply
    never echoed to the sender)
  * CONNECT_IND snooping builds a per-Access-Address connection, after
    which data PDUs route point-to-point (the third node does NOT see them)
  * per-link RSSI / loss overrides and position-based path loss drop PDUs
  * the control socket reports peers, connections and stats

Usage:
    make userspace
    python3 tests/harness.py        # or: make test

Exit code: 0 = all pass, 1 = at least one failure, 2 = setup error.
"""

import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------
# Wire format (must match vbt.h)
# ---------------------------------------------------------------------
VBT_MAGIC = 0x31544256          # "VBT1"
VBT_HELLO_MAGIC = 0x48544256    # "VBTH"
VBT_ADV_AA = 0x8E89BED6
HELLO_FLAG_PHYSICAL = 0x01

HELLO_FLAG_MONITOR = 0x02

LL_ADV = 0x01
LL_DATA = 0x02

PHY_1M = 1

ADV_IND = 0x00
CONNECT_IND = 0x05

HDR_FMT = '<I H H I I I I 6s B B B B b b H 6s'   # 44 bytes
HDR_SIZE = struct.calcsize(HDR_FMT)
assert HDR_SIZE == 44, HDR_SIZE


def make_hello(node_id: str, physical: bool = False,
               monitor: bool = False) -> bytes:
    payload = struct.pack('<I', VBT_HELLO_MAGIC) + node_id.encode() + b'\x00'
    flags = 0
    if physical:
        flags |= HELLO_FLAG_PHYSICAL
    if monitor:
        flags |= HELLO_FLAG_MONITOR
    if flags:
        payload += bytes([flags])
    return struct.pack('!I', len(payload)) + payload


def make_frame(tx_addr: bytes, pdu: bytes, *, ll_type=LL_ADV,
               access_addr=VBT_ADV_AA, channel=37, phy=PHY_1M,
               tx_power=0, addr_type=0, conn_handle=0) -> bytes:
    assert len(tx_addr) == 6
    hdr = struct.pack(
        HDR_FMT,
        VBT_MAGIC, 1, len(pdu),
        access_addr, 0, 0, 0,           # access_addr, tsf_lo, tsf_hi, flags
        tx_addr, addr_type, ll_type, channel, phy,
        tx_power, 0, conn_handle, b'\x00' * 6,
    )
    wire = hdr + pdu
    return struct.pack('!I', len(wire)) + wire


def adv_ind_pdu(adva: bytes, adv_data: bytes = b'') -> bytes:
    body = adva + adv_data
    return bytes([ADV_IND, len(body)]) + body


def connect_ind_pdu(inita: bytes, adva: bytes, access_addr: int) -> bytes:
    lldata = struct.pack('<I', access_addr) + b'\x00' * 18   # AA + 18B params
    body = inita + adva + lldata                              # 6+6+22 = 34
    return bytes([CONNECT_IND, len(body)]) + body


def parse_msgs(buf: bytes):
    """Yield (hdr_tuple, pdu, rssi) for each complete message in buf; return
    the unconsumed tail."""
    out = []
    off = 0
    while len(buf) - off >= 4:
        (plen,) = struct.unpack_from('!I', buf, off)
        if len(buf) - off - 4 < plen:
            break
        payload = buf[off + 4:off + 4 + plen]
        off += 4 + plen
        if plen >= HDR_SIZE:
            fields = struct.unpack_from(HDR_FMT, payload, 0)
            pdu = payload[HDR_SIZE:]
            rssi = fields[13]
            out.append((fields, pdu, rssi))
    return out, buf[off:]


class Node:
    """A mock BLE controller speaking the wire protocol directly."""
    def __init__(self, harness, node_id, addr, physical=False, monitor=False):
        self.node_id = node_id
        self.addr = addr
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(str(harness.sock))
        self.sock.sendall(make_hello(node_id, physical, monitor))
        self.rx = b''
        time.sleep(0.02)

    def send(self, wire: bytes):
        self.sock.sendall(wire)

    def recv_msgs(self, settle=0.15):
        time.sleep(settle)
        self.sock.setblocking(False)
        try:
            while True:
                d = self.sock.recv(65536)
                if not d:
                    break
                self.rx += d
        except (BlockingIOError, OSError):
            pass
        self.sock.setblocking(True)
        msgs, self.rx = parse_msgs(self.rx)
        return msgs

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class Harness:
    def __init__(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='vbt-test-'))
        self.sock = self.tmp / 'medium.sock'
        self.ctl = self.tmp / 'medium.ctl'
        self.log = self.tmp / 'hub.log'
        self.hub = None
        self.passed = 0
        self.failed = 0
        self.failures = []

    def start_hub(self):
        binary = REPO_ROOT / 'vbt-medium'
        if not binary.exists():
            sys.exit(f'FATAL: {binary} not built. Run "make userspace" first.')
        cmd = [str(binary), str(self.sock), '-c', str(self.ctl)]
        self.hub = subprocess.Popen(cmd, stdout=open(self.log, 'w'),
                                    stderr=subprocess.STDOUT, cwd=str(self.tmp))
        deadline = time.time() + 2.0
        while time.time() < deadline:
            if self.sock.exists() and self.ctl.exists():
                time.sleep(0.05)
                return
            if self.hub.poll() is not None:
                sys.exit(f'FATAL: hub exited early:\n{self.log.read_text()}')
            time.sleep(0.02)
        sys.exit(f'FATAL: hub did not bind in 2s:\n{self.log.read_text()}')

    def ctl_cmd(self, cmd: str) -> str:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect(str(self.ctl))
        s.sendall((cmd + '\n').encode())
        chunks = []
        s.settimeout(0.3)
        try:
            while True:
                d = s.recv(8192)
                if not d:
                    break
                chunks.append(d)
        except socket.timeout:
            pass
        finally:
            s.close()
        return b''.join(chunks).decode(errors='replace')

    def cleanup(self):
        if self.hub and self.hub.poll() is None:
            self.hub.terminate()
            try:
                self.hub.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.hub.kill()
                self.hub.wait()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def check(self, cond, what):
        if cond:
            self.passed += 1
            print(f'  PASS: {what}')
        else:
            self.failed += 1
            self.failures.append(what)
            print(f'  FAIL: {what}')


def has_adv_from(msgs, adva: bytes) -> bool:
    # HDR_FMT field order: ll_type is index 9, access_addr index 3.
    for fields, pdu, _rssi in msgs:
        if fields[9] == LL_ADV and len(pdu) >= 8 and pdu[2:8] == adva:
            return True
    return False


def count_data(msgs, aa: int) -> int:
    n = 0
    for fields, pdu, _rssi in msgs:
        # ll_type is field index 9; access_addr is field index 3
        if fields[9] == LL_DATA and fields[3] == aa:
            n += 1
    return n


def run():
    h = Harness()
    try:
        h.start_hub()
        A = b'\x11\x11\x11\x11\x11\x11'
        B = b'\x22\x22\x22\x22\x22\x22'
        C = b'\x33\x33\x33\x33\x33\x33'

        # --- Discovery: adv fans out to others but not the sender ---
        print('[1] advertising / discovery')
        na = Node(h, 'peri-a', A)
        nb = Node(h, 'scan-b', B)
        nc = Node(h, 'scan-c', C)
        na.send(make_frame(A, adv_ind_pdu(A, b'\x02\x01\x06')))
        mb = nb.recv_msgs()
        mc = nc.recv_msgs()
        ma = na.recv_msgs()
        h.check(has_adv_from(mb, A), 'scanner B hears A\'s ADV_IND')
        h.check(has_adv_from(mc, A), 'scanner C hears A\'s ADV_IND')
        h.check(not has_adv_from(ma, A), 'advertiser A does not hear its own ADV_IND')

        # --- Connection: CONNECT_IND builds a point-to-point route ---
        print('[2] connection routing (CONNECT_IND snoop)')
        AA = 0xAF9E1234
        # C initiates a connection to A (A is the advertiser)
        nc.send(make_frame(C, connect_ind_pdu(C, A, AA)))
        time.sleep(0.1)
        # Now A and C exchange data PDUs on AA; B must not see them.
        na.recv_msgs(); nb.recv_msgs(); nc.recv_msgs()   # drain
        na.send(make_frame(A, b'\xAA' * 20, ll_type=LL_DATA,
                           access_addr=AA, channel=5))
        nc.send(make_frame(C, b'\xBB' * 20, ll_type=LL_DATA,
                           access_addr=AA, channel=5))
        mc = nc.recv_msgs()
        mb = nb.recv_msgs()
        ma = na.recv_msgs()
        h.check(count_data(mc, AA) >= 1, 'C receives A\'s data PDU on the connection')
        h.check(count_data(ma, AA) >= 1, 'A receives C\'s data PDU on the connection')
        h.check(count_data(mb, AA) == 0, 'uninvolved node B sees no connection data')

        out = h.ctl_cmd('LIST_CONNS')
        h.check(f'0x{AA:08x}' in out, 'LIST_CONNS reports the connection')

        # --- Promiscuous monitor sees connection data (medium_dump path) ---
        print('[2b] promiscuous monitor tap')
        mon = Node(h, 'dump', b'\x99\x99\x99\x99\x99\x99', monitor=True)
        mon.recv_msgs()   # drain
        na.send(make_frame(A, b'\xCC' * 16, ll_type=LL_DATA,
                           access_addr=AA, channel=9))
        na.send(make_frame(A, adv_ind_pdu(A)))
        mm = mon.recv_msgs()
        h.check(count_data(mm, AA) >= 1,
                'monitor sees point-to-point connection data')
        h.check(has_adv_from(mm, A), 'monitor also sees advertising')
        out = h.ctl_cmd('STATS')
        h.check('monitors=1' in out, 'STATS reports the monitor')
        mon.close()

        # --- Propagation: per-link loss=1.0 blocks discovery ---
        print('[3] propagation model')
        h.ctl_cmd('SET_LOSS peri-a scan-b 1.0')
        nb.recv_msgs()
        na.send(make_frame(A, adv_ind_pdu(A)))
        mb = nb.recv_msgs()
        mc = nc.recv_msgs()
        h.check(not has_adv_from(mb, A), 'loss=1.0 blocks A->B advertisement')
        h.check(has_adv_from(mc, A), 'C still hears A (loss override is per-link)')
        h.ctl_cmd('CLEAR_LINK peri-a scan-b')

        # --- Position-based path loss: far apart -> dropped ---
        print('[4] position-based path loss')
        h.ctl_cmd('SET_TXPOWER peri-a 0')
        h.ctl_cmd('SET_POS peri-a 0 0 0')
        h.ctl_cmd('SET_POS scan-c 100000 0 0')   # 100 km -> way below sensitivity
        h.ctl_cmd('SET_POS scan-b 1 0 0')        # 1 m -> audible
        nc.recv_msgs(); nb.recv_msgs()
        na.send(make_frame(A, adv_ind_pdu(A)))
        mc = nc.recv_msgs()
        mb = nb.recv_msgs()
        h.check(not has_adv_from(mc, A), 'distant node C drops A\'s advertisement')
        h.check(has_adv_from(mb, A), 'nearby node B still hears A')

        out = h.ctl_cmd('STATS')
        h.check(out.startswith('OK'), 'STATS responds OK')
        out = h.ctl_cmd('LIST_PEERS')
        h.check('peri-a' in out and 'scan-b' in out, 'LIST_PEERS lists nodes')

        na.close(); nb.close(); nc.close()

        # --- Scale smoke test: many nodes advertise at once ---
        print('[5] scale smoke test (60 nodes)')
        many = []
        for i in range(60):
            addr = bytes([0x40, 0, 0, 0, (i >> 8) & 0xff, i & 0xff])
            many.append(Node(h, f'n{i}', addr))
        # one advertiser, everyone else should hear it
        many[0].send(make_frame(many[0].addr, adv_ind_pdu(many[0].addr)))
        heard = 0
        for n in many[1:]:
            if has_adv_from(n.recv_msgs(settle=0.02), many[0].addr):
                heard += 1
        h.check(heard == 59, f'all 59 peers heard the advertiser (got {heard})')
        for n in many:
            n.close()

    finally:
        h.cleanup()

    print()
    print(f'RESULTS: {h.passed} passed, {h.failed} failed')
    if h.failed:
        for f in h.failures:
            print(f'  - {f}')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(run())
