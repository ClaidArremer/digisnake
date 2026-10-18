# DIGISNAKE

A Cartesian step sequencer for the **Elektron Digitone mk1** (and Digitone Keys), after Maxime Dangles'
MDD Snake and Make Noise's René, as an [elekloader](https://github.com/irpina/elekloader) mod (id
`digisnake`). Eight cells sit in a 4 × 2 grid that matches knobs A–H; a path, the snake, walks the grid and
each sequencer step plays the cell it lands on. It runs on every track: the four synth tracks and the
four MIDI tracks, for external gear.

By Robbie (designed and tested on a Digitone mk1, OS 1.44). GPL-2.0-or-later.

> Unofficial and unsupported. Flashing modified firmware is at your own risk; the stock OS always
> recovers the unit (send the stock .syx from the boot menu).

## What you need

| | |
|---|---|
| Unit | Digitone mk1 or Digitone Keys |
| OS | **1.44** (tested). A 1.43 build is included, untested; on 1.43 there are no drop-down messages and settings are not kept over a power cycle |
| Mods | elekloader's **core-dn1** (3.3 or later) and **seq-events** (0.1), both shipped with elekloader |

## Install

Add `elemods/digisnake-1.0-os1.44.elemod` (or the 1.43 file) in the elekloader app together with core-dn1
and seq-events, build, and send the firmware to the Digitone. From the command line:

```
python -m elekloader.patch --stock Digitone_and_Digitone_Keys_OS1.44.syx \
  --mod core-4.0-os1.44.elemod --mod seq-events-0.1-os1.44.elemod \
  --mod digisnake-1.0-os1.44.elemod --out Digitone_digisnake.syx
```

## Opening it

Press **TRIG** twice: DIGISNAKE is the page after the stock TRIG page, on synth and MIDI tracks alike.
Turn the track's **SNAKE** on (PATH page, knob A) and it plays the snake in place of its own trigs; lock
trigs still play. Everything is per track, saved with the project, and on OS 1.44 kept over a power
cycle. New projects start with a little demo line.

## Pages

**LEFT / RIGHT** move between the four pages. On NOTES, GATES and VELOCITY, knobs A–H are the eight cells.

| Page | Knobs A–H | Also |
|---|---|---|
| NOTES | Each cell's note, −24 to +24 semitones from the track's NOTE, quantised to SCALE | UP / DOWN: all notes a semitone (FUNC: an octave). FUNC + knob: −24 / −12 / 0 / +12 / +24 |
| GATES | Each cell plays or rests | Trig keys 1–8 toggle a cell. UP / DOWN: rotate |
| VELOCITY | Each cell's velocity, 1–127 | UP / DOWN: all up or down. FUNC + knob: 1 / 32 / 64 / 96 / 127 |
| PATH | The settings below | |

| Knob | PATH setting | Values |
|---|---|---|
| A | SNAKE | On or off for this track |
| B | SHAPE | ROWS, SNAKE, COLS, ZIGZAG, DIAG, CORNER, HOP |
| C | DIR | FWD, BWD, F/B, B/F, RND |
| D | LEN | Path length, 1–8 cells |
| E | DIV | 1/8, 1/4, 1/2, 1X, 2X. At 1/2 the snake plays every other step (eighth notes at 16th steps), at 1/4 every fourth. 2X runs the track at twice its own speed and needs the pattern's SCALE set to per track; DIGISNAKE puts the track's speed back when 2X or SNAKE goes off |
| F | SCALE | CHR, MAJ, MIN, DOR, PHR, LYD, MIX, HMIN, PENT, mPNT, BLUE, WHOL, in the keyboard's ROOT |
| G | RANGE | The spread of randomised notes, ±0–24 semitones (FUNC + knob: 0 / 12 / 24) |
| H | SHIFT | Where on the path the snake starts, 0–7 |

## Keys

PAGE is the TRIG key, held. On OS 1.44 every action shows the stock drop-down message.

| Keys | What happens |
|---|---|
| PAGE + YES | Randomise the page shown (PATH: all but SNAKE and SCALE), as often as you like |
| PAGE + NO | Back to how the track was when PAGE went down, a print included. No redo |
| PAGE + PLAY | Clear the page: notes 0, gates on, velocity 100, PATH defaults (SNAKE kept) |
| PAGE + REC | Copy the page shown |
| PAGE + STOP | Paste it onto this track, synth or MIDI (PATH keeps this track's SNAKE) |
| PAGE + FUNC + REC | **Print**: write the snake into the track's own trigs, notes and velocities over the track's length, then turn SNAKE off |
| FUNC + YES | Randomise NOTES, GATES and VELOCITY together |
| FUNC + NO | Back to before the last run of FUNC + YES |
| FUNC + knob | Jump to preset values (NOTES, VELOCITY, RANGE) |

Print keeps lock trigs and parameter locks, and a 2X track stays at 2X. With DIR on RND it freezes one
random run.

## How it works

DIGISNAKE uses seq-events' step-fire events: `ev_gen_trig` decides whether a step fires, `ev_gen_notes`
its note and `ev_fire_vel` its velocity, so the stock sequencer plays everything (timing, note length,
locks). The page is a core-dn1 mod page after the TRIG page (page 28 for synth tracks, 29 for MIDI
tracks), the settings are core-dn1 project data (`SNK1`, `SNK2`), and on OS 1.44 an edit asks the OS to
refresh its power-up copy of the project. No firmware address is named in the C source except the
drop-down message and the power-up copy on 1.44 (`mod.json`'s 1.44 port).

## Building

With elekloader and an m68k cross GCC on the path:

```
python -m elekloader.sdk.build --stock Digitone_and_Digitone_Keys_OS1.44.syx .
```

## Credits and third-party code

DIGISNAKE is Robbie's own work, written with Claude (Anthropic); every line of `digisnake.c` is new. The
idea, a path walking a grid of steps, comes from **MDD Snake** (Maxime Dangles' free Max for Live device)
and **Make Noise René**; no code from either. The random numbers use Marsaglia's xorshift, a public
algorithm.

DIGISNAKE builds against irpina's [elekloader](https://github.com/irpina/elekloader) SDK headers and runs on
its core-dn1 and seq-events (GPL-2.0-or-later); none of them is included here. The firmware addresses in
`mod.json` and the pattern layout DIGISNAKE prints into were found by studying the Digitone's own OS so the
mod can work with it; no Elektron code is included.

## Trademarks

Elektron and Digitone are trademarks of Elektron Music Machines; Make Noise and René of Make Noise; Max for
Live of Ableton AG and Cycling '74. They are named only to say what this works with or was inspired by.
This project is not affiliated with or endorsed by any of them.

## Licence

GPL-2.0-or-later, see [LICENSE](LICENSE).
