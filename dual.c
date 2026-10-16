/* gameplaySP
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

// Dual Cable Link: the session (instance A only). See dual.h.
//
// Both consoles run both GBAs: the local one (instance A, the one the
// frontend shows) and a copy of the other player's (instance B, hidden). The
// two GBAs of a console are joined by a cable in memory (the bus below), and
// only the buttons cross the network.
//
//   host                                 client
//     HELLO (ROM, BIOS, settings) <---->  HELLO
//     PING/PONG (round trip time)
//     STATE + SRAM of its own GBA <---->  STATE + SRAM of its own GBA
//     READY (input delay)         <---->  READY
//     INPUT (frame, buttons)      <---->  INPUT, every frame
//     HASH (both GBAs, sometimes) <---->  HASH: both copies must agree
//
// Both games are restarted when the cable is plugged in (games only look for
// it at boot), keeping each player's save: each GBA is copied to the other
// console. If anything does not match (other
// ROM, BIOS, settings or core) the real time cable is used instead.
//
// The buttons for frame N are sent at frame N - delay, and every console
// runs frame N with the same buttons on both GBAs: both consoles compute the
// same two machines. A console that does not have the other's buttons yet
// shows the last frame again and tries later.

#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <unistd.h>
#include "common.h"
#include "dual.h"
#include "libretro.h"
#include <streams/file_stream.h>

/* ---- instance B (the same core, globals renamed) ------------------------ */

void gbab_retro_set_environment(retro_environment_t);
void gbab_retro_set_video_refresh(retro_video_refresh_t);
void gbab_retro_set_audio_sample(retro_audio_sample_t);
void gbab_retro_set_audio_sample_batch(retro_audio_sample_batch_t);
void gbab_retro_set_input_poll(retro_input_poll_t);
void gbab_retro_set_input_state(retro_input_state_t);
void gbab_retro_init(void);
void gbab_retro_deinit(void);
bool gbab_retro_load_game(const struct retro_game_info *);
void gbab_retro_unload_game(void);
void gbab_retro_run(void);
bool gbab_retro_unserialize(const void *, size_t);
void *gbab_retro_get_memory_data(unsigned);

void gbab_dualport_begin(unsigned slot);
void gbab_dualport_end(void);
void gbab_dualport_set_keys(uint32_t keys);
void gbab_rtc_dual_begin(bool owner);
void gbab_rtc_dual_end(void);
extern int gbab_serial_mode;

extern u8 gbab_ewram[], gbab_iwram[], gbab_vram[];
extern u16 gbab_oam_ram[], gbab_palette_ram[], gbab_io_registers[];
extern u32 gbab_reg[];

/* ---- protocol ------------------------------------------------------------- */

#define DUAL_HEADER        0x4d434431   // MCD1 (Multiplayer Cable, Dual)
#define DUAL_VERSION       1

#define DPK_HELLO          1   // version, rom crc, bios crc, settings, host
#define DPK_PING           2   // time
#define DPK_PONG           3   // time (echoed)
#define DPK_STATE          4   // offset, total size, data
#define DPK_READY          5   // input delay (host)
#define DPK_INPUT          6   // frame, buttons
#define DPK_HASH           7   // frame, hash slot 0, hash slot 1
#define DPK_BYE            8   // reason

#define DUAL_CHUNK         (16 * 1024)
#define DUAL_SRAM_SIZE     (1024 * 128)
#define DUAL_BLOB_SIZE     (GBA_STATE_MEM_SIZE + DUAL_SRAM_SIZE)
#define DUAL_RING          256         // Frames of buttons kept
#define DUAL_HASH_EVERY    120         // Frames between agreement checks
#define DUAL_HASH_SLOTS    4
#define DUAL_HELLO_US      3000000     // No answer: the other core is not dual
#define DUAL_SETUP_US      30000000    // Whole setup
#define DUAL_PEER_US       15000000    // Peer silent while playing: it is gone
#define DUAL_STALL_US      12000       // Longest wait for buttons in a frame
#define DUAL_FRAME_US      16743
#define DUAL_PINGS         4           // Round trips measured before the states
#define DUAL_PING_US       1500000     // ...for at most this long

enum { DS_OFF = 0, DS_HELLO, DS_PING, DS_TRANSFER, DS_READY, DS_RUN };

static struct {
  // Frontend side
  retro_environment_t environ_cb;
  void (*message)(const char *text, unsigned ms);
  char rom_path[1024];
  u32 rom_crc;
  bool rom_crc_valid;
  u32 cfg;                      // Settings that change the machine

