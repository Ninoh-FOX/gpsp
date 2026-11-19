/*
 * gpSP standalone for Koriki (Miyoo Mini).
 *
 * The picogpsp frontend (menu, options, scaling, key config, libpicofe) driving
 * the current libretro gpSP core through the libretro API, statically linked:
 * the core is libretro/gpsp as is, so it can be updated without touching this.
 *
 * Same file names and behaviour as picogpsp: <rom>.sav, <rom>.st0..9 and
 * <rom>.cfg in $HOME/.picogpsp/, picogpsp.cfg for the global options. With
 * KORIKI_SAVE_BASE set (the launcher does it) the save is that path + .srm,
 * the file of RetroArch's gpSP. The
 * options of the frontend menu are handed to the core as libretro options.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include "libretro.h"
#include "frontend/main.h"
#include "frontend/menu.h"
#include "frontend/plat.h"
#include "frontend/libpicofe/plat.h"
#include "frontend/libpicofe/input.h"
#include "frontend/net.h"
#include "frontend/menurun.h"

/* Same as picogpsp's HUD_LEN (plat.h may already define it) */
#ifndef HUD_LEN
#define HUD_LEN 41
#endif

int should_quit = 0;

int dynarec_enable;
int state_slot;
frameskip_style_t frameskip_style;
scaling_mode_t scaling_mode;
int max_frameskip;
int color_correct;
int lcd_blend;
int show_fps;
int limit_frames;
int sound_rate_high = 1;
int turbo_frames = 4;  /* gpsp_turbo_period: frames of each turbo A/B cycle */
int link_mode;         /* gpsp_serial, see link_mode_values */
int link_players = 2;  /* gpsp_cable_players */
int border_style = 1;  /* Koriki: GBA border */
int grid_mode;         /* Koriki: grid off */

/* kept for scale.c (its own post-processing is not used: the core does it) */
uint16_t *gba_screen_pixels_prev = NULL;
uint16_t *gba_processed_pixels = NULL;

const uint16_t *koriki_frame = NULL;
unsigned koriki_frame_pitch = 240;
unsigned koriki_audio_in_rate = 65536;

static char rom_path[MAXPATHLEN];
static char system_dir[MAXPATHLEN];
static int options_changed = 1;
static retro_audio_buffer_status_callback_t audio_status_cb = NULL;
static uint32_t player_keys;         /* KBIT_* from libpicofe */
static int frame_rendered;

static float vsyncsps = 0.0, rendersps = 0.0;

void koriki_options_changed(void) { options_changed = 1; }

/* the menu entries of "Link mode", in this order */
const char *const link_mode_values[] = {
  "auto", "disabled", "rfu", "mul_poke", "mul_aw1", "mul_aw2", "mul_cable", NULL
};

/* ---------------------------------------------------------------- messages */

/* core and netplay messages, shown at the bottom (over the FPS) */
static char hud_msg[HUD_LEN];
static unsigned hud_msg_until;

void net_message(const char *text, unsigned ms)
{
  snprintf(hud_msg, sizeof(hud_msg), "%s", text);
  hud_msg_until = plat_get_ticks_ms() + ms;
  printf("gpsp: %s\n", text);
}

/* ---------------------------------------------------------------- names */

static void gamepak_related_name(char *buf, size_t len, const char *new_extension)
{
  char root_dir[MAXPATHLEN];
  char filename[MAXPATHLEN];
  char *p;

  plat_get_root_dir(root_dir, sizeof(root_dir));
  p = strrchr(rom_path, '/');
  p = p ? p + 1 : rom_path;
  strncpy(filename, p, sizeof(filename));
  filename[sizeof(filename) - 1] = 0;
  p = strrchr(filename, '.');
  if (p)
    *p = 0;
  snprintf(buf, len, "%s%s%s", root_dir, filename, new_extension);
}

void state_file_name(char *buf, size_t len, unsigned slot)
{
  char ext[20];
  snprintf(ext, sizeof(ext), ".st%d", slot);
  gamepak_related_name(buf, len, ext);
}

