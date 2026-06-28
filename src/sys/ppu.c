/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Tigerbyte - Game.com LCD render. See ppu.h. */
#include "ppu.h"

/* Authentic Game.com LCD palette (MAME gamecom_palette). */
const uint32_t gc_palette5[5] = {
   0x00000000u,  /* 0 black  */
   0x000F4F2Fu,  /* 1 gray 1 */
   0x006F8F4Fu,  /* 2 gray 2 */
   0x008FCF8Fu,  /* 3 gray 3 */
   0x00DFFF8Fu   /* 4 white  */
};

/* LCDC[5:4] grayscale mode -> map 2-bit pixel value to a shade index. */
static const uint8_t MODE[4][4] = {
   { 4, 3, 2, 0 },   /* 0x00 */
   { 4, 3, 1, 0 },   /* 0x10 */
   { 4, 3, 1, 0 },   /* 0x20 */
   { 4, 2, 1, 0 }    /* 0x30 */
};

void gc_render_shades(const gcbus_t *b, uint8_t *shades)
{
   uint8_t        lcdc = b->ram[0x30];
   const uint8_t *vram = b->ram + GC_VRAM_BASE + ((lcdc & 0x40) ? 0x2000 : 0);
   int            enabled = lcdc & 0x80;
   const uint8_t *pal = MODE[(lcdc >> 4) & 3];

   for (int x = 0; x < GC_W; x++) {             /* x = display column = scanline */
      const uint8_t *strip = vram + 40 * x;
      for (int y = 0; y < GC_H; y++) {          /* y = display row */
         uint8_t shade = 0;
         if (enabled) {
            uint8_t byte = strip[y >> 2];
            uint8_t pix  = (byte >> ((3 - (y & 3)) * 2)) & 3;
            shade = pal[pix];
         }
         shades[y * GC_W + x] = shade;
      }
   }
}

void gc_render(const gcbus_t *b, uint32_t *fb)
{
   static uint8_t sh[GC_W * GC_H];
   gc_render_shades(b, sh);
   for (int i = 0; i < GC_W * GC_H; i++)
      fb[i] = gc_palette5[sh[i]];
}
