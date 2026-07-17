/*
 * test_ll — unit/integration test for the BLE controller core (vbt_ll).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wires two vbt_ll controller cores together through an in-process 2-node
 * "medium" (cross-delivery) and drives them via HCI exactly as a host
 * stack would, checking the emitted HCI events. This exercises the whole
 * controller path — advertising, scanning, connection setup, ACL data,
 * link-layer encryption, and disconnect — with no kernel, QEMU, or BlueZ.
 *
 * Build:  gcc -Wall -Wextra -O2 -o test-ll tests/test_ll.c vbt_ll.c
 * Run:    ./test-ll
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../vbt_ll.h"

/* ---- captured HCI event stream per core ---- */
#define EVTQ 65536
struct core {
    struct vbt_ll *ll;
    struct core   *peer;
    uint8_t        evt[EVTQ];
    size_t         evt_len;
    const char    *name;
};

static int g_pass, g_fail;
static void check(int cond, const char *what)
{
    if (cond) { g_pass++; printf("  PASS: %s\n", what); }
    else      { g_fail++; printf("  FAIL: %s\n", what); }
}

static void on_hci(void *ctx, const uint8_t *pkt, size_t len)
{
    struct core *c = ctx;
    if (c->evt_len + len <= EVTQ) {
        memcpy(c->evt + c->evt_len, pkt, len);
        c->evt_len += len;
    }
}

static void on_pdu(void *ctx, const struct vbt_frame_hdr *hdr,
                   const uint8_t *pdu, size_t pdu_len)
{
    struct core *c = ctx;
    /* 2-node medium: deliver straight to the peer. */
    vbt_ll_pdu_from_medium(c->peer->ll, hdr, pdu, pdu_len);
}

/* Walk the captured H4 event stream; return a pointer to the params of the
 * first event matching evt_code (and, for LE meta 0x3E, subevent), or NULL.
 * Sets *plen to the parameter length. */
static const uint8_t *find_evt(struct core *c, uint8_t evt_code,
                               int subevent, uint8_t *plen)
{
    size_t i = 0;
    while (i + 3 <= c->evt_len) {
        if (c->evt[i] != 0x04) break;           /* not an event packet */
        uint8_t code = c->evt[i + 1];
        uint8_t len = c->evt[i + 2];
        const uint8_t *params = c->evt + i + 3;
        if (i + 3 + len > c->evt_len) break;
        if (code == evt_code &&
            (subevent < 0 || (len >= 1 && params[0] == subevent))) {
            if (plen) *plen = len;
            return params;
        }
        i += 3 + len;
    }
    return NULL;
}

static void clear_evts(struct core *c) { c->evt_len = 0; }

/* ---- HCI command builders ---- */
static void cmd(struct core *c, uint16_t opcode, const uint8_t *p, uint8_t plen)
{
    uint8_t buf[260];
    buf[0] = 0x01;
    buf[1] = (uint8_t)(opcode & 0xff);
    buf[2] = (uint8_t)(opcode >> 8);
    buf[3] = plen;
    if (plen) memcpy(buf + 4, p, plen);
    vbt_ll_hci_from_host(c->ll, buf, 4 + plen);
}

static void acl(struct core *c, uint16_t handle, const uint8_t *l2, uint16_t n)
{
    uint8_t buf[512];
    uint16_t hf = (uint16_t)(handle | (0x0u << 12));   /* PB=00: first (host->ctrl) */
    buf[0] = 0x02;
    buf[1] = (uint8_t)(hf & 0xff);
    buf[2] = (uint8_t)(hf >> 8);
    buf[3] = (uint8_t)(n & 0xff);
    buf[4] = (uint8_t)(n >> 8);
    memcpy(buf + 5, l2, n);
    vbt_ll_hci_from_host(c->ll, buf, 5 + n);
}

