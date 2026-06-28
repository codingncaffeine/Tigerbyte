/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Tigerbyte - Game.com sound generation.
 *
 * The SM8521 has two 32-step 4-bit wavetable channels (SG0/SG1), an 8-bit
 * direct DAC (SGDA, used by most games for streamed music/speech), and a noise
 * channel (SG2, unimplemented anywhere). This is a first-pass generator from
 * the sound register state; the DAC is sampled once per frame (sub-frame
 * streaming + the TIM1 coupling are accuracy work for later).
 */
#ifndef TIGERBYTE_SOUND_H
#define TIGERBYTE_SOUND_H

#include <stdint.h>

typedef struct {
   float phase[2];      /* wavetable phase accumulators (SG0, SG1) */
   int   idx[2];        /* current wavetable step (0..31)          */
} gc_sound_t;

/* Generate `n` interleaved-stereo int16 samples from the register page `ram`.
 * `dac_stream`/`dac_n` are the DAC values the CPU wrote during the frame (in order),
 * stretched across the output to reconstruct the streamed DAC audio. */
void gc_sound_generate(gc_sound_t *s, const uint8_t *ram,
                       const uint8_t *dac_stream, int dac_n,
                       int16_t *out, int n, int rate);

#endif /* TIGERBYTE_SOUND_H */
