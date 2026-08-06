/*
 * vbt-vhost-user — virtio-bluetooth backend via vhost-user (no QEMU rebuild)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A vhost-user backend that presents a virtio-bluetooth controller to a
 * guest using *unmodified* QEMU. QEMU attaches it with the stock generic
 * vhost-user device:
 *
 *   -chardev socket,id=vbt,path=/tmp/vbt-vhost.sock \
 *   -device  vhost-user-device-pci,chardev=vbt,virtio-id=40,num_vqs=2
 *
 * The guest's in-tree virtio_bt driver binds and BlueZ sees hci0. This
 * process maps the guest virtqueues (via libvhost-user), pulls H4-framed
 * HCI packets off the TX ring into the shared BLE controller core
 * (vbt_ll), pushes events/ACL back on the RX ring, and bridges Link-Layer
 * PDUs to a vbt-medium hub — the same core and wire protocol the QEMU
 * device model and the vhci host bridge use. Only the transport differs.
 *
 * Build (needs QEMU's libvhost-user; not a standard distro package):
 *   make vbt-vhost-user LIBVHOST_USER=/path/to/qemu/subprojects/libvhost-user
 * See README.md "vhost-user backend" for details.
 *
 * NOTE: libvhost-user is not available in this repo's CI, so this file is
 * written against the documented libvhost-user API and verified by
 * building it against a QEMU tree and attaching a guest. The controller
 * core it wraps (vbt_ll) is unit-tested in tests/test_ll.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <arpa/inet.h>

#include "libvhost-user.h"

#include "vbt_ll.h"
#include "vbt.h"

#ifndef VIRTIO_ID_BLUETOOTH
#define VIRTIO_ID_BLUETOOTH  40
#endif
#ifndef VIRTIO_F_VERSION_1
#define VIRTIO_F_VERSION_1   32
#endif

/* virtio-bt config space (v1 layout): primary controller, no vendor ext. */
struct virtio_bt_config {
    uint8_t  type;
    uint8_t  vendor;
    uint16_t msft_opcode;
};

/* Queue indices — must match the guest virtio_bt driver's vq order
 * (index 0 = "tx": guest -> controller; index 1 = "rx": controller ->
 * guest). If a future driver revision swaps these, flip TX_VQ/RX_VQ. */
#define TX_VQ 0
#define RX_VQ 1

#define MAX_WATCH        32
#define PENDING_MAX      256
#define MEDIUM_RECONNECT_MS 2000
#define TICK_MS          5

struct pending_pkt { uint8_t data[VBT_MAX_MSG_SIZE]; uint32_t len; };

struct watch_ent { int fd; vu_watch_cb cb; void *data; bool used; };

static struct {
    VuDev        dev;
    bool         dev_ready;
    int          listen_fd;
    int          conn_fd;

    struct vbt_ll *ll;
    uint8_t       bdaddr[6];
    char          node_id[64];
    char          medium_path[108];

    int           medium_fd;
    bool          medium_connected;
    uint8_t       medium_rxbuf[VBT_RXBUF_SIZE];
    uint32_t      medium_rxused;

    struct pending_pkt pending[PENDING_MAX];
    uint32_t      pend_head, pend_tail;

    struct watch_ent watches[MAX_WATCH];
    bool          verbose;
} g;

static volatile sig_atomic_t g_running = 1;
static void on_signal(int s) { (void)s; g_running = 0; }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- small iovec helpers (QEMU's qemu/iov.h isn't available here) ---- */
static size_t iov_to_buf(const struct iovec *iov, unsigned cnt,
                         void *buf, size_t max)
{
    size_t off = 0;
    for (unsigned i = 0; i < cnt && off < max; i++) {
        size_t c = iov[i].iov_len;
        if (c > max - off) c = max - off;
        memcpy((uint8_t *)buf + off, iov[i].iov_base, c);
        off += c;
    }
    return off;
}

