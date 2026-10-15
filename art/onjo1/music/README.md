# `art/onjo1/music/`: the game's music

The masters of the music the base game (`assets/`) and its expansion `onjo1`
(`expansions/onjo1/`) stream: the mission start, the win and lose stings and
the menu beds. Every master is our own, composed from CC0 instrument samples
and synthesis. The original game's music is reference only: its banks were
decoded to a scratch folder outside the repo, measured and never committed.

- `SOURCES.md`: every master, what it is and how it was made; every source file
  with its author, page and licence.
- `sources/`: the CC0 samples as downloaded (Freesound's HQ OGG preview of each
  sound, the stream its page plays).
- `masters/`: the cues at 22050 Hz 16-bit stereo, the rate and channels of the
  game's music banks, ready to be encoded into them.

## Rules

- **CC0 1.0 only**, recorded per file in `SOURCES.md`.
- **Modelled on the original, never taken from it.** Its banks' formats,
  lengths, loudness and spectra are measured and its scripts read for how they
  play; its audio never enters the repo.
- **The banks' format.** A music bank (`.sbf`) holds byte-paired stereo at
  22050 Hz: each chunk of 4096 bytes is left and right samples of one byte each,
  scaled by a shift per chunk, so a sample reaches (127 x 128) / 32768, -6.0
  dBFS, at most (`engine/formats/sbf`, docs/audio/mus-sbf-re.md). Every
  retail stream peaks at -6.0 dBFS; every master here peaks at -6.1 or under,
  so the encoder never clips it.
- **Names fit the bank**: a stream's name is at most 16 bytes in the bank's
  index (the game plays a stream by its place and never reads its name);
  ours start `ON`.
- The sequencing and mixing scripts are throwaway; the sources plus the masters
  are the truth, and `SOURCES.md` says in words what each master is.

## Reference study (JO: Combined Arms)

The game streams one music context at a time: the menu pair
(`MENUMUS.SBF` with its script `MENUMUS.BIN`) from boot, the game pair
(`GAMEMUS.SBF`, `GAMEMUS.BIN`) from a mission's start in every session with a
local player. A script plays a stream by its place in the bank's index and
steps on only when the stream has finished, so a cue is a run of streams and a
section changes on a stream's end (docs/audio/mus-sbf-re.md).

### `gamemus.sbf` and its script

| # | Stream | Length | Peak, RMS | What it is |
|---|---|---|---|---|
| 0 | `NULLS` | 0.050 s | silent | one chunk of silence, 1108 frames |
| 1 | `GAMINT` | 8.43 s | -6.9, -25.9 dB | the mission-start sting |
| 2 to 7 | `JOWin1_1` to `_6` | 5.9, 6.3, 14.3, 19.5, 15.3, 20.4 s (81.9 s) | -6.0 to -7.9, -19.3 to -22.3 dB | the win cue in six parts |
| 8 to 12 | `JOLose1_1` to `_5` | 6.2, 5.0, 9.9, 8.3, 9.5 s (38.9 s) | -6.0, -19.6 to -22.1 dB | the lose cue in five parts |

The script starts in `Testmission`, which reads `Var01` (never written, so
always 0) and enters `Multiplayerstart`: `NULLS` twenty times (a second of
silence), `GAMINT` once, then the self-looping `Missionnull`, silence for the
rest of the mission. The single-player round end signals the script's
`MessageHandler` (1 a win, 2 a loss), which plays `JOWin1_1` to `_6` in order
or `JOLose1_1` to `_5`, then idles. The expansion's `GJox01` pair is the same
script and bank.

### `menumus.sbf` and its script

47 streams: `MENUERR` (5.51 s, a low drone, 97 % under 250 Hz, peak -11.9 dB,
RMS -23.1 dB), `JOMENU601` (1.07 s, an opening phrase), the theme's parts
`JOMEN602A` to `606D` (16 streams, 4.6 to 27.3 s), `JOMENUC610` and `JOMENU611`
to `623` (18.3 s each) and the credits suite `JOCREDS01` to `JOCREDSC8` (16
streams, 11.3 to 19.1 s). Every stream but `MENUERR` and `JOMENU601` peaks at
-6.0 to -7.9 dBFS at -17.4 to -23.8 dB RMS, loudest 50 ms -13.0 to -16.7 dB,
with 50 to 96 % of its energy under 250 Hz (drums and low strings).

