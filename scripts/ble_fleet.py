#!/usr/bin/env python3
"""
ble_fleet.py — catalog-driven fleet of simulated BLE peripherals on the medium.

The BLE analogue of qemu-vwifi's multi-node medium_test: instead of one
hard-coded peripheral (scripts/sim_peripheral.py), this reads a JSON device
catalog and spins up many profile-accurate peripherals at once — each a
distinct node on the vbt-medium hub — so a real central (a guest's virtio_bt
hci0, or another vbt-controller) sees a populated, realistic BLE environment
to scan, discover, connect to, and read.

Each simulated device is built from its catalog profile:
  * advertising payload  — flags, name ({n}/{4hex} tokens), 16-bit service
    UUIDs, service data, appearance, TX power, and manufacturer data
    (iBeacon, Eddystone, RuuviTag RAWv2, and a generic company payload),
    packed into the 31-byte AD budget with overflow spilled to SCAN_RSP
  * address + privacy    — public / random-static / RPA / NRPA, with
    rotation on schedule (and a correlatable counter left in the payload
    where the profile leaks one)
  * GATT database        — services + characteristics with realistic,
    dynamic values (HR per-heartbeat, battery decay, ESS scaling, ...),
    served over ATT (discovery + read), with security-gated reads
  * state machine        — advertising <-> connected

Everything above HCI that needs real crypto (SMP bonding) is out of scope,
same as sim_peripheral.py: a device that requires encryption returns
"Insufficient Authentication" on protected reads (realistic), and pairing
is declined. For real bonded pairing, put a BlueZ stack on the medium with
vbt-controller.

Usage:
  python3 scripts/ble_fleet.py --list
  python3 scripts/ble_fleet.py --all
  python3 scripts/ble_fleet.py --generic --count 10
  python3 scripts/ble_fleet.py --id generic_hr_chest_strap --id apple_airtag
  python3 scripts/ble_fleet.py --scenario security-ladder -v
  python3 scripts/ble_fleet.py --grep 'lock|hue' --medium /tmp/vbt.sock
"""

import argparse
import asyncio
import json
import os
import random
import signal
import struct
import subprocess
import sys
import time

# ============================================================
#  Wire protocol (must match vbt.h)
# ============================================================
VBT_MAGIC = 0x31544256
VBT_HELLO_MAGIC = 0x48544256
VBT_ADV_AA = 0x8E89BED6
HDR_FMT = '<I H H I I I I 6s B B B B b b H 6s'
HDR_SIZE = struct.calcsize(HDR_FMT)
assert HDR_SIZE == 44

LL_ADV, LL_DATA = 0x01, 0x02
ADV_IND, ADV_DIRECT, ADV_NONCONN, SCAN_REQ, SCAN_RSP, CONNECT_IND, ADV_SCAN_IND = \
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06

LLID_CONT, LLID_START, LLID_CTRL = 0x01, 0x02, 0x03
LL_TERMINATE_IND, LL_VERSION_IND, LL_FEATURE_REQ, LL_FEATURE_RSP, LL_SLAVE_FEATURE_REQ = \
    0x02, 0x0C, 0x08, 0x09, 0x0E

# ATT
ATT_ERROR = 0x01
ATT_MTU_REQ, ATT_MTU_RSP = 0x02, 0x03
ATT_FIND_INFO_REQ, ATT_FIND_INFO_RSP = 0x04, 0x05
ATT_FIND_BY_TYPE_REQ = 0x06
ATT_READ_BY_TYPE_REQ, ATT_READ_BY_TYPE_RSP = 0x08, 0x09
ATT_READ_REQ, ATT_READ_RSP = 0x0A, 0x0B
ATT_READ_BY_GROUP_REQ, ATT_READ_BY_GROUP_RSP = 0x10, 0x11
ATT_WRITE_REQ, ATT_WRITE_RSP = 0x12, 0x13
ATT_WRITE_CMD = 0x52
ATT_HANDLE_VALUE_NTF, ATT_HANDLE_VALUE_IND = 0x1B, 0x1D
ERR_INVALID_HANDLE, ERR_READ_NOT_PERM, ERR_REQ_NOT_SUPP = 0x01, 0x02, 0x06
ERR_INSUFF_AUTH, ERR_ATTR_NOT_FOUND, ERR_UNLIKELY = 0x05, 0x0A, 0x0E

CID_ATT, CID_SMP = 0x0004, 0x0006
UUID_PRIMARY_SVC, UUID_CHARACTERISTIC, UUID_CCCD = 0x2800, 0x2803, 0x2902

PROP_READ, PROP_WRITE_NR, PROP_WRITE, PROP_NOTIFY, PROP_INDICATE = \
    0x02, 0x04, 0x08, 0x10, 0x20


def le16(v): return struct.pack('<H', v & 0xffff)
def le32(v): return struct.pack('<I', v & 0xffffffff)
def be16(v): return struct.pack('>H', v & 0xffff)
def mac_str(b): return ":".join(f"{x:02x}" for x in b[::-1])


def make_hello(node_id):
    p = struct.pack('<I', VBT_HELLO_MAGIC) + node_id.encode()[:40] + b'\x00'
    return struct.pack('!I', len(p)) + p


