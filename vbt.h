/*
 * vbt – Virtual Bluetooth Medium Protocol
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wire protocol for exchanging Bluetooth Low Energy (BLE) Link-Layer
 * PDUs between QEMU instances via a chardev backend (typically a
 * Unix-domain socket connected to a hub process).
 *
 * Architecture:
 *   QEMU-A  <--socket-->  vbt-medium  <--socket-->  QEMU-B
 *
 * The hub (vbt-medium) is a channel- and connection-aware fan-out
 * process. Each QEMU node carries a virtual BLE controller (the Link
 * Layer lives in the -device model); the medium only models the RF
 * broadcast domain:
 *
 *   - Advertising-channel PDUs (ll_type == VBT_LL_ADV) are broadcast to
 *     every other node, subject to the propagation model. Every node's
 *     controller decides for itself whether it is scanning and whether to
 *     surface the advertisement to its host stack.
 *   - Data-channel PDUs (ll_type == VBT_LL_DATA) carry a per-connection
 *     Access Address. The hub snoops CONNECT_IND PDUs to learn which two
 *     nodes own an Access Address and then routes data PDUs only between
 *     those two endpoints — a virtual point-to-point link.
 *
 * Because the controller (Link Layer) lives in each node and the medium
 * only fans out PDUs, the host stack (BlueZ: L2CAP, ATT/GATT, SMP) runs
 * unmodified: pairing and service discovery happen end-to-end between two
 * real host stacks, exactly as on real silicon.
 *
 * Wire protocol (stream socket, length-prefixed):
 *   [uint32_t length (network byte order)]  -- total bytes following
 *   [struct vbt_frame_hdr]                   -- medium header (44 bytes)
 *   [uint8_t pdu[...]]                        -- raw BLE Link-Layer PDU
 *
 * Protocol version history:
 *   v1: BLE advertising + data physical-channel PDUs. BR/EDR (Classic)
 *       PDU types are reserved in the ll_type namespace for a later phase
 *       but not yet emitted or filtered.
 */

#ifndef VBT_H
#define VBT_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
/* The kernel's <linux/types.h> provides the unsigned fixed-width aliases
 * but not the signed ones; vbt_frame_hdr uses int8_t. */
typedef __s8 int8_t;
#else
#include <stdint.h>
#endif

/* ================================================================
 *  Wire protocol constants
 * ================================================================ */

/* Protocol magic / version.
 *
 * Magic spells "VBT1" on the wire (bytes 'V','B','T','1' =
 * 0x56,0x42,0x54,0x31, read as a little-endian uint32 = 0x31544256). */
#define VBT_MAGIC                0x31544256  /* "VBT1" */
#define VBT_VERSION              1

/* A BLE Link-Layer PDU is at most 2 (header) + 255 (payload) = 257 bytes
 * for LE Data Length Extension / extended advertising. Give generous
 * headroom so future BR/EDR baseband payloads also fit. */
#define VBT_MAX_PDU_SIZE         512

/* Total max message size on the wire: header + max PDU. */
#define VBT_MAX_MSG_SIZE         \
    (sizeof(struct vbt_frame_hdr) + VBT_MAX_PDU_SIZE)

/* ================================================================
 *  Link-Layer PDU classes (medium header ll_type field)
 * ================================================================ */
#define VBT_LL_ADV               0x01  /* advertising physical channel PDU */
#define VBT_LL_DATA              0x02  /* data physical channel PDU (conn) */
/* 0x10..0x1F reserved for BR/EDR (Classic) baseband PDUs — not used in v1 */
#define VBT_LL_BR_RESERVED_BASE  0x10

/* ================================================================
 *  BLE PHY identifiers (medium header phy field)
 * ================================================================ */
#define VBT_PHY_1M               1  /* LE 1M (mandatory) */
#define VBT_PHY_2M               2  /* LE 2M */
#define VBT_PHY_CODED_S8         3  /* LE Coded, S=8 (long range) */
#define VBT_PHY_CODED_S2         4  /* LE Coded, S=2 */

/* ================================================================
 *  BLE channel plan
 *
 *  There are 40 RF channels, 2 MHz apart, 2402..2480 MHz. The Link
 *  Layer channel *index* used here is:
 *     0..36  data channels
 *     37,38,39  primary advertising channels (2402, 2426, 2480 MHz)
 *  The medium filters connection traffic by Access Address rather than
 *  by channel (endpoints hop channels within a connection), and fans
 *  advertising PDUs out on all channels, so `channel` is primarily
 *  informational / used by the survey and propagation model.
 * ================================================================ */
#define VBT_CHAN_ADV_37          37   /* 2402 MHz */
#define VBT_CHAN_ADV_38          38   /* 2426 MHz */
#define VBT_CHAN_ADV_39          39   /* 2480 MHz */
#define VBT_NUM_CHANNELS         40

/* The fixed Access Address used by all advertising-channel PDUs. */
#define VBT_ADV_ACCESS_ADDR      0x8E89BED6u

/* ================================================================
 *  BLE advertising-channel PDU header decoding (for the hub's
 *  CONNECT_IND snoop). The first PDU octet's low nibble is the type.
 * ================================================================ */
