#!/usr/bin/env python3
"""
medium_dump.py — Tap the vbt virtual Bluetooth medium and decode PDUs.

Connects to a running vbt-medium hub as a *promiscuous monitor* (a hello
flag the hub honours by copying every PDU it forwards — advertising and
point-to-point connection data alike — to this tap). It decodes each BLE
Link-Layer PDU and prints a one-line summary, optionally with a hex dump,
and can write a Wireshark-readable pcap (LINKTYPE_BLUETOOTH_LE_LL).

This is the virtual equivalent of a BLE sniffer, and the first thing to
reach for when a discovery/connection/pairing flow isn't behaving.

Usage:
    python3 scripts/medium_dump.py /tmp/vbt.sock
    python3 scripts/medium_dump.py /tmp/vbt.sock -v            # + hex dump
    python3 scripts/medium_dump.py /tmp/vbt.sock -w capture.pcap
    python3 scripts/medium_dump.py /tmp/vbt.sock --filter data # adv|data|all

Open the pcap in Wireshark (it dissects LE LL, ATT/GATT, and SMP natively).
"""

import argparse
import socket
import struct
import sys
import time

# ---- wire format (must match vbt.h) ----
VBT_MAGIC = 0x31544256          # "VBT1"
VBT_HELLO_MAGIC = 0x48544256    # "VBTH"
HELLO_FLAG_MONITOR = 0x02
VBT_ADV_AA = 0x8E89BED6

HDR_FMT = '<I H H I I I I 6s B B B B b b H 6s'   # 44 bytes
HDR_SIZE = struct.calcsize(HDR_FMT)
assert HDR_SIZE == 44, HDR_SIZE

LL_ADV, LL_DATA = 0x01, 0x02

ADV_PDU = {
    0x00: "ADV_IND", 0x01: "ADV_DIRECT_IND", 0x02: "ADV_NONCONN_IND",
    0x03: "SCAN_REQ", 0x04: "SCAN_RSP", 0x05: "CONNECT_IND",
    0x06: "ADV_SCAN_IND", 0x07: "ADV_EXT_IND",
}

LLID = {0x01: "L2CAP-cont/empty", 0x02: "L2CAP-start", 0x03: "LL-control"}

LL_CTRL = {
    0x00: "LL_CONNECTION_UPDATE_IND", 0x01: "LL_CHANNEL_MAP_IND",
    0x02: "LL_TERMINATE_IND", 0x03: "LL_ENC_REQ", 0x04: "LL_ENC_RSP",
    0x05: "LL_START_ENC_REQ", 0x06: "LL_START_ENC_RSP",
    0x07: "LL_UNKNOWN_RSP", 0x08: "LL_FEATURE_REQ", 0x09: "LL_FEATURE_RSP",
    0x0A: "LL_PAUSE_ENC_REQ", 0x0B: "LL_PAUSE_ENC_RSP", 0x0C: "LL_VERSION_IND",
    0x0D: "LL_REJECT_IND", 0x0E: "LL_SLAVE_FEATURE_REQ",
    0x0F: "LL_CONNECTION_PARAM_REQ", 0x10: "LL_CONNECTION_PARAM_RSP",
    0x11: "LL_REJECT_EXT_IND", 0x12: "LL_PING_REQ", 0x13: "LL_PING_RSP",
    0x14: "LL_LENGTH_REQ", 0x15: "LL_LENGTH_RSP",
}

L2CAP_CID = {0x0004: "ATT", 0x0005: "LE-SIG", 0x0006: "SMP"}

ATT_OP = {
    0x01: "ErrorRsp", 0x02: "ExchangeMTUReq", 0x03: "ExchangeMTURsp",
    0x04: "FindInfoReq", 0x05: "FindInfoRsp", 0x06: "FindByTypeReq",
    0x07: "FindByTypeRsp", 0x08: "ReadByTypeReq", 0x09: "ReadByTypeRsp",
    0x0A: "ReadReq", 0x0B: "ReadRsp", 0x0C: "ReadBlobReq", 0x0D: "ReadBlobRsp",
    0x10: "ReadByGroupTypeReq", 0x11: "ReadByGroupTypeRsp",
    0x12: "WriteReq", 0x13: "WriteRsp", 0x16: "PrepareWriteReq",
    0x18: "ExecuteWriteReq", 0x19: "ExecuteWriteRsp",
    0x1B: "HandleValueNtf", 0x1D: "HandleValueInd", 0x1E: "HandleValueCfm",
    0x52: "WriteCmd", 0xD2: "SignedWriteCmd",
}

SMP_OP = {
    0x01: "PairingRequest", 0x02: "PairingResponse", 0x03: "PairingConfirm",
    0x04: "PairingRandom", 0x05: "PairingFailed", 0x06: "EncryptionInfo",
    0x07: "MasterIdentification", 0x08: "IdentityInformation",
    0x09: "IdentityAddressInformation", 0x0A: "SigningInformation",
    0x0B: "SecurityRequest", 0x0C: "PairingPublicKey",
    0x0D: "PairingDHKeyCheck", 0x0E: "KeypressNotification",
}

