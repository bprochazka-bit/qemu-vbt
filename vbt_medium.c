/*
 * vbt-medium — Virtual Bluetooth (BLE) Medium Hub
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A userspace fan-out process that connects virtual BLE controllers
 * (QEMU -device vbt-virtio instances, or the vbt-controller vhci daemon)
 * over Unix-domain sockets, modelling the 2.4 GHz BLE broadcast domain.
 *
 * Responsibilities:
 *   - Fan advertising-channel PDUs out to every node (subject to a
 *     log-distance propagation model when node positions are set).
 *   - Learn device addresses from advertising PDUs, snoop CONNECT_IND to
 *     build a per-Access-Address connection table, and route data-channel
 *     PDUs point-to-point between the two connection endpoints.
 *   - Expose a text control socket for positioning nodes, pinning
 *     per-link RSSI / loss, and inspecting peers, connections and stats.
 *
 * The Link Layer and the entire host stack (L2CAP/ATT/GATT/SMP) live in
 * the nodes; the hub is deliberately dumb about everything above the PDU
 * routing needed to make a connection point-to-point.
 *
 * Usage:
 *   ./vbt-medium <unix-socket-path> [options]
 *
 * See usage() for options, and README.md for the full picture.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <math.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "vbt.h"

/* ================================================================
 *  Limits
 * ================================================================ */
#define MAX_PEERS            300   /* headroom above the 50-100 VM target */
#define MAX_NODES            300
#define MAX_CONNS            512
#define MAX_ADDRS_PER_NODE   8
#define MAX_OVERRIDES        1024
#define MAX_CTL_CLIENTS      16
#define MAX_UPSTREAMS        16
#define NODE_ID_LEN          64

#define RECV_BUF_SIZE        VBT_RXBUF_SIZE
#define SEND_BUF_SIZE        (256 * 1024)   /* per-peer outbound backlog */
#define CTL_BUF_SIZE         1024
#define CTL_OUTBUF_SIZE      (128 * 1024)

#define UPSTREAM_RETRY_SEC   2

/* ================================================================
 *  Propagation model (log-distance path loss)
 * ================================================================ */
#define PL_REF_DBM           40.0   /* path loss at reference distance d0 */
#define PL_REF_DIST_M        1.0    /* reference distance d0 (metres) */
#define PL_EXPONENT_DEFAULT  2.0    /* free-space-ish exponent */

static double g_path_exponent = PL_EXPONENT_DEFAULT;

/* ================================================================
 *  Node (a named endpoint; survives peer reconnection)
 * ================================================================ */
struct node {
    char        node_id[NODE_ID_LEN];
    uint8_t     addrs[MAX_ADDRS_PER_NODE][6];
    int         num_addrs;
    double      pos_x, pos_y, pos_z;
    bool        pos_set;
    double      tx_power_dbm;   /* overrides per-frame tx_power when set>? */
    bool        tx_power_set;
    double      rx_sens_dbm;
    int         peer_idx;       /* bound peer, or -1 if offline */
    bool        active;
    bool        physical;       /* real radio: bypass propagation model */
    bool        auto_named;
    uint8_t     last_channel;
    uint8_t     last_phy;
    uint64_t    tx_pdus;
    uint64_t    rx_pdus;
    uint64_t    rx_dropped;
};

static struct node nodes[MAX_NODES];
static int num_nodes = 0;
static int next_auto_id = 0;

/* ================================================================
 *  Peer (a live socket connection)
 * ================================================================ */
struct peer {
    int         fd;
    uint8_t     rxbuf[RECV_BUF_SIZE];
    uint32_t    rxused;
    uint8_t    *outbuf;          /* lazily allocated backlog ring */
    uint32_t    outused;
    bool        want_write;
    bool        is_bridge;       /* TCP inter-hub trunk */
    bool        is_physical;     /* hello flagged physical radio */
    char        label[80];
    int         node_idx;        /* index into nodes[], or -1 */
    uint64_t    frames_seen;
    uint64_t    tx_dropped;
};

static struct peer peers[MAX_PEERS];
static int num_peers = 0;

/* ================================================================
 *  Connection (Access Address -> two endpoints)
 * ================================================================ */
struct conn {
    uint32_t    access_addr;
    int         node_a;          /* initiator */
    int         node_b;          /* advertiser (may be -1 if unknown) */
    bool        active;
    uint64_t    pdus;
    uint8_t     inita[6];
    uint8_t     adva[6];
};

static struct conn conns[MAX_CONNS];
static int num_conns = 0;

/* ================================================================
 *  Per-link overrides (pin RSSI or loss probability)
 * ================================================================ */
struct link_override {
    char        a[NODE_ID_LEN], b[NODE_ID_LEN];   /* unordered node-id pair */
    bool        rssi_set;
    int         rssi_dbm;
    bool        loss_set;
    double      loss_prob;       /* 0..1 */
    bool        used;
};

static struct link_override overrides[MAX_OVERRIDES];
static int num_overrides = 0;

/* ================================================================
 *  Listeners / control / upstream state
 * ================================================================ */
static int unix_listen_fd = -1;
static int tcp_listen_fd = -1;
static int ctl_listen_fd = -1;
static char *socket_path = NULL;
static char *ctl_socket_path = NULL;

struct ctl_client {
    int         fd;
    char        buf[CTL_BUF_SIZE];
    uint32_t    buf_used;
    char        outbuf[CTL_OUTBUF_SIZE];
    uint32_t    outbuf_used;
    int         active;   /* 0=free, 1=active, 2=closing after flush */
};
static struct ctl_client ctl_clients[MAX_CTL_CLIENTS];

struct upstream_addr { char host[256]; char port[16]; };
static struct upstream_addr upstreams[MAX_UPSTREAMS];
static int num_upstreams = 0;
struct upstream_state { int peer_idx; time_t last_attempt; };
static struct upstream_state upstream_state[MAX_UPSTREAMS];

/* ================================================================
 *  Global stats
 * ================================================================ */
static uint64_t stat_adv_forwarded = 0;
static uint64_t stat_data_forwarded = 0;
static uint64_t stat_dropped_model = 0;    /* propagation-model drops */
static uint64_t stat_dropped_backpressure = 0;
static uint64_t stat_conns_opened = 0;
static uint64_t stat_conns_closed = 0;
static time_t   stat_start_time = 0;

static volatile sig_atomic_t g_running = 1;

/* ================================================================
 *  Utility
 * ================================================================ */
static void on_signal(int sig) { (void)sig; g_running = 0; }

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool addr_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

