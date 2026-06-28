/* Tigerbyte - SM8521 executing core. See sm8521.h for notes. */
#include "sm8521.h"

/* PS1 flag bits */
#define FC 0x80
#define FZ 0x40
#define FS 0x20
#define FV 0x10
#define FD 0x08
#define FH 0x04
#define FB 0x02
#define FI 0x01

/* byte-register index -> word-register-pair base address */
static const uint8_t B2W[8] = { 0, 8, 2, 10, 4, 12, 6, 14 };

/* ---- memory access (low 16 bytes = banked register window) ---- */
static uint8_t rb(sm8521_t *c, uint16_t a)
{
   if (a < 0x10) return c->reg[(c->ps0 & 0xF8) + a];
   return c->rd(c->ctx, a);
}
static void get_sp(sm8521_t *c)
{
   c->sp = c->rd(c->ctx, 0x1d);
   if (c->sys & 0x40) c->sp |= (uint16_t)(c->rd(c->ctx, 0x1c) << 8);
}
static void wb(sm8521_t *c, uint16_t a, uint8_t v)
{
   if (a < 0x10) c->reg[(c->ps0 & 0xF8) + a] = v;
   c->wr(c->ctx, a, v);
   /* writes to the in-memory CPU registers refresh the cached copies */
   switch (a) {
   case 0x19: c->sys = v; break;
   case 0x1c: case 0x1d: get_sp(c); break;
   case 0x1e: c->ps0 = v; break;
   case 0x1f: c->ps1 = v; break;
   }
}
static uint16_t rw(sm8521_t *c, uint16_t a) { return (uint16_t)((rb(c, a) << 8) | rb(c, (uint16_t)(a + 1))); }
static void     ww(sm8521_t *c, uint16_t a, uint16_t v) { wb(c, a, (uint8_t)(v >> 8)); wb(c, (uint16_t)(a + 1), (uint8_t)v); }

static uint8_t  fb(sm8521_t *c) { return rb(c, c->pc++); }
static uint16_t fw(sm8521_t *c) { uint16_t h = fb(c); return (uint16_t)((h << 8) | fb(c)); }

/* ---- stack ---- */
static void push8(sm8521_t *c, uint8_t v) { c->sp--; if (!(c->sys & 0x40)) c->sp &= 0xFF; wb(c, c->sp, v); }
static uint8_t pop8(sm8521_t *c) { uint8_t v = rb(c, c->sp); c->sp++; if (!(c->sys & 0x40)) c->sp &= 0xFF; return v; }
static void push16(sm8521_t *c, uint16_t v) { push8(c, (uint8_t)v); push8(c, (uint8_t)(v >> 8)); }
static uint16_t pop16(sm8521_t *c) { uint16_t h = pop8(c); return (uint16_t)((h << 8) | pop8(c)); }

/* ---- 8-bit ALU primitives (set PS1, return result) ---- */
static uint8_t add8(sm8521_t *c, uint8_t a, uint8_t b, int carry)
{
   unsigned ci = carry && (c->ps1 & FC) ? 1 : 0;
   unsigned res = (unsigned)a + b + ci;
   uint8_t f = c->ps1 & (FB | FI);
   if (res > 0xFF) f |= FC;
   if ((res & 0xFF) == 0) f |= FZ;
   if (res & 0x80) f |= FS;
   if (!carry) { if (((b ^ a ^ 0x80) & (res ^ a)) & 0x80) f |= FV; }
   else        { if (((b ^ a) & (res ^ a)) & 0x80) f |= FV; }
   if ((a ^ b ^ res) & 0x10) f |= FH;
   c->ps1 = f;
   return (uint8_t)res;
}
static uint8_t sub8(sm8521_t *c, uint8_t a, uint8_t b, int borrow, int writeback)
{
   unsigned bi = borrow && (c->ps1 & FC) ? 1 : 0;
   unsigned res = (unsigned)a - b - bi;
   uint8_t keep = writeback ? (FB | FI) : (FB | FI | FH | FD);
   uint8_t f = c->ps1 & keep;
   if (res > 0xFF) f |= FC;
   if ((res & 0xFF) == 0) f |= FZ;
   if (res & 0x80) f |= FS;
   if (((b ^ a) & (res ^ a)) & 0x80) f |= FV;
   if (writeback) { f |= FD; if ((a ^ b ^ res) & 0x10) f |= FH; }
   c->ps1 = f;
   return (uint8_t)res;
}
static uint8_t logic8(sm8521_t *c, uint8_t res)
{
   uint8_t f = c->ps1 & (FC | FD | FH | FB | FI);
   if (res == 0) f |= FZ;
   if (res & 0x80) f |= FS;
   c->ps1 = f;
   return res;
}
/* selector 0..7 = cmp,add,sub,adc,sbc,and,or,xor */
static void alu8(sm8521_t *c, int sel, uint16_t dst, uint8_t src)
{
   uint8_t a = rb(c, dst), r;
   switch (sel) {
   case 0: sub8(c, a, src, 0, 0); return;            /* cmp */
   case 1: r = add8(c, a, src, 0); break;            /* add */
   case 2: r = sub8(c, a, src, 0, 1); break;         /* sub */
   case 3: r = add8(c, a, src, 1); break;            /* adc */
   case 4: r = sub8(c, a, src, 1, 1); break;         /* sbc */
   case 5: r = logic8(c, (uint8_t)(a & src)); break; /* and */
   case 6: r = logic8(c, (uint8_t)(a | src)); break; /* or  */
   default: r = logic8(c, (uint8_t)(a ^ src)); break;/* xor */
   }
   wb(c, dst, r);
}