AD_TYPE = {
    0x01: "Flags", 0x02: "UUID16-more", 0x03: "UUID16", 0x04: "UUID32-more",
    0x05: "UUID32", 0x06: "UUID128-more", 0x07: "UUID128",
    0x08: "ShortName", 0x09: "Name", 0x0A: "TxPower", 0x16: "ServiceData",
    0x19: "Appearance", 0xFF: "MfgData",
}


def mac(b):
    return ":".join(f"{x:02x}" for x in b[::-1])   # BLE addrs print MSB-first


def chan_freq(ch):
    if ch == 37: return 2402
    if ch == 38: return 2426
    if ch == 39: return 2480
    if ch <= 10: return 2404 + ch * 2
    if ch <= 36: return 2404 + (ch + 1) * 2
    return 0


def hexs(b, n=48):
    s = " ".join(f"{x:02x}" for x in b[:n])
    return s + (" ..." if len(b) > n else "")


def parse_adv_data(ad):
    """Decode AD structures into a compact string."""
    out, i = [], 0
    while i + 1 < len(ad):
        ln = ad[i]
        if ln == 0 or i + 1 + ln > len(ad):
            break
        t = ad[i + 1]
        val = ad[i + 2:i + 1 + ln]
        name = AD_TYPE.get(t, f"AD-0x{t:02x}")
        if t in (0x08, 0x09):
            out.append(f"{name}='{val.decode('utf-8', 'replace')}'")
        elif t == 0x01 and val:
            out.append(f"Flags=0x{val[0:1].hex()}")
        elif t in (0x02, 0x03) and len(val) >= 2:
            uuids = [f"0x{val[j+1]<<8|val[j]:04x}" for j in range(0, len(val) - 1, 2)]
            out.append(f"{name}=[{','.join(uuids)}]")
        elif t == 0x0A and val:
            out.append(f"TxPower={struct.unpack('b', val[0:1])[0]}dBm")
        else:
            out.append(f"{name}={val.hex()}")
        i += 1 + ln
    return " ".join(out)


def decode_adv(pdu):
    if len(pdu) < 2:
        return "ADV(short)"
    ptype = pdu[0] & 0x0F
    name = ADV_PDU.get(ptype, f"ADV-{ptype}")
    body = pdu[2:2 + pdu[1]]
    if ptype in (0x00, 0x02, 0x06) and len(body) >= 6:      # *_IND with AdvA
        info = f"AdvA={mac(body[0:6])}"
        ad = body[6:]
        if ad:
            info += " " + parse_adv_data(ad)
        return f"{name} {info}"
    if ptype == 0x04 and len(body) >= 6:                    # SCAN_RSP
        return f"{name} AdvA={mac(body[0:6])} {parse_adv_data(body[6:])}"
    if ptype == 0x03 and len(body) >= 12:                   # SCAN_REQ
        return f"{name} ScanA={mac(body[0:6])} AdvA={mac(body[6:12])}"
    if ptype == 0x01 and len(body) >= 12:                   # ADV_DIRECT_IND
        return f"{name} AdvA={mac(body[0:6])} InitA={mac(body[6:12])}"
    if ptype == 0x05 and len(body) >= 34:                   # CONNECT_IND
        aa = struct.unpack('<I', body[12:16])[0]
        interval = struct.unpack('<H', body[22:24])[0]
        return (f"{name} InitA={mac(body[0:6])} AdvA={mac(body[6:12])} "
                f"AA=0x{aa:08x} interval={interval*1.25:.1f}ms")
    return name


def decode_l2cap(payload):
    if len(payload) < 4:
        return f"L2CAP(short {payload.hex()})"
    plen, cid = struct.unpack('<HH', payload[0:4])
    body = payload[4:4 + plen]
    cname = L2CAP_CID.get(cid, f"CID-0x{cid:04x}")
    if cid == 0x0004 and body:      # ATT
        op = body[0]
        detail = ATT_OP.get(op, f"op-0x{op:02x}")
        if op in (0x1B, 0x1D, 0x0A, 0x0B, 0x12, 0x52) and len(body) >= 3:
            h = struct.unpack('<H', body[1:3])[0]
            detail += f" handle=0x{h:04x}"
        return f"ATT {detail}"
    if cid == 0x0006 and body:      # SMP
        return f"SMP {SMP_OP.get(body[0], f'op-0x{body[0]:02x}')}"
    if cid == 0x0005 and body:      # LE signaling
        return f"LE-SIG code=0x{body[0]:02x}"
    return f"{cname} len={plen} {body[:16].hex()}"


