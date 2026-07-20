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
#include <string.h>
#include <stdint.h>

#include "sys/gcsystem.h"
#include "sys/ppu.h"
#include "cpu/sm8521_disasm.h"

static uint8_t trc_rd(void *ctx, uint16_t a) { return gcbus_read(ctx, a); }

/* single-step until the PC lands in a cart-mapped window, then trace N instructions */
static void trace_cart(gcsystem_t *s, int n)
{
   long guard = 3000000;
   while (guard-- > 0) {
      uint16_t pc = s->cpu.pc;
      int win = (pc >= 0x2000 && pc < 0xA000) ? (pc - 0x2000) >> 13 : -1;
      int in_cart = win >= 0 && s->bus.ram[0x25 + win] >= 0x20;
      if (in_cart) break;
      gcbus_tick(&s->bus, sm8521_step(&s->cpu), s->cpu.stopped);
   }
   fprintf(stderr, "[cart-trace] entered cart code at PC=%04X (MMU=%02X.%02X.%02X.%02X)\n",
           s->cpu.pc, s->bus.ram[0x25], s->bus.ram[0x26], s->bus.ram[0x27], s->bus.ram[0x28]);
   for (int i = 0; i < n; i++) {
      char dis[64];
      uint16_t pc = s->cpu.pc;
      sm8521_disasm(dis, sizeof dis, pc, trc_rd, &s->bus);
      int cyc = sm8521_step(&s->cpu);
      gcbus_tick(&s->bus, cyc, s->cpu.stopped);
      fprintf(stderr, "[cart-trace] %04X %-26s", pc, dis);
      if ((i % 4) == 3) {
         uint8_t b = (uint8_t)(s->cpu.ps0 & 0xF8);
         fprintf(stderr, " | r0-7: %02X %02X %02X %02X %02X %02X %02X %02X PS1=%02X",
                 s->cpu.reg[b + 0], s->cpu.reg[b + 1], s->cpu.reg[b + 2], s->cpu.reg[b + 3],
                 s->cpu.reg[b + 4], s->cpu.reg[b + 5], s->cpu.reg[b + 6], s->cpu.reg[b + 7], s->cpu.ps1);
      }
      fprintf(stderr, "\n");
   }
}

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
   /* input injection for experiments: TB_IN0=hex button held; TB_TAP="col,row"
      touch held; both applied after 1/3 of the run. TB_TAPS="c,r@f;c,r@f;..."
      scripts multiple taps (each held 8 frames from its @frame). */
   const char *be = getenv("TB_IN0"), *te = getenv("TB_TAP"), *ts = getenv("TB_TAPS");
   const char *b1 = getenv("TB_IN1"), *b2 = getenv("TB_IN2");
   uint8_t hold  = be ? (uint8_t)strtoul(be, NULL, 16) : 0xFF;
   uint8_t hold1 = b1 ? (uint8_t)strtoul(b1, NULL, 16) : 0xFF;
   uint8_t hold2 = b2 ? (uint8_t)strtoul(b2, NULL, 16) : 0xFF;
   int tcol = -1, trow = 0;
   if (te) sscanf(te, "%d,%d", &tcol, &trow);
   int scol[16], srow[16], sfr[16], ntap = 0;
   if (ts) {
      const char *p = ts;
      while (ntap < 16) {
         int c2, r2, f2;
         if (sscanf(p, "%d,%d@%d", &c2, &r2, &f2) != 3) break;
         scol[ntap] = c2; srow[ntap] = r2; sfr[ntap] = f2; ntap++;
         p = strchr(p, ';');
         if (!p) break;
         p++;
      }
   }
   /* a proper tap: press for ~8 frames, then release (a touch UI acts on release).
      TB_AT overrides the hold window's start frame (default: 1/3 of the run).
      TB_PRESS="f1,f2,..." holds button A for 8 frames at each listed frame. */
   int t0 = getenv("TB_AT") ? atoi(getenv("TB_AT")) : frames / 3;
   int t1 = t0 + (getenv("TB_LEN") ? atoi(getenv("TB_LEN")) : 8);
   const char *press = getenv("TB_PRESS");
   int audio_peak = 0;
   uint32_t p_irq = 0, p_ovf = 0, p_dac = 0;
   for (int f = 0; f < frames && !sys.cpu.trapped; f++) {
      int act = (f >= t0 && f < t1);
      uint8_t in0 = act ? hold : 0xFF;
      if (press) {
         const char *p = press;
         while (*p) {
            int pf = atoi(p);
            if (f >= pf && f < pf + 8) in0 &= 0x7F;   /* A held (active-low bit7) */
            p = strchr(p, ',');
            if (!p) break;
            p++;
         }
      }
      gcbus_set_buttons(&sys.bus, in0, act ? hold1 : 0xFF, act ? hold2 : 0xFF);
      for (int c = 0; c < 13; c++) gcbus_set_touch(&sys.bus, c, 0);
      if (act && tcol >= 0) gcbus_set_touch(&sys.bus, tcol, (uint16_t)(1 << trow));
      for (int k = 0; k < ntap; k++)
         if (f >= sfr[k] && f < sfr[k] + 8)
            gcbus_set_touch(&sys.bus, scol[k], (uint16_t)(1 << srow[k]));
      gcsystem_run_frame(&sys);
      if (getenv("TB_CARTTRACE") && f == 1000)
         trace_cart(&sys, 150);
      for (int k = 0; k < sys.audio_samples; k++) {
         int v = sys.audio[k * 2]; if (v < 0) v = -v; if (v > audio_peak) audio_peak = v;
      }
      if (getenv("TB_TRACE") && (f % 60) == 59) {   /* once per emulated second */
         fprintf(stderr,
            "[%3ds] PC=%04X %s CLKT=%02X IE=%02X.%02X iflags=%03X TM1C=%02X TM1D(tc)=%02X "
            "irq/s=%u ovf/s=%u ck=%u dac/s=%u LCDC=%02X\n",
            (f + 1) / 60, sys.cpu.pc,
            sys.cpu.stopped ? "STOP" : (sys.cpu.halted ? "HALT" : "run "),
            sys.bus.ram[0x1a], sys.bus.ram[0x10], sys.bus.ram[0x11], sys.cpu.iflags,
            sys.bus.ram[0x52], sys.bus.timer[1].reload,
            sys.cpu.irq_taken - p_irq, sys.bus.dbg_ovf - p_ovf, sys.bus.dbg_ck,
            sys.bus.snd_dac_writes - p_dac, sys.bus.ram[0x30]);
         p_irq = sys.cpu.irq_taken; p_ovf = sys.bus.dbg_ovf; p_dac = sys.bus.snd_dac_writes;
      }
   }
   {
      int cart_mapped = sys.bus.ram[0x25] >= 0x20 || sys.bus.ram[0x26] >= 0x20 ||
                        sys.bus.ram[0x27] >= 0x20 || sys.bus.ram[0x28] >= 0x20;
      int win = (sys.cpu.pc >= 0x2000 && sys.cpu.pc < 0xA000) ? (sys.cpu.pc - 0x2000) / 0x2000 : -1;
      int cart_pc = win >= 0 && sys.bus.ram[0x25 + win] >= 0x20;
      printf("[cart: pc-in-cart=%s mapped=%s  MMU=%02X.%02X.%02X.%02X  audiopeak=%d]\n",
             cart_pc ? "YES" : "no", cart_mapped ? "YES" : "no",
             sys.bus.ram[0x25], sys.bus.ram[0x26], sys.bus.ram[0x27], sys.bus.ram[0x28], audio_peak);
   }
   {
      int win = (sys.cpu.pc >= 0x2000 && sys.cpu.pc < 0xA000) ? (sys.cpu.pc - 0x2000) / 0x2000 : -1;
      int cart_code = win >= 0 && sys.bus.ram[0x25 + win] >= 0x20;
      printf("[cart code executing: %s  MMU=%02X.%02X.%02X.%02X]\n", cart_code ? "YES" : "no",
             sys.bus.ram[0x25], sys.bus.ram[0x26], sys.bus.ram[0x27], sys.bus.ram[0x28]);
   }

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
