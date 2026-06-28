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

void gcbus_write(void *ctx, uint16_t addr, uint8_t val)
{
   gcbus_t *b = (gcbus_t *)ctx;

   if (addr < 0x1000) { b->ram[addr] = val; return; }   /* RAM/IO (incl MMU regs) */
   if (addr < 0xA000) return;                            /* ROM windows: ignore */
   b->ram[addr] = val;                                   /* VRAM + NVRAM */
}