/* ---- 16-bit ALU ---- */
static uint16_t add16(sm8521_t *c, uint16_t a, uint16_t b, int carry)
{
   unsigned ci = carry && (c->ps1 & FC) ? 1 : 0;
   unsigned res = (unsigned)a + b + ci;
   uint8_t f = c->ps1 & (FB | FI);
   if (res > 0xFFFF) f |= FC;
   if ((res & 0xFFFF) == 0) f |= FZ;
   if (res & 0x8000) f |= FS;
   if (((b ^ a) & (res ^ a)) & 0x8000) f |= FV;
   if ((a ^ b ^ res) & 0x0010) f |= FH;
   c->ps1 = f;
   return (uint16_t)res;
}
static uint16_t sub16(sm8521_t *c, uint16_t a, uint16_t b, int borrow, int writeback)
{
   unsigned bi = borrow && (c->ps1 & FC) ? 1 : 0;
   unsigned res = (unsigned)a - b - bi;
   uint8_t keep = writeback ? (FB | FI) : (FB | FI | FH | FD);
   uint8_t f = c->ps1 & keep;
   if (res > 0xFFFF) f |= FC;
   if ((res & 0xFFFF) == 0) f |= FZ;
   if (res & 0x8000) f |= FS;
   if (((b ^ a) & (res ^ a)) & 0x8000) f |= FV;
   if (writeback) { f |= FD; if ((a ^ b ^ res) & 0x0010) f |= FH; }
   c->ps1 = f;
   return (uint16_t)res;
}
static uint16_t logic16(sm8521_t *c, uint16_t res)
{
   uint8_t f = c->ps1 & (FC | FD | FH | FB | FI);
   if (res == 0) f |= FZ;
   if (res & 0x8000) f |= FS;
   c->ps1 = f;
   return res;
}
static void alu16(sm8521_t *c, int sel, uint16_t dst, uint16_t src)
{
   uint16_t a = rw(c, dst), r;
   switch (sel) {
   case 0: sub16(c, a, src, 0, 0); return;            /* cmpw */
   case 1: r = add16(c, a, src, 0); break;            /* addw */
   case 2: r = sub16(c, a, src, 0, 1); break;         /* subw */
   case 3: r = add16(c, a, src, 1); break;            /* adcw */
   case 4: r = sub16(c, a, src, 1, 1); break;         /* sbcw */
   case 5: r = logic16(c, (uint16_t)(a & src)); break;
   case 6: r = logic16(c, (uint16_t)(a | src)); break;
   default: r = logic16(c, (uint16_t)(a ^ src)); break;
   }
   ww(c, dst, r);
}

