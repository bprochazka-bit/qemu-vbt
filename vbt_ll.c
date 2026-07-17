/*
 * vbt_ll — Portable BLE controller core (HCI + Link Layer)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * See vbt_ll.h for the design. This file implements the HCI LE command
 * engine and the advertising / scanning / initiating / connected Link
 * Layer, exchanging BLE PDUs with the virtual medium.
 *
 * Scope (BLE-first):
 *   - controller/init + informational HCI commands
 *   - undirected connectable advertising (ADV_IND) with scan response
 *   - passive and active scanning -> LE Advertising Report
 *   - initiating (LE Create Connection -> CONNECT_IND)
 *   - one or more simultaneous connections, ACL data both directions with
 *     Number Of Completed Packets flow control
 *   - link-layer disconnect (LL_TERMINATE_IND)
 *   - link-layer encryption modelled as a trusted-medium pass-through so
 *     SMP bonding completes end-to-end (no bytes are actually enciphered;
 *     the medium is already private)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vbt_ll.h"

/* ================================================================
 *  HCI opcodes (OGF<<10 | OCF)
 * ================================================================ */
#define OP_DISCONNECT                 0x0406
#define OP_READ_REMOTE_VERSION        0x041D
#define OP_SET_EVENT_MASK             0x0C01
#define OP_RESET                      0x0C03
#define OP_SET_EVENT_MASK_PAGE2       0x0C63
#define OP_READ_LOCAL_VERSION         0x1001
#define OP_READ_LOCAL_COMMANDS        0x1002
#define OP_READ_LOCAL_FEATURES        0x1003
#define OP_READ_BUFFER_SIZE           0x1005
#define OP_READ_BD_ADDR               0x1009
#define OP_LE_SET_EVENT_MASK          0x2001
#define OP_LE_READ_BUFFER_SIZE        0x2002
#define OP_LE_READ_LOCAL_FEATURES     0x2003
#define OP_LE_SET_RANDOM_ADDRESS      0x2005
#define OP_LE_SET_ADV_PARAMETERS      0x2006
#define OP_LE_READ_ADV_TX_POWER       0x2007
#define OP_LE_SET_ADV_DATA            0x2008
#define OP_LE_SET_SCAN_RSP_DATA       0x2009
#define OP_LE_SET_ADV_ENABLE          0x200A
#define OP_LE_SET_SCAN_PARAMETERS     0x200B
#define OP_LE_SET_SCAN_ENABLE         0x200C
#define OP_LE_CREATE_CONN             0x200D
#define OP_LE_CREATE_CONN_CANCEL      0x200E
#define OP_LE_READ_WHITE_LIST_SIZE    0x200F
#define OP_LE_CLEAR_WHITE_LIST        0x2010
#define OP_LE_ADD_WHITE_LIST          0x2011
#define OP_LE_REMOVE_WHITE_LIST       0x2012
#define OP_LE_CONN_UPDATE             0x2013
#define OP_LE_SET_HOST_CHAN_CLASS     0x2014
#define OP_LE_READ_REMOTE_FEATURES    0x2016
#define OP_LE_ENCRYPT                 0x2017
#define OP_LE_RAND                    0x2018
#define OP_LE_START_ENCRYPTION        0x2019
#define OP_LE_LTK_REQ_REPLY           0x201A
#define OP_LE_LTK_REQ_NEG_REPLY       0x201B
#define OP_LE_READ_SUPPORTED_STATES   0x201C

/* ================================================================
 *  HCI event codes
 * ================================================================ */
#define EVT_DISCONN_COMPLETE          0x05
#define EVT_ENCRYPT_CHANGE            0x08
#define EVT_READ_REMOTE_VERSION_CMPL  0x0C
#define EVT_CMD_COMPLETE              0x0E
#define EVT_CMD_STATUS                0x0F
#define EVT_NUM_COMPLETED_PACKETS     0x13
#define EVT_LE_META                   0x3E

/* LE meta subevents */
#define SUB_LE_CONN_COMPLETE          0x01
#define SUB_LE_ADV_REPORT             0x02
#define SUB_LE_CONN_UPDATE_COMPLETE   0x03
#define SUB_LE_READ_REMOTE_FEAT_CMPL  0x04
#define SUB_LE_LTK_REQUEST            0x05

/* HCI status codes */
#define HCI_SUCCESS                   0x00
#define HCI_UNKNOWN_COMMAND           0x01
#define HCI_UNKNOWN_CONN_ID           0x02
#define HCI_CONN_TIMEOUT              0x08
#define HCI_REMOTE_USER_TERM          0x13
#define HCI_CONN_TERM_LOCAL_HOST      0x16

/* ================================================================
 *  Link-Layer control PDU opcodes (LLID = 0b11)
 * ================================================================ */
#define LL_CONNECTION_UPDATE_IND      0x00
#define LL_CHANNEL_MAP_IND            0x01
#define LL_TERMINATE_IND              0x02
#define LL_ENC_REQ                    0x03
#define LL_ENC_RSP                    0x04
#define LL_START_ENC_REQ             0x05
#define LL_START_ENC_RSP             0x06
#define LL_FEATURE_REQ                0x08
#define LL_FEATURE_RSP                0x09
#define LL_VERSION_IND                0x0C
#define LL_SLAVE_FEATURE_REQ          0x0E

/* LLID (lower 2 bits of the data PDU header byte 0). */
#define LLID_CONT                     0x01  /* continuation / empty */
#define LLID_START                    0x02  /* start of an L2CAP message */
#define LLID_CONTROL                  0x03  /* LL control PDU */

/* ================================================================
 *  Configuration
 * ================================================================ */
#define VBT_LL_MAX_CONN               8
#define ADV_INTERVAL_MS_DEFAULT       100
#define BASE_CONN_HANDLE              0x0040
#define LE_ACL_MTU                    251
#define LE_ACL_PKTS                   8

enum ll_state {
    LL_IDLE = 0,
    LL_ADVERTISING,
    LL_SCANNING,
    LL_INITIATING,
};

