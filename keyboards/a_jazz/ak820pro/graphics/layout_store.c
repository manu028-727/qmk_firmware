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

dl_custom_t   dl_custom[DL_CUSTOM_MAX];
uint8_t       dl_custom_n;
flash_asset_t dl_font[DL_FONT_MAX];
uint8_t       dl_font_n;
dl_icon_t     dl_icon[DLI_MAX];

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
    flash_asset_t    pend_f[DL_FONT_MAX];
    uint8_t          pend_fn;
    dl_icon_t        pend_i[DLI_MAX];
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

/* A face index from a record (DL_NO_FONT: none) that covers [lo, hi]:
 * its cell into *cw and *ch. */
static bool font_ok(uint8_t f, uint8_t lo, uint8_t hi, uint8_t *cw, uint8_t *ch) {
    if (f == DL_NO_FONT) return true;
    if (f >= ls.pend_fn) return false;
    const flash_asset_t *a = &ls.pend_f[f];
    if (a->first > lo || (uint16_t)a->first + a->count <= hi) return false;
    *cw = a->cell_w; *ch = a->cell_h;
    return true;
}

/* The sizes the records asked for, checked and put into the pending layout:
 * the faces cover what each element draws, and a set of icons is whole. */
static bool sizes_ok(void) {
    display_layout_t *l = &ls.pend;
    uint8_t cw = 0, ch = 0;
    if (!font_ok(l->clock_f, '0', ':', &cw, &ch)) return false;
    if (l->clock_f != DL_NO_FONT) {
        if (ch < 14u || ch > 60u) return false;   /* the date's small face fits the band */
        l->clock_ch = ch;
    }
    if (!font_ok(l->text_f, 0x20, 0x7E, &cw, &ch)) return false;
    if (l->text_f != DL_NO_FONT) {
        if (ch < 12u || ch > 40u) return false;   /* the transport icon's 12 rows beside line 0 */
        l->text_lh = ch;
    }
    /* The OS logo: none of its icons, or all three, square and alike. */
    const dl_icon_t *ic = ls.pend_i;
    if (ic[DLI_APPLE].w || ic[DLI_WINDOWS].w || ic[DLI_LINUX].w) {
        for (uint8_t i = DLI_APPLE; i <= DLI_LINUX; i++)
            if (ic[i].w != ic[DLI_APPLE].w || ic[i].h != ic[DLI_APPLE].w) return false;
        l->os_s = ic[DLI_APPLE].w;
    }
    /* The strip: none, or all three alike, with a digit face no taller. */
    if (ic[DLI_CONN_CABLE].w || ic[DLI_CONN_BT].w || ic[DLI_CONN_24G].w) {
        for (uint8_t i = DLI_CONN_CABLE; i <= DLI_CONN_24G; i++)
            if (ic[i].w != ic[DLI_CONN_CABLE].w || ic[i].h != ic[DLI_CONN_CABLE].h) return false;
        if (l->conn_f == DL_NO_FONT) return false;
        l->conn_w = ic[DLI_CONN_CABLE].w;
        l->conn_h = ic[DLI_CONN_CABLE].h;
    }
    if (!font_ok(l->conn_f, '1', '3', &cw, &ch)) return false;
    if (l->conn_f != DL_NO_FONT) {
        if (ch > DL_CONN_H(l)) return false;
        l->conn_nw = cw;
        if ((uint16_t)DL_CONN_W(l) + cw + 2u > 128u) return false;   /* the strip and its digit on the panel */
    }
    return true;
}

