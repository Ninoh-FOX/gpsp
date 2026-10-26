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
// Every console runs every GBA of the session: the local one (instance A,
// the one the frontend shows) and copies of the other players' (the hidden
// instances). The GBAs of a console are joined by a cable in memory (the bus
// below), and only the buttons cross the network. How many GBAs there are
// (2 to 4) is a core option; only that many are loaded.
//
//   host                                     clients
//     HELLO (ROM, BIOS, settings) <-------->  HELLO
//     WAITING (players so far)    --------->  (the games go on meanwhile)
//     START (who is in each slot) --------->  every game stops
//     PING/PONG (round trip to each client)
//     GO                          --------->
//     STATE + SRAM of its own GBA <-------->  STATE + SRAM, to everybody
//       (run length coded: after the restart it is mostly empty)
//     READY (input delay)         --------->
//     INPUT (frame, buttons, ...) <-------->  INPUT, every frame, to everybody
//     HASH (every GBA, sometimes) <-------->  HASH: all the copies must agree
//
// The clients only talk to the host; the frontend passes on what a client
// sends to everybody. The host waits for all the players before it starts,
// so that the games restart once (games only look for the cable at boot),
// keeping each player's save: each GBA is copied to every console. If
// anything does not match (other ROM, BIOS, settings, number of players or
// core) the real time cable is used instead, for the whole session.
//
// The buttons for frame N are sent at frame N - delay, and every console
// runs frame N with the same buttons on every GBA: all the consoles compute
// the same machines. A console that does not have someone's buttons yet
// shows the last frame again and tries later.
//
// All the consoles also show frame N at the same time, so that the screens
// (and the speakers) go together: each one tells the others how far ahead of
// each it thinks it is, and one that is ahead waits a frame now and then.

#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <unistd.h>
#include "common.h"
#include "dual.h"
#include "libretro.h"
#include <streams/file_stream.h>

/* ---- hidden instances (the same core, globals renamed) ------------------- */

#define HIDDEN_DECLARE(p)                                                    \
  void p##retro_set_environment(retro_environment_t);                       \
  void p##retro_set_video_refresh(retro_video_refresh_t);                   \
  void p##retro_set_audio_sample(retro_audio_sample_t);                     \
  void p##retro_set_audio_sample_batch(retro_audio_sample_batch_t);         \
  void p##retro_set_input_poll(retro_input_poll_t);                         \
  void p##retro_set_input_state(retro_input_state_t);                       \
  void p##retro_init(void);                                                 \
  void p##retro_deinit(void);                                               \
  bool p##retro_load_game(const struct retro_game_info *);                  \
  void p##retro_unload_game(void);                                          \
  void p##retro_run(void);                                                  \
  bool p##retro_unserialize(const void *, size_t);                          \
  void *p##retro_get_memory_data(unsigned);                                 \
  void p##dualport_begin(unsigned slot);                                    \
  void p##dualport_end(void);                                               \
  void p##dualport_set_keys(uint32_t keys);                                 \
  void p##reset_gba(void);                                                  \
  void p##rtc_dual_begin(bool owner);                                       \
  void p##rtc_dual_end(void);                                               \
  extern int p##serial_mode;                                                \
  extern u8 p##ewram[], p##iwram[], p##vram[];                              \
  extern u16 p##oam_ram[], p##palette_ram[], p##io_registers[];             \
  extern u32 p##reg[];

HIDDEN_DECLARE(gbab_)
HIDDEN_DECLARE(gbac_)
HIDDEN_DECLARE(gbad_)

typedef struct {
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
  bool (*unserialize)(const void *, size_t);
  void *(*get_memory_data)(unsigned);
  void (*port_begin)(unsigned slot);
  void (*port_end)(void);
  void (*set_keys)(uint32_t keys);
  void (*reset)(void);
  void (*rtc_begin)(bool owner);
  void (*rtc_end)(void);
  int *serial_mode;
  u8 *ewram, *iwram, *vram;
  u16 *oam, *palette, *io;
  u32 *reg;
} gba_iface;

