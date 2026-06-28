/*
 * Tigerbyte - CPU decode spike.
 *
 * Standalone test harness for the SM8521 decoder: loads a raw binary into a
 * flat 64 KB address space at a given base, then disassembles a run of
 * instructions from a start address. Used to validate decoding (instruction
 * lengths + operands) against the real BIOS / cartridge code before the
 * executing core exists.
 *
 *   cpu_spike <binfile> <baseHex> <startHex> <count>
 *   e.g.  cpu_spike internal.bin 1000 1020 48
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "cpu/sm8521_disasm.h"

static uint8_t mem[0x10000];

static uint8_t fetch(void *ctx, uint16_t addr) { (void)ctx; return mem[addr]; }

int main(int argc, char **argv)
{
   if (argc < 5) {
      fprintf(stderr, "usage: %s <binfile> <baseHex> <startHex> <count>\n", argv[0]);
      return 1;
   }
   const char  *path  = argv[1];
   unsigned     base  = (unsigned)strtoul(argv[2], NULL, 16) & 0xFFFF;
   unsigned     start = (unsigned)strtoul(argv[3], NULL, 16) & 0xFFFF;
   int          count = atoi(argv[4]);

   FILE *f = fopen(path, "rb");
   if (!f) { perror(path); return 1; }
   size_t n = fread(mem + base, 1, sizeof(mem) - base, f);
   fclose(f);
   printf("loaded %zu bytes at $%04X from %s\n\n", n, base, path);

   uint16_t pc = (uint16_t)start;
   for (int i = 0; i < count; i++) {
      char buf[80];
      int len = sm8521_disasm(buf, sizeof buf, pc, fetch, NULL);

      printf("%04X:  ", pc);
      for (int b = 0; b < len; b++) printf("%02X ", mem[(uint16_t)(pc + b)]);
      for (int b = len; b < 5; b++) printf("   ");
      printf(" %s\n", buf);

      pc = (uint16_t)(pc + len);
   }
   return 0;
}
