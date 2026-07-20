/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Tigerbyte - WAV activity mapper for audio calibration.
 *
 * Prints the active segments (|sample| above threshold, merged across gaps
 * shorter than 0.5 s) of a 16-bit PCM WAV, so the emulated boot jingle's
 * length can be compared against the real-hardware reference recording.
 *
 *   wav_span <file.wav> [threshold]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(int argc, char **argv)
{
   if (argc < 2) { fprintf(stderr, "usage: %s <file.wav> [threshold]\n", argv[0]); return 1; }
   int thr = (argc > 2) ? atoi(argv[2]) : 500;

   FILE *f = fopen(argv[1], "rb");
   if (!f) { perror(argv[1]); return 1; }
   uint8_t hdr[44];
   if (fread(hdr, 1, 44, f) != 44) { fprintf(stderr, "short header\n"); return 1; }
   int ch   = hdr[22] | (hdr[23] << 8);
   long rate = hdr[24] | ((long)hdr[25] << 8) | ((long)hdr[26] << 16) | ((long)hdr[27] << 24);
   int bits = hdr[34] | (hdr[35] << 8);
   if (bits != 16 || ch < 1 || ch > 2) { fprintf(stderr, "need 16-bit mono/stereo PCM\n"); return 1; }

   long gap_limit = rate / 2;                     /* merge activity across <0.5 s gaps */
   long n = 0, seg_start = -1, last_active = -1;
   long total_active_first = -1, total_active_last = -1;
   int16_t buf[4096];
   size_t got;
   printf("%s: %ld Hz, %d ch\n", argv[1], rate, ch);
   while ((got = fread(buf, 2, 4096, f)) > 0) {
      for (size_t i = 0; i < got; i += (size_t)ch) {
         int v = buf[i]; if (v < 0) v = -v;
         if (v > thr) {
            if (seg_start < 0) seg_start = n;
            else if (n - last_active > gap_limit) {
               printf("  segment %8.3fs .. %8.3fs  (%.3fs)\n", seg_start / (double)rate,
                      last_active / (double)rate, (last_active - seg_start) / (double)rate);
               seg_start = n;
            }
            last_active = n;
            if (total_active_first < 0) total_active_first = n;
            total_active_last = n;
         }
         n++;
      }
   }
   if (seg_start >= 0)
      printf("  segment %8.3fs .. %8.3fs  (%.3fs)\n", seg_start / (double)rate,
             last_active / (double)rate, (last_active - seg_start) / (double)rate);
   if (total_active_first >= 0)
      printf("  overall active %.3fs .. %.3fs  span %.3fs\n",
             total_active_first / (double)rate, total_active_last / (double)rate,
             (total_active_last - total_active_first) / (double)rate);
   fclose(f);
   return 0;
}