static size_t iov_from_buf(const struct iovec *iov, unsigned cnt,
                           const void *buf, size_t len)
{
    size_t off = 0;
    for (unsigned i = 0; i < cnt && off < len; i++) {
        size_t c = iov[i].iov_len;
        if (c > len - off) c = len - off;
        memcpy(iov[i].iov_base, (const uint8_t *)buf + off, c);
        off += c;
    }
    return off;
}

/* ================================================================
 *  RX toward guest: fill the RX virtqueue from the pending FIFO
 * ================================================================ */
static void flush_pending(void)
{
    if (!g.dev_ready) return;
    VuVirtq *vq = vu_get_queue(&g.dev, RX_VQ);
    if (!vq) return;

    while (g.pend_head != g.pend_tail) {
        VuVirtqElement *e = vu_queue_pop(&g.dev, vq, sizeof(VuVirtqElement));
        if (!e) break;                 /* guest gave us no buffer yet */
        struct pending_pkt *p = &g.pending[g.pend_head % PENDING_MAX];
        iov_from_buf(e->in_sg, e->in_num, p->data, p->len);
        vu_queue_push(&g.dev, vq, e, p->len);
        free(e);
        g.pend_head++;
    }
    vu_queue_notify(&g.dev, vq);
}

/* ================================================================
 *  vbt_ll callbacks
 * ================================================================ */
static void cb_hci_to_host(void *ctx, const uint8_t *pkt, size_t len)
{
    (void)ctx;
    if (len > VBT_MAX_MSG_SIZE) return;
    if (g.pend_tail - g.pend_head >= PENDING_MAX) {
        if (g.verbose) fprintf(stderr, "vbt-vhost: RX backlog full, drop\n");
        return;
    }
    struct pending_pkt *p = &g.pending[g.pend_tail % PENDING_MAX];
    memcpy(p->data, pkt, len);
    p->len = (uint32_t)len;
    g.pend_tail++;
    flush_pending();
}

static void cb_pdu_to_medium(void *ctx, const struct vbt_frame_hdr *hdr,
                             const uint8_t *pdu, size_t pdu_len)
{
    (void)ctx;
    if (!g.medium_connected || g.medium_fd < 0) return;
    uint8_t wire[4 + VBT_MAX_MSG_SIZE];
    uint32_t msg = (uint32_t)VBT_HDR_SIZE + (uint32_t)pdu_len;
    uint32_t net = htonl(msg);
    memcpy(wire, &net, 4);
    memcpy(wire + 4, hdr, VBT_HDR_SIZE);
    memcpy(wire + 4 + VBT_HDR_SIZE, pdu, pdu_len);
    uint32_t total = 4 + msg, off = 0;
    while (off < total) {
        ssize_t n = write(g.medium_fd, wire + off, total - off);
        if (n > 0) { off += (uint32_t)n; continue; }
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        close(g.medium_fd);
        g.medium_fd = -1;
        g.medium_connected = false;
        return;
    }
}

/* ================================================================
 *  Virtqueue handlers
 * ================================================================ */
static void handle_tx(VuDev *dev, int qidx)
{
    VuVirtq *vq = vu_get_queue(dev, qidx);
    for (;;) {
        VuVirtqElement *e = vu_queue_pop(dev, vq, sizeof(VuVirtqElement));
        if (!e) break;
        uint8_t pkt[VBT_MAX_MSG_SIZE];
        size_t n = iov_to_buf(e->out_sg, e->out_num, pkt, sizeof(pkt));
        if (n > 0) vbt_ll_hci_from_host(g.ll, pkt, n);
        vu_queue_push(dev, vq, e, 0);
        free(e);
    }
    vu_queue_notify(dev, vq);
}

static void handle_rx(VuDev *dev, int qidx)
{
    (void)dev; (void)qidx;
    flush_pending();     /* guest supplied fresh RX buffers */
}

/* ================================================================
 *  VuDevIface callbacks
 * ================================================================ */
static uint64_t get_features(VuDev *dev)
{
    (void)dev;
    return 1ULL << VIRTIO_F_VERSION_1;
}

static void set_features(VuDev *dev, uint64_t features)
{
    (void)dev; (void)features;
}

