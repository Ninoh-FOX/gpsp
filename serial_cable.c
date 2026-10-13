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

// Serial-Cable: a real (protocol agnostic) link cable between GBAs.
//
// Unlike the Pokemon/Advance Wars modes, nothing is faked or batched here:
// every single serial transfer is exchanged with the other consoles while it
// happens, exactly as the wire would do it.
//
// The netplay host is the parent (player 1), clients are children. For each
// multiplayer transfer:
//
//   parent                                child(ren)
//                             <--------   REPLY(seq, child word)    [early]
//     XFER(seq, parent word)  -------->   start the transfer
//                             <--------   REPLY(seq, child word)    [if not sent yet]
//     [transfer time elapses, waits for the replies if they are late]
//     DONE(seq, all 4 words)  -------->   fill SIOMULTIx, raise serial IRQ
//     raise serial IRQ
//
// A network round trip per transfer is what makes this slow, so we avoid as
// many as we can without changing what the games see:
//
//  - Games load their next word right after a transfer ends (serial IRQ).
//    As soon as a child does so, it sends the word for the *next* transfer
//    without waiting to be asked. The parent usually has it by the time it
//    needs it. If the game did not write anything, the word is sampled when
//    the transfer starts, as usual.
//  - With only two consoles the child knows both words as soon as XFER gets
//    there, so it finishes the transfer on its own and no DONE is needed.
//    The transfers then overlap: two of them fit in a single round trip.
//
// Normal (8/32 bit) mode is a plain XFER/REPLY swap between the parent and
// player 2.
//
// Both consoles must also advance at the same pace. Emulators run a frame
// in a burst and then sleep in the frontend, where they cannot answer. A
// child that runs ahead goes to sleep right when the parent needs it, and
// every transfer of the frame is then stuck behind that nap. To prevent it
// the parent reports its emulated clock (PKT_TICK and every transfer) and
// the children never run past it: they wait for the parent inside the core,
// where they still answer at once. Children also line up the end of their
// frames with the parent's, so that everybody goes to sleep at the same time
// and nobody is missed.
//
// All waits are bounded: if a peer stops answering (paused, menu, gone) we
// stop waiting for it and it simply reads as a console that is not
// transmitting.

#include "common.h"

// Debug print logic:
//#define SERIALCABLE_DEBUG 1
#ifdef SERIALCABLE_DEBUG
  #define CBL_DEBUG_LOG(...) printf(__VA_ARGS__)
#else
  #define CBL_DEBUG_LOG(...)
#endif

void netpacket_send(uint16_t client_id, const void *buf, size_t len);
void netpacket_poll_receive();
bool netpacket_active(void);
u64 netpacket_time_usec(void);

#define NET_CABLE_HEADER       0x4d434232   // MCB2 (Multiplayer CaBle, v2)

#define PKT_STATUS             1    // a: status flags
#define PKT_XFER               2    // a: data, b: transfer config, c: parent clock
#define PKT_REPLY              3    // a: data, b: transfer mode
#define PKT_DONE               4    // a: words 0/1, b: words 2/3, c: parent clock
#define PKT_TICK               5    // a: tick flags, c: parent clock

// Status flags
#define ST_MULTI               0x1  // Console is in multiplayer mode
#define ST_SO                  0x2  // SO line level (normal mode)
#define ST_ALLREADY            0x4  // Parent only: every console is in multiplayer mode
#define ST_UNKNOWN             0xFF

// Tick flags
#define TICK_ACTIVE            0x1  // The link is in use, keep up with the parent
#define TICK_FRAME_END         0x2  // Parent just finished a frame

// Transfer modes (XFER config, bits 0..7)
#define XMODE_MULTI            0
#define XMODE_NORMAL           1

