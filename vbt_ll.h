/*
 * vbt_ll — Portable BLE controller core (HCI + Link Layer)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A transport-agnostic Bluetooth Low Energy controller: it consumes HCI
 * packets from a host stack, drives a Link-Layer state machine
 * (advertising, scanning, initiating, connected), and exchanges BLE
 * Link-Layer PDUs with the virtual medium. Both the QEMU vbt-virtio
 * device and the vbt-controller vhci daemon embed this same core; only
 * the transport glue differs.
 *
 * The core never blocks and does no I/O itself. It calls out through two
 * callbacks — one to hand an HCI packet up to the host, one to put a PDU
 * on the medium — and is fed by three entry points: HCI from the host,
 * PDUs from the medium, and a periodic tick.
 *
 * Everything above HCI (L2CAP, ATT/GATT, SMP pairing) lives in the host
 * stack and rides through as ACL data, so this core deliberately knows
 * nothing about services or pairing beyond carrying the bytes and
 * modelling link-layer encryption as a trusted-medium pass-through.
 */

#ifndef VBT_LL_H
#define VBT_LL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "vbt.h"

/* HCI H4 packet type indicators (first byte of each HCI packet). */
#define HCI_CMD_PKT     0x01
#define HCI_ACL_PKT     0x02
#define HCI_SCO_PKT     0x03
#define HCI_EVT_PKT     0x04

struct vbt_ll;

struct vbt_ll_ops {
    /* Deliver an HCI packet (H4 type-prefixed) up to the host stack. */
    void (*hci_to_host)(void *ctx, const uint8_t *pkt, size_t len);
    /* Put a BLE Link-Layer PDU on the medium. The core fills a fully
     * populated medium header; the transport prepends the length and
     * writes it to the hub socket. */
    void (*pdu_to_medium)(void *ctx, const struct vbt_frame_hdr *hdr,
                          const uint8_t *pdu, size_t pdu_len);
    void *ctx;
};

/* Create/destroy a controller core with the given public device address. */
struct vbt_ll *vbt_ll_new(const struct vbt_ll_ops *ops, const uint8_t bdaddr[6]);
void vbt_ll_free(struct vbt_ll *ll);

/* Host -> controller: one HCI packet (H4 type-prefixed). */
void vbt_ll_hci_from_host(struct vbt_ll *ll, const uint8_t *pkt, size_t len);

/* Medium -> controller: one received wire frame (header + PDU). */
void vbt_ll_pdu_from_medium(struct vbt_ll *ll, const struct vbt_frame_hdr *hdr,
                            const uint8_t *pdu, size_t pdu_len);

/* Periodic tick in milliseconds — drives advertising/scan timers and
 * connection supervision. Call roughly every 5–10 ms. */
void vbt_ll_tick(struct vbt_ll *ll, uint64_t now_ms);

/* The controller's current public address (for logging/registration). */
const uint8_t *vbt_ll_bdaddr(const struct vbt_ll *ll);

#endif /* VBT_LL_H */