A screen's `MUSICVAR` is written to the script's `Var02` on every screen event,
and the script tests it after every stream (`MenuParse`):

| `Var02` | Screens (JO:CA's `.mnu`) | Section | Streams |
|---|---|---|---|
| 0 | none written yet | `MenuError` | `MENUERR`, again while it stays 0 |
| 1 | `main.mnu` | `LoopSection` | `JOMENU601` once at boot, then the theme: 602A-C, 603A-C, 604 three times, then 605A-D, 606A-D, round again |
| 9 | `options.mnu` | `MenuOptions` | `JOCREDSC1`, then the credits suite and 610 to 623 |
| 10 | `player.mnu` | `MenuPlayer` | 604, 605A-D (three times), 606A-D |
| 11 | `sp.mnu` | `MenuSP` | 604, 606A-D, 603A-C, 605A-D, 602A-C in a long round |
| 5, 6 | `mp.mnu` (two screens) | `MenuMP` | 604, 606A-D, 602A-C, 603A-C |
| 8 | `mp.mnu` (hosting) | `MenuHost` | 604, 606A-D, 605A-D |
| 2, 3, 4, 7, 12, 13, 14 | `cmap`, `death`, `game`, `stat`, `vehicle`, `weapon` | none | back to `LoopSection`, which plays `JOMEN602A` and parses again: 602A over and over |

The expansion's `MJox01` pair (317 streams) has the same sections. An
expansion's music is `expansion\<name>\M<name>.sbf` and `G<name>.sbf` (loose
files) with `M<name>.bin` and `G<name>.bin`, chosen whenever the expansion's
`.pff` exists: an expansion that ships part of its music plays none.

## What we made

Means per cue; retail's in brackets.

| Cue | Masters | Length | Peak, RMS | Loudest 50 ms | Under 250 Hz |
|---|---|---|---|---|---|
| Null | `ONNULL` | 0.050 s (0.050) | silent | | |
| Mission start | `ONGAMINT` | 22.8 s (8.4) | -6.2, -20.2 dB (-6.9, -25.9) | -13.8 dB (-18.6) | 63 % (13) |
| Win | `ONWIN1_1` to `_3` | 7.2, 7.2, 8.4 s (81.9 in six) | -6.2, -19.3 to -22.3 dB (-6.0 to -7.9, -19.3 to -22.3) | -13.2 to -14.6 dB (-15.6 to -19.0) | 32 to 66 % (44 to 67) |
| Lose | `ONLOSE1_1` to `_3` | 6.0 s each (38.9 in five) | -6.2 to -8.7, -19.3 to -21.6 dB (-6.0, -19.6 to -22.1) | -16.0 to -17.8 dB (-15.0 to -16.8) | 28 to 87 % (38 to 57) |
| Menu beds | `ONMENU01` to `04` | 24.0 s each, looping (4.6 to 27.3) | -6.2 to -8.3, -20.2 to -21.1 dB (-6.0 to -7.9, -17.4 to -23.8) | -12.6 to -15.7 dB (-13.0 to -16.7) | 48 to 80 % (50 to 96) |
| Menu error | `ONMENUERR` | 5.5 s, looping (5.51) | -14.2, -23.0 dB (-11.9, -23.1) | -18.6 dB (-16.3) | 97 % (97) |
| Expansion | `ONSMENU01`, `ONSGAMINT`, `ONSWIN1_1`, `ONSLOSE1_1` | 24.0, 18.0, 10.2, 9.0 s | -6.2 to -7.6, -20.3 to -22.1 dB | -13.3 to -14.6 dB | 54 to 83 % |

Total: 17 masters, 19.9 MB of 16-bit WAV (`masters/`, LFS); 3.8 MB of sources.
Encoded, the four banks come to about 10 MB (the engine's encoder,
`sbf_encode_stream`, measured: 2.8 MB `gamemus.sbf`, 4.5 MB `menumus.sbf`,
1.6 MB `Gonjo1.sbf`, 1.3 MB `Monjo1.sbf`), and every master encodes unclipped.

## The banks and scripts

Made in the editor: each bank a stream per master (the music bank document's
stream from a wave), each script its MUS text in the music script document,
which compiles it. A script plays a stream by its place, so each bank keeps
this order.