struct ll_conn {
    bool        active;
    uint16_t    handle;
    uint32_t    access_addr;
    uint8_t     role;            /* 0 = central, 1 = peripheral */
    uint8_t     peer_addr[6];
    uint8_t     peer_addr_type;
    bool        encrypting;
    bool        encrypted;
};

struct vbt_ll {
    struct vbt_ll_ops ops;
    uint8_t     public_addr[6];
    uint8_t     random_addr[6];
    bool        random_addr_set;

    enum ll_state state;
    uint8_t      num_hci_cmd;    /* command flow-control credits */

    /* Advertising */
    bool        adv_enabled;
    uint8_t     adv_type;
    uint8_t     adv_own_addr_type;
    uint8_t     adv_direct_addr_type;
    uint8_t     adv_direct_addr[6];
    uint16_t    adv_interval_ms;
    uint8_t     adv_data[31];
    uint8_t     adv_data_len;
    uint8_t     scan_rsp_data[31];
    uint8_t     scan_rsp_data_len;
    uint64_t    last_adv_ms;

    /* Scanning / initiating */
    bool        scan_enabled;
    uint8_t     scan_type;       /* 0 = passive, 1 = active */
    uint8_t     scan_own_addr_type;
    uint8_t     init_peer_addr[6];
    uint8_t     init_peer_addr_type;
    uint8_t     init_own_addr_type;
    uint16_t    init_interval;   /* connection interval (units of 1.25ms) */

    /* Connections */
    struct ll_conn conns[VBT_LL_MAX_CONN];
    uint32_t    next_aa;
};

/* ================================================================
 *  Small helpers
 * ================================================================ */
static const uint8_t *own_source_addr(const struct vbt_ll *ll, uint8_t own_type)
{
    /* own_type: 0/2 = public, 1/3 = random */
    if ((own_type & 0x01) && ll->random_addr_set)
        return ll->random_addr;
    return ll->public_addr;
}

static uint8_t own_source_addr_type(const struct vbt_ll *ll, uint8_t own_type)
{
    (void)ll;
    return (own_type & 0x01) ? 1 : 0;
}

static struct ll_conn *conn_by_handle(struct vbt_ll *ll, uint16_t h)
{
    for (int i = 0; i < VBT_LL_MAX_CONN; i++)
        if (ll->conns[i].active && ll->conns[i].handle == h)
            return &ll->conns[i];
    return NULL;
}

static struct ll_conn *conn_by_aa(struct vbt_ll *ll, uint32_t aa)
{
    for (int i = 0; i < VBT_LL_MAX_CONN; i++)
        if (ll->conns[i].active && ll->conns[i].access_addr == aa)
            return &ll->conns[i];
    return NULL;
}

static struct ll_conn *conn_alloc(struct vbt_ll *ll)
{
    for (int i = 0; i < VBT_LL_MAX_CONN; i++)
        if (!ll->conns[i].active) {
            memset(&ll->conns[i], 0, sizeof(ll->conns[i]));
            ll->conns[i].active = true;
            ll->conns[i].handle = BASE_CONN_HANDLE + i;
            return &ll->conns[i];
        }
    return NULL;
}

/* Simple deterministic-ish pseudo-random for Access Addresses / LE Rand.
 * Seeded from the device address so two nodes don't collide in practice. */
static uint32_t ll_rand32(struct vbt_ll *ll)
{
    ll->next_aa = ll->next_aa * 1664525u + 1013904223u;
    return ll->next_aa;
}

/* ================================================================
 *  HCI event emission
 * ================================================================ */
static void send_hci(struct vbt_ll *ll, const uint8_t *pkt, size_t len)
{
    if (ll->ops.hci_to_host) ll->ops.hci_to_host(ll->ops.ctx, pkt, len);
}

/* Command Complete with return parameters (status is the first ret byte). */
static void cmd_complete(struct vbt_ll *ll, uint16_t opcode,
                         const uint8_t *ret, uint8_t ret_len)
{
    uint8_t buf[260];
    buf[0] = HCI_EVT_PKT;
    buf[1] = EVT_CMD_COMPLETE;
    buf[2] = (uint8_t)(3 + ret_len);
    buf[3] = ll->num_hci_cmd ? ll->num_hci_cmd : 1;
    buf[4] = (uint8_t)(opcode & 0xff);
    buf[5] = (uint8_t)(opcode >> 8);
    if (ret_len) memcpy(buf + 6, ret, ret_len);
    send_hci(ll, buf, 6 + ret_len);
}

static void cmd_complete_status(struct vbt_ll *ll, uint16_t opcode,
                                uint8_t status)
{
    cmd_complete(ll, opcode, &status, 1);
}

static void cmd_status(struct vbt_ll *ll, uint16_t opcode, uint8_t status)
{
    uint8_t buf[7];
    buf[0] = HCI_EVT_PKT;
    buf[1] = EVT_CMD_STATUS;
    buf[2] = 4;
    buf[3] = status;
    buf[4] = ll->num_hci_cmd ? ll->num_hci_cmd : 1;
    buf[5] = (uint8_t)(opcode & 0xff);
    buf[6] = (uint8_t)(opcode >> 8);
    send_hci(ll, buf, 7);
}

static void evt_le_conn_complete(struct vbt_ll *ll, struct ll_conn *c,
                                 uint8_t status)
{
    uint8_t p[24];
    int n = 0;
    p[n++] = HCI_EVT_PKT;
    p[n++] = EVT_LE_META;
    p[n++] = 0;                       /* param len, patched below */
    p[n++] = SUB_LE_CONN_COMPLETE;
    p[n++] = status;
    p[n++] = (uint8_t)(c->handle & 0xff);
    p[n++] = (uint8_t)(c->handle >> 8);
    p[n++] = c->role;
    p[n++] = c->peer_addr_type;
    memcpy(p + n, c->peer_addr, 6); n += 6;
    p[n++] = 0x18; p[n++] = 0x00;     /* conn interval = 0x0018 (30 ms) */
    p[n++] = 0x00; p[n++] = 0x00;     /* latency */
    p[n++] = 0x2a; p[n++] = 0x00;     /* supervision timeout = 0x002a */
    p[n++] = 0x00;                    /* central clock accuracy */
    p[2] = (uint8_t)(n - 3);
    send_hci(ll, p, n);
}

