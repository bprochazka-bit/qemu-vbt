/*
 * QEMU Virtual BLE Controller — virtio-bluetooth device
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A virtio-bluetooth controller device for QEMU guests. It presents a
 * standard HCI transport to the guest (the in-tree `virtio_bt` driver
 * binds and BlueZ sees an `hci0`), runs the portable BLE controller core
 * (vbt_ll: HCI + Link Layer), and bridges Link-Layer PDUs to a vbt-medium
 * hub over a Unix socket — exactly the socket/wire protocol the
 * vbt-controller and vbt-medium tools use.
 *
 * Because the controller lives here and the medium only fans out PDUs,
 * the guest's unmodified BlueZ stack performs advertising, discovery, SMP
 * pairing, and GATT end-to-end with its peers on the medium.
 *
 * Usage:
 *   -device virtio-bluetooth-pci,medium=/tmp/vbt.sock,node_id=vm-a
 *
 * This file is compiled inside a QEMU source tree; see src/README.md and
 * scripts/integrate.sh. The controller core it wraps (vbt_ll) is unit
 * tested in the repo (tests/test_ll.c); this transport glue is verified
 * by building QEMU and attaching a guest.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/iov.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-pci.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#include "vbt_ll.h"
#include "vbt.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>

/* virtio device id for Bluetooth (see linux/virtio_ids.h). */
#ifndef VIRTIO_ID_BLUETOOTH
#define VIRTIO_ID_BLUETOOTH  40
#endif

/* virtio-bt config space (v1 layout). type = primary controller. */
#define VIRTIO_BT_CONFIG_TYPE_PRIMARY   0
#define VIRTIO_BT_CONFIG_VENDOR_NONE    0

struct virtio_bt_config {
    uint8_t  type;
    uint8_t  vendor;
    uint16_t msft_opcode;
};

#define MEDIUM_RECONNECT_MS  2000
#define TICK_MS              5
#define PENDING_MAX          256      /* HCI packets buffered toward guest */

/* -------------------------------------------------------------------
 *  Device state
 * ------------------------------------------------------------------- */
#define TYPE_VIRTIO_BT "virtio-bluetooth"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOBT, VIRTIO_BT)

struct pending_pkt {
    uint8_t  data[VBT_MAX_MSG_SIZE];
    uint32_t len;
};

struct VirtIOBT {
    VirtIODevice parent_obj;

    VirtQueue   *tx_vq;      /* guest -> controller (HCI cmd / ACL out) */
    VirtQueue   *rx_vq;      /* controller -> guest (HCI evt / ACL in)  */

    /* Configuration properties. */
    char        *medium_path;
    char        *node_id;
    char        *bdaddr_str;
    uint8_t      bdaddr[6];

    /* Controller core. */
    struct vbt_ll *ll;

    /* Medium socket. */
    int          medium_fd;
    bool         medium_connected;
    uint8_t      medium_rxbuf[VBT_RXBUF_SIZE];
    uint32_t     medium_rxbuf_used;

    /* HCI packets waiting for a free guest RX buffer. */
    struct pending_pkt pending[PENDING_MAX];
    uint32_t     pend_head, pend_tail;

    QEMUTimer   *tick_timer;
    QEMUTimer   *reconnect_timer;

    uint64_t     hci_from_guest, hci_to_guest, pdu_tx, pdu_rx;
};

/* -------------------------------------------------------------------
 *  Guest RX: deliver an HCI packet up to the driver
 * ------------------------------------------------------------------- */
static void virtio_bt_flush_pending(VirtIOBT *s)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(s);

    while (s->pend_head != s->pend_tail) {
        VirtQueueElement *elem = virtqueue_pop(s->rx_vq, sizeof(*elem));
        if (!elem) break;   /* no guest buffer available; try again later */

        struct pending_pkt *p = &s->pending[s->pend_head % PENDING_MAX];
        size_t n = iov_from_buf(elem->in_sg, elem->in_num, 0, p->data, p->len);
        virtqueue_push(s->rx_vq, elem, n);
        g_free(elem);
        s->pend_head++;
        s->hci_to_guest++;
    }
    virtio_notify(vdev, s->rx_vq);
}