void config_file_name(char *buf, size_t len, int is_game)
{
  char root_dir[MAXPATHLEN];

  if (is_game) {
    gamepak_related_name(buf, len, ".cfg");
  } else {
    plat_get_root_dir(root_dir, MAXPATHLEN);
    snprintf(buf, len, "%s%s", root_dir, "picogpsp.cfg");
  }
}

/* ---------------------------------------------------------------- backup */

static uint8_t *saved_copy;   /* last contents written to <rom>.sav */

/* $KORIKI_SAVE_BASE.srm (RetroArch's), or <rom>.sav in $HOME/.picogpsp/ */
static void backup_name(char *buf, size_t len)
{
  const char *base = getenv("KORIKI_SAVE_BASE");

  if (base && *base)
    snprintf(buf, len, "%s.srm", base);
  else
    gamepak_related_name(buf, len, ".sav");
}

static void backup_load(void)
{
  char name[MAXPATHLEN];
  uint8_t *mem = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
  size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
  FILE *f;

  if (!mem || !size)
    return;
  backup_name(name, sizeof(name));
  f = fopen(name, "rb");
  if (f) {
    if (fread(mem, 1, size, f) == 0)
      printf("gpsp: empty save %s\n", name);
    fclose(f);
  }
  saved_copy = malloc(size);
  if (saved_copy)
    memcpy(saved_copy, mem, size);
}

/* write <rom>.sav when the game changed its backup memory */
static void update_backup(void)
{
  char name[MAXPATHLEN];
  uint8_t *mem = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
  size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
  FILE *f;

  if (!mem || !size || (saved_copy && !memcmp(saved_copy, mem, size)))
    return;
  backup_name(name, sizeof(name));
  f = fopen(name, "wb");
  if (!f)
    return;
  if (fwrite(mem, 1, size, f) == size && saved_copy)
    memcpy(saved_copy, mem, size);
  fclose(f);
  sync();
}

/* ---------------------------------------------------------------- states */

int save_state_file(unsigned slot)
{
  char name[MAXPATHLEN];
  size_t size = retro_serialize_size();
  void *data;
  FILE *f;
  int ret = -1;

  state_file_name(name, sizeof(name), slot);
  data = calloc(1, size);
  if (!data)
    return -1;
  if (retro_serialize(data, size) && (f = fopen(name, "wb"))) {
    if (fwrite(data, 1, size, f) == size)
      ret = 0;
    fclose(f);
  }
  free(data);
  return ret;
}

int load_state_file(unsigned slot)
{
  char name[MAXPATHLEN];
  size_t size = retro_serialize_size();
  void *data;
  FILE *f;
  int ret = -1;

  state_file_name(name, sizeof(name), slot);
  f = fopen(name, "rb");
  if (!f)
    return -1;
  data = calloc(1, size);
  if (data && fread(data, 1, size, f) == size && retro_unserialize(data, size))
    ret = 0;
  free(data);
  fclose(f);
  return ret;
}

/* ---------------------------------------------------------------- actions */

static void toggle_fast_forward(int force_off)
{
  static int limit_frames_was;
  static int fast_forward;

  if (force_off && !fast_forward)
    return;
  fast_forward = !fast_forward;
  if (fast_forward) {
    limit_frames_was = limit_frames;
    limit_frames = 0;
  } else {
    limit_frames = limit_frames_was;
  }
}

static int menu_requested;     /* MENU pressed: the menu opens after the frame */