  // Instance B
  bool b_loaded;
  pthread_t b_thread;
  bool b_thread_ok;
  sem_t b_start, b_done;
  volatile bool b_quit;

  // Session
  int state;
  bool is_host;
  int peer_id;                  // libretro client id of the other console
  unsigned my_slot;             // 0 on the host, 1 on the client
  u64 t_state;                  // When the current step started
  u64 t_heard;                  // Last packet from the peer
  bool hello_sent, hello_rcvd;
  u32 peer_hello[6];
  u32 rtt_us, rtt_samples;
  bool peer_ready;
  u32 delay;                    // Input delay, frames

  u8 *own_blob, *peer_blob;
  u32 peer_rcvd;
  bool own_sent;
  bool loaded;

  u32 frame;                    // Next frame to run
  u32 sent_upto;                // Buttons sent for frames < this
  u32 own_keys[DUAL_RING];
  u32 peer_keys[DUAL_RING];
  u32 peer_frame_ok[DUAL_RING]; // frame + 1 when peer_keys[] is valid
  // Agreement checks: [frame + 1, slot 0 GBA, slot 1 GBA], ours and theirs
  u32 my_hash[DUAL_HASH_SLOTS][3];
  u32 peer_hash[DUAL_HASH_SLOTS][3];
} ds;

void netpacket_send(uint16_t client_id, const void *buf, size_t len);
void netpacket_poll_receive();
bool netpacket_active(void);
u64 netpacket_time_usec(void);

static u32 crc32_table[256];

