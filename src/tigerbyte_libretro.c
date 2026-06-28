/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Tigerbyte - a Tiger Game.com libretro core.
 *
 * Runs the full system (SM8521 CPU + bus + PPU + timers) a frame at a time,
 * maps the RetroPad to the console's keys and RETRO_DEVICE_POINTER to the
 * resistive touch grid, and renders the LCD to XRGB8888. The two system ROMs
 * are loaded from the frontend's system directory.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "libretro.h"
#include "sys/gcsystem.h"
#include "sys/ppu.h"

#define GC_ASPECT      ((float)GC_W / (float)GC_H)   /* 200x160 ~ 5:4 */
#define GC_FPS         59.732155
#define GC_SAMPLE_RATE 44100.0
#define GC_AUDIO_MAX   1024

static gcsystem_t sys;
static int        system_ready;
static uint32_t   framebuffer[GC_W * GC_H];
static int16_t    audio_frame[GC_AUDIO_MAX * 2];     /* silence until Phase 11 */

static retro_video_refresh_t      video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;
static retro_environment_t        environ_cb;
static retro_log_printf_t         log_cb;

static void fallback_log(enum retro_log_level lvl, const char *fmt, ...)
{ va_list va; (void)lvl; va_start(va, fmt); vfprintf(stderr, fmt, va); va_end(va); }

/* RetroPad -> Game.com keys (Game.com: d-pad, A/B/C/D, Menu, Pause, Sound). */
static const struct retro_input_descriptor input_desc[] = {
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "Up" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "Down" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "Left" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "C" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "D" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Menu" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,"Pause" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Sound" },
   { 0 }
};

/* ---- libretro boilerplate ---- */
void retro_init(void)   { system_ready = 0; }
void retro_deinit(void) { system_ready = 0; }
unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "Tigerbyte";
   info->library_version  = "0.2.0";
   info->valid_extensions = "tgc|bin";
   info->need_fullpath    = false;     /* deliver the cart image in game->data */
   info->block_extract    = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->geometry.base_width   = GC_W;
   info->geometry.base_height  = GC_H;
   info->geometry.max_width    = GC_W;
   info->geometry.max_height   = GC_H;
   info->geometry.aspect_ratio = GC_ASPECT;
   info->timing.fps            = GC_FPS;
   info->timing.sample_rate    = GC_SAMPLE_RATE;
}

void retro_set_environment(retro_environment_t cb)
{
   bool no_rom = true;
   environ_cb = cb;
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_rom);  /* may boot to the shell */
}

void retro_set_video_refresh(retro_video_refresh_t cb)           { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)             { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb)                 { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb)               { input_state_cb = cb; }
void retro_set_controller_port_device(unsigned p, unsigned d)    { (void)p; (void)d; }

void retro_reset(void) { if (system_ready) gcsystem_reset(&sys); }

/* ---- input ---- */
static void poll_input(void)
{
   uint8_t in0 = 0xFF, in1 = 0xFF, in2 = 0xFF;
   #define HELD(id) input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id)
   if (HELD(RETRO_DEVICE_ID_JOYPAD_UP))     in0 &= (uint8_t)~0x01;
   if (HELD(RETRO_DEVICE_ID_JOYPAD_DOWN))   in0 &= (uint8_t)~0x02;
   if (HELD(RETRO_DEVICE_ID_JOYPAD_LEFT))   in0 &= (uint8_t)~0x04;
   if (HELD(RETRO_DEVICE_ID_JOYPAD_RIGHT))  in0 &= (uint8_t)~0x08;
   if (HELD(RETRO_DEVICE_ID_JOYPAD_START))  in0 &= (uint8_t)~0x10;   /* Menu  */
   if (HELD(RETRO_DEVICE_ID_JOYPAD_SELECT)) in0 &= (uint8_t)~0x20;   /* Pause */
   if (HELD(RETRO_DEVICE_ID_JOYPAD_L))      in0 &= (uint8_t)~0x40;   /* Sound */
   if (HELD(RETRO_DEVICE_ID_JOYPAD_B))      in0 &= (uint8_t)~0x80;   /* A */
   if (HELD(RETRO_DEVICE_ID_JOYPAD_A))      in1 &= (uint8_t)~0x01;   /* B */
   if (HELD(RETRO_DEVICE_ID_JOYPAD_Y))      in1 &= (uint8_t)~0x02;   /* C */
   if (HELD(RETRO_DEVICE_ID_JOYPAD_X))      in2 &= (uint8_t)~0x02;   /* D */
   #undef HELD
   gcbus_set_buttons(&sys.bus, in0, in1, in2);

   for (int c = 0; c < 13; c++) gcbus_set_touch(&sys.bus, c, 0);
   if (input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED)) {
      int px = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
      int py = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
      int sx = (int)(((int64_t)(px + 0x7fff) * GC_W) / 0xffff);
      int sy = (int)(((int64_t)(py + 0x7fff) * GC_H) / 0xffff);
      if (sx >= 0 && sx < GC_W && sy >= 0 && sy < GC_H) {
         int col = sx * 13 / GC_W, row = sy * 10 / GC_H;     /* pixel -> touch zone */
         gcbus_set_touch(&sys.bus, col, (uint16_t)(1 << row));
      }
   }
}