static bool addr_is_zero(const uint8_t *a)
{
    static const uint8_t z[6] = {0};
    return addr_eq(a, z);
}

static void fmt_addr(char *out, size_t n, const uint8_t *a)
{
    snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x",
             a[0], a[1], a[2], a[3], a[4], a[5]);
}

/* Parse "aa:bb:cc:dd:ee:ff" into 6 bytes. Returns true on success. */
static bool parse_addr(const char *s, uint8_t *out)
{
    unsigned m[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x",
               &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6)
        return false;
    for (int i = 0; i < 6; i++) {
        if (m[i] > 0xff) return false;
        out[i] = (uint8_t)m[i];
    }
    return true;
}

/* Map a BLE channel index to its RF centre frequency in MHz. */
static int vbt_channel_to_freq(uint8_t ch)
{
    if (ch == VBT_CHAN_ADV_37) return 2402;
    if (ch == VBT_CHAN_ADV_38) return 2426;
    if (ch == VBT_CHAN_ADV_39) return 2480;
    if (ch <= 10) return 2404 + ch * 2;          /* data 0..10  */
    if (ch <= 36) return 2404 + (ch + 1) * 2;    /* data 11..36 (skip 2426) */
    return 0;
}

/* ================================================================
 *  Node table
 * ================================================================ */
static struct node *find_node(const char *nid, bool create)
{
    for (int i = 0; i < num_nodes; i++)
        if (nodes[i].active && strcmp(nodes[i].node_id, nid) == 0)
            return &nodes[i];
    if (!create) return NULL;

    struct node *nd = NULL;
    for (int i = 0; i < num_nodes; i++)
        if (!nodes[i].active) { nd = &nodes[i]; break; }
    if (!nd) {
        if (num_nodes >= MAX_NODES) return NULL;
        nd = &nodes[num_nodes++];
    }
    memset(nd, 0, sizeof(*nd));
    snprintf(nd->node_id, NODE_ID_LEN, "%s", nid);
    nd->peer_idx = -1;
    nd->active = true;
    nd->rx_sens_dbm = VBT_DEFAULT_RX_SENS;
    nd->tx_power_dbm = VBT_DEFAULT_TXPOWER;
    return nd;
}

static int node_index(const struct node *nd)
{
    return nd ? (int)(nd - nodes) : -1;
}

static struct node *find_node_by_addr(const uint8_t *addr)
{
    if (addr_is_zero(addr)) return NULL;
    for (int i = 0; i < num_nodes; i++) {
        if (!nodes[i].active) continue;
        for (int j = 0; j < nodes[i].num_addrs; j++)
            if (addr_eq(nodes[i].addrs[j], addr))
                return &nodes[i];
    }
    return NULL;
}

static void node_add_addr(struct node *nd, const uint8_t *addr)
{
    if (!nd || addr_is_zero(addr)) return;
    for (int j = 0; j < nd->num_addrs; j++)
        if (addr_eq(nd->addrs[j], addr)) return;
    /* An address can migrate between nodes (random resolvable addresses);
     * remove it from any other node first so lookups stay unambiguous. */
    for (int i = 0; i < num_nodes; i++) {
        if (!nodes[i].active || &nodes[i] == nd) continue;
        for (int j = 0; j < nodes[i].num_addrs; j++) {
            if (addr_eq(nodes[i].addrs[j], addr)) {
                memcpy(nodes[i].addrs[j],
                       nodes[i].addrs[nodes[i].num_addrs - 1], 6);
                nodes[i].num_addrs--;
                j--;
            }
        }
    }
    if (nd->num_addrs < MAX_ADDRS_PER_NODE) {
        memcpy(nd->addrs[nd->num_addrs++], addr, 6);
        char s[32]; fmt_addr(s, sizeof(s), addr);
        fprintf(stderr, "hub: node %s: learned addr %s (%d total)\n",
                nd->node_id, s, nd->num_addrs);
    }
}

static struct node *resolve_node(const char *ident)
{
    struct node *nd = find_node(ident, false);
    if (nd) return nd;
    uint8_t a[6];
    if (parse_addr(ident, a)) return find_node_by_addr(a);
    return NULL;
}

/* ================================================================
 *  Connection table (Access Address routing)
 * ================================================================ */
static struct conn *find_conn(uint32_t aa)
{
    if (aa == 0 || aa == VBT_ADV_ACCESS_ADDR) return NULL;
    for (int i = 0; i < num_conns; i++)
        if (conns[i].active && conns[i].access_addr == aa)
            return &conns[i];
    return NULL;
}

static struct conn *conn_alloc(uint32_t aa)
{
    for (int i = 0; i < num_conns; i++)
        if (!conns[i].active) {
            memset(&conns[i], 0, sizeof(conns[i]));
            conns[i].access_addr = aa;
            conns[i].active = true;
            conns[i].node_a = conns[i].node_b = -1;
            return &conns[i];
        }
    if (num_conns >= MAX_CONNS) return NULL;
    struct conn *c = &conns[num_conns++];
    memset(c, 0, sizeof(*c));
    c->access_addr = aa;
    c->active = true;
    c->node_a = c->node_b = -1;
    return c;
}

/* Snoop a CONNECT_IND advertising PDU to build a connection route. */
static void observe_connect_ind(int initiator_node, const uint8_t *pdu,
                                uint32_t len)
{
    if (len < VBT_CONNIND_AA_OFF + 4) return;
    uint8_t inita[6], adva[6];
    memcpy(inita, pdu + VBT_CONNIND_INITA_OFF, 6);
    memcpy(adva, pdu + VBT_CONNIND_ADVA_OFF, 6);
    uint32_t aa;
    memcpy(&aa, pdu + VBT_CONNIND_AA_OFF, 4);   /* little-endian on wire */

    if (aa == 0 || aa == VBT_ADV_ACCESS_ADDR) return;

    struct conn *c = find_conn(aa);
    if (!c) {
        c = conn_alloc(aa);
        if (!c) { fprintf(stderr, "hub: conn table full\n"); return; }
        stat_conns_opened++;
    }
    c->node_a = initiator_node;
    struct node *adv = find_node_by_addr(adva);
    c->node_b = node_index(adv);
    memcpy(c->inita, inita, 6);
    memcpy(c->adva, adva, 6);

    char ia[32], aas[32];
    fmt_addr(ia, sizeof(ia), inita);
    fmt_addr(aas, sizeof(aas), adva);
    fprintf(stderr,
            "hub: conn 0x%08x %s(init) -> %s(adv) [%s <-> %s]\n",
            aa, ia, aas,
            c->node_a >= 0 ? nodes[c->node_a].node_id : "?",
            c->node_b >= 0 ? nodes[c->node_b].node_id : "?(unseen)");
}