#define CBL_QUEUE_SIZE         16
#define CBL_POLL_CYCLES        4096      // Children poll the network ~68 times per frame
#define CBL_TICK_CYCLES        35112     // Parent reports its clock 8 times per frame
#define CBL_ACTIVE_CYCLES      (60 * CBL_FRAME_CYCLES)  // The link is in use for ~1s after a transfer
#define CBL_SLACK_CYCLES       2048      // How far past the parent clock a child can run
#define CBL_FRAME_CYCLES        280896
#define CBL_MAX_LAG_CYCLES     CBL_FRAME_CYCLES   // A child that fell behind does not owe more than this
#define CBL_SETTLE_CYCLES      128       // Let the game finish writing its word

// Host-time limits for the blocking waits (microseconds).
#define CBL_PARENT_TIMEOUT_US  100000    // Waiting for the children words
#define CBL_CHILD_TIMEOUT_US   50000     // Waiting for the parent to catch up
#define CBL_MAX_WAIT_POLLS     2000000   // Safety net if the host has no usable clock

// Transfer timings, see serial.c
static const u16 multi_cycles[4] = { 31457, 7864, 5242, 2621 };
#define NORMAL_CYC_256KHZ_8BIT   524
#define NORMAL_CYC_2MHZ_8BIT      67

#define ME   (netplay_client_id & 3)

typedef struct {
  u8 type;
  u32 seq, a, b, c;
} cable_event;

static struct {
  struct {
    u8 present;                  // Console is plugged to the cable
    u8 flags;                    // Last reported status
    u8 responsive;               // Answered in time last time we waited for it
    // Words we were sent: the one for the ongoing transfer and, possibly,
    // the one for the next transfer (indexed by the lowest sequence bit).
    struct {
      u8 valid;
      u8 mode;                   // Kind of transfer it was meant for
      u32 seq;
      u32 data;
    } reply[2];
  } peer[4];

  u8 status_sent;                // Last status we reported
  u8 link_dirty;                 // Line levels might have changed
  u32 clock;                     // Emulated time
  u32 poll_cycles;

  // Parent
  u8 m_busy;                     // Transfer in progress
  u8 m_mode;
  u8 m_mask;                     // Consoles taking part in the transfer
  u8 m_bits32;
  u32 m_seq;
  u32 m_cycles;                  // Remaining transfer time
  u32 m_send;
  u32 m_active;                  // Time left until the link is considered idle
  u32 m_tick;                    // Time since we last reported our clock

  // Child
  cable_event queue[CBL_QUEUE_SIZE];
  u8 q_head, q_count;
  u8 s_inxfer;                   // Transfer in progress
  u8 s_local;                    // ...that we finish ourselves (no DONE needed)
  u8 s_live;                     // Parent is using the link and reporting its clock
  u8 s_resync;                   // Parent is back, adopt its clock
  u8 s_wrote;                    // Game loaded a new word to send
  u8 s_sent;                     // We sent our word...
  u8 s_pframe_valid;
  u16 s_sent_word;
  u32 s_sent_seq;                // ...for this transfer
  u16 s_pword;                   // Parent word of the ongoing transfer
  u32 s_seq;                     // Last transfer seen
  u32 s_gap;                     // Time until our next word can be sampled
  u32 s_settle;                  // Time until the written word is stable
  u32 s_xferleft;                // Remaining time of the ongoing transfer
  u32 s_wordcyc;                 // Duration of one word of the ongoing transfer
  u32 s_time;                    // Our position on the parent clock
  u32 s_pclock;                  // Parent clock, as last reported
  u32 s_pframe;                  // Parent clock at the end of its last frame
} cbl;

static void cable_send(u16 client_id, u32 type, u32 seq, u32 a, u32 b, u32 c) {
  u32 pkt[6] = {
    netorder32(NET_CABLE_HEADER), netorder32(type), netorder32(seq),
    netorder32(a), netorder32(b), netorder32(c)
  };
  netpacket_send(client_id, pkt, sizeof(pkt));
}

static bool in_multi_mode(void) {
  return !(read_ioreg(REG_RCNT) & 0x8000) &&
         (read_ioreg(REG_SIOCNT) & 0x3000) == 0x2000;
}

