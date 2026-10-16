/* dualtest: two consoles with RetroArch, in one process, for the Dual Cable
 * Link of gpSP.
 *
 * Loads two copies of the core (each copy is a console: two dlopen'ed files,
 * so each has its own globals), plays the frontend for both and joins their
 * netpacket interfaces with a network in memory that adds a fixed latency.
 * Messages from the core are printed with the console they come from.
 *
 *   dualtest CORE.so ROM FRAMES [latency_ms] [solo_frames]
 *
 * Exit status 0 if the session ran FRAMES frames on both consoles without
 * the core reporting a problem. With the link test ROM (linktest.gba) it
 * also checks that both GBAs logged the very same words.
 */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "libretro.h"

#define MAXPKT 4096

typedef struct {
  void *h;
  void (*set_environment)(retro_environment_t);
  void (*set_video_refresh)(retro_video_refresh_t);
  void (*set_audio_sample)(retro_audio_sample_t);
  void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
  void (*set_input_poll)(retro_input_poll_t);
  void (*set_input_state)(retro_input_state_t);
  void (*init)(void);
  void (*deinit)(void);
  bool (*load_game)(const struct retro_game_info *);
  void (*unload_game)(void);
  void (*run)(void);
  void *(*get_memory_data)(unsigned);

  struct retro_netpacket_callback np;
  bool np_set;
  int id;                 // 0 = host, 1 = client
  unsigned frames_shown;  // Frames that were run (not repeated)
  unsigned frames_dupe;
  uint32_t keys;
  bool bad;
} console;

static console con[2];
static int latency_us = 0;

typedef struct pkt {
  struct pkt *next;
  int64_t at;
  int to;
  uint16_t from_id;
  size_t len;
  unsigned char data[];
} pkt;

static pkt *q_head, *q_tail;

static int64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void net_send(int from, int flags, const void *buf, size_t len, uint16_t client_id) {
  pkt *p = malloc(sizeof(pkt) + len);
  (void)flags; (void)client_id;
  p->next = NULL;
  p->at = now_us() + latency_us;
  p->to = from ^ 1;
  p->from_id = (uint16_t)from;    // host is client id 0, client is 1
  p->len = len;
  memcpy(p->data, buf, len);
  if (q_tail) q_tail->next = p; else q_head = p;
  q_tail = p;
}

static void net_poll(int who) {
  pkt **pp = &q_head, *prev = NULL;
  const int64_t t = now_us();
  while (*pp) {
    pkt *p = *pp;
    if (p->to == who && p->at <= t) {
      *pp = p->next;
      if (q_tail == p) q_tail = prev;
      if (con[who].np_set && con[who].np.receive)
        con[who].np.receive(p->data, p->len, p->from_id);
      free(p);
      continue;
    }
    prev = p;
    pp = &p->next;
  }
}

/* ---- frontend callbacks, one set per console ---- */

static const char *opt(const char *key) {
  if (!strcmp(key, "gpsp_serial")) return getenv("SERIAL") ? getenv("SERIAL") : "mul_cable";
  if (!strcmp(key, "gpsp_bios")) return "builtin";
  if (!strcmp(key, "gpsp_boot_mode")) return "game";
  if (!strcmp(key, "gpsp_drc")) return "enabled";
  if (!strcmp(key, "gpsp_frameskip")) return "disabled";
  return NULL;
}

static bool env(int who, unsigned cmd, void *data) {
  switch (cmd) {
  case RETRO_ENVIRONMENT_GET_VARIABLE: {
    struct retro_variable *v = data;
    v->value = opt(v->key);
    return v->value != NULL;
  }
  case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
  case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    *(const char **)data = ".";
    return true;
  case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
  case RETRO_ENVIRONMENT_GET_CAN_DUPE:
    if (data) *(bool *)data = true;
    return true;
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    return true;
  case RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE:
    if (data) {
      con[who].np = *(const struct retro_netpacket_callback *)data;
      con[who].np_set = true;
    } else
      con[who].np_set = false;
    return true;
  case RETRO_ENVIRONMENT_SET_MESSAGE: {
    const struct retro_message *m = data;
    printf("[console %d] message: %s\n", who, m->msg);
    return true;
  }
  case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
    const struct retro_message_ext *m = data;
    printf("[console %d] message: %s\n", who, m->msg);
    if (strstr(m->msg, "out of sync") || strstr(m->msg, "could not") ||
        strstr(m->msg, "not enough") || strstr(m->msg, "did not answer"))
      con[who].bad = true;
    return true;
  }
  case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
    *(unsigned *)data = 1;
    return true;
  case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
    *(bool *)data = false;
    return true;
  default:
    return false;
  }
}