/* ---- unary 8-bit ops (opcodes 0x00-0x0F) ---- */
static uint8_t inc8(sm8521_t *c, uint8_t v)
{
   unsigned res = v + 1; uint8_t f = c->ps1 & (FC | FD | FH | FB | FI);
   if ((res & 0xFF) == 0) f |= FZ;
   if (res & 0x80) f |= FS;
   if (((v ^ res) & 0x80) && !(res & 0x80)) f |= FV;
   c->ps1 = f; return (uint8_t)res;
}
static uint8_t dec8(sm8521_t *c, uint8_t v)
{
   unsigned res = v - 1; uint8_t f = c->ps1 & (FC | FD | FH | FB | FI);
   if ((res & 0xFF) == 0) f |= FZ;
   if (res & 0x80) f |= FS;
   if (((v ^ res) & 0x80) && (res & 0x80)) f |= FV;
   c->ps1 = f; return (uint8_t)res;
}
static uint8_t neg8(sm8521_t *c, uint8_t v)
{
   unsigned res = (unsigned)(-(int)v); uint8_t f = c->ps1 & (FD | FH | FB | FI);
   if ((res & 0xFF) == 0) f |= FC | FZ;
   if (res & 0x80) f |= FS;
   if ((res & 0xFF) == 0x80) f |= FV;
   c->ps1 = f; return (uint8_t)res;
}
static uint8_t com8(sm8521_t *c, uint8_t v)
{
   uint8_t res = (uint8_t)~v; uint8_t f = c->ps1 & (FC | FD | FH | FB | FI);
   if (res == 0) f |= FZ;
   if (res & 0x80) f |= FS;
   c->ps1 = f; return res;
}
static uint8_t rot8(sm8521_t *c, int kind, uint8_t v)
{
   /* kind: 0 rr,1 rl,2 rrc,3 rlc,4 srl,5 sra,6 sll,7 swap */
   uint8_t f = c->ps1 & (FD | FH | FB | FI), res = 0; int oldc = (c->ps1 & FC) ? 1 : 0;
   switch (kind) {
   case 0: res = (uint8_t)(v >> 1); if (v & 1) res |= 0x80; if (v & 1) f |= FC; break;
   case 1: res = (uint8_t)(v << 1); if (v & 0x80) { res |= 1; f |= FC; } break;
   case 2: res = (uint8_t)(v >> 1); if (oldc) res |= 0x80; if (v & 1) f |= FC; break;
   case 3: res = (uint8_t)(v << 1); if (oldc) res |= 1; if (v & 0x80) f |= FC; break;
   case 4: res = (uint8_t)(v >> 1); if (v & 1) f |= FC; break;
   case 5: res = (uint8_t)(v >> 1); if (v & 0x80) res |= 0x80; if (v & 1) f |= FC; break;
   case 6: res = (uint8_t)(v << 1); if (v & 0x80) f |= FC; break;
   default: res = (uint8_t)((v >> 4) | (v << 4)); break; /* swap: flags below */
   }
   if (res == 0) f |= FZ;
   if (res & 0x80) f |= FS;
   /* V for rotates/shifts that define it (rr/rl/rrc/rlc) */
   if (kind == 1 || kind == 3) { if ((v ^ res) & 0x80) f |= FV; }
   else if (kind == 0 || kind == 2) { if (((v ^ res) & 0x80) && !(res & 0x80)) f |= FV; }
   c->ps1 = f;
   return res;
}

/* ---- condition codes (cc 0..15) ---- */
static int cond(sm8521_t *c, uint8_t cc)
{
   uint8_t f = c->ps1;
   int C = !!(f & FC), Z = !!(f & FZ), S = !!(f & FS), V = !!(f & FV), base = 0;
   switch (cc & 7) {
   case 0: base = 0; break;                /* F  */
   case 1: base = (S ^ V); break;          /* LT */
   case 2: base = (Z || (S ^ V)); break;   /* LE */
   case 3: base = (C || Z); break;         /* ULE */
   case 4: base = V; break;                /* OV */
   case 5: base = S; break;                /* MI */
   case 6: base = Z; break;                /* Z  */
   case 7: base = C; break;                /* C  */
   }
   return (cc & 8) ? !base : base;
}