static bool in_normal_mode(void) {
  return !(read_ioreg(REG_RCNT) & 0x8000) && !(read_ioreg(REG_SIOCNT) & 0x2000);
}

// Whether every console on the cable is in multiplayer mode (SD line).
static bool parent_all_ready(void) {
  u32 i;
  for (i = 1; i <= 3; i++)
    if (cbl.peer[i].present && !(cbl.peer[i].flags & ST_MULTI))
      return false;
  return true;
}

static u8 own_status(void) {
  u8 flags = 0;
  if (in_multi_mode()) {
    flags |= ST_MULTI;
    if (!ME && parent_all_ready())
      flags |= ST_ALLREADY;
  }
  else if (in_normal_mode() && (read_ioreg(REG_SIOCNT) & 0x0008))
    flags |= ST_SO;
  return flags;
}

// Tell the other consoles about our mode/lines whenever they change.
static void cable_sync_status(void) {
  u8 flags = own_status();
  if (flags != cbl.status_sent && netpacket_active()) {
    cbl.status_sent = flags;
    cable_send(RETRO_NETPACKET_BROADCAST, PKT_STATUS, 0, flags, 0, 0);
  }
}

// SI/SD/ID fields of SIOCNT in multiplayer mode.
u16 serialcable_mul_siocnt(void) {
  bool ready;
  if (!netpacket_active())
    return 0x8;        // Nothing plugged: lone parent.

  if (!ME)
    ready = parent_all_ready();
  else
    ready = cbl.peer[0].present && (cbl.peer[0].flags & ST_ALLREADY);

  return (ME << 4) | (ME ? 0x4 : 0) | (ready ? 0x8 : 0);
}

// Refresh the SIOCNT bits that are driven by the other consoles.
static void cable_refresh_lines(void) {
  u16 siocnt = read_ioreg(REG_SIOCNT);
  if (in_multi_mode()) {
    if (ME)   // For children the start bit is just the busy flag.
      siocnt = (siocnt & ~0x0080) | (cbl.s_inxfer ? 0x0080 : 0);
    write_ioreg(REG_SIOCNT, (siocnt & 0xFF83) | serialcable_mul_siocnt());
  }
  else if (in_normal_mode()) {
    // SO of the parent is wired to the SI of the second console.
    bool si = (ME == 1) && cbl.peer[0].present && (cbl.peer[0].flags & ST_SO);
    write_ioreg(REG_SIOCNT, (siocnt & ~0x0004) | (si ? 0x0004 : 0));
  }
}

// Console reset: any ongoing transfer is lost, the cable stays plugged.
void serialcable_reset(void) {
  cbl.m_busy = 0;
  cbl.q_head = cbl.q_count = 0;
  cbl.s_inxfer = 0;
  cbl.s_gap = 0;
  cbl.s_settle = 0;
  cbl.s_wrote = 0;
  cbl.s_sent = 0;
  cbl.status_sent = ST_UNKNOWN;
  cbl.link_dirty = 1;
}

// Netplay session started/finished: (un)plug the cable.
void serialcable_session_reset(void) {
  memset(&cbl, 0, sizeof(cbl));
  serialcable_reset();
}

void serialcable_peer_disconnected(uint16_t client_id) {
  if (client_id <= 3) {
    memset(&cbl.peer[client_id], 0, sizeof(cbl.peer[client_id]));
    cbl.link_dirty = 1;
    if (!client_id)
      cbl.s_live = cbl.s_pframe_valid = 0;
  }
}

// Called after SIOCNT/RCNT are written, with the registers already updated.
void serialcable_mode_changed(void) {
  cable_sync_status();
}

// The game loaded a word to send in multiplayer mode (SIOMLT_SEND).
void serialcable_send_written(void) {
  if (ME) {
    cbl.s_wrote = 1;
    cbl.s_settle = CBL_SETTLE_CYCLES;
  }
}

// A transfer starts: children are told our clock along with it.
static void parent_xfer_started(void) {
  cbl.m_active = CBL_ACTIVE_CYCLES;
  cbl.m_tick = 0;
}

