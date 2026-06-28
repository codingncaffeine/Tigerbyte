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
   for (int fr = 0; fr < frames; fr++) {
      gcsystem_run_frame(&sys);
      for (int i = 0; i < sys.audio_samples; i++) {
         w16(f, (uint16_t)sys.audio[i * 2]);
         w16(f, (uint16_t)sys.audio[i * 2 + 1]);
         nsamp++;
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