void retro_run(void)
{
   input_poll_cb();

   if (system_ready) {
      poll_input();
      gcsystem_run_frame(&sys);
      gc_render(&sys.bus, framebuffer);
   } else {
      memset(framebuffer, 0, sizeof framebuffer);
   }

   video_cb(framebuffer, GC_W, GC_H, GC_W * sizeof(uint32_t));

   if (system_ready) {
      audio_batch_cb(sys.audio, sys.audio_samples);
   } else {
      int frames = (int)(GC_SAMPLE_RATE / GC_FPS);
      if (frames > GC_AUDIO_MAX) frames = GC_AUDIO_MAX;
      audio_batch_cb(audio_frame, frames);
   }
}

/* ---- BIOS loading from the system directory ---- */
static int load_one(int (*fn)(gcsystem_t *, const char *), const char *dir, const char *name)
{
   char path[1280];
   snprintf(path, sizeof path, "%s/%s", dir, name);
   return fn(&sys, path) == 0;
}
static int load_bios(const char *dir)
{
   const char *names_i[] = { "internal.bin", "gamecom/internal.bin", "gamecom_internal.bin" };
   const char *names_e[] = { "external.bin", "gamecom/external.bin", "gamecom_external.bin" };
   int got_i = 0, got_e = 0;
   if (!dir) dir = ".";
   for (unsigned k = 0; k < 3 && !got_i; k++) got_i = load_one(gcsystem_load_internal, dir, names_i[k]);
   for (unsigned k = 0; k < 3 && !got_e; k++) got_e = load_one(gcsystem_load_external, dir, names_e[k]);
   return got_i && got_e;
}

bool retro_load_game(const struct retro_game_info *game)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   struct retro_log_callback logging;
   const char *sysdir = NULL;

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) return false;
   log_cb = environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging) ? logging.log : fallback_log;
   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)input_desc);
   environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sysdir);

   gcsystem_init(&sys);
   if (!load_bios(sysdir)) {
      struct retro_message msg = { "Tigerbyte: missing system ROMs (internal.bin + external.bin) in the system directory", 360 };
      log_cb(RETRO_LOG_ERROR, "Tigerbyte: system ROMs not found in '%s'\n", sysdir ? sysdir : "(null)");
      environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
      return false;
   }
   if (game && game->data && game->size)
      gcsystem_load_cart_mem(&sys, (const uint8_t *)game->data, game->size);

   gcsystem_reset(&sys);
   system_ready = 1;
   return true;
}

bool retro_load_game_special(unsigned t, const struct retro_game_info *i, size_t n)
{ (void)t; (void)i; (void)n; return false; }

void retro_unload_game(void) { system_ready = 0; }

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

/* Save states arrive once the machine state is split from the host pointers. */
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *d, size_t s)         { (void)d; (void)s; return false; }
bool retro_unserialize(const void *d, size_t s) { (void)d; (void)s; return false; }

void   retro_cheat_reset(void) { }
void   retro_cheat_set(unsigned i, bool e, const char *c) { (void)i; (void)e; (void)c; }
void  *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