// The game set the start bit in normal mode. Returns the new SIOCNT value.
u16 serialcable_normal_write(u16 oldval, u16 newval) {
  // SI is not writable, it follows the other console.
  newval = (newval & ~0x0004) | (oldval & 0x0004);

  // Only the console with the internal clock can start a transfer.
  if ((newval & 0x0080) && (newval & 0x0001) && !cbl.m_busy && !ME) {
    cbl.m_busy = 1;
    cbl.m_mode = XMODE_NORMAL;
    cbl.m_bits32 = (newval & 0x1000) ? 1 : 0;
    cbl.m_mask = cbl.peer[1].present ? 0x2 : 0;
    cbl.m_cycles = (newval & 0x2) ? NORMAL_CYC_2MHZ_8BIT : NORMAL_CYC_256KHZ_8BIT;
    if (cbl.m_bits32)
      cbl.m_cycles <<= 2;
    cbl.m_send = cbl.m_bits32 ? read_ioreg32(REG_SIODATA32_L) :
                                (read_ioreg(REG_SIODATA8) & 0xFF);
    cbl.m_seq++;

    if (cbl.m_mask) {
      cable_send(1, PKT_XFER, cbl.m_seq, cbl.m_send,
                 XMODE_NORMAL | (cbl.m_bits32 << 16), cbl.clock);
      parent_xfer_started();
    }
  }
  return newval;
}

// The game wrote SIOCNT in multiplayer mode. Returns the new SIOCNT value.
u16 serialcable_multi_write(u16 oldval, u16 newval) {
  newval = (newval & 0xFF83) | serialcable_mul_siocnt();

  if (ME) {
    // For children the start bit is just the busy flag.
    newval = (newval & ~0x0080) | (cbl.s_inxfer ? 0x0080 : 0);
  }
  else if ((newval & 0x0080) && !cbl.m_busy) {
    u32 i, count = 0;
    cbl.m_mask = 0;
    for (i = 1; i <= 3; i++) {
      if (cbl.peer[i].present) {
        cbl.m_mask |= 1 << i;
        count++;
      }
    }

    cbl.m_busy = 1;
    cbl.m_mode = XMODE_MULTI;
    cbl.m_cycles = multi_cycles[newval & 0x3] * (count + 1);
    cbl.m_send = read_ioreg(REG_SIOMLT_SEND);
    cbl.m_seq++;

    if (cbl.m_mask) {
      cable_send(RETRO_NETPACKET_BROADCAST, PKT_XFER, cbl.m_seq, cbl.m_send,
                 XMODE_MULTI | ((newval & 0x3) << 8) | (count << 24), cbl.clock);
      parent_xfer_started();
    }
  }
  return newval;
}

// Whether a console sent its word for the ongoing transfer.
static bool parent_has_reply(u32 i) {
  return cbl.peer[i].reply[cbl.m_seq & 1].valid &&
         cbl.peer[i].reply[cbl.m_seq & 1].seq == cbl.m_seq &&
         cbl.peer[i].reply[cbl.m_seq & 1].mode == cbl.m_mode;
}

// Consoles we are still expecting a word from.
static u32 parent_missing(void) {
  u32 i, missing = 0;
  for (i = 1; i <= 3; i++)
    if ((cbl.m_mask & (1 << i)) && cbl.peer[i].present &&
        cbl.peer[i].responsive && !parent_has_reply(i))
      missing |= 1 << i;
  return missing;
}