#define HIDDEN_IFACE(p)                                                      \
  { p##retro_set_environment, p##retro_set_video_refresh,                   \
    p##retro_set_audio_sample, p##retro_set_audio_sample_batch,             \
    p##retro_set_input_poll, p##retro_set_input_state, p##retro_init,       \
    p##retro_deinit, p##retro_load_game, p##retro_unload_game,              \
    p##retro_run, p##retro_unserialize, p##retro_get_memory_data,           \
    p##dualport_begin, p##dualport_end, p##dualport_set_keys,               \
    p##reset_gba, p##rtc_dual_begin, p##rtc_dual_end, &p##serial_mode,      \
    p##ewram, p##iwram, p##vram, p##oam_ram, p##palette_ram,                \
    p##io_registers, p##reg }

#define DUAL_HIDDEN (DUAL_MAX_PLAYERS - 1)

static const gba_iface hid_if[DUAL_HIDDEN] = {
  HIDDEN_IFACE(gbab_), HIDDEN_IFACE(gbac_), HIDDEN_IFACE(gbad_)
};

#if defined(ARM64_ARCH) && defined(HAVE_DYNAREC)
extern int gbab_dynarec_enable, gbac_dynarec_enable, gbad_dynarec_enable;
static int *const hid_dynarec[DUAL_HIDDEN] = {
  &gbab_dynarec_enable, &gbac_dynarec_enable, &gbad_dynarec_enable
};
#endif

/* ---- protocol ------------------------------------------------------------- */

#define DUAL_HEADER        0x4d434431   // MCD1 (Multiplayer Cable, Dual)
#define DUAL_VERSION       5

// Packets: header, type | sender's slot << 8 (FF: none yet), words, data.
#define DPK_HELLO          1   // version, rom, bios, settings, players, host
#define DPK_PING           2   // time
#define DPK_PONG           3   // time (echoed)
#define DPK_STATE          4   // offset, total (coded) size, data
#define DPK_READY          5   // input delay, one way time of each slot (us)
#define DPK_INPUT          6   // frame, buttons, current frame, lead over
                               // each slot (us), times we waited for each
#define DPK_HASH           7   // frame, hash of each slot's GBA
#define DPK_BYE            8
#define DPK_WAITING        9   // players so far, players wanted
#define DPK_START          10  // players, client id of each slot
#define DPK_GO             11  // the states can go
#define DPK_GIVEUP         12  // use the real time cable (1: players differ)

#define DUAL_NO_SLOT       0xFF
#define DUAL_CHUNK         (16 * 1024)
#define DUAL_SRAM_SIZE     (1024 * 128)
#define DUAL_BLOB_SIZE     (GBA_STATE_MEM_SIZE + DUAL_SRAM_SIZE)
#define DUAL_PACK_MAX      (DUAL_BLOB_SIZE + DUAL_BLOB_SIZE / 128 + 16)
#define DUAL_RING          256         // Frames of buttons kept
#define DUAL_HASH_EVERY    120         // Frames between agreement checks
#define DUAL_HASH_SLOTS    4
#define DUAL_HELLO_US      6000000     // No answer: the other core is not dual
#define DUAL_HELLO_EVERY   250000      // HELLO sent again until the host's comes
#define DUAL_SETUP_US      30000000    // Whole setup
#define DUAL_PEER_US       15000000    // Peer silent while playing: it is gone
#define DUAL_STALL_US      12000       // Longest wait for buttons in a frame
#define DUAL_FRAME_US      16743
#define DUAL_KEY_RESET     0x80000000  // With the buttons: every game restarts
#define DUAL_PINGS         8           // Round trips measured before the states
#define DUAL_PING_US       1000000     // ...for at most this long
#define DUAL_PONG_US       200000      // A ping with no answer: send another
#define DUAL_SLICE_US      10000       // Setup work per frame (it waits anyway)
#define DUAL_SYNC_WIN      16          // Packets a peer's position comes from
#define DUAL_SYNC_US       (DUAL_FRAME_US * 5 / 8)   // Ahead by more: wait
#define DUAL_SYNC_GAP      60          // Frames between two waits
#define DUAL_DELAY_MORE    4           // Most frames added to the input delay
#define DUAL_DELAY_CALM_US 60000000    // Peers happy this long: one frame less
#define DUAL_DELAY_CALM_MAX 600000000  // (twice as long each time that was wrong)

#define DUAL_MSG_DIFFER  "Cable Link: games or settings differ, using the real time cable"
#define DUAL_MSG_PLAYERS "Cable Link: the consoles have a different number of players, using the real time cable"

enum { DS_OFF = 0, DS_HELLO, DS_WAIT, DS_PING, DS_TRANSFER, DS_READY, DS_RUN };

#define NP DUAL_MAX_PLAYERS

static struct {
  // Frontend side
  retro_environment_t environ_cb;
  void (*message)(const char *text, unsigned ms);
  char rom_path[1024];
  u32 rom_crc;
  bool rom_crc_valid;
  u32 cfg;                      // Settings that change the machine
  unsigned players;             // GBAs on the cable in this session
  unsigned cfg_players;         // Core option (for the next session)

  // Hidden instances
  struct {
    bool loaded;
    bool th_ok;
    pthread_t th;
    sem_t start, done;
    volatile bool quit;
  } hid[DUAL_HIDDEN];

  // Session
  int state;
  bool is_host;
  bool gave_up;                 // Real time cable for the rest of the session
  bool boot_reset;              // Client: restart for the real time cable
  unsigned my_id;               // Our libretro client id (host: 0)
  unsigned n;                   // GBAs on the cable (once START is out)
  unsigned my_slot;
  int slot_id[NP];              // libretro client id of each slot's console
  u64 t_state;                  // When the current step started
  u64 t_heard[NP];              // Last packet from each slot

  // Host, before START: the clients that joined, in that order
  struct {
    int id;
    bool hello;                 // Its HELLO came (and matched)
    u64 t_join;
  } cand[NP - 1];
  unsigned ncand;
  // Host: clients whose HELLO came (and matched) before the frontend told us
  // they joined (RetroArch starts a client's session first). Bit = id.
  u64 early_hello;

  // Client, before START
  bool hello_sent, hello_rcvd;
  u64 t_hello;
  bool go;                      // GO came

  // Host: round trips to each client
  u32 rtt[NP][DUAL_PINGS], rtt_n[NP];
  bool ping_out[NP];
  u64 t_ping[NP];

  u32 ow_us[NP];                // One way time between each slot and the host
  bool peer_ready;              // Client: READY came
  u32 delay;                    // Input delay, frames (agreed at the start)
  u32 send_delay;               // Ours now: more after a peer waited
  u32 stalls[NP];               // Frames we waited for each slot's buttons
  u32 stall_frame[NP];          // (the last one counted, + 1)
  u32 peer_stalls[NP];          // ...and each slot for ours
  u64 t_peer_stall;             // When a peer last waited for us
  u64 t_lowered;                // When we last took a frame off
  u64 calm_us;                  // How long it must be calm for that
  bool reset_wanted;            // The player asked for a reset

  u8 *own_blob, *own_pack;      // Our GBA, and the same run length coded
  u32 own_pack_len;
  u8 *peer_pack[NP];            // Each slot's, coded, as it comes
  u32 peer_pack_len[NP], peer_rcvd[NP];
  bool loaded;

  u32 frame;                    // Next frame to run
  u32 sent_upto;                // Buttons sent for frames < this
  u32 own_keys[DUAL_RING];
  u32 keys[NP][DUAL_RING];      // Each slot's buttons...
  u32 frame_ok[NP][DUAL_RING];  // ...valid when this is frame + 1
  // Agreement checks: [frame + 1, each slot's GBA], ours and each slot's
  u32 my_hash[DUAL_HASH_SLOTS][1 + NP];
  u32 peer_hash[NP][DUAL_HASH_SLOTS][1 + NP];
  u32 hash_done[NP];            // Last check made with each slot (frame + 1)
  u32 hash_bad[NP];             // Last one that differed (frame + 1)

  // Time sync. Where each peer is: from each INPUT, its frame (in us) at our
  // time 0; the best of the last ones is the least late.
  s64 peer_pos[NP][DUAL_SYNC_WIN];
  u32 peer_pos_n[NP];
  s32 peer_lead[NP];            // How far ahead of us each peer says it is
  u32 sync_next;                // No waiting before this frame
} ds = { .players = 2, .cfg_players = 2 };

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

// To one console (libretro client id) or RETRO_NETPACKET_BROADCAST.
static void dual_send(unsigned to, u32 type, const u32 *words, u32 nwords,
                      const void *data, u32 len) {
  u8 buf[8 + 16 * 4 + DUAL_CHUNK];
  u32 *hdr = (u32*)buf;
  u32 i;
  const u32 slot = ds.n ? ds.my_slot : DUAL_NO_SLOT;
  hdr[0] = netorder32(DUAL_HEADER);
  hdr[1] = netorder32(type | (slot << 8));
  for (i = 0; i < nwords; i++)
    hdr[2 + i] = netorder32(words[i]);
  if (len)
    memcpy(&buf[8 + nwords * 4], data, len);
  netpacket_send(to, buf, 8 + nwords * 4 + len);
}

static void dual_send_all(u32 type, const u32 *words, u32 nwords,
                          const void *data, u32 len) {
  dual_send(RETRO_NETPACKET_BROADCAST, type, words, nwords, data, len);
}

static void dual_msg(const char *text) {
  if (ds.message)
    ds.message(text, 3000);
}

// The hidden GBA for each other slot, in slot order.
static unsigned hidden_slot(unsigned k) {
  return k < ds.my_slot ? k : k + 1;
}

/* ---- the bus: the cable between the GBAs of this console ----------------- */

#define DUAL_QSIZE   512
#define DUAL_REPLIES 64     // Transfers a child's answer is kept for

// A u64 that atomics can use whole: 32 bit x86 aligns u64 to 4 bytes only,
// and a 64 bit atomic across that boundary is a slow, locked bus split.
typedef u64 bus_clock __attribute__((aligned(8)));

static struct {
  bus_clock pub[NP];                    // Published clocks (atomic)
  unsigned n;
  dual_event q[NP][NP][DUAL_QSIZE];     // Events FOR each slot, FROM each slot
  u32 qhead[NP][NP], qcount[NP][NP];
  struct { u32 seq, word, valid; } rep[NP][DUAL_REPLIES];  // Children's answers
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  int waiters;
  u64 notified[NP];                     // pub[] when we last woke the others up
  volatile int abort;
} bus = { .mtx = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

static void bus_reset(unsigned n) {
  pthread_mutex_lock(&bus.mtx);
  memset(bus.pub, 0, sizeof(bus.pub));
  memset(bus.qhead, 0, sizeof(bus.qhead));
  memset(bus.qcount, 0, sizeof(bus.qcount));
  memset(bus.rep, 0, sizeof(bus.rep));
  memset(bus.notified, 0, sizeof(bus.notified));
  bus.n = n;
  bus.abort = 0;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  pthread_mutex_unlock(&bus.mtx);
}

unsigned dualbus_players(void) {
  return bus.n;
}

// Waking a sleeping thread up costs some tens of microseconds, and a child
// is never more than a few hundred cycles behind the parent: waking them up
// at every scheduler call would add up to milliseconds per frame. So a GBA
// wakes the others up only every DUAL_NOTIFY_STEP cycles, or whenever it
// stops itself (it waits, or its frame is over). This decides when the other
// threads wake up, never what they compute.
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

// Whether a GBA at `clock` may go on: no child passes the parent, the parent
// does not get more than the lookahead ahead of the slowest child.
static bool bus_may_run(unsigned slot, u64 clock) {
  unsigned s;
  if (slot)
    return __atomic_load_n(&bus.pub[0], __ATOMIC_SEQ_CST) >= clock;
  for (s = 1; s < bus.n; s++)
    if (__atomic_load_n(&bus.pub[s], __ATOMIC_SEQ_CST) + DUAL_LOOKAHEAD < clock)
      return false;
  return true;
}

static bool bus_has_reply(unsigned from, u32 seq) {
  return __atomic_load_n(&bus.rep[from][seq % DUAL_REPLIES].valid, __ATOMIC_SEQ_CST) &&
         __atomic_load_n(&bus.rep[from][seq % DUAL_REPLIES].seq, __ATOMIC_SEQ_CST) == seq;
}

// Spin for a little while (the other GBAs run on the other CPU cores and are
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
        DBG("[bus] %s:%d sleeps, pub %llu %llu\n", __func__, __LINE__,      \
            (unsigned long long)bus.pub[0], (unsigned long long)bus.pub[1]); \
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

void dualbus_post(unsigned from_slot, unsigned to, const dual_event *ev) {
  DBG("[bus] slot %u posts kind %u t %llu\n", from_slot, ev->kind, (unsigned long long)ev->t);
  pthread_mutex_lock(&bus.mtx);
  if (bus.qcount[to][from_slot] < DUAL_QSIZE) {
    dual_event *e = &bus.q[to][from_slot][(bus.qhead[to][from_slot] +
                                           bus.qcount[to][from_slot]) % DUAL_QSIZE];
    *e = *ev;
    e->from = from_slot;
    bus.qcount[to][from_slot]++;
  }
  pthread_mutex_unlock(&bus.mtx);
}

// Takes the event for `slot` due first, if it is due at `now`. What a child
// does reaches the others DUAL_LOOKAHEAD cycles later. (Ties go to the
// lowest slot: the order never depends on the threads.)
int dualbus_pop(unsigned slot, uint64_t now, dual_event *ev) {
  int best = -1;
  u64 best_due = 0;
  unsigned s;
  pthread_mutex_lock(&bus.mtx);
  for (s = 0; s < bus.n; s++) {
    if (s != slot && bus.qcount[slot][s]) {
      const dual_event *h = &bus.q[slot][s][bus.qhead[slot][s]];
      const u64 due = h->t + (s ? DUAL_LOOKAHEAD : 0);
      if (due <= now && (best < 0 || due < best_due)) {
        best = (int)s;
        best_due = due;
      }
    }
  }
  if (best >= 0) {
    *ev = bus.q[slot][best][bus.qhead[slot][best]];
    bus.qhead[slot][best] = (bus.qhead[slot][best] + 1) % DUAL_QSIZE;
    bus.qcount[slot][best]--;
  }
  pthread_mutex_unlock(&bus.mtx);
  return best >= 0;
}

void dualbus_reply(unsigned from_slot, uint32_t seq, uint32_t word, int active) {
  pthread_mutex_lock(&bus.mtx);
  __atomic_store_n(&bus.rep[from_slot][seq % DUAL_REPLIES].word,
                   active ? word : 0xFFFF, __ATOMIC_SEQ_CST);
  __atomic_store_n(&bus.rep[from_slot][seq % DUAL_REPLIES].seq, seq, __ATOMIC_SEQ_CST);
  __atomic_store_n(&bus.rep[from_slot][seq % DUAL_REPLIES].valid, 1, __ATOMIC_SEQ_CST);
  pthread_cond_broadcast(&bus.cond);
  pthread_mutex_unlock(&bus.mtx);
}

int dualbus_wait_reply(unsigned slot, unsigned from_slot, uint32_t seq,
                       uint32_t *word) {
  DBG("[bus] slot %u waits reply %u from %u\n", slot, seq, from_slot);
  bus_notify(slot, true);
  BUS_WAIT(bus_has_reply(from_slot, seq));
  if (!bus_has_reply(from_slot, seq))
    return 0;
  *word = __atomic_load_n(&bus.rep[from_slot][seq % DUAL_REPLIES].word, __ATOMIC_SEQ_CST);
  return 1;
}

/* ---- hidden instances ----------------------------------------------------- */

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
  const unsigned k = (unsigned)(uintptr_t)arg;
  while (1) {
    sem_wait(&ds.hid[k].start);
    if (ds.hid[k].quit)
      break;
    hid_if[k].run();
    sem_post(&ds.hid[k].done);
  }
  return NULL;
}

static bool b_load(unsigned k) {
  const gba_iface *g = &hid_if[k];
  struct retro_game_info info;
  if (ds.hid[k].loaded)
    return true;

  g->set_environment(b_environ);
  g->set_video_refresh(b_video);
  g->set_audio_sample(b_audio);
  g->set_audio_sample_batch(b_audio_batch);
  g->set_input_poll(b_input_poll);
  g->set_input_state(b_input_state);
  g->init();

  memset(&info, 0, sizeof(info));
  info.path = ds.rom_path;
  if (!g->load_game(&info)) {
    g->deinit();
    return false;
  }
#if defined(ARM64_ARCH) && defined(HAVE_DYNAREC)
  // Without its JIT cache (see memmap.c) a hidden GBA would run on the
  // interpreter, whose timing is not that of its copy on the other console:
  // the real time cable is better than GBAs that drift apart.
  if (dynarec_enable && !*hid_dynarec[k]) {
    g->unload_game();
    g->deinit();
    return false;
  }
#endif

  if (!ds.hid[k].th_ok) {
    sem_init(&ds.hid[k].start, 0, 0);
    sem_init(&ds.hid[k].done, 0, 0);
    ds.hid[k].quit = false;
    ds.hid[k].th_ok = !pthread_create(&ds.hid[k].th, NULL, b_thread_main,
                                      (void*)(uintptr_t)k);
    if (!ds.hid[k].th_ok) {
      g->unload_game();
      g->deinit();
      return false;
    }
  }
  ds.hid[k].loaded = true;
  return true;
}

// The hidden GBAs for `players` (only as many as the option asks for).
static bool b_load_for(unsigned players) {
  unsigned k;
  for (k = 0; k + 1 < players; k++)
    if (!b_load(k))
      return false;
  return true;
}

static void b_unload(unsigned k) {
  if (ds.hid[k].th_ok) {
    ds.hid[k].quit = true;
    sem_post(&ds.hid[k].start);
    pthread_join(ds.hid[k].th, NULL);
    sem_destroy(&ds.hid[k].start);
    sem_destroy(&ds.hid[k].done);
    ds.hid[k].th_ok = false;
  }
  if (ds.hid[k].loaded) {
    hid_if[k].unload_game();
    hid_if[k].deinit();
    ds.hid[k].loaded = false;
  }
}

// The hidden GBAs read the ROM from ours (gba_memory.c), when we have it all
// in memory: blocks of 1 MB.
const u8 *dual_rom_block(u32 idx) {
  extern u8 *gamepak_buffers[32];
  extern u32 gamepak_file_blocks;
  if (gamepak_must_swap() || gamepak_mini_materialized ||
      idx >= gamepak_buffer_count || idx >= (gamepak_file_blocks + 31) / 32)
    return NULL;
  return gamepak_buffers[idx];
}

/* ---- agreement checks -------------------------------------------------- */

static u32 fnv(u32 h, const void *data, size_t len) {
  const u8 *p = (const u8*)data;
  while (len--)
    h = (h ^ *p++) * 16777619u;
  return h;
}

// Everything the game can see. (Not the renderer's own bookkeeping: the
// copies of a GBA on the other consoles never draw.)
static u32 hash_gba(const u8 *ewram_, const u8 *iwram_, const u8 *vram_,
                    const u16 *oam_, const u16 *pal_, const u16 *io_,
                    const u32 *reg_) {
  u32 h = 2166136261u;
  h = fnv(h, ewram_, 0x40000);
  h = fnv(h, &iwram_[0x8000], 0x8000);
  h = fnv(h, vram_, 1024 * 96);
  h = fnv(h, oam_, 1024);
  h = fnv(h, pal_, 1024);
  h = fnv(h, io_, 1024);
  h = fnv(h, reg_, 17 * 4);
  return h;
}

static u32 hash_slot(unsigned s) {
  const gba_iface *g;
  if (s == ds.my_slot)
    return hash_gba(ewram, iwram, vram, oam_ram, palette_ram, io_registers, reg);
  g = &hid_if[s < ds.my_slot ? s : s - 1];
  return hash_gba(g->ewram, g->iwram, g->vram, g->oam, g->palette, g->io, g->reg);
}

/* ---- session ---------------------------------------------------------------- */

static void dual_free_blobs(void) {
  unsigned s;
  free(ds.own_blob);
  free(ds.own_pack);
  ds.own_blob = ds.own_pack = NULL;
  for (s = 0; s < NP; s++) {
    free(ds.peer_pack[s]);
    ds.peer_pack[s] = NULL;
  }
}

// Run length coding for the states sent at the start: after the restart
// they are mostly zeros (and save memory mostly FF). A control byte c < 80
// is followed by c + 1 bytes as they are; otherwise the next byte is there
// (c & 7F) + 3 times.
static u32 rle_pack(u8 *dst, const u8 *src, u32 len) {
  u32 i = 0, lit = 0, o = 0;
  while (i <= len) {
    u32 run = 0;
    if (i < len)
      for (run = 1; i + run < len && run < 130 && src[i + run] == src[i]; run++);
    if (i == len || run >= 3) {
      while (lit < i) {
        const u32 n = MIN(128, i - lit);
        dst[o++] = n - 1;
        memcpy(&dst[o], &src[lit], n);
        o += n;
        lit += n;
      }
      if (i == len)
        break;
      dst[o++] = 0x80 | (run - 3);
      dst[o++] = src[i];
      lit = i + run;
    }
    i += run;
  }
  return o;
}

static bool rle_unpack(u8 *dst, u32 len, const u8 *src, u32 slen) {
  u32 i = 0, o = 0;
  while (i < slen) {
    const u32 c = src[i++];
    if (c < 0x80) {
      if (i + c + 1 > slen || o + c + 1 > len)
        return false;
      memcpy(&dst[o], &src[i], c + 1);
      i += c + 1;
      o += c + 1;
    }
    else {
      if (i >= slen || o + (c & 0x7F) + 3 > len)
        return false;
      memset(&dst[o], src[i++], (c & 0x7F) + 3);
      o += (c & 0x7F) + 3;
    }
  }
  return o == len;
}

// Back to a single GBA: the player's one goes on, unplugged.
static void dual_stop(const char *why) {
  unsigned k;
  if (ds.state == DS_OFF)
    return;
  if (why)
    dual_msg(why);

  ds.state = DS_OFF;
  dual_free_blobs();
  cable_end_bios_boot();
  // The clocks go back to normal even if the setup did not get to the end:
  // ours took the session time when the game restarted (dual_capture), the
  // hidden GBAs when their state came in.
  rtc_dual_end();
  for (k = 0; k < DUAL_HIDDEN; k++)
    if (ds.hid[k].loaded)
      hid_if[k].rtc_end();
  if (ds.loaded) {
    dualport_end();
    for (k = 0; k + 1 < ds.n; k++)
      hid_if[k].port_end();
    serial_mode = SERIAL_MODE_SERIAL_CABLE;
    serialcable_session_reset();
    ds.loaded = false;
  }
}

// The real time cable for the rest of this session (for everybody).
static void dual_give_up(const char *why) {
  if (ds.is_host) {
    u32 w[1] = { why && !strcmp(why, DUAL_MSG_PLAYERS) };
    dual_send_all(DPK_GIVEUP, w, 1, NULL, 0);
  }
  else if (ds.state == DS_HELLO || ds.state == DS_WAIT)
    dual_send(0, DPK_BYE, NULL, 0, NULL, 0);
  dual_stop(NULL);
  ds.state = DS_OFF;
  ds.gave_up = true;
  // A client restarts with the cable plugged, as games only look for it at
  // boot (with the dual cable every game restarts anyway, once all the
  // players are there).
  ds.boot_reset = !ds.is_host;
  if (why)
    dual_msg(why);
}

// One way time between two slots: through the host.
static u32 dual_ow(unsigned a, unsigned b) {
  return ds.ow_us[a] + ds.ow_us[b];
}

// One frame waited for a peer: everybody hears about it with our buttons.
static void dual_count_stall(unsigned s, u32 f) {
  if (ds.stall_frame[s] != f + 1) {
    ds.stall_frame[s] = f + 1;
    ds.stalls[s]++;
  }
}

static u32 dual_rom_crc(void) ;

static void dual_send_hello(unsigned to) {
  u32 w[6];
  w[0] = DUAL_VERSION;
  w[1] = dual_rom_crc();
  w[2] = dual_crc32(0, bios_rom, 0x4000);
  w[3] = ds.cfg;
  w[4] = ds.players;
  w[5] = ds.is_host ? 1 : 0;
  dual_send(to, DPK_HELLO, w, 6, NULL, 0);
  ds.hello_sent = true;
  ds.t_hello = netpacket_time_usec();
}

static bool dual_hello_matches(const u32 *w) {
  return w[0] == DUAL_VERSION &&
         w[1] == dual_rom_crc() &&
         w[2] == dual_crc32(0, bios_rom, 0x4000) &&
         w[3] == ds.cfg &&
         w[4] == ds.players &&
         w[5] == (ds.is_host ? 0 : 1);
}

// One 32 KB block of the ROM. The RTC/rumble registers (C4-C9) are written
// into the first block while the game runs: they are left out.
static u32 rom_hash_block(u32 h, const u8 *data, u32 idx) {
  static u32 blk[32 * 1024 / 4];
  u32 i;
  memcpy(blk, data, sizeof(blk));
  if (!idx)
    memset((u8*)blk + 0xC4, 0, 6);
  for (i = 0; i < 32 * 1024 / 4; i++) {
    h = (h ^ blk[i]) * 0x01000193u;
    h = (h << 5) | (h >> 27);
  }
  return h;
}

// Which game this is. It is in memory already unless the ROM is too big for
// it (then it is read from the file, same result): reading the file again is
// slow on a SD card, and the players wait for it.
static u32 dual_rom_crc(void) {
  extern u8 *gamepak_buffers[32];
  extern u32 gamepak_file_blocks;
  if (!ds.rom_crc_valid) {
    u32 h = 0x811C9DC5u ^ gamepak_file_blocks, b;
    if (!gamepak_must_swap() && !gamepak_mini_materialized &&
        gamepak_file_blocks <= gamepak_buffer_count * 32) {
      for (b = 0; b < gamepak_file_blocks; b++)
        h = rom_hash_block(h, &gamepak_buffers[b >> 5][(b & 31) << 15], b);
    }
    else {
      RFILE *f = filestream_open(ds.rom_path, RETRO_VFS_FILE_ACCESS_READ,
                                 RETRO_VFS_FILE_ACCESS_HINT_NONE);
      static u8 buf[32 * 1024];
      if (f) {
        for (b = 0; b < gamepak_file_blocks &&
                    filestream_read(f, buf, sizeof(buf)) == sizeof(buf); b++)
          h = rom_hash_block(h, buf, b);
        filestream_close(f);
      }
    }
    ds.rom_crc = h;
    ds.rom_crc_valid = true;
  }
  return ds.rom_crc;
}

static void dual_send_ping(unsigned s) {
  u32 w[2];
  const u64 now = netpacket_time_usec();
  w[0] = (u32)now;
  w[1] = (u32)(now >> 32);
  dual_send(ds.slot_id[s], DPK_PING, w, 2, NULL, 0);
  ds.ping_out[s] = true;
  ds.t_ping[s] = now;
}

static u32 dual_rtt_min(unsigned s) {
  u32 i, m = ~0U;
  for (i = 0; i < MIN(ds.rtt_n[s], DUAL_PINGS); i++)
    m = MIN(m, ds.rtt[s][i]);
  return ds.rtt_n[s] ? m : 0;
}

// Freeze our GBA where it is (between two frames) and make the copy that is
// sent to the others. The game starts over first.
static bool dual_capture(void) {
  ds.own_blob = (u8*)malloc(DUAL_BLOB_SIZE);
  ds.own_pack = (u8*)malloc(DUAL_PACK_MAX);
  if (!ds.own_blob || !ds.own_pack)
    return false;

  cable_force_bios_boot();
  reset_gba();
  rtc_dual_begin(true);
  memset(ds.own_blob, 0, GBA_STATE_MEM_SIZE);
  gba_save_state(ds.own_blob);
  memcpy(&ds.own_blob[GBA_STATE_MEM_SIZE], gamepak_backup, DUAL_SRAM_SIZE);
  ds.own_pack_len = rle_pack(ds.own_pack, ds.own_blob, DUAL_BLOB_SIZE);
  return true;
}

static void dual_send_blob(void) {
  u32 off;
  for (off = 0; off < ds.own_pack_len; off += DUAL_CHUNK) {
    u32 w[2] = { off, ds.own_pack_len };
    u32 len = MIN(DUAL_CHUNK, ds.own_pack_len - off);
    dual_send_all(DPK_STATE, w, 2, &ds.own_pack[off], len);
  }
  free(ds.own_pack);
  ds.own_pack = NULL;
}

static bool dual_have_states(void) {
  unsigned s;
  for (s = 0; s < ds.n; s++)
    if (s != ds.my_slot &&
        (!ds.peer_pack_len[s] || ds.peer_rcvd[s] < ds.peer_pack_len[s]))
      return false;
  return true;
}

// Every GBA starts from the states that were exchanged: ours from the copy
// we sent (so that every copy of it starts the same way, with no recompiled
// code), the others from what we got.
static bool dual_load_machines(void) {
  u8 *blob;
  unsigned k;

  if (!gba_load_state(ds.own_blob))
    return false;
  blob = (u8*)malloc(DUAL_BLOB_SIZE);
  if (!blob)
    return false;
  for (k = 0; k + 1 < ds.n; k++) {
    const unsigned s = hidden_slot(k);
    u8 *sram;
    if (!rle_unpack(blob, DUAL_BLOB_SIZE, ds.peer_pack[s], ds.peer_pack_len[s]) ||
        !hid_if[k].unserialize(blob, GBA_STATE_MEM_SIZE)) {
      free(blob);
      return false;
    }
    sram = (u8*)hid_if[k].get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (sram)
      memcpy(sram, &blob[GBA_STATE_MEM_SIZE], DUAL_SRAM_SIZE);
    hid_if[k].rtc_begin(false);
  }
  free(blob);

  bus_reset(ds.n);
  serial_mode = SERIAL_MODE_DUAL_CABLE;
  dualport_begin(ds.my_slot);
  for (k = 0; k + 1 < ds.n; k++) {
    *hid_if[k].serial_mode = SERIAL_MODE_DUAL_CABLE;
    hid_if[k].port_begin(hidden_slot(k));
  }
  dual_free_blobs();
  ds.loaded = true;
  return true;
}

// A new session: forget everything about the last one. Packets from the
// others can get here before we are ready for them, so this is done before
// anything is sent.
static void dual_session_reset(void) {
  const u64 now = netpacket_time_usec();
  unsigned s;
  dual_free_blobs();
  ds.n = 0;
  ds.ncand = 0;
  ds.early_hello = 0;
  ds.gave_up = false;
  ds.hello_sent = ds.hello_rcvd = ds.go = ds.peer_ready = false;
  memset(ds.peer_pack_len, 0, sizeof(ds.peer_pack_len));
  memset(ds.peer_rcvd, 0, sizeof(ds.peer_rcvd));
  memset(ds.rtt_n, 0, sizeof(ds.rtt_n));
  memset(ds.ping_out, 0, sizeof(ds.ping_out));
  memset(ds.ow_us, 0, sizeof(ds.ow_us));
  memset(ds.keys, 0, sizeof(ds.keys));
  memset(ds.frame_ok, 0, sizeof(ds.frame_ok));
  memset(ds.my_hash, 0, sizeof(ds.my_hash));
  memset(ds.peer_hash, 0, sizeof(ds.peer_hash));
  memset(ds.hash_done, 0, sizeof(ds.hash_done));
  memset(ds.hash_bad, 0, sizeof(ds.hash_bad));
  memset(ds.peer_pos_n, 0, sizeof(ds.peer_pos_n));
  memset(ds.peer_lead, 0, sizeof(ds.peer_lead));
  for (s = 0; s < NP; s++) {
    ds.slot_id[s] = -1;
    ds.t_heard[s] = now;
  }
  ds.t_state = now;
}

static void dual_start_running(void) {
  u32 i;
  unsigned s;
  ds.state = DS_RUN;
  ds.frame = 0;
  // Nobody presses anything during the first frames (the delay).
  for (i = 0; i < ds.delay; i++) {
    ds.own_keys[i] = 0;
    for (s = 0; s < NP; s++) {
      ds.keys[s][i] = 0;
      ds.frame_ok[s][i] = i + 1;
    }
  }
  ds.sent_upto = ds.delay;
  ds.send_delay = ds.delay;
  memset(ds.stalls, 0, sizeof(ds.stalls));
  memset(ds.stall_frame, 0, sizeof(ds.stall_frame));
  memset(ds.peer_stalls, 0, sizeof(ds.peer_stalls));
  ds.t_peer_stall = netpacket_time_usec();
  ds.t_lowered = 0;
  ds.calm_us = DUAL_DELAY_CALM_US;
  ds.sync_next = DUAL_SYNC_GAP;
  ds.reset_wanted = false;
  for (s = 0; s < NP; s++)
    ds.t_heard[s] = netpacket_time_usec();
  {
    char msg[96];
    if (ds.n == 2)
      snprintf(msg, sizeof(msg), "Cable Link: both GBAs connected (%u frames of delay)",
               (unsigned)ds.delay);
    else
      snprintf(msg, sizeof(msg), "Cable Link: %u GBAs connected (%u frames of delay)",
               ds.n, (unsigned)ds.delay);
    dual_msg(msg);
  }
}

// Input delay from the round trips: the buttons must get to every other
// console before it needs them, and between two clients they go through the
// host, which only passes them on when the frontend reads the network (once
// a frame: up to a frame more). The time sync keeps the consoles within
// DUAL_SYNC_US of each other; on top of that the network takes one way, and
// a frame more covers it having a bad moment.
static u32 dual_pick_delay(void) {
  u32 a = 0, b = ds.n > 2 ? DUAL_FRAME_US : 0, d;   // (+ the relay)
  u32 ow_b = 0;            // The two longest one way times: a, ow_b
  unsigned s;
  for (s = 1; s < ds.n; s++) {
    const u32 ow = ds.ow_us[s];
    if (ow > a) {
      ow_b = a;
      a = ow;
    }
    else if (ow > ow_b)
      ow_b = ow;
  }
  d = (a + ow_b + b + DUAL_SYNC_US + DUAL_FRAME_US + DUAL_FRAME_US - 1) /
      DUAL_FRAME_US;
  return MAX(2, MIN(d, 30));
}

static void dual_waiting_msg(unsigned have) {
  char msg[96];
  snprintf(msg, sizeof(msg), "Cable Link: waiting for players (%u/%u)",
           have, ds.players);
  dual_msg(msg);
}

// Host: one more client said HELLO: tell everybody how many are here.
static void dual_cand_ready(void) {
  u32 wt[2] = { ds.ncand + 1, ds.players };
  if (ds.ncand + 1 < ds.players) {
    dual_send_all(DPK_WAITING, wt, 2, NULL, 0);
    dual_waiting_msg(ds.ncand + 1);
  }
}

// Host: everybody is here. Each client gets its slot, in the order they came.
static void dual_host_start(void) {
  u32 w[1 + NP];
  unsigned s;
  ds.n = ds.ncand + 1;
  ds.my_slot = 0;
  ds.slot_id[0] = 0;
  for (s = 1; s < ds.n; s++)
    ds.slot_id[s] = ds.cand[s - 1].id;
  w[0] = ds.n;
  for (s = 0; s < NP; s++)
    w[1 + s] = (u32)ds.slot_id[s];
  dual_send_all(DPK_START, w, 1 + NP, NULL, 0);
  ds.state = DS_PING;
  ds.t_state = netpacket_time_usec();
  for (s = 1; s < ds.n; s++)
    dual_send_ping(s);
}

static void dual_begin_transfer(void) {
  if (!b_load_for(ds.n) || !dual_capture()) {
    dual_send_all(DPK_BYE, NULL, 0, NULL, 0);
    dual_stop("Cable Link: not enough memory for the other GBAs");
    return;
  }
  ds.state = DS_TRANSFER;
  dual_send_blob();
}

// Setup steps, between frames.
static void dual_setup_step(void) {
  const u64 now = netpacket_time_usec();
  unsigned s;

  if (ds.state == DS_HELLO) {
    // Client. The host may not be listening yet (it only is once the
    // frontend tells it about us): say it again until it answers.
    if (!ds.hello_sent || now - ds.t_hello > DUAL_HELLO_EVERY)
      dual_send_hello(0);
    if (now - ds.t_state > DUAL_HELLO_US)
      dual_give_up(NULL);       // The host does not have this core
    return;
  }

  if (ds.state == DS_WAIT) {
    if (!ds.is_host)
      return;                   // Waiting for START (the game goes on)
    // Host: a client that never says HELLO does not have this core.
    for (s = 0; s < ds.ncand; s++)
      if (!ds.cand[s].hello && now - ds.cand[s].t_join > DUAL_HELLO_US) {
        dual_give_up("Cable Link: a console does not have this core, using the real time cable");
        return;
      }
    if (ds.ncand + 1 == ds.players) {
      for (s = 0; s < ds.ncand && ds.cand[s].hello; s++);
      if (s == ds.ncand)
        dual_host_start();
    }
    return;
  }

  if (ds.state == DS_PING) {
    if (ds.is_host) {
      // The round trip to each client, one ping at a time each, so that each
      // one has the network to itself.
      bool done = true;
      for (s = 1; s < ds.n; s++)
        if (ds.rtt_n[s] < DUAL_PINGS) {
          done = false;
          if (!ds.ping_out[s] || now - ds.t_ping[s] > DUAL_PONG_US)
            dual_send_ping(s);
        }
      if (done || now - ds.t_state > DUAL_PING_US) {
        for (s = 1; s < ds.n; s++)
          ds.ow_us[s] = dual_rtt_min(s) / 2;
        ds.delay = ds.n > 1 ? dual_pick_delay() : 4;
        dual_send_all(DPK_GO, NULL, 0, NULL, 0);
        dual_begin_transfer();
      }
    }
    else if (ds.go)
      dual_begin_transfer();
  }

  if (ds.state == DS_TRANSFER && dual_have_states()) {
    if (!dual_load_machines()) {
      dual_send_all(DPK_BYE, NULL, 0, NULL, 0);
      dual_stop("Cable Link: could not start the other GBAs");
      return;
    }
    if (ds.is_host) {
      u32 w[1 + NP];
      w[0] = ds.delay;
      for (s = 0; s < NP; s++)
        w[1 + s] = ds.ow_us[s];
      dual_send_all(DPK_READY, w, 1 + NP, NULL, 0);
    }
    ds.state = DS_READY;
  }

  if (ds.state == DS_READY && (ds.is_host || ds.peer_ready))
    dual_start_running();

  if (ds.state >= DS_PING && ds.state < DS_RUN &&
      now - ds.t_state > DUAL_SETUP_US) {
    dual_send_all(DPK_BYE, NULL, 0, NULL, 0);
    dual_stop("Cable Link: another console did not answer");
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

// The settings, again, as a netplay session starts (between sessions). The
// hidden GBAs took theirs from the options when they were loaded: if they
// changed since, they are loaded again.
void dual_set_cfg(u32 cfg) {
  unsigned k;
  if (cfg == ds.cfg)
    return;
  dual_stop(NULL);
  ds.cfg = cfg;
  for (k = 0; k < DUAL_HIDDEN; k++)
    b_unload(k);
}

// The core option. (A session that is on keeps its own number.)
void dual_set_players(unsigned players) {
  ds.cfg_players = MAX(2, MIN(players, DUAL_MAX_PLAYERS));
  if (ds.state == DS_OFF && !ds.gave_up)
    ds.players = ds.cfg_players;
}

// A netplay session started (Cable Link mode). The host waits for clients.
// The slow part of the setup (which game this is, the other GBAs) is done
// now: the host does it while it waits, a client while its HELLO is on the
// way.
void dual_session_start(unsigned client_id) {
  dual_stop(NULL);
  dual_session_reset();
  ds.players = MAX(2, MIN(ds.cfg_players, DUAL_MAX_PLAYERS));
  ds.boot_reset = false;
  ds.is_host = (client_id == 0);
  ds.my_id = client_id;
  if (ds.is_host)
    ds.state = DS_WAIT;
  else {
    ds.state = DS_HELLO;
    dual_send_hello(0);
  }
  dual_rom_crc();
  // Only the GBAs the option asks for: free the others (left from a session
  // with more players), the memory of the console is small.
  {
    unsigned k;
    for (k = ds.players - 1; k < DUAL_HIDDEN; k++)
      b_unload(k);
  }
  b_load_for(ds.players);   // (if it fails, the setup says so)
  // The time to answer counts from now, not from before the slow part.
  ds.t_state = netpacket_time_usec();
}

// Host: a client joined. It takes part if the cable is not full yet.
bool dual_peer_connected(unsigned client_id) {
  const u64 bit = client_id < 64 ? 1ull << client_id : 0;
  // (The real time cable takes it: nobody uses the dual cable now, ie. it
  // stopped after a console left.)
  if (!ds.is_host || ds.gave_up || ds.state == DS_OFF)
    return true;
  if (ds.state != DS_WAIT || ds.ncand + 1 >= ds.players)
    return false;
  ds.cand[ds.ncand].id = client_id;
  ds.cand[ds.ncand].hello = (ds.early_hello & bit) != 0;
  ds.cand[ds.ncand].t_join = netpacket_time_usec();
  ds.early_hello &= ~bit;
  ds.ncand++;
  if (ds.cand[ds.ncand - 1].hello)
    dual_cand_ready();
  return true;
}

void dual_peer_disconnected(unsigned client_id) {
  unsigned s;
  if (!ds.is_host || ds.state == DS_OFF)
    return;
  if (ds.state == DS_WAIT) {
    // Not playing yet: it just is not there any more. (A HELLO it sent
    // before the frontend told us about it is forgotten too: the next
    // console to get this id is another one.)
    if (client_id < 64)
      ds.early_hello &= ~(1ull << client_id);
    for (s = 0; s < ds.ncand; s++)
      if (ds.cand[s].id == (int)client_id) {
        memmove(&ds.cand[s], &ds.cand[s + 1], (ds.ncand - s - 1) * sizeof(ds.cand[0]));
        ds.ncand--;
        {
          u32 w[2] = { ds.ncand + 1, ds.players };
          dual_send_all(DPK_WAITING, w, 2, NULL, 0);
        }
        dual_waiting_msg(ds.ncand + 1);
        break;
      }
    return;
  }
  for (s = 1; s < ds.n; s++)
    if (ds.slot_id[s] == (int)client_id) {
      dual_send_all(DPK_BYE, NULL, 0, NULL, 0);
      dual_stop(ds.state == DS_RUN ? "Cable Link: a console left" : NULL);
      return;
    }
}

void dual_session_stop(void) {
  dual_stop(NULL);
  ds.state = DS_OFF;
}

// Whether the dual cable is setting up (true: do not run the game now).
// The game is stopped anyway: rather than one step per frame, go on for most
// of the frame, as soon as the others answer. (While the host waits for the
// players, the games go on.)
bool dual_setting_up(void) {
  const u64 t0 = netpacket_time_usec();
  if (ds.state == DS_WAIT) {
    dual_setup_step();
    if (ds.state == DS_WAIT || ds.state == DS_OFF)
      return false;
  }
  while (ds.state != DS_OFF && ds.state != DS_RUN && ds.state != DS_WAIT) {
    dual_setup_step();
    if (ds.state == DS_OFF || ds.state == DS_RUN || ds.state == DS_WAIT ||
        netpacket_time_usec() - t0 > DUAL_SLICE_US)
      break;
    netpacket_poll_receive();
    usleep(250);
  }
  return ds.state != DS_OFF && ds.state != DS_RUN && ds.state != DS_WAIT;
}

bool dual_running(void) {
  return ds.state == DS_RUN;
}

// Whether the dual cable may still be used in this session (it is not
// running yet, and nobody gave up): the real time cable stays unplugged.
bool dual_cable_pending(void) {
  return ds.state != DS_OFF && ds.state != DS_RUN;
}

// Whether the game must restart now for the real time cable (once).
bool dual_take_boot_reset(void) {
  const bool r = ds.boot_reset;
  ds.boot_reset = false;
  return r;
}

// The player pressed reset: it goes with the next buttons sent.
void dual_request_reset(void) {
  if (ds.state == DS_RUN)
    ds.reset_wanted = true;
}

// How far ahead of slot `s` we are (us, at `now`): where we are (the frame
// about to start) against where it is (the frame it was starting when it
// sent its last buttons, plus the time since).
static s32 dual_lead(unsigned s, u64 now) {
  s64 best;
  u32 i;
  if (!ds.peer_pos_n[s])
    return 0;
  best = ds.peer_pos[s][0];
  for (i = 1; i < MIN(ds.peer_pos_n[s], DUAL_SYNC_WIN); i++)
    best = MAX(best, ds.peer_pos[s][i]);
  return (s32)((s64)ds.frame * DUAL_FRAME_US - (s64)now - best);
}

// Every GBA starts over, with the cable plugged again (as at the start).
static void dual_reset_all(void) {
  unsigned k;
  reset_gba();
  for (k = 0; k + 1 < ds.n; k++)
    hid_if[k].reset();
  bus_reset(ds.n);
  dualport_begin(ds.my_slot);
  for (k = 0; k + 1 < ds.n; k++)
    hid_if[k].port_begin(hidden_slot(k));
}

// The buttons for this frame. Returns DUAL_FRAME_WAIT if someone's are not
// here yet, DUAL_FRAME_HOLD if we are ahead of the others: the frame is not
// run now.
int dual_frame_begin(u32 local_keys) {
  const u32 f = ds.frame;
  const u32 fi = f % DUAL_RING;
  const u64 t0 = netpacket_time_usec();
  s32 lead[NP] = { 0 }, ahead = 0;
  bool full = true;
  unsigned s, k;

  // Ahead of another console (each pair works it out the same way, so only
  // one of the two can be): stay a frame where we are. Never so often that
  // the others cannot see it before the next time. Each client keeps with
  // the host, and the host with each client: then they all keep together.
  // (Between two clients the buttons go through the host, and when it
  // passes them on varies too much to tell where the other one is.)
  for (s = 0; s < ds.n; s++)
    if (s != ds.my_slot) {
      lead[s] = dual_lead(s, t0);
      if (ds.my_slot && s)
        continue;
      ahead = MAX(ahead, (lead[s] - ds.peer_lead[s]) / 2);
      if (ds.peer_pos_n[s] < DUAL_SYNC_WIN)
        full = false;
    }
  if (f >= ds.sync_next && full && ahead > DUAL_SYNC_US) {
    ds.sync_next = f + DUAL_SYNC_GAP;
    return DUAL_FRAME_HOLD;
  }

  // The network had a bad moment and someone waited for our buttons: they
  // go out earlier (each console sends its own buttons for whatever frame
  // it likes, so this needs no agreement). Back down once all is calm.
  if (ds.send_delay > ds.delay && t0 - ds.t_peer_stall > ds.calm_us) {
    ds.send_delay--;
    ds.t_peer_stall = ds.t_lowered = t0;
  }

  // Ours for frame f + delay. (With a shorter delay than last time, nothing
  // goes out until it catches up: those frames have their buttons already.)
  while (ds.sent_upto <= f + ds.send_delay) {
    u32 w[3 + 2 * NP];
    w[0] = ds.sent_upto;
    w[1] = local_keys | (ds.reset_wanted ? DUAL_KEY_RESET : 0);
    ds.reset_wanted = false;
    w[2] = f;
    for (s = 0; s < NP; s++) {
      w[3 + s] = (u32)lead[s];
      w[3 + NP + s] = ds.stalls[s];
    }
    ds.own_keys[ds.sent_upto % DUAL_RING] = w[1];
    dual_send_all(DPK_INPUT, w, 3 + 2 * NP, NULL, 0);
    ds.sent_upto++;
  }

  // Everybody else's for frame f.
  while (1) {
    const u64 now = netpacket_time_usec();
    bool missing = false;
    for (s = 0; s < ds.n; s++)
      if (s != ds.my_slot && ds.frame_ok[s][fi] != f + 1)
        missing = true;
    if (!missing)
      break;
    if (ds.state != DS_RUN)
      return DUAL_FRAME_WAIT;
    for (s = 0; s < ds.n; s++)
      if (s != ds.my_slot && now - ds.t_heard[s] > DUAL_PEER_US) {
        dual_send_all(DPK_BYE, NULL, 0, NULL, 0);
        dual_stop("Cable Link: a console stopped answering");
        return DUAL_FRAME_WAIT;
      }
    if (now - t0 > DUAL_STALL_US) {
      for (s = 0; s < ds.n; s++)
        if (s != ds.my_slot && ds.frame_ok[s][fi] != f + 1)
          dual_count_stall(s, f);
      return DUAL_FRAME_WAIT;
    }
    netpacket_poll_receive();
    usleep(250);
  }

  // A player pressed reset: every console restarts every game in this very
  // frame, with the cable plugged again (as at the start of the session).
  {
    u32 any = ds.own_keys[fi];
    for (s = 0; s < ds.n; s++)
      if (s != ds.my_slot)
        any |= ds.keys[s][fi];
    if (any & DUAL_KEY_RESET)
      dual_reset_all();
  }

  dualport_set_keys(ds.own_keys[fi] & ~DUAL_KEY_RESET);
  for (k = 0; k + 1 < ds.n; k++)
    hid_if[k].set_keys(ds.keys[hidden_slot(k)][fi] & ~DUAL_KEY_RESET);
#ifdef DUAL_DEBUG
  // Test of the agreement check: a hidden GBA gets other buttons once.
  if (getenv("DUAL_FAULT") && f == 300)
    hid_if[0].set_keys(ds.keys[hidden_slot(0)][fi] ^ 0x1);
#endif
  return DUAL_FRAME_RUN;
}

void dual_b_frame_start(void) {
  unsigned k;
  DBG("[dual] frame %u starts\n", ds.frame);
  for (k = 0; k + 1 < ds.n; k++)
    sem_post(&ds.hid[k].start);
}

void dual_b_frame_wait(void) {
  unsigned k;
  for (k = 0; k + 1 < ds.n; k++)
    sem_wait(&ds.hid[k].done);
}

// Every copy of every GBA must be the same: compare once each one told.
// The link is cut when two checks in a row differ. Consoles of different
// kinds (ARM32 Miyoo Mini, ARM64 Android...) can disagree for a moment on
// values nothing reads: the CPU flags an instruction left unused (each
// dynarec computes only the ones it needs), which an interrupt can also
// push on the stack. Those are gone by the next check; a GBA that really
// went another way stays different.
static void dual_check_hash(u32 f) {
  const u32 i = (f / DUAL_HASH_EVERY) % DUAL_HASH_SLOTS;
  const u32 *mine = ds.my_hash[i];
  unsigned s, g;
  if (mine[0] != f + 1)
    return;
  for (s = 0; s < ds.n; s++) {
    const u32 *theirs = ds.peer_hash[s][i];
    if (s == ds.my_slot || theirs[0] != f + 1 || ds.hash_done[s] >= f + 1)
      continue;
    ds.hash_done[s] = f + 1;
    for (g = 0; g < ds.n; g++)
      if (mine[1 + g] != theirs[1 + g])
        break;
    if (g == ds.n)
      continue;
    if (f >= DUAL_HASH_EVERY && ds.hash_bad[s] == f + 1 - DUAL_HASH_EVERY) {
      dual_send_all(DPK_BYE, NULL, 0, NULL, 0);
      dual_stop("Cable Link: the consoles went out of sync");
      return;
    }
    ds.hash_bad[s] = f + 1;
  }
}

void dual_frame_end(void) {
  const u32 f = ds.frame++;
  if (f % DUAL_HASH_EVERY == 0) {
    u32 w[1 + NP], *mine = ds.my_hash[(f / DUAL_HASH_EVERY) % DUAL_HASH_SLOTS];
    unsigned s;
    w[0] = f;
    mine[0] = f + 1;
    for (s = 0; s < NP; s++) {
      w[1 + s] = s < ds.n ? hash_slot(s) : 0;
      mine[1 + s] = w[1 + s];
    }
    dual_send_all(DPK_HASH, w, 1 + NP, NULL, 0);
    dual_check_hash(f);
  }
}

// Returns true if the packet was ours.
bool dual_net_receive(const void *buf, size_t len, uint16_t client_id) {
  const u32 *pkt = (const u32*)buf;
  const u64 now = netpacket_time_usec();
  u32 type, from;
  unsigned s;
  if (len < 8 || netorder32(pkt[0]) != DUAL_HEADER)
    return false;
  if (ds.state == DS_OFF)
    return true;

  type = netorder32(pkt[1]) & 0xFF;
  from = (netorder32(pkt[1]) >> 8) & 0xFF;
  // (START is what tells a client who is in which slot.)
  if (type == DPK_START)
    from = DUAL_NO_SLOT;
  // Packets with a slot: only from a console of this session.
  if (from != DUAL_NO_SLOT) {
    if (!ds.n || from >= ds.n || from == ds.my_slot ||
        ds.slot_id[from] != (int)client_id)
      return true;
    ds.t_heard[from] = now;
  }

  switch (type) {
  case DPK_HELLO:
    if (len < 8 + 6 * 4)
      break;
    {
      u32 w[6];
      for (s = 0; s < 6; s++)
        w[s] = netorder32(pkt[2 + s]);
      if (ds.is_host && ds.state == DS_WAIT) {
        for (s = 0; s < ds.ncand && ds.cand[s].id != (int)client_id; s++);
        if (!dual_hello_matches(w)) {
          // Same answer for everybody: the real time cable.
          dual_give_up(w[4] != ds.players ? DUAL_MSG_PLAYERS : DUAL_MSG_DIFFER);
          break;
        }
        // Answer right away, even if the frontend has not told us about it
        // yet: otherwise it would think we do not have this core.
        dual_send_hello(client_id);
        if (s == ds.ncand) {
          if (client_id < 64)
            ds.early_hello |= 1ull << client_id;
          break;
        }
        if (!ds.cand[s].hello) {
          ds.cand[s].hello = true;
          dual_cand_ready();
        }
      }
      else if (!ds.is_host && ds.state == DS_HELLO && client_id == 0) {
        ds.hello_rcvd = true;
        if (!dual_hello_matches(w))
          dual_give_up(w[4] != ds.players ? DUAL_MSG_PLAYERS : DUAL_MSG_DIFFER);
        else
          ds.state = DS_WAIT;
      }
    }
    break;

  case DPK_WAITING:
    if (len >= 16 && !ds.is_host && client_id == 0 &&
        (ds.state == DS_WAIT || ds.state == DS_HELLO)) {
      const u32 have = netorder32(pkt[2]);
      if (have < ds.players)
        dual_waiting_msg(have);
    }
    break;

  case DPK_START:
    if (len >= 8 + (1 + NP) * 4 && !ds.is_host && client_id == 0 &&
        (ds.state == DS_WAIT || ds.state == DS_HELLO)) {
      const u32 n = netorder32(pkt[2]);
      if (n < 2 || n > NP || n != ds.players) {
        dual_give_up(DUAL_MSG_PLAYERS);
        break;
      }
      ds.n = n;
      ds.my_slot = DUAL_NO_SLOT;
      for (s = 0; s < NP; s++) {
        ds.slot_id[s] = (int)netorder32(pkt[3 + s]);
        if (s < n && ds.slot_id[s] == (int)ds.my_id)
          ds.my_slot = s;
      }
      if (ds.my_slot == DUAL_NO_SLOT || !ds.my_slot) {
        ds.n = 0;
        dual_give_up(NULL);
        break;
      }
      for (s = 0; s < NP; s++)
        ds.t_heard[s] = now;
      ds.state = DS_PING;
      ds.t_state = now;
    }
    break;

  case DPK_PING:
    if (len >= 16) {
      u32 w[2] = { netorder32(pkt[2]), netorder32(pkt[3]) };
      dual_send(client_id, DPK_PONG, w, 2, NULL, 0);
    }
    break;

  case DPK_PONG:
    if (len >= 16 && ds.is_host && from != DUAL_NO_SLOT) {
      const u64 t = netorder32(pkt[2]) | ((u64)netorder32(pkt[3]) << 32);
      ds.rtt[from][ds.rtt_n[from] % DUAL_PINGS] = (u32)(now - t);
      ds.rtt_n[from]++;
      ds.ping_out[from] = false;
    }
    break;

  case DPK_GO:
    if (!ds.is_host && from == 0)
      ds.go = true;
    break;

  case DPK_STATE:
    if (len > 16 && from != DUAL_NO_SLOT &&
        ds.state >= DS_PING && ds.state <= DS_TRANSFER) {
      const u32 off = netorder32(pkt[2]);
      const u32 total = netorder32(pkt[3]);
      const u32 dlen = len - 16;
      if (!ds.peer_pack[from])
        ds.peer_pack[from] = (u8*)malloc(DUAL_PACK_MAX);
      // (Checked so that no sum can wrap around.)
      if (ds.peer_pack[from] && total <= DUAL_PACK_MAX && off <= total &&
          dlen <= total - off) {
        memcpy(&ds.peer_pack[from][off], &pkt[4], dlen);
        ds.peer_pack_len[from] = total;
        ds.peer_rcvd[from] += dlen;
      }
    }
    break;

  case DPK_READY:
    if (len >= 8 + (1 + NP) * 4 && !ds.is_host && from == 0) {
      ds.delay = MAX(1, MIN(netorder32(pkt[2]), 30));
      for (s = 0; s < NP; s++)
        ds.ow_us[s] = netorder32(pkt[3 + s]);
      ds.peer_ready = true;
    }
    break;

  case DPK_INPUT:
    if (len >= 8 + (3 + 2 * NP) * 4 && from != DUAL_NO_SLOT &&
        ds.state >= DS_TRANSFER) {
      const u32 f = netorder32(pkt[2]);
      const u32 their_stalls = netorder32(pkt[5 + NP + ds.my_slot]);
      ds.keys[from][f % DUAL_RING] = netorder32(pkt[3]);
      ds.frame_ok[from][f % DUAL_RING] = f + 1;
      // It was starting this frame one way ago.
      ds.peer_pos[from][ds.peer_pos_n[from]++ % DUAL_SYNC_WIN] =
        (s64)netorder32(pkt[4]) * DUAL_FRAME_US + dual_ow(ds.my_slot, from) -
        (s64)now;
      ds.peer_lead[from] = (s32)netorder32(pkt[5 + ds.my_slot]);
      // It waited for our buttons: they go out a frame earlier.
      if (their_stalls != ds.peer_stalls[from] && ds.state == DS_RUN) {
        ds.peer_stalls[from] = their_stalls;
        ds.send_delay = MIN(ds.send_delay + 1,
                            MIN(ds.delay + DUAL_DELAY_MORE, DUAL_RING / 2));
        // Taking that frame off was too soon: next time, wait longer.
        if (ds.t_lowered && now - ds.t_lowered < ds.calm_us)
          ds.calm_us = MIN(ds.calm_us * 2, DUAL_DELAY_CALM_MAX);
        ds.t_peer_stall = now;
      }
    }
    break;

  case DPK_HASH:
    if (len >= 8 + (1 + NP) * 4 && from != DUAL_NO_SLOT && ds.state == DS_RUN) {
      const u32 f = netorder32(pkt[2]);
      u32 *theirs = ds.peer_hash[from][(f / DUAL_HASH_EVERY) % DUAL_HASH_SLOTS];
      theirs[0] = f + 1;
      for (s = 0; s < NP; s++)
        theirs[1 + s] = netorder32(pkt[3 + s]);
      dual_check_hash(f);
    }
    break;

  case DPK_GIVEUP:
    if (!ds.is_host && client_id == 0) {
      dual_stop(NULL);
      ds.gave_up = true;
      ds.boot_reset = true;
      dual_msg(len >= 12 && netorder32(pkt[2]) ? DUAL_MSG_PLAYERS : DUAL_MSG_DIFFER);
    }
    break;

  case DPK_BYE:
    if (ds.is_host && ds.state == DS_WAIT && from == DUAL_NO_SLOT) {
      // A client that cannot take part: the real time cable for everybody.
      dual_give_up(DUAL_MSG_DIFFER);
      break;
    }
    if (from != DUAL_NO_SLOT || client_id == 0)
      dual_stop(ds.state == DS_RUN ? "Cable Link: a console left" : NULL);
    break;
  };
  return true;
}

void dual_shutdown(void) {
  unsigned k;
  dual_stop(NULL);
  bus.abort = 1;
  for (k = 0; k < DUAL_HIDDEN; k++)
    b_unload(k);
}
