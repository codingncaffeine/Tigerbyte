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

/* Direct-to-file debug log next to the system dir — independent of the frontend's
   log plumbing (whose printf formatter only handles a few varargs). Path set at load. */
static char g_logpath[600];
static void tb_log(const char *fmt, ...)
{
   FILE *f; va_list va;
   if (!g_logpath[0]) return;
   f = fopen(g_logpath, "a");
   if (!f) return;
   va_start(va, fmt); vfprintf(f, fmt, va); va_end(va);
   fclose(f);
}

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
   info->library_version  = "0.3.14";
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

   /* The SDK's delay loops prove 4.9152 MHz for CPU-visible timing, but real
      boot recordings pace the whole machine ~30% faster than the model at that
      clock — either the console really runs the part past its rating or the
      unanchored opcode costs are collectively high. The calibrated default
      matches the recordings' tempo; the fine steps let the last percent be
      tuned by ear, and the nominal/MAME values remain for measurement work. */
   static const struct retro_variable vars[] = {
      { "tigerbyte_clock",
        "System clock; 6.30 MHz (calibrated)|6.25 MHz|6.20 MHz|6.35 MHz|6.40 MHz|6.5536 MHz|original 4.92 MHz|mame 5.53 MHz" },
      { NULL, NULL }
   };
   cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)vars);
}

static void apply_clock_option(void)
{
   struct retro_variable var = { "tigerbyte_clock", NULL };
   int hz = 6300000;
   if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      float mhz = 0.0f;
      if      (var.value[0] == 'o') hz = 4915200;
      else if (var.value[0] == 'm') hz = 5529600;
      else if (sscanf(var.value, "%f", &mhz) == 1 && mhz > 1.0f && mhz < 20.0f)
         hz = (int)(mhz * 1000000.0f + 0.5f);
   }
   gcsystem_set_clock(&sys, hz);
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
      bool updated = false;
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
         apply_clock_option();
      poll_input();
      gcsystem_run_frame(&sys);
      gc_render(&sys.bus, framebuffer);
   } else {
      memset(framebuffer, 0, sizeof framebuffer);
   }

   video_cb(framebuffer, GC_W, GC_H, GC_W * sizeof(uint32_t));

   if (system_ready) {
      audio_batch_cb(sys.audio, sys.audio_samples);

      /* --- sound debug: a once-per-second summary through the frontend log --- */
      {
         static unsigned dbg_frames = 0;
         static uint32_t last_reg = 0, last_dac = 0, last_wave = 0;
         if (++dbg_frames >= 60) {
            const uint8_t *r = sys.bus.ram;
            uint32_t reg = sys.bus.snd_reg_writes - last_reg;
            uint32_t dac = sys.bus.snd_dac_writes - last_dac;
            uint32_t wav = sys.bus.snd_wave_writes - last_wave;
            int peak = 0, i;
            last_reg = sys.bus.snd_reg_writes;
            last_dac = sys.bus.snd_dac_writes;
            last_wave = sys.bus.snd_wave_writes;
            for (i = 0; i < sys.audio_samples; i++) {
               int v = sys.audio[i * 2]; if (v < 0) v = -v; if (v > peak) peak = v;
            }
            tb_log("[snd] SGC=%02X periods=%03X/%03X lvls=%02X/%02X SG2=%03X/%02X DAC=%02X TM1=%02X/%02X | writes/s ctrl=%u dac=%u wave=%u | peak=%d\n",
               r[0x40], ((r[0x46] << 8) | r[0x47]) & 0xFFF, ((r[0x48] << 8) | r[0x49]) & 0xFFF,
               r[0x42] & 0x1F, r[0x44] & 0x1F,
               ((r[0x4C] << 8) | r[0x4D]) & 0xFFF, r[0x4A] & 0x1F, r[0x4E],
               r[0x52], sys.bus.timer[1].reload,
               reg, dac, wav, peak);
            {  /* display state — catches a black screen (display off / empty page) */
               const uint8_t *v = r + 0xA000;
               uint8_t lcdc = r[0x30];
               int nz0 = 0, nz1 = 0, k;
               for (k = 0; k < 0x2000; k++) { if (v[k]) nz0++; if (v[k + 0x2000]) nz1++; }
               tb_log("[vid] LCDC=%02X en=%d page=%d mode=%d | vram nz: p0=%d p1=%d\n",
                      lcdc, (lcdc & 0x80) ? 1 : 0, (lcdc & 0x40) ? 1 : 0, (lcdc >> 4) & 3, nz0, nz1);
            }
            dbg_frames = 0;
         }
      }
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

   if (sysdir && sysdir[0])
      snprintf(g_logpath, sizeof g_logpath, "%s/tigerbyte.log", sysdir);
   tb_log("=== load_game: sysdir='%s' ===\n", sysdir ? sysdir : "(null)");

   gcsystem_init(&sys);
   apply_clock_option();
   if (!load_bios(sysdir)) {
      tb_log("load_bios FAILED — system ROMs not found\n");
      struct retro_message msg = { "Tigerbyte: missing system ROMs (internal.bin + external.bin) in the system directory", 360 };
      log_cb(RETRO_LOG_ERROR, "Tigerbyte: system ROMs not found in '%s'\n", sysdir ? sysdir : "(null)");
      environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
      return false;
   }
   if (game && game->data && game->size)
      gcsystem_load_cart_mem(&sys, (const uint8_t *)game->data, game->size);

   gcsystem_reset(&sys);
   system_ready = 1;
   tb_log("loaded OK: cart_size=%lu system_ready=%d build=0.3.14 clock=%d\n",
          (unsigned long)(game ? game->size : 0), system_ready, sys.clock_hz);
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
/* The console's external SRAM (0xE000-0xFFFF) is battery-backed: high-score
   tables, player names, and the OS apps' data (phone book, calendar) live
   there and persist across power cycles. Exposing it as save RAM lets the
   frontend do the battery's job — without it, games re-run their first-boot
   name-entry flows on every launch. */
void *retro_get_memory_data(unsigned id)
{
   if (id == RETRO_MEMORY_SAVE_RAM && system_ready) return &sys.bus.ram[0xE000];
   return NULL;
}
size_t retro_get_memory_size(unsigned id)
{
   return (id == RETRO_MEMORY_SAVE_RAM) ? 0x2000 : 0;
}