static void video(int who, const void *data, unsigned w, unsigned h, size_t pitch) {
  if (data) con[who].frames_shown++; else con[who].frames_dupe++;
}

static int16_t input_state(int who, unsigned port, unsigned dev, unsigned idx, unsigned id) {
  if (port || dev != RETRO_DEVICE_JOYPAD) return 0;
  if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)con[who].keys;
  return (con[who].keys >> id) & 1;
}

static void np_send0(int f, const void *b, size_t l, uint16_t c) { net_send(0, f, b, l, c); }
static void np_send1(int f, const void *b, size_t l, uint16_t c) { net_send(1, f, b, l, c); }
static void np_poll0(void) { net_poll(0); }
static void np_poll1(void) { net_poll(1); }

#define CBSET(n) \
  static bool env##n(unsigned c, void *d) { return env(n, c, d); } \
  static void video##n(const void *d, unsigned w, unsigned h, size_t p) { video(n, d, w, h, p); } \
  static int16_t input##n(unsigned p, unsigned d, unsigned i, unsigned id) { return input_state(n, p, d, i, id); }
CBSET(0)
CBSET(1)

static void audio(int16_t l, int16_t r) {}
static size_t audio_batch(const int16_t *d, size_t f) { return f; }
static void input_poll(void) {}

static void load_core(console *c, const char *path, int who) {
#define SYM(f) c->f = dlsym(c->h, "retro_" #f); if (!c->f) { fprintf(stderr, "missing retro_%s\n", #f); exit(2); }
  c->h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!c->h) { fprintf(stderr, "dlopen %s: %s\n", path, dlerror()); exit(2); }
  SYM(set_environment) SYM(set_video_refresh) SYM(set_audio_sample)
  SYM(set_audio_sample_batch) SYM(set_input_poll) SYM(set_input_state)
  SYM(init) SYM(deinit) SYM(load_game) SYM(unload_game) SYM(run)
  SYM(get_memory_data)
  c->id = who;
  c->set_environment(who ? env1 : env0);
  c->set_video_refresh(who ? video1 : video0);
  c->set_audio_sample(audio);
  c->set_audio_sample_batch(audio_batch);
  c->set_input_poll(input_poll);
  c->set_input_state(who ? input1 : input0);
  printf("  init\n");
  c->init();
  printf("  init done\n");
}

static bool copy_file(const char *from, const char *to) {
  FILE *a = fopen(from, "rb"), *b = fopen(to, "wb");
  char buf[65536];
  size_t n;
  bool ok = a && b;
  while (ok && (n = fread(buf, 1, sizeof(buf), a)) > 0)
    ok = fwrite(buf, 1, n, b) == n;
  if (a) fclose(a);
  if (b) fclose(b);
  return ok;
}

/* Buttons that change now and then, different for each player. */
static uint32_t script_keys(int who, unsigned frame) {
  uint32_t x = (frame / 9) * 2654435761u + who * 40503u;
  x ^= x >> 13;
  return x & 0x0FF;   /* A B Select Start and the pad, no L/R/X/Y */
}

