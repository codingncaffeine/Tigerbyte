/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Headless boot-audio capture: boots the core (no cart) and dumps N frames of
 * audio to a 44.1 kHz stereo WAV, so the OS boot jingle can be measured against a
 * real-hardware recording for timing/pitch calibration.
 *
 *   audio_capture internal.bin external.bin out.wav <frames>
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "../src/sys/gcsystem.h"
#include "../src/cpu/sm8521_disasm.h"

static uint8_t isr_rd(void *ctx, uint16_t a) { return gcbus_read(ctx, a); }

static gcsystem_t sys;

static void w32(FILE *f, uint32_t v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); fputc((v >> 16) & 0xff, f); fputc((v >> 24) & 0xff, f); }
static void w16(FILE *f, uint16_t v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); }

int main(int argc, char **argv)
{
   if (argc < 5) { fprintf(stderr, "usage: %s internal.bin external.bin out.wav frames\n", argv[0]); return 1; }
   const char *ipath = argv[1], *epath = argv[2], *opath = argv[3];
   int frames = atoi(argv[4]);
   const int rate = 44100;

   gcsystem_init(&sys);
   if (getenv("TB_CLOCK")) {
      gcsystem_set_clock(&sys, atoi(getenv("TB_CLOCK")));
      fprintf(stderr, "[clock] fCK=%d Hz (%d cyc/frame)\n", sys.clock_hz, sys.cycles_per_frame);
   }
   if (gcsystem_load_internal(&sys, ipath) != 0) { fprintf(stderr, "load internal.bin failed\n"); return 1; }
   if (gcsystem_load_external(&sys, epath) != 0) { fprintf(stderr, "load external.bin failed\n"); return 1; }
   gcsystem_reset(&sys);

   FILE *f = fopen(opath, "wb");
   if (!f) { fprintf(stderr, "open %s failed\n", opath); return 1; }

   fwrite("RIFF", 1, 4, f); w32(f, 0); fwrite("WAVE", 1, 4, f);            /* sizes patched below */
   fwrite("fmt ", 1, 4, f); w32(f, 16); w16(f, 1); w16(f, 2); w32(f, rate);
   w32(f, rate * 2 * 2); w16(f, 4); w16(f, 16);
   fwrite("data", 1, 4, f); long datasize_pos = ftell(f); w32(f, 0);
   long datastart = ftell(f);

   long nsamp = 0;
   uint32_t last_dac = 0;
   for (int fr = 0; fr < frames; fr++) {
      gcsystem_run_frame(&sys);
      for (int i = 0; i < sys.audio_samples; i++) {
         w16(f, (uint16_t)sys.audio[i * 2]);
         w16(f, (uint16_t)sys.audio[i * 2 + 1]);
         nsamp++;
      }
      if (fr == 181 && getenv("TB_ISRDUMP")) {
         /* single-step until the TIM1 vector is taken, then trace the ISR with
            per-instruction cycle costs until IRET — the DAC pitch is set by
            this path's total, so its instruction mix is the calibration data */
         uint16_t handler = (uint16_t)((gcbus_read(&sys.bus, 0x1012) << 8) | gcbus_read(&sys.bus, 0x1013));
         fprintf(stderr, "[isr] TIM1 handler=%04X\n", handler);
         int guard = 2000000, tracing = 0, steps = 0, total = 0, passes = 0;
         while (guard-- > 0 && passes < 3) {
            uint16_t pc = sys.cpu.pc;
            uint32_t irq_before = sys.cpu.irq_taken;
            int tim1_pending = (sys.cpu.iflags >> 6) & 1;   /* SM_TIM1, before the step */
            char dis[64];
            uint8_t op = gcbus_read(&sys.bus, pc);
            if (tracing) sm8521_disasm(dis, sizeof dis, pc, isr_rd, &sys.bus);
            int cyc = sm8521_step(&sys.cpu);
            gcbus_tick(&sys.bus, cyc, sys.cpu.stopped);
            if (!tracing && tim1_pending && sys.cpu.irq_taken != irq_before) {
               /* this step vectored and ran the ISR's first instruction */
               tracing = 1; steps = 1; total = cyc;
               fprintf(stderr, "[isr] --- pass %d entry (vector+first instr = %d cyc) ---\n", passes + 1, cyc);
               continue;
            }
            if (tracing) {
               total += cyc; steps++;
               if (steps < 46)
                  fprintf(stderr, "[isr] %04X %-24s %2d  (sum %d)\n", pc, dis, cyc, total);
               if (op == 0xF9) {
                  fprintf(stderr, "[isr] --- iret: %d instr, %d cycles ---\n", steps, total);
                  tracing = 0; passes++;
               }
            }
         }
      }
      if (fr == 180 && sys.bus.dac_stream_n > 25) {   /* one mid-jingle frame: ISR cadence */
         fprintf(stderr, "[cadence] n=%d deltas:", sys.bus.dac_stream_n);
         for (int i = 1; i <= 24; i++)
            fprintf(stderr, " %u", sys.bus.dac_cycle[i] - sys.bus.dac_cycle[i - 1]);
         fprintf(stderr, "\n");
      }
      if ((fr % 30) == 29) {   /* twice/sec: the game's live TIM1 config + our DAC rate */
         uint32_t d = (sys.bus.snd_dac_writes - last_dac) * 2;  /* -> per second */
         last_dac = sys.bus.snd_dac_writes;
         fprintf(stderr, "[%4.1fs] TM1C=%02X TM1D=%02X SGC=%02X | dac/s=%u\n",
                 (fr + 1) / 59.73, sys.bus.ram[0x52], sys.bus.ram[0x53], sys.bus.ram[0x40], d);
      }
   }

   long dataend = ftell(f);
   uint32_t datasize = (uint32_t)(dataend - datastart);
   fseek(f, 4, SEEK_SET);            w32(f, 36 + datasize);
   fseek(f, datasize_pos, SEEK_SET); w32(f, datasize);
   fclose(f);
   fprintf(stderr, "wrote %s: %d frames, %ld samples (%.3fs @ %d Hz)\n",
           opath, frames, nsamp, nsamp / (double)rate, rate);
   return 0;
}
