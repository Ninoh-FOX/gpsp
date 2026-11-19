#include <sys/stat.h>
#include "common.h"

#include "frontend/config.h"
#include "frontend/main.h"
#include "frontend/menu.h"
#include "frontend/scale.h"
#include "frontend/plat.h"
#include "frontend/net.h"
#include <SDL_keysym.h>    /* the default turbo keys */

#include "frontend/libpicofe/config_file.h"

#define MENU_ALIGN_LEFT 0
#define MENU_X2 0

typedef enum
{
  MA_NONE = 1,
  MA_MAIN_RESUME_GAME,
  MA_MAIN_SAVE_STATE,
  MA_MAIN_LOAD_STATE,
  MA_MAIN_RESET_GAME,
  MA_MAIN_CREDITS,
  MA_MAIN_EXIT,
  MA_OPT_SAVECFG,
  MA_OPT_SAVECFG_GAME,
  MA_CTRL_PLAYER1,
  MA_CTRL_EMU,
  MA_LINK_STATUS,
  MA_LINK_MODE,
  MA_LINK_PLAYERS,
  MA_LINK_HOST,
  MA_LINK_JOIN,
  MA_LINK_LAST,
  MA_LINK_STOP,
  MA_LINK_FOUND,      /* + index of the host found */
} menu_id;

int menu_save_config(int is_game);
void menu_set_defaults(void);

me_bind_action me_ctrl_actions[] =
{
  { "UP       ", 1 << KBIT_UP},
  { "DOWN     ", 1 << KBIT_DOWN },
  { "LEFT     ", 1 << KBIT_LEFT },
  { "RIGHT    ", 1 << KBIT_RIGHT },
  { "A BUTTON ", 1 << KBIT_A },
  { "B BUTTON ", 1 << KBIT_B },
  { "START    ", 1 << KBIT_START },
  { "SELECT   ", 1 << KBIT_SELECT },
  { "L BUTTON ", 1 << KBIT_L },
  { "R BUTTON ", 1 << KBIT_R },
  { "TURBO A  ", 1 << KBIT_X },
  { "TURBO B  ", 1 << KBIT_Y },
  { NULL,       0 }
};

me_bind_action emuctrl_actions[] =
{
  { "Save State       ", 1 << EACTION_SAVE_STATE },
  { "Load State       ", 1 << EACTION_LOAD_STATE },
  { "Toggle Frameskip ", 1 << EACTION_TOGGLE_FSKIP },
  { "Show/Hide FPS    ", 1 << EACTION_TOGGLE_FPS },
  { "Toggle FF        ", 1 << EACTION_TOGGLE_FF },
  { "Enter Menu       ", 1 << EACTION_MENU },
  { NULL,                0 }
};

int emu_check_save_file(int slot, int *time)
{
  char fname[MAXPATHLEN];
  struct stat status;
  int ret;

  state_file_name(fname, sizeof(fname), slot);

  ret = stat(fname, &status);
  if (ret != 0)
    return 0;

  return 1;
}

int emu_save_load_game(int load, int unused)
{
  int ret;

  if (load)
    ret = load_state_file(state_slot);
  else
    ret = save_state_file(state_slot);

  return ret;
}

#include "frontend/libpicofe/menu.c"

static const char *mgn_saveloadcfg(int id, int *offs)
{
  return "";
}

static int mh_restore_defaults(int id, int keys)
{
  menu_set_defaults();
  menu_update_msg("defaults restored");
  return 1;
}

static int mh_savecfg(int id, int keys)
{
  if (menu_save_config(id == MA_OPT_SAVECFG_GAME ? 1 : 0) == 0)
    menu_update_msg("config saved");
  else
    menu_update_msg("failed to write config");

  return 1;
}

static const char h_restore_def[]     = "Switches back to default / recommended\n"
          "configuration";