int main(void)
{
    uint8_t addr_p[6] = { 0xAA, 0, 0, 0, 0, 0x01 };
    uint8_t addr_c[6] = { 0xAA, 0, 0, 0, 0, 0x02 };

    struct core P = { .name = "peripheral" };
    struct core C = { .name = "central" };
    struct vbt_ll_ops ops_p = { on_hci, on_pdu, &P };
    struct vbt_ll_ops ops_c = { on_hci, on_pdu, &C };
    P.ll = vbt_ll_new(&ops_p, addr_p);
    C.ll = vbt_ll_new(&ops_c, addr_c);
    P.peer = &C; C.peer = &P;

    printf("[1] HCI reset / init\n");
    cmd(&P, 0x0C03, NULL, 0);   /* Reset */
    cmd(&C, 0x0C03, NULL, 0);
    check(find_evt(&P, 0x0E, -1, NULL) != NULL, "peripheral Reset -> Command Complete");
    clear_evts(&C);
    cmd(&C, 0x1009, NULL, 0);   /* Read BD_ADDR */
    uint8_t pl = 0;
    /* Command Complete params: num_cmd(1) opcode(2) status(1) bdaddr(6). */
    const uint8_t *r = find_evt(&C, 0x0E, -1, &pl);
    check(r && pl >= 10 && r[3] == 0x00 && memcmp(r + 4, addr_c, 6) == 0,
          "Read BD_ADDR returns our address");

    printf("[2] advertising + discovery\n");
    /* Peripheral: set adv params/data, enable advertising. */
    uint8_t advp[15] = { 0xa0,0x00, 0xa0,0x00, 0x00, 0x00, 0x00,
                         0,0,0,0,0,0, 0x07, 0x00 };
    cmd(&P, 0x2006, advp, sizeof(advp));      /* Set Adv Params (ADV_IND) */
    uint8_t advd[8] = { 7, 0x02,0x01,0x06, 0x03,0x03,0x0f,0x18 };
    cmd(&P, 0x2008, advd, sizeof(advd));      /* Set Adv Data */
    uint8_t en1[1] = { 1 };
    cmd(&P, 0x200A, en1, 1);                  /* Adv Enable */

    /* Central: enable scanning. */
    uint8_t scp[7] = { 0x01, 0x10,0x00, 0x10,0x00, 0x00, 0x00 };
    cmd(&C, 0x200B, scp, sizeof(scp));        /* Set Scan Params (active) */
    clear_evts(&C);
    cmd(&C, 0x200C, en1, 1);                  /* Scan Enable */

    vbt_ll_tick(P.ll, 1000);                  /* peripheral beacons once */
    const uint8_t *rep = find_evt(&C, 0x3E, 0x02, &pl);
    check(rep != NULL, "central sees an LE Advertising Report");
    if (rep)
        check(memcmp(rep + 4, addr_p, 6) == 0,
              "advertising report carries the peripheral's address");

    printf("[3] connection setup\n");
    uint8_t cc[25] = {0};
    cc[0]=0x10; cc[1]=0x00; cc[2]=0x10; cc[3]=0x00;   /* scan int/window */
    cc[4]=0x00;                                       /* filter policy */
    cc[5]=0x00;                                       /* peer addr type */
    memcpy(cc + 6, addr_p, 6);                        /* peer addr */
    cc[12]=0x00;                                      /* own addr type */
    cc[13]=0x18; cc[14]=0x00; cc[15]=0x28; cc[16]=0x00; /* conn interval */
    clear_evts(&C); clear_evts(&P);
    cmd(&C, 0x200D, cc, sizeof(cc));                  /* LE Create Connection */
    vbt_ll_tick(P.ll, 2000);                          /* peripheral beacons; central connects */

    const uint8_t *ccc = find_evt(&C, 0x3E, 0x01, &pl);
    const uint8_t *ccp = find_evt(&P, 0x3E, 0x01, &pl);
    check(ccc && ccc[1] == 0x00, "central gets LE Connection Complete (success)");
    check(ccp && ccp[1] == 0x00, "peripheral gets LE Connection Complete (success)");
    uint16_t hc = 0, hp = 0;
    if (ccc) hc = (uint16_t)(ccc[2] | (ccc[3] << 8));
    if (ccp) hp = (uint16_t)(ccp[2] | (ccp[3] << 8));
    check(ccc && ccc[4] == 0x00, "central role = central");
    check(ccp && ccp[4] == 0x01, "peripheral role = peripheral");

    printf("[4] ACL data both directions\n");
    clear_evts(&P); clear_evts(&C);
    uint8_t l2[8] = { 0x04,0x00, 0x04,0x00, 0x0a,0x01,0x02,0x03 }; /* ATT-ish */
    acl(&C, hc, l2, sizeof(l2));
    const uint8_t *ncp = find_evt(&C, 0x13, -1, NULL);
    check(ncp != NULL, "central gets Number Of Completed Packets");
    /* peripheral should have received the ACL as an event? No — ACL arrives
     * as an ACL packet (0x02), not an event. Scan the raw stream for it. */
    bool got_acl = false;
    for (size_t i = 0; i + 5 <= P.evt_len; ) {
        if (P.evt[i] == 0x02) {
            uint16_t dl = (uint16_t)(P.evt[i+3] | (P.evt[i+4] << 8));
            if (dl == sizeof(l2) && memcmp(P.evt + i + 5, l2, sizeof(l2)) == 0)
                got_acl = true;
            i += 5 + dl;
        } else if (P.evt[i] == 0x04) {
            i += 3 + P.evt[i+2];
        } else break;
    }
    check(got_acl, "peripheral receives the central's ACL payload intact");

    clear_evts(&P); clear_evts(&C);
    uint8_t l2b[6] = { 0x02,0x00, 0x04,0x00, 0x0b,0x55 };
    acl(&P, hp, l2b, sizeof(l2b));
    bool got_acl_c = false;
    for (size_t i = 0; i + 5 <= C.evt_len; ) {
        if (C.evt[i] == 0x02) {
            uint16_t dl = (uint16_t)(C.evt[i+3] | (C.evt[i+4] << 8));
            if (dl == sizeof(l2b) && memcmp(C.evt + i + 5, l2b, sizeof(l2b)) == 0)
                got_acl_c = true;
            i += 5 + dl;
        } else if (C.evt[i] == 0x04) { i += 3 + C.evt[i+2]; }
        else break;
    }
    check(got_acl_c, "central receives the peripheral's ACL payload intact");

    printf("[5] link-layer encryption (pass-through)\n");
    clear_evts(&P); clear_evts(&C);
    uint8_t se[28] = {0};
    se[0] = (uint8_t)(hc & 0xff); se[1] = (uint8_t)(hc >> 8);
    cmd(&C, 0x2019, se, sizeof(se));          /* LE Start Encryption */
    /* Peripheral host must answer the LTK request. */
    const uint8_t *ltk = find_evt(&P, 0x3E, 0x05, &pl);
    check(ltk != NULL, "peripheral host gets LE LTK Request");
    uint8_t rr[18] = {0};
    rr[0] = (uint8_t)(hp & 0xff); rr[1] = (uint8_t)(hp >> 8);
    cmd(&P, 0x201A, rr, sizeof(rr));          /* LTK Request Reply */
    check(find_evt(&C, 0x08, -1, NULL) != NULL, "central gets Encryption Change");
    check(find_evt(&P, 0x08, -1, NULL) != NULL, "peripheral gets Encryption Change");

    printf("[6] disconnect\n");
    clear_evts(&P); clear_evts(&C);
    uint8_t dis[3] = { (uint8_t)(hc & 0xff), (uint8_t)(hc >> 8), 0x13 };
    cmd(&C, 0x0406, dis, sizeof(dis));        /* Disconnect */
    check(find_evt(&C, 0x05, -1, NULL) != NULL, "central gets Disconnection Complete");
    check(find_evt(&P, 0x05, -1, NULL) != NULL, "peripheral gets Disconnection Complete");

    vbt_ll_free(P.ll);
    vbt_ll_free(C.ll);

    printf("\nRESULTS: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
