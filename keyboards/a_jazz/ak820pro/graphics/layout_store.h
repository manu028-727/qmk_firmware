// Copyright 2026 manu028-727
// SPDX-License-Identifier: GPL-2.0-or-later
/* A dashboard layout from the external flash (the screen widget editor, M2
 * and M3). Two 1 MB slots, A at 0xE00000 and B at 0xF00000; the valid one
 * with the higher sequence number wins, and with neither the built-in
 * default (today's dashboard) stands. The host writes the slot that is NOT
 * active, verifies its CRC, then asks for a reload, so a failed upload never
 * leaves the board without a layout.
 *
 * The package (little-endian):
 *   header, 32 B
 *     0  u32 magic 'AKL1'        4 u8 version (1)     5 u8 records
 *     6  u16 meta_len (header + records + label text, <= 4096)
 *     8  u32 seq                 12 u32 total_len (the whole package)
 *     16 u32 meta_crc: CRC-32 (zlib's) of bytes [0,16) then [20,meta_len)
 *     20..31 zero
 *   records from 32, 16 B each
 *     0 u8 kind   1 u8 flags   2 u8 x   3 u8 y   4 u8 w   5 u8 h
 *     6 u8 p0     7 u8 p1      8 u32 off (from the slot's start)   12 u32 zero
 *   kinds: 1 os  2 conn  3 text  4 clock  5 lock  6 status   (built-in: x/y
 *          move it, flags bit 0 hides it; text and clock never hide; the
 *          four bands take a width too, w, 0 meaning to the right edge)
 *          16 image: w x h RGB565 pixels, big-endian, row by row, at off
 *          17 label: p1 characters at off, p0 the face (0 small 6x14, 1 20px)
 * A built-in a layout leaves out keeps its default place.
 *
 * Every rule is checked before anything is used (display_layout_valid(), the
 * custom widgets inside the panel, off each other and off every visible
 * built-in's rows); a layout that fails any of them is not taken. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LAYOUT_SLOT_A     0xE00000u
#define LAYOUT_SLOT_B     0xF00000u
#define LAYOUT_SLOT_SIZE  0x100000u
#define LAYOUT_MAGIC      0x314C4B41u   /* 'AKL1' */
#define LAYOUT_VERSION    1u
#define LAYOUT_META_MAX   4096u
#define LAYOUT_RECORDS_MAX 16u

enum { DLK_OS = 1, DLK_CONN, DLK_TEXT, DLK_CLOCK, DLK_LOCK, DLK_STATUS, DLK_IMAGE = 16, DLK_LABEL = 17 };

/* A custom widget, as drawn: addr is absolute in the external flash. */
typedef struct {
    uint8_t  kind, x, y, w, h, font, len, pad;
    uint32_t addr;
} dl_custom_t;
#define DL_CUSTOM_MAX 8u

extern dl_custom_t dl_custom[DL_CUSTOM_MAX];
extern uint8_t     dl_custom_n;

/* Why the last load took no layout (or 0: it took one). */
enum { DLE_OK = 0, DLE_NONE, DLE_HEADER, DLE_CRC, DLE_RECORD, DLE_PLACE };

/* At boot, before the dashboard's first paint: load synchronously. */
void layout_store_boot(void);
/* Start a reload in the background (false: one is running). */
bool layout_store_reload(void);
/* One bounded step of a running reload; every main-loop pass. */
void layout_store_task(void);
/* The FC_LAYOUT_INFO reply, from out[0]: version, loading, active slot
 * (0 A, 1 B, 0xFF the default), seq (4 B), last error, custom count, slot A
 * and B (the address's top byte), the custom maximum. 12 bytes. */
void layout_store_info(uint8_t *out);