static const char h_color_correct[]   = "Modifies colors to simulate original display";
static const char h_lcd_blend[]       = "Blends frames to simulate LCD lag";
static const char h_show_fps[]        = "Shows frames and vsyncs per second";
static const char h_dynarec_enable[]  = "Improves performance, but may reduce accuracy";


static const char *men_frameskip[] = { "OFF", "Auto", "Manual", NULL };

static const char *men_scaling[] = { "Native", "3:2 Sharp", "3:2 Smooth", "4:3 Sharp", "4:3 Smooth", NULL};

/* Koriki: libretro gpSP core option gpsp_sound_rate */
static const char *men_sound_rate[] = { "32768 Hz", "65536 Hz", NULL };
static const char h_sound_rate[]      = "65536 renders the full mixer bandwidth\n"
          "(needed by some games, e.g. Golden Sun)";

static const char h_turbo[]           = "X = turbo A, Y = turbo B:\n"
          "frames of each press-release cycle";

static menu_entry e_menu_options[] =
{
  mee_enum         ("Frameskip",                0, frameskip_style, men_frameskip),
  mee_range        ("Max Frameskip",            0, max_frameskip, 1, 5),
  mee_enum         ("Scaling",                  0, scaling_mode, men_scaling),
  mee_onoff_h      ("Color Correction",         0, color_correct, 1, h_color_correct),
  mee_onoff_h      ("LCD Ghosting",             0, lcd_blend, 1, h_lcd_blend),
  mee_onoff_h      ("Dynamic Recompiler",       0, dynarec_enable, 1, h_dynarec_enable),
  mee_enum_h       ("Sound Rate",               0, sound_rate_high, men_sound_rate, h_sound_rate),
  mee_range_h      ("Turbo period (frames)",    0, turbo_frames, 4, 60, h_turbo),
  mee_onoff_h      ("Show FPS",                 0, show_fps, 1, h_show_fps),
  mee_cust_nosave  ("Save global config",       MA_OPT_SAVECFG,      mh_savecfg, mgn_saveloadcfg),
  mee_cust_nosave  ("Save game config",         MA_OPT_SAVECFG_GAME, mh_savecfg, mgn_saveloadcfg),
  mee_handler_h    ("Restore defaults",         mh_restore_defaults, h_restore_def),
  mee_end,
};

static int menu_loop_options(int id, int keys)
{
  static int sel = 0;
  me_loop(e_menu_options, &sel);

  /* the libretro core reads its options again on the next frame */
  koriki_options_changed();

  return 0;
}

/* ---------------------------------------------------------------- link */

static const char *men_link_mode[] = { "Automatic", "Disabled", "Wireless Adapter",
  "Pokemon Gen3", "Advance Wars 1", "Advance Wars 2", "Cable Link", NULL };
static const char h_link_mode[] = "Every console must use the same mode.\n"
          "Automatic: Wireless Adapter, Pokemon or\n"
          "Advance Wars for the games that need them,\n"
          "Cable Link for the rest";
static const char h_link_players[] = "GBAs on the Cable Link (same on every console)";
static const char h_link_host[] = "This console hosts: the others join it";
static const char h_link_join[] = "Looks for a console hosting on the network";

static char last_host[16];
static int link_resume;
static net_found_t found[NET_MAX_FOUND];
static char found_names[NET_MAX_FOUND][64];

static void last_host_file(char *buf, size_t len)
{
  char root[MAXPATHLEN];
  plat_get_root_dir(root, sizeof(root));
  snprintf(buf, len, "%slast_host.txt", root);
}

static void last_host_load(void)
{
  char name[MAXPATHLEN];
  FILE *f;
  last_host_file(name, sizeof(name));
  last_host[0] = 0;
  if ((f = fopen(name, "r"))) {
    if (fgets(last_host, sizeof(last_host), f))
      last_host[strcspn(last_host, "\r\n")] = 0;
    fclose(f);
  }
}