/* ---- register-indirect operand pointers (byte/word pointer regs) ---- */
static uint16_t arg_rmb(sm8521_t *c, uint8_t *dst)   /* 8-bit pointer register */
{
   uint8_t m = fb(c); uint16_t s;
   switch (m & 0xC0) {
   case 0x00: s = rb(c, m & 7); break;
   case 0x40: s = rb(c, m & 7); wb(c, m & 7, (uint8_t)(s + 1)); break;
   case 0x80: { uint8_t i = fb(c); s = i; if (m & 7) s = (uint16_t)(i + rb(c, m & 7)); } break;
   default:   s = rb(c, m & 7); wb(c, m & 7, (uint8_t)(s - 1)); break;
   }
   *dst = (m >> 3) & 7;
   return s;
}
static uint16_t arg_rmw(sm8521_t *c, uint8_t *dst)   /* 16-bit pointer register pair */
{
   uint8_t m = fb(c); uint16_t s, rp = B2W[m & 7];
   switch (m & 0xC0) {
   case 0x00: s = rw(c, rp); break;
   case 0x40: s = rw(c, rp); ww(c, rp, (uint16_t)(s + 1)); break;
   case 0x80: { uint16_t i = fw(c); s = i; if (m & 7) s = (uint16_t)(i + rw(c, rp)); } break;
   default:   s = (uint16_t)(rw(c, rp) - 1); ww(c, rp, s); break;
   }
   *dst = (m >> 3) & 7;
   return s;
}

/* ---- interrupts (mirrors the SM8500 priority ladder) ---- */
static void take_interrupt(sm8521_t *c, uint16_t vector)
{
   c->sp  = c->rd(c->ctx, 0x1d);
   c->sys = c->rd(c->ctx, 0x19);
   if (c->sys & 0x40) c->sp |= (uint16_t)(c->rd(c->ctx, 0x1c) << 8);
   c->ps1 = c->rd(c->ctx, 0x1f);
   push8(c, (uint8_t)(c->pc & 0xFF));
   push8(c, (uint8_t)(c->pc >> 8));
   push8(c, c->ps1);
   c->ps1 &= (uint8_t)~FI;
   c->wr(c->ctx, 0x1f, c->ps1);
   c->wr(c->ctx, 0x1d, (uint8_t)(c->sp & 0xFF));
   if (c->sys & 0x40) c->wr(c->ctx, 0x1c, (uint8_t)(c->sp >> 8));
   c->pc = rw(c, vector);
}
static void process_interrupts(sm8521_t *c)
{
   if (!c->check_irq) return;
   for (int line = 0; line < 11; line++) {
      if (!(c->iflags & (1u << line))) continue;
      c->halted = 0;
      c->ps0 = c->rd(c->ctx, 0x1e);
      c->ps1 = c->rd(c->ctx, 0x1f);
      uint8_t ie0 = c->rd(c->ctx, 0x10), ie1 = c->rd(c->ctx, 0x11);
      int prio = c->ps0 & 0x07, gie = c->ps1 & FI;
      switch (line) {
      case SM_WDT: take_interrupt(c, 0x101C); break;
      case SM_ILL: case SM_NMI: take_interrupt(c, 0x101E); break;
      case SM_DMA:  if ((ie0 & 0x80) && gie) take_interrupt(c, 0x1000); break;
      case SM_TIM0: if ((ie0 & 0x40) && gie) take_interrupt(c, 0x1002); break;
      case SM_EXT:  if ((ie0 & 0x10) && prio < 7 && gie) take_interrupt(c, 0x1006); break;
      case SM_UART: if ((ie0 & 0x08) && prio < 6 && gie) take_interrupt(c, 0x1008); break;
      case SM_LCDC: if ((ie0 & 0x01) && prio < 5 && gie) take_interrupt(c, 0x100E); break;
      case SM_TIM1: if ((ie1 & 0x40) && prio < 4 && gie) take_interrupt(c, 0x1012); break;
      case SM_CK:   if ((ie1 & 0x10) && prio < 3 && gie) take_interrupt(c, 0x1016); break;
      case SM_PIO:  if ((ie1 & 0x04) && prio < 2 && gie) take_interrupt(c, 0x101A); break;
      }
      c->iflags &= ~(1u << line);
      c->wr(c->ctx, 0x1f, c->ps1);
   }
   if (c->iflags == 0) c->check_irq = 0;
}

