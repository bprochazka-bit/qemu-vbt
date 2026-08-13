#!/usr/bin/env python3
"""
sim_peripheral.py — a standalone simulated BLE peripheral on the vbt medium.

Attaches to a running vbt-medium hub as a node and behaves like a BLE
peripheral at the Link-Layer/L2CAP level: it advertises a connectable
device (name + service UUID), answers scan requests, accepts a connection,
and serves a tiny GATT database (a couple of services + readable
characteristics). This lets a real guest (its virtio_bt hci0) run
`scan on` → discover it → `connect` → browse/read services — no second VM.

Scope: discovery, connection, and *unauthenticated* GATT (read). It does
NOT implement SMP, so it can't complete bonded/encrypted pairing — for
that, attach a real BlueZ stack to the medium with vbt-controller (see
README). A pairing attempt is answered with SMP "Pairing Not Supported"
so the central fails cleanly instead of hanging.

Usage:
    python3 scripts/sim_peripheral.py /tmp/vbt.sock
    python3 scripts/sim_peripheral.py /tmp/vbt.sock --name Sensor --uuid 180f
    python3 scripts/sim_peripheral.py /tmp/vbt.sock --addr c0:ff:ee:00:00:01 -v
"""

import argparse
import json
import selectors
import socket
import struct
import sys
import time

# --- wire format (must match vbt.h) ---
VBT_MAGIC = 0x31544256
VBT_HELLO_MAGIC = 0x48544256
VBT_ADV_AA = 0x8E89BED6
HDR_FMT = '<I H H I I I I 6s B B B B b b H 6s'
HDR_SIZE = struct.calcsize(HDR_FMT)
assert HDR_SIZE == 44

LL_ADV, LL_DATA = 0x01, 0x02
ADV_IND, SCAN_REQ, SCAN_RSP, CONNECT_IND = 0x00, 0x03, 0x04, 0x05

# LLID (data PDU header byte0, low 2 bits)
LLID_CONT, LLID_START, LLID_CTRL = 0x01, 0x02, 0x03
LL_TERMINATE_IND, LL_ENC_REQ, LL_FEATURE_REQ, LL_FEATURE_RSP = 0x02, 0x03, 0x08, 0x09
LL_VERSION_IND, LL_SLAVE_FEATURE_REQ = 0x0C, 0x0E

# ATT opcodes
ATT_ERROR = 0x01
ATT_EXCHANGE_MTU_REQ, ATT_EXCHANGE_MTU_RSP = 0x02, 0x03
ATT_FIND_INFO_REQ, ATT_FIND_INFO_RSP = 0x04, 0x05
ATT_READ_BY_TYPE_REQ, ATT_READ_BY_TYPE_RSP = 0x08, 0x09
ATT_READ_REQ, ATT_READ_RSP = 0x0A, 0x0B
ATT_READ_BY_GROUP_REQ, ATT_READ_BY_GROUP_RSP = 0x10, 0x11
ATT_WRITE_REQ, ATT_WRITE_RSP = 0x12, 0x13
ATT_HANDLE_VALUE_NTF = 0x1B
# ATT error codes
ERR_INVALID_HANDLE, ERR_READ_NOT_PERM, ERR_REQ_NOT_SUPP, ERR_ATTR_NOT_FOUND = \
    0x01, 0x02, 0x06, 0x0A

CID_ATT, CID_SMP = 0x0004, 0x0006
UUID_PRIMARY_SERVICE = 0x2800
UUID_CHARACTERISTIC = 0x2803


def le16(v): return struct.pack('<H', v)
def mac(b): return ":".join(f"{x:02x}" for x in b[::-1])


def make_hello(node_id):
    p = struct.pack('<I', VBT_HELLO_MAGIC) + node_id.encode() + b'\x00'
    return struct.pack('!I', len(p)) + p


def make_frame(tx_addr, pdu, *, ll_type, access_addr, channel):
    hdr = struct.pack(HDR_FMT, VBT_MAGIC, 1, len(pdu), access_addr, 0, 0, 0,
                      tx_addr, 0, ll_type, channel, 1, 0, -40, 0, b'\x00' * 6)
    wire = hdr + pdu
    return struct.pack('!I', len(wire)) + wire


