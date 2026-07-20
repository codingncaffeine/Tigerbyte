/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Tigerbyte - Game.com system memory bus. See gcbus.h. */
#include "gcbus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void gcbus_init(gcbus_t *b)
{
   memset(b, 0, sizeof *b);
   b->in0 = b->in1 = b->in2 = 0xFF;     /* no buttons pressed (active-low) */
   b->clock_hz = 4915200u;
}

void gcbus_set_buttons(gcbus_t *b, uint8_t in0, uint8_t in1, uint8_t in2)
{
   b->in0 = in0; b->in1 = in1; b->in2 = in2;
}
void gcbus_set_touch(gcbus_t *b, int column, uint16_t rows)
{
   if (column >= 0 && column < 13) b->grid[column] = rows;
}

/* The kernel strobes P1/P2 to scan one key/touch column; the hardware then
 * fills P0/P1 with the pressed state for that strobe (mirrors gamecom_m.cpp). */
static void gc_input_press(gcbus_t *b, uint16_t mux)
{
   uint8_t *ram = b->ram;
   int col;
   switch (mux) {
   case 0xFFFB: col = 0;  break;  case 0xFFF7: col = 1;  break;
   case 0xFFEF: col = 2;  break;  case 0xFFDF: col = 3;  break;
   case 0xFFBF: col = 4;  break;  case 0xFF7F: col = 5;  break;
   case 0xFEFF: col = 6;  break;  case 0xFDFF: col = 7;  break;
   case 0xFBFF: col = 8;  break;  case 0xF7FF: col = 9;  break;
   case 0xEFFF: col = 10; break;  case 0xDFFF: col = 11; break;
   case 0xBFFF: col = 12; break;
   case 0x7FFF:                                  /* d-pad / face keys */
      ram[0x14] = b->in0;
      ram[0x15] = (uint8_t)((ram[0x15] & 0xFC) | (b->in1 & 3));
      return;
   case 0xFFFF:                                  /* power / button D */
      ram[0x14] = (uint8_t)((ram[0x14] & 0xFC) | (b->in2 & 3));
      ram[0x15] = 0xFF;
      return;
   default: return;
   }
   {                                             /* touch column */
      uint16_t data = b->grid[col];
      if (data) {
         uint16_t sy = (uint16_t)(data ^ 0x3FF);
         ram[0x14] = (uint8_t)sy;
         ram[0x15] = (uint8_t)((ram[0x15] & 0xFC) | (sy >> 8));
      } else {
         ram[0x14] = 0xFF;
         ram[0x15] |= 3;
      }
   }
}

static long load_file(const char *path, uint8_t *dst, size_t cap)
{
   FILE *f = fopen(path, "rb");
   if (!f) return -1;
   size_t n = fread(dst, 1, cap, f);
   fclose(f);
   return (long)n;
}

int gcbus_load_internal(gcbus_t *b, const char *path)
{
   long n = load_file(path, b->irom, GC_IROM_SIZE);
   return (n == GC_IROM_SIZE) ? 0 : 1;
}

int gcbus_load_external(gcbus_t *b, const char *path)
{
   long n = load_file(path, b->krom, GC_KROM_SIZE);
   return (n == GC_KROM_SIZE) ? 0 : 1;
}

void gcbus_load_cart_mem(gcbus_t *b, const uint8_t *data, size_t size)
{
   if (!data || size == 0) return;
   memset(b->cart, 0, GC_CART_SIZE);
   if (size == 0x1C0000) {
      /* 1.75 MB mask ROMs sit at the TOP of the 2 MB space: the first 256 KB of
         page numbers don't exist on the chip. Loading them at 0 skewed every
         bank the kernel touches and the launcher bounced back to the menu. */
      memcpy(b->cart + 0x40000, data, size);
   } else if (size <= GC_CART_SIZE && (size & (size - 1)) == 0) {
      /* power-of-two image: mirror by doubling up to the full window */
      memcpy(b->cart, data, size);
      for (size_t sz = size; sz < GC_CART_SIZE; sz <<= 1)
         memcpy(b->cart + sz, b->cart, sz);
   } else {
      /* odd homebrew size: permissive sequential fill */
      for (size_t off = 0; off < GC_CART_SIZE; off += size)
         memcpy(b->cart + off, data, (off + size <= GC_CART_SIZE) ? size : (GC_CART_SIZE - off));
   }
   b->cart_loaded = 1;
}

int gcbus_load_cart(gcbus_t *b, const char *path)
{
   static uint8_t tmp[GC_CART_SIZE];   /* static: avoid a 2 MB stack buffer */
   long n = load_file(path, tmp, GC_CART_SIZE);
   if (n <= 0) return 1;
   gcbus_load_cart_mem(b, tmp, (size_t)n);
   return 0;
}