static void evt_disconn_complete(struct vbt_ll *ll, uint16_t handle,
                                 uint8_t reason)
{
    uint8_t p[7];
    p[0] = HCI_EVT_PKT;
    p[1] = EVT_DISCONN_COMPLETE;
    p[2] = 4;
    p[3] = HCI_SUCCESS;
    p[4] = (uint8_t)(handle & 0xff);
    p[5] = (uint8_t)(handle >> 8);
    p[6] = reason;
    send_hci(ll, p, 7);
}

static void evt_num_completed(struct vbt_ll *ll, uint16_t handle, uint16_t n)
{
    uint8_t p[10];
    p[0] = HCI_EVT_PKT;
    p[1] = EVT_NUM_COMPLETED_PACKETS;
    p[2] = 5;
    p[3] = 1;                          /* number of handles */
    p[4] = (uint8_t)(handle & 0xff);
    p[5] = (uint8_t)(handle >> 8);
    p[6] = (uint8_t)(n & 0xff);
    p[7] = (uint8_t)(n >> 8);
    send_hci(ll, p, 8);
}

static void evt_encrypt_change(struct vbt_ll *ll, uint16_t handle,
                               uint8_t enabled)
{
    uint8_t p[7];
    p[0] = HCI_EVT_PKT;
    p[1] = EVT_ENCRYPT_CHANGE;
    p[2] = 4;
    p[3] = HCI_SUCCESS;
    p[4] = (uint8_t)(handle & 0xff);
    p[5] = (uint8_t)(handle >> 8);
    p[6] = enabled;
    send_hci(ll, p, 7);
}

static void evt_le_ltk_request(struct vbt_ll *ll, struct ll_conn *c,
                               const uint8_t *rand8, const uint8_t *ediv2)
{
    uint8_t p[16];
    int n = 0;
    p[n++] = HCI_EVT_PKT;
    p[n++] = EVT_LE_META;
    p[n++] = 0;
    p[n++] = SUB_LE_LTK_REQUEST;
    p[n++] = (uint8_t)(c->handle & 0xff);
    p[n++] = (uint8_t)(c->handle >> 8);
    memcpy(p + n, rand8, 8); n += 8;
    memcpy(p + n, ediv2, 2); n += 2;
    p[2] = (uint8_t)(n - 3);
    send_hci(ll, p, n);
}

/* ================================================================
 *  Medium PDU emission
 * ================================================================ */
static void fill_hdr(struct vbt_ll *ll, struct vbt_frame_hdr *h,
                     uint8_t ll_type, uint32_t aa, uint8_t channel,
                     const uint8_t *tx_addr, uint8_t tx_addr_type,
                     uint16_t pdu_len)
{
    memset(h, 0, sizeof(*h));
    h->magic = VBT_MAGIC;
    h->version = VBT_VERSION;
    h->pdu_len = pdu_len;
    h->access_addr = aa;
    h->ll_type = ll_type;
    h->channel = channel;
    h->phy = VBT_PHY_1M;
    h->tx_power_dbm = VBT_DEFAULT_TXPOWER;
    h->rssi = VBT_DEFAULT_RSSI;
    memcpy(h->tx_addr, tx_addr, 6);
    h->tx_addr_type = tx_addr_type;
    (void)ll;
}

static void send_adv_pdu(struct vbt_ll *ll, uint8_t channel,
                         const uint8_t *tx_addr, uint8_t tx_addr_type,
                         const uint8_t *pdu, uint16_t pdu_len)
{
    struct vbt_frame_hdr h;
    fill_hdr(ll, &h, VBT_LL_ADV, VBT_ADV_ACCESS_ADDR, channel,
             tx_addr, tx_addr_type, pdu_len);
    if (ll->ops.pdu_to_medium)
        ll->ops.pdu_to_medium(ll->ops.ctx, &h, pdu, pdu_len);
}

static void send_data_pdu(struct vbt_ll *ll, struct ll_conn *c,
                          const uint8_t *pdu, uint16_t pdu_len)
{
    struct vbt_frame_hdr h;
    const uint8_t *src = own_source_addr(ll, c->role == 1 ?
                                         ll->adv_own_addr_type :
                                         ll->init_own_addr_type);
    uint8_t stype = own_source_addr_type(ll, c->role == 1 ?
                                          ll->adv_own_addr_type :
                                          ll->init_own_addr_type);
    fill_hdr(ll, &h, VBT_LL_DATA, c->access_addr, 5, src, stype, pdu_len);
    h.conn_handle = c->handle;
    if (ll->ops.pdu_to_medium)
        ll->ops.pdu_to_medium(ll->ops.ctx, &h, pdu, pdu_len);
}

/* Send an LL control PDU on a connection. */
static void send_ll_control(struct vbt_ll *ll, struct ll_conn *c,
                            uint8_t opcode, const uint8_t *params,
                            uint8_t plen)
{
    uint8_t pdu[64];
    pdu[0] = LLID_CONTROL;
    pdu[1] = (uint8_t)(1 + plen);
    pdu[2] = opcode;
    if (plen) memcpy(pdu + 3, params, plen);
    send_data_pdu(ll, c, pdu, 2 + 1 + plen);
}

/* ================================================================
 *  Advertising
 * ================================================================ */
static void emit_advertisement(struct vbt_ll *ll)
{
    const uint8_t *adva = own_source_addr(ll, ll->adv_own_addr_type);
    uint8_t atype = own_source_addr_type(ll, ll->adv_own_addr_type);

    uint8_t pdu[2 + 6 + 31];
    uint8_t type = VBT_ADV_IND;
    if (ll->adv_type == 0x02) type = VBT_ADV_NONCONN_IND;
    else if (ll->adv_type == 0x03) type = VBT_ADV_NONCONN_IND;   /* NONCONN */
    else if (ll->adv_type == 0x06) type = VBT_ADV_SCAN_IND;

    pdu[0] = (uint8_t)(type | (atype ? (1u << 6) : 0));
    pdu[1] = (uint8_t)(6 + ll->adv_data_len);
    memcpy(pdu + 2, adva, 6);
    if (ll->adv_data_len) memcpy(pdu + 8, ll->adv_data, ll->adv_data_len);
    uint16_t plen = (uint16_t)(2 + 6 + ll->adv_data_len);

    /* Advertise on all three primary channels. The medium broadcasts
     * regardless of channel, but stamping each keeps surveys honest. */
    send_adv_pdu(ll, VBT_CHAN_ADV_37, adva, atype, pdu, plen);
    send_adv_pdu(ll, VBT_CHAN_ADV_38, adva, atype, pdu, plen);
    send_adv_pdu(ll, VBT_CHAN_ADV_39, adva, atype, pdu, plen);
}