static uint64_t get_protocol_features(VuDev *dev)
{
    (void)dev;
    /* CONFIG lets the guest read our virtio_bt_config (type/vendor). */
    return 1ULL << VHOST_USER_PROTOCOL_F_CONFIG;
}

static void set_protocol_features(VuDev *dev, uint64_t features)
{
    (void)dev; (void)features;
}

static int get_config(VuDev *dev, uint8_t *config, uint32_t len)
{
    (void)dev;
    struct virtio_bt_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.type = 0;      /* VIRTIO_BT_CONFIG_TYPE_PRIMARY */
    cfg.vendor = 0;    /* VIRTIO_BT_CONFIG_VENDOR_NONE  */
    cfg.msft_opcode = 0;
    memcpy(config, &cfg, len < sizeof(cfg) ? len : sizeof(cfg));
    return 0;
}

static void queue_set_started(VuDev *dev, int qidx, bool started)
{
    VuVirtq *vq = vu_get_queue(dev, qidx);
    if (!vq) return;
    if (qidx == TX_VQ)
        vu_set_queue_handler(dev, vq, started ? handle_tx : NULL);
    else if (qidx == RX_VQ)
        vu_set_queue_handler(dev, vq, started ? handle_rx : NULL);
    if (started && qidx == RX_VQ) g.dev_ready = true;
}

static const VuDevIface vbt_iface = {
    .get_features          = get_features,
    .set_features          = set_features,
    .get_protocol_features = get_protocol_features,
    .set_protocol_features = set_protocol_features,
    .get_config            = get_config,
    .queue_set_started     = queue_set_started,
};

/* ================================================================
 *  libvhost-user watch integration
 * ================================================================ */
static void vu_panic(VuDev *dev, const char *msg)
{
    (void)dev;
    fprintf(stderr, "vbt-vhost: libvhost-user panic: %s\n", msg);
    g_running = 0;
}

static void set_watch(VuDev *dev, int fd, int cond, vu_watch_cb cb, void *data)
{
    (void)dev;
    for (int i = 0; i < MAX_WATCH; i++) {
        if (g.watches[i].used && g.watches[i].fd == fd) {
            g.watches[i].cb = cb; g.watches[i].data = data; return;
        }
    }
    for (int i = 0; i < MAX_WATCH; i++) {
        if (!g.watches[i].used) {
            g.watches[i].used = true; g.watches[i].fd = fd;
            g.watches[i].cb = cb; g.watches[i].data = data;
            return;
        }
    }
    (void)cond;
    fprintf(stderr, "vbt-vhost: watch table full\n");
}

static void remove_watch(VuDev *dev, int fd)
{
    (void)dev;
    for (int i = 0; i < MAX_WATCH; i++)
        if (g.watches[i].used && g.watches[i].fd == fd)
            g.watches[i].used = false;
}

/* ================================================================
 *  Medium socket bridge (same protocol as vbt-controller)
 * ================================================================ */
static bool connect_medium(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", g.medium_path);
    if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
        close(fd); return false;
    }
    uint8_t hb[4 + 4 + 64];
    size_t idlen = strlen(g.node_id) + 1;
    if (idlen > 64) idlen = 64;
    uint32_t payload = 4 + (uint32_t)idlen;
    uint32_t net = htonl(payload), magic = VBT_HELLO_MAGIC;
    memcpy(hb, &net, 4);
    memcpy(hb + 4, &magic, 4);
    memcpy(hb + 8, g.node_id, idlen);
    hb[8 + idlen - 1] = '\0';
    if (write(fd, hb, 4 + payload) < 0) { close(fd); return false; }
    g.medium_fd = fd;
    g.medium_connected = true;
    g.medium_rxused = 0;
    fprintf(stderr, "vbt-vhost: connected to medium %s (node_id=%s)\n",
            g.medium_path, g.node_id);
    return true;
}

