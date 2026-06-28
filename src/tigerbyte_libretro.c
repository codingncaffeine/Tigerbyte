/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Tigerbyte - a Tiger Game.com libretro core.
 *
 * M0 scaffold: no emulation yet. This stage exists to prove the libretro
 * harness end-to-end and to LOCK the two things existing Game.com overlays
 * depend on:
 *   1. correct display geometry / aspect ratio (200x160), and
 *   2. the touchscreen exposed as a standard RETRO_DEVICE_POINTER.
 *
 * Everything below the "TODO(core)" markers is replaced as the SM8521 CPU,
 * memory map and LCD controller come online (milestones M2-M4).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "libretro.h"

/* ------- Tiger Game.com display geometry (spec-locked, M1 may refine AR) ------- */
#define GC_SCREEN_W 200
#define GC_SCREEN_H 160
/* Game.com LCD pixels are ~square -> 5:4. Refine once M1 confirms pixel ratio. */
#define GC_ASPECT   ((float)GC_SCREEN_W / (float)GC_SCREEN_H)
#define GC_CLOCK_HZ 4915200    /* 4.9152 MHz (XTAL 9.8304/2). NOT MAME's 5.5296 (off ~12.5%, issue #7303). */
#define GC_FPS      59.732155  /* MAME-confirmed LCD refresh; was a placeholder 60.0 */
#define GC_SAMPLE_RATE 44100.0

/* Authentic Game.com LCD palette (from MAME gamecom_palette), XRGB8888.
 * 5 shade levels 0..4: index 0 = darkest (black), 4 = lightest (warm LCD white).
 * Hardware is 2bpp; the LCDC[5:4] mode selects which 4 of these 5 the pixel
 * values map to -- that mapping lands in the PPU at M3. */
static const uint32_t gc_palette[5] = {
   0x00000000u, /* 0: black  */
   0x000f4f2fu, /* 1: gray 1 */
   0x006f8f4fu, /* 2: gray 2 */
   0x008fcf8fu, /* 3: gray 3 */
   0x00dfff8fu  /* 4: white (warm greenish LCD) */
};

static uint32_t framebuffer[GC_SCREEN_W * GC_SCREEN_H];

/* Per-frame audio scratch. Static => zero-initialised => silence for now.
 * Sized with generous headroom over one frame's samples (~738 at GC_FPS). */
#define GC_AUDIO_MAX 1024
static int16_t audio_frame[GC_AUDIO_MAX * 2]; /* interleaved stereo */

/* ------- libretro callbacks ------- */
static retro_video_refresh_t   video_cb;
static retro_audio_sample_t    audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t      input_poll_cb;
static retro_input_state_t     input_state_cb;
static retro_environment_t     environ_cb;
static retro_log_printf_t      log_cb;

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   va_list va;
   (void)level;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

/* ------- input state (stylus + buttons), fed to the CPU starting M5 ------- */
struct gc_input {
   int touch_active;
   int touch_x, touch_y;   /* in screen pixels, top-left origin */
   uint16_t buttons;       /* bitfield, see GC_BTN_* */
};
static struct gc_input input;

enum {
   GC_BTN_UP = 0, GC_BTN_DOWN, GC_BTN_LEFT, GC_BTN_RIGHT,
   GC_BTN_A, GC_BTN_B, GC_BTN_C, GC_BTN_D,
   GC_BTN_MENU, GC_BTN_PAUSE, GC_BTN_SOUND
};

/* RetroPad -> Game.com face buttons. Game.com has a d-pad + A/B/C/D + the
 * MENU / PAUSE / SOUND hardware keys. */
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

/* ------- libretro API ------- */

void retro_init(void)   { memset(&input, 0, sizeof(input)); }
void retro_deinit(void) { }

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "Tigerbyte";
   info->library_version  = "0.0.1";
   info->valid_extensions = "tgc|bin";   /* refine when M1 confirms cart format */
   info->need_fullpath    = false;
   info->block_extract    = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   /* These three lines are what every overlay/bezel aligns against. */
   info->geometry.base_width   = GC_SCREEN_W;
   info->geometry.base_height  = GC_SCREEN_H;
   info->geometry.max_width    = GC_SCREEN_W;
   info->geometry.max_height   = GC_SCREEN_H;
   info->geometry.aspect_ratio = GC_ASPECT;
   info->timing.fps            = GC_FPS;
   info->timing.sample_rate    = GC_SAMPLE_RATE;
}

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   /* Game.com has no second player; tell the frontend it's a 1-player machine
    * and that the cartridge is optional (so the BIOS menu can boot bare). */
   bool no_rom = true;
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_rom);
}

void retro_set_video_refresh(retro_video_refresh_t cb)       { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)         { audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb)            { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb)          { input_state_cb = cb; }

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port; (void)device;
}

void retro_reset(void) { /* TODO(core): reset CPU/LCD/RAM */ }

/* Read the stylus through the STANDARD pointer device so existing Game.com
 * input/bezel overlays map their touch hitboxes straight through. The pointer
 * API returns [-0x7fff, 0x7fff] across the active display; convert to pixels. */