static void last_host_save(const char *ip)
{
  char name[MAXPATHLEN];
  FILE *f;
  snprintf(last_host, sizeof(last_host), "%s", ip);
  last_host_file(name, sizeof(name));
  if ((f = fopen(name, "w"))) {
    fprintf(f, "%s\n", ip);
    fclose(f);
  }
}

static const char *mgn_link_status(int id, int *offs)
{
  static char s[48];
  switch (net_state()) {
  case NET_HOSTING:
    snprintf(s, sizeof(s), "Hosting, %d of %d players", net_clients() + 1, 4);
    break;
  case NET_CONNECTING:
    snprintf(s, sizeof(s), "Joining %s...", net_peer_ip());
    break;
  case NET_CONNECTED:
    snprintf(s, sizeof(s), "Player %d with %s", net_client_id() + 1, net_peer_ip());
    break;
  default:
    snprintf(s, sizeof(s), "Not connected");
    break;
  }
  return s;
}

static const char *mgn_last_host(int id, int *offs)
{
  return last_host[0] ? last_host : "none";
}

static int join(const char *ip)
{
  if (net_join(ip)) {
    menu_update_msg("could not join");
    return 0;
  }
  last_host_save(ip);
  link_resume = 1;      /* back to the game: it has to run to link up */
  return 1;
}

static int mh_join_found(int id, int keys)
{
  return join(found[id - MA_LINK_FOUND].ip);
}

static int join_list(int n)
{
  static menu_entry e[NET_MAX_FOUND + 1];
  int i, sel = 0;

  memset(e, 0, sizeof(e));
  for (i = 0; i < n; i++) {
    snprintf(found_names[i], sizeof(found_names[i]), "%.30s (%d) %s",
             found[i].name, found[i].players, found[i].ip);
    e[i].name = found_names[i];
    e[i].beh = MB_NONE;
    e[i].id = MA_LINK_FOUND + i;
    e[i].enabled = 1;
    e[i].selectable = 1;
    e[i].handler = mh_join_found;
  }
  me_loop(e, &sel);
  return link_resume;
}

static int mh_link(int id, int keys)
{
  int n;

  switch (id) {
  case MA_LINK_HOST:
    if (net_host()) {
      menu_update_msg("could not host (network?)");
      return 0;
    }
    link_resume = 1;
    return 1;
  case MA_LINK_JOIN:
    menu_update_msg("looking for hosts...");
    n = net_discover(found, NET_MAX_FOUND, 1000);
    if (n == 0) {
      menu_update_msg("no host found");
      return 0;
    }
    if (n == 1)
      return join(found[0].ip);
    return join_list(n);
  case MA_LINK_LAST:
    if (!last_host[0])
      return 0;
    return join(last_host);
  case MA_LINK_STOP:
    net_stop();
    return 0;
  }
  return 0;
}

static menu_entry e_menu_link[] =
{
  mee_label_mk     (MA_LINK_STATUS, mgn_link_status),
  mee_enum_h       ("Link mode",                MA_LINK_MODE, link_mode, men_link_mode, h_link_mode),
  mee_range_h      ("Cable Link players",       MA_LINK_PLAYERS, link_players, 2, 4, h_link_players),
  mee_handler_id_h ("Host game",                MA_LINK_HOST, mh_link, h_link_host),
  mee_handler_id_h ("Join game",                MA_LINK_JOIN, mh_link, h_link_join),
  mee_cust_nosave  ("Join last host",           MA_LINK_LAST, mh_link, mgn_last_host),
  mee_handler_id   ("Disconnect",               MA_LINK_STOP, mh_link),
  mee_cust_nosave  ("Save global config",       MA_OPT_SAVECFG,      mh_savecfg, mgn_saveloadcfg),
  mee_cust_nosave  ("Save game config",         MA_OPT_SAVECFG_GAME, mh_savecfg, mgn_saveloadcfg),
  mee_end,
};