int main(int argc, char **argv) {
  const char *core, *rom;
  unsigned frames, solo, f;
  struct retro_game_info info;
  char copy[2][256];
  int i, ok = 1;

  setvbuf(stdout, NULL, _IOLBF, 0);
  if (argc < 4) {
    fprintf(stderr, "usage: %s CORE.so ROM FRAMES [latency_ms] [solo_frames]\n", argv[0]);
    return 2;
  }
  core = argv[1];
  rom = argv[2];
  frames = (unsigned)atoi(argv[3]);
  latency_us = argc > 4 ? atoi(argv[4]) * 1000 : 0;
  solo = argc > 5 ? (unsigned)atoi(argv[5]) : 30;

  /* Two files: dlopen gives each its own globals. */
  for (i = 0; i < 2; i++) {
    snprintf(copy[i], sizeof(copy[i]), "/tmp/dualtest_core%d_%d.so", i, (int)getpid());
    if (!copy_file(core, copy[i])) { fprintf(stderr, "cannot copy the core\n"); return 2; }
    printf("console %d: loading core\n", i);
    load_core(&con[i], copy[i], i);
    printf("console %d: loading game\n", i);
    memset(&info, 0, sizeof(info));
    /* ROM2 in the environment: the client has another game. */
    info.path = (i == 1 && getenv("ROM2")) ? getenv("ROM2") : rom;
    if (!con[i].load_game(&info)) { fprintf(stderr, "load_game failed\n"); return 2; }
    printf("console %d: ready\n", i);
  }

  /* Each player plays alone for a while, so that the two GBAs differ. */
  for (f = 0; f < solo; f++)
    for (i = 0; i < 2; i++) {
      con[i].keys = script_keys(i, f + 1000 * i);
      con[i].run();
    }
  printf("solo frames done (%u)\n", solo);

  /* Netplay: the host starts, the client joins. */
  if (!con[0].np_set || !con[1].np_set) { fprintf(stderr, "no netpacket interface\n"); return 2; }
  con[0].np.start(0, np_send0, np_poll0);
  con[1].np.start(1, np_send1, np_poll1);
  if (con[0].np.connected && !con[0].np.connected(1)) { fprintf(stderr, "host refused client\n"); return 2; }

  for (i = 0; i < 2; i++)
    con[i].frames_shown = con[i].frames_dupe = 0;

  {
    const int64_t t0 = now_us();
    unsigned rounds = 0;
    const int64_t limit = t0 + 120 * 1000000LL + (int64_t)frames * 100000;
    while ((con[0].frames_shown < frames || con[1].frames_shown < frames) && now_us() < limit) {
      for (i = 0; i < 2; i++) {
        con[i].keys = script_keys(i, con[i].frames_shown);
        net_poll(i);
        con[i].run();
      }
      rounds++;
      if (rounds % 50 == 0)
        printf("  round %u: host %u (+%u rep), client %u (+%u rep)\n", rounds,
               con[0].frames_shown, con[0].frames_dupe,
               con[1].frames_shown, con[1].frames_dupe);
      if (con[0].bad || con[1].bad) break;
    }
    printf("session: host %u frames (%u repeated), client %u frames (%u repeated), %.1f s\n",
           con[0].frames_shown, con[0].frames_dupe, con[1].frames_shown, con[1].frames_dupe,
           (now_us() - t0) / 1e6);
  }

  if (con[0].bad || con[1].bad || con[0].frames_shown < frames || con[1].frames_shown < frames)
    ok = 0;

  /* Link test ROM: both GBAs must have logged the same words. */
  {
    const uint32_t *e0 = con[0].get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    const uint32_t *e1 = con[1].get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    if (e0 && e1 && (e0[2] == 1 || e0[2] == 2) && (e1[2] == 1 || e1[2] == 2)) {
      unsigned n0 = e0[0], n1 = e1[0], n = n0 < n1 ? n0 : n1, k, diff = 0;
      if (n > 8192) n = 8192;
      for (k = 0; k < n; k++)
        if (e0[4 + k] != e1[4 + k]) diff++;
      printf("link test: host role %u, client role %u, transfers %u / %u, "
             "first %u compared, %u differ\n", e0[2], e1[2], n0, n1, n, diff);
      if (n > 0)
        printf("  first words: %08x %08x %08x  /  %08x %08x %08x\n",
               e0[4], e0[5], e0[6], e1[4], e1[5], e1[6]);
      if (e0[2] != 1 || e1[2] != 2 || n < 10 || diff)
        ok = 0;
    }
  }

  for (i = 0; i < 2; i++) {
    if (con[i].np_set && con[i].np.stop) con[i].np.stop();
    con[i].unload_game();
    con[i].deinit();
    unlink(copy[i]);
  }
  printf("%s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
