/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Tigerbyte - Game.com sound generation. See sound.h. */
#include "sound.h"

/* one 4-bit step of a wavetable channel (32 nibbles packed in 16 bytes) */
static int wave_step(const uint8_t *tbl, int idx)
{
   uint8_t byte = tbl[idx >> 1];
   int nib = (idx & 1) ? (byte >> 4) : (byte & 0x0F);
   return nib - 8;                  /* centre around 0 (-8..+7) */
}

void gc_sound_generate(gc_sound_t *s, const uint8_t *ram,
                       const uint8_t *dac_stream, int dac_n,
                       int16_t *out, int n, int rate)
{
   uint8_t sgc = ram[0x40];
   int master = sgc & 0x80;
   int en0 = sgc & 0x01, en1 = sgc & 0x02;

   int sg0t = ((ram[0x46] << 8) | ram[0x47]) & 0x0FFF;
   int sg1t = ((ram[0x48] << 8) | ram[0x49]) & 0x0FFF;
   float f0 = (master && en0 && sg0t) ? (2457600.0f / sg0t) / rate : 0.0f;
   float f1 = (master && en1 && sg1t) ? (2457600.0f / sg1t) / rate : 0.0f;
   int   l0 = ram[0x42] & 0x1F, l1 = ram[0x44] & 0x1F;       /* 5-bit levels */
   /* DAC fires only when it's the sole active channel (matches hardware/MAME):
      master + DAC enabled, and SG0/SG1/SG2 all off. Prevents stale DAC bleeding
      over wavetable music. */
   int   use_dac = (sgc & 0x8F) == 0x88;

   /* SG2 noise channel — Furnace's reconstruction (32-bit LFSR, taps 0/5/8/13,
      output toggles on bit-0 edges). Unimplemented in MAME and elsewhere; this is
      the missing noise SFX (e.g. the Centipede shot). Taps are a best-guess pending
      a hardware capture. */
   int   en2  = sgc & 0x04;
   int   l2   = ram[0x4A] & 0x1F;
   int   sg2t = ((ram[0x4C] << 8) | ram[0x4D]) & 0x0FFF;
   float fn   = (master && en2 && sg2t) ? (2457600.0f / sg2t) / rate : 0.0f;
   if (s->lfsr == 0) s->lfsr = 0x89abcdefu;                 /* seed; 0 is a dead LFSR */

   for (int i = 0; i < n; i++) {
      int dval = (dac_n > 0) ? dac_stream[(int)((long)i * dac_n / n)] : ram[0x4E];
      int dac  = use_dac ? (dval - 128) : 0;
      int mix  = dac * 64;                                   /* 8-bit DAC, scaled for level */

      if (f0 > 0.0f) {
         mix += wave_step(&ram[0x60], s->idx[0]) * l0 * 16;
         s->phase[0] += f0;
         while (s->phase[0] >= 1.0f) { s->phase[0] -= 1.0f; s->idx[0] = (s->idx[0] + 1) & 31; }
      }
      if (f1 > 0.0f) {
         mix += wave_step(&ram[0x70], s->idx[1]) * l1 * 16;
         s->phase[1] += f1;
         while (s->phase[1] >= 1.0f) { s->phase[1] -= 1.0f; s->idx[1] = (s->idx[1] + 1) & 31; }
      }
      if (fn > 0.0f) {
         s->noise_phase += fn;
         while (s->noise_phase >= 1.0f) {
            s->noise_phase -= 1.0f;
            int oldbit = (int)(s->lfsr & 1u);
            s->lfsr = (s->lfsr >> 1) |
                      (((s->lfsr ^ (s->lfsr >> 5) ^ (s->lfsr >> 8) ^ (s->lfsr >> 13)) & 1u) << 31);
            if (oldbit ^ (int)(s->lfsr & 1u)) s->noise_out ^= 1;
         }
         mix += ((s->noise_out ? 7 : -8) * l2) * 16;
      }

      if (mix > 32767)  mix = 32767;
      if (mix < -32768) mix = -32768;
      out[i * 2] = out[i * 2 + 1] = (int16_t)mix;
   }
}