static void virtio_bt_queue_to_guest(VirtIOBT *s, const uint8_t *pkt, size_t len)
{
    if (len > VBT_MAX_MSG_SIZE) return;
    if (s->pend_tail - s->pend_head >= PENDING_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "virtio-bt: RX backlog full, dropping HCI packet\n");
        return;
    }
    struct pending_pkt *p = &s->pending[s->pend_tail % PENDING_MAX];
    memcpy(p->data, pkt, len);
    p->len = (uint32_t)len;
    s->pend_tail++;
    virtio_bt_flush_pending(s);
}

/* -------------------------------------------------------------------
 *  vbt_ll callbacks
 * ------------------------------------------------------------------- */
static void cb_hci_to_host(void *ctx, const uint8_t *pkt, size_t len)
{
    virtio_bt_queue_to_guest((VirtIOBT *)ctx, pkt, len);
}

static void cb_pdu_to_medium(void *ctx, const struct vbt_frame_hdr *hdr,
                             const uint8_t *pdu, size_t pdu_len)
{
    VirtIOBT *s = ctx;
    if (!s->medium_connected || s->medium_fd < 0) return;

    uint8_t wire[4 + VBT_MAX_MSG_SIZE];
    uint32_t msg = (uint32_t)VBT_HDR_SIZE + (uint32_t)pdu_len;
    uint32_t net = htonl(msg);
    memcpy(wire, &net, 4);
    memcpy(wire + 4, hdr, VBT_HDR_SIZE);
    memcpy(wire + 4 + VBT_HDR_SIZE, pdu, pdu_len);

    uint32_t total = 4 + msg, off = 0;
    while (off < total) {
        ssize_t n = write(s->medium_fd, wire + off, total - off);
        if (n > 0) { off += (uint32_t)n; continue; }
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        qemu_log_mask(LOG_GUEST_ERROR, "virtio-bt: medium write failed\n");
        qemu_set_fd_handler(s->medium_fd, NULL, NULL, NULL);
        close(s->medium_fd);
        s->medium_fd = -1;
        s->medium_connected = false;
        return;
    }
    s->pdu_tx++;
}

/* -------------------------------------------------------------------
 *  Guest TX virtqueue: HCI from the guest driver
 * ------------------------------------------------------------------- */
static void virtio_bt_handle_tx(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOBT *s = VIRTIO_BT(vdev);
    VirtQueueElement *elem;

    while ((elem = virtqueue_pop(vq, sizeof(*elem))) != NULL) {
        uint8_t pkt[VBT_MAX_MSG_SIZE];
        size_t len = iov_to_buf(elem->out_sg, elem->out_num, 0,
                                pkt, sizeof(pkt));
        if (len > 0) {
            s->hci_from_guest++;
            vbt_ll_hci_from_host(s->ll, pkt, len);
        }
        virtqueue_push(vq, elem, 0);
        g_free(elem);
    }
    virtio_notify(vdev, vq);
}

static void virtio_bt_handle_rx(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOBT *s = VIRTIO_BT(vdev);
    (void)vq;
    /* The guest supplied fresh RX buffers; drain anything pending. */
    virtio_bt_flush_pending(s);
}

/* -------------------------------------------------------------------
 *  Medium socket I/O
 * ------------------------------------------------------------------- */
static void virtio_bt_schedule_reconnect(VirtIOBT *s);

static void virtio_bt_medium_read(void *opaque)
{
    VirtIOBT *s = opaque;
    if (s->medium_fd < 0) return;

    uint32_t used = s->medium_rxbuf_used;
    if (used >= VBT_RXBUF_SIZE) { s->medium_rxbuf_used = used = 0; }
    ssize_t n = read(s->medium_fd, s->medium_rxbuf + used, VBT_RXBUF_SIZE - used);
    if (n <= 0) {
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) return;
        qemu_set_fd_handler(s->medium_fd, NULL, NULL, NULL);
        close(s->medium_fd);
        s->medium_fd = -1;
        s->medium_connected = false;
        s->medium_rxbuf_used = 0;
        virtio_bt_schedule_reconnect(s);
        return;
    }
    s->medium_rxbuf_used = used + (uint32_t)n;

    while (s->medium_rxbuf_used >= 4) {
        uint32_t net, msg;
        memcpy(&net, s->medium_rxbuf, 4);
        msg = ntohl(net);
        if (msg > VBT_MAX_MSG_SIZE) { s->medium_rxbuf_used = 0; break; }
        if (s->medium_rxbuf_used < 4 + msg) break;

        if (msg >= VBT_HDR_SIZE) {
            struct vbt_frame_hdr hdr;
            memcpy(&hdr, s->medium_rxbuf + 4, VBT_HDR_SIZE);
            if (hdr.magic == VBT_MAGIC) {
                uint32_t pdu_len = msg - (uint32_t)VBT_HDR_SIZE;
                if (pdu_len == hdr.pdu_len) {
                    vbt_ll_pdu_from_medium(s->ll, &hdr,
                                           s->medium_rxbuf + 4 + VBT_HDR_SIZE,
                                           pdu_len);
                    s->pdu_rx++;
                }
            }
        }
        uint32_t consumed = 4 + msg;
        s->medium_rxbuf_used -= consumed;
        if (s->medium_rxbuf_used)
            memmove(s->medium_rxbuf, s->medium_rxbuf + consumed,
                    s->medium_rxbuf_used);
    }
}