void handle_emu_action(emu_action action)
{
  static frameskip_style_t prev_frameskip_style;
  static emu_action prev_action = EACTION_NONE;
  if (prev_action != EACTION_NONE && prev_action == action) return;

  switch (action)
  {
    case EACTION_NONE:
      break;
    case EACTION_QUIT:
      should_quit = 1;
      break;
    case EACTION_TOGGLE_FPS:
      show_fps = !show_fps;
      plat_video_set_msg(" ");
      break;
    case EACTION_SAVE_STATE:
      save_state_file(0);
      break;
    case EACTION_LOAD_STATE:
      load_state_file(0);
      break;
    case EACTION_TOGGLE_FSKIP:
      if (prev_frameskip_style == FRAMESKIP_NONE)
        prev_frameskip_style = FRAMESKIP_AUTO;
      if (frameskip_style == FRAMESKIP_NONE) {
        frameskip_style = prev_frameskip_style;
      } else {
        prev_frameskip_style = frameskip_style;
        frameskip_style = FRAMESKIP_NONE;
      }
      options_changed = 1;
      break;
    case EACTION_TOGGLE_FF:
      toggle_fast_forward(0);
      break;
    case EACTION_MENU:
      /* Koriki: opened by the main loop once the frame is over (this runs
         inside retro_run(), and the game behind the menu needs the core) */
      menu_requested = 1;
      break;
    default:
      break;
  }

  prev_action = action;
}

/* ---------------------------------------------------------------- libretro */

static const char *option_value(const char *key)
{
  static char num[8];

  if (!strcmp(key, "gpsp_drc"))              return dynarec_enable ? "enabled" : "disabled";
  if (!strcmp(key, "gpsp_color_correction")) return color_correct ? "enabled" : "disabled";
  if (!strcmp(key, "gpsp_frame_mixing"))     return lcd_blend ? "enabled" : "disabled";
  if (!strcmp(key, "gpsp_sound_rate"))       return sound_rate_high ? "65536" : "32768";
  if (!strcmp(key, "gpsp_frameskip")) {
    if (frameskip_style == FRAMESKIP_AUTO)   return "auto";
    if (frameskip_style == FRAMESKIP_MANUAL) return "fixed_interval";
    return "disabled";
  }
  if (!strcmp(key, "gpsp_frameskip_interval")) {
    snprintf(num, sizeof(num), "%d", max_frameskip < 0 ? 0 : max_frameskip > 10 ? 10 : max_frameskip);
    return num;
  }
  if (!strcmp(key, "gpsp_bios"))             return "auto";   /* gba_bios.bin in the app folder, else built-in */
  if (!strcmp(key, "gpsp_boot_mode"))        return "game";
  if (!strcmp(key, "gpsp_rumble"))           return "auto";
  if (!strcmp(key, "gpsp_turbo_period")) {
    if (turbo_frames < 4 || turbo_frames > 120)
      turbo_frames = 4;
    snprintf(num, sizeof(num), "%d", turbo_frames);
    return num;
  }
  if (!strcmp(key, "gpsp_serial")) {
    if (link_mode < 0 || link_mode >= (int)(array_size(link_mode_values) - 1))
      link_mode = 0;
    return link_mode_values[link_mode];
  }
  if (!strcmp(key, "gpsp_cable_players")) {
    if (link_players < 2 || link_players > 4)
      link_players = 2;
    snprintf(num, sizeof(num), "%d", link_players);
    return num;
  }
  return NULL;                                                /* core default */
}