/* Tear down all connections that reference a departing node. */
static void conns_drop_node(int node_idx)
{
    for (int i = 0; i < num_conns; i++) {
        if (!conns[i].active) continue;
        if (conns[i].node_a == node_idx || conns[i].node_b == node_idx) {
            conns[i].active = false;
            stat_conns_closed++;
        }
    }
}

/* ================================================================
 *  Overrides
 * ================================================================ */
static struct link_override *find_override(const char *a, const char *b,
                                           bool create)
{
    for (int i = 0; i < num_overrides; i++) {
        if (!overrides[i].used) continue;
        if ((strcmp(overrides[i].a, a) == 0 && strcmp(overrides[i].b, b) == 0) ||
            (strcmp(overrides[i].a, b) == 0 && strcmp(overrides[i].b, a) == 0))
            return &overrides[i];
    }
    if (!create) return NULL;
    struct link_override *o = NULL;
    for (int i = 0; i < num_overrides; i++)
        if (!overrides[i].used) { o = &overrides[i]; break; }
    if (!o) {
        if (num_overrides >= MAX_OVERRIDES) return NULL;
        o = &overrides[num_overrides++];
    }
    memset(o, 0, sizeof(*o));
    snprintf(o->a, NODE_ID_LEN, "%s", a);
    snprintf(o->b, NODE_ID_LEN, "%s", b);
    o->used = true;
    return o;
}

/* ================================================================
 *  Propagation model
 *
 *  Returns true if the PDU is delivered to rx, and writes the RSSI the
 *  receiver should see into *out_rssi. Physical or bridge links, and the
 *  default "no positions set" case, deliver unconditionally.
 * ================================================================ */
static bool propagate(const struct node *tx, const struct node *rx,
                      int8_t frame_txpower, int8_t frame_rssi,
                      int *out_rssi)
{
    *out_rssi = frame_rssi ? frame_rssi : VBT_DEFAULT_RSSI;

    if (!tx || !rx) return true;
    if (tx->physical || rx->physical) { *out_rssi = frame_rssi; return true; }

    /* Per-link overrides win over the geometric model. */
    {
        struct link_override *o =
            find_override(tx->node_id, rx->node_id, false);
        if (o) {
            if (o->loss_set && o->loss_prob > 0.0) {
                double r = (double)rand() / (double)RAND_MAX;
                if (r < o->loss_prob) return false;
            }
            if (o->rssi_set) {
                *out_rssi = o->rssi_dbm;
                if (o->rssi_dbm < (int)rx->rx_sens_dbm) return false;
                return true;
            }
        }
    }

    if (!tx->pos_set || !rx->pos_set) return true;   /* default: audible */

    double dx = tx->pos_x - rx->pos_x;
    double dy = tx->pos_y - rx->pos_y;
    double dz = tx->pos_z - rx->pos_z;
    double d = sqrt(dx * dx + dy * dy + dz * dz);
    if (d < PL_REF_DIST_M) d = PL_REF_DIST_M;

    double txp = tx->tx_power_set ? tx->tx_power_dbm : (double)frame_txpower;
    double pl = PL_REF_DBM + 10.0 * g_path_exponent * log10(d / PL_REF_DIST_M);
    double rssi = txp - pl;

    *out_rssi = (int)lround(rssi);
    if (*out_rssi < -128) *out_rssi = -128;
    if (*out_rssi > 20) *out_rssi = 20;
    if (rssi < rx->rx_sens_dbm) return false;
    return true;
}

/* ================================================================
 *  Peer I/O
 * ================================================================ */
static void peer_close(int idx)
{
    struct peer *p = &peers[idx];
    if (p->fd < 0) return;

    if (p->node_idx >= 0) {
        struct node *nd = &nodes[p->node_idx];
        if (nd->peer_idx == idx) nd->peer_idx = -1;
        conns_drop_node(p->node_idx);
        if (nd->auto_named && nd->num_addrs == 0) nd->active = false;
        fprintf(stderr, "hub: peer %d disconnected: %s (fd=%d)\n",
                idx, p->label, p->fd);
    }

    /* If this peer was an upstream bridge, mark it for reconnection. */
    for (int i = 0; i < num_upstreams; i++)
        if (upstream_state[i].peer_idx == idx)
            upstream_state[i].peer_idx = -1;

    close(p->fd);
    free(p->outbuf);
    memset(p, 0, sizeof(*p));
    p->fd = -1;
    p->node_idx = -1;
}

/* Queue bytes to a peer, buffering on EAGAIN. Drops (and closes) only on
 * hard error or when the backlog overflows. */
static bool peer_send(int idx, const uint8_t *data, uint32_t len)
{
    struct peer *p = &peers[idx];
    if (p->fd < 0) return false;

    /* Fast path: nothing buffered, try a direct write. */
    uint32_t off = 0;
    if (p->outused == 0) {
        while (off < len) {
            ssize_t n = write(p->fd, data + off, len - off);
            if (n > 0) { off += (uint32_t)n; continue; }
            if (n < 0 && (errno == EINTR)) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            peer_close(idx);
            return false;
        }
        if (off == len) return true;
    }

    /* Buffer the remainder. */
    uint32_t rem = len - off;
    if (!p->outbuf) {
        p->outbuf = malloc(SEND_BUF_SIZE);
        if (!p->outbuf) { peer_close(idx); return false; }
        p->outused = 0;
    }
    if (p->outused + rem > SEND_BUF_SIZE) {
        p->tx_dropped++;
        stat_dropped_backpressure++;
        return false;   /* slow consumer; drop this PDU, keep the peer */
    }
    memcpy(p->outbuf + p->outused, data + off, rem);
    p->outused += rem;
    p->want_write = true;
    return true;
}

