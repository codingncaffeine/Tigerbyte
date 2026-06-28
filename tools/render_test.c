/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Tigerbyte - LCD render test.
 *
 * Boots the BIOS, runs N instructions (simulating a peripheral wake), then
 * renders the LCD and prints it as downsampled ASCII art so the frame is
 * visible without a GUI.
 *
 *   render_test <internal.bin> <external.bin> <count> [cart]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "cpu/sm8521.h"
#include "sys/gcbus.h"
#include "sys/ppu.h"

static gcbus_t bus;
static uint8_t shades[GC_W * GC_H];

int main(int argc, char **argv)
{
   if (argc < 4) { fprintf(stderr, "usage: %s <internal> <external> <count> [cart]\n", argv[0]); return 1; }
   gcbus_init(&bus);
   if (gcbus_load_internal(&bus, argv[1]) || gcbus_load_external(&bus, argv[2])) {
      fprintf(stderr, "error: bad ROM image\n"); return 1;
   }
   if (argc > 4) gcbus_load_cart(&bus, argv[4]);
   long count = atol(argv[3]);

   sm8521_t c;
   sm8521_init(&c, gcbus_read, gcbus_write, &bus);
   sm8521_reset(&c);
   for (long i = 0; i < count && !c.trapped; i++) {
      sm8521_step(&c);
      if (c.stopped || c.halted) sm8521_set_irq(&c, SM_CK, 1);
   }
   printf("ran %ld instrs  LCDC=%02X  trapped=%d\n\n", count, bus.ram[0x30], c.trapped);

   gc_render_shades(&bus, shades);
   static const char map[5] = { '@', '#', '+', '.', ' ' };  /* 0 dark .. 4 light */
   for (int y = 0; y < GC_H; y += 4) {            /* downsample to 50 rows */
      for (int x = 0; x < GC_W; x += 2)            /* ...and 100 cols */
         putchar(map[shades[y * GC_W + x]]);
      putchar('\n');
   }
   return 0;
}