uint8_t gcbus_read(void *ctx, uint16_t addr)
{
   gcbus_t *b = (gcbus_t *)ctx;

   if (addr < 0x1000)                       /* register file / I/O + scratch RAM */
      return b->ram[addr];
   if (addr < 0x2000)                       /* internal boot ROM */
      return b->irom[addr - 0x1000];
   if (addr < 0xA000) {                     /* MMU-paged windows 1-4 */
      int      win  = (addr - 0x2000) >> 13;          /* 0..3 -> MMU1..MMU4 */
      uint8_t  page = b->ram[0x25 + win];             /* MMU register value */
      uint32_t off  = ((uint32_t)page << 13) | (addr & 0x1FFF);
      if (page < 0x20)
         return b->krom[off & (GC_KROM_SIZE - 1)];
      /* cart pages only respond when P3[7:6] selects slot 1 and a cart is in */
      if (b->cart_loaded && (b->ram[0x17] & 0xC0) == 0x40)
         return b->cart[off & (GC_CART_SIZE - 1)];
      return 0xFF;
   }
   if (addr < 0xE000)                        /* VRAM is write-only to the CPU */
      return 0;
   return b->ram[addr];                      /* NVRAM / extended I/O */
}

/* DMA register addresses */
enum { DMC=0x34, DMX1=0x35, DMY1=0x36, DMDX=0x37, DMDY=0x38,
       DMX2=0x39, DMY2=0x3A, DMPL=0x3B, DMBR=0x3C, DMVP=0x3D, LCH=0x31 };

/* Hardware sprite blitter. Runs to completion when DMC bit7 is written set. */
static void gcbus_dma(gcbus_t *b)
{
   uint8_t *ram = b->ram;
   uint8_t  dmc = ram[DMC];
   int  overwrite = dmc & 0x01;
   int  mode      = dmc & 0x06;
   int  adjust_x  = (dmc & 0x08) ? -1 : 1;
   int  dec_y     = (dmc & 0x10) != 0;
   uint8_t  bw = ram[DMDX], bh = ram[DMDY];
   uint8_t  sx = ram[DMX1], sy = ram[DMY1];
   uint8_t  dx = ram[DMX2], dy = ram[DMY2];
   uint8_t  pal = ram[DMPL];
   int  sw = (ram[LCH] & 0x20) ? 50 : 40, dw = sw;
   uint16_t smask = 0x1FFF, dmask = 0x1FFF;
   uint8_t *vram  = ram + GC_VRAM_BASE;
   uint8_t *sbank = vram + ((ram[DMVP] & 1) ? 0x2000 : 0);
   uint8_t *dbank = vram + ((ram[DMVP] & 2) ? 0x2000 : 0);

   switch (mode) {
   case 0x00: break;                                  /* VRAM -> VRAM */
   case 0x02:                                         /* ROM  -> VRAM */
      sw = 64; smask = 0x3FFF;
      if (ram[DMBR] < 16) sbank = b->krom + ((uint32_t)ram[DMBR] << 14);
      else sbank = b->cart + (((uint32_t)ram[DMBR] << 14) & (GC_CART_SIZE - 1));
      break;
   case 0x04: sw = 64; sbank = ram + 0xE000; break;   /* ExtRAM -> VRAM */
   case 0x06: dw = 64; dbank = ram + 0xE000; break;   /* VRAM -> ExtRAM */
   }

   {  /* TB_DMALOG: dump game-content blits (cart-tile and ExtRAM sources),
         collapsing runs that differ only in destination */
      static int dmalog = -1, dmalogged = 0;
      static int lmode = -1, lbr = -1, lsy = -1, lw = -1;
      if (dmalog < 0) dmalog = getenv("TB_DMALOG") != NULL;
      if (dmalog && dmalogged < 220 &&
          ((mode == 0x02 && ram[DMBR] >= 16) || mode == 0x04 || mode == 0x06)) {
         if (mode != lmode || ram[DMBR] != lbr || (int)bw != lw ||
             (sy > lsy + 8) || (sy < lsy - 8)) {
            lmode = mode; lbr = ram[DMBR]; lsy = sy; lw = bw;
            dmalogged++;
            fprintf(stderr, "[dma] mode=%X DMBR=%02X %dx%d src=(%d,%d) dst=(%d,%d) pal=%02X vp=%02X | src:",
                    mode, ram[DMBR], bw + 1, bh + 1, sx, sy, dx, dy, pal, ram[DMVP]);
            for (int i = 0; i < 8; i++)
               fprintf(stderr, " %02X", sbank[(uint16_t)((sw * sy + (sx >> 2) + i)) & smask]);
            fprintf(stderr, "\n");
         } else
            lsy = sy;
      }
   }

   int sx_cur = sx & 3, dx_cur = dx & 3;
   int s_cur = sw * sy + (sx >> 2), d_cur = dw * dy + (dx >> 2);
   int s_line = s_cur, d_line = d_cur;

   for (int yc = 0; yc <= bh; yc++) {
      for (int xc = 0; xc <= bw; xc++) {
         uint16_t sa = (uint16_t)(s_cur & smask), da = (uint16_t)(d_cur & dmask);
         int dadj = (dx_cur ^ 3) << 1, sadj = (sx_cur ^ 3) << 1;
         uint8_t spix = (sbank[sa] >> sadj) & 3;
         if (overwrite || spix) {
            uint8_t other = dbank[da] & (uint8_t)~(3 << dadj);
            dbank[da] = (uint8_t)(other | (((pal >> (spix << 1)) & 3) << dadj));
         }
         sx_cur += adjust_x;
         if (sx_cur & 4) { s_cur += adjust_x; sx_cur &= 3; }
         dx_cur++;
         if (dx_cur & 4) { d_cur++; dx_cur &= 3; }
      }
      sx_cur = sx & 3; dx_cur = dx & 3;
      s_line += dec_y ? -sw : sw; s_cur = s_line;
      d_line += dw; d_cur = d_line;
   }
   /* The pixels land in VRAM immediately (it's CPU-write-only, nothing can peek),
      but completion must take real time: the kernel starts a blit and then HALTs
      waiting for the DMA interrupt — an instant IRQ gets consumed before the HALT
      and the kernel oversleeps into the next 1 Hz clock tick (boot ran at one
      animation step per second). ~1.5 cycles/pixel of bus occupancy. */
   if (b->irq) {
      int px = ((int)bw + 1) * ((int)bh + 1);
      b->dma_cycles_left = px + (px >> 1);
      if (b->dma_cycles_left < 8) b->dma_cycles_left = 8;
   } else {
      ram[DMC] = (uint8_t)(dmc & 0x7F);  /* bare-CPU tools (no IRQ sink): instant */
   }
}

