/*
 * vbt-controller — host BLE controller bridging Linux hci_vhci to the medium
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Attaches the portable BLE controller core (vbt_ll) to Linux's virtual
 * HCI driver (`/dev/vhci`, hci_vhci) on one side and the vbt-medium hub on
 * the other. The result is a new `hciN` that BlueZ drives exactly like a
 * real adapter — advertise, scan, pair, GATT — while its Link-Layer PDUs
 * ride the virtual medium alongside the QEMU vbt-virtio VMs.
 *
 * The same vbt_ll core powers the QEMU device model; only this transport
 * glue differs.
 *
 * Usage:
 *   sudo modprobe hci_vhci
 *   sudo ./vbt-controller /tmp/vbt.sock --node-id host-a
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
#include <arpa/inet.h>

#include "vbt_ll.h"

/* hci_vhci packet-type indicators. */
#define HCI_VENDOR_PKT   0xff
#define HCI_PRIMARY      0x00   /* dev_type: BR/EDR + LE primary controller */

#define VHCI_PATH        "/dev/vhci"
#define MEDIUM_RECONNECT_MS 2000
#define TICK_MS          5

static volatile sig_atomic_t g_running = 1;
static void on_signal(int s) { (void)s; g_running = 0; }

struct ctx {
    int         vhci_fd;
    int         medium_fd;
    char        medium_path[108];   /* fits sockaddr_un.sun_path */
    char        node_id[64];
    bool        verbose;
    uint8_t     rxbuf[VBT_RXBUF_SIZE];
    uint32_t    rxused;
    struct vbt_ll *ll;
    uint64_t    tx_pdus, rx_pdus, hci_in, hci_out;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool parse_addr(const char *s, uint8_t *out)
{
    unsigned m[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x",
               &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6)
        return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)m[i];
    return true;
}

/* ---- vbt_ll callbacks ---- */
static void cb_hci_to_host(void *c, const uint8_t *pkt, size_t len)
{
    struct ctx *x = c;
    ssize_t w = write(x->vhci_fd, pkt, len);
    if (w < 0 && x->verbose)
        fprintf(stderr, "vbt-controller: vhci write failed: %s\n",
                strerror(errno));
    x->hci_out++;
}

static void cb_pdu_to_medium(void *c, const struct vbt_frame_hdr *hdr,
                             const uint8_t *pdu, size_t pdu_len)
{
    struct ctx *x = c;
    if (x->medium_fd < 0) return;
    uint8_t wire[4 + VBT_MAX_MSG_SIZE];
    uint32_t msg = (uint32_t)VBT_HDR_SIZE + (uint32_t)pdu_len;
    uint32_t net = htonl(msg);
    memcpy(wire, &net, 4);
    memcpy(wire + 4, hdr, VBT_HDR_SIZE);
    memcpy(wire + 4 + VBT_HDR_SIZE, pdu, pdu_len);
    uint32_t total = 4 + msg, off = 0;
    while (off < total) {
        ssize_t n = write(x->medium_fd, wire + off, total - off);
        if (n > 0) { off += (uint32_t)n; continue; }
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (x->verbose)
            fprintf(stderr, "vbt-controller: medium write failed\n");
        close(x->medium_fd);
        x->medium_fd = -1;
        return;
    }
    x->tx_pdus++;
}

/* ---- medium connection ---- */
static bool connect_medium(struct ctx *x)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", x->medium_path);
    if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
        close(fd);
        return false;
    }
    /* Hello: [len][VBT_HELLO_MAGIC][node_id\0] */
    uint8_t hb[4 + 4 + 64];
    size_t idlen = strlen(x->node_id) + 1;
    uint32_t payload = 4 + (uint32_t)idlen;
    uint32_t net = htonl(payload);
    uint32_t magic = VBT_HELLO_MAGIC;
    memcpy(hb, &net, 4);
    memcpy(hb + 4, &magic, 4);
    memcpy(hb + 8, x->node_id, idlen);
    if (write(fd, hb, 4 + payload) < 0) { close(fd); return false; }
    x->medium_fd = fd;
    x->rxused = 0;
    fprintf(stderr, "vbt-controller: connected to medium %s (node_id=%s)\n",
            x->medium_path, x->node_id);
    return true;
}

