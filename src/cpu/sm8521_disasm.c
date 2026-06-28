/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Tigerbyte - Sharp SM8521 (SM85CPU) instruction decoder / disassembler.
 * See sm8521_disasm.h for licensing/source notes.
 *
 * The opcode table maps each of the 256 opcode bytes to a mnemonic and an
 * addressing-mode class; the decode switch then knows exactly how many operand
 * bytes follow each mode, which is what gives every instruction its length.
 */
#include "sm8521_disasm.h"
#include <stdio.h>
#include <string.h>

/* Addressing-mode classes (one per distinct operand encoding). */
enum {
   AM_NONE = 0,
   AM_R, AM_rr, AM_S, AM_1A, AM_1B, AM_riB, AM_rmb, AM_mbr, AM_bid, AM_i,
   AM_Ri, AM_rmw, AM_mwr, AM_smw, AM_mws, AM_ss, AM_2, AM_RR, AM_ii, AM_SS,
   AM_Sw, AM_iR, AM_iS, AM_bR, AM_4F, AM_5A, AM_5B, AM_RiR, AM_Rii, AM_rib,
   AM_riw, AM_rbr, AM_Rbr, AM_cjp, AM_Rb, AM_rR, AM_Rr, AM_pi, AM_cbr, AM_CALS
};

typedef struct { const char *m; unsigned char am; } op_t;

/* 16 condition codes used by BR/JMP cc. */
static const char *const COND[16] = {
   "F","LT","LE","ULE","OV","MI","Z","C","T","GE","GT","UGT","NOV","PL","NZ","NC"
};

/* byte-register index -> word-register-pair index */
static const unsigned char B2W[8] = { 0, 8, 2, 10, 4, 12, 6, 14 };

