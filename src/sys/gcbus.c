/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Tigerbyte - Game.com system memory bus. See gcbus.h. */
#include "gcbus.h"
#include <stdio.h>
#include <string.h>

void gcbus_init(gcbus_t *b)
{
   memset(b, 0, sizeof *b);
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

int gcbus_load_cart(gcbus_t *b, const char *path)
{
   uint8_t tmp[GC_CART_SIZE];
   long n = load_file(path, tmp, GC_CART_SIZE);
   if (n <= 0) return 1;
   /* mirror-fill the 2 MB cartridge window with the image */
   for (size_t off = 0; off < GC_CART_SIZE; off += (size_t)n)
      memcpy(b->cart + off, tmp, (off + (size_t)n <= GC_CART_SIZE) ? (size_t)n : (GC_CART_SIZE - off));
   b->cart_loaded = 1;
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
      return b->cart_loaded ? b->cart[off & (GC_CART_SIZE - 1)] : 0xFF;
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
   ram[DMC] = (uint8_t)(dmc & 0x7F);   /* clear start/busy */
   if (b->irq) b->irq(b->irq_user, GC_IRQ_DMA);
}

/* TM*C prescaler-select -> divisor (halved, per the SM8521 timer). */
static const int TIMER_LIMIT[8] = { 2, 1024, 2048, 4096, 8192, 16384, 32768, 65536 };

void gcbus_set_irq_handler(gcbus_t *b, gc_irq_fn fn, void *user)
{
   b->irq = fn;
   b->irq_user = user;
}

void gcbus_tick(gcbus_t *b, int cycles)
{
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
      case 0x50:  /* TM0C */
         b->timer[0].enabled = (val & 0x80) != 0;
         b->timer[0].prescale_max = TIMER_LIMIT[val & 7] >> 1;
         b->timer[0].prescale_count = 0;
         b->ram[0x50] = val; b->ram[0x51] = 0; return;
      case 0x51:  b->timer[0].reload = val; b->ram[0x51] = 0; return;   /* TM0D */
      case 0x52:  /* TM1C */
         b->timer[1].enabled = (val & 0x80) != 0;
         b->timer[1].prescale_max = TIMER_LIMIT[val & 7] >> 1;
         b->timer[1].prescale_count = 0;
         b->ram[0x52] = val; b->ram[0x53] = 0; return;
      case 0x53:  b->timer[1].reload = val; b->ram[0x53] = 0; return;   /* TM1D */
      }
      b->ram[addr] = val;
      if (addr == DMC && (val & 0x80)) gcbus_dma(b);     /* trigger blitter */
      return;
   }
   if (addr < 0xA000) return;                            /* ROM windows: ignore */
   b->ram[addr] = val;                                   /* VRAM + NVRAM */
}