// The transfer time is over, collect the words and finish it.
static bool parent_complete(void) {
  u32 i, missing, polls = 0;
  u16 siocnt;
  u64 t0 = 0;

  // The answers are usually here already. If not, hold the console until
  // they arrive: that is what keeps the transfer consistent on both ends.
  // (Receiving packets re-enters serialcable_net_receive)
  while ((missing = parent_missing())) {
    if (!polls)
      t0 = netpacket_time_usec();
    else if (netpacket_time_usec() - t0 > CBL_PARENT_TIMEOUT_US ||
             polls > CBL_MAX_WAIT_POLLS) {
      // Do not stall again until they show signs of life.
      for (i = 1; i <= 3; i++)
        if (missing & (1 << i))
          cbl.peer[i].responsive = 0;
      CBL_DEBUG_LOG("Transfer %u timed out (missing mask %x)\n", cbl.m_seq, missing);
      break;
    }
    polls++;
    netpacket_poll_receive();
  }

  cbl.m_busy = 0;
  siocnt = read_ioreg(REG_SIOCNT) & ~0x0080;

  if (cbl.m_mode == XMODE_MULTI) {
    u16 w[4];
    w[0] = cbl.m_send;
    for (i = 1; i <= 3; i++)
      w[i] = ((cbl.m_mask & (1 << i)) && parent_has_reply(i)) ?
             cbl.peer[i].reply[cbl.m_seq & 1].data : 0xFFFF;
    for (i = 0; i <= 3; i++)
      write_ioreg(REG_SIOMULTI0 + i, w[i]);

    // A lone child already knows both words.
    if (cbl.m_mask & (cbl.m_mask - 1)) {
      cable_send(RETRO_NETPACKET_BROADCAST, PKT_DONE, cbl.m_seq,
                 w[0] | (w[1] << 16), w[2] | (w[3] << 16), cbl.clock);
      cbl.m_tick = 0;
    }
  }
  else {
    u32 data = ((cbl.m_mask & 0x2) && parent_has_reply(1)) ?
               cbl.peer[1].reply[cbl.m_seq & 1].data : 0xFFFFFFFF;
    if (cbl.m_bits32) {
      write_ioreg(REG_SIODATA32_L, data & 0xFFFF);
      write_ioreg(REG_SIODATA32_H, data >> 16);
    }
    else
      write_ioreg(REG_SIODATA8, data & 0xFF);
  }

  write_ioreg(REG_SIOCNT, siocnt);
  return (siocnt & 0x4000) != 0;
}

// Keep the children posted about our clock while the link is in use.
static void parent_tick(unsigned cycles) {
  if (!cbl.m_active)
    return;

  cbl.m_tick += cycles;
  if (cbl.m_active <= cycles) {
    // Nobody is using the link anymore: let the children run on their own.
    cbl.m_active = 0;
    cable_send(RETRO_NETPACKET_BROADCAST, PKT_TICK, 0, 0, 0, cbl.clock);
  }
  else {
    cbl.m_active -= cycles;
    if (cbl.m_tick >= CBL_TICK_CYCLES) {
      cbl.m_tick = 0;
      cable_send(RETRO_NETPACKET_BROADCAST, PKT_TICK, 0, TICK_ACTIVE, 0, cbl.clock);
    }
  }
}

// Block (for a short while) until the parent sends something.
static bool child_wait(u32 timeout_us) {
  u32 polls = 0;
  const u32 pclock = cbl.s_pclock;
  const u64 t0 = netpacket_time_usec();
  while (!cbl.q_count && cbl.s_live && cbl.s_pclock == pclock) {
    if (netpacket_time_usec() - t0 > timeout_us || ++polls > CBL_MAX_WAIT_POLLS)
      return false;
    netpacket_poll_receive();
  }
  return true;
}

// Whether our word for the given transfer is already on its way.
static bool child_word_sent(u32 seq) {
  return cbl.s_sent && cbl.s_sent_seq == seq;
}

// Sample the word the game wants to send and hand it to the parent.
static void child_send_word(u32 seq) {
  const u16 word = in_multi_mode() ? read_ioreg(REG_SIOMLT_SEND) : 0xFFFF;

  cbl.s_wrote = 0;
  if (child_word_sent(seq) && cbl.s_sent_word == word)
    return;

  cbl.s_sent = 1;
  cbl.s_sent_seq = seq;
  cbl.s_sent_word = word;
  cable_send(0, PKT_REPLY, seq, word, XMODE_MULTI, 0);
}

