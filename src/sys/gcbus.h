/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Tigerbyte - Game.com system memory bus.
 *
 * Decodes the SM8521 16-bit address space into the console's regions and
 * handles MMU bank paging for the four 8 KB windows (0x2000-0x9FFF):
 *   MMU page < 0x20  -> external kernel ROM
 *   MMU page >= 0x20 -> cartridge ROM
 * VRAM (0xA000-0xDFFF) is write-only to the CPU (reads return 0); the PPU
 * reads it directly. The low register-file / I/O page and NVRAM are RAM.
 */
#ifndef TIGERBYTE_GCBUS_H
#define TIGERBYTE_GCBUS_H

#include <stdint.h>
#include <stddef.h>

#define GC_IROM_SIZE  0x1000      /* 4 KB internal boot ROM            */
#define GC_KROM_SIZE  0x40000     /* 256 KB external kernel ROM        */
#define GC_CART_SIZE  0x200000    /* 2 MB cartridge window (mirrored)  */
#define GC_VRAM_BASE  0xA000
#define GC_VRAM_SIZE  0x4000      /* 16 KB VRAM                        */

/* Interrupt line numbers (match the SM8521 vector order in sm8521.h). */
enum { GC_IRQ_DMA = 1, GC_IRQ_TIM0 = 2, GC_IRQ_UART = 4, GC_IRQ_LCDC = 5, GC_IRQ_TIM1 = 6, GC_IRQ_CK = 7 };

typedef void (*gc_irq_fn)(void *user, int line);

typedef struct {
   int     enabled;
   uint8_t reload;          /* upcounter target (set when TMxD is written) */
   int     prescale_max;
   int     prescale_count;
} gc_timer_t;

typedef struct {
   uint8_t    ram[0x10000];              /* RAM/IO page, NVRAM, and VRAM backing */
   uint8_t    irom[GC_IROM_SIZE];
   uint8_t    krom[GC_KROM_SIZE];
   uint8_t    cart[GC_CART_SIZE];
   int        cart_loaded;
   gc_timer_t timer[2];                  /* TM0, TM1 */
   gc_irq_fn  irq;                       /* raised on timer overflow / DMA done */
   void      *irq_user;
   uint8_t    in0, in1, in2;             /* button port states (active-low, 0xFF=none) */
   uint16_t   grid[13];                  /* touch-screen zone columns (10 rows each) */
   uint32_t   dbg_ovf;                   /* total timer overflows */
   uint32_t   dbg_ovf_raised;            /* overflows that passed the IRQ gate */
   uint32_t   snd_dac_writes;            /* writes to the DAC reg (0x4E) — sound debug */
   uint32_t   snd_reg_writes;            /* writes to any sound ctrl reg (0x40-0x4F) — sound debug */
   uint32_t   snd_wave_writes;           /* writes to wavetable RAM (0x60-0x7F) — voice-streaming debug */
   uint8_t    dac_stream[2048];          /* DAC (0x4E) values captured in write order this frame */
   uint32_t   dac_cycle[2048];           /* cycle-within-frame each DAC write happened at (~0..82287) */
   int        dac_stream_n;              /* count captured this frame (reset each frame) */
   int        cur_cycle;                 /* running cycle position within the frame (set by gcsystem) */
   uint32_t   ck_accum;                  /* cycle accumulator for the 1 Hz clock-timer (CK) tick */
   uint32_t   dbg_ck;                    /* clock-timer interrupts raised */
   int        dma_cycles_left;           /* blitter busy time remaining (completion IRQ pends) */
   int        uart_cycles_left;          /* pending transmit-done interrupt (nothing attached) */
   uint32_t   clock_hz;                  /* CPU clock, for the 1 s clock-timer period */
   double     tclk_frac;                 /* CPU-cycle -> fixed-timer-clock remainder */
} gcbus_t;

void gcbus_init(gcbus_t *b);

/* Load ROM images; return 0 on success, nonzero on error (bad size / IO). */
int gcbus_load_internal(gcbus_t *b, const char *path);
int gcbus_load_external(gcbus_t *b, const char *path);
int gcbus_load_cart(gcbus_t *b, const char *path);
void gcbus_load_cart_mem(gcbus_t *b, const uint8_t *data, size_t size);

void gcbus_set_irq_handler(gcbus_t *b, gc_irq_fn fn, void *user);

/* Input state (called by the host each frame).
 * in0 (active-low): b0 up,b1 down,b2 left,b3 right,b4 menu,b5 pause,b6 sound,b7 A.
 * in1 (active-low): b0 B, b1 C.   in2 (active-low): b0 power, b1 D.
 * Touch: set grid[column] bits for pressed rows (0 = released). */
void gcbus_set_buttons(gcbus_t *b, uint8_t in0, uint8_t in1, uint8_t in2);
void gcbus_set_touch(gcbus_t *b, int column, uint16_t rows);

/* Advance the timers + clock-timer by `cycles` and raise interrupts via the
 * handler. `stopped` = CPU is in STOP mode: the main clock (and so TM0/TM1) is
 * halted, but the sub-clock-driven clock timer keeps running. */
void gcbus_tick(gcbus_t *b, int cycles, int stopped);

/* CPU bus callbacks (cast ctx to gcbus_t*). */
uint8_t gcbus_read(void *ctx, uint16_t addr);
void    gcbus_write(void *ctx, uint16_t addr, uint8_t val);

#endif /* TIGERBYTE_GCBUS_H */
