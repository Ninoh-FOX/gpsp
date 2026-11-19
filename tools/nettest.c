/* nettest: one console of the standalone netplay, headless (no SDL), to run
 * two or more under qemu-arm on one PC.
 *   nettest host ROM FRAMES OUT
 *   nettest join IP ROM FRAMES OUT
 *   nettest discover
 * FRAMES: frames to run once the link is up. OUT: EWRAM dump at the end.
 * Env: SERIAL (gpsp_serial, default mul_cable), PLAYERS (default 2).
 *
 * Build (after make -f Makefile.koriki, which leaves libgpsp_core.a):
 *   T=/opt/miyoomini-toolchain/bin/arm-linux-gnueabihf-
 *   F="-O2 -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -I. -Ilibretro
 *      -Ilibretro/libretro-common/include -D_GNU_SOURCE"
 *   $T"gcc" $F -c tools/nettest.c -o /tmp/nettest.o && $T"gcc" $F -c frontend/net.c -o /tmp/net.o
 *   $T"g++" -marm -o /tmp/nettest /tmp/nettest.o /tmp/net.o libgpsp_core.a -lm -lpthread
 * Run with qemu-arm -L <sysroot>: the host first, then each client. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include "libretro.h"
#include "frontend/net.h"
#ifdef MENU_TEST
#include "frontend/menurun.h"
void __wrap_plat_sleep_ms(int ms);
void __real_plat_sleep_ms(int ms) { usleep(ms * 1000); }
#endif

/* More environment, for real games:
 *   KEYS="from-to:key+key;..."  buttons at those frames (counted from the
 *        link-up, or from the start with "solo"); keys: a b select start
 *        right left up down r l
 *   SAV=file   save memory to load (SAVEOUT=file writes it at the end)
 *   BIOSDIR=dir  official BIOS (dir/gba_bios.bin); BOOT=bios|game
 *   SHOTS=dir  screens as dir/NNNNN.ppm every SHOT_EVERY frames (or at
 *        SHOT_AT="f,f,...")
 *   nettest solo ROM FRAMES OUT  runs without network
 *   MENU_AT=n MENU_MS=t  at session frame n the menu is open for t ms, as the
 *        frontend does it (frontend/menurun.c: link it in, see below)
 * With the menu test add frontend/menurun.c to the build. */

static const char *role;
static unsigned frame, sframe;   /* sframe: frames of the session (KEYS, SHOTS) */
static const uint16_t *last_fb; static size_t last_pitch;

void net_message(const char *text, unsigned ms) { printf("[%s %u/%u] MSG %s\n", role, frame, sframe, text); fflush(stdout); }

static void logp(enum retro_log_level l, const char *fmt, ...) { va_list a; if (l < RETRO_LOG_WARN) return; va_start(a, fmt); printf("[%s %u] LOG ", role, frame); vprintf(fmt, a); va_end(a); fflush(stdout); }

static bool env(unsigned cmd, void *data)
{
  switch (cmd) {
  case RETRO_ENVIRONMENT_GET_VARIABLE: {
    struct retro_variable *v = data;
    v->value = NULL;
    if (!strcmp(v->key, "gpsp_serial")) v->value = getenv("SERIAL") ? getenv("SERIAL") : "auto";
    else if (!strcmp(v->key, "gpsp_cable_players")) v->value = getenv("PLAYERS") ? getenv("PLAYERS") : "2";
    else if (!strcmp(v->key, "gpsp_bios")) v->value = getenv("BIOSDIR") ? "official" : "builtin";
    else if (!strcmp(v->key, "gpsp_boot_mode")) v->value = getenv("BOOT") ? getenv("BOOT") : "game";
    else if (!strcmp(v->key, "gpsp_drc")) v->value = "enabled";
    return v->value != NULL;
  }
  case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data = false; return true;
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return true;
  case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    *(const char **)data = getenv("BIOSDIR") ? getenv("BIOSDIR") : "."; return true;
  case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = "."; return true;
  case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return true;
  case RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE: net_set_interface(data); return true;
  case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION: *(unsigned *)data = 1; return true;
  case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: { const struct retro_message_ext *m = data; if (m->msg) net_message(m->msg, 0); return true; }
  case RETRO_ENVIRONMENT_SET_MESSAGE: { const struct retro_message *m = data; if (m->msg) net_message(m->msg, 0); return true; }
  case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback *)data)->log = logp; return true;
  }
  return false;
}

static void shot(void)
{
  const char *dir = getenv("SHOTS"), *at = getenv("SHOT_AT"), *ev = getenv("SHOT_EVERY");
  char path[512]; FILE *f; int x, y;
  if (!dir || !last_fb) return;
  if (at) { char key[16]; snprintf(key, sizeof(key), ",%u,", sframe); char list[1024]; snprintf(list, sizeof(list), ",%s,", at); if (!strstr(list, key)) return; }
  else if (!ev || sframe % atoi(ev)) return;
  snprintf(path, sizeof(path), "%s/%s_%05u.ppm", dir, role, sframe);
  if (!(f = fopen(path, "wb"))) return;
  fprintf(f, "P6\n240 160\n255\n");
  for (y = 0; y < 160; y++) for (x = 0; x < 240; x++) {
    uint16_t p = last_fb[y * (last_pitch / 2) + x];
    unsigned char c[3] = { (p >> 11) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3 };
    fwrite(c, 1, 3, f);
  }
  fclose(f);
}