def decode_data(pdu):
    if len(pdu) < 2:
        return "DATA(short)"
    llid = pdu[0] & 0x03
    body = pdu[2:2 + pdu[1]]
    if llid == 0x03:                # LL control
        if not body:
            return "LL-control(empty)"
        return f"LL {LL_CTRL.get(body[0], f'ctrl-0x{body[0]:02x}')}"
    if llid == 0x01 and not body:
        return "LL empty PDU (keepalive)"
    if llid in (0x01, 0x02):
        return decode_l2cap(body)
    return f"LLID={llid} {body.hex()}"


# ---- pcap (LINKTYPE_BLUETOOTH_LE_LL = 251) ----
PCAP_MAGIC = 0xA1B2C3D4
DLT_BLE_LL = 251


class Pcap:
    def __init__(self, path):
        self.f = open(path, 'wb')
        self.f.write(struct.pack('<IHHiIII', PCAP_MAGIC, 2, 4, 0, 0, 65535,
                                  DLT_BLE_LL))

    def write(self, access_addr, pdu):
        # LE LL record: access address (4, LE) + PDU + CRC (3, dummy)
        rec = struct.pack('<I', access_addr) + pdu + b'\x00\x00\x00'
        now = time.time()
        self.f.write(struct.pack('<IIII', int(now),
                                 int((now % 1) * 1e6), len(rec), len(rec)))
        self.f.write(rec)
        self.f.flush()

    def close(self):
        self.f.close()


def make_hello(node_id):
    payload = struct.pack('<I', VBT_HELLO_MAGIC) + node_id.encode() + b'\x00'
    payload += bytes([HELLO_FLAG_MONITOR])
    return struct.pack('!I', len(payload)) + payload


def main():
    ap = argparse.ArgumentParser(description="Tap and decode the vbt medium")
    ap.add_argument("socket", help="path to the vbt-medium data socket")
    ap.add_argument("-v", "--verbose", action="store_true", help="hex dump each PDU")
    ap.add_argument("-w", "--pcap", metavar="FILE", help="write a Wireshark pcap")
    ap.add_argument("--node-id", default="dump", help="tap identity (default: dump)")
    ap.add_argument("--filter", choices=["adv", "data", "all"], default="all",
                    help="only show advertising, only connection data, or all")
    args = ap.parse_args()

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(args.socket)
    except OSError as e:
        print(f"error: cannot connect to {args.socket}: {e}", file=sys.stderr)
        print("  (is vbt-medium running on that socket?)", file=sys.stderr)
        sys.exit(1)
    s.sendall(make_hello(args.node_id))

    pcap = Pcap(args.pcap) if args.pcap else None
    print(f"tapping {args.socket} as monitor '{args.node_id}'"
          + (f", writing {args.pcap}" if pcap else "") + " (Ctrl-C to stop)\n")

    buf = b""
    n = 0
    try:
        while True:
            data = s.recv(65536)
            if not data:
                print("medium closed the connection")
                break
            buf += data
            while len(buf) >= 4:
                (plen,) = struct.unpack('!I', buf[:4])
                if plen > HDR_SIZE + 512:
                    buf = b""
                    break
                if len(buf) < 4 + plen:
                    break
                msg, buf = buf[4:4 + plen], buf[4 + plen:]
                if plen < HDR_SIZE:
                    continue
                f = struct.unpack_from(HDR_FMT, msg, 0)
                (magic, _ver, pdu_len, aa, _tlo, _thi, _flags,
                 tx_addr, tx_atype, ll_type, channel, phy, txpow, rssi,
                 _conn, _rsv) = f
                if magic != VBT_MAGIC:
                    continue
                pdu = msg[HDR_SIZE:HDR_SIZE + pdu_len]

                kind = "ADV" if ll_type == LL_ADV else "DATA"
                if args.filter == "adv" and ll_type != LL_ADV:
                    continue
                if args.filter == "data" and ll_type != LL_DATA:
                    continue

                n += 1
                if ll_type == LL_ADV:
                    summary = decode_adv(pdu)
                elif ll_type == LL_DATA:
                    summary = decode_data(pdu)
                else:
                    summary = f"ll_type=0x{ll_type:02x}"

                phys = {1: "1M", 2: "2M", 3: "coded-s8", 4: "coded-s2"}.get(phy, str(phy))
                ts = time.strftime("%H:%M:%S")
                aas = "adv" if aa == VBT_ADV_AA else f"0x{aa:08x}"
                print(f"[{ts}] #{n:<5d} {kind:<4s} ch={channel:<2d}"
                      f"({chan_freq(channel)}MHz) {phys:<4s} aa={aas:<10s} "
                      f"tx={mac(tx_addr)} rssi={rssi}dBm  {summary}")
                if args.verbose:
                    print(f"           pdu[{pdu_len}]: {hexs(pdu)}")

                if pcap:
                    pcap.write(aa, pdu)
    except KeyboardInterrupt:
        print(f"\n{n} PDUs captured")
    finally:
        s.close()
        if pcap:
            pcap.close()
            print(f"pcap written to {args.pcap}")


if __name__ == "__main__":
    main()
