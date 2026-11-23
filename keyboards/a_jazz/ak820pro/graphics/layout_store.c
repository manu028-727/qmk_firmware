// Copyright 2026 manu028-727
// SPDX-License-Identifier: GPL-2.0-or-later
/* The layout store: layout_store.h has the format and the rules.
 *
 * Reading the external flash costs a driver call per byte (docs/display.md),
 * so a reload at run time goes LOAD_CHUNK bytes a main-loop pass: the meta of
 * a big layout (4 KB) takes about a second, a typical one (a few hundred
 * bytes) a few dozen passes. The boot runs the same steps back to back,
 * before the dashboard's first paint. Nothing touches the chip while it is
 * programming or erasing, or while the animation owns the bus. */
#include <string.h>
#include "graphics/layout_store.h"
#include "graphics/display_layout.h"
#include "graphics/lcd_bus.h"

#define LOAD_CHUNK 16u

dl_custom_t dl_custom[DL_CUSTOM_MAX];
uint8_t     dl_custom_n;

typedef enum { LS_IDLE = 0, LS_HEADER, LS_CRC, LS_PICK, LS_RECORDS, LS_COMMIT } ls_state_t;

static struct {
    ls_state_t state;
    bool       boot;          /* the synchronous boot load: no repaint at commit */
    uint8_t    slot;          /* 0 A, 1 B: the one being read */
    bool       ok[2];         /* its header and CRC passed */
    uint32_t   seq[2];
    uint16_t   meta[2];
    uint32_t   total[2];
    uint8_t    n[2];          /* its records */
    uint32_t   want_crc;
    uint32_t   crc;
    uint16_t   pos;           /* LS_CRC: the next byte; LS_RECORDS: the next record */
    uint8_t    tried;         /* LS_PICK: slots already tried (bit per slot) */
    uint8_t    err;           /* the error of the slot being parsed */
    display_layout_t pend;
    dl_custom_t      pend_c[DL_CUSTOM_MAX];
    uint8_t          pend_n;
    uint8_t          seen;    /* built-in kinds already given (bit per kind) */
} ls;

static uint8_t  active      = 0xFFu;   /* 0 A, 1 B, 0xFF the default */
static uint32_t active_seq  = 0;
static uint8_t  last_err    = DLE_NONE;