static void vid(const void *d, unsigned w, unsigned h, size_t p) { if (d) { last_fb = d; last_pitch = p; } }
static void aud(int16_t l, int16_t r) {}
static size_t audb(const int16_t *d, size_t f) { return f; }
static void inp(void) {}

static unsigned keys_now(void)
{
  static const char *names[] = { "b", "y", "select", "start", "up", "down", "left", "right", "a", "x", "l", "r" };
  const char *k = getenv("KEYS"); unsigned mask = 0;
  if (!k) return 0;
  while (*k) {
    unsigned from, to; char buf[128]; int n = 0;
    if (sscanf(k, "%u-%u:%127[^;]%n", &from, &to, buf, &n) < 3) break;
    if (sframe >= from && sframe <= to) {
      char *t = strtok(buf, "+");
      for (; t; t = strtok(NULL, "+")) { unsigned i; for (i = 0; i < 12; i++) if (!strcmp(t, names[i])) mask |= 1u << i; }
    }
    k += n; if (*k == ';') k++;
  }
  return mask;
}
static int16_t ist(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
  unsigned m = keys_now();
  if (port || dev != RETRO_DEVICE_JOYPAD) return 0;
  if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)m;
  return (m >> id) & 1;
}
static unsigned long ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000UL + t.tv_nsec / 1000000; }

static void file_mem(const char *path, int write)
{
  void *mem = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM); size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
  FILE *f; if (!path || !mem || !size) return;
  if (!(f = fopen(path, write ? "wb" : "rb"))) return;
  if (write) fwrite(mem, 1, size, f); else if (!fread(mem, 1, size, f)) printf("empty save\n");
  fclose(f);
}

int main(int argc, char **argv)
{
  struct retro_game_info g;
  const char *rom, *out; unsigned frames; unsigned long next; int a = 2, solo;
  role = argv[1];
  if (!strcmp(role, "discover")) {
    net_found_t f[8]; int i, n = net_discover(f, 8, 1500);
    printf("found %d\n", n);
    for (i = 0; i < n; i++) printf("  %s '%s' players %d\n", f[i].ip, f[i].name, f[i].players);
    return n ? 0 : 1;
  }
  solo = !strcmp(role, "solo");
  if (!strcmp(role, "join")) a = 3;
  rom = argv[a]; frames = atoi(argv[a + 1]); out = argv[a + 2];
  retro_set_environment(env); retro_set_video_refresh(vid); retro_set_audio_sample(aud);
  retro_set_audio_sample_batch(audb); retro_set_input_poll(inp); retro_set_input_state(ist);
  retro_init();
  memset(&g, 0, sizeof(g)); g.path = rom;
  if (!retro_load_game(&g)) { printf("load failed\n"); return 2; }
  file_mem(getenv("SAV"), 0);
  net_set_game_name(getenv("NAME") ? getenv("NAME") : "test");
  if (!strcmp(role, "host")) { if (net_host()) { printf("host failed\n"); return 3; } }
  else if (!solo && net_join(argv[2])) { printf("join failed\n"); return 3; }
  next = ms();
  for (frame = 0; ; frame++) {
    int up;
    net_poll();
    retro_run();
    up = solo ? 1 : !strcmp(role, "host") ? net_clients() > 0 : net_state() == NET_CONNECTED;
    if (up) { shot(); sframe++; }
    else if (sframe) { printf("[%s %u] link lost after %u frames\n", role, frame, sframe); break; }
#ifdef MENU_TEST
    if (up && getenv("MENU_AT") && sframe == (unsigned)atoi(getenv("MENU_AT"))) {
      long t0 = ms(), len = getenv("MENU_MS") ? atoi(getenv("MENU_MS")) : 3000;
      printf("[%s %u] menu open\n", role, sframe); fflush(stdout);
      menu_run_begin();
      while (ms() - t0 < len)
        __wrap_plat_sleep_ms(10);       /* what libpicofe's menu does while it waits */
      menu_run_end();
      printf("[%s %u] menu closed after %ld ms\n", role, sframe, ms() - t0); fflush(stdout);
      next = ms();
    }
#endif
    if (sframe >= frames) break;
    if (frame > 60 * 600) { printf("[%s] timeout, linked %u\n", role, sframe); break; }
    if (!getenv("NOPACE")) { next += 17; { long w = (long)(next - ms()); if (w > 0) usleep(w * 1000); else if (w < -500) next = ms(); } }
  }
  if (out) { FILE *f = fopen(out, "wb"); fwrite(retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM), 1, 65536, f); fclose(f); }
  file_mem(getenv("SAVEOUT"), 1);
  printf("[%s] done: %u frames, %u in session\n", role, frame, sframe);
  if (!strcmp(role, "host")) usleep(500000);
  net_stop();
  retro_unload_game(); retro_deinit();
  return sframe >= frames ? 0 : 1;
}