def make_frame(tx_addr, tx_type, pdu, *, ll_type, aa, channel):
    hdr = struct.pack(HDR_FMT, VBT_MAGIC, 1, len(pdu), aa, 0, 0, 0,
                      tx_addr, tx_type & 1, ll_type, channel, 1, 0, -50, 0,
                      b'\x00' * 6)
    wire = hdr + pdu
    return struct.pack('!I', len(wire)) + wire


# ============================================================
#  AD structure packing (31-byte budget, overflow to SCAN_RSP)
# ============================================================
def ad(t, data):
    return bytes([len(data) + 1, t]) + data


def parse_uuid16(u):
    """Return an int for a '0xXXXX' 16-bit UUID, or None otherwise."""
    if isinstance(u, int):
        return u
    if isinstance(u, str) and u.lower().startswith('0x') and len(u) <= 6:
        try:
            return int(u, 16)
        except ValueError:
            return None
    return None


def pack_ads(primary, extra, scannable):
    """primary AD structs go in the adv packet; extra spill to adv then
    (if scannable) scan_rsp; anything that still won't fit is dropped."""
    adv = b''
    for a in primary:
        if len(adv) + len(a) <= 31:
            adv += a
    rsp = b''
    dropped = 0
    for a in extra:
        if len(adv) + len(a) <= 31:
            adv += a
        elif scannable and len(rsp) + len(a) <= 31:
            rsp += a
        else:
            dropped += 1
    return adv, rsp, dropped


# ============================================================
#  Manufacturer-data / service-data encoders
# ============================================================
def enc_ibeacon(dev, now):
    # company 0x004C, then 02 15 <uuid16b> <major BE> <minor BE> <power>
    uuid = dev.state.setdefault('ibeacon_uuid',
                                bytes(dev.rng.randrange(256) for _ in range(16)))
    major = dev.state.setdefault('ibeacon_major', dev.rng.randrange(1, 65535))
    minor = dev.state.setdefault('ibeacon_minor', dev.rng.randrange(1, 65535))
    payload = bytes([0x02, 0x15]) + uuid + be16(major) + be16(minor) + \
        struct.pack('b', -59)
    return ad(0xFF, le16(0x004C) + payload)


def enc_eddystone_uid(dev, now):
    # Service Data 0xFEAA: frame 0x00 (UID): txpower, 10B namespace, 6B instance
    ns = dev.state.setdefault('eddy_ns', bytes(dev.rng.randrange(256) for _ in range(10)))
    inst = dev.state.setdefault('eddy_inst', bytes(dev.rng.randrange(256) for _ in range(6)))
    frame = bytes([0x00, 0xEE]) + ns + inst + b'\x00\x00'
    return ad(0x16, le16(0xFEAA) + frame)