static bool virtio_bt_try_connect(VirtIOBT *s)
{
    if (!s->medium_path || s->medium_path[0] == '\0') return false;
    if (s->medium_connected) return true;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", s->medium_path);
    if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
        close(fd);
        return false;
    }
    s->medium_fd = fd;
    s->medium_connected = true;
    s->medium_rxbuf_used = 0;
    qemu_set_fd_handler(fd, virtio_bt_medium_read, NULL, s);

    /* Hello: [len][VBT_HELLO_MAGIC][node_id\0] */
    const char *nid = (s->node_id && s->node_id[0]) ? s->node_id : "vbt-vm";
    size_t idlen = strlen(nid) + 1;
    uint8_t hb[4 + 4 + 64];
    if (idlen > 64) idlen = 64;
    uint32_t payload = 4 + (uint32_t)idlen;
    uint32_t net = htonl(payload);
    uint32_t magic = VBT_HELLO_MAGIC;
    memcpy(hb, &net, 4);
    memcpy(hb + 4, &magic, 4);
    memcpy(hb + 8, nid, idlen);
    hb[8 + idlen - 1] = '\0';
    if (write(fd, hb, 4 + payload) < 0) { /* best effort */ }

    timer_del(s->reconnect_timer);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "virtio-bt: connected to medium %s (node_id=%s)\n",
                  s->medium_path, nid);
    return true;
}

static void virtio_bt_reconnect_cb(void *opaque)
{
    VirtIOBT *s = opaque;
    if (s->medium_connected) return;
    if (!virtio_bt_try_connect(s))
        timer_mod(s->reconnect_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + MEDIUM_RECONNECT_MS);
}

static void virtio_bt_schedule_reconnect(VirtIOBT *s)
{
    timer_mod(s->reconnect_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + MEDIUM_RECONNECT_MS);
}