static void medium_readable(struct ctx *x)
{
    uint32_t used = x->rxused;
    if (used >= sizeof(x->rxbuf)) used = 0;             /* defensive */
    size_t avail = sizeof(x->rxbuf) - used;
    ssize_t n = read(x->medium_fd, x->rxbuf + used, avail);
    if (n <= 0) {
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) return;
        fprintf(stderr, "vbt-controller: medium disconnected\n");
        close(x->medium_fd);
        x->medium_fd = -1;
        return;
    }
    x->rxused = used + (uint32_t)n;

    while (x->rxused >= 4) {
        uint32_t plen;
        memcpy(&plen, x->rxbuf, 4);
        plen = ntohl(plen);
        if (plen > VBT_MAX_MSG_SIZE) { x->rxused = 0; break; }
        if (x->rxused < 4 + plen) break;
        if (plen >= VBT_HDR_SIZE) {
            struct vbt_frame_hdr hdr;
            memcpy(&hdr, x->rxbuf + 4, VBT_HDR_SIZE);
            if (hdr.magic == VBT_MAGIC) {
                uint32_t pdu_len = plen - (uint32_t)VBT_HDR_SIZE;
                if (pdu_len == hdr.pdu_len)
                    vbt_ll_pdu_from_medium(x->ll, &hdr,
                                           x->rxbuf + 4 + VBT_HDR_SIZE, pdu_len);
                x->rx_pdus++;
            }
        }
        uint32_t total = 4 + plen;
        x->rxused -= total;
        if (x->rxused > 0) memmove(x->rxbuf, x->rxbuf + total, x->rxused);
    }
}

static void vhci_readable(struct ctx *x)
{
    uint8_t buf[VBT_MAX_MSG_SIZE];
    ssize_t n = read(x->vhci_fd, buf, sizeof(buf));
    if (n <= 0) {
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) return;
        fprintf(stderr, "vbt-controller: vhci closed\n");
        g_running = 0;
        return;
    }
    x->hci_in++;
    vbt_ll_hci_from_host(x->ll, buf, (size_t)n);
}

static void usage(const char *p)
{
    fprintf(stderr,
        "vbt-controller — host BLE controller (hci_vhci <-> vbt-medium)\n\n"
        "Usage: sudo %s <medium-socket> [options]\n\n"
        "  --node-id <id>    Node identity for the medium (default: vbt-host)\n"
        "  --bdaddr <addr>   Public device address (default: derived from id)\n"
        "  --vhci <path>     vhci device path (default: %s)\n"
        "  -v                Verbose logging\n"
        "  -h                Help\n\n"
        "Load hci_vhci first:  sudo modprobe hci_vhci\n",
        p, VHCI_PATH);
}