/* Every rule a parsed layout must meet before it is drawn. */
static bool place_ok(void) {
    if (!sizes_ok()) return false;
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

static inline uint8_t band_w(uint8_t x, uint8_t w) { return w ? w : (x < 128u ? (uint8_t)(128u - x) : 0u); }

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
        uint8_t f = p0 ? (uint8_t)(p0 - 1u) : DL_NO_FONT;   /* the element's face, if it names one */
        if (p0 && (kind == DLK_OS || kind == DLK_LOCK || kind == DLK_STATUS)) return false;
        if (kind == DLK_CONN) l->conn_f = f;
        if (kind == DLK_TEXT) l->text_f = f;
        if (kind == DLK_CLOCK) l->clock_f = f;
        switch (kind) {
            case DLK_OS:     l->os_x = x; l->os_y = y; if (hide) l->hidden |= DL_HIDE_OS; break;
            case DLK_CONN:   l->conn_x = x; l->conn_y = y; if (hide) l->hidden |= DL_HIDE_CONN; break;
            /* The bands: x and a width, 0 meaning to the panel's right edge. */
            case DLK_TEXT:   l->text_x = x; l->text_y = y; l->text_w = band_w(x, w); if (hide) l->hidden |= DL_HIDE_TEXT; break;
            case DLK_CLOCK:  l->clock_x = x; l->clock_y = y; l->clock_w = band_w(x, w); if (hide) l->hidden |= DL_HIDE_CLOCK; break;
            case DLK_LOCK:   l->lock_x = x; l->lock_y = y; l->lock_w = band_w(x, w); if (hide) l->hidden |= DL_HIDE_LOCK; break;
            case DLK_STATUS: l->status_x = x; l->status_y = y; l->status_w = band_w(x, w); if (hide) l->hidden |= DL_HIDE_STATUS; break;
        }
        return true;
    }
    if (kind == DLK_FONT) {
        /* p1 glyphs from p0, each w x h. */
        if (ls.pend_fn >= DL_FONT_MAX || !w || !h || w > 128u || h > 128u || !p1 || p0 < 0x20u || (uint16_t)p0 + p1 > 0x7Fu)
            return false;
        uint32_t flen = (uint32_t)w * h * 2u * p1;
        if (off > ls.total[s] || flen > ls.total[s] - off) return false;
        flash_asset_t *a = &ls.pend_f[ls.pend_fn];
        memset(a, 0, sizeof *a);
        a->id     = DL_FONT_ID(ls.pend_fn);
        a->off    = slot_base(s) + off - FLASH_ASSET_BASE;
        a->fmt    = 1;
        a->w      = w; a->h = h;
        a->cell_w = w; a->cell_h = h;
        a->first  = p0; a->count = p1;
        ls.pend_fn++;
        return true;
    }
    if (kind == DLK_ICON) {
        if (p0 >= DLI_MAX || ls.pend_i[p0].w || !w || !h || w > 128u || h > 128u) return false;
        uint32_t ilen = (uint32_t)w * h * 2u;
        if (off > ls.total[s] || ilen > ls.total[s] - off) return false;
        ls.pend_i[p0].addr = slot_base(s) + off;
        ls.pend_i[p0].w    = w;
        ls.pend_i[p0].h    = h;
        return true;
    }
    if (kind != DLK_IMAGE && kind != DLK_LABEL && kind != DLK_ANIM) return false;
    if (ls.pend_n >= DL_CUSTOM_MAX) return false;
    dl_custom_t *c = &ls.pend_c[ls.pend_n];
    memset(c, 0, sizeof *c);
    c->kind = kind; c->x = x; c->y = y;
    uint32_t len;
    if (kind == DLK_IMAGE) {
        c->w = w; c->h = h;
        len  = (uint32_t)w * h * 2u;
    } else if (kind == DLK_ANIM) {
        if (!p0) return false;
        c->w = w; c->h = h; c->len = p0;
        len  = (((uint32_t)p0 + 1u) & ~1u) + (uint32_t)p0 * w * h * 2u;
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
                ls.pend_fn = 0;
                memset(ls.pend_i, 0, sizeof ls.pend_i);
                if (!ls.err) ls.err = DLE_NONE;   /* else the last refusal's reason */
                ls.slot  = 0xFFu;
                ls.state = LS_COMMIT;
                return;
            }
            ls.slot   = (uint8_t)best;
            ls.tried |= (uint8_t)(1u << best);
            ls.pend   = (display_layout_t)DISPLAY_LAYOUT_DEFAULT;
            ls.pend_n = 0;
            ls.pend_fn = 0;
            memset(ls.pend_i, 0, sizeof ls.pend_i);
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
            memcpy(dl_font, ls.pend_f, sizeof dl_font);
            dl_font_n   = ls.pend_fn;
            memcpy(dl_icon, ls.pend_i, sizeof dl_icon);
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
        dl_font_n      = 0;
        memset(dl_icon, 0, sizeof dl_icon);
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