class GattDB:
    """A tiny GATT database: GAP (device name) + Battery (level)."""
    def __init__(self, name: str, svc_uuid: int):
        self.name = name.encode()[:20]
        # (handle, att_type_uuid16, value_bytes)
        self.attrs = {}
        self.services = []   # (start, end, uuid16)

        # GAP service 0x1800: Device Name char 0x2A00 @ 0x0003
        self._add_service(0x0001, 0x0003, 0x1800, [
            (0x0002, UUID_CHARACTERISTIC,
             bytes([0x02]) + le16(0x0003) + le16(0x2A00)),   # read
            (0x0003, 0x2A00, self.name),
        ])
        # A user service (default Battery 0x180F): level char 0x2A19 @ 0x0006
        self._add_service(0x0004, 0x0006, svc_uuid, [
            (0x0005, UUID_CHARACTERISTIC,
             bytes([0x02]) + le16(0x0006) + le16(0x2A19)),   # read
            (0x0006, 0x2A19, bytes([0x64])),                 # 100%
        ])

    def _add_service(self, start, end, uuid16, members):
        self.attrs[start] = (UUID_PRIMARY_SERVICE, le16(uuid16))
        for h, t, v in members:
            self.attrs[h] = (t, v)
        self.services.append((start, end, uuid16))


def build_att_response(req: bytes, db: GattDB) -> bytes:
    """Return the ATT response PDU for a request, or b'' to stay silent."""
    if not req:
        return b''
    op = req[0]

    def err(o, h, e):
        return bytes([ATT_ERROR, o]) + le16(h) + bytes([e])

    if op == ATT_EXCHANGE_MTU_REQ:
        return bytes([ATT_EXCHANGE_MTU_RSP]) + le16(23)

    if op == ATT_READ_BY_GROUP_REQ and len(req) >= 7:
        start, end, gtype = struct.unpack('<HHH', req[1:7])
        if gtype != UUID_PRIMARY_SERVICE:
            return err(op, start, ERR_REQ_NOT_SUPP)
        entries = []
        for s, e, u in db.services:
            if s >= start and s <= end:
                entries.append(le16(s) + le16(e) + le16(u))
        if not entries:
            return err(op, start, ERR_ATTR_NOT_FOUND)
        each = len(entries[0])   # handle(2) + end-group(2) + uuid(2) = 6
        body = b''.join(x for x in entries if len(x) == each)
        return bytes([ATT_READ_BY_GROUP_RSP, each]) + body

    if op == ATT_READ_BY_TYPE_REQ and len(req) >= 7:
        start, end, atype = struct.unpack('<HHH', req[1:7])
        entries = []
        for h in sorted(db.attrs):
            if h < start or h > end:
                continue
            t, v = db.attrs[h]
            if t == atype:
                entries.append(le16(h) + v)
        if not entries:
            return err(op, start, ERR_ATTR_NOT_FOUND)
        each = len(entries[0])
        body = b''.join(x for x in entries if len(x) == each)
        return bytes([ATT_READ_BY_TYPE_RSP, each]) + body

    if op == ATT_READ_REQ and len(req) >= 3:
        (h,) = struct.unpack('<H', req[1:3])
        if h not in db.attrs:
            return err(op, h, ERR_INVALID_HANDLE)
        return bytes([ATT_READ_RSP]) + db.attrs[h][1]

    if op == ATT_FIND_INFO_REQ and len(req) >= 5:
        start, _end = struct.unpack('<HH', req[1:5])
        return err(op, start, ERR_ATTR_NOT_FOUND)   # no descriptors

    if op == ATT_WRITE_REQ and len(req) >= 3:
        (h,) = struct.unpack('<H', req[1:3])
        if h in db.attrs:
            return bytes([ATT_WRITE_RSP])
        return err(op, h, ERR_INVALID_HANDLE)

    return err(op, 0x0000, ERR_REQ_NOT_SUPP)


