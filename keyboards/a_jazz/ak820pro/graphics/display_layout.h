// Copyright 2026 manu028-727
// SPDX-License-Identifier: GPL-2.0-or-later
/* The dashboard's layout as data: phase 1 of the screen widget editor
 * (Manuel's SCREEN-EDITOR-SCOPE.md, M1).
 *
 * Each dashboard element is anchored at one origin, and everything that
 * element draws is placed relative to it. Before this, the origins were
 * #defines in display.c; they are now fields of display_layout, which starts
 * at DISPLAY_LAYOUT_DEFAULT -- exactly the old values -- so this phase changes
 * nothing on the panel. A later phase loads a layout from the external flash
 * and validates it with display_layout_valid() before it is used.
 *
 * What moves with each origin:
 *   os      the OS logo (24x24 asset)
 *   conn    the transport strip and the slot digit after it
 *   text    the host text band: song lines, transport icon, connection words
 *   clock   the clock band (clock, date, play time, AM/PM)
 *   lock    the lock row (padlock, CAPS, WIN, FN/SCR)
 *   status  the battery row (icon, bolt/bulb slot, percentage, warnings)
 * The bands stay full-width in this phase: only their rows are data, and the
 * x of the two top-left elements. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t os_x, os_y;
    uint8_t conn_x, conn_y;
    uint8_t text_y;
    uint8_t clock_y;
    uint8_t lock_y;
    uint8_t status_y;
} display_layout_t;

/* Today's dashboard, as display.c's #defines had it. */
#define DL_DEF_OS_X     0
#define DL_DEF_OS_Y     0
#define DL_DEF_CONN_X   29   /* 5px clear of the OS logo: it shows a different kind of thing */
#define DL_DEF_CONN_Y   0
#define DL_DEF_TEXT_Y   27
#define DL_DEF_CLOCK_Y  58
#define DL_DEF_LOCK_Y   83
#define DL_DEF_STATUS_Y 105

#define DISPLAY_LAYOUT_DEFAULT \
    { DL_DEF_OS_X, DL_DEF_OS_Y, DL_DEF_CONN_X, DL_DEF_CONN_Y, \
      DL_DEF_TEXT_Y, DL_DEF_CLOCK_Y, DL_DEF_LOCK_Y, DL_DEF_STATUS_Y }

/* DISPLAY_LAYOUT_CONST makes the layout a compile-time constant: the proof
 * build, whose display.o must match the pre-layout one instruction for
 * instruction (the compiler folds every field back into the old immediate). */
#ifdef DISPLAY_LAYOUT_CONST
static const display_layout_t display_layout = DISPLAY_LAYOUT_DEFAULT;
#else
extern display_layout_t display_layout;
#endif

/* The band invariants display.c used to assert at compile time, for a layout
 * that arrives at run time. True: safe to draw. */
bool display_layout_valid(const display_layout_t *l);
