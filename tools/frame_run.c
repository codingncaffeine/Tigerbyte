/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Tigerbyte - frame-loop boot test.
 *
 * Runs the full system (CPU + bus + timers + vblank interrupts) for N frames,
 * then renders the LCD as ASCII art. This is the first harness driven by real
 * peripheral interrupts rather than a simulated wake.
 *
 *   frame_run <internal.bin> <external.bin> <frames> [cart]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "sys/gcsystem.h"
#include "sys/ppu.h"

static gcsystem_t sys;
static uint8_t shades[GC_W * GC_H];

int main(int argc, char **argv)
{
   if (argc < 4) { fprintf(stderr, "usage: %s <internal> <external> <frames> [cart]\n", argv[0]); return 1; }
   gcsystem_init(&sys);
   if (gcsystem_load_internal(&sys, argv[1]) || gcsystem_load_external(&sys, argv[2])) {
      fprintf(stderr, "error: bad ROM image\n"); return 1;
   }
   if (argc > 4) gcsystem_load_cart(&sys, argv[4]);
   gcsystem_reset(&sys);

   int frames = atoi(argv[3]);
   for (int f = 0; f < frames && !sys.cpu.trapped; f++)
      gcsystem_run_frame(&sys);

   printf("ran %d frames  PC=%04X  LCDC=%02X  trapped=%d(op %02X)\n",
          frames, sys.cpu.pc, sys.bus.ram[0x30], sys.cpu.trapped, sys.cpu.trap_op);
   printf("IRQs vectored=%u  IE0=%02X IE1=%02X PS0=%02X PS1=%02X  TM0C=%02X TM1C=%02X\n",
          sys.cpu.irq_taken, sys.bus.ram[0x10], sys.bus.ram[0x11], sys.bus.ram[0x1e],
          sys.bus.ram[0x1f], sys.bus.ram[0x50], sys.bus.ram[0x52]);
   printf("timer overflows=%u  passed IRQ gate=%u\n\n",
          sys.bus.dbg_ovf, sys.bus.dbg_ovf_raised);

   gc_render_shades(&sys.bus, shades);
   static const char map[5] = { '@', '#', '+', '.', ' ' };
   for (int y = 0; y < GC_H; y += 4) {
      for (int x = 0; x < GC_W; x += 2)
         putchar(map[shades[y * GC_W + x]]);
      putchar('\n');
   }
   return 0;
}
