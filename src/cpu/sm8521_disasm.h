/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Tigerbyte - Sharp SM8521 (SM85CPU) instruction decoder / disassembler.
 *
 * This is the decode foundation the executing CPU core is built on: it knows
 * every opcode's length and operand layout, so both the disassembler and the
 * (later) executor agree on how to parse an instruction stream.
 *
 * Opcode encodings follow the Sharp SM8521 datasheet (instruction summary + 23
 * addressing modes, pp.51-54), adapted from MAME's BSD-3-Clause SM8500
 * disassembler (© Wilbert Pol). Licensed GPL-3.0-or-later; see LICENSE.
 */
#ifndef TIGERBYTE_SM8521_DISASM_H
#define TIGERBYTE_SM8521_DISASM_H

#include <stdint.h>
#include <stddef.h>

/* Byte-fetch callback: return the byte at absolute 16-bit address. */
typedef uint8_t (*sm8521_fetch_fn)(void *ctx, uint16_t addr);

/*
 * Decode one instruction beginning at `pc`.
 *   out/outsz : buffer that receives a human-readable disassembly (may be NULL).
 *   fetch/ctx : how to read program bytes.
 * Returns the instruction length in bytes (always >= 1).
 */
int sm8521_disasm(char *out, size_t outsz, uint16_t pc,
                  sm8521_fetch_fn fetch, void *ctx);

#endif /* TIGERBYTE_SM8521_DISASM_H */
