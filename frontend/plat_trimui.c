#include <SDL/SDL.h>
#include "common.h"

#include "frontend/main.h"
#include "frontend/plat.h"
#include "frontend/scale.h"
#include "frontend/koriki_frame.h"
#include "frontend/libpicofe/menu.h"
#include "frontend/libpicofe/plat.h"
#include "frontend/libpicofe/input.h"
#include "frontend/libpicofe/in_sdl.h"

static SDL_Surface* screen;

#define BUF_LEN 8192
#ifdef KORIKI
#include <stdint.h>
#define KORIKI_OUT_RATE 48000
#endif

static short buf[BUF_LEN];
static int buf_w, buf_r;

static char msg[HUD_LEN];

static const struct in_default_bind in_sdl_defbinds[] = {
  { SDLK_UP,        IN_BINDTYPE_PLAYER12, KBIT_UP },
  { SDLK_DOWN,      IN_BINDTYPE_PLAYER12, KBIT_DOWN },
  { SDLK_LEFT,      IN_BINDTYPE_PLAYER12, KBIT_LEFT },
  { SDLK_RIGHT,     IN_BINDTYPE_PLAYER12, KBIT_RIGHT },
  { SDLK_LCTRL,     IN_BINDTYPE_PLAYER12, KBIT_B },
  { SDLK_SPACE,     IN_BINDTYPE_PLAYER12, KBIT_A },
  { SDLK_RETURN,    IN_BINDTYPE_PLAYER12, KBIT_START },
  { SDLK_RCTRL,     IN_BINDTYPE_PLAYER12, KBIT_SELECT },
  { SDLK_TAB,       IN_BINDTYPE_PLAYER12, KBIT_L },
  { SDLK_BACKSPACE, IN_BINDTYPE_PLAYER12, KBIT_R },
  { SDLK_LSHIFT,    IN_BINDTYPE_PLAYER12, KBIT_X },   /* X: turbo A */
  { SDLK_LALT,      IN_BINDTYPE_PLAYER12, KBIT_Y },   /* Y: turbo B */
#ifdef KORIKI
  /* Miyoo Mini: L1/R1 are E/T (L2/R2 = TAB/BACKSPACE also work as L/R) */
  { SDLK_e,         IN_BINDTYPE_PLAYER12, KBIT_L },
  { SDLK_t,         IN_BINDTYPE_PLAYER12, KBIT_R },
#endif
  { SDLK_ESCAPE,    IN_BINDTYPE_EMU, EACTION_MENU },
  { 0, 0, 0 }
};

const struct menu_keymap in_sdl_key_map[] =
{
  { SDLK_UP,        PBTN_UP },
  { SDLK_DOWN,      PBTN_DOWN },
  { SDLK_LEFT,      PBTN_LEFT },
  { SDLK_RIGHT,     PBTN_RIGHT },
  { SDLK_SPACE,     PBTN_MOK },
  { SDLK_LCTRL,     PBTN_MBACK },
  { SDLK_LALT,      PBTN_MA2 },
  { SDLK_LSHIFT,    PBTN_MA3 },
  { SDLK_TAB,       PBTN_L },
  { SDLK_BACKSPACE, PBTN_R },
#ifdef KORIKI
  { SDLK_e,         PBTN_L },
  { SDLK_t,         PBTN_R },
#endif
};

const struct menu_keymap in_sdl_joy_map[] =
{
  { SDLK_UP,    PBTN_UP },
  { SDLK_DOWN,  PBTN_DOWN },
  { SDLK_LEFT,  PBTN_LEFT },
  { SDLK_RIGHT, PBTN_RIGHT },
  { SDLK_WORLD_0, PBTN_MOK },
  { SDLK_WORLD_1, PBTN_MBACK },
  { SDLK_WORLD_2, PBTN_MA2 },
  { SDLK_WORLD_3, PBTN_MA3 },
};