static void medium_readable(void)
{
    uint32_t used = g.medium_rxused;
    if (used >= VBT_RXBUF_SIZE) used = 0;
    ssize_t n = read(g.medium_fd, g.medium_rxbuf + used, VBT_RXBUF_SIZE - used);
    if (n <= 0) {
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) return;
        close(g.medium_fd); g.medium_fd = -1; g.medium_connected = false;
        return;
    }
    g.medium_rxused = used + (uint32_t)n;
    while (g.medium_rxused >= 4) {
        uint32_t net, msg;
        memcpy(&net, g.medium_rxbuf, 4);
        msg = ntohl(net);
        if (msg > VBT_MAX_MSG_SIZE) { g.medium_rxused = 0; break; }
        if (g.medium_rxused < 4 + msg) break;
        if (msg >= VBT_HDR_SIZE) {
            struct vbt_frame_hdr hdr;
            memcpy(&hdr, g.medium_rxbuf + 4, VBT_HDR_SIZE);
            if (hdr.magic == VBT_MAGIC) {
                uint32_t pdu_len = msg - (uint32_t)VBT_HDR_SIZE;
                if (pdu_len == hdr.pdu_len)
                    vbt_ll_pdu_from_medium(g.ll, &hdr,
                                           g.medium_rxbuf + 4 + VBT_HDR_SIZE,
                                           pdu_len);
            }
        }
        uint32_t consumed = 4 + msg;
        g.medium_rxused -= consumed;
        if (g.medium_rxused)
            memmove(g.medium_rxbuf, g.medium_rxbuf + consumed, g.medium_rxused);
    }
}

/* ================================================================
 *  Serve one QEMU connection
 * ================================================================ */
static void serve_connection(int conn_fd)
{
    memset(g.watches, 0, sizeof(g.watches));
    g.dev_ready = false;
    g.pend_head = g.pend_tail = 0;

    if (!vu_init(&g.dev, 2, conn_fd, vu_panic, NULL,
                 set_watch, remove_watch, &vbt_iface)) {
        fprintf(stderr, "vbt-vhost: vu_init failed\n");
        return;
    }

    uint64_t last_tick = now_ms(), last_reconnect = 0;
    bool alive = true;

    while (g_running && alive) {
        struct pollfd pfds[MAX_WATCH + 3];
        int map[MAX_WATCH + 3];
        int nf = 0;

        pfds[nf].fd = conn_fd; pfds[nf].events = POLLIN; map[nf] = -1; nf++;
        int medium_row = -1;
        if (g.medium_fd >= 0) {
            pfds[nf].fd = g.medium_fd; pfds[nf].events = POLLIN;
            map[nf] = -2; medium_row = nf; nf++;
        }
        for (int i = 0; i < MAX_WATCH && nf < (int)(sizeof(pfds)/sizeof(pfds[0])); i++) {
            if (!g.watches[i].used) continue;
            pfds[nf].fd = g.watches[i].fd; pfds[nf].events = POLLIN;
            map[nf] = i; nf++;
        }

        int r = poll(pfds, nf, TICK_MS);
        uint64_t t = now_ms();

        if (r > 0) {
            for (int k = 0; k < nf; k++) {
                if (!(pfds[k].revents & (POLLIN | POLLERR | POLLHUP))) continue;
                if (map[k] == -1) {
                    if (!vu_dispatch(&g.dev)) { alive = false; break; }
                } else if (map[k] == -2) {
                    medium_readable();
                } else {
                    struct watch_ent *w = &g.watches[map[k]];
                    if (w->used && w->cb) w->cb(&g.dev, VU_WATCH_IN, w->data);
                }
            }
        }
        (void)medium_row;

        if (t - last_tick >= TICK_MS) {
            if (g.ll) vbt_ll_tick(g.ll, t);
            flush_pending();
            last_tick = t;
        }
        if (g.medium_fd < 0 && t - last_reconnect >= MEDIUM_RECONNECT_MS) {
            last_reconnect = t;
            connect_medium();
        }
    }

    vu_deinit(&g.dev);
    g.dev_ready = false;
}

static void derive_bdaddr(void)
{
    uint32_t h = 0x811c9dc5u;
    for (const char *p = g.node_id; *p; p++) h = (h ^ (uint8_t)*p) * 0x01000193u;
    g.bdaddr[0] = 0x02; g.bdaddr[1] = 0xbe;
    g.bdaddr[2] = (uint8_t)(h >> 24); g.bdaddr[3] = (uint8_t)(h >> 16);
    g.bdaddr[4] = (uint8_t)(h >> 8);  g.bdaddr[5] = (uint8_t)h;
}