class SimPeripheral:
    def __init__(self, args):
        self.args = args
        self.addr = args.addr
        self.name = args.name
        self.db = GattDB(args.name, args.uuid)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(args.socket)
        self.sock.sendall(make_hello(args.node_id))
        self.sock.setblocking(False)
        self.rx = b''
        self.connected = False
        self.conn_aa = None
        self.last_adv = 0.0
        self.sel = selectors.DefaultSelector()
        self.sel.register(self.sock, selectors.EVENT_READ)

    def log(self, *a):
        if self.args.verbose or a and a[0].startswith('*'):
            print('sim:', *a, flush=True)

    # --- advertising ---
    def adv_pdu(self):
        ad = bytes([0x02, 0x01, 0x06])                  # Flags: LE General
        ad += bytes([1 + len(self.name), 0x09]) + self.name.encode()  # Name
        ad += bytes([0x03, 0x03]) + le16(self.args.uuid)  # 16-bit svc UUID
        body = self.addr + ad
        return bytes([ADV_IND, len(body)]) + body

    def send(self, pdu, *, ll_type, aa, channel):
        try:
            self.sock.sendall(make_frame(self.addr, pdu, ll_type=ll_type,
                                         access_addr=aa, channel=channel))
        except OSError as e:
            self.log('send failed:', e)

    def advertise(self):
        for ch in (37, 38, 39):
            self.send(self.adv_pdu(), ll_type=LL_ADV, aa=VBT_ADV_AA, channel=ch)

    def send_scan_rsp(self):
        body = self.addr + bytes([1 + len(self.name), 0x09]) + self.name.encode()
        pdu = bytes([SCAN_RSP, len(body)]) + body
        self.send(pdu, ll_type=LL_ADV, aa=VBT_ADV_AA, channel=37)

    # --- connection data ---
    def send_l2cap(self, cid, payload):
        l2 = struct.pack('<HH', len(payload), cid) + payload
        pdu = bytes([LLID_START, len(l2)]) + l2
        self.send(pdu, ll_type=LL_DATA, aa=self.conn_aa, channel=5)

    def send_ll_ctrl(self, opcode, params=b''):
        pdu = bytes([LLID_CTRL, 1 + len(params), opcode]) + params
        self.send(pdu, ll_type=LL_DATA, aa=self.conn_aa, channel=5)

    # --- receive path ---
    def on_adv(self, pdu):
        if len(pdu) < 2:
            return
        ptype = pdu[0] & 0x0F
        body = pdu[2:2 + pdu[1]]
        if ptype == SCAN_REQ and len(body) >= 12:
            if body[6:12] == self.addr:           # AdvA targets us
                self.send_scan_rsp()
        elif ptype == CONNECT_IND and len(body) >= 34:
            adva = body[6:12]
            if adva == self.addr and not self.connected:
                self.conn_aa = struct.unpack('<I', body[12:16])[0]
                self.connected = True
                self.log('* connected by', mac(body[0:6]),
                         f'(AA=0x{self.conn_aa:08x})')

    def on_data(self, aa, pdu):
        if aa != self.conn_aa or len(pdu) < 2:
            return
        llid = pdu[0] & 0x03
        body = pdu[2:2 + pdu[1]]
        if llid == LLID_CTRL and body:
            op = body[0]
            if op == LL_TERMINATE_IND:
                self.log('* disconnected'); self.connected = False; self.conn_aa = None
            elif op == LL_VERSION_IND:
                self.send_ll_ctrl(LL_VERSION_IND, bytes([0x0C]) + le16(0xFFFF) + le16(0))
            elif op in (LL_FEATURE_REQ, LL_SLAVE_FEATURE_REQ):
                self.send_ll_ctrl(LL_FEATURE_RSP, bytes([0x01, 0, 0, 0, 0, 0, 0, 0]))
            return
        if llid in (LLID_START, LLID_CONT) and len(body) >= 4:
            plen, cid = struct.unpack('<HH', body[0:4])
            payload = body[4:4 + plen]
            if cid == CID_ATT:
                rsp = build_att_response(payload, self.db)
                if rsp:
                    self.log('ATT', f'op=0x{payload[0]:02x} -> 0x{rsp[0]:02x}')
                    self.send_l2cap(CID_ATT, rsp)
            elif cid == CID_SMP and payload:
                # Decline pairing cleanly: SMP Pairing Failed, reason 0x05.
                self.log('* SMP pairing requested — declining (not supported)')
                self.send_l2cap(CID_SMP, bytes([0x05, 0x05]))

    def pump(self):
        try:
            data = self.sock.recv(65536)
        except (BlockingIOError, OSError):
            return True
        if not data:
            print('sim: medium closed', file=sys.stderr)
            return False
        self.rx += data
        while len(self.rx) >= 4:
            (plen,) = struct.unpack('!I', self.rx[:4])
            if plen > HDR_SIZE + 512:
                self.rx = b''
                break
            if len(self.rx) < 4 + plen:
                break
            msg, self.rx = self.rx[4:4 + plen], self.rx[4 + plen:]
            if plen < HDR_SIZE:
                continue
            f = struct.unpack_from(HDR_FMT, msg, 0)
            magic, _v, pdu_len, aa, _a, _b, _c, tx, _at, ll_type = f[:10]
            if magic != VBT_MAGIC:
                continue
            pdu = msg[HDR_SIZE:HDR_SIZE + pdu_len]
            if ll_type == LL_ADV:
                self.on_adv(pdu)
            elif ll_type == LL_DATA:
                self.on_data(aa, pdu)
        return True

    def run(self):
        print(f"sim: peripheral '{self.name}' {mac(self.addr)} "
              f"svc=0x{self.args.uuid:04x} on {self.args.socket} "
              f"(Ctrl-C to stop)", flush=True)
        try:
            while True:
                now = time.time()
                if not self.connected and now - self.last_adv >= 0.1:
                    self.advertise()
                    self.last_adv = now
                events = self.sel.select(timeout=0.05)
                if events:
                    if not self.pump():
                        break
        except KeyboardInterrupt:
            print('\nsim: stopping', flush=True)
        finally:
            self.sock.close()


