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
// transfer the parent starts:
//
//   parent                                child(ren)
//     XFER(seq, parent word)  -------->   latch its own send word
//                             <--------   REPLY(seq, child word)
//     [transfer time elapses, waits for the replies if they are late]
//     DONE(seq, all 4 words)  -------->   fill SIOMULTIx, raise serial IRQ
//     raise serial IRQ
//
// Normal (8/32 bit) mode works the same way between parent and player 2,
// but needs no DONE message since only two words are swapped.
//
// To keep both consoles in step (and avoid a full frame of latency whenever
// one of them is idling in its frontend) each side briefly blocks on the
// network when the other one is known to owe it a message. All waits are
// bounded: if a peer stops answering (paused, menu, gone) we stop waiting
// for it and it simply reads as a console that is not transmitting.

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

#define NET_CABLE_HEADER       0x4d43424c   // MCBL (Multiplayer CaBLe)

#define PKT_STATUS             1    // a: status flags
#define PKT_XFER               2    // a: data, b: transfer config, c: parent clock
#define PKT_REPLY              3    // a: data
#define PKT_DONE               4    // a: words 0/1, b: words 2/3

// Status flags
#define ST_MULTI               0x1  // Console is in multiplayer mode
#define ST_SO                  0x2  // SO line level (normal mode)
#define ST_ALLREADY            0x4  // Parent only: every console is in multiplayer mode
#define ST_UNKNOWN             0xFF

// Transfer modes (XFER config, bits 0..7)
#define XMODE_MULTI            0
#define XMODE_NORMAL           1

#define CBL_QUEUE_SIZE         16
#define CBL_POLL_CYCLES        4096      // Children poll the network ~68 times per frame
#define CBL_MAX_PERIOD         300000    // Transfers further apart than a frame are not a stream

// Host-time limits for the blocking waits (microseconds).
#define CBL_PARENT_TIMEOUT_US  100000    // Waiting for the children words
#define CBL_CHILD_TIMEOUT_US   30000     // Waiting for the end of an ongoing transfer
#define CBL_STREAM_TIMEOUT_US  5000      // Waiting for the next transfer of a stream
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
    u8 replied;                  // Answered the ongoing transfer
    u32 reply;
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

  // Child
  cable_event queue[CBL_QUEUE_SIZE];
  u8 q_head, q_count;
  u8 s_inxfer;                   // Replied to a transfer, waiting for it to finish
  u8 s_live;                     // Parent is delivering its messages in time
  u8 s_seen;                     // At least one transfer was seen (clock is valid)
  u32 s_gap;                     // Time until the next transfer can be accepted
  u32 s_elapsed;                 // Time since the last transfer started
  u32 s_period;                  // Expected time between transfers (0: unknown)
  u32 s_xfercyc;                 // Duration of the ongoing transfer
  u32 s_wordcyc;                 // Duration of one word of the ongoing transfer
  u32 s_mclock;                  // Parent clock at its last transfer
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
  cbl.s_period = 0;
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
  }
}

// Called after SIOCNT/RCNT are written, with the registers already updated.
void serialcable_mode_changed(void) {
  cable_sync_status();
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
    cbl.peer[1].replied = 0;

    if (cbl.m_mask)
      cable_send(1, PKT_XFER, cbl.m_seq, cbl.m_send,
                 XMODE_NORMAL | (cbl.m_bits32 << 16), cbl.clock);
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
      cbl.peer[i].replied = 0;
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

    if (cbl.m_mask)
      cable_send(RETRO_NETPACKET_BROADCAST, PKT_XFER, cbl.m_seq, cbl.m_send,
                 XMODE_MULTI | ((newval & 0x3) << 8) | (count << 24), cbl.clock);
  }
  return newval;
}

// Consoles we are still expecting a word from.
static u32 parent_missing(void) {
  u32 i, missing = 0;
  for (i = 1; i <= 3; i++)
    if ((cbl.m_mask & (1 << i)) && cbl.peer[i].present &&
        cbl.peer[i].responsive && !cbl.peer[i].replied)
      missing |= 1 << i;
  return missing;
}

