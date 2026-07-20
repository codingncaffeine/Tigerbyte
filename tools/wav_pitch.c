/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Tigerbyte - crude dominant-pitch probe for audio calibration.
 *
 * Splits [t0,t1] of a 16-bit PCM WAV into 100 ms windows, estimates each
 * window's dominant frequency by positive-going zero-crossing rate (fine for
 * the boot jingle's near-tonal content), and prints the median and quartiles.
 * Comparing the same musical passage between the real-hardware recording and
 * an emulated capture yields the DAC-rate ratio directly.
 *
 *   wav_pitch <file.wav> <t0> <t1> [min_amp]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static int cmpd(const void *a, const void *b)
{
   double x = *(const double *)a, y = *(const double *)b;
   return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
   if (argc < 4) { fprintf(stderr, "usage: %s <file.wav> <t0> <t1> [min_amp] [lp_taps]\n", argv[0]); return 1; }
   double t0 = atof(argv[2]), t1 = atof(argv[3]);
   int min_amp = (argc > 4) ? atoi(argv[4]) : 1200;
   int lp = (argc > 5) ? atoi(argv[5]) : 1;      /* box lowpass width (1 = off) */

   FILE *f = fopen(argv[1], "rb");
   if (!f) { perror(argv[1]); return 1; }
   uint8_t hdr[44];
   if (fread(hdr, 1, 44, f) != 44) return 1;
   int ch = hdr[22] | (hdr[23] << 8);
   long rate = hdr[24] | ((long)hdr[25] << 8) | ((long)hdr[26] << 16) | ((long)hdr[27] << 24);

   long from = (long)(t0 * rate), to = (long)(t1 * rate);
   long win = rate / 10;
   double freqs[4096]; int nf = 0;

   fseek(f, 44 + from * 2L * ch, SEEK_SET);
   int16_t *buf = malloc((size_t)win * 2 * ch);
   for (long w = from; w + win <= to && nf < 4096; w += win) {
      if (fread(buf, 2 * ch, (size_t)win, f) != (size_t)win) break;
      if (lp > 1) {                     /* box lowpass: kills resampler imaging */
         for (long i = 0; i + lp < win; i++) {
            long acc = 0;
            for (int k = 0; k < lp; k++) acc += buf[(i + k) * ch];
            buf[i * ch] = (int16_t)(acc / lp);
         }
      }
      int peak = 0;
      for (long i = 0; i < win; i++) {
         int a = buf[i * ch] < 0 ? -buf[i * ch] : buf[i * ch];
         if (a > peak) peak = a;
      }
      if (peak < min_amp) continue;
      /* autocorrelation fundamental in 80..2500 Hz — robust against the strong
         step harmonics of the real DAC that fool a zero-crossing count.
         Mean-removed and overlap-normalized, else DC pins the peak at min lag. */
      double mean = 0;
      for (long i = 0; i < win; i++) mean += buf[i * ch];
      mean /= win;
      long lag_min = rate / 2500, lag_max = rate / 80;
      double best = 0; long best_lag = 0;
      double norm0 = 0;
      for (long i = 0; i < win; i++) {
         double s = buf[i * ch] - mean;
         norm0 += s * s;
      }
      if (norm0 <= 0) continue;
      norm0 /= win;
      for (long lag = lag_min; lag <= lag_max && lag < win / 2; lag++) {
         double acc = 0; long cnt = 0;
         for (long i = 0; i + lag < win; i += 2) {        /* stride 2: 2x faster, same peak */
            acc += ((double)buf[i * ch] - mean) * ((double)buf[(i + lag) * ch] - mean);
            cnt++;
         }
         if (cnt) acc /= cnt;
         if (acc > best) { best = acc; best_lag = lag; }
      }
      if (best_lag > 0 && best > 0.3 * norm0)
         freqs[nf++] = (double)rate / best_lag;
   }
   if (!nf) { printf("%s: no tonal windows in range\n", argv[1]); return 1; }
   qsort(freqs, (size_t)nf, sizeof(double), cmpd);
   printf("%s [%0.2f..%0.2fs]: %d windows  p25=%.0f  median=%.0f Hz  p75=%.0f\n",
          argv[1], t0, t1, nf, freqs[nf / 4], freqs[nf / 2], freqs[3 * nf / 4]);
   return 0;
}