static int menu_loop_link(int id, int keys)
{
  static int sel = 1;
  int off = net_state() == NET_OFF;

  if (!net_available()) {
    menu_update_msg("this core has no link support");
    return 0;
  }
  last_host_load();
  link_resume = 0;
  /* the mode and the players are fixed during a session */
  me_enable(e_menu_link, MA_LINK_MODE, off);
  me_enable(e_menu_link, MA_LINK_PLAYERS, off);
  me_enable(e_menu_link, MA_LINK_HOST, off);
  me_enable(e_menu_link, MA_LINK_JOIN, off);
  me_enable(e_menu_link, MA_LINK_LAST, off);
  me_enable(e_menu_link, MA_LINK_STOP, !off);
  me_loop(e_menu_link, &sel);

  /* the core reads its options again on the next frame */
  koriki_options_changed();
  return link_resume;
}

static int key_config_loop_wrap(int id, int keys)
{
  switch (id) {
    case MA_CTRL_PLAYER1:
      key_config_loop(me_ctrl_actions, array_size(me_ctrl_actions) - 1, 0);
      break;
    case MA_CTRL_EMU:
      key_config_loop(emuctrl_actions, array_size(emuctrl_actions) - 1, -1);
      break;
    default:
      break;
  }
  return 0;
}

static menu_entry e_menu_keyconfig[] =
{
  mee_handler_id  ("Player controls",    MA_CTRL_PLAYER1,     key_config_loop_wrap),
  mee_handler_id  ("Emulator controls",  MA_CTRL_EMU,         key_config_loop_wrap),
  mee_cust_nosave ("Save global config", MA_OPT_SAVECFG,      mh_savecfg, mgn_saveloadcfg),
  mee_cust_nosave ("Save game config",   MA_OPT_SAVECFG_GAME, mh_savecfg, mgn_saveloadcfg),
  mee_end,
};

static int menu_loop_keyconfig(int id, int keys)
{
  static int sel = 0;
  me_loop(e_menu_keyconfig, &sel);
  return 0;
}


static int main_menu_handler(int id, int keys)
{
  switch (id)
  {
  case MA_MAIN_RESUME_GAME:
    return 1;
  case MA_MAIN_SAVE_STATE:
    return menu_loop_savestate(0);
  case MA_MAIN_LOAD_STATE:
    return menu_loop_savestate(1);
  case MA_MAIN_RESET_GAME:
    retro_reset();
    return 1;
  case MA_MAIN_EXIT:
    should_quit = 1;
    return 1;
  default:
    lprintf("%s: something unknown selected\n", __FUNCTION__);
    break;
  }

  return 0;
}

static menu_entry e_menu_main[] =
{
  mee_handler_id("Resume game",        MA_MAIN_RESUME_GAME, main_menu_handler),
  mee_handler_id("Save State",         MA_MAIN_SAVE_STATE,  main_menu_handler),
  mee_handler_id("Load State",         MA_MAIN_LOAD_STATE,  main_menu_handler),
  mee_handler_id("Reset game",         MA_MAIN_RESET_GAME,  main_menu_handler),
  mee_handler   ("Options",            menu_loop_options),
  mee_handler   ("Controls",           menu_loop_keyconfig),
  mee_handler   ("Link (netplay)",     menu_loop_link),
  /* mee_handler_id("Cheats",             MA_MAIN_CHEATS,      main_menu_handler), */
  mee_handler_id("Exit",               MA_MAIN_EXIT,        main_menu_handler),
  mee_end,
};

void draw_savestate_bg(int slot)
{
}

void menu_set_defaults(void)
{
  dynarec_enable = 1;
  frameskip_style = 1;
  scaling_mode = 0;
  max_frameskip = 3;
  color_correct = 0;
  lcd_blend = 0;
  show_fps = 0;
  limit_frames = 1;
  sound_rate_high = 1;
  turbo_frames = 4;
  link_mode = 0;
  link_players = 2;
}