def enc_ruuvi(dev, now):
    # RAWv2 (format 5), all big-endian, with a leaked sequence counter.
    seq = dev.state.get('ruuvi_seq', dev.rng.randrange(65535))
    dev.state['ruuvi_seq'] = (seq + 1) & 0xffff
    mv = dev.state.setdefault('ruuvi_mv', dev.rng.randrange(255))
    temp = int((20.0 + 5.0 * dev.rng.random()) / 0.005)
    hum = int((45.0 + 10.0 * dev.rng.random()) / 0.0025)
    pres = int(101325 - 50000)
    volt = 3000 - 1600            # 3.0 V above 1600 mV
    power = ((volt & 0x7ff) << 5) | ((4 + 40) // 2 & 0x1f)
    body = (bytes([0x05]) + struct.pack('>h', temp) + struct.pack('>H', hum) +
            struct.pack('>H', pres) + struct.pack('>h', 0) * 3 +
            struct.pack('>H', power) + bytes([mv]) + struct.pack('>H', seq) +
            dev.addr[::-1])
    return ad(0xFF, le16(0x0499) + body)


def enc_apple_findmy(dev, now):
    # Approximate Apple "offline finding" mfr blob with a rotating status +
    # a leaked counter (models AirTag/AirPods correlatable byte).
    cnt = dev.state.get('apple_cnt', 0)
    dev.state['apple_cnt'] = (cnt + 1) & 0xff
    payload = bytes([0x12, 0x19, 0x10]) + \
        bytes(dev.rng.randrange(256) for _ in range(20)) + bytes([cnt])
    return ad(0xFF, le16(0x004C) + payload)


def enc_generic_mfr(dev, now):
    # Unknown vendor: company id + a small rotating counter (correlatable).
    cid = dev.company_id or 0xFFFF
    cnt = dev.state.get('gcnt', dev.rng.randrange(256))
    dev.state['gcnt'] = (cnt + 1) & 0xffff
    seed = dev.state.setdefault('gseed', bytes(dev.rng.randrange(256) for _ in range(4)))
    return ad(0xFF, le16(cid) + seed + le16(cnt))


MFR_ENCODERS = {
    'generic_ibeacon': enc_ibeacon,
    'apple_airtag': enc_apple_findmy,
    'apple_airpods_pro_2': enc_apple_findmy,
    'generic_eddystone': enc_eddystone_uid,
    'ruuvitag': enc_ruuvi,
}


def enc_env_service_data(dev, now):
    # BTHome-ish / ESS service data 0x181A: temp sint16(0.01C) hum uint16(0.01%)
    t = int(dev.env_temp() / 0.01)
    h = int(dev.env_hum() / 0.01)
    return ad(0x16, le16(0x181A) + struct.pack('<h', t) + struct.pack('<H', h) +
              bytes([dev.battery]))


# ============================================================
#  GATT characteristic value generators (by 16-bit UUID)
# ============================================================
def hr_measurement(dev):
    bpm = dev.hr_bpm()
    contact = 0b110 if dev.state.get('contact', True) else 0b100  # bits1-2
    flags = contact                                              # uint8 BPM
    rr = int(60.0 / max(bpm, 1) * 1024)                          # 1/1024 s
    flags |= 0x10                                                # RR present
    return bytes([flags, min(bpm, 255)]) + struct.pack('<H', rr)


def battery_level(dev):
    return bytes([dev.battery])


def temperature_ess(dev):
    return struct.pack('<h', int(dev.env_temp() / 0.01))


def humidity_ess(dev):
    return struct.pack('<H', int(dev.env_hum() / 0.01))


def pressure_ess(dev):
    return struct.pack('<I', int(101325 / 0.1))                 # uint32 0.1 Pa


VALUE_GENS = {
    0x2A37: hr_measurement,
    0x2A19: battery_level,
    0x2A38: lambda dev: bytes([0x01]),                          # body loc: chest
    0x2A6E: temperature_ess,
    0x2A6F: humidity_ess,
    0x2A6D: pressure_ess,
    0x2A29: lambda dev: b'VirtualBT',                           # mfr name
    0x2A24: lambda dev: dev.profile_id.encode()[:16],           # model number
    0x2A25: lambda dev: b'SN-' + mac_str(dev.addr).replace(':', '').encode(),
    0x2A00: lambda dev: dev.name.encode()[:20],                 # device name
    0x2A01: lambda dev: le16(dev.appearance),                   # appearance
}


def gen_value(dev, uuid16):
    fn = VALUE_GENS.get(uuid16)
    if fn:
        try:
            return fn(dev)
        except Exception:
            pass
    return bytes([0x00])


# Standard services a device lists as discoverable but rarely spells out in
# the catalog's detailed `gatt` array — synthesized so GAP/DIS/Battery are
# actually present and readable. (char_uuid16, [props]).
STD_SVC = {
    0x1800: [(0x2A00, ['read']), (0x2A01, ['read'])],           # GAP
    0x1801: [(0x2A05, ['indicate'])],                           # GATT
    0x180A: [(0x2A29, ['read']), (0x2A24, ['read']),            # DIS
             (0x2A25, ['read'])],
    0x180F: [(0x2A19, ['read', 'notify'])],                     # Battery
}


# ============================================================
#  Device — one simulated peripheral built from a catalog profile
# ============================================================
class Device:
    def __init__(self, profile, index, seed):
        self.profile = profile
        self.profile_id = profile['id']
        self.index = index
        self.rng = random.Random(seed)   # deterministic: same seed -> same device
        self.state = {}
        adv = profile.get('adv') or {}
        self.addr_type_name = adv.get('address_type', 'public')
        self.rotation_s = adv.get('privacy_rotation_s')
        iv = adv.get('interval_ms') or [100, 100]
        self.interval_min, self.interval_max = iv[0] / 1000.0, iv[1] / 1000.0
        self.connectable = bool(adv.get('connectable', False))
        self.scannable = bool(adv.get('scannable', False))
        self.pdu_name = adv.get('pdu', 'ADV_IND')
        self.name = self._make_name(adv.get('name_pattern'))
        ap = profile.get('appearance') or {}
        self.appearance = int(ap.get('value', '0x0000'), 16) if isinstance(
            ap.get('value'), str) else 0
        cid = profile.get('company_id')
        self.company_id = int(cid, 16) if isinstance(cid, str) else None
        self.sec = profile.get('security') or {}
        self.min_level = int(self.sec.get('min_level', 1) or 1)
        self.node_id = f"{self.profile_id}-{index}"[:40]

        self.addr = self._gen_address()
        self.addr_type = 0 if self.addr_type_name == 'public' else 1

        # dynamic state
        self.battery = 100 - self.rng.randrange(0, 15)
        self._batt_t0 = time.time()
        self._hr_phase = self.rng.random()
        self._env_phase = self.rng.random()
        self._last_rotate = time.time()

        self.connected = False
        self.conn_aa = None
        self.subscriptions = set()     # CCCD handles with notify enabled
        self.db = []                   # [handle, type_uuid16or128, value, secure]
        self.services = []             # (start, end, uuid16 or None, uuid128)
        self._val_uuid = {}            # value-handle -> char uuid16
        self._cccd = {}                # cccd-handle -> value-handle
        self._build_gatt()

    # ---- identity ----
    def _make_name(self, pat):
        if not pat:
            return self.profile.get('label') or self.profile_id
        n = pat
        if '{n}' in n:
            n = n.replace('{n}', f"{self.rng.randrange(0x1000, 0xffff):04X}")
        if '{4hex}' in n:
            n = n.replace('{4hex}', f"{self.rng.randrange(0x10000):04X}")
        return n

    def _gen_address(self):
        h = self.rng.randrange(1 << 48).to_bytes(6, 'little')
        b = bytearray(h)
        t = self.addr_type_name
        if t == 'public':
            # stable, OUI-attributable
            b[3], b[4], b[5] = 0x00, 0x1A, 0x7D            # a fake vendor OUI (MSBs)
        elif t == 'random_static':
            b[5] |= 0xC0                                    # top two bits = 11
        elif t == 'rpa':
            b[5] = (b[5] & 0x3F) | 0x40                     # top two bits = 01
        elif t == 'nrpa':
            b[5] &= 0x3F                                    # top two bits = 00
        return bytes(b)

    def maybe_rotate(self):
        if self.addr_type_name in ('rpa', 'nrpa') and self.rotation_s:
            if time.time() - self._last_rotate >= self.rotation_s:
                self.addr = self._gen_address()
                self._last_rotate = time.time()
                return True
        return False

    # ---- dynamic sensor values ----
    def env_temp(self):
        return 21.0 + 3.0 * (0.5 + 0.5 * self._osc(self._env_phase, 120))

    def env_hum(self):
        return 45.0 + 8.0 * (0.5 + 0.5 * self._osc(self._env_phase + .3, 200))

    def hr_bpm(self):
        return int(70 + 40 * (0.5 + 0.5 * self._osc(self._hr_phase, 30)))

    def _osc(self, phase, period):
        import math
        return math.sin(2 * math.pi * ((time.time() / period) + phase))

    def tick_battery(self):
        # ~1% per simulated "hour" of uptime, floor 5%.
        elapsed_h = (time.time() - self._batt_t0) / 3600.0
        self.battery = max(5, int((100 - self.rng.randrange(0, 15)) - elapsed_h))

    # ---- advertising ----
    def adv_pdu(self, now):
        primary = []
        # Flags first (skip for pure non-connectable beacons per convention,
        # but most stacks tolerate/expect it — keep it for discoverability).
        flags = 0x06 if self.connectable else 0x04
        primary.append(ad(0x01, bytes([flags])))

        # Manufacturer / service data (identity of beacons) is high priority.
        enc = MFR_ENCODERS.get(self.profile_id)
        svc_adv = (self.profile.get('service_uuids') or {})
        if enc:
            primary.append(enc(self, now))
        elif 'service_data_0x16' in svc_adv and self.profile_id == 'generic_environmental_sensor':
            primary.append(enc_env_service_data(self, now))
        elif self.company_id and self.profile.get('manufacturer_data'):
            primary.append(enc_generic_mfr(self, now))

        # Advertised 16-bit service UUIDs.
        u16 = [parse_uuid16(u) for u in svc_adv.get('advertised', [])]
        u16 = [u for u in u16 if u is not None]
        if u16:
            primary.append(ad(0x03, b''.join(le16(u) for u in u16)))

        # Appearance + TX power + name are secondary (spill to scan_rsp).
        extra = []
        if self.name and self.pdu_name != 'ADV_NONCONN_IND':
            nm = self.name.encode()[:26]
            extra.append(ad(0x09, nm))
        if self.appearance:
            extra.append(ad(0x19, le16(self.appearance)))

        adv, rsp, _dropped = pack_ads(primary, extra, self.scannable)
        self._scan_rsp = rsp
        body = self.addr + adv
        ptype = {'ADV_IND': ADV_IND, 'ADV_NONCONN_IND': ADV_NONCONN,
                 'ADV_SCAN_IND': ADV_SCAN_IND,
                 'ADV_DIRECT_IND': ADV_IND}.get(self.pdu_name, ADV_IND)
        tx = 1 << 6 if self.addr_type else 0
        return bytes([ptype | tx, len(body)]) + body

    def scan_rsp_pdu(self):
        body = self.addr + getattr(self, '_scan_rsp', b'')
        tx = 1 << 6 if self.addr_type else 0
        return bytes([SCAN_RSP | tx, len(body)]) + body

    # ---- GATT database ----
    def _build_gatt(self):
        # Assemble service specs: the profile's detailed services first, then
        # synthesize the standard services it lists as discoverable but does
        # not spell out (GAP/GATT/DIS/Battery), so reads/discovery look real.
        specs = []            # (uuid16|None, raw_uuid, [(cu16|None, raw, [props])])
        built16 = set()
        for svc in (self.profile.get('gatt') or []):
            su16 = parse_uuid16(svc.get('uuid'))
            chars = [(parse_uuid16(c.get('uuid')), c.get('uuid'), c.get('props', []))
                     for c in svc.get('chars', [])]
            specs.append((su16, svc.get('uuid'), chars))
            if su16 is not None:
                built16.add(su16)
        for u in (self.profile.get('service_uuids') or {}).get('gatt', []):
            u16 = parse_uuid16(u)
            if u16 in STD_SVC and u16 not in built16:
                chars = [(cu, hex(cu), props) for cu, props in STD_SVC[u16]]
                specs.append((u16, u, chars))
                built16.add(u16)

        handle = 1
        for su16, su_raw, chars in specs:
            start = handle
            sval = le16(su16) if su16 is not None else self._uuid128(su_raw)
            self.db.append([handle, UUID_PRIMARY_SVC, sval, False]); handle += 1
            for cu16, cu_raw, props_list in chars:
                props = 0
                for p in props_list:
                    props |= {'read': PROP_READ, 'write': PROP_WRITE,
                              'write-without-response': PROP_WRITE_NR,
                              'notify': PROP_NOTIFY, 'indicate': PROP_INDICATE
                              }.get(p, 0)
                if props == 0:
                    props = PROP_READ
                val_handle = handle + 1
                cuuid = le16(cu16) if cu16 is not None else self._uuid128(cu_raw)
                self.db.append([handle, UUID_CHARACTERISTIC,
                                bytes([props]) + le16(val_handle) + cuuid, False])
                handle += 1
                secure = self._char_secure(su16)
                self.db.append([val_handle, cu16 if cu16 is not None else 0xFFFF,
                                None, secure])   # value filled dynamically on read
                self._val_uuid[val_handle] = cu16
                handle += 1
                if props & (PROP_NOTIFY | PROP_INDICATE):
                    self.db.append([handle, UUID_CCCD, bytes([0, 0]), False])
                    self._cccd[handle] = val_handle
                    handle += 1
            self.services.append((start, handle - 1, su16,
                                  None if su16 is not None else sval))

    def _uuid128(self, name):
        # Deterministic 128-bit UUID for named/vendor services.
        seed = (str(name) + self.profile_id).encode()
        h = 0x811c9dc5
        out = bytearray(16)
        for i in range(16):
            for b in seed + bytes([i]):
                h = ((h ^ b) * 0x01000193) & 0xffffffff
            out[i] = h & 0xff
        return bytes(out)

    def _char_secure(self, service_uuid16):
        # Chars in GAP(0x1800)/GATT(0x1801)/DIS(0x180A) stay open; otherwise
        # a device with min_level>1 requires encryption we can't provide.
        if self.min_level <= 1:
            return False
        return service_uuid16 not in (0x1800, 0x1801, 0x180A)

    def read_attr(self, handle):
        for h, t, v, secure in self.db:
            if h != handle:
                continue
            if secure and not self.encrypted():
                return ('err', ERR_INSUFF_AUTH)
            if v is None:                     # dynamic char value
                u16 = getattr(self, '_val_uuid', {}).get(handle)
                return ('ok', gen_value(self, u16) if u16 else b'\x00')
            return ('ok', v)
        return ('err', ERR_INVALID_HANDLE)

    def encrypted(self):
        return False                          # no SMP in the sim

    def notify_handles(self):
        """value handles that are notifiable AND currently subscribed."""
        out = []
        for cccd_handle, val_handle in getattr(self, '_cccd', {}).items():
            if cccd_handle in self.subscriptions:
                out.append(val_handle)
        return out


# ============================================================
#  ATT server over a Device's DB
# ============================================================
def att_error(op, handle, err):
    return bytes([ATT_ERROR, op]) + le16(handle) + bytes([err])


def handle_att(dev, req):
    if not req:
        return b''
    op = req[0]

    if op == ATT_MTU_REQ:
        return bytes([ATT_MTU_RSP]) + le16(23)

    if op == ATT_READ_BY_GROUP_REQ and len(req) >= 7:
        start, end, gtype = struct.unpack('<HHH', req[1:7])
        if gtype != UUID_PRIMARY_SVC:
            return att_error(op, start, ERR_REQ_NOT_SUPP)
        entries, each = [], None
        for s, e, u16, u128 in dev.services:
            if s < start or s > end:
                continue
            val = le16(u16) if u16 is not None else u128
            entry = le16(s) + le16(e) + val
            if each is None:
                each = len(entry)
            if len(entry) == each:
                entries.append(entry)
        if not entries:
            return att_error(op, start, ERR_ATTR_NOT_FOUND)
        return bytes([ATT_READ_BY_GROUP_RSP, each]) + b''.join(entries)

    if op == ATT_READ_BY_TYPE_REQ and len(req) >= 7:
        start, end, atype = struct.unpack('<HHH', req[1:7])
        entries, each = [], None
        for h, t, v, secure in dev.db:
            if h < start or h > end:
                continue
            tt = t if isinstance(t, int) else None
            if tt != atype:
                continue
            val = v
            if v is None:
                u16 = getattr(dev, '_val_uuid', {}).get(h)
                val = gen_value(dev, u16) if u16 else b'\x00'
            entry = le16(h) + val
            if each is None:
                each = len(entry)
            if len(entry) == each:
                entries.append(entry)
        if not entries:
            return att_error(op, start, ERR_ATTR_NOT_FOUND)
        return bytes([ATT_READ_BY_TYPE_RSP, each]) + b''.join(entries)

    if op == ATT_FIND_INFO_REQ and len(req) >= 5:
        start, end = struct.unpack('<HH', req[1:5])
        entries = []
        for h, t, v, secure in dev.db:
            if h < start or h > end:
                continue
            if isinstance(t, int) and t <= 0xffff:
                entries.append(le16(h) + le16(t))
        if not entries:
            return att_error(op, start, ERR_ATTR_NOT_FOUND)
        return bytes([ATT_FIND_INFO_RSP, 0x01]) + b''.join(entries)

    if op == ATT_READ_REQ and len(req) >= 3:
        (h,) = struct.unpack('<H', req[1:3])
        kind, val = dev.read_attr(h)
        if kind == 'err':
            return att_error(op, h, val)
        return bytes([ATT_READ_RSP]) + val

    if op in (ATT_WRITE_REQ, ATT_WRITE_CMD) and len(req) >= 3:
        (h,) = struct.unpack('<H', req[1:3])
        val = req[3:]
        for entry in dev.db:
            if entry[0] == h and entry[1] == UUID_CCCD:
                entry[2] = (val + b'\x00\x00')[:2]
                if val and val[0] & 0x03:
                    dev.subscriptions.add(h)
                else:
                    dev.subscriptions.discard(h)
                return b'' if op == ATT_WRITE_CMD else bytes([ATT_WRITE_RSP])
        if op == ATT_WRITE_CMD:
            return b''
        return bytes([ATT_WRITE_RSP])

    return att_error(op, 0x0000, ERR_REQ_NOT_SUPP)


# ============================================================
#  Peripheral runtime (async): one connection to the medium
# ============================================================
class Peripheral:
    def __init__(self, dev, medium_path, verbose):
        self.dev = dev
        self.path = medium_path
        self.verbose = verbose
        self.reader = None
        self.writer = None
        self.rx = b''
        self.running = True

    def log(self, *a):
        if self.verbose:
            print(f"[{self.dev.node_id}]", *a, flush=True)

    def emit(self, pdu, *, ll_type, aa, channel):
        if not self.writer:
            return
        try:
            self.writer.write(make_frame(self.dev.addr, self.dev.addr_type, pdu,
                                         ll_type=ll_type, aa=aa, channel=channel))
        except Exception:
            self.running = False

    async def connect(self):
        self.reader, self.writer = await asyncio.open_unix_connection(self.path)
        self.writer.write(make_hello(self.dev.node_id))
        await self.writer.drain()

    async def rx_loop(self):
        while self.running:
            try:
                data = await self.reader.read(65536)
            except Exception:
                break
            if not data:
                break
            self.rx += data
            while len(self.rx) >= 4:
                (plen,) = struct.unpack('!I', self.rx[:4])
                if plen > HDR_SIZE + 512:
                    self.rx = b''
                    break
                if len(self.rx) < 4 + plen:
                    break
                msg, self.rx = self.rx[4:4 + plen], self.rx[4 + plen:]
                if plen >= HDR_SIZE:
                    self._on_msg(msg)
        self.running = False

    def _on_msg(self, msg):
        f = struct.unpack_from(HDR_FMT, msg, 0)
        magic, _v, plen, aa, _a, _b, _c, _tx, _at, ll_type = f[:10]
        if magic != VBT_MAGIC:
            return
        pdu = msg[HDR_SIZE:HDR_SIZE + plen]
        d = self.dev
        if ll_type == LL_ADV and pdu:
            ptype = pdu[0] & 0x0F
            body = pdu[2:2 + pdu[1]] if len(pdu) >= 2 else b''
            if ptype == SCAN_REQ and d.scannable and len(body) >= 12 and \
                    body[6:12] == d.addr:
                self.emit(d.scan_rsp_pdu(), ll_type=LL_ADV, aa=VBT_ADV_AA, channel=37)
            elif ptype == CONNECT_IND and d.connectable and len(body) >= 34 and \
                    body[6:12] == d.addr and not d.connected:
                d.conn_aa = struct.unpack('<I', body[12:16])[0]
                d.connected = True
                d.subscriptions.clear()
                self.log("connected by", mac_str(body[0:6]),
                         f"(AA=0x{d.conn_aa:08x})")
        elif ll_type == LL_DATA and d.connected and aa == d.conn_aa and pdu:
            self._on_data(pdu)

    def _on_data(self, pdu):
        d = self.dev
        llid = pdu[0] & 0x03
        body = pdu[2:2 + pdu[1]] if len(pdu) >= 2 else b''
        if llid == LLID_CTRL and body:
            op = body[0]
            if op == LL_TERMINATE_IND:
                self.log("disconnected"); d.connected = False; d.conn_aa = None
                d.subscriptions.clear()
            elif op == LL_VERSION_IND:
                self._send_ctrl(LL_VERSION_IND, bytes([0x0C]) + le16(0xFFFF) + le16(0))
            elif op in (LL_FEATURE_REQ, LL_SLAVE_FEATURE_REQ):
                self._send_ctrl(LL_FEATURE_RSP, bytes([0x01, 0, 0, 0, 0, 0, 0, 0]))
            return
        if llid in (LLID_START, LLID_CONT) and len(body) >= 4:
            plen, cid = struct.unpack('<HH', body[0:4])
            payload = body[4:4 + plen]
            if cid == CID_ATT:
                rsp = handle_att(d, payload)
                if rsp:
                    self._send_l2cap(CID_ATT, rsp)
            elif cid == CID_SMP and payload:
                self.log("declining SMP pairing")
                self._send_l2cap(CID_SMP, bytes([0x05, 0x05]))  # Pairing Failed

    def _send_l2cap(self, cid, payload):
        l2 = struct.pack('<HH', len(payload), cid) + payload
        pdu = bytes([LLID_START, len(l2)]) + l2
        self.emit(pdu, ll_type=LL_DATA, aa=self.dev.conn_aa, channel=6)

    def _send_ctrl(self, op, params=b''):
        pdu = bytes([LLID_CTRL, 1 + len(params), op]) + params
        self.emit(pdu, ll_type=LL_DATA, aa=self.dev.conn_aa, channel=6)

    async def timer_loop(self):
        d = self.dev
        next_notify = time.time() + 1.0
        while self.running:
            now = time.time()
            d.tick_battery()
            if not d.connected:
                if d.maybe_rotate():
                    self.log("rotated address ->", mac_str(d.addr))
                # advertise on the three primary channels
                pdu = d.adv_pdu(now)
                for ch in (37, 38, 39):
                    self.emit(pdu, ll_type=LL_ADV, aa=VBT_ADV_AA, channel=ch)
                delay = d.rng.uniform(d.interval_min, d.interval_max)
            else:
                # push notifications for subscribed characteristics
                if now >= next_notify:
                    for vh in d.notify_handles():
                        u16 = getattr(d, '_val_uuid', {}).get(vh)
                        val = gen_value(d, u16) if u16 else b'\x00'
                        self._send_l2cap(CID_ATT, bytes([ATT_HANDLE_VALUE_NTF]) +
                                         le16(vh) + val)
                    # heart-rate cadence is per-heartbeat, others ~1 Hz
                    period = 60.0 / max(d.hr_bpm(), 1) if 0x2A37 in \
                        set(getattr(d, '_val_uuid', {}).values()) else 1.0
                    next_notify = now + period
                delay = 0.1
            try:
                await self.writer.drain()
            except Exception:
                break
            await asyncio.sleep(max(delay, 0.02))
        self.running = False

    async def run(self):
        try:
            await self.connect()
        except Exception as e:
            print(f"[{self.dev.node_id}] connect failed: {e}", file=sys.stderr)
            return
        await asyncio.gather(self.rx_loop(), self.timer_loop(),
                             return_exceptions=True)
        try:
            self.writer.close()
        except Exception:
            pass


# ============================================================
#  Catalog + selection + CLI
# ============================================================
SCENARIOS = {
    'security-ladder': ['govee_led_strip', 'meater_plus', 'wahoo_tickr',
                        'generic_hr_chest_strap', 'philips_hue_bulb',
                        'xiaomi_mi_band_8', 'dexcom_g7', 'august_smart_lock_4'],
    'beacons': ['generic_ibeacon', 'generic_eddystone', 'ruuvitag',
                'apple_airtag', 'samsung_smarttag2', 'tile_mate'],
    'medical': ['generic_blood_pressure_monitor', 'generic_glucose_meter',
                'generic_cgm', 'generic_pulse_oximeter', 'dexcom_g7',
                'omron_evolv'],
    'stress': ['polar_h10', 'nordic_nrf52840_dk'],
    'office': ['generic_smartphone', 'generic_laptop', 'generic_smartwatch',
               'generic_wireless_earbuds', 'generic_hid_keyboard',
               'generic_hid_mouse', 'generic_bt_speaker'],
}


def load_catalog(path):
    with open(path) as f:
        d = json.load(f)
    profiles = {}
    for p in d.get('generic_types', []) + d.get('specific_devices', []):
        profiles[p['id']] = p
    return profiles


def select(profiles, args):
    import re
    chosen = []
    if args.id:
        for i in args.id:
            if i in profiles:
                chosen.append(i)
            else:
                print(f"ble_fleet: unknown --id {i}", file=sys.stderr)
    if args.scenario:
        for i in SCENARIOS.get(args.scenario, []):
            if i in profiles:
                chosen.append(i)
    if args.grep:
        rx = re.compile(args.grep, re.I)
        chosen += [i for i in profiles if rx.search(i) or
                   rx.search(profiles[i].get('label', ''))]
    if args.generic:
        chosen += [i for i, p in profiles.items() if p.get('class') == 'generic']
    if args.specific:
        chosen += [i for i, p in profiles.items() if p.get('class') == 'specific']
    if args.all or not chosen:
        if args.all or not (args.id or args.scenario or args.grep or
                            args.generic or args.specific):
            chosen = list(profiles)
    # de-dupe preserving order
    seen, uniq = set(), []
    for i in chosen:
        if i not in seen:
            seen.add(i); uniq.append(i)
    return uniq


def build_specs(chosen, dup, base_seed):
    """(profile_id, index, per-device seed) triples; deterministic per seed."""
    r = random.Random(base_seed)
    specs, idx = [], 0
    for pid in chosen:
        for _ in range(max(1, dup)):
            specs.append((pid, idx, r.randrange(1 << 30)))
            idx += 1
    return specs


async def _await_stop(runner):
    stop = asyncio.Event()
    loop = asyncio.get_event_loop()
    for s in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(s, stop.set)
        except (NotImplementedError, ValueError):
            pass
    done, _ = await asyncio.wait(
        {asyncio.create_task(runner), asyncio.create_task(stop.wait())},
        return_when=asyncio.FIRST_COMPLETED)


# ---- single-process mode: all peripherals in one asyncio loop ----
async def run_fleet(devices, medium, verbose):
    peris = [Peripheral(d, medium, verbose) for d in devices]
    tasks = [asyncio.create_task(p.run()) for p in peris]
    stop = asyncio.Event()
    loop = asyncio.get_event_loop()
    for s in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(s, stop.set)
        except (NotImplementedError, ValueError):
            pass
    await stop.wait()
    for p in peris:
        p.running = False
    await asyncio.gather(*tasks, return_exceptions=True)


# ---- worker: one peripheral == one process ----
async def _worker_main(peri):
    stop = asyncio.Event()
    loop = asyncio.get_event_loop()
    for s in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(s, stop.set)
        except (NotImplementedError, ValueError):
            pass
    runner = asyncio.create_task(peri.run())
    stopper = asyncio.create_task(stop.wait())
    await asyncio.wait({runner, stopper}, return_when=asyncio.FIRST_COMPLETED)
    peri.running = False
    await asyncio.gather(runner, return_exceptions=True)


# ---- process mode: spawn one worker subprocess per peripheral ----
def run_processes(specs, catalog, medium, verbose):
    procs = []
    for pid, i, seed in specs:
        cmd = [sys.executable, os.path.abspath(__file__), '--worker',
               '--worker-id', pid, '--worker-index', str(i),
               '--worker-seed', str(seed), '--catalog', catalog,
               '--medium', medium]
        if verbose:
            cmd.append('-v')
        procs.append(subprocess.Popen(cmd))

    def _stop(*_a):
        for p in procs:
            if p.poll() is None:
                try:
                    p.terminate()
                except OSError:
                    pass
    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)
    try:
        for p in procs:
            p.wait()
    except KeyboardInterrupt:
        _stop()
        for p in procs:
            try:
                p.wait()
            except KeyboardInterrupt:
                pass


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default_cat = os.path.join(here, '..', 'catalog', 'ble_device_catalog.json')

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--medium', default='/tmp/vbt.sock', help='vbt-medium socket')
    ap.add_argument('--catalog', default=os.path.normpath(default_cat))
    ap.add_argument('--all', action='store_true', help='every catalog device')
    ap.add_argument('--generic', action='store_true', help='all generic_types')
    ap.add_argument('--specific', action='store_true', help='all specific_devices')
    ap.add_argument('--id', action='append', default=[], help='device id (repeatable)')
    ap.add_argument('--grep', help='regex over id/label')
    ap.add_argument('--scenario', choices=sorted(SCENARIOS), help='a preset mix')
    ap.add_argument('--count', type=int, help='random N from the selection')
    ap.add_argument('--dup', type=int, default=1, help='instances per device (default 1)')
    ap.add_argument('--seed', type=int, default=1, help='RNG seed')
    ap.add_argument('--list', action='store_true', help='list catalog and exit')
    ap.add_argument('--in-process', action='store_true',
                    help='run all peripherals in one asyncio process '
                         '(default: one subprocess per peripheral)')
    ap.add_argument('-v', '--verbose', action='store_true')
    # worker mode (internal): run exactly one peripheral as its own process
    ap.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    ap.add_argument('--worker-id', help=argparse.SUPPRESS)
    ap.add_argument('--worker-index', type=int, default=0, help=argparse.SUPPRESS)
    ap.add_argument('--worker-seed', type=int, default=0, help=argparse.SUPPRESS)
    args = ap.parse_args()

    try:
        profiles = load_catalog(args.catalog)
    except OSError as e:
        print(f"ble_fleet: cannot read catalog {args.catalog}: {e}", file=sys.stderr)
        sys.exit(1)

    # Internal: run exactly one peripheral (spawned by the parent per device).
    if args.worker:
        prof = profiles.get(args.worker_id)
        if not prof:
            print(f"ble_fleet worker: unknown id {args.worker_id}", file=sys.stderr)
            sys.exit(1)
        dev = Device(prof, args.worker_index, args.worker_seed)
        peri = Peripheral(dev, args.medium, args.verbose)
        try:
            asyncio.run(_worker_main(peri))
        except KeyboardInterrupt:
            pass
        return

    if args.list:
        for cls in ('generic', 'specific'):
            print(f"\n== {cls} ==")
            for i, p in profiles.items():
                if p.get('class') == cls:
                    a = p.get('adv', {})
                    print(f"  {i:32s} {'conn' if a.get('connectable') else 'beacon':6s} "
                          f"{a.get('address_type','?'):13s} {p.get('label','')}")
        print(f"\nscenarios: {', '.join(sorted(SCENARIOS))}")
        return

    chosen = select(profiles, args)
    if args.count and args.count < len(chosen):
        chosen = random.Random(args.seed).sample(chosen, args.count)

    specs = build_specs(chosen, args.dup, args.seed)
    if not specs:
        print("ble_fleet: no devices selected (try --list)", file=sys.stderr)
        sys.exit(1)

    # Build the devices once to print the roster (deterministic from seed, so
    # the worker subprocesses reconstruct the identical addresses/names).
    devices = [Device(profiles[pid], i, s) for pid, i, s in specs]
    mode = 'in-process' if args.in_process else 'process-per-peripheral'
    print(f"ble_fleet: launching {len(devices)} peripheral(s) on {args.medium} "
          f"[{mode}]")
    for d in devices:
        kind = 'conn' if d.connectable else 'beacon'
        print(f"  {d.node_id:36s} {mac_str(d.addr)} {d.addr_type_name:13s} "
              f"{kind:6s} '{d.name}'")
    print("Ctrl-C to stop.")

    try:
        if args.in_process:
            asyncio.run(run_fleet(devices, args.medium, args.verbose))
        else:
            run_processes(specs, args.catalog, args.medium, args.verbose)
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