static void send_scan_rsp(struct vbt_ll *ll, const uint8_t *scanner)
{
    (void)scanner;
    const uint8_t *adva = own_source_addr(ll, ll->adv_own_addr_type);
    uint8_t atype = own_source_addr_type(ll, ll->adv_own_addr_type);
    uint8_t pdu[2 + 6 + 31];
    pdu[0] = (uint8_t)(VBT_SCAN_RSP | (atype ? (1u << 6) : 0));
    pdu[1] = (uint8_t)(6 + ll->scan_rsp_data_len);
    memcpy(pdu + 2, adva, 6);
    if (ll->scan_rsp_data_len)
        memcpy(pdu + 8, ll->scan_rsp_data, ll->scan_rsp_data_len);
    send_adv_pdu(ll, VBT_CHAN_ADV_37, adva, atype, pdu,
                 (uint16_t)(2 + 6 + ll->scan_rsp_data_len));
}

/* ================================================================
 *  Scanning / initiating: report an advertisement to the host
 * ================================================================ */
static void report_adv(struct vbt_ll *ll, uint8_t event_type,
                       uint8_t addr_type, const uint8_t *addr,
                       const uint8_t *data, uint8_t data_len, int8_t rssi)
{
    uint8_t p[2 + 1 + 1 + 1 + 1 + 6 + 1 + 31 + 1];
    int n = 0;
    p[n++] = HCI_EVT_PKT;
    p[n++] = EVT_LE_META;
    p[n++] = 0;                       /* len patched */
    p[n++] = SUB_LE_ADV_REPORT;
    p[n++] = 1;                       /* num reports */
    p[n++] = event_type;
    p[n++] = addr_type;
    memcpy(p + n, addr, 6); n += 6;
    p[n++] = data_len;
    if (data_len) { memcpy(p + n, data, data_len); n += data_len; }
    p[n++] = (uint8_t)rssi;
    p[2] = (uint8_t)(n - 3);
    send_hci(ll, p, n);
}

static void send_connect_ind(struct vbt_ll *ll, const uint8_t *adva,
                             uint8_t adva_type, struct ll_conn *c)
{
    const uint8_t *inita = own_source_addr(ll, ll->init_own_addr_type);
    uint8_t itype = own_source_addr_type(ll, ll->init_own_addr_type);

    uint8_t pdu[2 + 6 + 6 + 22];
    memset(pdu, 0, sizeof(pdu));
    pdu[0] = (uint8_t)(VBT_CONNECT_IND
                       | (itype ? (1u << 6) : 0)
                       | (adva_type ? (1u << 7) : 0));
    pdu[1] = 34;
    memcpy(pdu + 2, inita, 6);
    memcpy(pdu + 8, adva, 6);
    /* LLData: Access Address (LE) then connection parameters. */
    memcpy(pdu + 14, &c->access_addr, 4);
    /* CRCInit (3), WinSize (1), WinOffset (2), Interval (2), Latency (2),
     * Timeout (2), ChM (5), Hop/SCA (1) — plausible constants. */
    pdu[18] = 0x55; pdu[19] = 0x55; pdu[20] = 0x55;   /* CRCInit */
    pdu[21] = 0x03;                                   /* WinSize */
    pdu[22] = 0x00; pdu[23] = 0x00;                   /* WinOffset */
    pdu[24] = 0x18; pdu[25] = 0x00;                   /* Interval = 30 ms */
    pdu[26] = 0x00; pdu[27] = 0x00;                   /* Latency */
    pdu[28] = 0x2a; pdu[29] = 0x00;                   /* Timeout */
    pdu[30] = 0xff; pdu[31] = 0xff; pdu[32] = 0xff;   /* ChM */
    pdu[33] = 0xff; pdu[34] = 0x1f;
    pdu[35] = 0x00;                                   /* Hop/SCA */
    send_adv_pdu(ll, VBT_CHAN_ADV_37, inita, itype, pdu, 36);
}

/* ================================================================
 *  ACL data: host <-> medium
 * ================================================================ */
static void acl_to_host(struct vbt_ll *ll, uint16_t handle,
                        const uint8_t *l2cap, uint16_t l2len)
{
    uint8_t p[4 + LE_ACL_MTU + 16];
    if (l2len > LE_ACL_MTU + 4) return;
    uint16_t hf = (uint16_t)(handle | (0x2u << 12));   /* PB = 10b: first */
    p[0] = HCI_ACL_PKT;
    p[1] = (uint8_t)(hf & 0xff);
    p[2] = (uint8_t)(hf >> 8);
    p[3] = (uint8_t)(l2len & 0xff);
    p[4] = (uint8_t)(l2len >> 8);
    memcpy(p + 5, l2cap, l2len);
    send_hci(ll, p, 5 + l2len);
}

static void handle_acl_from_host(struct vbt_ll *ll, const uint8_t *pkt,
                                 size_t len)
{
    if (len < 5) return;
    uint16_t hf = (uint16_t)(pkt[1] | (pkt[2] << 8));
    uint16_t handle = hf & 0x0fff;
    uint16_t dlen = (uint16_t)(pkt[3] | (pkt[4] << 8));
    if ((size_t)(5 + dlen) > len) dlen = (uint16_t)(len - 5);

    struct ll_conn *c = conn_by_handle(ll, handle);
    if (!c) return;

    /* Wrap the L2CAP payload in one LL data PDU (LLID = start). The
     * virtual medium is reliable and point-to-point, so no 27-byte
     * fragmentation is needed within VBT_MAX_PDU_SIZE. */
    uint8_t pdu[2 + LE_ACL_MTU + 8];
    if (dlen > LE_ACL_MTU) dlen = LE_ACL_MTU;
    pdu[0] = LLID_START;
    pdu[1] = (uint8_t)dlen;
    memcpy(pdu + 2, pkt + 5, dlen);
    send_data_pdu(ll, c, pdu, (uint16_t)(2 + dlen));

    /* Free the host's buffer immediately. */
    evt_num_completed(ll, handle, 1);
}