def parse_addr(s):
    parts = s.split(':')
    if len(parts) != 6:
        raise argparse.ArgumentTypeError('address must be xx:xx:xx:xx:xx:xx')
    return bytes(int(p, 16) for p in reversed(parts))   # store LSB-first


def build_parser():
    ap = argparse.ArgumentParser(description="Simulated BLE peripheral on the vbt medium")
    ap.add_argument('--dump-config', action='store_true',
                    help='print a JSON descriptor of this device type\'s '
                         'configurable parameters and exit (used by tools '
                         'that build a UI or validate configs)')
    ap.add_argument('socket', help='path to the vbt-medium data socket')
    ap.add_argument('--name', default='SimPeri', help='advertised device name')
    ap.add_argument('--addr', type=parse_addr, default=parse_addr('c0:ff:ee:00:00:01'),
                    help='device address (default c0:ff:ee:00:00:01)')
    ap.add_argument('--uuid', type=lambda s: int(s, 16), default=0x180F,
                    help='advertised 16-bit service UUID (hex, default 180f Battery)')
    ap.add_argument('--node-id', default='sim-peri', help='hub node identity')
    ap.add_argument('-v', '--verbose', action='store_true')
    return ap


# Human-readable defaults for the two arguments whose parsed default is not
# a plain string (an address is stored LSB-first bytes; the UUID as an int).
# The config descriptor should show what a user would actually type.
_DESCRIPTOR_DEFAULTS = {'addr': 'c0:ff:ee:00:00:01', 'uuid': '180f'}
_DESCRIPTOR_TYPES = {'addr': 'str', 'uuid': 'str'}


def _param(action):
    """One parameter descriptor, matching the vwifi launchers' shape so a
    consumer parses a single schema for every device type."""
    if isinstance(action, argparse._StoreTrueAction):
        ptype, default = 'bool', bool(action.default)
    else:
        ptype = _DESCRIPTOR_TYPES.get(action.dest, 'str')
        if action.type is int:
            ptype = 'int'
        default = _DESCRIPTOR_DEFAULTS.get(action.dest, action.default)
        if not isinstance(default, (str, int, float, bool, type(None))):
            default = str(default)
    flags = list(action.option_strings)
    return {
        'name': action.dest,
        'flags': flags,
        'positional': not flags,
        'type': ptype,
        'required': bool(action.required) or not flags,
        'default': default,
        'choices': list(action.choices) if action.choices else None,
        'help': action.help or '',
        'metavar': action.metavar if isinstance(action.metavar, str) else None,
    }


def dump_config(parser):
    params = [_param(a) for a in parser._actions
              if not isinstance(a, argparse._HelpAction)
              and a.dest not in ('help', 'dump_config')]
    return json.dumps({
        'device_type': 'ble-peripheral',
        'binary': 'vbt-sim-peripheral',
        'description': (parser.description or '').strip(),
        'params': params,
        'extras': {},
        # No cross-field rules: every parameter is independent here.
        'constraints': [],
    }, indent=2)


def main():
    parser = build_parser()
    # --dump-config short-circuits argument validation: a caller asking for
    # the config descriptor has not supplied the socket, and shouldn't need to.
    if '--dump-config' in sys.argv[1:]:
        print(dump_config(parser))
        return
    args = parser.parse_args()
    try:
        SimPeripheral(args).run()
    except OSError as e:
        print(f"sim: cannot connect to {args.socket}: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