void menu_loop(void)
{
  static int sel = 0;
  plat_video_menu_enter(1);
  video_scale(g_menubg_ptr, g_menuscreen_h, g_menuscreen_pp);
  menu_darken_bg(g_menubg_ptr, g_menubg_ptr, g_menuscreen_h * g_menuscreen_pp, 0);
  me_loop_d(e_menu_main, &sel, NULL, NULL);

  /* wait until menu, ok, back is released */
  while (in_menu_wait_any(NULL, 50) & (PBTN_MENU|PBTN_MOK|PBTN_MBACK))
    ;
  memset(g_menubg_ptr, 0, g_menuscreen_h * g_menuscreen_pp * sizeof(uint16_t));

  plat_video_menu_leave();
}

int menu_save_config(int is_game)
{
  char config_filename[MAXPATHLEN];
  FILE *config_file;

  config_file_name(config_filename, MAXPATHLEN, is_game);
  config_file = fopen(config_filename, "wb");
  if (!config_file) {
    fprintf(stderr, "Could not write config to %s\n", config_filename);
    return -1;
  }

  config_write(config_file);
  config_write_keys(config_file);

  fclose(config_file);
  return 0;
}


void menu_load_config(void)
{
  char config_filename[MAXPATHLEN];
  FILE *config_file;
  size_t length;
  char *config;

  config_file_name(config_filename, MAXPATHLEN, 1);
  config_file = fopen(config_filename, "rb");
  if (!config_file) {
    config_file_name(config_filename, MAXPATHLEN, 0);
    config_file = fopen(config_filename, "rb");
  }

  if (!config_file)
    return;

  fseek(config_file, 0, SEEK_END);
  length = ftell(config_file);
  fseek(config_file, 0, SEEK_SET);

  config = calloc(1, length);

  if (fread(config, 1, length, config_file))
  fclose(config_file);

  config_read(config);
  config_read_keys(config);

  if (config)
    free(config);
}

/* A config saved before X/Y were turbo A/B replaces every default binding:
 * give them X and Y again when no key has them and those keys are free. */
static void turbo_default_binds(void)
{
  static const struct { int key, kbit; } tb[] = {
    { SDLK_LSHIFT, KBIT_X }, { SDLK_LALT, KBIT_Y },
  };
  int dev, k, i;

  for (dev = 0; dev < IN_MAX_DEVS; dev++) {
    const int *binds = in_get_dev_binds(dev);
    int count = 0, has[2] = { 0, 0 };
    if (!binds || in_get_config(dev, IN_CFG_BIND_COUNT, &count) || count <= 0)
      continue;
    for (k = 0; k < count; k++)
      for (i = 0; i < 2; i++)
        if (binds[IN_BIND_OFFS(k, IN_BINDTYPE_PLAYER12)] & (1 << tb[i].kbit))
          has[i] = 1;
    for (i = 0; i < 2; i++) {
      if (has[i] || tb[i].key >= count)
        continue;
      binds = in_get_dev_binds(dev);
      if (binds && !binds[IN_BIND_OFFS(tb[i].key, IN_BINDTYPE_PLAYER12)])
        in_bind_key(dev, tb[i].key, 1 << tb[i].kbit, IN_BINDTYPE_PLAYER12, 0);
    }
  }
}

void menu_init(void)
{
  menu_init_base();

  menu_set_defaults();
  menu_load_config();
  turbo_default_binds();

  g_menubg_src_ptr = calloc(g_menuscreen_w * g_menuscreen_h * 2, 1);
  g_menubg_ptr = calloc(g_menuscreen_w * g_menuscreen_h * 2, 1);
  if (g_menubg_src_ptr == NULL || g_menubg_ptr == NULL) {
    fprintf(stderr, "OOM\n");
    exit(1);
  }
}

void menu_finish(void)
{
}

static void debug_menu_loop(void)
{
}

void menu_update_msg(const char *msg)
{
  strncpy(menu_error_msg, msg, sizeof(menu_error_msg));
  menu_error_msg[sizeof(menu_error_msg) - 1] = 0;

  menu_error_time = plat_get_ticks_ms();
  lprintf("msg: %s\n", menu_error_msg);
}
