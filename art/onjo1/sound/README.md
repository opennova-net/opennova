# `art/onjo1/sound/`: the base game's sounds

The sources of the base game's sounds (`assets/`): the footsteps, deaths, falls
and parachute the infantry sound profile plays and the impacts the carbine's
round plays on each surface.
Every game file is our own: cut, layered and processed from CC0 recordings, or
synthesised. The original game's sounds are reference only: extracted to a
scratch folder outside the repo, measured and never committed or imported.

- `SOURCES.md`: every game file, what it was made from and how; every source
  file with its author, page and licence.
- `sources/`: the CC0 originals as downloaded (Freesound's HQ OGG preview of
  each sound, the stream its page plays; Kenney's Impact Sounds pack).
- `masters/`: the synthesised sounds at 44.1 kHz 16-bit, before the game
  format's resample and quantise (a synthesised sound has no other source).

## Rules

- **CC0 1.0 only**, recorded per file in `SOURCES.md`.
- **Modelled on the original, never taken from it.** The reference is Joint
  Operations: Combined Arms (Steam). Its sets, formats, lengths, loudness and
  variation counts are measured; its audio never enters the repo.
- **The game formats the original uses**: PCM mono, 22050 Hz 8-bit for steps
  and soft hits, 44100 Hz 8-bit for hard hits, 22050 Hz 16-bit for the bullet
  pass, the rate and depth retail ships each counterpart in.
- **Names fit the archive**: at most 15 characters with `.wav`, starting `on`.
- The processing scripts are throwaway; the sources plus the exported wavs are
  the truth, and `SOURCES.md` says in words what each export is.

## Reference study (JO: Combined Arms)

### Footsteps

The infantry profiles in `SndProf.def` (`SP_JO_SP_PlayerM1`, `AI_JO_M1` and the
rest) author three of the four footstep pairs the engine picks from
(`audio::footstep_slot`: feet under water, standing on an entity, snow,
ground): `SSLFootGND`/`SSRFootGND` `FSP_DIRT_L`/`FSP_DIRT_R`,
`SSLFootOBJ`/`SSRFootOBJ` `FSP_OBJ_L`/`FSP_OBJ_R`, `SSFootWater` `FS_WATER`. No
shipped profile (base or `jox01`) authors `SSLFootSnow`/`SSRFootSnow`, so retail's snow steps are
silent. The sets live in `game.lwf`:

| Set | Members | Member pitch, random range | Volume | Falloff |
|---|---|---|---|---|
| `FSP_DIRT_L` / `_R` | 7 / 7 wavs | 0xF0A2 (0.94), 0x11EB / 0x0CCC | 100 | 50 |
| `FSP_OBJ_L` / `_R` | 4 / 5 | 0xFFFF, 0 / 0x0CCC | 100 | 50 |
| `FS_WATER` | 6 | 0xFFFF, 0x11EB | 80 | 50 |

Every set: one layer, flags 0xF (heading, internal, external, random), set
pitch 0xFFFF, cull range 10000. The step wavs are 22050 Hz 8-bit mono PCM,
peak-normalised to 0 dBFS. Dirt steps run 0.32 to 0.43 s at -14 to -21 dB RMS,
a heel thump (31 % of the energy under 250 Hz) under a gravel crunch (centroid
2.3 kHz, 22 % above 4 kHz) that decays to -30 dB in 0.1 to 0.35 s. Object
steps (0.37 to 0.46 s) are hollower: half the energy at 250 Hz to 1 kHz,
centroid 1.5 kHz. Water steps are long sloshes, 0.67 to 1.15 s, the peak
0.1 to 0.4 s in, centroid 1.5 kHz.

The same profiles fill the six anim-driven foley slots (the clips' FOLEY_1 to
FOLEY_6 bits): `SSAudio1` and `SSAudio2` `FSP_DIRT_R`, `SSAudio3` `FSP_OBJ_L`,
`SSAudio4` `FS_SWIM` (4 strokes, 0.96 to 1.88 s, volume 90, jitter 0x3333),
`SSAudio5` `FSP_PRONE` (8 crawl drags, 0.50 to 0.87 s, centroid 2.2 kHz, volume
60, jitter 0x3FFF) and `SSAudio6` `FSP_PRONE_ROLL` (4 rolls, 1.05 to 1.21 s,
volume 60, jitter 0x0CCC), all 22050 Hz 8-bit. The rebels' profiles put their
reload in `SSAudio5`; the player's and the friendly soldiers' put the crawl
there, which our person clips fire on the crawls. A stance change plays one of
three global sets by name, whatever the profile: `TO_CROUCH` (2 rustles),
`TO_PRONE` (1, a drop) and `TO_STAND` (3), 0.47 to 0.72 s, volume 70, jitter
0x0CCC (0x0F5C on the drop).

### Bullet impacts

`AMMO_CAR15_556MM` and `AMMO_M16_556MM` author the same 23 rows (tags 1 to 23,
contiguous, which the engine needs: it reads the compacted rows by tag
position). Hit effects are `Effect_AmHit*` particles (`none` on move and zip);
the sounds:

| Rows | Set | Layers |
|---|---|---|
| move | none | |
| obj | none (the hit effect alone) | |
| dirt, snow, packeddirt | `IMP_BULLET_DIRT` | 3 hits; a ricochet layer, 3 of 8 members a ricochet, the rest silent |
| grass | `imp_bullet_grass` | 3 members over 2 short thumps |
| cement, stone | `IMP_BULLET_CEMENT` | 8 cracks |
| water | `IMP_BULLET_WATER` | the step sloshes at 2.5x pitch; grass thumps at 3.5x |
| railroad, metal | `IMP_BULLET_METAL` | 8 hits; debris, 2 ricochets and 5 silent members |
| mud, ice | `imp_bullet_mud` | 4 |
| sand, quicksand | `IMP_BULLET_SAND` | 3 |
| wood | `IMP_BULLET_WOOD` | 6 hits; 3 splinters and 3 silent members |
| glass | `IMP_BULLET_GLASS` | 5 |
| cloth | `IMP_BULLET_CLOTH` | 3 |
| foliage | `IMP_BULLET_FOLIAGE` | 3 |
| hmetal | `IMP_BULLET_ARMOR` | 7 hits; 2 ricochets and 5 silent members |
| flesh / player | `IMP_BULLET_FLESH` / `IMP_BULLET_PLAYER` | 3, the same wavs |
| zip | `WSH_BULLET_BY2` | 8 passes |

Hard-surface hits are 44100 Hz 8-bit, soft ones (grass, sand, mud, cloth,
flesh) and the passes 22050 Hz (the passes mostly 16-bit). Lengths: dirt
0.14 to 0.19 s, cement 0.10 to 0.34 s, metal 0.09 to 0.50 s, wood 0.04 to
0.10 s, glass 0.34 to 0.64 s, sand 17 ms, ricochets 0.7 to 1.0 s, passes 0.15
to 0.75 s. They are loud: peak 0 dBFS and the loudest 50 ms window at -2.5
(metal) to -16 (grass) dB RMS, the metal and armour hits clipped square.
Spectra: cement and glass are cracks (centroid 5 and 7 kHz), metal a thunk
under a ring (half the energy under 250 Hz), wood splinters a 8 kHz crackle,
foliage a thump under a leaf rustle.

### The carbine (WPN_M4AUTO)

Its `weapon.def` actions name `GS_M4` (fire), `GF_M4_RL` (reload), `DRY_OICW`
(dry fire), `GF_M4_ST` and `GF_M4_SF` (switch to and from); the round's
`ai_launch` names `GS_M4AI`, the fire others hear. One wav per layer, no
variation:

| Set | Layer falloff: wav, format, length | Volume (clamp) | Loudest 50 ms |
|---|---|---|---|
| `GS_M4` | 150: the close shot, 44100 Hz 16-bit, 0.64 s, centroid 1.3 kHz; 900: the far report, 22050 Hz 16-bit, 1.47 s | 245; 255 (110) | -2.9; -13.9 dB |
| `GS_M4AI` | the same two at 80 (pitch 1.1) and 800 | 195; 255 (110) | |
| `GF_M4_RL` | 25: 2.24 s, 22050 Hz 16-bit, 13 clicks (magazine out, in, bolt) | 190 (230) | -8.7 dB |
| `DRY_OICW` | 25: 0.14 s, 22050 Hz 8-bit, one click (centroid 4.8 kHz) | 255 (180) | -9.0 dB |
| `GF_M4_ST` / `_SF` | 25: 0.79 s / 0.65 s, 22050 Hz 8-bit, 5 to 7 handling clicks | 130 | -12.5 dB |

### Deaths, falls, landings and the parachute

The player's and the friendly soldiers' profiles (`SP_JO_SP_PlayerM1`,
`AI_JO_M1`) fill eight more slots: `sounddeath` `BM1_DEATH`, `SSNightDead`
`BM1_DEATH_K` (the death on a night mission), `SSFallDead` `FALLDEAD2` (a dead
body landing), `SSFallAlive` `JUMPLAND_DIRT` (a live one landing), and the
parachute's `ChuteOpen` `PARACHUTE_OPEN`, `ChuteClose` `PARACHUTE_LAND`,
`ChuteFlap` `PARACHUTE_GLIDE` and `FreeFall` `FREEFALL`; they add `rotor_impact`
(a rotor striking the body). The rebels' profiles name their own voices
(`DEATH_INDO_1`, `DEATH_K_INDO_1`) and `FALLDEAD1`. The voices are in the
localised bank (`gamelocl.lwf`), the rest in `game.lwf`.

The slots play as the engine finds them: an AI body's death plays
`sounddeath` (`SSNightDead` when the mission enables night vision), but a
player's body plays the set named `<body>_DEATH` (`_DEATH_K` at night) whatever
its profile says, `<body>` the avatar's body model, `BM1` for the first
(`audio::compose_entity_sound_set`; `Entity_GetBodyModelPrefix @ 0x5280F0`), so
a bank without `BM1_DEATH` leaves the player's death silent. A body that lands
plays `SSFallAlive`, or `SSFallDead` if the fall killed it or it was already
dead; a falling player plays `FreeFall` each 64th tick under -0x3000 and the
chute's three as it opens, flaps and closes.

| Set | Layer (falloff) | Members | Member pitch, jitter | Volume (clamp) | Waves |
|---|---|---|---|---|---|
| `BM1_DEATH` | 150 | 8 | 0xE3D6 (0.89), 0x1EB8 | 200 (one 200) | 0.55 to 2.25 s, 22050 Hz 8-bit, death cries; loudest 50 ms -5.4 dB, centroid 1.3 kHz |
| `BM1_DEATH_K` | 140 | 3 | 0xFFFF, 0 | 200 (200) | 2.37 to 2.54 s, 8-bit, groans and gasps; RMS -16.9, loudest -7.0 dB |
| `FALLDEAD2` | 100 | 3 | 0xFFFF, 0 | 180 | 0.56 to 0.86 s, 8-bit, a body's thud at once (55 % under 250 Hz); loudest -5.0 dB |
| `JUMPLAND_DIRT` | 50 | 2 | 0xFFFF, 0x11EB | 108 | 0.53, 0.85 s, 8-bit, boots on dirt; loudest -6.7 dB |
| `PARACHUTE_OPEN` | 50 | 1 | 0xFFFF, 0 | 210 | 3.52 s, 22050 Hz 16-bit, a swell to the canopy's snap at 1.9 s; RMS -17.0 |
| `PARACHUTE_LAND` | 50 | 1 | 0xFFFF, 0 | 210 | 2.81 s, 16-bit, a thud as the canopy folds; RMS -24.1, loudest -9.0 dB |
| `PARACHUTE_GLIDE` | 50 | 7 | 0xB332 (0.70), 0x4CCC | 80 | six 2.40 to 2.61 s flaps and one of 0.71 s, 8-bit, cloth in the wind (70 % at 250 Hz to 1 kHz); RMS -16.9 |
| `FREEFALL` | 50 | 8 | 0x14CCB (1.30), 0x3333 | 190 | 2.35 to 4.54 s, 16-bit, wind; RMS -17.4, centroid 0.7 kHz |

### What the engine does with an impact table

In `AmmoDef_ParseProperty @ 0x40A2D0`, a `none` hit effect skips the effect intern (@ 0x40A51A) with no log; any other
name that resolves to nothing clones the engine's stock effect under that name
or logs "unknown particle id" (@ 0x40A549). The sound resolves by set name
across the loaded banks at parse (`SoundBank_FindSetByNameAnyBank`
@ 0x40A573), and the mission start loads the banks (@ 0x52544A) before
`ammo.def` (@ 0x525495), so a set the banks lack is silence, not a crash.

## What we made

Measured the same way (means per family; retail's in brackets):

| Family | Files | Length | Centroid | Loudest 50 ms |
|---|---|---|---|---|
| Ground steps | 7 + 7 | 0.42 s (0.32 to 0.43) | 1.9 kHz (2.3) | -11.5 dB (-10.3) |
| Snow steps | 4 + 4 | 0.45 s (none in retail) | 1.6 kHz | |
| Object steps | 4 + 5 | 0.42 s (0.37 to 0.46) | 0.8 kHz (1.5) | -10.9 dB (-10.8) |
| Water steps | 6 | 0.48 to 0.95 s (0.67 to 1.15) | 1.7 kHz (1.5) | -9.0 dB (-10.4) |
| Dirt, ricochets | 3, 3 | 0.19 s, 0.90 s | 2.9, 2.2 kHz (1.5, 5.0) | -4.4, -5.2 dB (-4.4, -5.7) |
| Grass | 3 | 0.11 s (0.05 to 0.11) | 1.1 kHz (1.4) | -11.0 dB (-16.2) |
| Cement | 8 | 0.24 s (0.10 to 0.34) | 5.4 kHz (5.3) | -7.9 dB (-7.9) |
| Metal, debris | 8, 1 | 0.30 s, 0.60 s | 2.4, 1.9 kHz (1.3, 2.5) | -4.3, -8.2 dB (-2.5, -8.1) |
| Armour, ricochets | 7, 2 | 0.40 to 0.55 s, 0.72 s | 2.5, 2.5 kHz (3.1, 3.9) | -4.2, -5.3 dB (-4.1, -5.5) |
| Mud | 4 | 0.20 to 0.45 s (0.15 to 0.47) | 2.3 kHz (1.5) | -11.6 dB (-12.0) |
| Sand | 3 | 40 ms (17 ms) | 3.6 kHz (2.5) | -8.9 dB (-8.4) |
| Wood, splinters | 6, 3 | 0.10 s, 0.26 s | 1.5, 9.0 kHz (2.1, 7.9) | -8.8, -12.5 dB (-7.9, -12.4) |
| Glass | 5 | 0.40 to 0.55 s (0.34 to 0.64) | 8.1 kHz (7.4) | -5.3 dB (-5.5) |
| Cloth | 3 | 0.15 s (0.06 to 0.19) | 0.5 kHz (0.7) | -11.9 dB (-11.4) |
| Foliage | 3 | 0.15 to 0.38 s (0.13 to 0.40) | 6.0 kHz (4.5) | -7.7 dB (-8.4) |
| Flesh | 3 | 0.14 s (0.02 to 0.14) | 0.5 kHz (1.3) | -8.0 dB (-7.9) |
| Bullet pass | 8 | 0.16 to 0.72 s (0.15 to 0.75) | 3.7 kHz (4.2) | -6.8 dB (-6.9) |
| Silent member | 1 | 50 ms | | |
| Carbine fire, near and far | 1, 1 | 0.64 s, 1.47 s (0.64, 1.47) | 1.7, 1.3 kHz (1.3, 1.8) | -3.0, -8.0 dB (-2.9, -13.9) |
| Carbine reload | 1 | 2.24 s (2.24) | 3.5 kHz (3.4) | -5.1 dB (-8.7) |
| Carbine dry fire | 1 | 0.15 s (0.14) | 4.2 kHz (4.8) | -8.0 dB (-9.0) |
| Carbine switch to, from | 1, 1 | 0.79 s, 0.65 s (0.79, 0.65) | 1.3, 1.5 kHz (3.1, 5.4) | -7.1, -6.9 dB (-12.5, -12.4) |
| Swim strokes | 4 | 1.3 to 1.8 s (0.96 to 1.88) | 1.5 kHz (1.2) | -8.6 dB (-7.8) |
| Prone crawl | 8 | 0.58 to 0.73 s (0.50 to 0.87) | 2.4 kHz (2.2) | -12.7 dB (-12.8) |
| Prone roll | 4 | 1.10 to 1.19 s (1.05 to 1.21) | 1.9 kHz (2.0) | -10.4 dB (-9.9) |
| Stance changes | 2 + 1 + 3 | 0.48 to 0.72 s (0.47 to 0.72) | 1.9 kHz (1.8) | -15.6 dB RMS (-15.4) |
| Death cries | 8 | 0.80 to 1.65 s (0.55 to 2.25) | 1.5 kHz (1.3) | -5.5 dB (-5.4) |
| Deaths at night | 3 | 2.45 s (2.37 to 2.54) | 1.0 kHz (1.3) | -7.1 dB (-7.0) |
| Dead body landing | 3 | 0.63 to 0.85 s (0.56 to 0.86) | 0.5 kHz (0.9) | -5.4 dB (-5.0) |
| Landing on the feet | 2 | 0.55, 0.82 s (0.53, 0.85) | 1.0 kHz (1.2) | -6.8 dB (-6.7) |
| Parachute opening, landing | 1, 1 | 3.65 s, 2.80 s (3.52, 2.81) | 0.8, 0.8 kHz (0.9, 0.8) | -8.6, -8.8 dB (-7.4, -9.0) |
| Parachute flapping | 6 + 1 | 2.40 to 2.60 s, 0.71 s (2.40 to 2.61, 0.71) | 0.6 kHz (1.0) | -15.8 dB RMS (-16.9) |
| Free fall | 8 | 2.35 to 4.54 s (2.35 to 4.54) | 0.6 kHz (0.7) | -12.2 dB RMS (-17.4) |

The member volumes below match each family's loudness to retail's: retail's
volume times the ratio of the two families' loudest-50 ms RMS, at most 255;
for the long winds and flaps (`ON_FREEFALL`, `ON_CHUTE_GLIDE`) the ratio of
their whole-file RMS.

## The sets and the profile (`game.lwf`, `SndProf.def`)

Authored in the editor (the sound bank and SndProf.def documents, through
`edit_record`). Every set: set pitch
0xFFFF, no set pitch jitter, cull range 10000, set flags 0; every layer flags
0xF, min distance 0; every member clamp 255 unless noted. "pitch" is the
member's base, "jitter" its random range, both Q16.

| Set | Layer (falloff) | Members | Pitch | Jitter | Volume |
|---|---|---|---|---|---|
| `ON_FS_GND_L` | 50 | `onfsgnd01` to `07` | 0xF0A2 | 0x11EB | 115 |
| `ON_FS_GND_R` | 50 | `onfsgnd08` to `14` | 0xF0A2 | 0x0CCC | 115 |
| `ON_FS_SNOW_L` | 50 | `onfssnw1` to `4` | 0xFFFF | 0x11EB | 115 |
| `ON_FS_SNOW_R` | 50 | `onfssnw5` to `8` | 0xFFFF | 0x0CCC | 115 |
| `ON_FS_OBJ_L` | 50 | `onfsobj1` to `4` | 0xFFFF | 0 | 100 |
| `ON_FS_OBJ_R` | 50 | `onfsobj5` to `9` | 0xFFFF | 0x0CCC | 100 |
| `ON_FS_WATER` | 50 | `onfswat1` to `6` | 0xFFFF | 0x11EB | 68 |
| `ON_FS_SWIM` | 50 | `onfsswm1` to `4` | 0xFFFF | 0x3333 | 99 |
| `ON_FS_PRONE` | 50 | `onfsprn1` to `8` | 0xFFFF | 0x3FFF | 60 |
| `ON_FS_PRONE_ROLL` | 50 | `onfsrol1` to `4` | 0xFFFF | 0x0CCC | 63 |
| `TO_CROUCH` | 50 | `ontocrh1`, `ontocrh2` | 0xFFFF | 0x0CCC | 70 |
| `TO_PRONE` | 50 | `ontoprn1` | 0xFFFF | 0x0F5C | 70 |
| `TO_STAND` | 50 | `ontostd1` to `3` | 0xFFFF | 0x0CCC | 70 |
| `ON_IMP_DIRT` | 60 | `onimpdrt1` to `3` | 0xFFFF | 0x2666 | 230 |
| | 90 | `onnull` x5 | 0xFFFF | 0x3333 | 255 |
| | | `onricodt1` to `3` | 0xFFFF | 0x3333 | 125 |
| `ON_IMP_GRASS` | 80 | `onimpgrs1` to `3` | 0xFFFF | 0x7FFF | 125 |
| `ON_IMP_CEMENT` | 100 | `onimpcem1` to `8` | 0xFFFF | 0x2147 | 240 |
| `ON_IMP_WATER` | 60 | `onfswat1` to `6` (clamp 127) | 0x27FFD | 0 | 98 |
| | 60 | `onimpgrs1` to `3` | 0x37FFC | 0x7FFF | 63 |
| `ON_IMP_METAL` | 130 | `onimpmtl1` to `8` | 0xFFFF | 0x1EB8 | 255 |
| | 100 | `onimpmdb1` | 0xFFFF | 0 | 225 |
| | | `onnull` x5 | 0xFFFF | 0 | 255 |
| | | `onricodt1`, `onricodt3` | 0xFFFF | 0x6666 | 205 |
| `ON_IMP_MUD` | 100 | `onimpmud1` to `4` | 0xFFFF | 0x7FFF | 175 |
| `ON_IMP_SAND` | 100 | `onimpsnd1` to `3` | 0xFFFF | 0x7FFF | 185 |
| `ON_IMP_WOOD` | 100 | `onnull` x3 | 0x13332 | 0x3333 | 125 |
| | | `onwdsplt1` to `3` | 0x13332 | 0x3333 | 125 |
| | 100 | `onimpwod1` to `6` | 0xFFFF | 0x7FFF | 250 |
| `ON_IMP_GLASS` | 140 | `onimpgls1` to `5` | 0xFFFF | 0x30A3 | 205 |
| `ON_IMP_CLOTH` | 80 | `onimpclt1` to `3` | 0xFFFF | 0x4CCC | 240 |
| `ON_IMP_FOLIAGE` | 100 | `onimplvs1` to `3` | 0xFFFF | 0xE665 | 120 |
| `ON_IMP_ARMOR` | 100 | `onimparm1` to `7` | 0xFFFF | 0x1997 | 215 |
| | 130 | `onnull` x5 | 0xFFFF | 0x3333 | 255 |
| | | `onricohm1`, `onricohm2` | 0xFFFF | 0x3333 | 125 |
| `ON_IMP_FLESH` | 80 | `onimpfls1` to `3` | 0xFFFF | 0x7FFF | 200 |
| `ON_IMP_PLAYER` | 100 | `onimpfls1` to `3` | 0xFFFF | 0x7FFF | 200 |
| `ON_BULLET_BY` | 80 | `onbltby1` to `8` | 0xFFFF | 0x4CCC | 100 |
| `GS_ONAR15` | 150 | `onar15f1` | 0xFFFF | 0 | 245 |
| | 900 | `onar15f2` (clamp 110) | 0xFFFF | 0 | 130 |
| `GS_ONAR15AI` | 80 | `onar15f1` | 0x11998 | 0 | 195 |
| | 800 | `onar15f2` (clamp 110) | 0xFFFF | 0 | 130 |
| `GF_ONAR15_RL` | 25 | `onar15rl` (clamp 230) | 0xFFFF | 0 | 125 |
| `DRY_ONAR15` | 25 | `onar15dry` (clamp 180) | 0xFFFF | 0 | 225 |
| `GF_ONAR15_ST` | 25 | `onar15st` | 0xFFFF | 0 | 70 |
| `GF_ONAR15_SF` | 25 | `onar15sf` | 0xFFFF | 0 | 70 |
| `BM1_DEATH` | 150 | `ondeath1` to `8` | 0xE3D6 | 0x1EB8 | 201 |
| `BM1_DEATH_K` | 140 | `ondeathk1` to `3` (clamp 200) | 0xFFFF | 0 | 202 |
| `ON_FALLDEAD` | 100 | `onfldead1` to `3` | 0xFFFF | 0 | 188 |
| `ON_JUMPLAND` | 50 | `onjmplnd1`, `onjmplnd2` | 0xFFFF | 0x11EB | 109 |
| `ON_CHUTE_OPEN` | 50 | `onchtopn` | 0xFFFF | 0 | 241 |
| `ON_CHUTE_LAND` | 50 | `onchtlnd` | 0xFFFF | 0 | 205 |
| `ON_CHUTE_GLIDE` | 50 | `onchtflp1` to `7` | 0xB332 | 0x4CCC | 70 |
| `ON_FREEFALL` | 50 | `onfrfall1` to `8` | 0x14CCB | 0x3333 | 104 |

A layer plays one member, chosen at random, and every layer of a set plays at
once, so a silent `onnull` member is the chance that the second layer adds
nothing (a ricochet on 3 of 8 dirt hits, as retail). The carbine's sets are
the ones its `weapon.def` row names (`GS_ONAR15` fire, `GF_ONAR15_RL` reload,
`DRY_ONAR15` dry fire, `GF_ONAR15_ST` and `GF_ONAR15_SF` switch to and from);
`GS_ONAR15AI` is for `AMMO_ON_556`'s `ai_launch`, the fire others hear.

The infantry profile `on_soldier` in `SndProf.def`, which the player and enemy
person items name with `sound_profile on_soldier`:

| Slot | Set |
|---|---|
| `sounddeath` | `BM1_DEATH` |
| `SSNightDead` | `BM1_DEATH_K` |
| `SSFallDead` | `ON_FALLDEAD` |
| `SSFallAlive` | `ON_JUMPLAND` |
| `SSLFootGND` / `SSRFootGND` | `ON_FS_GND_L` / `ON_FS_GND_R` |
| `SSLFootSnow` / `SSRFootSnow` | `ON_FS_SNOW_L` / `ON_FS_SNOW_R` |
| `SSLFootOBJ` / `SSRFootOBJ` | `ON_FS_OBJ_L` / `ON_FS_OBJ_R` |
| `SSFootWater` | `ON_FS_WATER` |
| `SSAudio1`, `SSAudio2` | `ON_FS_GND_R` |
| `SSAudio3` | `ON_FS_OBJ_L` |
| `SSAudio4` | `ON_FS_SWIM` |
| `SSAudio5` | `ON_FS_PRONE` |
| `SSAudio6` | `ON_FS_PRONE_ROLL` |
| `ChuteOpen` / `ChuteClose` | `ON_CHUTE_OPEN` / `ON_CHUTE_LAND` |
| `ChuteFlap` | `ON_CHUTE_GLIDE` |
| `FreeFall` | `ON_FREEFALL` |

A step plays only where the body's clip authors a foot event (bit 0x1 left,
0x2 right) on the frame the foot lands, a foley slot where it sets that slot's
bit (0x20 to 0x400). The death sets keep the engine's names: the player's
death plays `BM1_DEATH` (`BM1_DEATH_K` at night) by name whatever its profile
says, so the profile names the same two sets, as the original's player profile
does, and the rebel cries as the player does. `rotor_impact` stays empty (the
game has no helicopter yet), and `default` stays empty.

`AMMO_ON_556`'s `effects_table` (`assets/ammo.def`) names the impact sets row by
row: obj, dirt, snow and packeddirt `ON_IMP_DIRT`; grass `ON_IMP_GRASS`; cement
and stone `ON_IMP_CEMENT`; water `ON_IMP_WATER`; railroad and metal
`ON_IMP_METAL`; mud and ice `ON_IMP_MUD`; sand and quicksand `ON_IMP_SAND`;
wood, glass, cloth, foliage `ON_IMP_WOOD`, `_GLASS`, `_CLOTH`, `_FOLIAGE`;
hmetal `ON_IMP_ARMOR`; flesh `ON_IMP_FLESH`; player `ON_IMP_PLAYER`; zip
`ON_BULLET_BY`; move none. Unlike the original's carbine, obj plays a sound
(the dirt set, as the original's default ammo `AT_NULL` does), so a hit on an
object of no material, or on terrain of class 0, is heard. Every hit effect is
`none` until the game has its own particles.