static void log_printf(enum retro_log_level level, const char *fmt, ...)
{
  va_list ap;
  (void)level;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

static bool set_rumble(unsigned port, enum retro_rumble_effect effect, uint16_t strength)
{
  (void)port; (void)effect;
  plat_trigger_vibrate(0, strength, strength);
  return true;
}

static bool environment(unsigned cmd, void *data)
{
  switch (cmd)
  {
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      struct retro_variable *var = (struct retro_variable *)data;
      var->value = option_value(var->key);
      return var->value != NULL;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *(bool *)data = options_changed;
      options_changed = 0;
      return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      *(const char **)data = system_dir;
      return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
      return true;
    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: {
      const struct retro_audio_buffer_status_callback *cb = data;
      audio_status_cb = cb ? cb->callback : NULL;
      return true;
    }
    case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
      return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
      const struct retro_system_av_info *av = data;
      if (av && av->timing.sample_rate > 0)
        koriki_audio_in_rate = (unsigned)av->timing.sample_rate;
      return true;
    }
    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE: {
      struct retro_rumble_interface *r = data;
      r->set_rumble_state = set_rumble;
      return true;
    }
    case RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE:
      net_set_interface((const struct retro_netpacket_callback *)data);
      return true;
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
      *(unsigned *)data = 1;
      return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
      const struct retro_message_ext *m = data;
      if (m->msg && m->type != RETRO_MESSAGE_TYPE_STATUS)
        net_message(m->msg, m->duration ? m->duration : 3000);
      return true;
    }
    case RETRO_ENVIRONMENT_SET_MESSAGE: {
      const struct retro_message *m = data;
      if (m->msg)
        net_message(m->msg, m->frames ? m->frames * 17 : 3000);
      return true;
    }
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
      struct retro_log_callback *l = data;
      l->log = log_printf;
      return true;
    }
    default:
      return false;
  }
}

static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
  (void)width; (void)height;
  if (!data)
    return;                         /* frame skipped by the core */
  koriki_frame = (const uint16_t *)data;
  koriki_frame_pitch = pitch / sizeof(uint16_t);
  frame_rendered = 1;
}

static void audio_sample(int16_t left, int16_t right)
{
  int16_t s[2] = { left, right };
  if (menu_run_active)
    return;                         /* the game behind the menu: no sound */
  plat_sound_write(s, sizeof(s));
}

static size_t audio_sample_batch(const int16_t *data, size_t frames)
{
  if (menu_run_active)
    return frames;
  plat_sound_write((void *)data, frames * 2 * sizeof(int16_t));
  return frames;
}

/* KBIT_* order of libpicofe == GBA key bits; map them to the RetroPad */
static const struct { unsigned kbit, retro; } keymap[] = {
  { KBIT_A,      RETRO_DEVICE_ID_JOYPAD_A },
  { KBIT_B,      RETRO_DEVICE_ID_JOYPAD_B },
  { KBIT_SELECT, RETRO_DEVICE_ID_JOYPAD_SELECT },
  { KBIT_START,  RETRO_DEVICE_ID_JOYPAD_START },
  { KBIT_RIGHT,  RETRO_DEVICE_ID_JOYPAD_RIGHT },
  { KBIT_LEFT,   RETRO_DEVICE_ID_JOYPAD_LEFT },
  { KBIT_UP,     RETRO_DEVICE_ID_JOYPAD_UP },
  { KBIT_DOWN,   RETRO_DEVICE_ID_JOYPAD_DOWN },
  { KBIT_R,      RETRO_DEVICE_ID_JOYPAD_R },
  { KBIT_L,      RETRO_DEVICE_ID_JOYPAD_L },
  { KBIT_X,      RETRO_DEVICE_ID_JOYPAD_X },     /* turbo A */
  { KBIT_Y,      RETRO_DEVICE_ID_JOYPAD_Y },     /* turbo B */
};

static void input_poll(void)
{
  int actions[IN_BINDTYPE_COUNT] = { 0, };
  unsigned int emu_act;
  int which = EACTION_NONE;

  if (menu_run_active) {            /* the game behind the menu: no buttons */
    player_keys = 0;
    return;
  }
  in_update(actions);
  emu_act = actions[IN_BINDTYPE_EMU];
  if (emu_act) {
    for (; !(emu_act & 1); emu_act >>= 1, which++)
      ;
  }
  handle_emu_action(which);
  player_keys = actions[IN_BINDTYPE_PLAYER12];
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
  unsigned i;
  (void)index;

  if (port != 0 || device != RETRO_DEVICE_JOYPAD)
    return 0;
  if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
    int16_t mask = 0;
    for (i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++)
      if (player_keys & (1u << keymap[i].kbit))
        mask |= 1 << keymap[i].retro;
    return mask;
  }
  for (i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++)
    if (keymap[i].retro == id)
      return (player_keys >> keymap[i].kbit) & 1;
  return 0;
}