/* TM*C prescaler-select -> divisor. The datasheet's table (p.31: fCK/2, /1024,
   ... /65536) describes the timer OUTPUT square wave, which inverts on each
   counter/time-constant coincidence — so coincidences (and the interrupt) run at
   TWICE those rates, i.e. an effective divisor of half the table. Empirically
   pinned by the real-hardware boot recording: the kernel programs sel=0/TC=208
   and streams the DAC from every 3rd TIM1 interrupt; only the halved divisor
   (interrupt rate fCK/208 = 23.6 kHz, DAC ~7.9 kHz) reproduces the recording's
   pitch/length — fCK/(2*208) leaves the jingle an octave low and twice as long.
   (Same trap MAME's "countdown goes twice as fast as it should" note fell in.) */
static const int TIMER_EFF_DIV[8] = { 1, 512, 1024, 2048, 4096, 8192, 16384, 32768 };

void gcbus_set_irq_handler(gcbus_t *b, gc_irq_fn fn, void *user)
{
   b->irq = fn;
   b->irq_user = user;
}

void gcbus_tick(gcbus_t *b, int cycles, int stopped)
{
   /* clock timer (CK): a real periodic source per CLKT (0x1A) — bit7 run/reset,
      bit6 selects 1 s / 1 min. Sub-clock driven, so it runs even in STOP mode
      and is what wakes the kernel's boot STOP. (Was: a fake CK fired every idle
      step, which storm-fed the CK ISR and distorted idle timing.) */
   if (b->ram[0x1a] & 0x80) {
      uint32_t period = (b->ram[0x1a] & 0x40) ? b->clock_hz * 60u : b->clock_hz;
      b->ck_accum += (uint32_t)cycles;
      if (b->ck_accum >= period) {
         b->ck_accum -= period;
         b->dbg_ck++;
         if (b->irq) b->irq(b->irq_user, GC_IRQ_CK);
      }
   } else
      b->ck_accum = 0;

   if (stopped) return;              /* STOP halts the main clock: TM0/TM1 + blitter freeze */

   if (b->dma_cycles_left > 0) {
      b->dma_cycles_left -= cycles;
      if (b->dma_cycles_left <= 0) {
         b->dma_cycles_left = 0;
         b->ram[0x34] &= 0x7F;                            /* DMC: clear start/busy */
         if (b->irq) b->irq(b->irq_user, GC_IRQ_DMA);
      }
   }

   if (b->uart_cycles_left > 0) {
      b->uart_cycles_left -= cycles;
      if (b->uart_cycles_left <= 0) {
         b->uart_cycles_left = 0;
         if (b->irq) b->irq(b->irq_user, GC_IRQ_UART);
      }
   }

   for (int t = 0; t < 2; t++) {
      gc_timer_t *tm = &b->timer[t];
      if (!tm->enabled || tm->prescale_max <= 0) continue;
      tm->prescale_count += cycles;
      while (tm->prescale_count >= tm->prescale_max) {
         tm->prescale_count -= tm->prescale_max;
         uint16_t dreg = (t == 0) ? 0x51 : 0x53;          /* TM0D / TM1D counter */
         b->ram[dreg]++;
         if (b->ram[dreg] >= tm->reload) {
            b->ram[dreg] = 0;
            b->dbg_ovf++;
            /* raise only when the source AND global interrupts are enabled
               (matches MAME; avoids choking the interrupt queue) */
            int ie   = (t == 0) ? (b->ram[0x10] & 0x40) : (b->ram[0x11] & 0x40);
            int gie  = b->ram[0x1f] & 0x01;
            int prio = (t == 0) ? 1 : ((b->ram[0x1e] & 7) < 4);
            if (ie && gie && prio && b->irq) {
               b->dbg_ovf_raised++;
               b->irq(b->irq_user, (t == 0) ? GC_IRQ_TIM0 : GC_IRQ_TIM1);
            }
         }
      }
   }
}

