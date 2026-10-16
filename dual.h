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

// Dual Cable Link: both GBAs of a link session run on every console, joined
// by a cable in memory, and only the buttons travel over the network.
//
// The core is built twice into the same library. Instance A is the core the
// frontend talks to; instance B is the very same code with every global
// renamed (gbab_*), so it is a second, independent GBA. Each instance has its
// own end of the cable (serial_dual.c). The cable itself (the "bus") and the
// session logic live once, in A (dual.c).
//
// Slot 0 is the parent end of the cable (the netplay host's GBA), slot 1 the
// child (the client's GBA). On the host A is slot 0 and B slot 1; on the
// client it is the other way around. Both consoles compute the same two
// machines, so everything below must depend on emulated time only, never on
// how fast the two threads happen to run.

#ifndef DUAL_H
#define DUAL_H

#include <stdint.h>

// Events from one end of the cable to the other.
#define DEV_WIRES       1   // v0: the sender's mode and pin levels
#define DEV_MP_START    2   // multiplayer transfer started (parent)
#define DEV_N_START     3   // normal mode transfer started (internal clock)

typedef struct {
  uint64_t t;               // When it happened (sender's session cycles)
  uint64_t t_done;          // *_START: when the transfer ends
  uint32_t v0;              // Data word
  uint32_t seq;             // *_START: transfer number
  uint8_t kind;
  uint8_t a;                // N_START: 8 or 32 bits
} dual_event;

// How far (cycles) the parent may run ahead of the child. The child never
// runs ahead of the parent, so whatever the parent does reaches the child on
// time; what the child does (other than answering a transfer) reaches the
// parent this much later. Half a frame, as GBAdhoc settled on.
#define DUAL_LOOKAHEAD   140448

// What a GBA that finished its frame promises the other one (cycles).
#define DUAL_FRAME_SLACK 1232

// The bus (dual.c, instance A only). The instances call these.
void dualbus_sync(unsigned slot, uint64_t publish, uint64_t clock);
void dualbus_post(unsigned from_slot, const dual_event *ev);
int dualbus_pop(unsigned slot, uint64_t now, dual_event *ev);
void dualbus_reply(unsigned from_slot, uint32_t seq, uint32_t word, int active);
int dualbus_wait_reply(unsigned slot, uint32_t seq, uint32_t *word);

// The session (dual.c, instance A only), called by libretro.c.
#ifdef __LIBRETRO__
#include <stdbool.h>
#include <stddef.h>
#include "libretro.h"
void dual_set_frontend(retro_environment_t cb,
                       void (*message)(const char *text, unsigned ms));
void dual_game_loaded(const char *path, uint32_t cfg);
void dual_session_start(unsigned client_id);
bool dual_peer_connected(unsigned client_id);
void dual_peer_disconnected(unsigned client_id);
void dual_session_stop(void);
bool dual_net_receive(const void *buf, size_t len, uint16_t client_id);
bool dual_setting_up(void);
bool dual_running(void);
bool dual_frame_begin(uint32_t local_keys);
void dual_b_frame_start(void);
void dual_b_frame_wait(void);
void dual_frame_end(void);
void dual_shutdown(void);
#endif

// One end of the cable (serial_dual.c, in both instances).
void dualport_begin(unsigned slot);
void dualport_end(void);
void dualport_sync(unsigned cycles);
void dualport_frame_end(void);
uint32_t dualport_next_event(void);
int dualport_update(void);
void dualport_set_keys(uint32_t keys);
uint32_t dualport_get_keys(void);
uint64_t dualport_clock(void);

#endif
