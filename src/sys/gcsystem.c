/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Tigerbyte - Game.com system: CPU + bus + frame timing. See gcsystem.h. */
#include "gcsystem.h"

/* ~4.9152 MHz / 60. Per-instruction cycle counts are still approximate, so the
 * timer rate is approximate too; exact pacing is calibrated in a later pass. */
#define GC_CYCLES_PER_FRAME 81920

static void on_irq(void *user, int line)
{
   sm8521_set_irq((sm8521_t *)user, line, 1);
}

void gcsystem_init(gcsystem_t *s)
{
   gcbus_init(&s->bus);
   sm8521_init(&s->cpu, gcbus_read, gcbus_write, &s->bus);
   gcbus_set_irq_handler(&s->bus, on_irq, &s->cpu);
}

void gcsystem_reset(gcsystem_t *s) { sm8521_reset(&s->cpu); }

int gcsystem_load_internal(gcsystem_t *s, const char *p) { return gcbus_load_internal(&s->bus, p); }
int gcsystem_load_external(gcsystem_t *s, const char *p) { return gcbus_load_external(&s->bus, p); }
int gcsystem_load_cart(gcsystem_t *s, const char *p)     { return gcbus_load_cart(&s->bus, p); }
void gcsystem_load_cart_mem(gcsystem_t *s, const uint8_t *d, size_t n) { gcbus_load_cart_mem(&s->bus, d, n); }

void gcsystem_run_frame(gcsystem_t *s)
{
   sm8521_t *c = &s->cpu;
   gcbus_t  *b = &s->bus;

   b->ram[0x32] &= (uint8_t)~0x80;          /* LCV: active display (not vblank) */

   int budget = GC_CYCLES_PER_FRAME;
   while (budget > 0 && !c->trapped) {
      int cyc = sm8521_step(c);
      gcbus_tick(b, cyc);
      budget -= cyc;
      /* The emulated clock oscillator is always stable, so model the
         clock-ready (CK) signal that wakes a STOP/HALT idle. */
      if (c->stopped || c->halted) sm8521_set_irq(c, SM_CK, 1);
   }

   /* vblank: set LCV status, raise LCDC interrupt when the kernel enabled it */
   b->ram[0x32] |= 0x80;
   if ((b->ram[0x10] & 0x01) && (b->ram[0x1f] & 0x01) && ((b->ram[0x1e] & 7) < 5))
      sm8521_set_irq(c, SM_LCDC, 1);
}