// A multiplayer transfer is over: here are all the words.
static bool child_finish(u16 w0, u16 w1, u16 w2, u16 w3) {
  u16 siocnt = read_ioreg(REG_SIOCNT) & ~0x0080;
  cbl.s_inxfer = 0;

  // On the wire our word is not sampled until one word time into the next
  // transfer: give the game that same time to load its next word.
  cbl.s_gap = cbl.s_wordcyc;

  if (!in_multi_mode())
    return false;

  write_ioreg(REG_SIOMULTI0, w0);
  write_ioreg(REG_SIOMULTI1, w1);
  write_ioreg(REG_SIOMULTI2, w2);
  write_ioreg(REG_SIOMULTI3, w3);
  write_ioreg(REG_SIOCNT, siocnt);

  return (siocnt & 0x4000) != 0;
}

// The parent started a transfer.
static bool child_xfer(const cable_event *ev) {
  const u32 mode = ev->b & 0xFF;
  u16 siocnt = read_ioreg(REG_SIOCNT);

  cbl.s_seq = ev->seq;

  if (mode == XMODE_MULTI) {
    const u32 children = (ev->b >> 24) & 0x3;
    const s32 late = (s32)(cbl.s_time - ev->c);

    cbl.s_wordcyc = multi_cycles[(ev->b >> 8) & 0x3];
    cbl.s_xferleft = cbl.s_wordcyc * (children + 1);
    // If we are ahead of the parent the transfer started a while ago for
    // us. Make up for it, or we would run faster than the parent does.
    if (cbl.s_live && !cbl.s_resync && late > 0)
      cbl.s_xferleft -= MIN((u32)late, cbl.s_xferleft - cbl.s_wordcyc);
    cbl.s_inxfer = 1;
    cbl.s_local = (children == 1);
    cbl.s_pword = ev->a;

    // Our word (unless it was sent ahead) follows once the game had its
    // time to load it, see child_update.
    if (in_multi_mode())
      write_ioreg(REG_SIOCNT, siocnt | 0x0080);     // Busy
  }
  else if (ME == 1) {
    const bool bits32 = (ev->b >> 16) & 1;
    // We must be waiting for an external clock, with the same word size.
    const bool ready = in_normal_mode() && (siocnt & 0x0081) == 0x0080 &&
                       ((siocnt & 0x1000) != 0) == bits32;
    u32 data = 0xFFFFFFFF;

    if (ready) {
      if (bits32) {
        data = read_ioreg32(REG_SIODATA32_L);
        write_ioreg(REG_SIODATA32_L, ev->a & 0xFFFF);
        write_ioreg(REG_SIODATA32_H, ev->a >> 16);
      } else {
        data = read_ioreg(REG_SIODATA8) & 0xFF;
        write_ioreg(REG_SIODATA8, ev->a & 0xFF);
      }
      siocnt &= ~0x0080;
      write_ioreg(REG_SIOCNT, siocnt);
    }
    cable_send(0, PKT_REPLY, ev->seq, data, XMODE_NORMAL, 0);

    return ready && (siocnt & 0x4000);
  }
  return false;
}

// Whether the game has something to do before it can be held.
static bool child_busy(void) {
  if (cbl.q_count || cbl.s_gap)
    return true;
  if (cbl.s_inxfer)
    return cbl.s_local || !child_word_sent(cbl.s_seq);
  return cbl.s_wrote;
}