static void peer_flush(int idx)
{
    struct peer *p = &peers[idx];
    if (p->fd < 0 || p->outused == 0) { p->want_write = false; return; }
    uint32_t off = 0;
    while (off < p->outused) {
        ssize_t n = write(p->fd, p->outbuf + off, p->outused - off);
        if (n > 0) { off += (uint32_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        peer_close(idx);
        return;
    }
    if (off > 0) {
        memmove(p->outbuf, p->outbuf + off, p->outused - off);
        p->outused -= off;
    }
    p->want_write = (p->outused > 0);
}

/* ================================================================
 *  Node <-> peer binding (from hello)
 * ================================================================ */
static void bind_peer_node(int pidx, const char *nid, bool physical)
{
    struct peer *p = &peers[pidx];
    struct node *nd = find_node(nid, true);
    if (!nd) { fprintf(stderr, "hub: node table full\n"); return; }

    if (nd->peer_idx >= 0 && nd->peer_idx != pidx) {
        fprintf(stderr, "hub: node %s: replacing peer %d with %d\n",
                nid, nd->peer_idx, pidx);
        peers[nd->peer_idx].node_idx = -1;
    }
    nd->peer_idx = pidx;
    nd->physical = physical;
    p->node_idx = node_index(nd);
    p->is_physical = physical;
    snprintf(p->label, sizeof(p->label), "%s", nid);
    fprintf(stderr, "hub: peer %d -> node '%s'%s\n", pidx, nid,
            physical ? " [physical]" : "");
}

/* ================================================================
 *  Frame fan-out
 * ================================================================ */
static uint8_t g_wire[4 + VBT_MAX_MSG_SIZE];

static void deliver_to_peer(int dst, const struct vbt_frame_hdr *hdr,
                            const uint8_t *pdu, uint32_t pdu_len, int rssi)
{
    struct vbt_frame_hdr h = *hdr;
    h.rssi = (int8_t)rssi;
    uint32_t msg = (uint32_t)VBT_HDR_SIZE + pdu_len;
    uint32_t net = htonl(msg);
    memcpy(g_wire, &net, 4);
    memcpy(g_wire + 4, &h, VBT_HDR_SIZE);
    memcpy(g_wire + 4 + VBT_HDR_SIZE, pdu, pdu_len);
    if (peer_send(dst, g_wire, 4 + msg)) {
        if (peers[dst].node_idx >= 0) nodes[peers[dst].node_idx].rx_pdus++;
    }
}

static void forward_frame(int src_peer, struct vbt_frame_hdr *hdr,
                          const uint8_t *pdu, uint32_t pdu_len)
{
    struct peer *sp = &peers[src_peer];
    int src_node = sp->node_idx;
    struct node *tx = (src_node >= 0) ? &nodes[src_node] : NULL;

    /* TTL for inter-hub bridge loops: byte 23 of flags. */
    uint8_t *flagb = (uint8_t *)&hdr->flags;
    if (sp->is_bridge) {
        if (flagb[3] == 0) return;   /* expired */
        flagb[3]--;
    } else {
        flagb[3] = 8;                /* fresh local frame */
    }

    if (tx) {
        tx->tx_pdus++;
        tx->last_channel = hdr->channel;
        tx->last_phy = hdr->phy;
        node_add_addr(tx, hdr->tx_addr);
    }

    if (hdr->ll_type == VBT_LL_ADV) {
        /* Learn addresses and snoop CONNECT_IND. */
        if (pdu_len >= 2 && (pdu[0] & VBT_ADV_PDU_TYPE_MASK) == VBT_CONNECT_IND)
            observe_connect_ind(src_node, pdu, pdu_len);

        /* Broadcast to every other node (+ bridges). */
        for (int i = 0; i < num_peers; i++) {
            if (i == src_peer || peers[i].fd < 0) continue;
            if (peers[i].node_idx < 0 && !peers[i].is_bridge) continue;

            if (peers[i].is_bridge) {
                deliver_to_peer(i, hdr, pdu, pdu_len, hdr->rssi);
                continue;
            }
            struct node *rx = &nodes[peers[i].node_idx];
            int rssi;
            if (!propagate(tx, rx, hdr->tx_power_dbm, hdr->rssi, &rssi)) {
                rx->rx_dropped++;
                stat_dropped_model++;
                continue;
            }
            deliver_to_peer(i, hdr, pdu, pdu_len, rssi);
        }
        stat_adv_forwarded++;
        return;
    }

    if (hdr->ll_type == VBT_LL_DATA) {
        struct conn *c = find_conn(hdr->access_addr);
        if (!c) {
            /* Unknown Access Address (we missed the CONNECT_IND, e.g. it
             * happened on another hub). Fall back to broadcasting to all
             * bridges so the owning hub can route it; drop locally. */
            for (int i = 0; i < num_peers; i++) {
                if (i == src_peer || peers[i].fd < 0) continue;
                if (peers[i].is_bridge)
                    deliver_to_peer(i, hdr, pdu, pdu_len, hdr->rssi);
            }
            return;
        }
        /* Route to the *other* endpoint. */
        int other = -1;
        if (src_node >= 0 && c->node_a == src_node) other = c->node_b;
        else if (src_node >= 0 && c->node_b == src_node) other = c->node_a;
        else other = (c->node_a == src_node) ? c->node_b : c->node_a;

        c->pdus++;
        if (other >= 0 && nodes[other].active && nodes[other].peer_idx >= 0) {
            int dst = nodes[other].peer_idx;
            int rssi;
            if (propagate(tx, &nodes[other], hdr->tx_power_dbm, hdr->rssi,
                          &rssi))
                deliver_to_peer(dst, hdr, pdu, pdu_len, rssi);
            else { nodes[other].rx_dropped++; stat_dropped_model++; }
        }
        /* Also mirror to bridges so a remote endpoint on another hub hears
         * it (the remote hub routes by AA on its side). */
        for (int i = 0; i < num_peers; i++) {
            if (i == src_peer || peers[i].fd < 0) continue;
            if (peers[i].is_bridge)
                deliver_to_peer(i, hdr, pdu, pdu_len, hdr->rssi);
        }
        stat_data_forwarded++;
        return;
    }
    /* Unknown ll_type (e.g. reserved BR/EDR): ignore for now. */
}

/* ================================================================
 *  Hello handling
 * ================================================================ */
/* Returns bytes consumed (a hello is absorbed, not forwarded), or 0 if the
 * payload is not a hello. */
static uint32_t maybe_hello(int pidx, const uint8_t *payload, uint32_t len)
{
    if (len < 5) return 0;
    uint32_t magic;
    memcpy(&magic, payload, 4);
    if (magic != VBT_HELLO_MAGIC) return 0;

    char nid[NODE_ID_LEN];
    uint32_t i = 4, j = 0;
    while (i < len && payload[i] != '\0' && j < NODE_ID_LEN - 1)
        nid[j++] = (char)payload[i++];
    nid[j] = '\0';
    bool physical = false;
    if (i < len && payload[i] == '\0') i++;
    if (i < len) physical = (payload[i] & VBT_HELLO_FLAG_PHYSICAL) != 0;

    if (nid[0] == '\0')
        snprintf(nid, sizeof(nid), "node%d", next_auto_id++);
    bind_peer_node(pidx, nid, physical);
    fprintf(stderr, "hub: peer %d hello node_id='%s'\n", pidx, nid);
    return len;   /* consumed whole payload */
}

/* ================================================================
 *  Peer receive
 * ================================================================ */
static void peer_readable(int idx)
{
    struct peer *p = &peers[idx];
    ssize_t n = read(p->fd, p->rxbuf + p->rxused,
                     sizeof(p->rxbuf) - p->rxused);
    if (n == 0) { peer_close(idx); return; }
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return;
        peer_close(idx);
        return;
    }
    p->rxused += (uint32_t)n;

    while (p->rxused >= 4) {
        uint32_t plen;
        memcpy(&plen, p->rxbuf, 4);
        plen = ntohl(plen);
        if (plen > VBT_MAX_MSG_SIZE) {
            fprintf(stderr, "hub: peer %d oversized msg (%u), dropping conn\n",
                    idx, plen);
            peer_close(idx);
            return;
        }
        if (p->rxused < 4 + plen) break;   /* need more */

        uint8_t *payload = p->rxbuf + 4;
        p->frames_seen++;

        uint32_t consumed = maybe_hello(idx, payload, plen);
        if (consumed == 0) {
            if (plen >= VBT_HDR_SIZE) {
                struct vbt_frame_hdr hdr;
                memcpy(&hdr, payload, VBT_HDR_SIZE);
                if (hdr.magic == VBT_MAGIC) {
                    uint32_t pdu_len = plen - (uint32_t)VBT_HDR_SIZE;
                    if (pdu_len == hdr.pdu_len && pdu_len <= VBT_MAX_PDU_SIZE)
                        forward_frame(idx, &hdr, payload + VBT_HDR_SIZE,
                                      pdu_len);
                }
            }
        }

        uint32_t total = 4 + plen;
        p->rxused -= total;
        if (p->rxused > 0) memmove(p->rxbuf, p->rxbuf + total, p->rxused);
    }
}

/* ================================================================
 *  Accept new peers
 * ================================================================ */
static int peer_alloc(int fd, bool is_bridge, const char *label)
{
    int idx = -1;
    for (int i = 0; i < num_peers; i++)
        if (peers[i].fd < 0) { idx = i; break; }
    if (idx < 0) {
        if (num_peers >= MAX_PEERS) {
            fprintf(stderr, "hub: max peers reached\n");
            close(fd);
            return -1;
        }
        idx = num_peers++;
    }
    struct peer *p = &peers[idx];
    memset(p, 0, sizeof(*p));
    p->fd = fd;
    p->node_idx = -1;
    p->is_bridge = is_bridge;
    snprintf(p->label, sizeof(p->label), "%s", label ? label : "?");
    set_nonblock(fd);
    fprintf(stderr, "hub: peer %d connected: %s (fd=%d, %s)\n",
            idx, p->label, fd, is_bridge ? "bridge" : "node");
    return idx;
}

static void accept_unix(void)
{
    int fd = accept(unix_listen_fd, NULL, NULL);
    if (fd < 0) return;
    peer_alloc(fd, false, "unix");
}

static void accept_tcp(void)
{
    struct sockaddr_in sin;
    socklen_t sl = sizeof(sin);
    int fd = accept(tcp_listen_fd, (struct sockaddr *)&sin, &sl);
    if (fd < 0) return;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    char label[64];
    snprintf(label, sizeof(label), "tcp:%s:%d",
             inet_ntoa(sin.sin_addr), ntohs(sin.sin_port));
    peer_alloc(fd, true, label);
}

/* ================================================================
 *  Control socket
 * ================================================================ */
static void ctl_out(int cidx, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void ctl_out(int cidx, const char *fmt, ...)
{
    struct ctl_client *c = &ctl_clients[cidx];
    if (!c->active) return;
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    uint32_t len = (uint32_t)n;
    if (c->outbuf_used + len > CTL_OUTBUF_SIZE)
        len = CTL_OUTBUF_SIZE - c->outbuf_used;
    memcpy(c->outbuf + c->outbuf_used, tmp, len);
    c->outbuf_used += len;
}

static void ctl_flush(int cidx)
{
    struct ctl_client *c = &ctl_clients[cidx];
    if (!c->active || c->outbuf_used == 0) return;
    ssize_t n = write(c->fd, c->outbuf, c->outbuf_used);
    if (n > 0) {
        if ((uint32_t)n < c->outbuf_used)
            memmove(c->outbuf, c->outbuf + n, c->outbuf_used - n);
        c->outbuf_used -= (uint32_t)n;
    }
}

static const char *mode_label(const struct node *nd)
{
    /* Best-effort role inference from live connections. */
    for (int i = 0; i < num_conns; i++) {
        if (!conns[i].active) continue;
        if (conns[i].node_a == node_index(nd)) return "central";
        if (conns[i].node_b == node_index(nd)) return "peripheral";
    }
    return nd->tx_pdus ? "idle" : "?";
}

static void save_config(int cidx, const char *path);

static void ctl_dispatch(int cidx, const char *cmd)
{
    char ident[80], ib[80];
    double x, y, z;

    if (strncasecmp(cmd, "LIST_PEERS", 10) == 0) {
        int online = 0;
        for (int i = 0; i < num_nodes; i++) if (nodes[i].active) online++;
        ctl_out(cidx, "OK %d nodes\n", online);
        for (int i = 0; i < num_nodes; i++) {
            struct node *nd = &nodes[i];
            if (!nd->active) continue;
            char addrs[256]; addrs[0] = '\0';
            for (int j = 0; j < nd->num_addrs; j++) {
                char a[24]; fmt_addr(a, sizeof(a), nd->addrs[j]);
                strncat(addrs, a, sizeof(addrs) - strlen(addrs) - 1);
                if (j + 1 < nd->num_addrs)
                    strncat(addrs, ",", sizeof(addrs) - strlen(addrs) - 1);
            }
            ctl_out(cidx,
                "  %-16s %-8s role=%-10s chan=%d freq=%d addrs=[%s] "
                "pos=(%.1f,%.1f,%.1f) txpow=%.0f tx=%llu rx=%llu drop=%llu%s\n",
                nd->node_id,
                nd->peer_idx >= 0 ? "online" : "offline",
                mode_label(nd), nd->last_channel,
                vbt_channel_to_freq(nd->last_channel),
                addrs[0] ? addrs : "-",
                nd->pos_x, nd->pos_y, nd->pos_z,
                nd->tx_power_dbm,
                (unsigned long long)nd->tx_pdus,
                (unsigned long long)nd->rx_pdus,
                (unsigned long long)nd->rx_dropped,
                nd->physical ? " [physical]" : "");
        }
        return;
    }

    if (strncasecmp(cmd, "LIST_CONNS", 10) == 0) {
        int active = 0;
        for (int i = 0; i < num_conns; i++) if (conns[i].active) active++;
        ctl_out(cidx, "OK %d connections\n", active);
        for (int i = 0; i < num_conns; i++) {
            struct conn *c = &conns[i];
            if (!c->active) continue;
            char ia[24], aa[24];
            fmt_addr(ia, sizeof(ia), c->inita);
            fmt_addr(aa, sizeof(aa), c->adva);
            ctl_out(cidx,
                "  aa=0x%08x central=%s peripheral=%s init=%s adv=%s pdus=%llu\n",
                c->access_addr,
                c->node_a >= 0 ? nodes[c->node_a].node_id : "?",
                c->node_b >= 0 ? nodes[c->node_b].node_id : "?",
                ia, aa, (unsigned long long)c->pdus);
        }
        return;
    }

    if (strncasecmp(cmd, "SET_POS ", 8) == 0) {
        int n = sscanf(cmd + 8, "%79s %lf %lf %lf", ident, &x, &y, &z);
        if (n < 3) { ctl_out(cidx, "ERR usage: SET_POS <node> <x> <y> [z]\n"); return; }
        struct node *nd = resolve_node(ident);
        if (!nd) nd = find_node(ident, true);
        if (!nd) { ctl_out(cidx, "ERR node table full\n"); return; }
        nd->pos_x = x; nd->pos_y = y; nd->pos_z = (n >= 4) ? z : 0.0;
        nd->pos_set = true;
        ctl_out(cidx, "OK pos %s (%.1f,%.1f,%.1f)\n",
                nd->node_id, nd->pos_x, nd->pos_y, nd->pos_z);
        return;
    }

    if (strncasecmp(cmd, "SET_TXPOWER ", 12) == 0) {
        if (sscanf(cmd + 12, "%79s %lf", ident, &x) != 2) {
            ctl_out(cidx, "ERR usage: SET_TXPOWER <node> <dBm>\n"); return;
        }
        struct node *nd = resolve_node(ident);
        if (!nd) nd = find_node(ident, true);
        if (!nd) { ctl_out(cidx, "ERR node table full\n"); return; }
        nd->tx_power_dbm = x; nd->tx_power_set = true;
        ctl_out(cidx, "OK txpower %s %.1f dBm\n", nd->node_id, x);
        return;
    }

    if (strncasecmp(cmd, "SET_SENS ", 9) == 0) {
        if (sscanf(cmd + 9, "%79s %lf", ident, &x) != 2) {
            ctl_out(cidx, "ERR usage: SET_SENS <node> <dBm>\n"); return;
        }
        struct node *nd = resolve_node(ident);
        if (!nd) nd = find_node(ident, true);
        if (!nd) { ctl_out(cidx, "ERR node table full\n"); return; }
        nd->rx_sens_dbm = x;
        ctl_out(cidx, "OK sensitivity %s %.1f dBm\n", nd->node_id, x);
        return;
    }

    if (strncasecmp(cmd, "SET_RSSI ", 9) == 0) {
        int rssi;
        if (sscanf(cmd + 9, "%79s %79s %d", ident, ib, &rssi) != 3) {
            ctl_out(cidx, "ERR usage: SET_RSSI <a> <b> <dBm>\n"); return;
        }
        struct node *na = resolve_node(ident), *nb = resolve_node(ib);
        if (!na || !nb) { ctl_out(cidx, "ERR unknown node(s)\n"); return; }
        struct link_override *o =
            find_override(na->node_id, nb->node_id, true);
        if (!o) { ctl_out(cidx, "ERR override table full\n"); return; }
        o->rssi_set = true; o->rssi_dbm = rssi;
        ctl_out(cidx, "OK rssi %s<->%s %d dBm\n", na->node_id, nb->node_id, rssi);
        return;
    }

    if (strncasecmp(cmd, "SET_LOSS ", 9) == 0) {
        double p;
        if (sscanf(cmd + 9, "%79s %79s %lf", ident, ib, &p) != 3) {
            ctl_out(cidx, "ERR usage: SET_LOSS <a> <b> <prob 0..1>\n"); return;
        }
        struct node *na = resolve_node(ident), *nb = resolve_node(ib);
        if (!na || !nb) { ctl_out(cidx, "ERR unknown node(s)\n"); return; }
        struct link_override *o =
            find_override(na->node_id, nb->node_id, true);
        if (!o) { ctl_out(cidx, "ERR override table full\n"); return; }
        o->loss_set = true; o->loss_prob = p < 0 ? 0 : (p > 1 ? 1 : p);
        ctl_out(cidx, "OK loss %s<->%s %.2f\n", na->node_id, nb->node_id,
                o->loss_prob);
        return;
    }

    if (strncasecmp(cmd, "CLEAR_LINK ", 11) == 0) {
        if (sscanf(cmd + 11, "%79s %79s", ident, ib) != 2) {
            ctl_out(cidx, "ERR usage: CLEAR_LINK <a> <b>\n"); return;
        }
        struct node *na = resolve_node(ident), *nb = resolve_node(ib);
        if (na && nb) {
            struct link_override *o =
                find_override(na->node_id, nb->node_id, false);
            if (o) o->used = false;
        }
        ctl_out(cidx, "OK cleared\n");
        return;
    }

    if (strncasecmp(cmd, "SET_PATHLOSS ", 13) == 0) {
        if (sscanf(cmd + 13, "%lf", &x) != 1) {
            ctl_out(cidx, "ERR usage: SET_PATHLOSS <exponent>\n"); return;
        }
        g_path_exponent = x;
        ctl_out(cidx, "OK path-loss exponent %.2f\n", g_path_exponent);
        return;
    }

    if (strncasecmp(cmd, "STATS", 5) == 0) {
        int online = 0, aconns = 0;
        for (int i = 0; i < num_nodes; i++) if (nodes[i].active) online++;
        for (int i = 0; i < num_conns; i++) if (conns[i].active) aconns++;
        ctl_out(cidx,
            "OK uptime=%llds nodes=%d conns=%d adv_fwd=%llu data_fwd=%llu "
            "drop_model=%llu drop_bp=%llu conns_opened=%llu conns_closed=%llu\n",
            (long long)(time(NULL) - stat_start_time), online, aconns,
            (unsigned long long)stat_adv_forwarded,
            (unsigned long long)stat_data_forwarded,
            (unsigned long long)stat_dropped_model,
            (unsigned long long)stat_dropped_backpressure,
            (unsigned long long)stat_conns_opened,
            (unsigned long long)stat_conns_closed);
        return;
    }

    if (strncasecmp(cmd, "SAVE_CONFIG ", 12) == 0) {
        char path[256];
        if (sscanf(cmd + 12, "%255s", path) != 1) {
            ctl_out(cidx, "ERR usage: SAVE_CONFIG <path>\n"); return;
        }
        save_config(cidx, path);
        return;
    }

    if (strncasecmp(cmd, "HELP", 4) == 0) {
        ctl_out(cidx,
            "OK commands:\n"
            "  LIST_PEERS                         Nodes: role, channel, addrs, pos\n"
            "  LIST_CONNS                         Active connections (by AA)\n"
            "  SET_POS <node> <x> <y> [z]         Position a node (metres)\n"
            "  SET_TXPOWER <node> <dBm>           Node TX power\n"
            "  SET_SENS <node> <dBm>              Node RX sensitivity floor\n"
            "  SET_RSSI <a> <b> <dBm>             Pin a per-link RSSI\n"
            "  SET_LOSS <a> <b> <prob>            Pin a per-link loss probability\n"
            "  CLEAR_LINK <a> <b>                 Clear per-link overrides\n"
            "  SET_PATHLOSS <exponent>            Global path-loss exponent\n"
            "  STATS                              Global counters\n"
            "  SAVE_CONFIG <path>                 Snapshot positions/overrides\n"
            "  QUIT                               Close this connection\n");
        return;
    }

    if (strncasecmp(cmd, "QUIT", 4) == 0) {
        ctl_out(cidx, "OK bye\n");
        ctl_clients[cidx].active = 2;   /* mark for close after flush */
        return;
    }

    ctl_out(cidx, "ERR unknown command: %.40s (try HELP)\n", cmd);
}

static void save_config(int cidx, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) { ctl_out(cidx, "ERR cannot open %s: %s\n", path, strerror(errno)); return; }
    fprintf(f, "# vbt-medium config snapshot\n");
    fprintf(f, "SET_PATHLOSS %.4f\n", g_path_exponent);
    for (int i = 0; i < num_nodes; i++) {
        struct node *nd = &nodes[i];
        if (!nd->active) continue;
        if (nd->pos_set)
            fprintf(f, "SET_POS %s %.4f %.4f %.4f\n",
                    nd->node_id, nd->pos_x, nd->pos_y, nd->pos_z);
        if (nd->tx_power_set)
            fprintf(f, "SET_TXPOWER %s %.4f\n", nd->node_id, nd->tx_power_dbm);
    }
    fclose(f);
    ctl_out(cidx, "OK saved %s\n", path);
}

static void ctl_readable(int cidx)
{
    struct ctl_client *c = &ctl_clients[cidx];
    ssize_t n = read(c->fd, c->buf + c->buf_used, sizeof(c->buf) - 1 - c->buf_used);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        close(c->fd); c->active = 0; return;
    }
    c->buf_used += (uint32_t)n;
    c->buf[c->buf_used] = '\0';

    char *line;
    while ((line = memchr(c->buf, '\n', c->buf_used)) != NULL) {
        *line = '\0';
        char *cr = strchr(c->buf, '\r'); if (cr) *cr = '\0';
        if (c->buf[0]) ctl_dispatch(cidx, c->buf);
        uint32_t consumed = (uint32_t)(line - c->buf) + 1;
        c->buf_used -= consumed;
        memmove(c->buf, c->buf + consumed, c->buf_used);
        c->buf[c->buf_used] = '\0';
        if (c->active == 2) break;
    }
}

