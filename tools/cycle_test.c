/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Tigerbyte - SDK-anchored cycle-count regression test.
 *
 * The official SDK's delay20ms routine is hardware-calibrated ground truth:
 *
 *     movw rr0,#01800h          ; 6 cycles
 *     loop: decw rr0            ; 8 cycles
 *           br nz,loop          ; 8 cycles taken
 *
 * (8+8) * 0x1800 iterations = 98,304 cycles = exactly 20 ms at 4.9152 MHz.
 * This test executes that loop on the core and asserts the measured total, so
 * any cycle-model regression on MOVW/DECW/BR breaks the build loudly.
 *
 *   cycle_test            (no arguments; exit 0 = pass)
 */
#include <stdio.h>
#include <stdint.h>

#include "cpu/sm8521.h"

static uint8_t mem[0x10000];
static uint8_t rd(void *ctx, uint16_t a) { (void)ctx; return mem[a]; }
static void    wr(void *ctx, uint16_t a, uint8_t v) { (void)ctx; mem[a] = v; }

int main(void)
{
   static const uint8_t prog[] = {
      0x78, 0x18, 0x00,   /* movw rr0,#0x1800 */
      0x19, 0x00,         /* decw RR0         */
      0xDE, 0xFC,         /* br   nz,-4       */
      0xF0                /* stop             */
   };
   for (unsigned i = 0; i < sizeof prog; i++) mem[0x1020 + i] = prog[i];

   sm8521_t c;
   sm8521_init(&c, rd, wr, NULL);
   sm8521_reset(&c);

   long total = 0;
   long steps = 0;
   while (!c.stopped && !c.trapped && steps < 200000) {
      total += sm8521_step(&c);
      steps++;
   }

   /* movw(6) + 0x1800*decw(8) + 0x17FF*br-taken(8) + br-not-taken(4) + stop(2) */
   const long expect = 6 + 0x1800L * 8 + 0x17FFL * 8 + 4 + 2;
   const long nominal20ms = 98304;   /* the SDK's (8+8)*0x1800 loop body */

   printf("delay20ms: %ld cycles over %ld instructions (expected %ld; loop nominal %ld, drift %+0.3f%%)\n",
          total, steps, expect, nominal20ms,
          100.0 * ((double)total - nominal20ms) / nominal20ms);

   if (c.trapped)      { printf("FAIL: trapped on op %02X\n", c.trap_op); return 1; }
   if (!c.stopped)     { printf("FAIL: loop never terminated\n"); return 1; }
   if (total != expect){ printf("FAIL: cycle total mismatch\n"); return 1; }
   printf("PASS\n");
   return 0;
}