| Bank | # | Stream (master) |
|---|---|---|
| `assets/gamemus.sbf` | 0 to 7 | `ONNULL`, `ONGAMINT`, `ONWIN1_1`, `ONWIN1_2`, `ONWIN1_3`, `ONLOSE1_1`, `ONLOSE1_2`, `ONLOSE1_3` |
| `assets/menumus.sbf` | 0 to 4 | `ONMENUERR`, `ONMENU01`, `ONMENU02`, `ONMENU03`, `ONMENU04` |
| `expansions/onjo1/music/Gonjo1.sbf` | 0 to 3 | `ONNULL`, `ONSGAMINT`, `ONSWIN1_1`, `ONSLOSE1_1` |
| `expansions/onjo1/music/Monjo1.sbf` | 0, 1 | `ONMENUERR`, `ONSMENU01` |

The expansion ships all four of its files or it plays no music at all, in the
menus or in a mission.

`gamemus.bin` (script `missionmusic`) keeps the original's shape, ours a
three-part cue each way; the handler's table takes the game's 0, 1 and 2:

```
section Begin { SV(200)  enter PickOpening }
handler MessageHandler(msgtype, source) { FB()  on (msgtype) enter Settle Victory Defeat }
section Settle { enter Settle }
section Victory { play ONWIN1_1  play ONWIN1_2  play ONWIN1_3  enter Settle }
section Defeat { play ONLOSE1_1  play ONLOSE1_2  play ONLOSE1_3  enter Settle }
section PickOpening { if ((Var01 != 0)) { enter Settle } else { enter Opening } }
section Opening { play ONNULL (twenty times)  play ONGAMINT  enter Settle }
```

`Gonjo1.bin` (script `stormmission`) is the same with `ONSGAMINT`, `ONSWIN1_1`
and `ONSLOSE1_1`, one stream a cue. `menumus.bin` (script `menumusic`) maps the
screens' `MUSICVAR` (the values JO:CA's menus use, which ours now carry) onto
the four beds, each a seamless loop the script plays again while the screen
stays:

| `Var02` | Screens | Section | Stream | Retail |
|---|---|---|---|---|
| 0 | none written yet | `NoScreen` | `ONMENUERR` | `MENUERR` |
| 1, and 2, 7, 13, 14 | `main`; `cmap`, `vehicle`, `weapon` | `MainBed` | `ONMENU01` | the theme; `JOMEN602A` over and over |
| 9 | `options` | `OptionsBed` | `ONMENU02` | the credits suite |
| 5, 6, 8, 10, 11 | `mp`, `player`, `sp` | `PlayBed` | `ONMENU03` | `MenuMP`, `MenuHost`, `MenuPlayer`, `MenuSP` |
| 3, 4, 12 | `death`, `game`, `stat` | `GameBed` | `ONMENU04` | none (602A over and over) |

```
section Begin { SV(200)  enter PickBed }
section PickBed {
    if ((Var02 == 0)) { enter NoScreen }
    if ((Var02 == 9)) { enter OptionsBed }
    if ((Var02 == 5)) { enter PlayBed }    (and so for 6, 8, 10 and 11)
    if ((Var02 == 3)) { enter GameBed }    (and so for 4 and 12)
    enter MainBed
}
section NoScreen { play ONMENUERR  enter PickBed }
section MainBed { SV(200)  play ONMENU01  enter PickBed }
section OptionsBed { SV(200)  play ONMENU02  enter PickBed }
section PlayBed { SV(200)  play ONMENU03  enter PickBed }
section GameBed { SV(200)  play ONMENU04  enter PickBed }
```

`Monjo1.bin` (script `stormmenu`): `PickBed` enters `NoScreen` (`ONMENUERR`)
for `Var02` 0 and `StormBed` (`ONSMENU01`) for every other value.

`ONMENU04` sounds only where the menu context is running. During a mission the
game context is the one open, and a `game` or `death` screen's `MUSICVAR` lands
in the game script's `Var02`, which it never reads, so those screens keep the
mission's silence after `ONGAMINT`, as retail's do. Whether the menu context is
open again by the time `stat.mnu` shows after a mission is not witnessed in
docs/audio/mus-sbf-re.md.