static void accept_ctl(void)
{
    int fd = accept(ctl_listen_fd, NULL, NULL);
    if (fd < 0) return;
    int idx = -1;
    for (int i = 0; i < MAX_CTL_CLIENTS; i++)
        if (!ctl_clients[i].active) { idx = i; break; }
    if (idx < 0) { close(fd); return; }
    memset(&ctl_clients[idx], 0, sizeof(ctl_clients[idx]));
    ctl_clients[idx].fd = fd;
    ctl_clients[idx].active = 1;
    set_nonblock(fd);
}

/* ================================================================
 *  Upstream bridge connections (outbound TCP trunks)
 * ================================================================ */
static void upstream_connect(int i)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(upstreams[i].host, upstreams[i].port, &hints, &res) != 0)
        return;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd); freeaddrinfo(res); return;
    }
    freeaddrinfo(res);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    char label[300];
    snprintf(label, sizeof(label), "upstream:%s:%s",
             upstreams[i].host, upstreams[i].port);
    int pidx = peer_alloc(fd, true, label);
    if (pidx >= 0) upstream_state[i].peer_idx = pidx;
}

static void upstream_tick(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < num_upstreams; i++) {
        if (upstream_state[i].peer_idx >= 0) continue;
        if (now - upstream_state[i].last_attempt < UPSTREAM_RETRY_SEC) continue;
        upstream_state[i].last_attempt = now;
        upstream_connect(i);
    }
}