static const char * const in_sdl_key_names[SDLK_LAST] = {
  [SDLK_UP]         = "up",
  [SDLK_DOWN]       = "down",
  [SDLK_LEFT]       = "left",
  [SDLK_RIGHT]      = "right",
  [SDLK_LSHIFT]     = "x",
  [SDLK_LCTRL]      = "b",
  [SDLK_SPACE]      = "a",
  [SDLK_LALT]       = "y",
  [SDLK_RETURN]     = "start",
  [SDLK_RCTRL]      = "select",
  [SDLK_TAB]        = "l",
  [SDLK_BACKSPACE]  = "r",
  [SDLK_ESCAPE]     = "menu",
};

static const struct in_pdata in_sdl_platform_data = {
  .defbinds  = in_sdl_defbinds,
  .key_map   = in_sdl_key_map,
  .kmap_size = array_size(in_sdl_key_map),
  .joy_map   = in_sdl_joy_map,
  .jmap_size = array_size(in_sdl_joy_map),
  .key_names = in_sdl_key_names,
};

static void *fb_flip(void)
{
  SDL_Flip(screen);
  return screen->pixels;
}

/* Koriki: the screen is always 640x480 (the panel's mode on the V1-V3 and
   Plus, stretched by the panel on the V4). The game is drawn there:
   - grid on: every scaling, nearest pixel, with the grid between the GBA
     pixels (kf_scale_grid);
   - grid off: the scalers of picogpsp on a 320x240 picture, doubled.
   The menu of libpicofe is drawn on a 320x240 buffer, doubled too. The border
   is painted once, when the options change or after the menu or a message
   (border_dirty). */
static uint16_t menubuf[320 * 240];      /* the menu (g_menuscreen_ptr) */
static uint16_t pic320[320 * 240];       /* the picture without the grid */
static int border_dirty = 1, msg_shown;
static int last_scaling = -1, last_border = -1, last_grid = -1;

/* copies the w x h block at (sx, sy) of a 320 wide picture doubled to the
   screen at (dx, dy) */
static void double_rect(const uint16_t *src, int sx, int sy, int dx, int dy, int w, int h)
{
  int pitch = screen->pitch / 2, i, j;
  for (j = 0; j < h; j++) {
    const uint16_t *s = src + (sy + j) * 320 + sx;
    uint32_t *d0 = (uint32_t *)((uint16_t *)screen->pixels + (dy + 2 * j) * pitch + dx);
    for (i = 0; i < w; i++)
      d0[i] = s[i] | ((uint32_t)s[i] << 16);
    memcpy((uint16_t *)d0 + pitch, d0, 2 * w * sizeof(uint16_t));
  }
}

static void paint_border(void)
{
  static const int style[3] = { KF_BORDER_NONE, KF_BORDER_GBA, KF_BORDER_SP };
  int x, y, w, h;
  if (koriki_picture_rect(640, &x, &y, &w, &h))
    kf_draw_border(screen->pixels, screen->pitch / 2, 640, 480, x, y, w, h,
                   style[border_style >= 0 && border_style <= 2 ? border_style : 0]);
  else
    memset(screen->pixels, 0, screen->pitch * 480);
}

void plat_video_menu_enter(int is_rom_loaded)
{
  border_dirty = 1;
}

void plat_video_menu_begin(void)
{
  g_menuscreen_ptr = menubuf;
}

void plat_video_menu_end(void)
{
  SDL_LockSurface(screen);
  double_rect(menubuf, 0, 0, 0, 0, 320, 240);
  SDL_UnlockSurface(screen);
  SDL_Flip(screen);
  g_menuscreen_ptr = menubuf;
}

void plat_video_menu_leave(void)
{
  memset(menubuf, 0, sizeof(menubuf));
  SDL_LockSurface(screen);
  memset(screen->pixels, 0, screen->pitch * 480);
  SDL_UnlockSurface(screen);
  SDL_Flip(screen);
  border_dirty = 1;
}

void plat_video_open(void)
{
}

void plat_video_set_msg(char *new_msg)
{
  snprintf(msg, HUD_LEN, "%s", new_msg);
}

