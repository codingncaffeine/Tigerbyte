/*
 * Tigerbyte - full boot harness.
 *
 * Loads the internal + external BIOS (and optionally a cartridge) into the
 * real Game.com memory bus, then runs the SM8521 through it and traces
 * execution. This is the first point the CPU boots against the actual kernel
 * ROM with MMU paging, so the signature-compare loop can succeed.
 *
 *   boot_run <internal.bin> <external.bin> <count> [cart]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "cpu/sm8521.h"
#include "cpu/sm8521_disasm.h"
#include "sys/gcbus.h"

static gcbus_t bus;

int main(int argc, char **argv)
{
   if (argc < 4) {
      fprintf(stderr, "usage: %s <internal.bin> <external.bin> <count> [cart]\n", argv[0]);
      return 1;
   }
   gcbus_init(&bus);
   if (gcbus_load_internal(&bus, argv[1])) { fprintf(stderr, "error: bad internal ROM (need 4 KB)\n"); return 1; }
   if (gcbus_load_external(&bus, argv[2])) { fprintf(stderr, "error: bad external ROM (need 256 KB)\n"); return 1; }
   int count = atoi(argv[3]);
   if (argc > 4 && gcbus_load_cart(&bus, argv[4])) fprintf(stderr, "warning: cartridge load failed\n");

   sm8521_t c;
   sm8521_init(&c, gcbus_read, gcbus_write, &bus);
   sm8521_reset(&c);
   printf("reset PC=%04X\n\n", c.pc);

   for (int i = 0; i < count && !c.trapped; i++) {
      char dis[64];
      uint16_t pc = c.pc;
      sm8521_disasm(dis, sizeof dis, pc, gcbus_read, &bus);
      sm8521_step(&c);
      printf("%04X: %-22s PC=%04X SP=%04X PS1=%02X MMU=%02X.%02X.%02X.%02X\n",
             pc, dis, c.pc, c.sp, c.ps1,
             bus.ram[0x25], bus.ram[0x26], bus.ram[0x27], bus.ram[0x28]);
   }
   if (c.trapped)
      printf("\n** TRAP: unimplemented opcode %02X at PC=%04X **\n", c.trap_op, c.trap_pc);
   return 0;
}
