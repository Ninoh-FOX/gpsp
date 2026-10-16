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
 * Environment: PACE=1 runs each console at 59.73 frames a second, as on the
 * real ones (SKEW_PPM=n: the client's clock is n parts per million faster),
 * BW_KBPS=n gives the network that bandwidth each way, JITTER_MS=n delays
 * each packet up to n ms more (in order, as TCP); SPIKE_MS=n with
 * SPIKE_EVERY=k delays one packet in k (at random) n ms more, and the ones
 * behind it wait (a WiFi having a bad moment). RESET_AT=n: the host
 * player presses reset at frame n of the session.
 *
 * KEYS0 / KEYS1 = "from-to:key+key;...": what each player presses at those
 * frames of the session (keys: a b select start up down left right l r),
 * instead of random buttons. SHOTS=dir saves the screens (dir/cN_FRAME.ppm)
 * at the frames in SHOT_AT="f,f,..." or every SHOT_EVERY frames. With PACE the harness
 * prints how long the setup took and how many frames apart both screens are.
 *
 * Exit status 0 if the session ran FRAMES frames on both consoles without
 * the core reporting a problem. With the link test ROM (linktest.gba) it
 * also checks that both GBAs logged the very same words.
 */
#include <dlfcn.h>
#include <pthread.h>
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
  void (*reset)(void);
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
static int64_t bw_bps = 0, jitter_us = 0, spike_us = 0, spike_every = 0;
static int64_t link_free[2], last_at[2];

typedef struct pkt {
  struct pkt *next;
  int64_t at;
  int to;
  uint16_t from_id;
  size_t len;
  unsigned char data[];
} pkt;

static pkt *q_head, *q_tail;
static pthread_mutex_t q_mtx = PTHREAD_MUTEX_INITIALIZER;

static int64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void net_send(int from, int flags, const void *buf, size_t len, uint16_t client_id) {
  pkt *p = malloc(sizeof(pkt) + len);
  pthread_mutex_lock(&q_mtx);
  (void)flags; (void)client_id;
  p->next = NULL;
  {
    int64_t t = now_us();
    if (bw_bps) {
      link_free[from] = (t > link_free[from] ? t : link_free[from]) +
                        (int64_t)len * 8 * 1000000 / bw_bps;
      t = link_free[from];
    }
    t += latency_us + (jitter_us ? rand() % (jitter_us + 1) : 0);
    if (spike_every && rand() % spike_every == 0)
      t += spike_us;
    if (t < last_at[from]) t = last_at[from];
    p->at = last_at[from] = t;
  }
  p->to = from ^ 1;
  p->from_id = (uint16_t)from;    // host is client id 0, client is 1
  p->len = len;
  memcpy(p->data, buf, len);
  if (q_tail) q_tail->next = p; else q_head = p;
  q_tail = p;
  pthread_mutex_unlock(&q_mtx);
}

static void net_poll(int who) {
  pkt **pp, *prev = NULL, *got = NULL, **got_tail = &got;
  const int64_t t = now_us();
  pthread_mutex_lock(&q_mtx);
  pp = &q_head;
  while (*pp) {
    pkt *p = *pp;
    if (p->to == who && p->at <= t) {
      *pp = p->next;
      if (q_tail == p) q_tail = prev;
      p->next = NULL;
      *got_tail = p;
      got_tail = &p->next;
      continue;
    }
    prev = p;
    pp = &p->next;
  }
  pthread_mutex_unlock(&q_mtx);
  /* (Delivered unlocked: the core answers some of them right away.) */
  while (got) {
    pkt *p = got;
    got = p->next;
    if (con[who].np_set && con[who].np.receive)
      con[who].np.receive(p->data, p->len, p->from_id);
    free(p);
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

static bool shot_wanted(unsigned f) {
  const char *at = getenv("SHOT_AT"), *every = getenv("SHOT_EVERY");
  if (every && atoi(every) > 0 && f % (unsigned)atoi(every) == 0)
    return true;
  while (at && *at) {
    if ((unsigned)atoi(at) == f)
      return true;
    at = strchr(at, ',');
    if (at) at++;
  }
  return false;
}

static void video(int who, const void *data, unsigned w, unsigned h, size_t pitch) {
  if (data) con[who].frames_shown++; else con[who].frames_dupe++;
  if (data && getenv("SHOTS") && shot_wanted(con[who].frames_shown)) {
    char path[512];
    FILE *f;
    unsigned x, y;
    snprintf(path, sizeof(path), "%s/c%d_%05u.ppm", getenv("SHOTS"), who, con[who].frames_shown);
    if ((f = fopen(path, "wb"))) {
      fprintf(f, "P6 %u %u 255\n", w, h);
      for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
          const uint16_t p = ((const uint16_t *)((const uint8_t *)data + y * pitch))[x];
          const unsigned char rgb[3] = { (p >> 11) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3 };
          fwrite(rgb, 1, 3, f);
        }
      fclose(f);
    }
  }
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
  SYM(get_memory_data) SYM(reset)
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

/* KEYS0 / KEYS1: "from-to:key+key;..." */
static bool scripted_keys(int who, unsigned frame, uint32_t *keys) {
  static const struct { const char *n; int id; } names[] = {
    { "a", RETRO_DEVICE_ID_JOYPAD_A }, { "b", RETRO_DEVICE_ID_JOYPAD_B },
    { "select", RETRO_DEVICE_ID_JOYPAD_SELECT }, { "start", RETRO_DEVICE_ID_JOYPAD_START },
    { "up", RETRO_DEVICE_ID_JOYPAD_UP }, { "down", RETRO_DEVICE_ID_JOYPAD_DOWN },
    { "left", RETRO_DEVICE_ID_JOYPAD_LEFT }, { "right", RETRO_DEVICE_ID_JOYPAD_RIGHT },
    { "l", RETRO_DEVICE_ID_JOYPAD_L }, { "r", RETRO_DEVICE_ID_JOYPAD_R },
  };
  const char *s = getenv(who ? "KEYS1" : "KEYS0");
  if (!s)
    return false;
  *keys = 0;
  while (*s) {
    char *e;
    const unsigned from = (unsigned)strtoul(s, &e, 10);
    unsigned to = from;
    if (*e == '-') to = (unsigned)strtoul(e + 1, &e, 10);
    if (*e == ':') e++;
    while (*e && *e != ';') {
      size_t n = strcspn(e, "+;");
      unsigned k;
      for (k = 0; k < sizeof(names) / sizeof(names[0]); k++)
        if (strlen(names[k].n) == n && !strncmp(e, names[k].n, n) && frame >= from && frame <= to)
          *keys |= 1u << names[k].id;
      e += n;
      if (*e == '+') e++;
    }
    s = *e ? e + 1 : e;
  }
  return true;
}

/* Buttons that change now and then, different for each player. */
static uint32_t script_keys(int who, unsigned frame) {
  uint32_t x = (frame / 9) * 2654435761u + who * 40503u;
  uint32_t k;
  if (scripted_keys(who, frame, &k))
    return k;
  x ^= x >> 13;
  return x & 0x0FF;   /* A B Select Start and the pad, no L/R/X/Y */
}

/* PACE: one thread per console. The host's thread also keeps the stats:
 * how many frames apart both screens are, sampled at each host frame. */
static struct {
  int64_t t0, t_start[2];
  unsigned frames;
  double ppm;
  volatile int stop, half;
  unsigned dupe_half[2];
  volatile int64_t t_last[2];
  double diff_sum, diff_min, diff_max;
  long long diff_n;
} pace = { .diff_min = 1e9, .diff_max = -1e9 };

static void *pace_main(void *arg) {
  const int i = (int)(intptr_t)arg;
  const double period = 1e6 / 59.7275 * (i ? 1 - pace.ppm / 1e6 : 1);
  const int64_t limit = pace.t0 + 60 * 1000000LL + (int64_t)pace.frames * 40000;
  double next = (double)pace.t0;
  while (!pace.stop && now_us() < limit &&
         (con[0].frames_shown < pace.frames || con[1].frames_shown < pace.frames)) {
    int64_t t = now_us();
    if (next > t) usleep((useconds_t)(next - t));
    con[i].keys = script_keys(i, con[i].frames_shown);
    net_poll(i);
    con[i].run();
    t = now_us();
    pace.t_last[i] = t;
    next += period;
    if (next < t - period) next = t;      /* missed a frame */
    if (!pace.t_start[i] && con[i].frames_shown)
      pace.t_start[i] = t;
    if (con[0].bad || con[1].bad) pace.stop = 1;
    if (i || !pace.t_start[0] || !pace.t_start[1]) continue;
    if (!pace.half && con[0].frames_shown >= pace.frames / 2 &&
        con[1].frames_shown >= pace.frames / 2) {
      pace.half = 1;
      pace.dupe_half[0] = con[0].frames_dupe;
      pace.dupe_half[1] = con[1].frames_dupe;
    }
    if (pace.half && con[0].frames_shown < pace.frames && con[1].frames_shown < pace.frames) {
      /* Where the client is: its last frame, plus the time since. */
      const double d = (double)con[0].frames_shown - (double)con[1].frames_shown -
                       (double)(t - pace.t_last[1]) / period;
      pace.diff_sum += d; pace.diff_n++;
      if (d < pace.diff_min) pace.diff_min = d;
      if (d > pace.diff_max) pace.diff_max = d;
    }
  }
  return NULL;
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
  bw_bps = getenv("BW_KBPS") ? atoll(getenv("BW_KBPS")) * 1000 : 0;
  jitter_us = getenv("JITTER_MS") ? atoll(getenv("JITTER_MS")) * 1000 : 0;
  spike_us = getenv("SPIKE_MS") ? atoll(getenv("SPIKE_MS")) * 1000 : 0;
  spike_every = getenv("SPIKE_EVERY") ? atoll(getenv("SPIKE_EVERY")) : 0;
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
  {
    int64_t ts = now_us();
    con[0].np.start(0, np_send0, np_poll0);
    printf("host session start took %.0f ms\n", (now_us() - ts) / 1e3);
    ts = now_us();
    con[1].np.start(1, np_send1, np_poll1);
    printf("client session start took %.0f ms\n", (now_us() - ts) / 1e3);
  }
  /* LATE_CONNECT=N: the host hears about the client N frames after the
   * client started (as RetroArch can do), so the client's first packets
   * get to a host that is not listening yet. */
  if (getenv("LATE_CONNECT")) {
    unsigned late = (unsigned)atoi(getenv("LATE_CONNECT"));
    for (f = 0; f < late; f++) {
      con[0].keys = 0;
      net_poll(0);
      con[0].run();
      con[1].keys = 0;
      net_poll(1);
      con[1].run();
    }
    printf("host told about the client %u frames late\n", late);
  }
  if (con[0].np.connected && !con[0].np.connected(1)) { fprintf(stderr, "host refused client\n"); return 2; }

  for (i = 0; i < 2; i++)
    con[i].frames_shown = con[i].frames_dupe = 0;

  if (getenv("PACE")) {
    /* Each console on its own clock and its own thread, as two real ones. */
    pthread_t th[2];
    pace.t0 = now_us();
    pace.frames = frames;
    pace.ppm = getenv("SKEW_PPM") ? atof(getenv("SKEW_PPM")) : 0;
    for (i = 0; i < 2; i++)
      pthread_create(&th[i], NULL, pace_main, (void *)(intptr_t)i);
    for (i = 0; i < 2; i++)
      pthread_join(th[i], NULL);
    printf("session: host %u frames (%u repeated), client %u frames (%u repeated), %.1f s\n",
           con[0].frames_shown, con[0].frames_dupe, con[1].frames_shown, con[1].frames_dupe,
           (now_us() - pace.t0) / 1e6);
    if (pace.t_start[0] && pace.t_start[1])
      printf("setup: host plays after %.0f ms, client after %.0f ms\n",
             (pace.t_start[0] - pace.t0) / 1e3, (pace.t_start[1] - pace.t0) / 1e3);
    if (pace.diff_n)
      printf("second half: host ahead of client by (frames): avg %.2f, min %.2f, max %.2f; "
             "repeated host %u, client %u\n", (double)pace.diff_sum / pace.diff_n,
             pace.diff_min, pace.diff_max,
             con[0].frames_dupe - pace.dupe_half[0], con[1].frames_dupe - pace.dupe_half[1]);
  }
  else {
    const int64_t t0 = now_us();
    unsigned rounds = 0;
    const int64_t limit = t0 + 120 * 1000000LL + (int64_t)frames * 100000;
    const unsigned reset_at = getenv("RESET_AT") ? (unsigned)atoi(getenv("RESET_AT")) : 0;
    while ((con[0].frames_shown < frames || con[1].frames_shown < frames) && now_us() < limit) {
      if (reset_at && con[0].frames_shown == reset_at) {
        printf("host presses reset at frame %u\n", reset_at);
        con[0].reset();
        con[0].frames_shown++;    /* (only once) */
      }
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