int main(int argc, char **argv)
{
    struct ctx x;
    memset(&x, 0, sizeof(x));
    x.vhci_fd = x.medium_fd = -1;
    snprintf(x.node_id, sizeof(x.node_id), "vbt-host");
    const char *vhci_path = VHCI_PATH;
    uint8_t bdaddr[6];
    bool bdaddr_set = false;

    static struct option lo[] = {
        { "node-id", required_argument, 0, 'n' },
        { "bdaddr",  required_argument, 0, 'b' },
        { "vhci",    required_argument, 0, 'V' },
        { 0, 0, 0, 0 },
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "vh", lo, NULL)) != -1) {
        switch (opt) {
        case 'n': snprintf(x.node_id, sizeof(x.node_id), "%s", optarg); break;
        case 'b':
            if (!parse_addr(optarg, bdaddr)) {
                fprintf(stderr, "bad --bdaddr\n"); return 2;
            }
            bdaddr_set = true;
            break;
        case 'V': vhci_path = optarg; break;
        case 'v': x.verbose = true; break;
        case 'h': default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }
    if (optind >= argc) { usage(argv[0]); return 2; }
    snprintf(x.medium_path, sizeof(x.medium_path), "%s", argv[optind]);

    if (!bdaddr_set) {
        /* Derive a stable locally-administered address from the node id. */
        uint32_t h = 0x811c9dc5u;
        for (const char *p = x.node_id; *p; p++) h = (h ^ (uint8_t)*p) * 0x01000193u;
        bdaddr[0] = 0x02;               /* locally administered, unicast */
        bdaddr[1] = 0xbe;
        bdaddr[2] = (uint8_t)(h >> 24);
        bdaddr[3] = (uint8_t)(h >> 16);
        bdaddr[4] = (uint8_t)(h >> 8);
        bdaddr[5] = (uint8_t)h;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    struct vbt_ll_ops ops = { cb_hci_to_host, cb_pdu_to_medium, &x };
    x.ll = vbt_ll_new(&ops, bdaddr);
    if (!x.ll) { fprintf(stderr, "out of memory\n"); return 1; }

    /* Open vhci and create the controller device. */
    x.vhci_fd = open(vhci_path, O_RDWR);
    if (x.vhci_fd < 0) {
        fprintf(stderr, "vbt-controller: open %s: %s\n"
                "  (is hci_vhci loaded? try: sudo modprobe hci_vhci)\n",
                vhci_path, strerror(errno));
        return 1;
    }
    uint8_t create[2] = { HCI_VENDOR_PKT, HCI_PRIMARY };
    if (write(x.vhci_fd, create, sizeof(create)) < 0) {
        fprintf(stderr, "vbt-controller: vhci create failed: %s\n",
                strerror(errno));
        return 1;
    }
    fprintf(stderr, "vbt-controller: created vhci controller "
            "%02x:%02x:%02x:%02x:%02x:%02x\n",
            bdaddr[0], bdaddr[1], bdaddr[2], bdaddr[3], bdaddr[4], bdaddr[5]);

    if (!connect_medium(&x))
        fprintf(stderr, "vbt-controller: medium %s not up yet, retrying...\n",
                x.medium_path);

    uint64_t last_tick = now_ms(), last_reconnect = 0;
    while (g_running) {
        struct pollfd pfds[2];
        int nf = 0;
        pfds[nf].fd = x.vhci_fd; pfds[nf].events = POLLIN; nf++;
        int medium_row = -1;
        if (x.medium_fd >= 0) {
            pfds[nf].fd = x.medium_fd; pfds[nf].events = POLLIN;
            medium_row = nf; nf++;
        }
        int r = poll(pfds, nf, TICK_MS);
        uint64_t t = now_ms();

        if (r > 0) {
            if (pfds[0].revents & (POLLERR | POLLHUP)) { g_running = 0; break; }
            if (pfds[0].revents & POLLIN) vhci_readable(&x);
            if (medium_row >= 0 && (pfds[medium_row].revents & POLLIN))
                medium_readable(&x);
        }

        if (t - last_tick >= TICK_MS) {
            vbt_ll_tick(x.ll, t);
            last_tick = t;
        }
        if (x.medium_fd < 0 && t - last_reconnect >= MEDIUM_RECONNECT_MS) {
            last_reconnect = t;
            connect_medium(&x);
        }
    }

    fprintf(stderr, "vbt-controller: shutting down "
            "(hci in=%llu out=%llu, medium tx=%llu rx=%llu)\n",
            (unsigned long long)x.hci_in, (unsigned long long)x.hci_out,
            (unsigned long long)x.tx_pdus, (unsigned long long)x.rx_pdus);
    if (x.vhci_fd >= 0) close(x.vhci_fd);
    if (x.medium_fd >= 0) close(x.medium_fd);
    vbt_ll_free(x.ll);
    return 0;
}
