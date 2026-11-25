#ifndef __FRONTEND_MAIN_H__
#define __FRONTEND_MAIN_H__

#include <stddef.h>
#include <stdint.h>

#define MAXPATHLEN 512

typedef enum {
  EACTION_NONE = 0,
  EACTION_MENU,
  EACTION_TOGGLE_FPS,
  EACTION_TOGGLE_FSKIP,
  EACTION_TOGGLE_FF,
  EACTION_SAVE_STATE,
  EACTION_LOAD_STATE,
  EACTION_QUIT,
} emu_action;

typedef enum {
  KBIT_A = 0,
  KBIT_B,
  KBIT_SELECT,
  KBIT_START,
  KBIT_RIGHT,
  KBIT_LEFT,
  KBIT_UP,
  KBIT_DOWN,
  KBIT_R,
  KBIT_L,
  KBIT_X,         /* turbo A (RetroPad X: the core repeats A) */
  KBIT_Y          /* turbo B (RetroPad Y) */
} keybit;

typedef enum {
  FRAMESKIP_NONE = 0,
  FRAMESKIP_AUTO,
  FRAMESKIP_MANUAL,
} frameskip_style_t;

typedef enum {
  SCALING_NONE = 0,
  SCALING_ASPECT_SHARP,
  SCALING_ASPECT_SMOOTH,
  SCALING_FULL_SHARP,
  SCALING_FULL_SMOOTH,
  SCALING_ASPECT_PIXEL,   /* Koriki: nearest pixel on the 640x480 screen */
  SCALING_FULL_PIXEL,
} scaling_mode_t;


extern int should_quit;

extern int state_slot;
extern int dynarec_enable;
extern frameskip_style_t frameskip_style;
extern scaling_mode_t scaling_mode;
extern int max_frameskip;
extern int color_correct;
extern int lcd_blend;
extern int show_fps;
extern int border_style;   /* Koriki: 0 none, 1 GBA, 2 SP (koriki_frame.c) */
extern int grid_mode;      /* Koriki: 0 off, 1 soft, 2 strong */
extern int limit_frames;
extern int turbo_frames;        /* gpsp_turbo_period */
extern int link_mode;           /* gpsp_serial: index in link_mode_values */
extern int link_players;        /* gpsp_cable_players (2-4) */
extern const char *const link_mode_values[];
bool link_mode_pending(void);
void rumble_stop(void);          /* Koriki: motor off (menu, exit) */
extern int boot_mode_bios;          /* gpsp_boot_mode: 1 = boot through the BIOS */
extern int sound_rate_high;     /* gpsp_sound_rate: 1 = 65536 Hz (full mixer), 0 = 32768 */

extern uint16_t *gba_screen_pixels_prev;
extern uint16_t *gba_processed_pixels;

#define array_size(x) (sizeof(x) / sizeof(x[0]))

void state_file_name(char *buf, size_t len, unsigned state_slot);
void config_file_name(char *buf, size_t len, int is_game);
void handle_emu_action(emu_action action);
int save_state_file(unsigned state_slot);
int load_state_file(unsigned state_slot);

/* Koriki: frontend over the libretro API of the current gpSP core */
void koriki_options_changed(void);
void retro_reset(void);
extern const uint16_t *koriki_frame;      /* last frame from the core (240x160 RGB565) */
extern unsigned koriki_frame_pitch;       /* in pixels */
extern unsigned koriki_audio_in_rate;     /* core sample rate (gpsp_sound_rate) */

#endif /* __FRONTEND_MAIN_H__ */