void plat_video_flip(void)
{
  int x, y, w, h;

  video_post_process();
  if (last_scaling != (int)scaling_mode || last_border != border_style || last_grid != grid_mode) {
    last_scaling = scaling_mode;
    last_border = border_style;
    last_grid = grid_mode;
    border_dirty = 1;
  }
  /* a message covers the bottom lines: the border is painted again after it */
  if (msg[0])
    msg_shown = 1;
  else if (msg_shown) {
    msg_shown = 0;
    border_dirty = 1;
  }

  SDL_LockSurface(screen);
  if (border_dirty) {
    paint_border();
    border_dirty = 0;
  }
  if (!koriki_picture_rect(640, &x, &y, &w, &h)) {
    x = y = 0; w = 640; h = 480;
  }
  {
    uint16_t *dst = (uint16_t *)screen->pixels + y * (screen->pitch / 2) + x;
    int pitch = screen->pitch / 2;
    if (scaling_mode == SCALING_NONE && grid_mode && koriki_frame)
      /* Native: 2x, every pixel the same size, the grid exactly between them */
      kf_scale_grid(dst, pitch, koriki_frame, koriki_frame_pitch, 240, 160, w, h, 0x0000, grid_mode);
    else {
      if ((scaling_mode == SCALING_ASPECT_PIXEL || scaling_mode == SCALING_FULL_PIXEL) && koriki_frame)
        /* Pixel: nearest pixel straight on the 640x480 screen */
        kf_scale_grid(dst, pitch, koriki_frame, koriki_frame_pitch, 240, 160, w, h, 0x0000, 0);
      else {
        int sx, sy, sw, sh;
        if (!koriki_picture_rect(320, &sx, &sy, &sw, &sh)) {
          sx = sy = 0; sw = 320; sh = 240;
        }
        video_scale(pic320, 240, 320);
        double_rect(pic320, sx, sy, x, y, sw, sh);
      }
      /* stretched: the pixels are not all the same size, so the grid is laid
         over the picture, the same everywhere (as Koriki's ATC-GRID overlay) */
      if (grid_mode && scaling_mode != SCALING_NONE)
        kf_overlay_grid(dst, pitch, w, h, grid_mode);
    }
  }
  if (msg[0]) {
    /* the message on a 320 wide strip, doubled at the bottom of the screen */
    static uint16_t strip[320 * 240];
    memset(strip + 230 * 320, 0, 10 * 320 * sizeof(uint16_t));
    video_print_msg(strip, 240, 320, msg);
    double_rect(strip, 0, 230, 0, 460, 320, 10);
  }
  SDL_UnlockSurface(screen);

  fb_flip();
  msg[0] = 0;
}

void plat_video_close(void)
{
}

void plat_sound_callback(void *unused, u8 *stream, int len)
{
  short *p = (short *)stream;
  len /= sizeof(short);

  while (buf_r != buf_w && len > 0) {
    *p++ = buf[buf_r++];
    buf_r %= BUF_LEN;
    --len;
  }

  while(len > 0) {
    *p++ = 0;
    --len;
  }
}

void plat_sound_finish(void)
{
  SDL_PauseAudio(1);
  SDL_CloseAudio();
}

int plat_sound_init(void)
{

  if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    return -1;
  }

  SDL_AudioSpec spec;

#ifdef KORIKI
  spec.freq = KORIKI_OUT_RATE;   /* the core runs at koriki_audio_in_rate, see plat_sound_write */
#else
  spec.freq = sound_frequency;
#endif
  spec.format = AUDIO_S16SYS;
  spec.channels = 2;
  spec.samples = 512;
  spec.callback = plat_sound_callback;

  if (SDL_OpenAudio(&spec, NULL) < 0) {
    plat_sound_finish();
    return -1;
  }

  SDL_PauseAudio(0);
  return 0;
}

float plat_sound_capacity(void)
{
  int buffered = 0;
  if (buf_w != buf_r) {
    buffered = buf_w > buf_r ? buf_w - buf_r : (buf_w + BUF_LEN) - buf_r;
  }

  return 1.0 - (float)buffered / BUF_LEN;
}