static inline uint32_t slot_base(uint8_t s) { return s ? LAYOUT_SLOT_B : LAYOUT_SLOT_A; }
static inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static bool rects_meet(uint8_t ax, uint8_t ay, uint8_t aw, uint8_t ah, uint8_t bx, uint8_t by, uint8_t bw, uint8_t bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

/* Every rule a parsed layout must meet before it is drawn. */
static bool place_ok(void) {
    if (!display_layout_valid(&ls.pend)) return false;
    for (uint8_t i = 0; i < ls.pend_n; i++) {
        const dl_custom_t *c = &ls.pend_c[i];
        if (!c->w || !c->h || c->x + c->w > 128u || c->y + c->h > 128u) return false;
        for (uint8_t b = 0; b < 6; b++) {
            uint8_t bx, by, bw, bh;
            if (display_layout_rect(&ls.pend, b, &bx, &by, &bw, &bh) && rects_meet(c->x, c->y, c->w, c->h, bx, by, bw, bh)) return false;
        }
        for (uint8_t j = 0; j < i; j++) {
            const dl_custom_t *d = &ls.pend_c[j];
            if (rects_meet(c->x, c->y, c->w, c->h, d->x, d->y, d->w, d->h)) return false;
        }
    }
    return true;
}

/* One record into the pending layout. False: the slot is refused. */
static bool take_record(const uint8_t *r, uint8_t s) {
    uint8_t  kind = r[0], flags = r[1], x = r[2], y = r[3], w = r[4], h = r[5], p0 = r[6], p1 = r[7];
    uint32_t off  = le32(&r[8]);
    if (kind >= DLK_OS && kind <= DLK_STATUS) {
        uint8_t bit = (uint8_t)(1u << kind);
        if (ls.seen & bit) return false;   /* each built-in once */
        ls.seen |= bit;
        bool hide = (flags & 1u) != 0;
        display_layout_t *l = &ls.pend;
        switch (kind) {
            case DLK_OS:     l->os_x = x; l->os_y = y; if (hide) l->hidden |= DL_HIDE_OS; break;
            case DLK_CONN:   l->conn_x = x; l->conn_y = y; if (hide) l->hidden |= DL_HIDE_CONN; break;
            case DLK_TEXT:   if (hide || x) return false; l->text_y = y; break;
            case DLK_CLOCK:  if (hide || x) return false; l->clock_y = y; break;
            case DLK_LOCK:   if (x) return false; l->lock_y = y; if (hide) l->hidden |= DL_HIDE_LOCK; break;
            case DLK_STATUS: if (x) return false; l->status_y = y; if (hide) l->hidden |= DL_HIDE_STATUS; break;
        }
        return true;
    }
    if (kind != DLK_IMAGE && kind != DLK_LABEL) return false;
    if (ls.pend_n >= DL_CUSTOM_MAX) return false;
    dl_custom_t *c = &ls.pend_c[ls.pend_n];
    memset(c, 0, sizeof *c);
    c->kind = kind; c->x = x; c->y = y;
    uint32_t len;
    if (kind == DLK_IMAGE) {
        c->w = w; c->h = h;
        len  = (uint32_t)w * h * 2u;
    } else {
        uint16_t font = p0 ? ASSET_IOSEVKA_MEDIUM_20 : ASSET_IOSEVKA_MEDIUM_13;
        uint16_t adv = lcd_font_advance(font), fh = lcd_font_height(font);
        if (!adv || !fh || !p1) return false;   /* no such face provisioned, or empty */
        c->font = p0 ? 1 : 0;
        c->len  = p1;
        if ((uint32_t)adv * p1 > 128u) return false;
        c->w = (uint8_t)(adv * p1);
        c->h = (uint8_t)fh;
        len  = p1;
    }
    if (!len || off > ls.total[s] || len > ls.total[s] - off) return false;
    c->addr = slot_base(s) + off;
    ls.pend_n++;
    return true;
}

static void start(bool boot) {
    memset(&ls, 0, sizeof ls);
    ls.boot  = boot;
    ls.slot  = 0;
    ls.state = LS_HEADER;
}

/* One bounded step. */
static void step(void) {
    switch (ls.state) {
        case LS_IDLE:
            return;

        case LS_HEADER: {
            uint8_t s = ls.slot, h[32];
            flash_read_bytes(slot_base(s), h, sizeof h);
            uint16_t meta  = le16(&h[6]);
            uint32_t total = le32(&h[12]);
            ls.ok[s] = false;
            if (le32(h) == LAYOUT_MAGIC && h[4] == LAYOUT_VERSION && h[5] <= LAYOUT_RECORDS_MAX &&
                meta >= 32u + 16u * h[5] && meta <= LAYOUT_META_MAX && total >= meta && total <= LAYOUT_SLOT_SIZE) {
                ls.n[s]     = h[5];
                ls.meta[s]  = meta;
                ls.seq[s]   = le32(&h[8]);
                ls.total[s] = total;
                ls.want_crc = le32(&h[16]);
                ls.crc      = flash_crc32_acc(0xFFFFFFFFu, slot_base(s), 16u);
                ls.pos      = 20;
                ls.state    = LS_CRC;
                return;
            }
            if (le32(h) == LAYOUT_MAGIC) ls.err = DLE_HEADER;   /* a layout, malformed; no magic is an empty slot */
            ls.state = LS_PICK;   /* this slot has nothing: the next one, or the pick */
            if (s == 0) { ls.slot = 1; ls.state = LS_HEADER; }
            return;
        }

        case LS_CRC: {
            uint8_t  s = ls.slot;
            uint16_t k = (uint16_t)(ls.meta[s] - ls.pos);
            if (k > LOAD_CHUNK) k = LOAD_CHUNK;
            if (k) { ls.crc = flash_crc32_acc(ls.crc, slot_base(s) + ls.pos, k); ls.pos = (uint16_t)(ls.pos + k); }
            if (ls.pos < ls.meta[s]) return;
            ls.ok[s] = (~ls.crc == ls.want_crc);
            if (!ls.ok[s]) ls.err = DLE_CRC;
            if (s == 0) { ls.slot = 1; ls.state = LS_HEADER; }
            else ls.state = LS_PICK;
            return;
        }

        case LS_PICK: {
            /* The best untried valid slot: the higher seq (wrapping). */
            int8_t best = -1;
            for (uint8_t s = 0; s < 2; s++) {
                if (!ls.ok[s] || (ls.tried & (1u << s))) continue;
                if (best < 0 || (int32_t)(ls.seq[s] - ls.seq[(uint8_t)best]) > 0) best = (int8_t)s;
            }
            if (best < 0) {
                /* Nothing to take: the default, and why. */
                ls.pend = (display_layout_t)DISPLAY_LAYOUT_DEFAULT;
                ls.pend_n = 0;
                if (!ls.err) ls.err = DLE_NONE;   /* else the last refusal's reason */
                ls.slot  = 0xFFu;
                ls.state = LS_COMMIT;
                return;
            }
            ls.slot   = (uint8_t)best;
            ls.tried |= (uint8_t)(1u << best);
            ls.pend   = (display_layout_t)DISPLAY_LAYOUT_DEFAULT;
            ls.pend_n = 0;
            ls.seen   = 0;
            ls.pos    = 0;
            ls.state  = LS_RECORDS;
            return;
        }

        case LS_RECORDS: {
            uint8_t s = ls.slot;
            if (ls.pos < ls.n[s]) {
                uint8_t r[16];
                flash_read_bytes(slot_base(s) + 32u + 16u * ls.pos, r, sizeof r);
                ls.pos++;
                if (!take_record(r, s)) { ls.err = DLE_RECORD; ls.state = LS_PICK; }
                return;
            }
            if (!place_ok()) { ls.err = DLE_PLACE; ls.state = LS_PICK; return; }
            ls.err   = DLE_OK;
            ls.state = LS_COMMIT;
            return;
        }

        case LS_COMMIT:
            display_layout = ls.pend;
            memcpy(dl_custom, ls.pend_c, sizeof dl_custom);
            dl_custom_n = ls.pend_n;
            active      = ls.slot;
            active_seq  = active == 0xFFu ? 0 : ls.seq[active];
            last_err    = ls.err;
            ls.state    = LS_IDLE;
            if (!ls.boot) display_layout_changed();
            return;
    }
}

void layout_store_boot(void) {
    start(true);
    for (uint16_t guard = 0; ls.state != LS_IDLE && guard < 2000u; guard++) step();
    if (ls.state != LS_IDLE) {   /* can't happen: the steps are bounded */
        ls.state       = LS_IDLE;
        display_layout = (display_layout_t)DISPLAY_LAYOUT_DEFAULT;
        dl_custom_n    = 0;
    }
}

bool layout_store_reload(void) {
    if (ls.state != LS_IDLE) return false;
    start(false);
    return true;
}

void layout_store_task(void) {
    if (ls.state == LS_IDLE) return;
    if (anim_active() || flash_busy()) return;   /* the bus is the animation's, or the chip is writing */
    step();
}

void layout_store_info(uint8_t *out) {
    out[0]  = LAYOUT_VERSION;
    out[1]  = ls.state != LS_IDLE;
    out[2]  = active;
    out[3]  = (uint8_t)active_seq;
    out[4]  = (uint8_t)(active_seq >> 8);
    out[5]  = (uint8_t)(active_seq >> 16);
    out[6]  = (uint8_t)(active_seq >> 24);
    out[7]  = last_err;
    out[8]  = dl_custom_n;
    out[9]  = (uint8_t)(LAYOUT_SLOT_A >> 16);
    out[10] = (uint8_t)(LAYOUT_SLOT_B >> 16);
    out[11] = DL_CUSTOM_MAX;
}