/* ================================================================
 *  Config file (startup -C)
 * ================================================================ */
static void load_config_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "hub: cannot open config %s\n", path); return; }
    char line[512];
    /* Reuse a synthetic ctl_client slot (STDERR-backed) for responses. */
    int slot = 0;
    ctl_clients[slot].active = 1;
    ctl_clients[slot].fd = STDERR_FILENO;
    ctl_clients[slot].buf_used = 0;
    ctl_clients[slot].outbuf_used = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;
        ctl_dispatch(slot, line);
        ctl_clients[slot].outbuf_used = 0;   /* discard responses */
    }
    ctl_clients[slot].active = 0;
    fclose(f);
    fprintf(stderr, "hub: loaded config %s\n", path);
}

/* ================================================================
 *  Listener setup
 * ================================================================ */
static int make_unix_listener(const char *path, mode_t mode)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, path, sizeof(sun.sun_path) - 1);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
        fprintf(stderr, "hub: bind %s: %s\n", path, strerror(errno));
        close(fd); return -1;
    }
    if (listen(fd, 64) < 0) { close(fd); return -1; }
    chmod(path, mode);
    set_nonblock(fd);
    return fd;
}

static int make_tcp_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        fprintf(stderr, "hub: TCP bind :%d: %s\n", port, strerror(errno));
        close(fd); return -1;
    }
    if (listen(fd, 64) < 0) { close(fd); return -1; }
    set_nonblock(fd);
    return fd;
}

