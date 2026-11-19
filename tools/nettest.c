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

static const char *role;
static unsigned frame;

void net_message(const char *text, unsigned ms) { printf("[%s %u] MSG %s\n", role, frame, text); fflush(stdout); }

static void logp(enum retro_log_level l, const char *fmt, ...) { va_list a; va_start(a, fmt); printf("[%s %u] LOG ", role, frame); vprintf(fmt, a); va_end(a); fflush(stdout); }

static bool env(unsigned cmd, void *data)
{
  switch (cmd) {
  case RETRO_ENVIRONMENT_GET_VARIABLE: {
    struct retro_variable *v = data;
    v->value = NULL;
    if (!strcmp(v->key, "gpsp_serial")) v->value = getenv("SERIAL") ? getenv("SERIAL") : "mul_cable";
    else if (!strcmp(v->key, "gpsp_cable_players")) v->value = getenv("PLAYERS") ? getenv("PLAYERS") : "2";
    else if (!strcmp(v->key, "gpsp_bios")) v->value = "builtin";
    else if (!strcmp(v->key, "gpsp_drc")) v->value = "enabled";
    return v->value != NULL;
  }
  case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data = false; return true;
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return true;
  case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
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
static void vid(const void *d, unsigned w, unsigned h, size_t p) {}
static void aud(int16_t l, int16_t r) {}
static size_t audb(const int16_t *d, size_t f) { return f; }
static void inp(void) {}
static int16_t ist(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
  /* some buttons, different per console and over time */
  unsigned k = (frame / 7 + (role[0] == 'h' ? 0 : 3)) % 10;
  if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)(1 << k);
  return id == k;
}
static unsigned long ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000UL + t.tv_nsec / 1000000; }

int main(int argc, char **argv)
{
  struct retro_game_info g;
  const char *rom, *out; unsigned frames, linked = 0; unsigned long next; int a = 2;
  role = argv[1];
  if (!strcmp(role, "discover")) {
    net_found_t f[8]; int i, n = net_discover(f, 8, 1500);
    printf("found %d\n", n);
    for (i = 0; i < n; i++) printf("  %s '%s' players %d\n", f[i].ip, f[i].name, f[i].players);
    return n ? 0 : 1;
  }
  if (!strcmp(role, "join")) a = 3;
  rom = argv[a]; frames = atoi(argv[a + 1]); out = argv[a + 2];
  retro_set_environment(env); retro_set_video_refresh(vid); retro_set_audio_sample(aud);
  retro_set_audio_sample_batch(audb); retro_set_input_poll(inp); retro_set_input_state(ist);
  retro_init();
  memset(&g, 0, sizeof(g)); g.path = rom;
  if (!retro_load_game(&g)) { printf("load failed\n"); return 2; }
  net_set_game_name("linktest");
  if (!strcmp(role, "host")) { if (net_host()) { printf("host failed\n"); return 3; } }
  else if (net_join(argv[2])) { printf("join failed\n"); return 3; }
  next = ms();
  for (frame = 0; ; frame++) {
    int up;
    net_poll();
    retro_run();
    up = !strcmp(role, "host") ? net_clients() > 0 : net_state() == NET_CONNECTED;
    if (up) linked++;
    else if (linked) { printf("[%s %u] link lost after %u frames\n", role, frame, linked); break; }
    if (linked >= frames) break;
    if (frame > 60 * 120) { printf("[%s] timeout, linked %u\n", role, linked); break; }
    next += 17; { long w = (long)(next - ms()); if (w > 0) usleep(w * 1000); else if (w < -500) next = ms(); }
  }
  {
    FILE *f = fopen(out, "wb");
    fwrite(retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM), 1, 65536, f); fclose(f);
  }
  printf("[%s] done: %u frames, %u linked\n", role, frame, linked);
  if (!strcmp(role, "join")) net_stop(); else { usleep(500000); net_stop(); }
  retro_unload_game(); retro_deinit();
  return linked >= frames ? 0 : 1;
}