static void virtio_bt_tick_cb(void *opaque)
{
    VirtIOBT *s = opaque;
    vbt_ll_tick(s->ll, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
    virtio_bt_flush_pending(s);
    timer_mod(s->tick_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + TICK_MS);
}

/* -------------------------------------------------------------------
 *  virtio glue
 * ------------------------------------------------------------------- */
static uint64_t virtio_bt_get_features(VirtIODevice *vdev, uint64_t features,
                                       Error **errp)
{
    (void)vdev; (void)errp;
    return features;   /* virtio core adds VIRTIO_F_VERSION_1 etc. */
}

static void virtio_bt_get_config(VirtIODevice *vdev, uint8_t *config)
{
    struct virtio_bt_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.type = VIRTIO_BT_CONFIG_TYPE_PRIMARY;
    cfg.vendor = VIRTIO_BT_CONFIG_VENDOR_NONE;
    cfg.msft_opcode = 0;
    memcpy(config, &cfg, sizeof(cfg));
}

static void virtio_bt_set_config(VirtIODevice *vdev, const uint8_t *config)
{
    (void)vdev; (void)config;   /* config is read-only for the guest */
}

static void virtio_bt_reset(VirtIODevice *vdev)
{
    VirtIOBT *s = VIRTIO_BT(vdev);
    s->pend_head = s->pend_tail = 0;
    if (s->ll) {
        uint8_t reset_cmd[4] = { HCI_CMD_PKT, 0x03, 0x0c, 0x00 };
        vbt_ll_hci_from_host(s->ll, reset_cmd, sizeof(reset_cmd));
    }
}

static void virtio_bt_parse_bdaddr(VirtIOBT *s)
{
    unsigned m[6];
    if (s->bdaddr_str && sscanf(s->bdaddr_str, "%x:%x:%x:%x:%x:%x",
                                &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
        for (int i = 0; i < 6; i++) s->bdaddr[i] = (uint8_t)m[i];
        return;
    }
    /* Derive a stable locally-administered address from node_id. */
    const char *nid = (s->node_id && s->node_id[0]) ? s->node_id : "vbt-vm";
    uint32_t h = 0x811c9dc5u;
    for (const char *p = nid; *p; p++) h = (h ^ (uint8_t)*p) * 0x01000193u;
    s->bdaddr[0] = 0x02; s->bdaddr[1] = 0xbe;
    s->bdaddr[2] = (uint8_t)(h >> 24); s->bdaddr[3] = (uint8_t)(h >> 16);
    s->bdaddr[4] = (uint8_t)(h >> 8);  s->bdaddr[5] = (uint8_t)h;
}

static void virtio_bt_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOBT *s = VIRTIO_BT(dev);

    virtio_init(vdev, VIRTIO_ID_BLUETOOTH, sizeof(struct virtio_bt_config));

    s->tx_vq = virtio_add_queue(vdev, 128, virtio_bt_handle_tx);
    s->rx_vq = virtio_add_queue(vdev, 128, virtio_bt_handle_rx);

    virtio_bt_parse_bdaddr(s);

    static const struct vbt_ll_ops ops = { cb_hci_to_host, cb_pdu_to_medium,
                                           NULL };
    struct vbt_ll_ops o = ops;
    o.ctx = s;
    s->ll = vbt_ll_new(&o, s->bdaddr);
    if (!s->ll) {
        error_setg(errp, "virtio-bt: failed to create controller core");
        return;
    }

    s->medium_fd = -1;
    s->medium_connected = false;
    s->pend_head = s->pend_tail = 0;

    s->tick_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, virtio_bt_tick_cb, s);
    s->reconnect_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                      virtio_bt_reconnect_cb, s);
    timer_mod(s->tick_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + TICK_MS);

    if (!virtio_bt_try_connect(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "virtio-bt: medium %s not available, retrying\n",
                      s->medium_path ? s->medium_path : "(none)");
        virtio_bt_schedule_reconnect(s);
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "virtio-bt: realized bdaddr %02x:%02x:%02x:%02x:%02x:%02x\n",
                  s->bdaddr[0], s->bdaddr[1], s->bdaddr[2],
                  s->bdaddr[3], s->bdaddr[4], s->bdaddr[5]);
}

static void virtio_bt_device_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOBT *s = VIRTIO_BT(dev);

    if (s->tick_timer) { timer_del(s->tick_timer); timer_free(s->tick_timer); }
    if (s->reconnect_timer) {
        timer_del(s->reconnect_timer);
        timer_free(s->reconnect_timer);
    }
    if (s->medium_fd >= 0) {
        qemu_set_fd_handler(s->medium_fd, NULL, NULL, NULL);
        close(s->medium_fd);
        s->medium_fd = -1;
    }
    if (s->ll) { vbt_ll_free(s->ll); s->ll = NULL; }

    virtio_delete_queue(s->tx_vq);
    virtio_delete_queue(s->rx_vq);
    virtio_cleanup(vdev);
}

static const VMStateDescription vmstate_virtio_bt = {
    .name = "virtio-bluetooth",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_VIRTIO_DEVICE,
        VMSTATE_END_OF_LIST()
    },
};

static void virtio_bt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    (void)data;

    /* The user-facing string properties (medium/node_id/bdaddr) are added
     * on the virtio-bluetooth-pci proxy in its instance_init via
     * object_property_add_str(), so this device needs no qdev-property
     * table — and thus no hw/qdev-properties.h, which some QEMU trees do
     * not expose to out-of-tree device objects. */
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    dc->vmsd = &vmstate_virtio_bt;

    vdc->realize = virtio_bt_device_realize;
    vdc->unrealize = virtio_bt_device_unrealize;
    vdc->get_config = virtio_bt_get_config;
    vdc->set_config = virtio_bt_set_config;
    vdc->get_features = virtio_bt_get_features;
    vdc->reset = virtio_bt_reset;
}

static const TypeInfo virtio_bt_info = {
    .name          = TYPE_VIRTIO_BT,
    .parent        = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOBT),
    .class_init    = virtio_bt_class_init,
};

/* -------------------------------------------------------------------
 *  virtio-pci binding: -device virtio-bluetooth-pci
 * ------------------------------------------------------------------- */
