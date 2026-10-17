# Dual Cable Link

In gpSP the "Cable Link" mode runs **every GBA of the link on every
console**: the player's own GBA, and hidden copies of the other players'.
The GBAs of a console are joined by a link cable in memory, and only the
buttons travel over the network. A game's link traffic never waits for the
WiFi, however many transfers it makes per frame (Zelda: Four Swords, Street
Fighter, ...). The price is a few frames of input delay (2 on a local
network with 2 players, 3 with 3 or 4), chosen automatically from the round
trip times, and the CPU of the other GBAs (they run on the other CPU core).

The **Cable Link players** core option (2, 3 or 4, 2 by default) says how
many GBAs are on the cable; every console must have the same value. Only
that many GBAs are loaded: with 2, each console runs 2. The hidden GBAs read
the ROM from the player's one (no extra copy of it in memory, except the
first megabyte).

Every other mode, and Cable Link when the consoles cannot use the dual
link, works exactly as before.

## How a session goes

Every console selects **Cable Link** (or **Automatic**, which picks Cable
Link for every game without a Wireless Adapter, Pokemon or Advance Wars mode
of its own) and the same number of players, and starts a netplay session
(host and clients) as usual.

1. Each client compares ROM, BIOS, core version, number of players and the
   settings that change the machine with the host. If anything differs, the
   real time cable is used instead, for everybody, with a message (and each
   client restarts its game then, as games only look for the cable at boot).
   (Which game it is, and the other GBAs, are worked out when the session
   starts: the host does it while it waits for the clients.)
2. Until all the players are there the games go on, and every console shows
   "waiting for players (2/4)" and so on each time one comes.
3. With everybody there, the host measures the round trip to each client
   (input delay = one way + 1.6 frames, rounded up, at least 2; with 3 or 4
   players a frame more, as the buttons between two clients go through the
   host).
4. Each console freezes its GBA between two frames and sends it (savestate
   and save memory, 544 KB, run length coded: after the restart it is mostly
   empty) to every other one. Every game restarts first, once; each player
   keeps their own save. The host is player 1, the clients follow in the
   order they came.
5. From then on only the buttons are sent. Every 2 seconds the consoles
   compare all the GBAs; if they ever differ the link is cut with a message.
6. Every console shows the same frame at the same time (screen and sound go
   together): with the buttons each one says how far ahead it is, and one
   that is ahead holds a frame (a frame of silence) when it gets more than
   5/8 of a frame ahead, at most once a second. Each client keeps with the
   host and the host with each client. Consoles never run at exactly the
   same speed, so this happens now and then.
7. If the WiFi has a bad moment and a console has to wait for another's
   buttons, it says so, and that one sends its buttons a frame earlier from
   then on (up to 4 frames more). After a calm minute it takes a frame off
   again (never below the delay of step 3); if that was too soon, it waits
   twice as long the next time (up to 10 minutes).

A reset while linked goes with the buttons: every console restarts every
game in the same frame, the cable still plugged in.

While linked, savestates cannot be loaded, cheats are off and fast forward
is disabled: all of them would make the copies of a GBA different. When a
console leaves, each player's GBA carries on alone.

## Code

| file | what |
|---|---|
| `dual.h` | the design notes and the interfaces |
| `serial_dual.c` | one end of the cable (SIOCNT/RCNT/SIO data, timing; multiplayer for 2-4, normal mode between players 1 and 2); in every instance |
| `dual.c` | the cable between the ends, the hidden instances, network protocol, input lockstep; instance A only |
| `Makefile` | builds the hidden instances: the core again with `-DGPSP_INSTANCE_B`, partially linked, copied with every global renamed `gbab_*`, `gbac_*`, `gbad_*` |

Hooks in the core, all inactive unless `serial_mode == SERIAL_MODE_DUAL_CABLE`:
`update_gba` (time sync, end of frame), `serial.c` (register writes,
scheduling), `input.c` (buttons), `cheats.c`, the RTC (read in UTC from the
savestate clock, so that every console agrees), the ROM loading of the hidden
GBAs (`gba_memory.c`), and `libretro.c`.

## Build

    make platform=miyoomini

gives `gpsp_plus_libretro.so` (no LTO here: every GBA must be the very same
machine code).

## Test without consoles

`tools/dualtest` runs 2 to 4 consoles with RetroArch in one process, joined
by a network in memory (the clients only talk to the host, which passes
their packets on, as RetroArch does), on the real arm32 build under
`qemu-arm`:

    make -C tools/dualtest
    tools/dualtest/run.sh $PWD/tools/dualtest/linktest.gba 1200 5 0
    PLAYERS=4 tools/dualtest/run.sh /path/to/game.gba 2400 5 600

Arguments: ROM, frames, one-way latency (ms), frames each player plays alone
first. `PLAYERS=n` sets the consoles and their players option (`PLAYERS1=n`
gives console 1 another value), `JOIN_GAP=n` the frames between two clients
joining, `LATE_CONNECT=n` the frames the host hears about each client late.

`PACE=1` runs each console on its own thread at 59.73 frames a second and
prints how long the setup took, how far apart the screens are and how many
frames were repeated (held by the time sync, or waits); with it
`SKEW_PPM=n` makes client k's clock k times n ppm faster, `BW_KBPS=n` limits
the network bandwidth, `JITTER_MS=n` adds a random delay to each packet and
`SPIKE_MS=n SPIKE_EVERY=k` delays one packet in k by n ms (the ones behind
it wait).

`KEYS0`..`KEYS3` script each player's buttons and `SHOTS=dir` with
`SHOT_AT`/`SHOT_EVERY` saves screenshots, to drive a game into its
multiplayer mode; `RESET_AT=n` presses reset on the host. `linktest.gba`
(built from `linktest.c`) makes tens of multiplayer transfers per frame and
logs the four words of each; the harness checks that every GBA logged the
same words, with a word from each player. `run.sh debug ...` links a core
whose cable prints what it does; `DUAL_FAULT=1` with it gives a hidden GBA
other buttons once, to check that the agreement check catches it.
`SERIAL=auto` (or any other value of the option) changes the link mode,
`ROM2=...` gives client 1 another game (the real time cable must take over).