/* ---------------------------------------------------------------- loop */

static void count_fps(void)
{
  static uint32_t vsyncs, renders, nextsec;
  char msg[HUD_LEN];
  unsigned int ticks;

  ticks = plat_get_ticks_ms();
  if (hud_msg[0] && (int)(hud_msg_until - ticks) > 0) {
    plat_video_set_msg(hud_msg);
    return;
  }
  hud_msg[0] = 0;
  if (!show_fps)
    return;
  if (ticks > nextsec) {
    vsyncsps = vsyncs;
    rendersps = renders;
    vsyncs = 0;
    renders = 0;
    nextsec = ticks + 1000;
  }
  vsyncs++;
  if (frame_rendered)
    renders++;
  snprintf(msg, HUD_LEN, "FPS: %2.0f (%4.1f)", rendersps, vsyncsps);
  plat_video_set_msg(msg);
}

static void report_audio_buffer(void)
{
  if (audio_status_cb) {
    float free_part = plat_sound_capacity();          /* 1.0 = empty */
    unsigned occupancy = (unsigned)((1.0f - free_part) * 100.0f);
    audio_status_cb(true, occupancy, occupancy < 25);
  }
}

static void quit(void)
{
  net_stop();
  update_backup();
  retro_unload_game();
  retro_deinit();
  menu_finish();
  plat_finish();
  exit(0);
}

int main(int argc, char *argv[])
{
  struct retro_system_av_info av;
  struct retro_game_info game;
  char path[MAXPATHLEN];
  unsigned frames = 0;

  if (argc < 2) {
    printf("Usage: picogpsp FILE\n");
    return 0;
  }

  strncpy(rom_path, argv[1], MAXPATHLEN - 1);
  if (rom_path[0] != '/' && getcwd(path, sizeof(path))) {
    if (strlen(path) + strlen(rom_path) + 1 < MAXPATHLEN) {
      strcat(path, "/");
      strcat(path, rom_path);
      strcpy(rom_path, path);
    }
  }
  /* gba_bios.bin is looked for in the app folder (the launcher's cwd) */
  if (!getcwd(system_dir, sizeof(system_dir)))
    strcpy(system_dir, ".");

  if (plat_init())
    return -1;
  menu_init();          /* loads picogpsp.cfg and <rom>.cfg into the options */

  retro_set_environment(environment);
  retro_set_video_refresh(video_refresh);
  retro_set_audio_sample(audio_sample);
  retro_set_audio_sample_batch(audio_sample_batch);
  retro_set_input_poll(input_poll);
  retro_set_input_state(input_state);
  retro_init();

  memset(&game, 0, sizeof(game));
  game.path = rom_path;
  if (!retro_load_game(&game)) {
    fprintf(stderr, "Could not load the game file.\n");
    plat_finish();
    return -1;
  }
  retro_get_system_av_info(&av);
  if (av.timing.sample_rate > 0)
    koriki_audio_in_rate = (unsigned)av.timing.sample_rate;
  backup_load();
  {
    const char *b = strrchr(rom_path, '/');
    char name[64];
    snprintf(name, sizeof(name), "%s", b ? b + 1 : rom_path);
    if (strrchr(name, '.'))
      *strrchr(name, '.') = 0;
    net_set_game_name(name);
  }

  do {
    frame_rendered = 0;
    report_audio_buffer();
    net_poll();
    retro_run();
    count_fps();
    if (frame_rendered)
      plat_video_flip();
    if (menu_requested) {
      menu_requested = 0;
      toggle_fast_forward(1);
      update_backup();
      menu_run_begin();             /* Koriki: in a link session the game goes on */
      menu_loop();
      menu_run_end();
    }
    /* the core may change its rate when gpsp_sound_rate changes */
    if (options_changed == 0 && (++frames % 600) == 0)
      update_backup();    /* every ~10 s, only when the game wrote its save */
  } while (!should_quit);

  quit();
  return 0;
}