#define VBT_ADV_PDU_TYPE_MASK    0x0F
#define VBT_ADV_IND              0x00
#define VBT_ADV_DIRECT_IND       0x01
#define VBT_ADV_NONCONN_IND      0x02
#define VBT_SCAN_REQ             0x03
#define VBT_SCAN_RSP             0x04
#define VBT_CONNECT_IND          0x05
#define VBT_ADV_SCAN_IND         0x06
#define VBT_ADV_EXT_IND          0x07

/* Byte offsets within a CONNECT_IND PDU (2-octet header first):
 *   [0..1]  header (type/flags, length)
 *   [2..7]  InitA (initiator address)
 *   [8..13] AdvA  (advertiser address)
 *   [14..17] LLData: Access Address (little-endian)
 *   ...      CRCInit, WinSize, WinOffset, Interval, Latency, Timeout,
 *            ChM, Hop/SCA
 */
#define VBT_CONNIND_INITA_OFF    2
#define VBT_CONNIND_ADVA_OFF     8
#define VBT_CONNIND_AA_OFF       14

/* ================================================================
 *  Hello / registration message
 *
 *  Sent by every node immediately after connecting, before any frame:
 *     [uint32 length (net)] [uint32 VBT_HELLO_MAGIC] [node_id\0] [flags?]
 *
 *  The flags byte is optional and trails node_id's NUL terminator; a
 *  hello that omits it implies flags = 0. VBT_HELLO_FLAG_PHYSICAL is
 *  reserved for a future physical Bluetooth bridge (a real radio whose
 *  links bypass the simulated propagation model), mirroring vwifi.
 * ================================================================ */
#define VBT_HELLO_MAGIC          0x48544256  /* "VBTH" */
#define VBT_HELLO_FLAG_PHYSICAL  0x01

/* ================================================================
 *  Frame header — prepended to every BLE PDU on the wire (44 bytes)
 *
 *  Field layout is chosen to be naturally aligned (no compiler padding)
 *  and stable across builds. All multi-byte fields are little-endian
 *  (native x86); only the 4-byte length prefix is network (big-endian).
 *
 *    Offset  Size  Field
 *    0       4     magic
 *    4       2     version
 *    6       2     pdu_len
 *    8       4     access_addr
 *    12      4     tsf_lo
 *    16      4     tsf_hi
 *    20      4     flags            (byte 23 = TTL, used by hub)
 *    24      6     tx_addr
 *    30      1     tx_addr_type     (0 = public, 1 = random)
 *    31      1     ll_type          (VBT_LL_ADV / VBT_LL_DATA)
 *    32      1     channel          (BLE channel index 0..39)
 *    33      1     phy              (VBT_PHY_*)
 *    34      1     tx_power_dbm     (advertiser/initiator TX power, signed)
 *    35      1     rssi             (per-link, filled by medium)
 *    36      2     conn_handle      (informational; 0 = none / advertising)
 *    38      6     _reserved        (must be zero)
 * ================================================================ */
struct vbt_frame_hdr {
    uint32_t    magic;              /* VBT_MAGIC */
    uint16_t    version;            /* VBT_VERSION */
    uint16_t    pdu_len;            /* length of BLE PDU following this header */
    uint32_t    access_addr;        /* LL Access Address (adv = 0x8E89BED6) */
    uint32_t    tsf_lo;             /* transmit timestamp low word */
    uint32_t    tsf_hi;             /* transmit timestamp high word */
    uint32_t    flags;              /* byte 23 = TTL (hub use); rest reserved */
    uint8_t     tx_addr[6];         /* transmitting device address */
    uint8_t     tx_addr_type;       /* 0 = public, 1 = random */
    uint8_t     ll_type;            /* VBT_LL_ADV / VBT_LL_DATA */
    uint8_t     channel;            /* BLE channel index 0..39 */
    uint8_t     phy;                /* VBT_PHY_* */
    int8_t      tx_power_dbm;       /* advertiser/initiator TX power (dBm) */
    int8_t      rssi;               /* simulated RSSI (dBm), filled by medium */
    uint16_t    conn_handle;        /* informational connection id (0 = none) */
    uint8_t     _reserved[6];       /* must be zero */
};

/* Header size on the wire. */
#define VBT_HDR_SIZE             sizeof(struct vbt_frame_hdr)

/* ================================================================
 *  Default medium parameters
 * ================================================================ */

/* Default RSSI reported for PDUs received over the medium when no
 * position-based propagation model applies. -40 dBm = strong signal,
 * appropriate for "same host" VMs. */
#define VBT_DEFAULT_RSSI         (-40)

/* Default advertiser TX power (dBm) when a node has not set one. */
#define VBT_DEFAULT_TXPOWER      0

/* Receive sensitivity floor (dBm). Below this a PDU is not decodable and
 * the propagation model drops it. Real BLE 1M receivers sit near -95 dBm. */
#define VBT_DEFAULT_RX_SENS      (-95)

/* Receive buffer size for reassembling length-prefixed messages from the
 * stream socket: 4 (length prefix) + max message. */
#define VBT_RXBUF_SIZE           (4 + VBT_HDR_SIZE + VBT_MAX_PDU_SIZE)

#endif /* VBT_H */