/* ================================================================
 *  LL control PDU handling (RX)
 * ================================================================ */
static void handle_ll_control(struct vbt_ll *ll, struct ll_conn *c,
                              const uint8_t *payload, uint8_t plen)
{
    if (plen < 1) return;
    uint8_t op = payload[0];
    switch (op) {
    case LL_TERMINATE_IND: {
        uint8_t reason = (plen >= 2) ? payload[1] : HCI_REMOTE_USER_TERM;
        uint16_t h = c->handle;
        c->active = false;
        evt_disconn_complete(ll, h, reason);
        break;
    }
    case LL_VERSION_IND: {
        /* Reply once with our own version. */
        uint8_t v[5] = { 0x0c, 0x0f, 0x00, 0x0f, 0x00 };  /* ver 5.2-ish */
        send_ll_control(ll, c, LL_VERSION_IND, v, sizeof(v));
        break;
    }
    case LL_FEATURE_REQ:
    case LL_SLAVE_FEATURE_REQ: {
        uint8_t feat[8] = { 0x01, 0, 0, 0, 0, 0, 0, 0 };  /* LE Encryption */
        send_ll_control(ll, c, LL_FEATURE_RSP, feat, sizeof(feat));
        break;
    }
    case LL_ENC_REQ: {
        /* Peripheral side. Ask the host for the LTK, then continue the
         * (pass-through) start-encryption handshake. Rand/EDIV are the
         * middle 8+2 bytes of the request payload. */
        c->encrypting = true;
        uint8_t rand8[8] = {0}, ediv[2] = {0};
        if (plen >= 1 + 8 + 2 + 8 + 4) {  /* Rand(8) EDIV(2) SKDm(8) IVm(4) */
            memcpy(rand8, payload + 1, 8);
            memcpy(ediv, payload + 9, 2);
        }
        uint8_t rsp[12] = {0};            /* SKDs(8) IVs(4) */
        send_ll_control(ll, c, LL_ENC_RSP, rsp, sizeof(rsp));
        evt_le_ltk_request(ll, c, rand8, ediv);
        break;
    }
    case LL_ENC_RSP:
        /* Central side: peer accepted; wait for START_ENC_REQ. */
        break;
    case LL_START_ENC_REQ:
        /* Central receives this from the peripheral: reply RSP and mark
         * the link encrypted. */
        send_ll_control(ll, c, LL_START_ENC_RSP, NULL, 0);
        if (!c->encrypted) {
            c->encrypted = true;
            evt_encrypt_change(ll, c->handle, 1);
        }
        break;
    case LL_START_ENC_RSP:
        /* Peripheral receives the central's confirmation. */
        if (!c->encrypted) {
            c->encrypted = true;
            evt_encrypt_change(ll, c->handle, 1);
        }
        break;
    case LL_CONNECTION_UPDATE_IND:
    case LL_CHANNEL_MAP_IND:
    default:
        break;
    }
}

/* ================================================================
 *  HCI command dispatch
 * ================================================================ */
static void handle_le_create_conn(struct vbt_ll *ll, const uint8_t *params,
                                  uint8_t plen)
{
    /* LE Create Connection params: ScanInterval(2) ScanWindow(2)
     * FilterPolicy(1) PeerAddrType(1) PeerAddr(6) OwnAddrType(1) ... */
    if (plen < 13) { cmd_status(ll, OP_LE_CREATE_CONN, HCI_UNKNOWN_COMMAND); return; }
    ll->init_peer_addr_type = params[5];
    memcpy(ll->init_peer_addr, params + 6, 6);
    ll->init_own_addr_type = params[12];
    ll->state = LL_INITIATING;
    cmd_status(ll, OP_LE_CREATE_CONN, HCI_SUCCESS);
}