// The transfer time is over, collect the words and finish it.
static bool parent_complete(void) {
  u32 i, missing, polls = 0;
  u16 siocnt;
  const u64 t0 = netpacket_time_usec();

  // The answers are usually here already. If not, hold the console until
  // they arrive: that is what keeps the transfer consistent on both ends.
  // (Receiving packets re-enters serialcable_net_receive)
  while ((missing = parent_missing())) {
    if (netpacket_time_usec() - t0 > CBL_PARENT_TIMEOUT_US ||
        ++polls > CBL_MAX_WAIT_POLLS) {
      // Do not stall again until they show signs of life.
      for (i = 1; i <= 3; i++)
        if (missing & (1 << i))
          cbl.peer[i].responsive = 0;
      CBL_DEBUG_LOG("Transfer %u timed out (missing mask %x)\n", cbl.m_seq, missing);
      break;
    }
    netpacket_poll_receive();
  }

  cbl.m_busy = 0;
  siocnt = read_ioreg(REG_SIOCNT) & ~0x0080;

  if (cbl.m_mode == XMODE_MULTI) {
    u16 w[4];
    w[0] = cbl.m_send;
    for (i = 1; i <= 3; i++)
      w[i] = ((cbl.m_mask & (1 << i)) && cbl.peer[i].replied) ?
             cbl.peer[i].reply : 0xFFFF;
    for (i = 0; i <= 3; i++)
      write_ioreg(REG_SIOMULTI0 + i, w[i]);

    if (cbl.m_mask)
      cable_send(RETRO_NETPACKET_BROADCAST, PKT_DONE, cbl.m_seq,
                 w[0] | (w[1] << 16), w[2] | (w[3] << 16), 0);
  }
  else {
    u32 data = ((cbl.m_mask & 0x2) && cbl.peer[1].replied) ?
               cbl.peer[1].reply : 0xFFFFFFFF;
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

// Block (for a short while) until the parent sends something.
static bool child_wait(u32 timeout_us) {
  u32 polls = 0;
  const u64 t0 = netpacket_time_usec();
  while (!cbl.q_count) {
    if (netpacket_time_usec() - t0 > timeout_us || ++polls > CBL_MAX_WAIT_POLLS)
      return false;
    netpacket_poll_receive();
  }
  return true;
}

// The parent started a transfer: answer with our word.
static bool child_xfer(const cable_event *ev) {
  const u32 mode = ev->b & 0xFF;
  const u32 delta = ev->c - cbl.s_mclock;
  u16 siocnt = read_ioreg(REG_SIOCNT);

  // Learn the transfer cadence, to know when to expect the next one.
  cbl.s_period = (cbl.s_seen && delta < CBL_MAX_PERIOD) ? delta : 0;
  cbl.s_mclock = ev->c;
  cbl.s_seen = 1;
  cbl.s_elapsed = 0;

  if (mode == XMODE_MULTI) {
    u16 word = 0xFFFF;
    cbl.s_wordcyc = multi_cycles[(ev->b >> 8) & 0x3];
    cbl.s_xfercyc = cbl.s_wordcyc * (((ev->b >> 24) & 0x3) + 1);
    cbl.s_inxfer = 1;

    if (in_multi_mode()) {
      word = read_ioreg(REG_SIOMLT_SEND);
      write_ioreg(REG_SIOCNT, siocnt | 0x0080);     // Busy
    }
    cable_send(0, PKT_REPLY, ev->seq, word, 0, 0);
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
      // Leave the game some room before the next word comes in.
      cbl.s_gap = bits32 ? NORMAL_CYC_2MHZ_8BIT * 4 : NORMAL_CYC_2MHZ_8BIT;
    }
    cable_send(0, PKT_REPLY, ev->seq, data, 0, 0);

    return ready && (siocnt & 0x4000);
  }
  return false;
}

// The parent finished a multiplayer transfer: here are all the words.
static bool child_done(const cable_event *ev) {
  u16 siocnt = read_ioreg(REG_SIOCNT) & ~0x0080;
  cbl.s_inxfer = 0;

  if (!in_multi_mode())
    return false;

  write_ioreg(REG_SIOMULTI0, ev->a & 0xFFFF);
  write_ioreg(REG_SIOMULTI1, ev->a >> 16);
  write_ioreg(REG_SIOMULTI2, ev->b & 0xFFFF);
  write_ioreg(REG_SIOMULTI3, ev->b >> 16);
  write_ioreg(REG_SIOCNT, siocnt);

  // On the wire our word is not sampled until one word time into the next
  // transfer: give the game that same time to load its next word.
  cbl.s_gap = cbl.s_wordcyc;

  return (siocnt & 0x4000) != 0;
}

static bool child_update(unsigned cycles) {
  cbl.s_gap -= MIN(cbl.s_gap, cycles);
  if (cbl.s_elapsed < 0x40000000)
    cbl.s_elapsed += cycles;

  cbl.poll_cycles += cycles;
  if (cbl.poll_cycles >= CBL_POLL_CYCLES) {
    cbl.poll_cycles = 0;
    netpacket_poll_receive();
  }

  // Do not run ahead of the parent when we know it owes us a message.
  if (cbl.s_live && !cbl.q_count) {
    if (cbl.s_inxfer) {
      // The transfer should be over by now.
      if (cbl.s_elapsed >= cbl.s_xfercyc && !child_wait(CBL_CHILD_TIMEOUT_US))
        cbl.s_live = 0;
    }
    else if (cbl.s_period && cbl.s_elapsed >= cbl.s_period + cbl.s_period / 8) {
      // The next transfer of the stream is due. If it does not show up the
      // stream is over, do not wait again until a new one is going.
      if (!child_wait(CBL_STREAM_TIMEOUT_US))
        cbl.s_period = 0;
    }
  }

  while (cbl.q_count) {
    bool irq;
    const cable_event ev = cbl.queue[cbl.q_head];
    if (ev.type == PKT_XFER && cbl.s_gap)
      break;      // Too early for the game, keep it for later.

    cbl.q_head = (cbl.q_head + 1) % CBL_QUEUE_SIZE;
    cbl.q_count--;

    irq = (ev.type == PKT_XFER) ? child_xfer(&ev) : child_done(&ev);
    if (irq)
      return true;
  }

  return false;
}

// How many cycles until the next cable event happens.
u32 serialcable_next_event(void) {
  if (cbl.m_busy)
    return cbl.m_cycles;
  if (cbl.s_gap && cbl.q_count)
    return cbl.s_gap;
  return ~0U;
}

// Account for consumed cycles and return if a serial IRQ should be raised.
bool serialcable_update(unsigned cycles) {
  cbl.clock += cycles;

  if (cbl.link_dirty) {
    cbl.link_dirty = 0;
    cable_sync_status();
    cable_refresh_lines();
  }

  if (ME)
    return netpacket_active() ? child_update(cycles) : false;

  if (cbl.m_busy) {
    if (cbl.m_cycles > cycles)
      cbl.m_cycles -= cycles;
    else
      return parent_complete();
  }
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
    if (cbl.m_busy && seq == cbl.m_seq) {
      cbl.peer[client_id].reply = a;
      cbl.peer[client_id].replied = 1;
    }
    break;

  case PKT_XFER:
  case PKT_DONE:
    if (!ME || client_id != 0)
      break;
    if (cbl.q_count >= CBL_QUEUE_SIZE) {
      // We fell way behind (ie. we were paused): whatever is queued is
      // stale by now, drop it and resync with the next transfer.
      CBL_DEBUG_LOG("Cable queue overflow, dropping stale transfers\n");
      cbl.q_head = cbl.q_count = 0;
      cbl.s_inxfer = 0;
      cbl.s_period = 0;
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
      cbl.s_live = 1;
    }
    break;
  };
}
