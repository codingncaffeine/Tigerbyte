/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Tigerbyte - Game.com sound generation.
 *
 * The SM8521 has two 32-step 4-bit wavetable channels (SG0/SG1), an 8-bit
 * direct DAC (SGDA, used by most games for streamed music/speech), and a noise
 * channel (SG2, unimplemented anywhere). The DAC is reconstructed by resampling
 * each write at its true CPU-cycle timestamp (the game modulates the DAC rate
 * per sample), with linear interpolation to band-limit.
 */
#ifndef TIGERBYTE_SOUND_H
#define TIGERBYTE_SOUND_H

#include <stdint.h>

typedef struct {
   float    phase[2];   /* wavetable phase accumulators (SG0, SG1) */
   int      idx[2];     /* current wavetable step (0..31)          */
   uint32_t lfsr;       /* SG2 noise LFSR state                    */
   int      noise_out;  /* SG2 noise 1-bit toggle output           */
   float    noise_phase;/* SG2 noise step accumulator              */
} gc_sound_t;

/* Generate `n` interleaved-stereo int16 samples from the register page `ram`.
 * `dac_stream`/`dac_cycle`/`dac_n` are the DAC values the CPU wrote during the frame and
 * the cycle each was written at; `cyc_per_frame` is the frame's total cycle span. The DAC
 * is resampled by timestamp (linear interpolation) onto the `n` output samples. */
void gc_sound_generate(gc_sound_t *s, const uint8_t *ram,
                       const uint8_t *dac_stream, const uint32_t *dac_cycle, int dac_n,
                       int cyc_per_frame, int16_t *out, int n, int rate);

#endif /* TIGERBYTE_SOUND_H */
