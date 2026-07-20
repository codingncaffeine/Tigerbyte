/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Tigerbyte - Game.com system: CPU + bus + frame timing.
 *
 * Owns the SM8521 and the memory bus, wires bus interrupt requests (timer
 * overflow, DMA completion) into the CPU, and runs the machine a frame at a
 * time, ticking the timers and raising the per-frame LCD (vblank) interrupt.
 */
#ifndef TIGERBYTE_GCSYSTEM_H
#define TIGERBYTE_GCSYSTEM_H

#include "cpu/sm8521.h"
#include "gcbus.h"
#include "sound.h"

typedef struct {
   gcbus_t    bus;
   sm8521_t   cpu;
   gc_sound_t snd;
   int16_t    audio[2048 * 2];   /* one frame of interleaved-stereo output */
   int        audio_samples;
   int        clock_hz;          /* system clock fCK (default 4,915,200) */
   int        cycles_per_frame;  /* clock_hz / 59.732155 */
} gcsystem_t;

void gcsystem_init(gcsystem_t *s);
/* Override the system clock (calibration/experiments; call after init). */
void gcsystem_set_clock(gcsystem_t *s, int hz);
void gcsystem_reset(gcsystem_t *s);
int  gcsystem_load_internal(gcsystem_t *s, const char *path);
int  gcsystem_load_external(gcsystem_t *s, const char *path);
int  gcsystem_load_cart(gcsystem_t *s, const char *path);
void gcsystem_load_cart_mem(gcsystem_t *s, const uint8_t *data, size_t size);

/* Run one display frame: CPU + timers, then the vblank interrupt. */
void gcsystem_run_frame(gcsystem_t *s);

#endif /* TIGERBYTE_GCSYSTEM_H */