static void usage(const char *p)
{
    fprintf(stderr,
        "vbt-vhost-user — virtio-bluetooth vhost-user backend\n\n"
        "Usage: %s --socket <path> --medium <path> [options]\n\n"
        "  --socket <path>   vhost-user listen socket (QEMU chardev connects)\n"
        "  --medium <path>   vbt-medium hub socket\n"
        "  --node-id <id>    Node identity for the medium (default: vbt-vm)\n"
        "  --bdaddr <addr>   Public device address (default: derived from id)\n"
        "  -v                Verbose\n"
        "  -h                Help\n\n"
        "QEMU (unmodified) attaches it with:\n"
        "  -chardev socket,id=vbt,path=<socket> \\\n"
        "  -device vhost-user-device-pci,chardev=vbt,virtio-id=40,num_vqs=2\n",
        p);
}

int main(int argc, char **argv)
{
    memset(&g, 0, sizeof(g));
    g.medium_fd = -1;
    g.listen_fd = g.conn_fd = -1;
    snprintf(g.node_id, sizeof(g.node_id), "vbt-vm");
    const char *sock_path = NULL;
    bool bdaddr_set = false;

    static struct option lo[] = {
        { "socket",  required_argument, 0, 's' },
        { "medium",  required_argument, 0, 'm' },
        { "node-id", required_argument, 0, 'n' },
        { "bdaddr",  required_argument, 0, 'b' },
        { 0, 0, 0, 0 },
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "vh", lo, NULL)) != -1) {
        switch (opt) {
        case 's': sock_path = optarg; break;
        case 'm': snprintf(g.medium_path, sizeof(g.medium_path), "%s", optarg); break;
        case 'n': snprintf(g.node_id, sizeof(g.node_id), "%s", optarg); break;
        case 'b': {
            unsigned m[6];
            if (sscanf(optarg, "%x:%x:%x:%x:%x:%x",
                       &m[0],&m[1],&m[2],&m[3],&m[4],&m[5]) != 6) {
                fprintf(stderr, "bad --bdaddr\n"); return 2;
            }
            for (int i = 0; i < 6; i++) g.bdaddr[i] = (uint8_t)m[i];
            bdaddr_set = true;
            break;
        }
        case 'v': g.verbose = true; break;
        case 'h': default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }
    if (!sock_path || g.medium_path[0] == '\0') { usage(argv[0]); return 2; }
    if (!bdaddr_set) derive_bdaddr();

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    struct vbt_ll_ops ops = { cb_hci_to_host, cb_pdu_to_medium, &g };
    g.ll = vbt_ll_new(&ops, g.bdaddr);
    if (!g.ll) { fprintf(stderr, "out of memory\n"); return 1; }

    /* Listen for QEMU; be the server so the chardev can reconnect. */
    g.listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g.listen_fd < 0) { perror("socket"); return 1; }
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", sock_path);
    unlink(sock_path);
    if (bind(g.listen_fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(g.listen_fd, 1) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "vbt-vhost: listening on %s "
            "(bdaddr %02x:%02x:%02x:%02x:%02x:%02x)\n",
            sock_path, g.bdaddr[0], g.bdaddr[1], g.bdaddr[2],
            g.bdaddr[3], g.bdaddr[4], g.bdaddr[5]);

    connect_medium();

    while (g_running) {
        int c = accept(g.listen_fd, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; break; }
        fprintf(stderr, "vbt-vhost: QEMU connected\n");
        g.conn_fd = c;
        serve_connection(c);
        close(c);
        g.conn_fd = -1;
        fprintf(stderr, "vbt-vhost: QEMU disconnected\n");
    }

    if (g.medium_fd >= 0) close(g.medium_fd);
    if (g.listen_fd >= 0) close(g.listen_fd);
    unlink(sock_path);
    vbt_ll_free(g.ll);
    return 0;
}