static void dispatch_command(struct vbt_ll *ll, const uint8_t *pkt, size_t len)
{
    if (len < 4) return;
    uint16_t opcode = (uint16_t)(pkt[1] | (pkt[2] << 8));
    uint8_t plen = pkt[3];
    const uint8_t *params = pkt + 4;
    if ((size_t)(4 + plen) > len) plen = (uint8_t)(len - 4);

    switch (opcode) {
    case OP_RESET:
        memset(ll->conns, 0, sizeof(ll->conns));
        ll->state = LL_IDLE;
        ll->adv_enabled = ll->scan_enabled = false;
        cmd_complete_status(ll, opcode, HCI_SUCCESS);
        break;

    case OP_SET_EVENT_MASK:
    case OP_SET_EVENT_MASK_PAGE2:
    case OP_LE_SET_EVENT_MASK:
    case OP_LE_SET_HOST_CHAN_CLASS:
    case OP_LE_CLEAR_WHITE_LIST:
    case OP_LE_ADD_WHITE_LIST:
    case OP_LE_REMOVE_WHITE_LIST:
        cmd_complete_status(ll, opcode, HCI_SUCCESS);
        break;

    case OP_READ_LOCAL_VERSION: {
        uint8_t r[9] = { HCI_SUCCESS,
                         0x0c,               /* HCI version 5.3 */
                         0x00, 0x00,         /* HCI revision */
                         0x0c,               /* LMP version 5.3 */
                         0x3f, 0x00,         /* manufacturer (test) */
                         0x00, 0x00 };       /* LMP subversion */
        cmd_complete(ll, opcode, r, sizeof(r));
        break;
    }
    case OP_READ_LOCAL_COMMANDS: {
        /* Permissive: claim support for the commands we implement (and
         * some we no-op). A genuinely-unhandled command still returns
         * Unknown Command below, which BlueZ tolerates. */
        uint8_t r[65];
        r[0] = HCI_SUCCESS;
        memset(r + 1, 0xff, 64);
        cmd_complete(ll, opcode, r, sizeof(r));
        break;
    }
    case OP_READ_LOCAL_FEATURES:
    case OP_LE_READ_LOCAL_FEATURES: {
        uint8_t r[9] = { HCI_SUCCESS, 0, 0, 0, 0, 0, 0, 0, 0 };
        if (opcode == OP_LE_READ_LOCAL_FEATURES)
            r[1] = 0x01;                    /* LE Encryption */
        else
            r[5] = 0x60;                    /* BR/EDR: LE supported bits */
        cmd_complete(ll, opcode, r, sizeof(r));
        break;
    }
    case OP_READ_BUFFER_SIZE: {
        uint8_t r[8] = { HCI_SUCCESS,
                         LE_ACL_MTU & 0xff, LE_ACL_MTU >> 8,
                         0,                 /* SCO len */
                         LE_ACL_PKTS, 0,    /* ACL pkts */
                         0, 0 };            /* SCO pkts */
        cmd_complete(ll, opcode, r, sizeof(r));
        break;
    }
    case OP_LE_READ_BUFFER_SIZE: {
        uint8_t r[4] = { HCI_SUCCESS,
                         LE_ACL_MTU & 0xff, LE_ACL_MTU >> 8, LE_ACL_PKTS };
        cmd_complete(ll, opcode, r, sizeof(r));
        break;
    }
    case OP_READ_BD_ADDR: {
        uint8_t r[7];
        r[0] = HCI_SUCCESS;
        memcpy(r + 1, ll->public_addr, 6);
        cmd_complete(ll, opcode, r, sizeof(r));
        break;
    }
    case OP_LE_READ_ADV_TX_POWER: {
        uint8_t r[2] = { HCI_SUCCESS, 0x00 };
        cmd_complete(ll, opcode, r, sizeof(r));
        break;
    }
    case OP_LE_READ_WHITE_LIST_SIZE: {
        uint8_t r[2] = { HCI_SUCCESS, 8 };
        cmd_complete(ll, opcode, r, sizeof(r));
        break;
    }
    case OP_LE_READ_SUPPORTED_STATES: {
        uint8_t r[9] = { HCI_SUCCESS, 0xff, 0xff, 0xff, 0xff,
                         0xff, 0xff, 0xff, 0x03 };
        cmd_complete(ll, opcode, r, sizeof(r));
        break;
    }
    case OP_LE_SET_RANDOM_ADDRESS:
        if (plen >= 6) { memcpy(ll->random_addr, params, 6); ll->random_addr_set = true; }
        cmd_complete_status(ll, opcode, HCI_SUCCESS);
        break;

    case OP_LE_SET_ADV_PARAMETERS:
        if (plen >= 15) {
            ll->adv_interval_ms = 100;   /* nominal; timing is virtual */
            ll->adv_type = params[4];
            ll->adv_own_addr_type = params[5];
            ll->adv_direct_addr_type = params[6];
            memcpy(ll->adv_direct_addr, params + 7, 6);
        }
        cmd_complete_status(ll, opcode, HCI_SUCCESS);
        break;

    case OP_LE_SET_ADV_DATA:
        if (plen >= 1) {
            ll->adv_data_len = params[0] > 31 ? 31 : params[0];
            memcpy(ll->adv_data, params + 1, ll->adv_data_len);
        }
        cmd_complete_status(ll, opcode, HCI_SUCCESS);
        break;

    case OP_LE_SET_SCAN_RSP_DATA:
        if (plen >= 1) {
            ll->scan_rsp_data_len = params[0] > 31 ? 31 : params[0];
            memcpy(ll->scan_rsp_data, params + 1, ll->scan_rsp_data_len);
        }
        cmd_complete_status(ll, opcode, HCI_SUCCESS);
        break;

    case OP_LE_SET_ADV_ENABLE:
        ll->adv_enabled = (plen >= 1 && params[0]);
        if (ll->adv_enabled) { ll->state = LL_ADVERTISING; ll->last_adv_ms = 0; }
        else if (ll->state == LL_ADVERTISING) ll->state = LL_IDLE;
        cmd_complete_status(ll, opcode, HCI_SUCCESS);
        break;

    case OP_LE_SET_SCAN_PARAMETERS:
        if (plen >= 1) {
            ll->scan_type = params[0];
            if (plen >= 6) ll->scan_own_addr_type = params[5];
        }
        cmd_complete_status(ll, opcode, HCI_SUCCESS);
        break;

    case OP_LE_SET_SCAN_ENABLE:
        ll->scan_enabled = (plen >= 1 && params[0]);
        if (ll->scan_enabled) ll->state = LL_SCANNING;
        else if (ll->state == LL_SCANNING) ll->state = LL_IDLE;
        cmd_complete_status(ll, opcode, HCI_SUCCESS);
        break;

    case OP_LE_CREATE_CONN:
        handle_le_create_conn(ll, params, plen);
        break;

    case OP_LE_CREATE_CONN_CANCEL:
        if (ll->state == LL_INITIATING) ll->state = LL_IDLE;
        cmd_complete_status(ll, opcode, HCI_SUCCESS);
        /* Spec: also send LE Connection Complete with Unknown Conn ID. */
        break;

    case OP_LE_CONN_UPDATE: {
        cmd_status(ll, opcode, HCI_SUCCESS);
        if (plen >= 2) {
            uint16_t h = (uint16_t)(params[0] | (params[1] << 8));
            struct ll_conn *c = conn_by_handle(ll, h);
            if (c) {
                uint8_t p[16];
                p[0] = HCI_EVT_PKT; p[1] = EVT_LE_META; p[2] = 0;
                int n = 3;
                p[n++] = SUB_LE_CONN_UPDATE_COMPLETE;
                p[n++] = HCI_SUCCESS;
                p[n++] = (uint8_t)(h & 0xff); p[n++] = (uint8_t)(h >> 8);
                p[n++] = 0x18; p[n++] = 0x00;   /* interval */
                p[n++] = 0x00; p[n++] = 0x00;   /* latency */
                p[n++] = 0x2a; p[n++] = 0x00;   /* timeout */
                p[2] = (uint8_t)(n - 3);
                send_hci(ll, p, n);
            }
        }
        break;
    }

    case OP_LE_READ_REMOTE_FEATURES: {
        cmd_status(ll, opcode, HCI_SUCCESS);
        if (plen >= 2) {
            uint16_t h = (uint16_t)(params[0] | (params[1] << 8));
            uint8_t p[16];
            int n = 0;
            p[n++] = HCI_EVT_PKT; p[n++] = EVT_LE_META; p[n++] = 0;
            p[n++] = SUB_LE_READ_REMOTE_FEAT_CMPL;
            p[n++] = HCI_SUCCESS;
            p[n++] = (uint8_t)(h & 0xff); p[n++] = (uint8_t)(h >> 8);
            p[n++] = 0x01; for (int i = 0; i < 7; i++) p[n++] = 0x00;
            p[2] = (uint8_t)(n - 3);
            send_hci(ll, p, n);
        }
        break;
    }

    case OP_LE_RAND: {
        uint8_t r[9];
        r[0] = HCI_SUCCESS;
        uint32_t a = ll_rand32(ll), b = ll_rand32(ll);
        memcpy(r + 1, &a, 4);
        memcpy(r + 5, &b, 4);
        cmd_complete(ll, opcode, r, sizeof(r));
        break;
    }
    case OP_LE_ENCRYPT: {
        /* No real crypto on a trusted medium — echo the plaintext block so
         * the host's key-generation flow proceeds deterministically. */
        uint8_t r[17];
        r[0] = HCI_SUCCESS;
        if (plen >= 32) memcpy(r + 1, params + 16, 16);
        else memset(r + 1, 0, 16);
        cmd_complete(ll, opcode, r, sizeof(r));
        break;
    }

    case OP_LE_START_ENCRYPTION: {
        /* Central starts encryption: kick off the LL handshake. */
        cmd_status(ll, opcode, HCI_SUCCESS);
        if (plen >= 2) {
            uint16_t h = (uint16_t)(params[0] | (params[1] << 8));
            struct ll_conn *c = conn_by_handle(ll, h);
            if (c) {
                c->encrypting = true;
                uint8_t req[22] = {0};   /* Rand(8) EDIV(2) SKDm(8) IVm(4) */
                if (plen >= 28) memcpy(req, params + 2, 10);
                send_ll_control(ll, c, LL_ENC_REQ, req, sizeof(req));
            }
        }
        break;
    }

    case OP_LE_LTK_REQ_REPLY: {
        /* Peripheral host supplied the LTK: continue the handshake by
         * sending START_ENC_REQ to the central. */
        uint16_t h = 0;
        if (plen >= 2) h = (uint16_t)(params[0] | (params[1] << 8));
        struct ll_conn *c = conn_by_handle(ll, h);
        uint8_t r[3];
        r[0] = HCI_SUCCESS;
        r[1] = (uint8_t)(h & 0xff); r[2] = (uint8_t)(h >> 8);
        cmd_complete(ll, opcode, r, sizeof(r));
        if (c) send_ll_control(ll, c, LL_START_ENC_REQ, NULL, 0);
        break;
    }
    case OP_LE_LTK_REQ_NEG_REPLY: {
        uint16_t h = 0;
        if (plen >= 2) h = (uint16_t)(params[0] | (params[1] << 8));
        uint8_t r[3] = { HCI_SUCCESS, (uint8_t)(h & 0xff), (uint8_t)(h >> 8) };
        cmd_complete(ll, opcode, r, sizeof(r));
        break;
    }

    case OP_DISCONNECT: {
        cmd_status(ll, opcode, HCI_SUCCESS);
        if (plen >= 3) {
            uint16_t h = (uint16_t)(params[0] | (params[1] << 8));
            uint8_t reason = params[2];
            struct ll_conn *c = conn_by_handle(ll, h);
            if (c) {
                uint8_t rb[1] = { reason };
                send_ll_control(ll, c, LL_TERMINATE_IND, rb, 1);
                c->active = false;
                evt_disconn_complete(ll, h, HCI_CONN_TERM_LOCAL_HOST);
            }
        }
        break;
    }

    case OP_READ_REMOTE_VERSION: {
        cmd_status(ll, opcode, HCI_SUCCESS);
        if (plen >= 2) {
            uint16_t h = (uint16_t)(params[0] | (params[1] << 8));
            uint8_t p[11];
            p[0] = HCI_EVT_PKT; p[1] = EVT_READ_REMOTE_VERSION_CMPL; p[2] = 8;
            p[3] = HCI_SUCCESS;
            p[4] = (uint8_t)(h & 0xff); p[5] = (uint8_t)(h >> 8);
            p[6] = 0x0c; p[7] = 0x3f; p[8] = 0x00; p[9] = 0x00; p[10] = 0x00;
            send_hci(ll, p, 11);
        }
        break;
    }

    default:
        /* Unknown / unimplemented command. */
        if ((opcode >> 10) == 0x08 || (opcode >> 10) == 0x03 ||
            (opcode >> 10) == 0x04)
            cmd_complete_status(ll, opcode, HCI_UNKNOWN_COMMAND);
        else
            cmd_complete_status(ll, opcode, HCI_UNKNOWN_COMMAND);
        break;
    }
}