/* ---- public API ---- */
void sm8521_init(sm8521_t *c, sm8521_read_fn rd, sm8521_write_fn wr, void *ctx)
{
   for (unsigned i = 0; i < sizeof *c; i++) ((uint8_t *)c)[i] = 0;
   c->rd = rd; c->wr = wr; c->ctx = ctx;
}
void sm8521_reset(sm8521_t *c)
{
   for (unsigned i = 0; i < sizeof c->reg; i++) c->reg[i] = 0;
   c->pc = 0x1020; c->sp = 0; c->ps0 = 0; c->ps1 = 0; c->sys = 0;
   c->iflags = 0; c->check_irq = 0; c->halted = 0; c->stopped = 0;
   c->trapped = 0; c->trap_op = 0; c->trap_pc = 0;
   ww(c, 0x10, 0);        /* IE0,IE1 */
   ww(c, 0x12, 0);        /* IR0,IR1 */
   ww(c, 0x14, 0xFFFF);   /* P0,P1   */
   ww(c, 0x16, 0xFF00);   /* P2,P3   */
   wb(c, 0x19, 0);        /* SYS     */
   wb(c, 0x1a, 0);        /* CKC     */
   wb(c, 0x1f, 0);        /* PS1     */
   wb(c, 0x2b, 0xFF);     /* URTT    */
   wb(c, 0x2d, 0x42);     /* URTS    */
   wb(c, 0x5f, 0x38);     /* WDTC    */
}
void sm8521_set_irq(sm8521_t *c, int line, int asserted)
{
   if (asserted) { c->iflags |= (1u << line); c->check_irq = 1; }
   else { c->iflags &= ~(1u << line); if (!c->iflags) c->check_irq = 0; }
}

static void trap(sm8521_t *c, uint8_t op, uint16_t at) { c->trapped = 1; c->trap_op = op; c->trap_pc = at; }