#ifdef KORIKI
/* Koriki: the libretro core renders at its gpsp_sound_rate (65536 or 32768 Hz;
   the Trimui build forced 48000 Hz into the core, which breaks the sound of
   games such as Golden Sun); here it is converted to the 48 kHz output with
   linear interpolation, keeping the position between calls. */
static int koriki_push_stereo(short left, short right)
{
  const int next = (buf_w + 1) % BUF_LEN;
  const int after_pair = (buf_w + 2) % BUF_LEN;

  while (next == buf_r || after_pair == buf_r) {
    SDL_UnlockAudio();
    if (!limit_frames)
      return 0;
    plat_sleep_ms(1);
    SDL_LockAudio();
  }
  buf[buf_w] = left;
  buf[next] = right;
  buf_w = after_pair;
  return 1;
}

void plat_sound_write(void *data, int bytes)
{
  static int prev_l, prev_r;
  static uint32_t pos;          /* 16.16, between prev and the current input frame */
  const uint32_t step = (uint32_t)(((uint64_t)koriki_audio_in_rate << 16) / KORIKI_OUT_RATE);
  short *in = (short *)data;
  int frames = bytes / (2 * sizeof(short));

  SDL_LockAudio();
  while (frames-- > 0) {
    int l = in[0], r = in[1];
    in += 2;
    while (pos < 0x10000) {
      int f = pos;
      if (!koriki_push_stereo((short)(prev_l + (((int64_t)(l - prev_l) * f) >> 16)),
                              (short)(prev_r + (((int64_t)(r - prev_r) * f) >> 16))))
        return;               /* not limiting frames: drop, as the original does */
      pos += step;
    }
    pos -= 0x10000;
    prev_l = l;
    prev_r = r;
  }
  SDL_UnlockAudio();
}
#else
void plat_sound_write(void *data, int bytes)
{
  short *sound_data = (short *)data;
  SDL_LockAudio();

  while (bytes > 0) {
    while (((buf_w + 1) % BUF_LEN) == buf_r) {
      SDL_UnlockAudio();

      if (!limit_frames)
        return;

      plat_sleep_ms(1);
      SDL_LockAudio();
    }
    buf[buf_w] = *sound_data++;

    ++buf_w;
    buf_w %= BUF_LEN;
    bytes -= sizeof(short);
  }
  SDL_UnlockAudio();
}
#endif

void plat_sdl_event_handler(void *event_)
{
}

int plat_init(void)
{
  SDL_Init(SDL_INIT_VIDEO);
  screen = SDL_SetVideoMode(640, 480, 16, SDL_SWSURFACE);   /* Koriki: see plat_video_flip */

  if (screen == NULL) {
    printf("%s, failed to set video mode\n", __func__);
    return -1;
  }

  SDL_ShowCursor(0);

  g_menuscreen_w = 320;
  g_menuscreen_h = 240;
  g_menuscreen_pp = 320;
  g_menuscreen_ptr = menubuf;

  if (in_sdl_init(&in_sdl_platform_data, plat_sdl_event_handler)) {
    fprintf(stderr, "SDL input failed to init: %s\n", SDL_GetError());
    return -1;
  }
  in_probe();

#ifndef KORIKI
  sound_frequency = 48000;
#endif

  if (plat_sound_init()) {
    fprintf(stderr, "SDL sound failed to init: %s\n", SDL_GetError());
    return -1;
  }
  return 0;
}

void plat_pre_finish(void)
{
}

void plat_finish(void)
{
  plat_sound_finish();
  SDL_Quit();
}

#ifdef KORIKI
#include "koriki_rumble.h"
#endif

void plat_trigger_vibrate(int pad, int low, int high)
{
#ifdef KORIKI
  /* Miyoo Mini motor (GPIO 48), like Koriki's RetroArch */
  koriki_rumble(low || high);
#endif
}
