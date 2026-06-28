/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Tigerbyte - Game.com LCD render.
 *
 * The 200x160 display is stored column-major in VRAM: each display column X is
 * a 40-byte strip (160 pixels, 2bpp, MSB-first), strips spaced 40 bytes apart.
 * LCDC selects the VRAM page, the grayscale palette mode, and display enable.
 */
#ifndef TIGERBYTE_PPU_H
#define TIGERBYTE_PPU_H

#include <stdint.h>
#include "gcbus.h"

#define GC_W 200
#define GC_H 160

extern const uint32_t gc_palette5[5];   /* shade 0 (dark) .. 4 (light) -> XRGB8888 */

/* Fill `shades` (GC_W*GC_H, row-major) with shade indices 0..4. */
void gc_render_shades(const gcbus_t *b, uint8_t *shades);

/* Fill `fb` (GC_W*GC_H, row-major) with XRGB8888 pixels. */
void gc_render(const gcbus_t *b, uint32_t *fb);

#endif /* TIGERBYTE_PPU_H */
