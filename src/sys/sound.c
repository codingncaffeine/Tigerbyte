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
   int en0 = sgc & 0x01, en1 = sgc & 0x02, endac = sgc & 0x08;

   int sg0t = ((ram[0x46] << 8) | ram[0x47]) & 0x0FFF;
   int sg1t = ((ram[0x48] << 8) | ram[0x49]) & 0x0FFF;
   float f0 = (master && en0 && sg0t) ? (2764800.0f / sg0t) / rate : 0.0f;
   float f1 = (master && en1 && sg1t) ? (2764800.0f / sg1t) / rate : 0.0f;
   int   l0 = ram[0x42] & 0x1F, l1 = ram[0x44] & 0x1F;       /* 5-bit levels */
   int   use_dac = master && endac;

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

      if (mix > 32767)  mix = 32767;
      if (mix < -32768) mix = -32768;
      out[i * 2] = out[i * 2 + 1] = (int16_t)mix;
   }
}