int sm8521_step(sm8521_t *c)
{
   process_interrupts(c);
   if (c->halted || c->stopped) return 4;

   /* re-sync the registers the architecture keeps in memory */
   c->sys = c->rd(c->ctx, 0x19);
   c->ps0 = c->rd(c->ctx, 0x1e);
   c->ps1 = c->rd(c->ctx, 0x1f);
   c->sp  = c->rd(c->ctx, 0x1d);
   if (c->sys & 0x40) c->sp |= (uint16_t)(c->rd(c->ctx, 0x1c) << 8);

   uint16_t at = c->pc;
   uint8_t op = fb(c);
   int cyc = 6;
   uint8_t m, dst, a1, a2;
   uint16_t ptr, t;

   if (op <= 0x0F) {                    /* unary R-operand ops */
      uint16_t r = fb(c); uint8_t v = rb(c, r);
      switch (op) {
      case 0x00: wb(c, r, logic8(c, 0)); break;             /* clr (Z set) */
      case 0x01: wb(c, r, neg8(c, v)); break;
      case 0x02: wb(c, r, com8(c, v)); break;
      case 0x03: wb(c, r, rot8(c, 0, v)); break;            /* rr  */
      case 0x04: wb(c, r, rot8(c, 1, v)); break;            /* rl  */
      case 0x05: wb(c, r, rot8(c, 2, v)); break;            /* rrc */
      case 0x06: wb(c, r, rot8(c, 3, v)); break;            /* rlc */
      case 0x07: wb(c, r, rot8(c, 4, v)); break;            /* srl */
      case 0x08: wb(c, r, inc8(c, v)); break;
      case 0x09: wb(c, r, dec8(c, v)); break;
      case 0x0A: wb(c, r, rot8(c, 5, v)); break;            /* sra */
      case 0x0B: wb(c, r, rot8(c, 6, v)); break;            /* sll */
      case 0x0C: trap(c, op, at); break;                    /* da  (TODO) */
      case 0x0D: wb(c, r, rot8(c, 7, v)); break;            /* swap */
      case 0x0E: push8(c, v); break;                        /* push R */
      case 0x0F: wb(c, r, pop8(c)); break;                  /* pop R  */
      }
   } else if (op <= 0x17) {             /* ALU rr (two bank regs) */
      m = fb(c);
      if ((m & 0xC0) == 0) alu8(c, op & 7, (m >> 3) & 7, rb(c, m & 7));
   } else if (op == 0x18) { uint8_t r = fb(c); ww(c, r, add16(c, rw(c, r), 1, 0)); }  /* incw */
     else if (op == 0x19) { uint8_t r = fb(c); ww(c, r, sub16(c, rw(c, r), 1, 0, 1)); } /* decw */
     else if (op == 0x1C || op == 0x1D) {  /* bclr/bset riB (indexed bit) */
      m = fb(c); uint8_t bit = (uint8_t)(1 << (m & 7)); uint8_t disp = fb(c);
      uint16_t addr = (m & 0x38) ? (uint16_t)(disp + rb(c, (m >> 3) & 7)) : (uint16_t)(0xFF00 + disp);
      uint8_t v = rb(c, addr);
      wb(c, addr, (uint8_t)(op == 0x1C ? (v & ~bit) : (v | bit)));
   } else if (op == 0x1E) { uint8_t r = fb(c); push16(c, rw(c, r)); }   /* pushw */
     else if (op == 0x1F) { uint8_t r = fb(c); ww(c, r, pop16(c)); }    /* popw  */
     else if (op >= 0x20 && op <= 0x27) { ptr = arg_rmb(c, &dst); alu8(c, op & 7, dst, rb(c, ptr)); }
     else if (op == 0x28) { ptr = arg_rmb(c, &dst); wb(c, dst, rb(c, ptr)); }   /* mov rmb (load) */
     else if (op == 0x29) { ptr = arg_rmb(c, &dst); wb(c, ptr, rb(c, dst)); }   /* mov mbr (store) */
     else if (op == 0x2A || op == 0x2B) {   /* bbc/bbs direct-page bit, relative */
      m = fb(c); uint8_t bit = (uint8_t)(1 << (m & 7)); uint8_t disp = fb(c);
      uint16_t addr = (m & 0x38) ? (uint16_t)(disp + rb(c, (m >> 3) & 7)) : (uint16_t)(0xFF00 + disp);
      int8_t off = (int8_t)fb(c); int set = (rb(c, addr) & bit) != 0;
      if (op == 0x2A ? !set : set) c->pc = (uint16_t)(c->pc + off);
   } else if (op == 0x2E) { uint8_t v = fb(c); c->ps0 = v; wb(c, 0x1e, v); } /* mov PS0,i */
     else if (op <= 0x37 && op >= 0x30) { ptr = arg_rmw(c, &dst); alu8(c, op & 7, dst, rb(c, ptr)); }
     else if (op == 0x38) { ptr = arg_rmw(c, &dst); wb(c, dst, rb(c, ptr)); }  /* mov rmw (load) */
     else if (op == 0x39) { ptr = arg_rmw(c, &dst); wb(c, ptr, rb(c, dst)); }  /* mov mwr (store) */
     else if (op == 0x3A) { uint8_t pd; ptr = arg_rmw(c, &pd); ww(c, B2W[pd], rw(c, ptr)); } /* movw smw */
     else if (op == 0x3B) { uint8_t pd; ptr = arg_rmw(c, &pd); ww(c, ptr, rw(c, B2W[pd])); } /* movw mws */
     else if (op == 0x3C) { m = fb(c); if ((m & 0xC0) == 0) ww(c, B2W[(m >> 3) & 7], rw(c, B2W[m & 7])); } /* movw ss */
     else if (op == 0x3E || op == 0x3F) {   /* jmp/call (2) */
      m = fb(c);
      if ((m & 0xC0) == 0x00) t = rw(c, B2W[m & 7]);
      else if ((m & 0xC0) == 0x40) { uint16_t e = fw(c); if (m & 0x38) e = (uint16_t)(e + rb(c, (m >> 3) & 7)); t = rw(c, e); }
      else { trap(c, op, at); t = c->pc; }
      if (!c->trapped) { if (op == 0x3F) push16(c, c->pc); c->pc = t; }
   } else if (op <= 0x47 && op >= 0x40) { a1 = fb(c); a2 = fb(c); alu8(c, op & 7, a2, rb(c, a1)); } /* ALU RR */
     else if (op == 0x48) { a1 = fb(c); a2 = fb(c); wb(c, a2, rb(c, a1)); }   /* mov RR */
     else if (op == 0x49) { t = fw(c); push16(c, c->pc); c->pc = t; }         /* call addr */
     else if (op == 0x4A) { a1 = fb(c); a2 = fb(c); ww(c, a2, rw(c, a1)); }   /* movw SS */
     else if (op == 0x4B) { uint8_t r = fb(c); t = fw(c); ww(c, r, t); }      /* movw Sw */
     else if (op >= 0x50 && op <= 0x57) { a1 = fb(c); a2 = fb(c); alu8(c, op & 7, a2, a1); } /* ALU iR (imm,Raddr) */
     else if (op == 0x58) { a1 = fb(c); a2 = fb(c); wb(c, a2, a1); }          /* mov iR */
     else if (op >= 0x60 && op <= 0x67) { m = fb(c); if ((m & 0xC0) == 0) alu16(c, op & 7, B2W[(m >> 3) & 7], rw(c, B2W[m & 7])); } /* ALUW SS */
     else if (op >= 0x68 && op <= 0x6F) { uint8_t r = fb(c); t = fw(c); alu16(c, op & 7, r, t); } /* ALUW Sw */
     else if (op >= 0x70 && op <= 0x77) {   /* dbnz r,rel */
      int8_t off = (int8_t)fb(c); uint8_t v = (uint8_t)(rb(c, op & 7) - 1);
      wb(c, op & 7, v); if (v) c->pc = (uint16_t)(c->pc + off);
   } else if (op >= 0x78 && op <= 0x7F) { t = fw(c); ww(c, B2W[op & 7], t); } /* movw riw */
     else if (op >= 0x80 && op <= 0x8F) {   /* bbc/bbs R,#bit,rel */
      uint8_t r = fb(c); int8_t off = (int8_t)fb(c); int set = (rb(c, r) & (1 << (op & 7))) != 0;
      if ((op & 0x08) ? set : !set) c->pc = (uint16_t)(c->pc + off);
   } else if (op >= 0x90 && op <= 0x9F) { t = fw(c); if (cond(c, op & 0x0F)) c->pc = t; } /* jmp cc */
     else if (op >= 0xA0 && op <= 0xAF) {   /* bclr/bset R,#bit */
      uint8_t r = fb(c); uint8_t v = rb(c, r), bit = (uint8_t)(1 << (op & 7));
      wb(c, r, (uint8_t)((op & 0x08) ? (v | bit) : (v & ~bit)));
   } else if (op >= 0xB0 && op <= 0xB7) { uint8_t r = fb(c); wb(c, op & 7, rb(c, r)); }      /* mov rR */
     else if (op >= 0xB8 && op <= 0xBF) { uint8_t r = fb(c); wb(c, r, rb(c, op & 7)); }      /* mov Rr */
     else if (op >= 0xC0 && op <= 0xC7) { uint8_t i = fb(c); wb(c, op & 7, i); }             /* mov rib */
     else if (op >= 0xC8 && op <= 0xCF) { uint8_t i = fb(c); wb(c, 0x10 + (op & 7), i); }    /* mov pi  */
     else if (op >= 0xD0 && op <= 0xDF) { int8_t off = (int8_t)fb(c); if (cond(c, op & 0x0F)) c->pc = (uint16_t)(c->pc + off); } /* br cc */
     else if (op >= 0xE0 && op <= 0xEF) { uint8_t i = fb(c); push16(c, c->pc); c->pc = (uint16_t)(0x1000 | ((op & 0x0F) << 8) | i); } /* cals */
     else switch (op) {
      case 0xF0: c->stopped = 1; break;
      case 0xF1: c->halted = 1; break;
      case 0xF8: c->pc = pop16(c); break;                       /* ret  */
      case 0xF9: c->ps1 = pop8(c); c->pc = pop16(c); break;     /* iret */
      case 0xFA: c->ps1 &= (uint8_t)~FC; break;                 /* clrc */
      case 0xFB: c->ps1 ^= FC; break;                           /* comc */
      case 0xFC: c->ps1 |= FC; break;                           /* setc */
      case 0xFD: c->ps1 |= FI; break;                           /* ei   */
      case 0xFE: c->ps1 &= (uint8_t)~FI; break;                 /* di   */
      case 0xFF: break;                                         /* nop  */
      default: trap(c, op, at); break;
      }

   /* write back the in-memory registers */
   if (c->sys & 0x40) c->wr(c->ctx, 0x1c, (uint8_t)(c->sp >> 8));
   c->wr(c->ctx, 0x1d, (uint8_t)(c->sp & 0xFF));
   wb(c, 0x1e, c->ps0);
   c->wr(c->ctx, 0x1f, c->ps1);

   if (c->trapped) c->pc = at;   /* rewind so the trap PC is the faulting op */
   return cyc;
}