static const op_t OPS[256] = {
   /* 00 */ {"clr",AM_R},{"neg",AM_R},{"com",AM_R},{"rr",AM_R},
   /* 04 */ {"rl",AM_R},{"rrc",AM_R},{"rlc",AM_R},{"srl",AM_R},
   /* 08 */ {"inc",AM_R},{"dec",AM_R},{"sra",AM_R},{"sll",AM_R},
   /* 0C */ {"da",AM_R},{"swap",AM_R},{"push",AM_R},{"pop",AM_R},
   /* 10 */ {"cmp",AM_rr},{"add",AM_rr},{"sub",AM_rr},{"adc",AM_rr},
   /* 14 */ {"sbc",AM_rr},{"and",AM_rr},{"or",AM_rr},{"xor",AM_rr},
   /* 18 */ {"incw",AM_S},{"decw",AM_S},{"",AM_1A},{"",AM_1B},
   /* 1C */ {"bclr",AM_riB},{"bset",AM_riB},{"pushw",AM_S},{"popw",AM_S},
   /* 20 */ {"cmp",AM_rmb},{"add",AM_rmb},{"sub",AM_rmb},{"adc",AM_rmb},
   /* 24 */ {"sbc",AM_rmb},{"and",AM_rmb},{"or",AM_rmb},{"xor",AM_rmb},
   /* 28 */ {"mov",AM_rmb},{"mov",AM_mbr},{"bbc",AM_bid},{"bbs",AM_bid},
   /* 2C */ {"exts",AM_R},{"dm",AM_i},{"mov PS0,",AM_i},{"btst",AM_Ri},
   /* 30 */ {"cmp",AM_rmw},{"add",AM_rmw},{"sub",AM_rmw},{"adc",AM_rmw},
   /* 34 */ {"sbc",AM_rmw},{"and",AM_rmw},{"or",AM_rmw},{"xor",AM_rmw},
   /* 38 */ {"mov",AM_rmw},{"mov",AM_mwr},{"movw",AM_smw},{"movw",AM_mws},
   /* 3C */ {"movw",AM_ss},{"dm",AM_R},{"jmp",AM_2},{"call",AM_2},
   /* 40 */ {"cmp",AM_RR},{"add",AM_RR},{"sub",AM_RR},{"adc",AM_RR},
   /* 44 */ {"sbc",AM_RR},{"and",AM_RR},{"or",AM_RR},{"xor",AM_RR},
   /* 48 */ {"mov",AM_RR},{"call",AM_ii},{"movw",AM_SS},{"movw",AM_Sw},
   /* 4C */ {"mult",AM_RR},{"mult",AM_iR},{"bmov",AM_bR},{"",AM_4F},
   /* 50 */ {"cmp",AM_iR},{"add",AM_iR},{"sub",AM_iR},{"adc",AM_iR},
   /* 54 */ {"sbc",AM_iR},{"and",AM_iR},{"or",AM_iR},{"xor",AM_iR},
   /* 58 */ {"mov",AM_iR},{"invalid",AM_NONE},{"cmp",AM_5A},{"mov",AM_5B},
   /* 5C */ {"div",AM_SS},{"div",AM_iS},{"movm",AM_RiR},{"movm",AM_Rii},
   /* 60 */ {"cmpw",AM_SS},{"addw",AM_SS},{"subw",AM_SS},{"adcw",AM_SS},
   /* 64 */ {"sbcw",AM_SS},{"andw",AM_SS},{"orw",AM_SS},{"xorw",AM_SS},
   /* 68 */ {"cmpw",AM_Sw},{"addw",AM_Sw},{"subw",AM_Sw},{"adcw",AM_Sw},
   /* 6C */ {"sbcw",AM_Sw},{"andw",AM_Sw},{"orw",AM_Sw},{"xorw",AM_Sw},
   /* 70 */ {"dbnz",AM_rbr},{"dbnz",AM_rbr},{"dbnz",AM_rbr},{"dbnz",AM_rbr},
   /* 74 */ {"dbnz",AM_rbr},{"dbnz",AM_rbr},{"dbnz",AM_rbr},{"dbnz",AM_rbr},
   /* 78 */ {"movw",AM_riw},{"movw",AM_riw},{"movw",AM_riw},{"movw",AM_riw},
   /* 7C */ {"movw",AM_riw},{"movw",AM_riw},{"movw",AM_riw},{"movw",AM_riw},
   /* 80 */ {"bbc",AM_Rbr},{"bbc",AM_Rbr},{"bbc",AM_Rbr},{"bbc",AM_Rbr},
   /* 84 */ {"bbc",AM_Rbr},{"bbc",AM_Rbr},{"bbc",AM_Rbr},{"bbc",AM_Rbr},
   /* 88 */ {"bbs",AM_Rbr},{"bbs",AM_Rbr},{"bbs",AM_Rbr},{"bbs",AM_Rbr},
   /* 8C */ {"bbs",AM_Rbr},{"bbs",AM_Rbr},{"bbs",AM_Rbr},{"bbs",AM_Rbr},
   /* 90 */ {"jmp",AM_cjp},{"jmp",AM_cjp},{"jmp",AM_cjp},{"jmp",AM_cjp},
   /* 94 */ {"jmp",AM_cjp},{"jmp",AM_cjp},{"jmp",AM_cjp},{"jmp",AM_cjp},
   /* 98 */ {"jmp",AM_cjp},{"jmp",AM_cjp},{"jmp",AM_cjp},{"jmp",AM_cjp},
   /* 9C */ {"jmp",AM_cjp},{"jmp",AM_cjp},{"jmp",AM_cjp},{"jmp",AM_cjp},
   /* A0 */ {"bclr",AM_Rb},{"bclr",AM_Rb},{"bclr",AM_Rb},{"bclr",AM_Rb},
   /* A4 */ {"bclr",AM_Rb},{"bclr",AM_Rb},{"bclr",AM_Rb},{"bclr",AM_Rb},
   /* A8 */ {"bset",AM_Rb},{"bset",AM_Rb},{"bset",AM_Rb},{"bset",AM_Rb},
   /* AC */ {"bset",AM_Rb},{"bset",AM_Rb},{"bset",AM_Rb},{"bset",AM_Rb},
   /* B0 */ {"mov",AM_rR},{"mov",AM_rR},{"mov",AM_rR},{"mov",AM_rR},
   /* B4 */ {"mov",AM_rR},{"mov",AM_rR},{"mov",AM_rR},{"mov",AM_rR},
   /* B8 */ {"mov",AM_Rr},{"mov",AM_Rr},{"mov",AM_Rr},{"mov",AM_Rr},
   /* BC */ {"mov",AM_Rr},{"mov",AM_Rr},{"mov",AM_Rr},{"mov",AM_Rr},
   /* C0 */ {"mov",AM_rib},{"mov",AM_rib},{"mov",AM_rib},{"mov",AM_rib},
   /* C4 */ {"mov",AM_rib},{"mov",AM_rib},{"mov",AM_rib},{"mov",AM_rib},
   /* C8 */ {"mov",AM_pi},{"mov",AM_pi},{"mov",AM_pi},{"mov",AM_pi},
   /* CC */ {"mov",AM_pi},{"mov",AM_pi},{"mov",AM_pi},{"mov",AM_pi},
   /* D0 */ {"br",AM_cbr},{"br",AM_cbr},{"br",AM_cbr},{"br",AM_cbr},
   /* D4 */ {"br",AM_cbr},{"br",AM_cbr},{"br",AM_cbr},{"br",AM_cbr},
   /* D8 */ {"br",AM_cbr},{"br",AM_cbr},{"br",AM_cbr},{"br",AM_cbr},
   /* DC */ {"br",AM_cbr},{"br",AM_cbr},{"br",AM_cbr},{"br",AM_cbr},
   /* E0 */ {"cals",AM_CALS},{"cals",AM_CALS},{"cals",AM_CALS},{"cals",AM_CALS},
   /* E4 */ {"cals",AM_CALS},{"cals",AM_CALS},{"cals",AM_CALS},{"cals",AM_CALS},
   /* E8 */ {"cals",AM_CALS},{"cals",AM_CALS},{"cals",AM_CALS},{"cals",AM_CALS},
   /* EC */ {"cals",AM_CALS},{"cals",AM_CALS},{"cals",AM_CALS},{"cals",AM_CALS},
   /* F0 */ {"stop",AM_NONE},{"halt",AM_NONE},{"invalid",AM_NONE},{"invalid",AM_NONE},
   /* F4 */ {"invalid",AM_NONE},{"invalid",AM_NONE},{"invalid",AM_NONE},{"invalid",AM_NONE},
   /* F8 */ {"ret",AM_NONE},{"iret",AM_NONE},{"clrc",AM_NONE},{"comc",AM_NONE},
   /* FC */ {"setc",AM_NONE},{"ei",AM_NONE},{"di",AM_NONE},{"nop",AM_NONE}
};

