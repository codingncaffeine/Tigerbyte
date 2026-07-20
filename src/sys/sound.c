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
                       const uint8_t *dac_stream, const uint32_t *dac_cycle, int dac_n,
                       int cyc_per_frame, int16_t *out, int n, int rate)
{
   uint8_t sgc = ram[0x40];
   int master = sgc & 0x80;
   int en0 = sgc & 0x01, en1 = sgc & 0x02;

   int sg0t = ((ram[0x46] << 8) | ram[0x47]) & 0x0FFF;
   int sg1t = ((ram[0x48] << 8) | ram[0x49]) & 0x0FFF;
   /* Datasheet p.42: step period = (n-1)/fCK. The sound generator runs from the
      crystal-locked 4.9152 MHz, NOT the software-variable CPU clock — dialing
      the CPU-speed option must not retune the music. (MAME's 2,764,800 derives
      from its wrong crystal; 2,457,600 put the wavetable an octave too low.) */
   const float fck = 4915200.0f;
   float f0 = (master && en0 && sg0t > 1) ? (fck / (sg0t - 1)) / rate : 0.0f;
   float f1 = (master && en1 && sg1t > 1) ? (fck / (sg1t - 1)) / rate : 0.0f;
   int   l0 = ram[0x42] & 0x1F, l1 = ram[0x44] & 0x1F;       /* 5-bit levels */
   /* DAC fires only when it's the sole active channel (master + DAC, SG0/SG1/SG2 off) —
      matches the hardware. BUT SGC is sampled once per frame, so a frame the game ended
      with sound momentarily disabled was silencing DAC samples it had streamed all frame
      (heard as a "dip"). If the game wrote DAC samples this frame it was actively
      streaming, so honor them. */
   int   use_dac = (dac_n > 0) ? 1 : ((sgc & 0x8F) == 0x88);

   /* SG2 noise channel — Furnace's reconstruction (32-bit LFSR, taps 0/5/8/13,
      output toggles on bit-0 edges). Unimplemented in MAME and elsewhere; this is
      the missing noise SFX (e.g. the Centipede shot). Taps are a best-guess pending
      a hardware capture. */
   int   en2  = sgc & 0x04;
   int   l2   = ram[0x4A] & 0x1F;
   int   sg2t = ((ram[0x4C] << 8) | ram[0x4D]) & 0x0FFF;
   float fn   = (master && en2 && sg2t > 1) ? (fck / (sg2t - 1)) / rate : 0.0f;
   if (s->lfsr == 0) s->lfsr = 0x89abcdefu;                 /* seed; 0 is a dead LFSR */

   /* DAC resampling cursor: walk the timestamped writes as the output advances.
      The level carries across frame seams — restarting from this frame's first
      write put a step discontinuity at every 60 Hz boundary. */
   int dw = 0;
   int dac_prev_v = s->dac_primed ? s->dac_last : ((dac_n > 0) ? dac_stream[0] : ram[0x4E]);
   int dac_prev_c = 0;

   for (int i = 0; i < n; i++) {
      /* Resample the DAC at this output sample's true cycle position, interpolating
         between the surrounding writes (the game varies the inter-sample spacing). */
      int dval;
      if (dac_n > 0) {
         long tc = (long)i * cyc_per_frame / n;
         while (dw < dac_n && (long)dac_cycle[dw] <= tc) {
            dac_prev_v = dac_stream[dw]; dac_prev_c = (int)dac_cycle[dw]; dw++;
         }
         if (dw < dac_n) {
            int nv = dac_stream[dw], nc = (int)dac_cycle[dw];
            dval = (nc > dac_prev_c)
                 ? dac_prev_v + (int)((long)(nv - dac_prev_v) * (tc - dac_prev_c) / (nc - dac_prev_c))
                 : dac_prev_v;
         } else dval = dac_prev_v;
      } else dval = ram[0x4E];
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

   /* remember the closing DAC level for the next frame's seam */
   s->dac_last   = (dac_n > 0) ? dac_stream[dac_n - 1] : dac_prev_v;
   s->dac_primed = 1;
}
