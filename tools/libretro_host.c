/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Tigerbyte - minimal libretro host.
 *
 * Loads the built core DLL through the real libretro ABI (environment
 * callbacks, system-directory BIOS loading, video capture) and runs it for N
 * frames, then prints the captured frame as ASCII. Validates the core wrapper
 * end to end without a GUI frontend.
 *
 *   libretro_host <core.dll> <system_dir> <frames> [cart.tgc]
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

#include "libretro.h"

#define W 200
#define H 160

static uint32_t frame[W * H];
static unsigned frame_w, frame_h;
static const char *g_sysdir;

static void hlog(enum retro_log_level l, const char *fmt, ...)
{ va_list a; (void)l; va_start(a, fmt); vprintf(fmt, a); va_end(a); }

static bool env_cb(unsigned cmd, void *data)
{
   switch (cmd) {
   case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
   case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      *(const char **)data = g_sysdir; return true;
   case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
      ((struct retro_log_callback *)data)->log = hlog; return true;
   case RETRO_ENVIRONMENT_SET_MESSAGE:
      printf("[core msg] %s\n", ((const struct retro_message *)data)->msg); return true;
   case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
   case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
      return true;
   default:
      return false;
   }
}
static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
   frame_w = w; frame_h = h;
   if (!data || w > W || h > H) return;
   for (unsigned y = 0; y < h; y++)
      memcpy(&frame[y * W], (const uint8_t *)data + y * pitch, w * sizeof(uint32_t));
}
static size_t audio_batch_cb(const int16_t *d, size_t frames) { (void)d; return frames; }
static void   input_poll_cb(void) { }
static int16_t input_state_cb(unsigned port, unsigned dev, unsigned idx, unsigned id)
{ (void)port; (void)dev; (void)idx; (void)id; return 0; }

#define LD(sym) do { FARPROC p_ = GetProcAddress(dll, #sym); \
   if (!p_) { fprintf(stderr, "missing export: %s\n", #sym); return 1; } \
   memcpy(&sym, &p_, sizeof p_); } while (0)

int main(int argc, char **argv)
{
   if (argc < 4) { fprintf(stderr, "usage: %s <core.dll> <system_dir> <frames> [cart]\n", argv[0]); return 1; }
   g_sysdir = argv[2];
   int frames = atoi(argv[3]);

   HMODULE dll = LoadLibraryA(argv[1]);
   if (!dll) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }

   void (*retro_set_environment)(retro_environment_t);
   void (*retro_set_video_refresh)(retro_video_refresh_t);
   void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
   void (*retro_set_input_poll)(retro_input_poll_t);
   void (*retro_set_input_state)(retro_input_state_t);
   void (*retro_init)(void);
   bool (*retro_load_game)(const struct retro_game_info *);
   void (*retro_run)(void);
   LD(retro_set_environment); LD(retro_set_video_refresh); LD(retro_set_audio_sample_batch);
   LD(retro_set_input_poll); LD(retro_set_input_state); LD(retro_init);
   LD(retro_load_game); LD(retro_run);

   retro_set_environment(env_cb);
   retro_set_video_refresh(video_cb);
   retro_set_audio_sample_batch(audio_batch_cb);
   retro_set_input_poll(input_poll_cb);
   retro_set_input_state(input_state_cb);
   retro_init();

   struct retro_game_info game; memset(&game, 0, sizeof game);
   void *cart = NULL;
   if (argc > 4) {
      FILE *f = fopen(argv[4], "rb");
      if (f) {
         fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
         cart = malloc(n); fread(cart, 1, n, f); fclose(f);
         game.data = cart; game.size = (size_t)n; game.path = argv[4];
      }
   }
   if (!retro_load_game(argc > 4 ? &game : NULL)) {
      fprintf(stderr, "retro_load_game FAILED\n"); return 1;
   }
   for (int i = 0; i < frames; i++) retro_run();
   printf("ran %d frames; last video_cb frame %ux%u\n\n", frames, frame_w, frame_h);

   static const char map[5] = { '@', '#', '+', '.', ' ' };
   /* map XRGB back to shade by green channel (palette is monotonic in green) */
   static const uint32_t pal[5] = { 0x000000, 0x0F4F2F, 0x6F8F4F, 0x8FCF8F, 0xDFFF8F };
   for (int y = 0; y < H; y += 4) {
      for (int x = 0; x < W; x += 2) {
         uint32_t p = frame[y * W + x]; int best = 4, bd = 1 << 30;
         for (int s = 0; s < 5; s++) { int d = (int)((p & 0xFF) - (pal[s] & 0xFF)); if (d < 0) d = -d; if (d < bd) { bd = d; best = s; } }
         putchar(map[best]);
      }
      putchar('\n');
   }
   free(cart);
   return 0;
}
