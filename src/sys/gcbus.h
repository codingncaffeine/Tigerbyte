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

typedef struct {
   uint8_t ram[0x10000];                 /* RAM/IO page, NVRAM, and VRAM backing */
   uint8_t irom[GC_IROM_SIZE];
   uint8_t krom[GC_KROM_SIZE];
   uint8_t cart[GC_CART_SIZE];
   int     cart_loaded;
} gcbus_t;

void gcbus_init(gcbus_t *b);

/* Load ROM images; return 0 on success, nonzero on error (bad size / IO). */
int gcbus_load_internal(gcbus_t *b, const char *path);
int gcbus_load_external(gcbus_t *b, const char *path);
int gcbus_load_cart(gcbus_t *b, const char *path);

/* CPU bus callbacks (cast ctx to gcbus_t*). */
uint8_t gcbus_read(void *ctx, uint16_t addr);
void    gcbus_write(void *ctx, uint16_t addr, uint8_t val);

#endif /* TIGERBYTE_GCBUS_H */