#define TYPE_VIRTIO_BT_PCI "virtio-bluetooth-pci-base"
typedef struct VirtIOBTPCI VirtIOBTPCI;
DECLARE_INSTANCE_CHECKER(VirtIOBTPCI, VIRTIO_BT_PCI, TYPE_VIRTIO_BT_PCI)

struct VirtIOBTPCI {
    VirtIOPCIProxy parent_obj;
    VirtIOBT vdev;
};

static void virtio_bt_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
    VirtIOBTPCI *dev = VIRTIO_BT_PCI(vpci_dev);
    DeviceState *vdev = DEVICE(&dev->vdev);
    virtio_pci_force_virtio_1(vpci_dev);
    qdev_realize(vdev, BUS(&vpci_dev->bus), errp);
}

static void virtio_bt_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    (void)data;

    /* vendor/device/class ids are assigned by the virtio-pci proxy from the
     * virtio device id (VIRTIO_ID_BLUETOOTH); we only supply the realize
     * hook, category, and description. */
    k->realize = virtio_bt_pci_realize;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    dc->desc = "Virtual BLE controller (virtio-bluetooth)";
}

/*
 * String properties are registered directly on the proxy object the user
 * instantiates (-device virtio-bluetooth-pci,medium=...,node_id=...), so
 * no qdev-property aliasing (and no hw/qdev-properties.h) is needed. Each
 * setter stores into the embedded VirtIOBT, which realize then reads.
 */
static char *virtio_bt_pci_get_medium(Object *obj, Error **errp)
{
    (void)errp;
    VirtIOBTPCI *d = VIRTIO_BT_PCI(obj);
    return g_strdup(d->vdev.medium_path ? d->vdev.medium_path : "");
}
static void virtio_bt_pci_set_medium(Object *obj, const char *v, Error **errp)
{
    (void)errp;
    VirtIOBTPCI *d = VIRTIO_BT_PCI(obj);
    g_free(d->vdev.medium_path);
    d->vdev.medium_path = g_strdup(v);
}
static char *virtio_bt_pci_get_node_id(Object *obj, Error **errp)
{
    (void)errp;
    VirtIOBTPCI *d = VIRTIO_BT_PCI(obj);
    return g_strdup(d->vdev.node_id ? d->vdev.node_id : "");
}
static void virtio_bt_pci_set_node_id(Object *obj, const char *v, Error **errp)
{
    (void)errp;
    VirtIOBTPCI *d = VIRTIO_BT_PCI(obj);
    g_free(d->vdev.node_id);
    d->vdev.node_id = g_strdup(v);
}
static char *virtio_bt_pci_get_bdaddr(Object *obj, Error **errp)
{
    (void)errp;
    VirtIOBTPCI *d = VIRTIO_BT_PCI(obj);
    return g_strdup(d->vdev.bdaddr_str ? d->vdev.bdaddr_str : "");
}
static void virtio_bt_pci_set_bdaddr(Object *obj, const char *v, Error **errp)
{
    (void)errp;
    VirtIOBTPCI *d = VIRTIO_BT_PCI(obj);
    g_free(d->vdev.bdaddr_str);
    d->vdev.bdaddr_str = g_strdup(v);
}

static void virtio_bt_pci_instance_init(Object *obj)
{
    VirtIOBTPCI *dev = VIRTIO_BT_PCI(obj);
    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_BT);
    object_property_add_str(obj, "medium",
                            virtio_bt_pci_get_medium, virtio_bt_pci_set_medium);
    object_property_add_str(obj, "node_id",
                            virtio_bt_pci_get_node_id, virtio_bt_pci_set_node_id);
    object_property_add_str(obj, "bdaddr",
                            virtio_bt_pci_get_bdaddr, virtio_bt_pci_set_bdaddr);
}

static const VirtioPCIDeviceTypeInfo virtio_bt_pci_info = {
    .base_name     = TYPE_VIRTIO_BT_PCI,
    .generic_name  = "virtio-bluetooth-pci",
    .instance_size = sizeof(VirtIOBTPCI),
    .instance_init = virtio_bt_pci_instance_init,
    .class_init    = virtio_bt_pci_class_init,
};

static void virtio_bt_register_types(void)
{
    type_register_static(&virtio_bt_info);
    virtio_pci_types_register(&virtio_bt_pci_info);
}

type_init(virtio_bt_register_types)