static void poll_touch(void)
{
   int pressed = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);
   if (pressed)
   {
      int px = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
      int py = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
      input.touch_active = 1;
      input.touch_x = (int)(((int64_t)(px + 0x7fff) * GC_SCREEN_W) / 0xffff);
      input.touch_y = (int)(((int64_t)(py + 0x7fff) * GC_SCREEN_H) / 0xffff);
   }
   else
   {
      input.touch_active = 0;
   }
}

static void poll_buttons(void)
{
   uint16_t b = 0;
   #define MAP(id, bit) if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id)) b |= (1u << (bit))
   MAP(RETRO_DEVICE_ID_JOYPAD_UP,     GC_BTN_UP);
   MAP(RETRO_DEVICE_ID_JOYPAD_DOWN,   GC_BTN_DOWN);
   MAP(RETRO_DEVICE_ID_JOYPAD_LEFT,   GC_BTN_LEFT);
   MAP(RETRO_DEVICE_ID_JOYPAD_RIGHT,  GC_BTN_RIGHT);
   MAP(RETRO_DEVICE_ID_JOYPAD_B,      GC_BTN_A);
   MAP(RETRO_DEVICE_ID_JOYPAD_A,      GC_BTN_B);
   MAP(RETRO_DEVICE_ID_JOYPAD_Y,      GC_BTN_C);
   MAP(RETRO_DEVICE_ID_JOYPAD_X,      GC_BTN_D);
   MAP(RETRO_DEVICE_ID_JOYPAD_START,  GC_BTN_MENU);
   MAP(RETRO_DEVICE_ID_JOYPAD_SELECT, GC_BTN_PAUSE);
   MAP(RETRO_DEVICE_ID_JOYPAD_L,      GC_BTN_SOUND);
   #undef MAP
   input.buttons = b;
}

/* M0 placeholder picture: a moving 4-shade gradient with a 1px frame, plus a
 * crosshair under the stylus. Lets us drop a real bezel over it and verify the
 * screen sits exactly in the overlay's cutout. Deleted at M4. */
static void draw_test_pattern(void)
{
   static unsigned t = 0;
   t++;
   for (int y = 0; y < GC_SCREEN_H; y++)
   {
      for (int x = 0; x < GC_SCREEN_W; x++)
      {
         int shade = (((x + t) >> 5) + (y >> 5)) & 3;
         if (x == 0 || y == 0 || x == GC_SCREEN_W - 1 || y == GC_SCREEN_H - 1)
            shade = 0; /* black border: makes the screen rectangle obvious under a bezel */
         framebuffer[y * GC_SCREEN_W + x] = gc_palette[shade];
      }
   }
   if (input.touch_active &&
       input.touch_x >= 0 && input.touch_x < GC_SCREEN_W &&
       input.touch_y >= 0 && input.touch_y < GC_SCREEN_H)
   {
      for (int x = 0; x < GC_SCREEN_W; x++)
         framebuffer[input.touch_y * GC_SCREEN_W + x] = 0x00ff0000u;
      for (int y = 0; y < GC_SCREEN_H; y++)
         framebuffer[y * GC_SCREEN_W + input.touch_x] = 0x00ff0000u;
   }
}

void retro_run(void)
{
   input_poll_cb();
   poll_buttons();
   poll_touch();

   /* TODO(core): step the SM8521 for one frame; render the LCD into framebuffer. */
   draw_test_pattern();

   video_cb(framebuffer, GC_SCREEN_W, GC_SCREEN_H, GC_SCREEN_W * sizeof(uint32_t));

   /* TODO(core): generate real audio. Silence keeps the frontend's timing happy. */
   int frames = (int)(GC_SAMPLE_RATE / GC_FPS);
   if (frames > GC_AUDIO_MAX) frames = GC_AUDIO_MAX;
   audio_batch_cb(audio_frame, frames);
}

bool retro_load_game(const struct retro_game_info *game)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      if (log_cb) log_cb(RETRO_LOG_ERROR, "XRGB8888 not supported by frontend\n");
      return false;
   }

   struct retro_log_callback logging;
   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
      log_cb = logging.log;
   else
      log_cb = fallback_log;

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)input_desc);

   /* TODO(core): copy game->data (the cart ROM) into the cartridge space and
    * reset. For now we boot bare; game may be NULL (SUPPORT_NO_GAME). */
   (void)game;
   return true;
}

bool retro_load_game_special(unsigned t, const struct retro_game_info *i, size_t n)
{ (void)t; (void)i; (void)n; return false; }

void retro_unload_game(void) { /* TODO(core): free cart */ }

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

/* Save states / SRAM arrive once there's machine state to serialize (M4+). */
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *d, size_t s)   { (void)d; (void)s; return false; }
bool retro_unserialize(const void *d, size_t s) { (void)d; (void)s; return false; }

void   retro_cheat_reset(void) { }
void   retro_cheat_set(unsigned i, bool e, const char *c) { (void)i; (void)e; (void)c; }

void  *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
