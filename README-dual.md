# Dual Cable Link

In gpSP the "Cable Link" mode runs **both GBAs on
every console**: the player's own GBA, and a hidden copy of the other
player's. The two GBAs of a console are joined by a link cable in memory, and
only the buttons travel over the network. A game's link traffic never waits
for the WiFi, however many transfers it makes per frame (Zelda: Four Swords,
Street Fighter, ...). The price is a few frames of input delay (2 on a
local network), chosen automatically from the round trip time, and the CPU
of a second GBA (it runs on the other CPU core).

Every other mode, and Cable Link when the two consoles cannot use the dual
link, works exactly as before.

## How a session goes

Both consoles select **Cable Link** (or **Automatic**, which picks Cable Link
for every game without a Wireless Adapter, Pokemon or Advance Wars mode of
its own) and start a netplay session (host and client) as usual.

1. The consoles compare ROM, BIOS, core version and the settings that change
   the machine. If anything differs, the real time cable is used instead,
   exactly as in the main core.
   (Which game it is, and the second GBA, are worked out when the session
   starts: the host does it while it waits for the client.)
2. They measure the round trip time (input delay = one way + 1.6 frames,
   rounded up, at least 2).
3. Each console freezes its GBA between two frames and sends it (savestate
   and save memory, 544 KB, run length coded: after the restart it is mostly
   empty) to the other one. Both games restart first, as games only look for
   the cable at boot; each player keeps their own save.
4. From then on only the buttons are sent. Every 2 seconds both consoles
   compare both GBAs; if they ever differ the link is cut with a message.
5. Both consoles show the same frame at the same time (screen and sound go
   together): with the buttons each one says how far ahead it is, and the
   one ahead holds a frame (a frame of silence) when it gets more than 5/8
   of a frame ahead, at most once a second. Two consoles never run at
   exactly the same speed, so this happens now and then.
6. If the WiFi has a bad moment and a console has to wait for the other's
   buttons, it says so, and the other one sends its buttons a frame earlier
   from then on (up to 4 frames more). After a calm minute it takes a frame
   off again (never below the delay of step 2); if that was too soon, it
   waits twice as long the next time (up to 10 minutes).

A reset while linked goes with the buttons: both consoles restart both games
in the same frame, the cable still plugged in.

While linked, savestates cannot be loaded, cheats are off
and fast forward is disabled: all of them would make the two copies of a GBA
different. When the other console leaves, the player's GBA carries on alone.

## Code

| file | what |
|---|---|
| `dual.h` | the design notes and the interfaces |
| `serial_dual.c` | one end of the cable (SIOCNT/RCNT/SIO data, timing); in both instances |
| `dual.c` | the cable between the two ends, instance B, network protocol, input lockstep; instance A only |
| `Makefile` | builds instance B: the core again with `-DGPSP_INSTANCE_B`, partially linked, every global renamed `gbab_*` |

Hooks in the core, all inactive unless `serial_mode == SERIAL_MODE_DUAL_CABLE`:
`update_gba` (time sync, end of frame), `serial.c` (register writes,
scheduling), `input.c` (buttons), `cheats.c`, the RTC (read in UTC from the
savestate clock, so that both consoles agree), and `libretro.c`.

## Build

    make platform=miyoomini

gives `gpsp_plus_libretro.so` (no LTO here: both GBAs must be the very same
machine code).

## Test without consoles

`tools/dualtest` runs two consoles with RetroArch in one process, joined by a
network in memory, on the real arm32 build under `qemu-arm`:

    make -C tools/dualtest
    tools/dualtest/run.sh $PWD/tools/dualtest/linktest.gba 1200 5 0
    tools/dualtest/run.sh /path/to/game.gba 2400 5 600

Arguments: ROM, frames, one-way latency (ms), frames each player plays alone
first. `PACE=1` runs each console on its own thread at 59.73 frames a second
and prints how long the setup took and how far apart both screens are;
with it `SKEW_PPM=n` makes the client's clock faster, `BW_KBPS=n` limits the
network bandwidth, `JITTER_MS=n` adds a random delay to each packet and
`SPIKE_MS=n SPIKE_EVERY=k` delays one packet in k by n ms (the ones behind
it wait). `KEYS0`/`KEYS1` script each player's buttons and `SHOTS=dir` with
`SHOT_AT`/`SHOT_EVERY` saves screenshots, to drive a game into its
multiplayer mode; `RESET_AT=n` presses reset on the host. `linktest.gba` (built from
`linktest.c`) makes tens of multiplayer transfers per frame and the harness
checks that both GBAs logged the same words. `run.sh debug ...` links a core
whose cable prints what it does; `DUAL_FAULT=1` with it gives the hidden GBA
other buttons once, to check that the agreement check catches it. `SERIAL=auto` (or any other value of the option) changes the link mode, `ROM2=...`
gives the client another game (the real time cable must take over).