static bool child_update(unsigned cycles) {
  bool irq = false;

  cbl.s_time += cycles;
  cbl.s_gap -= MIN(cbl.s_gap, cycles);
  cbl.s_settle -= MIN(cbl.s_settle, cycles);

  cbl.poll_cycles += cycles;
  if (cbl.poll_cycles >= CBL_POLL_CYCLES) {
    cbl.poll_cycles = 0;
    netpacket_poll_receive();
  }

  // With a single child both words are known, the transfer ends on time.
  if (cbl.s_inxfer && cbl.s_local) {
    if (cbl.s_xferleft > cycles)
      cbl.s_xferleft -= cycles;
    else {
      if (!child_word_sent(cbl.s_seq))
        child_send_word(cbl.s_seq);
      irq = child_finish(cbl.s_pword, cbl.s_sent_word, 0xFFFF, 0xFFFF);
    }
  }

  if (!cbl.s_gap && !cbl.s_settle) {
    if (cbl.s_inxfer) {
      // The parent is waiting for our word.
      if (!child_word_sent(cbl.s_seq))
        child_send_word(cbl.s_seq);
    }
    else if (cbl.s_wrote && cbl.s_live && in_multi_mode())
      child_send_word(cbl.s_seq + 1);   // Next word is ready, send it ahead.
    else
      cbl.s_wrote = 0;
  }

  // Do not run ahead of the parent: wait here, where we can still answer.
  while (cbl.s_live && !irq && !child_busy()) {
    if (cbl.s_resync) {
      cbl.s_resync = 0;
      cbl.s_time = cbl.s_pclock;
    }
    if ((s32)(cbl.s_pclock - cbl.s_time) > CBL_MAX_LAG_CYCLES)
      cbl.s_time = cbl.s_pclock - CBL_MAX_LAG_CYCLES;
    if ((s32)(cbl.s_time - cbl.s_pclock) <= CBL_SLACK_CYCLES)
      break;

    if (!child_wait(CBL_CHILD_TIMEOUT_US)) {
      // Parent is not running (paused?), carry on until it shows up again.
      CBL_DEBUG_LOG("Parent clock timed out\n");
      cbl.s_live = cbl.s_pframe_valid = 0;
    }
  }

  while (cbl.q_count && !irq) {
    const cable_event ev = cbl.queue[cbl.q_head];
    if (ev.type == PKT_XFER && cbl.s_inxfer && cbl.s_local)
      break;      // Previous transfer is not over yet, keep it for later.

    cbl.q_head = (cbl.q_head + 1) % CBL_QUEUE_SIZE;
    cbl.q_count--;

    if (ev.type == PKT_XFER)
      irq = child_xfer(&ev);
    else if (cbl.s_inxfer && !cbl.s_local && ev.seq == cbl.s_seq)
      irq = child_finish(ev.a & 0xFFFF, ev.a >> 16, ev.b & 0xFFFF, ev.b >> 16);
  }

  return irq;
}

// A frame was emulated, we are about to return to the frontend.
void serialcable_frame_end(void) {
  s32 ahead;

  if (!netpacket_active())
    return;

  if (!ME) {
    // Children must know we will not be around for a while.
    if (cbl.m_active) {
      cbl.m_tick = 0;
      cable_send(RETRO_NETPACKET_BROADCAST, PKT_TICK, 0,
                 TICK_ACTIVE | TICK_FRAME_END, 0, cbl.clock);
    }
    return;
  }

  if (!cbl.s_live || cbl.s_resync || !cbl.s_pframe_valid)
    return;

  // Make our frames end right after the parent's ones: we both go to sleep
  // at the same time, instead of each one being stuck on the other's nap.
  ahead = (s32)(cbl.s_time - cbl.s_pframe) % CBL_FRAME_CYCLES;
  if (ahead < 0)
    ahead += CBL_FRAME_CYCLES;
  ahead -= CBL_SLACK_CYCLES / 2;
  if (ahead > CBL_FRAME_CYCLES / 2)
    ahead -= CBL_FRAME_CYCLES;
  cbl.s_time -= ahead;
}

// How many cycles until the next cable event happens.
u32 serialcable_next_event(void) {
  u32 next = ~0U;
  if (!ME)
    return cbl.m_busy ? cbl.m_cycles : ~0U;

  if (cbl.s_inxfer && cbl.s_local)
    next = cbl.s_xferleft;
  if (cbl.s_gap)
    next = MIN(next, cbl.s_gap);
  if (cbl.s_settle && cbl.s_wrote)
    next = MIN(next, cbl.s_settle);
  return next;
}

