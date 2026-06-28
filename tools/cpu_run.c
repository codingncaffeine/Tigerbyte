/*
 * Tigerbyte - CPU execution spike.
 *
 * Loads a raw binary into a flat 64 KB space, resets the SM8521 core, and
 * single-steps it, printing the disassembly plus key register state per
 * instruction. Used to watch the BIOS actually execute (stack-pointer init,
 * MMU setup, the signature-compare loop) before the full system bus exists.
 *
 *   cpu_run <binfile> <baseHex> <count>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "cpu/sm8521.h"
#include "cpu/sm8521_disasm.h"

static uint8_t mem[0x10000];

static uint8_t rd(void *ctx, uint16_t a) { (void)ctx; return mem[a]; }
static void    wr(void *ctx, uint16_t a, uint8_t v) { (void)ctx; mem[a] = v; }

int main(int argc, char **argv)
{
   if (argc < 4) { fprintf(stderr, "usage: %s <binfile> <baseHex> <count>\n", argv[0]); return 1; }
   const char *path = argv[1];
   unsigned base = (unsigned)strtoul(argv[2], NULL, 16) & 0xFFFF;
   int count = atoi(argv[3]);

   FILE *f = fopen(path, "rb");
   if (!f) { perror(path); return 1; }
   size_t n = fread(mem + base, 1, sizeof(mem) - base, f); (void)n;
   fclose(f);

   sm8521_t c;
   sm8521_init(&c, rd, wr, NULL);
   sm8521_reset(&c);
   printf("reset: PC=%04X\n\n", c.pc);

   for (int i = 0; i < count && !c.trapped; i++) {
      char dis[64];
      uint16_t pc = c.pc;
      sm8521_disasm(dis, sizeof dis, pc, rd, NULL);
      sm8521_step(&c);
      uint8_t r0 = c.reg[(c.ps0 & 0xF8) + 0], r1 = c.reg[(c.ps0 & 0xF8) + 1];
      printf("%04X: %-22s -> PC=%04X SP=%04X PS0=%02X PS1=%02X r0=%02X r1=%02X\n",
             pc, dis, c.pc, c.sp, c.ps0, c.ps1, r0, r1);
   }

   if (c.trapped)
      printf("\n** TRAP: unimplemented opcode %02X at PC=%04X **\n", c.trap_op, c.trap_pc);

   printf("\nstate: SYS(19)=%02X  SP=%04X  MMU1-4(25-28)=%02X %02X %02X %02X\n",
          mem[0x19], c.sp, mem[0x25], mem[0x26], mem[0x27], mem[0x28]);
   return 0;
}