/* ================================================================
 *  Public entry points
 * ================================================================ */
struct vbt_ll *vbt_ll_new(const struct vbt_ll_ops *ops, const uint8_t bdaddr[6])
{
    struct vbt_ll *ll = calloc(1, sizeof(*ll));
    if (!ll) return NULL;
    ll->ops = *ops;
    memcpy(ll->public_addr, bdaddr, 6);
    ll->num_hci_cmd = 1;
    ll->adv_interval_ms = ADV_INTERVAL_MS_DEFAULT;
    /* Seed the PRNG from the address so Access Addresses differ per node. */
    ll->next_aa = 0x811c9dc5u;
    for (int i = 0; i < 6; i++) ll->next_aa = ll->next_aa * 33u + bdaddr[i];
    return ll;
}

void vbt_ll_free(struct vbt_ll *ll) { free(ll); }

const uint8_t *vbt_ll_bdaddr(const struct vbt_ll *ll) { return ll->public_addr; }

void vbt_ll_hci_from_host(struct vbt_ll *ll, const uint8_t *pkt, size_t len)
{
    if (len < 1) return;
    switch (pkt[0]) {
    case HCI_CMD_PKT: dispatch_command(ll, pkt, len); break;
    case HCI_ACL_PKT: handle_acl_from_host(ll, pkt, len); break;
    default: break;   /* SCO not supported */
    }
}