// Account for consumed cycles and return if a serial IRQ should be raised.
bool serialcable_update(unsigned cycles) {
  cbl.clock += cycles;

  if (cbl.link_dirty) {
    cbl.link_dirty = 0;
    cable_sync_status();
    cable_refresh_lines();
  }

  if (!netpacket_active())
    cbl.m_active = 0;

  if (ME)
    return netpacket_active() ? child_update(cycles) : false;

  if (cbl.m_busy) {
    if (cbl.m_cycles > cycles)
      cbl.m_cycles -= cycles;
    else {
      const bool irq = parent_complete();
      parent_tick(cycles);
      return irq;
    }
  }
  parent_tick(cycles);
  return false;
}

// Note: this can be called from within our own network polls. It must only
// record what happened, registers are updated from serialcable_update.
void serialcable_net_receive(const void* buf, size_t len, uint16_t client_id) {
  const u32 *pkt = (u32*)buf;
  u32 type, seq, a, b, c;

  if (len != 24 || client_id > 3 || client_id == ME ||
      netorder32(pkt[0]) != NET_CABLE_HEADER)
    return;

  type = netorder32(pkt[1]);
  seq  = netorder32(pkt[2]);
  a    = netorder32(pkt[3]);
  b    = netorder32(pkt[4]);
  c    = netorder32(pkt[5]);

  switch (type) {
  case PKT_STATUS:
    if (!cbl.peer[client_id].present) {
      // New console on the cable, make sure it learns about us too.
      cbl.peer[client_id].present = 1;
      cbl.peer[client_id].responsive = 1;
      cbl.status_sent = ST_UNKNOWN;
    }
    cbl.peer[client_id].flags = a;
    cbl.link_dirty = 1;
    break;

  case PKT_REPLY:
    if (ME)
      break;
    cbl.peer[client_id].responsive = 1;
    // Either the word for the ongoing transfer or (ahead) for the next one.
    if ((cbl.m_busy && seq == cbl.m_seq) || seq == cbl.m_seq + 1) {
      cbl.peer[client_id].reply[seq & 1].data = a;
      cbl.peer[client_id].reply[seq & 1].seq = seq;
      cbl.peer[client_id].reply[seq & 1].mode = b;
      cbl.peer[client_id].reply[seq & 1].valid = 1;
    }
    break;

  case PKT_TICK:
  case PKT_XFER:
  case PKT_DONE:
    if (!ME || client_id != 0)
      break;

    if (type == PKT_TICK && !(a & TICK_ACTIVE)) {
      cbl.s_live = 0;     // Link is idle, nothing to keep up with.
      cbl.s_pframe_valid = 0;
      break;
    }
    if (!cbl.s_live) {
      cbl.s_live = 1;
      cbl.s_resync = 1;
      cbl.s_pclock = c;
    }
    else if ((s32)(c - cbl.s_pclock) > 0)
      cbl.s_pclock = c;

    if (type == PKT_TICK) {
      if (a & TICK_FRAME_END) {
        cbl.s_pframe = c;
        cbl.s_pframe_valid = 1;
      }
      break;
    }

    if (cbl.q_count >= CBL_QUEUE_SIZE) {
      // We fell way behind (ie. we were paused): whatever is queued is
      // stale by now, drop it and resync with the next transfer.
      CBL_DEBUG_LOG("Cable queue overflow, dropping stale transfers\n");
      cbl.q_head = cbl.q_count = 0;
      cbl.s_inxfer = 0;
      cbl.s_sent = 0;
      cbl.link_dirty = 1;
      if (type == PKT_DONE)
        break;
    }
    {
      cable_event *ev = &cbl.queue[(cbl.q_head + cbl.q_count) % CBL_QUEUE_SIZE];
      ev->type = type;
      ev->seq = seq;
      ev->a = a;
      ev->b = b;
      ev->c = c;
      cbl.q_count++;
    }
    break;
  };
}