static u32 dual_crc32(u32 crc, const void *data, size_t len) {
  const u8 *p = (const u8*)data;
  if (!crc32_table[1]) {
    u32 i, j;
    for (i = 0; i < 256; i++) {
      u32 c = i;
      for (j = 0; j < 8; j++)
        c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
      crc32_table[i] = c;
    }
  }
  crc = ~crc;
  while (len--)
    crc = crc32_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

static void dual_send(u32 type, const u32 *words, u32 nwords,
                      const void *data, u32 len) {
  u8 buf[8 + 8 * 4 + DUAL_CHUNK];
  u32 *hdr = (u32*)buf;
  u32 i;
  if (ds.peer_id < 0)
    return;
  hdr[0] = netorder32(DUAL_HEADER);
  hdr[1] = netorder32(type);
  for (i = 0; i < nwords; i++)
    hdr[2 + i] = netorder32(words[i]);
  if (len)
    memcpy(&buf[8 + nwords * 4], data, len);
  netpacket_send(ds.peer_id, buf, 8 + nwords * 4 + len);
}

static void dual_msg(const char *text) {
  if (ds.message)
    ds.message(text, 3000);
}

/* ---- the bus: the cable between the two GBAs of this console ------------- */

#define DUAL_QSIZE   1024

static struct {
  u64 pub[2];                   // Published clocks (atomic)
  dual_event q[2][DUAL_QSIZE];  // Events FOR each slot
  u32 qhead[2], qcount[2];
  u32 rep_seq, rep_word, rep_valid;   // The child's answer (for the parent)
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  int waiters;
  u64 notified[2];              // pub[] when we last woke the other one up
  volatile int abort;
} bus = { .mtx = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

static void bus_reset(void) {
  pthread_mutex_lock(&bus.mtx);
  __atomic_store_n(&bus.pub[0], 0, __ATOMIC_SEQ_CST);
  __atomic_store_n(&bus.pub[1], 0, __ATOMIC_SEQ_CST);
  bus.qhead[0] = bus.qhead[1] = 0;
  bus.qcount[0] = bus.qcount[1] = 0;
  bus.rep_valid = 0;
  bus.notified[0] = bus.notified[1] = 0;
  bus.abort = 0;
  pthread_mutex_unlock(&bus.mtx);
}

// Waking a sleeping thread up costs some tens of microseconds, and the child
// is never more than a few hundred cycles behind the parent: waking it up at
// every scheduler call would add up to milliseconds per frame. So a GBA
// wakes the other one up only every DUAL_NOTIFY_STEP cycles, or whenever it
// stops itself (it waits, or its frame is over). This decides when the other
// thread wakes up, never what it computes.
#define DUAL_NOTIFY_STEP 4096

static void bus_notify(unsigned slot, bool force) {
  if (__atomic_load_n(&bus.waiters, __ATOMIC_SEQ_CST)) {
    const u64 p = __atomic_load_n(&bus.pub[slot], __ATOMIC_SEQ_CST);
    if (force || p >= bus.notified[slot] + DUAL_NOTIFY_STEP) {
      bus.notified[slot] = p;
      pthread_mutex_lock(&bus.mtx);
      pthread_cond_broadcast(&bus.cond);
      pthread_mutex_unlock(&bus.mtx);
    }
  }
}

// Whether a GBA at `clock` may go on: the child never passes the parent,
// the parent does not get more than the lookahead ahead of the child.
static bool bus_may_run(unsigned slot, u64 clock) {
  if (slot)
    return __atomic_load_n(&bus.pub[0], __ATOMIC_SEQ_CST) >= clock;
  return __atomic_load_n(&bus.pub[1], __ATOMIC_SEQ_CST) + DUAL_LOOKAHEAD >= clock;
}

static bool bus_has_reply(u32 seq) {
  return __atomic_load_n(&bus.rep_valid, __ATOMIC_SEQ_CST) &&
         __atomic_load_n(&bus.rep_seq, __ATOMIC_SEQ_CST) == seq;
}

// Spin for a little while (the other GBA runs on the other CPU core and is
// usually about to get there), then sleep until woken up.
#ifdef DUAL_DEBUG
  #define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
  #define DBG(...)
#endif

#define BUS_WAIT(ready)                                                      \
  do {                                                                      \
    unsigned spins;                                                         \
    for (spins = 0; !(ready) && !bus.abort; spins++) {                       \
      if (spins < 200)                                                      \
        sched_yield();                                                      \
      else {                                                                \
        DBG("[bus] %s:%d sleeps, pub %llu %llu rep %u/%u\n", __func__,       \
            __LINE__, (unsigned long long)bus.pub[0],                       \
            (unsigned long long)bus.pub[1], bus.rep_seq, bus.rep_valid);    \
        pthread_mutex_lock(&bus.mtx);                                       \
        __atomic_add_fetch(&bus.waiters, 1, __ATOMIC_SEQ_CST);              \
        while (!(ready) && !bus.abort)                                       \
          pthread_cond_wait(&bus.cond, &bus.mtx);                           \
        __atomic_sub_fetch(&bus.waiters, 1, __ATOMIC_SEQ_CST);              \
        pthread_mutex_unlock(&bus.mtx);                                     \
        break;                                                              \
      }                                                                     \
    }                                                                       \
  } while (0)

// Publish how far we are (nothing new from us will be stamped at or before
// `publish`), then wait until we may run at `clock`.
void dualbus_sync(unsigned slot, uint64_t publish, uint64_t clock) {
  bool ready;
  DBG("[bus] slot %u at %llu\n", slot, (unsigned long long)clock);
  __atomic_store_n(&bus.pub[slot], publish, __ATOMIC_SEQ_CST);
  ready = bus_may_run(slot, clock);
  // (clock 0: our frame is over, we stop until the next one)
  bus_notify(slot, !ready || !clock);
  if (!ready)
    BUS_WAIT(bus_may_run(slot, clock));
}

void dualbus_post(unsigned from_slot, const dual_event *ev) {
  const unsigned to = from_slot ^ 1;
  DBG("[bus] slot %u posts kind %u t %llu\n", from_slot, ev->kind, (unsigned long long)ev->t);
  pthread_mutex_lock(&bus.mtx);
  if (bus.qcount[to] < DUAL_QSIZE) {
    bus.q[to][(bus.qhead[to] + bus.qcount[to]) % DUAL_QSIZE] = *ev;
    bus.qcount[to]++;
  }
  pthread_mutex_unlock(&bus.mtx);
}

// Takes the oldest event for `slot` if it is due at `now`. What the child
// does reaches the parent DUAL_LOOKAHEAD cycles later.
int dualbus_pop(unsigned slot, uint64_t now, dual_event *ev) {
  int ret = 0;
  pthread_mutex_lock(&bus.mtx);
  if (bus.qcount[slot]) {
    const dual_event *h = &bus.q[slot][bus.qhead[slot]];
    if (h->t + (slot ? 0 : DUAL_LOOKAHEAD) <= now) {
      *ev = *h;
      bus.qhead[slot] = (bus.qhead[slot] + 1) % DUAL_QSIZE;
      bus.qcount[slot]--;
      ret = 1;
    }
  }
  pthread_mutex_unlock(&bus.mtx);
  return ret;
}

void dualbus_reply(unsigned from_slot, uint32_t seq, uint32_t word, int active) {
  pthread_mutex_lock(&bus.mtx);
  __atomic_store_n(&bus.rep_word, active ? word : 0xFFFF, __ATOMIC_SEQ_CST);
  __atomic_store_n(&bus.rep_seq, seq, __ATOMIC_SEQ_CST);
  __atomic_store_n(&bus.rep_valid, 1, __ATOMIC_SEQ_CST);
  pthread_cond_broadcast(&bus.cond);
  pthread_mutex_unlock(&bus.mtx);
}

int dualbus_wait_reply(unsigned slot, uint32_t seq, uint32_t *word) {
  DBG("[bus] slot %u waits reply %u\n", slot, seq);
  bus_notify(slot, true);
  BUS_WAIT(bus_has_reply(seq));
  if (!bus_has_reply(seq))
    return 0;
  *word = __atomic_load_n(&bus.rep_word, __ATOMIC_SEQ_CST);
  return 1;
}

/* ---- instance B ------------------------------------------------------------ */

static bool b_environ(unsigned cmd, void *data) {
  switch (cmd) {
  case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
  case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
  case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
  case RETRO_ENVIRONMENT_GET_VARIABLE:
    // Same settings and files as the GBA the player sees.
    return ds.environ_cb(cmd, data);
  case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
    *(bool*)data = false;
    return true;
  case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
  case RETRO_ENVIRONMENT_GET_CAN_DUPE:
    if (data)
      *(bool*)data = true;
    return true;
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
  case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
  case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
  case RETRO_ENVIRONMENT_SET_VARIABLES:
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
  case RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE:
  case RETRO_ENVIRONMENT_SET_MESSAGE:
  case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
  case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
  case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
  case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
    return true;    // The frontend only ever hears about A
  default:
    return false;
  };
}

static void b_video(const void *data, unsigned w, unsigned h, size_t pitch) {}
static void b_audio(int16_t l, int16_t r) {}
static size_t b_audio_batch(const int16_t *data, size_t frames) { return frames; }
static void b_input_poll(void) {}
static int16_t b_input_state(unsigned port, unsigned dev, unsigned idx, unsigned id) { return 0; }

static void *b_thread_main(void *arg) {
  while (1) {
    sem_wait(&ds.b_start);
    if (ds.b_quit)
      break;
    gbab_retro_run();
    sem_post(&ds.b_done);
  }
  return NULL;
}

static bool b_load(void) {
  struct retro_game_info info;
  if (ds.b_loaded)
    return true;

  gbab_retro_set_environment(b_environ);
  gbab_retro_set_video_refresh(b_video);
  gbab_retro_set_audio_sample(b_audio);
  gbab_retro_set_audio_sample_batch(b_audio_batch);
  gbab_retro_set_input_poll(b_input_poll);
  gbab_retro_set_input_state(b_input_state);
  gbab_retro_init();

  memset(&info, 0, sizeof(info));
  info.path = ds.rom_path;
  if (!gbab_retro_load_game(&info)) {
    gbab_retro_deinit();
    return false;
  }

  if (!ds.b_thread_ok) {
    sem_init(&ds.b_start, 0, 0);
    sem_init(&ds.b_done, 0, 0);
    ds.b_quit = false;
    ds.b_thread_ok = !pthread_create(&ds.b_thread, NULL, b_thread_main, NULL);
    if (!ds.b_thread_ok) {
      gbab_retro_unload_game();
      gbab_retro_deinit();
      return false;
    }
  }
  ds.b_loaded = true;
  return true;
}

static void b_unload(void) {
  if (ds.b_thread_ok) {
    ds.b_quit = true;
    sem_post(&ds.b_start);
    pthread_join(ds.b_thread, NULL);
    sem_destroy(&ds.b_start);
    sem_destroy(&ds.b_done);
    ds.b_thread_ok = false;
  }
  if (ds.b_loaded) {
    gbab_retro_unload_game();
    gbab_retro_deinit();
    ds.b_loaded = false;
  }
}

/* ---- agreement checks -------------------------------------------------- */

static u32 fnv(u32 h, const void *data, size_t len) {
  const u8 *p = (const u8*)data;
  while (len--)
    h = (h ^ *p++) * 16777619u;
  return h;
}

// Everything the game can see. (Not the renderer's own bookkeeping: the copy
// of a GBA on the other console never draws.)
static u32 hash_a(void) {
  u32 h = 2166136261u;
  h = fnv(h, ewram, 0x40000);
  h = fnv(h, &iwram[0x8000], 0x8000);
  h = fnv(h, vram, 1024 * 96);
  h = fnv(h, oam_ram, 1024);
  h = fnv(h, palette_ram, 1024);
  h = fnv(h, io_registers, 1024);
  h = fnv(h, reg, 17 * 4);
  return h;
}

static u32 hash_b(void) {
  u32 h = 2166136261u;
  h = fnv(h, gbab_ewram, 0x40000);
  h = fnv(h, &gbab_iwram[0x8000], 0x8000);
  h = fnv(h, gbab_vram, 1024 * 96);
  h = fnv(h, gbab_oam_ram, 1024);
  h = fnv(h, gbab_palette_ram, 1024);
  h = fnv(h, gbab_io_registers, 1024);
  h = fnv(h, gbab_reg, 17 * 4);
  return h;
}

/* ---- session ---------------------------------------------------------------- */

static void dual_free_blobs(void) {
  free(ds.own_blob);
  free(ds.peer_blob);
  ds.own_blob = ds.peer_blob = NULL;
}

// Back to a single GBA: the player's one goes on, unplugged.
static void dual_stop(const char *why) {
  const bool was_running = (ds.state == DS_RUN);
  if (ds.state == DS_OFF)
    return;
  if (why)
    dual_msg(why);

  ds.state = DS_OFF;
  dual_free_blobs();
  if (ds.loaded) {
    dualport_end();
    gbab_dualport_end();
    rtc_dual_end();
    gbab_rtc_dual_end();
    serial_mode = SERIAL_MODE_SERIAL_CABLE;
    serialcable_session_reset();
    ds.loaded = false;
  }
  (void)was_running;
}

static u32 dual_rom_crc(void) {
  if (!ds.rom_crc_valid) {
    RFILE *f = filestream_open(ds.rom_path, RETRO_VFS_FILE_ACCESS_READ,
                               RETRO_VFS_FILE_ACCESS_HINT_NONE);
    u32 crc = 0;
    if (f) {
      static u8 buf[64 * 1024];
      int64_t n;
      while ((n = filestream_read(f, buf, sizeof(buf))) > 0)
        crc = dual_crc32(crc, buf, (size_t)n);
      filestream_close(f);
    }
    ds.rom_crc = crc;
    ds.rom_crc_valid = true;
  }
  return ds.rom_crc;
}

static void dual_send_hello(void) {
  u32 w[6];
  w[0] = DUAL_VERSION;
  w[1] = dual_rom_crc();
  w[2] = dual_crc32(0, bios_rom, 0x4000);
  w[3] = ds.cfg;
  w[4] = 1;                     // Restart on plug (always)
  w[5] = ds.is_host ? 1 : 0;
  dual_send(DPK_HELLO, w, 6, NULL, 0);
  ds.hello_sent = true;
}

static void dual_send_ping(void) {
  u32 w[2];
  const u64 now = netpacket_time_usec();
  w[0] = (u32)now;
  w[1] = (u32)(now >> 32);
  dual_send(DPK_PING, w, 2, NULL, 0);
}

static bool dual_hello_matches(void) {
  return ds.peer_hello[0] == DUAL_VERSION &&
         ds.peer_hello[1] == dual_rom_crc() &&
         ds.peer_hello[2] == dual_crc32(0, bios_rom, 0x4000) &&
         ds.peer_hello[3] == ds.cfg &&
         ds.peer_hello[5] == (ds.is_host ? 0 : 1);
}

// Freeze our GBA where it is (between two frames) and make the copy that is
// sent to the other console. The game starts over first.
static bool dual_capture(void) {
  ds.own_blob = (u8*)malloc(DUAL_BLOB_SIZE);
  if (!ds.peer_blob)
    ds.peer_blob = (u8*)malloc(DUAL_BLOB_SIZE);
  if (!ds.own_blob || !ds.peer_blob)
    return false;

  reset_gba();
  rtc_dual_begin(true);
  memset(ds.own_blob, 0, GBA_STATE_MEM_SIZE);
  gba_save_state(ds.own_blob);
  memcpy(&ds.own_blob[GBA_STATE_MEM_SIZE], gamepak_backup, DUAL_SRAM_SIZE);
  ds.own_sent = false;
  return true;
}

static void dual_send_blob(void) {
  u32 off;
  for (off = 0; off < DUAL_BLOB_SIZE; off += DUAL_CHUNK) {
    u32 w[2] = { off, DUAL_BLOB_SIZE };
    u32 len = MIN(DUAL_CHUNK, DUAL_BLOB_SIZE - off);
    dual_send(DPK_STATE, w, 2, &ds.own_blob[off], len);
  }
  ds.own_sent = true;
}

// Both GBAs start from the states that were exchanged: ours from the copy we
// sent (so that both copies of it start the same way, with no recompiled
// code), the other one from what we got.
static bool dual_load_machines(void) {
  const unsigned peer_slot = ds.my_slot ^ 1;
  u8 *sram_b;

  if (!gba_load_state(ds.own_blob))
    return false;
  if (!gbab_retro_unserialize(ds.peer_blob, GBA_STATE_MEM_SIZE))
    return false;
  sram_b = (u8*)gbab_retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
  if (sram_b)
    memcpy(sram_b, &ds.peer_blob[GBA_STATE_MEM_SIZE], DUAL_SRAM_SIZE);
  gbab_rtc_dual_begin(false);

  bus_reset();
  serial_mode = SERIAL_MODE_DUAL_CABLE;
  gbab_serial_mode = SERIAL_MODE_DUAL_CABLE;
  dualport_begin(ds.my_slot);
  gbab_dualport_begin(peer_slot);
  dual_free_blobs();
  ds.loaded = true;
  return true;
}

// A new session (or a new peer): forget everything about the last one. The
// other console's packets can get here before we are ready for them, so this
// is done before anything is sent.
static void dual_session_reset(void) {
  dual_free_blobs();
  ds.peer_rcvd = 0;
  ds.rtt_us = ds.rtt_samples = 0;
  ds.hello_sent = ds.hello_rcvd = ds.peer_ready = false;
  memset(ds.peer_keys, 0, sizeof(ds.peer_keys));
  memset(ds.peer_frame_ok, 0, sizeof(ds.peer_frame_ok));
  memset(ds.my_hash, 0, sizeof(ds.my_hash));
  memset(ds.peer_hash, 0, sizeof(ds.peer_hash));
  ds.t_state = ds.t_heard = netpacket_time_usec();
}

static void dual_start_running(void) {
  u32 i;
  ds.state = DS_RUN;
  ds.frame = 0;
  // Nobody presses anything during the first frames (the delay).
  for (i = 0; i < ds.delay; i++) {
    ds.own_keys[i] = 0;
    ds.peer_keys[i] = 0;
    ds.peer_frame_ok[i] = i + 1;
  }
  ds.sent_upto = ds.delay;
  ds.t_heard = netpacket_time_usec();
  {
    char msg[96];
    snprintf(msg, sizeof(msg), "Cable Link: both GBAs connected (%u frames of delay)",
             (unsigned)ds.delay);
    dual_msg(msg);
  }
}

// Input delay from the round trip: the buttons must get to the other console
// before it needs them. The two consoles are up to a frame apart, and a frame
// more covers the network having a bad moment.
static u32 dual_pick_delay(void) {
  u32 d;
  if (!ds.rtt_samples)
    return 4;
  d = (ds.rtt_us / 2 + DUAL_FRAME_US + DUAL_FRAME_US - 1) / DUAL_FRAME_US + 1;
  return MAX(2, MIN(d, 30));
}

// Setup steps, between frames.
static void dual_setup_step(void) {
  const u64 now = netpacket_time_usec();

  if (ds.state == DS_HELLO) {
    if (!ds.hello_sent)
      dual_send_hello();
    if (ds.hello_rcvd) {
      if (!dual_hello_matches()) {
        // Same answer on both consoles: both go on with the real time cable.
        ds.state = DS_OFF;
        dual_msg("Cable Link: games or settings differ, using the real time cable");
        return;
      }
      if (!b_load()) {
        dual_stop("Cable Link: not enough memory for the second GBA");
        dual_send(DPK_BYE, NULL, 0, NULL, 0);
        return;
      }
      // Measure the round trip while nothing else is on the way.
      ds.state = DS_PING;
      ds.t_state = now;
      dual_send_ping();
    }
    else if (now - ds.t_state > DUAL_HELLO_US) {
      // The other console does not have this core: real time cable.
      ds.state = DS_OFF;
    }
    return;
  }

  if (ds.state == DS_PING) {
    if (ds.rtt_samples >= DUAL_PINGS || now - ds.t_state > DUAL_PING_US) {
      if (!dual_capture()) {
        dual_stop("Cable Link: not enough memory for the second GBA");
        dual_send(DPK_BYE, NULL, 0, NULL, 0);
        return;
      }
      ds.state = DS_TRANSFER;
      dual_send_blob();
    }
    else
      dual_send_ping();
    return;
  }

  if (ds.state == DS_TRANSFER) {
    if (ds.peer_rcvd >= DUAL_BLOB_SIZE) {
      if (!dual_load_machines()) {
        dual_send(DPK_BYE, NULL, 0, NULL, 0);
        dual_stop("Cable Link: could not start the second GBA");
        return;
      }
      if (ds.is_host) {
        u32 w[1];
        ds.delay = dual_pick_delay();
        w[0] = ds.delay;
        dual_send(DPK_READY, w, 1, NULL, 0);
      }
      ds.state = DS_READY;
    }
  }

  if (ds.state == DS_READY) {
    if (ds.is_host)
      dual_start_running();
    else if (ds.peer_ready) {
      u32 w[1] = { ds.delay };
      dual_send(DPK_READY, w, 1, NULL, 0);
      dual_start_running();
    }
  }

  if (ds.state != DS_RUN && ds.state != DS_OFF &&
      now - ds.t_state > DUAL_SETUP_US) {
    dual_send(DPK_BYE, NULL, 0, NULL, 0);
    dual_stop("Cable Link: the other console did not answer");
  }
}

/* ---- frontend interface (libretro.c) ---------------------------------- */

void dual_set_frontend(retro_environment_t cb,
                       void (*message)(const char *text, unsigned ms)) {
  ds.environ_cb = cb;
  ds.message = message;
}

void dual_game_loaded(const char *path, u32 cfg) {
  strncpy(ds.rom_path, path, sizeof(ds.rom_path) - 1);
  ds.rom_path[sizeof(ds.rom_path) - 1] = 0;
  ds.rom_crc_valid = false;
  ds.cfg = cfg;
}

// A netplay session started (Cable Link mode). The host waits for a client.
void dual_session_start(unsigned client_id) {
  dual_stop(NULL);
  ds.is_host = (client_id == 0);
  ds.my_slot = ds.is_host ? 0 : 1;
  ds.peer_id = ds.is_host ? -1 : 0;
  dual_session_reset();
  ds.state = ds.is_host ? DS_OFF : DS_HELLO;
}

// Host: a client joined. Only one other console can be on a dual cable.
bool dual_peer_connected(unsigned client_id) {
  if (!ds.is_host)
    return true;
  if (ds.state != DS_OFF && ds.peer_id >= 0)
    return false;
  ds.peer_id = client_id;
  dual_session_reset();
  ds.state = DS_HELLO;
  return true;
}

void dual_peer_disconnected(unsigned client_id) {
  if ((int)client_id == ds.peer_id) {
    dual_stop(ds.state == DS_RUN ? "Cable Link: the other console left" : NULL);
    ds.peer_id = ds.is_host ? -1 : ds.peer_id;
  }
}

void dual_session_stop(void) {
  dual_stop(NULL);
  ds.peer_id = -1;
}

// Whether the dual cable is setting up (true: do not run the game now).
bool dual_setting_up(void) {
  if (ds.state != DS_OFF && ds.state != DS_RUN)
    dual_setup_step();
  return ds.state != DS_OFF && ds.state != DS_RUN;
}

bool dual_running(void) {
  return ds.state == DS_RUN;
}

// The buttons for this frame. Returns false if the other console's are not
// here yet: the frame is not run now.
bool dual_frame_begin(u32 local_keys) {
  const u32 f = ds.frame;
  const u32 fi = f % DUAL_RING;
  const u64 t0 = netpacket_time_usec();

  // Ours for frame f + delay.
  while (ds.sent_upto <= f + ds.delay) {
    u32 w[2];
    w[0] = ds.sent_upto;
    w[1] = local_keys;
    ds.own_keys[ds.sent_upto % DUAL_RING] = local_keys;
    dual_send(DPK_INPUT, w, 2, NULL, 0);
    ds.sent_upto++;
  }

  // Theirs for frame f.
  while (ds.peer_frame_ok[fi] != f + 1) {
    const u64 now = netpacket_time_usec();
    if (ds.state != DS_RUN)
      return false;
    if (now - ds.t_heard > DUAL_PEER_US) {
      dual_send(DPK_BYE, NULL, 0, NULL, 0);
      dual_stop("Cable Link: the other console stopped answering");
      return false;
    }
    if (now - t0 > DUAL_STALL_US)
      return false;
    netpacket_poll_receive();
    if (ds.peer_frame_ok[fi] != f + 1)
      usleep(250);
  }

  dualport_set_keys(ds.own_keys[fi]);
  gbab_dualport_set_keys(ds.peer_keys[fi]);
#ifdef DUAL_DEBUG
  // Test of the agreement check: the hidden GBA gets other buttons once.
  if (getenv("DUAL_FAULT") && f == 300)
    gbab_dualport_set_keys(ds.peer_keys[fi] ^ 0x1);
#endif
  return true;
}

void dual_b_frame_start(void) {
  DBG("[dual] frame %u starts\n", ds.frame);
  sem_post(&ds.b_start);
}

void dual_b_frame_wait(void) {
  sem_wait(&ds.b_done);
}

// Both copies of both GBAs must be the same: compare once both sides told.
static void dual_check_hash(u32 f) {
  const u32 i = (f / DUAL_HASH_EVERY) % DUAL_HASH_SLOTS;
  const u32 *mine = ds.my_hash[i], *theirs = ds.peer_hash[i];
  if (mine[0] == f + 1 && theirs[0] == f + 1 &&
      (mine[1] != theirs[1] || mine[2] != theirs[2])) {
    dual_send(DPK_BYE, NULL, 0, NULL, 0);
    dual_stop("Cable Link: the two consoles went out of sync");
  }
}

void dual_frame_end(void) {
  const u32 f = ds.frame++;
  if (f % DUAL_HASH_EVERY == 0) {
    u32 w[3], *mine = ds.my_hash[(f / DUAL_HASH_EVERY) % DUAL_HASH_SLOTS];
    const u32 ha = hash_a(), hb = hash_b();
    w[0] = f;
    w[1] = ds.my_slot ? hb : ha;    // slot 0's GBA
    w[2] = ds.my_slot ? ha : hb;    // slot 1's GBA
    mine[0] = f + 1;
    mine[1] = w[1];
    mine[2] = w[2];
    dual_send(DPK_HASH, w, 3, NULL, 0);
    dual_check_hash(f);
  }
}

// Returns true if the packet was ours.
bool dual_net_receive(const void *buf, size_t len, uint16_t client_id) {
  const u32 *pkt = (const u32*)buf;
  u32 type;
  if (len < 8 || netorder32(pkt[0]) != DUAL_HEADER)
    return false;
  if ((int)client_id != ds.peer_id)
    return true;

  type = netorder32(pkt[1]);
  ds.t_heard = netpacket_time_usec();

  switch (type) {
  case DPK_HELLO:
    if (len >= 8 + 6 * 4 && ds.state == DS_HELLO) {
      u32 i;
      for (i = 0; i < 6; i++)
        ds.peer_hello[i] = netorder32(pkt[2 + i]);
      ds.hello_rcvd = true;
      if (!ds.hello_sent)
        dual_send_hello();
    }
    break;

  case DPK_PING:
    if (len >= 16) {
      u32 w[2] = { netorder32(pkt[2]), netorder32(pkt[3]) };
      dual_send(DPK_PONG, w, 2, NULL, 0);
    }
    break;

  case DPK_PONG:
    if (len >= 16) {
      const u64 t = netorder32(pkt[2]) | ((u64)netorder32(pkt[3]) << 32);
      const u32 rtt = (u32)(netpacket_time_usec() - t);
      // The best one: the others waited behind something else.
      ds.rtt_us = ds.rtt_samples ? MIN(ds.rtt_us, rtt) : rtt;
      ds.rtt_samples++;
    }
    break;

  case DPK_STATE:
    if (len > 16 && ds.state >= DS_HELLO && ds.state <= DS_TRANSFER) {
      if (!ds.peer_blob)
        ds.peer_blob = (u8*)malloc(DUAL_BLOB_SIZE);
      if (!ds.peer_blob)
        break;
    }
    if (len > 16 && ds.peer_blob && ds.state >= DS_HELLO && ds.state <= DS_TRANSFER) {
      const u32 off = netorder32(pkt[2]);
      const u32 dlen = len - 16;
      if (off + dlen <= DUAL_BLOB_SIZE) {
        memcpy(&ds.peer_blob[off], &pkt[4], dlen);
        ds.peer_rcvd += dlen;
      }
    }
    break;

  case DPK_READY:
    if (len >= 12 && !ds.is_host) {
      ds.delay = MAX(1, MIN(netorder32(pkt[2]), 30));
      ds.peer_ready = true;
    }
    break;

  case DPK_INPUT:
    if (len >= 16 && ds.state != DS_OFF) {
      const u32 f = netorder32(pkt[2]);
      ds.peer_keys[f % DUAL_RING] = netorder32(pkt[3]);
      ds.peer_frame_ok[f % DUAL_RING] = f + 1;
    }
    break;

  case DPK_HASH:
    if (len >= 20 && ds.state == DS_RUN) {
      const u32 f = netorder32(pkt[2]);
      u32 *theirs = ds.peer_hash[(f / DUAL_HASH_EVERY) % DUAL_HASH_SLOTS];
      theirs[0] = f + 1;
      theirs[1] = netorder32(pkt[3]);
      theirs[2] = netorder32(pkt[4]);
      dual_check_hash(f);
    }
    break;

  case DPK_BYE:
    if (ds.state != DS_OFF)
      dual_stop(ds.state == DS_RUN ? "Cable Link: the other console left" : NULL);
    break;
  };
  return true;
}

void dual_shutdown(void) {
  dual_stop(NULL);
  b_unload();
}