/* ================================================================
 *  Main loop
 * ================================================================ */
static struct pollfd pfds[MAX_PEERS + MAX_CTL_CLIENTS + 4];

static void usage(const char *prog)
{
    fprintf(stderr,
        "vbt-medium — Virtual Bluetooth (BLE) medium hub\n\n"
        "Usage: %s <unix-socket-path> [options]\n\n"
        "  -c <path>        Control socket path (runtime commands)\n"
        "  -t <port>        TCP listen port for inter-hub bridges\n"
        "  -u <host:port>   Connect to an upstream hub (repeatable, max %d)\n"
        "  -C <path>        Initial config file (commands run at startup)\n"
        "  -h               Show this help\n\n"
        "The data socket is created mode 0666; the control socket 0600.\n",
        prog, MAX_UPSTREAMS);
}

int main(int argc, char **argv)
{
    int tcp_port = 0;
    const char *config_path = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "c:t:u:C:h")) != -1) {
        switch (opt) {
        case 'c': ctl_socket_path = optarg; break;
        case 't': tcp_port = atoi(optarg); break;
        case 'u': {
            if (num_upstreams >= MAX_UPSTREAMS) {
                fprintf(stderr, "hub: too many upstreams\n"); break;
            }
            char *colon = strrchr(optarg, ':');
            if (!colon) { fprintf(stderr, "hub: -u needs host:port\n"); break; }
            size_t hl = (size_t)(colon - optarg);
            if (hl >= sizeof(upstreams[0].host)) hl = sizeof(upstreams[0].host) - 1;
            memcpy(upstreams[num_upstreams].host, optarg, hl);
            upstreams[num_upstreams].host[hl] = '\0';
            snprintf(upstreams[num_upstreams].port,
                     sizeof(upstreams[0].port), "%s", colon + 1);
            upstream_state[num_upstreams].peer_idx = -1;
            num_upstreams++;
            break;
        }
        case 'C': config_path = optarg; break;
        case 'h': default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }
    if (optind >= argc) { usage(argv[0]); return 2; }
    socket_path = argv[optind];

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    for (int i = 0; i < MAX_PEERS; i++) peers[i].fd = -1;
    stat_start_time = time(NULL);
    srand((unsigned)(now_ms() & 0xffffffff));

    unix_listen_fd = make_unix_listener(socket_path, 0666);
    if (unix_listen_fd < 0) return 1;
    fprintf(stderr, "hub: listening on %s\n", socket_path);

    if (ctl_socket_path) {
        ctl_listen_fd = make_unix_listener(ctl_socket_path, 0600);
        if (ctl_listen_fd < 0) return 1;
        fprintf(stderr, "hub: control socket %s\n", ctl_socket_path);
    }
    if (tcp_port > 0) {
        tcp_listen_fd = make_tcp_listener(tcp_port);
        if (tcp_listen_fd < 0) return 1;
        fprintf(stderr, "hub: TCP bridge port %d\n", tcp_port);
    }
    if (config_path) load_config_file(config_path);

    fprintf(stderr, "hub: ready (BLE virtual medium)\n");

    while (g_running) {
        int nf = 0;
        int idx_unix = -1, idx_tcp = -1, idx_ctl = -1;

        pfds[nf].fd = unix_listen_fd; pfds[nf].events = POLLIN;
        idx_unix = nf++;
        if (tcp_listen_fd >= 0) {
            pfds[nf].fd = tcp_listen_fd; pfds[nf].events = POLLIN;
            idx_tcp = nf++;
        }
        if (ctl_listen_fd >= 0) {
            pfds[nf].fd = ctl_listen_fd; pfds[nf].events = POLLIN;
            idx_ctl = nf++;
        }

        int peer_base = nf;
        for (int i = 0; i < num_peers; i++) {
            if (peers[i].fd < 0) continue;
            pfds[nf].fd = peers[i].fd;
            pfds[nf].events = POLLIN | (peers[i].want_write ? POLLOUT : 0);
            pfds[nf].revents = 0;
            nf++;
        }
        int ctl_base = nf;
        for (int i = 0; i < MAX_CTL_CLIENTS; i++) {
            if (!ctl_clients[i].active) continue;
            pfds[nf].fd = ctl_clients[i].fd;
            pfds[nf].events = POLLIN | (ctl_clients[i].outbuf_used ? POLLOUT : 0);
            pfds[nf].revents = 0;
            nf++;
        }

        int r = poll(pfds, nf, 1000);
        if (r < 0) { if (errno == EINTR) continue; break; }

        if (num_upstreams) upstream_tick();

        if (r == 0) continue;

        if (pfds[idx_unix].revents & POLLIN) accept_unix();
        if (idx_tcp >= 0 && (pfds[idx_tcp].revents & POLLIN)) accept_tcp();
        if (idx_ctl >= 0 && (pfds[idx_ctl].revents & POLLIN)) accept_ctl();

        /* Map pollfd rows back to peers by fd (peer set is stable within
         * this iteration; peer_close only nulls fd which we re-check). */
        int row = peer_base;
        for (int i = 0; i < num_peers && row < ctl_base; i++) {
            if (peers[i].fd < 0) continue;
            short re = pfds[row].revents;
            int this_fd = peers[i].fd;
            row++;
            if (pfds[row - 1].fd != this_fd) continue;
            if (re & (POLLERR | POLLHUP | POLLNVAL)) { peer_close(i); continue; }
            if (re & POLLOUT) peer_flush(i);
            if (peers[i].fd >= 0 && (re & POLLIN)) peer_readable(i);
        }

        row = ctl_base;
        for (int i = 0; i < MAX_CTL_CLIENTS && row < nf; i++) {
            if (!ctl_clients[i].active) continue;
            short re = pfds[row].revents;
            row++;
            if (re & POLLIN) ctl_readable(i);
            if (ctl_clients[i].active) ctl_flush(i);
            if (ctl_clients[i].active == 2 && ctl_clients[i].outbuf_used == 0) {
                close(ctl_clients[i].fd);
                ctl_clients[i].active = 0;
            }
        }
    }

    fprintf(stderr, "hub: shutting down\n");
    for (int i = 0; i < num_peers; i++) if (peers[i].fd >= 0) peer_close(i);
    if (unix_listen_fd >= 0) close(unix_listen_fd);
    if (ctl_listen_fd >= 0) close(ctl_listen_fd);
    if (tcp_listen_fd >= 0) close(tcp_listen_fd);
    if (socket_path) unlink(socket_path);
    if (ctl_socket_path) unlink(ctl_socket_path);
    return 0;
}