void vbt_ll_pdu_from_medium(struct vbt_ll *ll, const struct vbt_frame_hdr *hdr,
                            const uint8_t *pdu, size_t pdu_len)
{
    if (pdu_len < 2) return;

    if (hdr->ll_type == VBT_LL_ADV) {
        uint8_t type = pdu[0] & VBT_ADV_PDU_TYPE_MASK;
        uint8_t txadd = (pdu[0] & (1u << 6)) ? 1 : 0;
        uint8_t body_len = pdu[1];
        const uint8_t *body = pdu + 2;
        if ((size_t)(2 + body_len) > pdu_len) body_len = (uint8_t)(pdu_len - 2);

        switch (type) {
        case VBT_ADV_IND:
        case VBT_ADV_DIRECT_IND:
        case VBT_ADV_NONCONN_IND:
        case VBT_ADV_SCAN_IND: {
            if (body_len < 6) return;
            const uint8_t *adva = body;
            const uint8_t *ad = body + 6;
            uint8_t ad_len = (uint8_t)(body_len - 6);

            /* Initiating: is this our target advertiser? */
            if (ll->state == LL_INITIATING &&
                (type == VBT_ADV_IND || type == VBT_ADV_DIRECT_IND) &&
                memcmp(adva, ll->init_peer_addr, 6) == 0) {
                struct ll_conn *c = conn_alloc(ll);
                if (c) {
                    c->access_addr = (ll_rand32(ll) & 0x7fffffff) | 0x00400000;
                    if (c->access_addr == VBT_ADV_ACCESS_ADDR) c->access_addr ^= 0x1;
                    c->role = 0;                     /* central */
                    c->peer_addr_type = txadd;
                    memcpy(c->peer_addr, adva, 6);
                    send_connect_ind(ll, adva, txadd, c);
                    ll->state = LL_IDLE;
                    ll->scan_enabled = false;
                    evt_le_conn_complete(ll, c, HCI_SUCCESS);
                }
                return;
            }

            /* Scanning: report to host. */
            if (ll->scan_enabled) {
                uint8_t evt = (type == VBT_ADV_IND) ? 0x00 :
                              (type == VBT_ADV_DIRECT_IND) ? 0x01 :
                              (type == VBT_ADV_SCAN_IND) ? 0x02 : 0x03;
                report_adv(ll, evt, txadd, adva, ad, ad_len, hdr->rssi);
                /* Active scanning: solicit a scan response. */
                if (ll->scan_type == 1 &&
                    (type == VBT_ADV_IND || type == VBT_ADV_SCAN_IND)) {
                    const uint8_t *sa =
                        own_source_addr(ll, ll->scan_own_addr_type);
                    uint8_t st = own_source_addr_type(ll, ll->scan_own_addr_type);
                    uint8_t req[2 + 12];
                    req[0] = (uint8_t)(VBT_SCAN_REQ | (st ? (1u << 6) : 0)
                                       | (txadd ? (1u << 7) : 0));
                    req[1] = 12;
                    memcpy(req + 2, sa, 6);
                    memcpy(req + 8, adva, 6);
                    send_adv_pdu(ll, VBT_CHAN_ADV_37, sa, st, req, 14);
                }
            }
            return;
        }
        case VBT_SCAN_REQ: {
            /* We are advertising: reply if the request targets us. */
            if (ll->adv_enabled && body_len >= 12) {
                const uint8_t *adva = body + 6;
                const uint8_t *self = own_source_addr(ll, ll->adv_own_addr_type);
                if (memcmp(adva, self, 6) == 0)
                    send_scan_rsp(ll, body);
            }
            return;
        }
        case VBT_SCAN_RSP: {
            if (ll->scan_enabled && body_len >= 6) {
                const uint8_t *adva = body;
                const uint8_t *ad = body + 6;
                report_adv(ll, 0x04, txadd, adva, ad,
                           (uint8_t)(body_len - 6), hdr->rssi);
            }
            return;
        }
        case VBT_CONNECT_IND: {
            /* We are advertising: did someone connect to us? */
            if (!ll->adv_enabled || body_len < 34) return;
            const uint8_t *inita = body;
            const uint8_t *adva = body + 6;
            const uint8_t *self = own_source_addr(ll, ll->adv_own_addr_type);
            if (memcmp(adva, self, 6) != 0) return;
            struct ll_conn *c = conn_alloc(ll);
            if (!c) return;
            memcpy(&c->access_addr, body + 12, 4);
            c->role = 1;                             /* peripheral */
            c->peer_addr_type = (pdu[0] & (1u << 6)) ? 1 : 0;
            memcpy(c->peer_addr, inita, 6);
            ll->adv_enabled = false;
            ll->state = LL_IDLE;
            evt_le_conn_complete(ll, c, HCI_SUCCESS);
            return;
        }
        default:
            return;
        }
    }

    if (hdr->ll_type == VBT_LL_DATA) {
        struct ll_conn *c = conn_by_aa(ll, hdr->access_addr);
        if (!c) return;
        uint8_t llid = pdu[0] & 0x03;
        uint8_t body_len = pdu[1];
        if ((size_t)(2 + body_len) > pdu_len) body_len = (uint8_t)(pdu_len - 2);
        if (llid == LLID_CONTROL) {
            handle_ll_control(ll, c, pdu + 2, body_len);
        } else if (llid == LLID_START || llid == LLID_CONT) {
            if (body_len > 0)
                acl_to_host(ll, c->handle, pdu + 2, body_len);
        }
        return;
    }
}

void vbt_ll_tick(struct vbt_ll *ll, uint64_t now_ms)
{
    if (ll->adv_enabled) {
        if (ll->last_adv_ms == 0 ||
            now_ms - ll->last_adv_ms >= ll->adv_interval_ms) {
            emit_advertisement(ll);
            ll->last_adv_ms = now_ms;
        }
    }
}