void gcbus_write(void *ctx, uint16_t addr, uint8_t val)
{
   gcbus_t *b = (gcbus_t *)ctx;

   if (addr < 0x1000) {                                  /* RAM/IO (incl MMU regs) */
      switch (addr) {
      case 0x15: case 0x16:  /* P1/P2 strobe -> scan input into P0/P1 */
         b->ram[addr] = val;
         gc_input_press(b, (uint16_t)(b->ram[0x15] | (b->ram[0x16] << 8)));
         return;
      case 0x50:  /* TM0C */
         b->timer[0].enabled = (val & 0x80) != 0;
         b->timer[0].prescale_max = TIMER_EFF_DIV[val & 7];
         b->timer[0].prescale_count = 0;
         b->ram[0x50] = val; b->ram[0x51] = 0; return;
      case 0x51:  b->timer[0].reload = val; b->ram[0x51] = 0; return;   /* TM0D */
      case 0x52:  /* TM1C */
         b->timer[1].enabled = (val & 0x80) != 0;
         b->timer[1].prescale_max = TIMER_EFF_DIV[val & 7];
         b->timer[1].prescale_count = 0;
         b->ram[0x52] = val; b->ram[0x53] = 0; return;
      case 0x53:  b->timer[1].reload = val; b->ram[0x53] = 0; return;   /* TM1D */
      }
      if (addr == 0x2B) {
         /* URTT write: nothing is attached to the link port, but the shifter
            still empties — complete the transmit with a UART interrupt so
            link-cable probes (fighting games, Web Link) run to completion.
            ~10 bits at 9600-ish baud ≈ 5000 cycles; exact rate isn't critical. */
         b->ram[addr] = val;
         b->uart_cycles_left = 5000;
         b->ram[0x2D] |= 0x02;                           /* URTS: TDRE (transmit empty) */
         return;
      }
      if (addr >= 0x40 && addr <= 0x4F) {                /* sound registers */
         b->snd_reg_writes++;
         if (addr == 0x4E) {                             /* capture the DAC stream + write timing */
            b->snd_dac_writes++;
            if (b->dac_stream_n < (int)(sizeof b->dac_stream)) {
               b->dac_cycle[b->dac_stream_n]  = (uint32_t)b->cur_cycle;
               b->dac_stream[b->dac_stream_n++] = val;
            }
         }
      }
      b->ram[addr] = val;
      if (addr == DMC && (val & 0x80)) gcbus_dma(b);     /* trigger blitter */
      return;
   }
   if (addr < 0xA000) return;                            /* ROM windows: ignore */
   if (addr < 0xE000) {                                  /* CPU-direct VRAM write */
      static int vlog = -1; static long vcount = 0;
      if (vlog < 0) vlog = getenv("TB_VRAMLOG") != NULL;
      if (vlog) {
         vcount++;
         if (vcount <= 48 || (vcount >= 100000 && vcount < 100048))
            fprintf(stderr, "[vram] #%ld %04X=%02X\n", vcount, addr, val);
      }
   }
   b->ram[addr] = val;                                   /* VRAM + NVRAM */
}
