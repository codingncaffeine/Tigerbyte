/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Tigerbyte - Sharp SM8521 (SM85CPU) executing core.
 *
 * Instruction semantics follow the Sharp SM8521 datasheet's instruction summary
 * + 23 addressing modes; opcode encodings, flag rules and the banked-register
 * model were adapted from MAME's BSD-3-Clause SM8500 core (© Wilbert Pol,
 * Robbbert). Licensed GPL-3.0-or-later; see LICENSE.
 *
 * The CPU sees a 16-bit address space through caller-supplied read/write
 * callbacks. The low 16 register-file bytes (r0..r15) are a bank-relative
 * window into an internal register RAM selected by PS0; everything else goes to
 * the bus. Cycle counts are approximate for now (no authoritative table exists).
 */
#ifndef TIGERBYTE_SM8521_H
#define TIGERBYTE_SM8521_H

#include <stdint.h>

typedef uint8_t (*sm8521_read_fn)(void *ctx, uint16_t addr);
typedef void    (*sm8521_write_fn)(void *ctx, uint16_t addr, uint8_t val);

/* interrupt line numbers (match SM8500 vector order / priority) */
enum {
   SM_ILL = 0, SM_DMA, SM_TIM0, SM_EXT, SM_UART,
   SM_LCDC, SM_TIM1, SM_CK, SM_PIO, SM_WDT, SM_NMI
};

typedef struct {
   uint16_t pc;
   uint16_t sp;
   uint8_t  ps0, ps1, sys;          /* re-synced from memory each step */
   uint8_t  reg[0x108];             /* banked register file */
   uint16_t iflags;                 /* pending interrupt lines (bitmask) */
   uint8_t  check_irq;
   uint8_t  halted, stopped;

   /* diagnostics */
   int      trapped;                /* set when an unimplemented opcode is hit */
   uint8_t  trap_op;
   uint16_t trap_pc;
   uint32_t irq_taken;             /* count of interrupts actually vectored */
   uint8_t  am;                    /* addressing-mode index (0-4) from arg_rmb/arg_rmw */
   int      bus_n;                 /* bus accesses this instruction (derived cycle timing) */

   sm8521_read_fn  rd;
   sm8521_write_fn wr;
   void *ctx;
} sm8521_t;

void sm8521_init (sm8521_t *c, sm8521_read_fn rd, sm8521_write_fn wr, void *ctx);
void sm8521_reset(sm8521_t *c);
int  sm8521_step (sm8521_t *c);                 /* execute one instruction; returns cycles */
void sm8521_set_irq(sm8521_t *c, int line, int asserted);

#endif /* TIGERBYTE_SM8521_H */