/* one-byte sub-opcode mnemonic tables for the 0x1A / 0x1B group */
static const char *const SUB_1A[8] = {"clr","neg","com","rr","rl","rrc","rlc","srl"};
static const char *const SUB_1B[8] = {"inc","dec","sra","sll","da","swap","push","pop"};
static const char *const SUB_4F[4] = {"bcmp","band","bor","bxor"};

/* tiny appender into the output buffer */
#define EMIT(...) do { if (out && bp < outsz) \
   bp += (size_t)snprintf(out + bp, outsz - bp, __VA_ARGS__); } while (0)

int sm8521_disasm(char *out, size_t outsz, uint16_t pc,
                  sm8521_fetch_fn fetch, void *ctx)
{
   uint16_t pos = pc;
   size_t bp = 0;
   uint8_t op = fetch(ctx, pos++);
   const op_t *e = &OPS[op];
   uint8_t ea, ea2, sub;
   int8_t off;
   uint16_t imm;

   /* mnemonic first (the multi-op groups print their own below) */
   if (e->am != AM_1A && e->am != AM_1B && e->am != AM_4F)
      EMIT("%-6s", e->m);

   switch (e->am) {
   case AM_NONE:
      break;
   case AM_R:   ea = fetch(ctx, pos++); EMIT("R%02X", ea); break;
   case AM_i:   ea = fetch(ctx, pos++); EMIT("$%02X", ea); break;
   case AM_S:   ea = fetch(ctx, pos++); EMIT("RR%02X", ea); break;
   case AM_rr:  ea = fetch(ctx, pos++); EMIT("r%u,r%u", (ea>>3)&7, ea&7); break;
   case AM_ss:  ea = fetch(ctx, pos++); EMIT("rr%u,rr%u", B2W[(ea>>3)&7], B2W[ea&7]); break;
   case AM_rib: ea = fetch(ctx, pos++); EMIT("r%u,$%02X", op&7, ea); break;
   case AM_pi:  ea = fetch(ctx, pos++); EMIT("r%u,$%02X", 0x10+(op&7), ea); break;
   case AM_rR:  ea = fetch(ctx, pos++); EMIT("r%u,R%02X", op&7, ea); break;
   case AM_Rr:  ea = fetch(ctx, pos++); EMIT("R%02X,r%u", ea, op&7); break;
   case AM_Rb:  ea = fetch(ctx, pos++); EMIT("R%02X,#%u", ea, op&7); break;
   case AM_riw: imm = (uint16_t)(fetch(ctx,pos++)<<8); imm |= fetch(ctx,pos++);
                EMIT("rr%u,$%04X", B2W[op&7], imm); break;
   case AM_RR:  ea = fetch(ctx,pos++); ea2 = fetch(ctx,pos++); EMIT("R%02X,R%02X", ea2, ea); break;
   case AM_SS:  ea = fetch(ctx,pos++); ea2 = fetch(ctx,pos++); EMIT("RR%02X,RR%02X", ea2, ea); break;
   case AM_ii:  imm = (uint16_t)(fetch(ctx,pos++)<<8); imm |= fetch(ctx,pos++); EMIT("$%04X", imm); break;
   case AM_Ri:  ea = fetch(ctx,pos++); ea2 = fetch(ctx,pos++); EMIT("R%02X,$%02X", ea, ea2); break;
   case AM_iR:  ea = fetch(ctx,pos++); ea2 = fetch(ctx,pos++); EMIT("R%02X,$%02X", ea2, ea); break;
   case AM_iS:  ea = fetch(ctx,pos++); ea2 = fetch(ctx,pos++); EMIT("RR%02X,$%02X", ea2, ea); break;
   case AM_bR:
      ea = fetch(ctx,pos++); ea2 = fetch(ctx,pos++);
      if ((ea&0xC0)==0x40) EMIT("R%02X,#%u,BF", ea2, ea&7);
      else                 EMIT("BF,R%02X,#%u", ea2, ea&7);
      break;
   case AM_cbr: off = (int8_t)fetch(ctx,pos++); EMIT("%s,$%04X", COND[op&15], (uint16_t)(pos+off)); break;
   case AM_rbr: off = (int8_t)fetch(ctx,pos++);
                EMIT("r%u,$%04X", op&7, (uint16_t)(pos+off)); break;
   case AM_Rbr: ea = fetch(ctx,pos++); off = (int8_t)fetch(ctx,pos++);
                EMIT("R%02X,#%u,$%04X", ea, op&7, (uint16_t)(pos+off)); break;
   case AM_cjp: imm = (uint16_t)(fetch(ctx,pos++)<<8); imm |= fetch(ctx,pos++);
                EMIT("%s,$%04X", COND[op&15], imm); break;
   case AM_CALS:ea = fetch(ctx,pos++); EMIT("$%04X", (uint16_t)(0x1000 | ((op&0x0F)<<8) | ea)); break;
   case AM_Sw:  ea2 = fetch(ctx,pos++); imm = (uint16_t)(fetch(ctx,pos++)<<8); imm |= fetch(ctx,pos++);
                EMIT("RR%02X,$%04X", ea2, imm); break;
   case AM_RiR: ea = fetch(ctx,pos++); ea2 = fetch(ctx,pos++); sub = fetch(ctx,pos++);
                EMIT("R%02X,$%02X,R%02X", ea, ea2, sub); break;
   case AM_Rii: ea = fetch(ctx,pos++); ea2 = fetch(ctx,pos++); sub = fetch(ctx,pos++);
                EMIT("R%02X,$%02X,$%02X", ea, ea2, sub); break;
   case AM_riB: ea = fetch(ctx,pos++); ea2 = fetch(ctx,pos++);
                EMIT("$%02X(r%u),#%u", ea2, (ea>>3)&7, ea&7); break;

   /* register-indirect groups: optional extra operand byte(s) when mode==0x80 */
   case AM_rmb: case AM_mbr:
      ea = fetch(ctx,pos++);
      if ((ea&0xC0)==0x80) ea2 = fetch(ctx,pos++);
      EMIT("r%u,@r%u(m%u)", (ea>>3)&7, ea&7, (ea>>6)&3); break;
   case AM_rmw: case AM_mwr: case AM_smw: case AM_mws:
      ea = fetch(ctx,pos++);
      if ((ea&0xC0)==0x80) { (void)fetch(ctx,pos++); (void)fetch(ctx,pos++); }
      EMIT("r%u,@rr%u(m%u)", (ea>>3)&7, B2W[ea&7], (ea>>6)&3); break;
   case AM_2: /* JMP/CALL indirect */
      ea = fetch(ctx,pos++);
      if ((ea&0xC0)==0x40) { imm = (uint16_t)(fetch(ctx,pos++)<<8); imm |= fetch(ctx,pos++);
                             EMIT("@$%04X", imm); }
      else                 EMIT("@rr%u", B2W[ea&7]);
      break;
   case AM_bid: /* BBC/BBS direct-page bit, relative */
      ea = fetch(ctx,pos++); ea2 = fetch(ctx,pos++); off = (int8_t)fetch(ctx,pos++);
      EMIT("$%04X,#%u,$%04X", (uint16_t)(0xFF00+ea2), ea&7, (uint16_t)(pos+off)); break;
   case AM_5A: case AM_5B: /* see MAME issue #7451: indexed sub-mode adds a byte */
      ea = fetch(ctx,pos++); ea2 = fetch(ctx,pos++);
      if ((ea&0xC0)==0x80) { sub = fetch(ctx,pos++); EMIT("(rr%u+%02X),$%02X", ea&7, ea2, sub); }
      else                   EMIT("(rr%u),$%02X", ea&7, ea2);
      break;

   case AM_1A: sub = fetch(ctx,pos++); EMIT("%-6s@r%u", SUB_1A[sub&7], (sub>>3)&7); break;
   case AM_1B: sub = fetch(ctx,pos++); EMIT("%-6s@r%u", SUB_1B[sub&7], (sub>>3)&7); break;
   case AM_4F: ea = fetch(ctx,pos++); ea2 = fetch(ctx,pos++);
               EMIT("%-6sR%02X,$%02X", SUB_4F[(ea>>6)&3], ea2, ea&7); break;
   default: break;
   }

   return (int)(uint16_t)(pos - pc);
}
