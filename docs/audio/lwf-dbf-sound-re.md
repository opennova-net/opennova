# LWF sound banks, DBF dialog banks, and the sound stack

How Joint Operations loads and plays its `.lwf` sound-profile banks and `.dbf` dialog banks, as
witnessed in the original engine. This is the reverse-engineering record behind `libs/lwf`,
`libs/dbf`, `libs/audio` (member selection), the `NovaLwfData`/`NovaDbfData` wrappers, and the
`NovaSoundBank`/`NovaMissionAudio` runtime.

Reverse-engineered from `Jointops.exe` (Joint Operations: Combined Arms, imagebase `0x400000`,
IDB `Jointops.exe.kong.i64`). All addresses below are absolute in that image. The container
layout was first derived from the authoring tools (lwfbuilder.exe / lwf2sdf.exe / sndc.exe);
this record anchors it to the engine reader (grilled 2026-06-09).

---

## LWF container ('LWF1')

`SoundBank_OpenFile @ 0x75caa0` opens a bank into a 204-byte slot: it reads the first dword as
the header size, re-reads that many bytes as the header, then `52 * single_count` bytes of
singles ("SNDTRIG DATA" @ 0x75cb33) and `12 * trigger_count` bytes of trigger records
("SNDTRIG TRIGGERS" @ 0x75cb63). `SoundBank_LoadTriggerSets @ 0x75c370` then reads the
MultiHeader at `multi_header_off`, the 80-byte set records (0x50 read @ 0x75c42b), and per set
the 48-byte playlist records (0x30 @ 0x75c548) and 28-byte member records (0x1C @ 0x75c5bb),
patching every file offset into a live pointer.

| Region | Stride | Engine witness |
|---|---|---|
| Header | 28 | first-dword size read @ 0x75cb03 |
| Single (audio ref) | 52 | `entries + 52 * index` @ 0x75c5d8 |
| Trigger record | 12 | alloc @ 0x75cb63 (count = header dword 3; 0 in every JO-era bank) |
| Multi (sound set) | 80 | read 0x50 @ 0x75c42b; name@4 used as alloc tag @ 0x75c50a |
| Playlist (layer) | 48 | read 0x30 @ 0x75c548 |
| Sndparm (member) | 28 | read 0x1C @ 0x75c5bb |
| String pool | 256/single | `count << 8` @ 0x75c688, patched into single+48 |

The magic `'LWF1'` (0x4C574631) gates only the filename-table patch @ 0x75c671 — a bank with a
wrong magic still loads, it just gets no wave filenames. Out-of-range member `single_index` is
coerced to 0 @ 0x75c5c8; layer/member counts above 8 are clamped to 8 (@ 0x75c648 / 0x75c61b).
`libs/lwf` is deliberately stricter on all three (parse errors instead), and rejects a nonzero
`trigger_count` rather than dropping the unmodeled table on re-encode.

Field semantics witnessed in playback (`SoundBank_PlayTriggerEntries @ 0x75ccd0`,
`SoundBank_SelectTriggerEntryFromBank @ 0x75bf20`):

| Field | Meaning | Witness |
|---|---|---|
| Multi dword 7 (`pitch_base`) | set pitch, Q16 (0xFFFF ~ 1.0), composed `(member_pitch * set) >> 16` | @ 0x75c0be |
| Multi dword 8 (`pitch_random_range`) | set pitch jitter `(range * rand8) >> 8` | @ 0x75c09e |
| Multi dword 18 (`target_id`) | copied to in-memory set+72 and read as the **3D one-shot cull range** (axis + euclid + occlusion-inflated recheck, whole units) — name resolution still never uses it; every JOX set carries a nonzero range | loader @ 0x75c47f -> `Sound_Play3DPositional @ 0x527cd1-0x527da1` (corrected 2026-07-10; ported into `play_oneshot_3d` 2026-07-11 — renaming the `target_id` field to its range meaning across libs/lwf + the ONED sound workspace is a tracked follow-up) |
| Multi dword 19 (`set_flags`) | bit0: require layer flag 0x20 to match the listener view bit | @ 0x75cd54 |
| Playlist u16 @4 (`falloff_radius`) | **audible falloff radius** (tool column "Falloff"): vol runs `vol * (1 - d/r)^2` to ZERO at r; also the ambient-emitter cull range | `SoundBank_CalcDistanceVolPan @ 0x75ca20` guard @ 0x75ca31; @ 0x75cf5c; emitter range cache @ 0x52856a (corrected 2026-07-10: previously "full-volume radius") |
| Playlist u16 @6 (`min_distance`) | **proximity fade radius** (tool column "Min distance"): inside it vol RISES as `(d/r)^2` (fades out closing on the emitter); the emitter path rebases the falloff to run min..falloff | @ 0x75cf1a..0x75cf55; emitter branches @ 0x528667..0x5286b9 (corrected 2026-07-10: previously "outer audible fade radius") |
| Playlist dword @12 | disk scratch; runtime random-seq cycle anchor | @ 0x75cd9b |
| Sndparm @4 (`pitch_scaled`) | member pitch base, Q16 | @ 0x75c109 |
| Sndparm @8 (`random_pitch_scaled`) | additive jitter `(range * rand8) >> 8` | @ 0x75cedc |
| Sndparm @12 (`volume`) | member volume 0..255 | @ 0x75cf25 |
| Sndparm @16 (`clamp_volume`) | ceiling on the distance-scaled volume; also the pan amplitude | `SoundBank_CalcDistanceVolPan` @ 0x75ca20 (clamp @ 0x75ca65) |

## Member selection and the sound RNG

The selection branch order @ 0x75cd5c..0x75cdfc (identical in both selectors):

1. `flags & 0x10` (sequential): play the cursor, advance, wrap to 0.
2. else `flags & 0x80` (random-sequential): with runtime bit 0x100 clear, draw a random anchor
   (`PRNG_ScaledRandom @ 0x75be50`), store it (layer word +12), set the cursor to it, play it,
   set bit 0x100. With 0x100 set, play the cursor and advance; when the cursor wraps back to the
   anchor, clear 0x100. Net effect: the anchor plays twice in a row, then every member in order.
3. else: a scaled-random member. Flag 0x08 ("Random" in the tools) is never tested — random is
   the engine default for unflagged layers.

One global RNG drives everything: `state = ROL32(state + ROL32(state, 11), 3)`, picks scale the
low byte as `(count * (state & 0xFF)) >> 8`. The state lives at `g_SoundRngState @ 0x85A3DC`,
statically seeded `0x2B0749C1` in .data and never reseeded; volume and pitch jitter draw from
the same stream during playback. `opennova::audio::SoundSelector` implements exactly this
machine (the stream is pinned by `tests/audio/sound_selector_test.cpp`); the reimpl does not
reproduce the interleaved volume/pitch draws, so a long-run stream diverges from a real session
even though algorithm and seed match. `SELECTION_FIRST` in `NovaLwfData` is an authoring-side
extension with no engine equivalent.

## Bank slots and load order

`Game_StartMission` loads six global bank slots in a fixed order (loop @ 0x525448 over the
0x104-stride name table @ 0x82A5B0, via `SoundBank_LoadIfExists @ 0x527530` into
`g_SoundBanks @ 0x24D6168`, 204 bytes per slot):

| Slot | Name | Filled by |
|---|---|---|
| 0 | `<expansion>L.lwf` | `Expansion_LoadAssets` @ 0x4a4989 (empty without an expansion) |
| 1 | `<expansion>.lwf` | `Expansion_LoadAssets` @ 0x4a495e |
| 2 | `gamelocl.lwf` | static (localized voice) |
| 3 | `game.lwf` | static (ambient loops / SFX, e.g. the `LPNV_*` sets) |
| 4 | `game3.lwf` | static (absent in JO base assets) |
| 5 | `game2.lwf` | static (absent in JO base assets) |

Name lookups search the slots in that order, first match wins
(`SoundBank_FindTriggerByName @ 0x75be90` — a case-insensitive `stricmp` over the 84-byte
in-memory set records; the engine's "trigger" is our Multi/sound set). A static game trigger
table @ 0x82F5B0 resolves once at mission start with a fallback-name second pass
(`DialogSystem_Init @ 0x5276cf`).

The mission co-named `.lwf` is NOT one of the six slots: `DialogSystem_Init @ 0x5275e0` builds
`<mission base>.dbf`, and `DialogManager_LoadFromFile @ 0x44e650` co-loads `<base>.lwf`
(falling back to `<base>.pwf` @ 0x44e7f5) into the dedicated dialog bank @ 0xA8A348 — only when
the `.dbf` exists. The menu UI has its own banks: `menu.lwf` loads at profile-selector init
(@ 0x5613bf into `g_MenuSoundBank @ 0x25DC3E0`), and menu XML elements add-ref further banks by
name through a 212-byte-entry collection (`sound_bank_collection_add_or_ref @ 0x652b40`, called
from `CUIElement_ParseXMLDefinition @ 0x648ada`) — menu-slice grill scope.

`NovaMissionAudio` loads one merged chain instead: co-named bank first (it carries the dialog
voices), then `gamelocl.LWF`, `game.lwf`, `game3.lwf`, `game2.lwf` in the engine's slot order.
Divergences from the original, accepted and documented:

- **D-SND-1 (bank scope):** the engine scopes the co-named bank to dialog playback; we keep it
  in the single search chain (a superset — dialog set names do not collide with ambient sets in
  JO assets).
- **D-SND-2 (expansion banks) — FIXED 2026-07-05:** `NovaMissionAudio` loads
  `<exp>L.lwf` / `<exp>.lwf` ahead of the static banks in the engine's slot order
  [orig: Expansion_LoadAssets @ 0x4a4989 / @ 0x4a495e; slot table @ 0x82A5B0] when the
  root was runtime-mounted with an expansion (`NovaResourceRoot.get_expansion()`, the
  `/exp` launch flag's mount). Missing files skip exactly like `SoundBank_LoadIfExists`.
- **D-SND-3 (strictness):** parser rejects what the engine silently tolerates (see above).

## DBF dialog banks ('DLG0')

`DialogManager_LoadFromFile @ 0x44e650`: 28-byte header (magic `'DLG0'` = 0x30474C44 checked
@ 0x44e6f7; version and header_size are never checked), `id_def_count` @ +12 (32-byte
"DlgMgr IDDEFS" records @ +16's offset — zero in every JO mission bank), group count @ +20 and
groups offset @ +24. Each dialog is a 52-byte group record followed by its 68-byte line
records, re-read as one `68*n + 52` blob @ 0x44e79d. A mission `PlayWavList` action carries a
dialog id; the group's lines name the sound definitions (sets) to play from the dialog bank
(`ActionSlot_PlaySound @ 0x4010c0` for the positional action path).

## Dialog playback is serialized — one channel at a time (grilled 2026-06-15; re-landed 2026-07-09)

A `PlayWavList` mission action does not play immediately; it **enqueues** a dialog,
and a per-frame updater plays the queue **one audio channel at a time**:

- `EventAction_Dispatch @ 0x4542e0` (PlayWavList case @ 0x454461) -> `Dialog_PlayByIndex
  @ 0x527ae0` formats the id as `"dlg%03i"` -> `Dialog_PlayByName @ 0x44d9f0` (matches the
  loaded dialog group by name, locale-suffixed first) -> `Dialog_Register @ 0x44d980`.
- `Dialog_Register` appends to a 256-entry queue (`dword_A89600` / count `dword_A895F8`)
  and claims one of 16 active slots (`dword_A8A248[]` data ptr, `dword_A8A24C[]` line/entry
  index, `dword_A8A250[]` countdown timer; active count `dword_A8A244`).
- `Dialog_UpdatePlayback @ 0x44e470` runs each frame. For each active slot it only loads
  and advances to the next clip when **no dialog channel is currently sounding** — the gate
  `!dword_A895FC || !AudioChannel_ValidateHandle(dword_A89DFC[dword_A895FC])` (@ 0x44e53a).
  `dword_A895FC` is the single live dialog channel index, set by `Dialog_LoadAudioClip
  @ 0x44dcc0`. A dialog *group* advances through its line records (entry index `+28` count)
  one line per free-channel cycle, with an inter-line countdown derived from the line byte
  `+53` (timer = `dword_A8A238 * byte53 / 10` with a counting-down high-bit flag,
  @ 0x44e585). So **dialog never overlaps dialog**: lines within a group and across queued
  groups play strictly sequentially. Each line load also broadcasts to clients
  (`Server_SendEntityStateToAll @ 0x44e5a5` with the group name + line index) — the
  MP dialog replication hook, out of scope for the SP-audio port (net follow-up). The
  `Dialog_PlayByName @ 0x44d9f0` "locale-suffixed first" pass degenerates in JO: its
  sprintf format is plain `"%s"` (empty locale suffix), so both passes match the same
  name.
- The pre-mission pass (`EventTrigger_UpdateAllWithFlag2 @ 0x454dc0`) dispatches actions
  through the **same** `EventTrigger_UpdateEntry @ 0x454c30` as normal events, so a
  PreMission `PlayWavList` *is* registered at mission start in the original — it just plays
  back serially through the one dialog channel, not all at once.

### Divergence

| ID | Ours | Original | Why / consequence |
|---|---|---|---|
| D-SND-4 | `NovaMissionAudio.play_dialog` **enqueues** the resolved line set-name(s) and plays them one at a time, starting the next on the previous voice's `finished` (`_dialog_queue` + `spawn_oneshot_2d`) | one dialog channel, `dword_A895FC`-gated, advanced by `Dialog_UpdatePlayback` | reimpl-side serialization that reproduces the observable behavior (no dialog overlap). The prior reimpl played every drained `dialog` effect immediately and non-blocking, so a mission's PreMission/early `PlayWavList` actions blared simultaneously at t=0. We do not model the 16-active-slot table or the per-line countdown timing (the reimpl presents on stream `finished`); the *id -> "dlg%03d" -> .DBF group lines* resolution matches the engine's `"dlg%03i"` path. |

## WAC scripted voice — `wave` / `pwave` (grilled 2026-06-15; re-confirmed 2026-07-09)

A WAC mission script triggers voice/wav lines separately from the BMS `PlayWavList`
dialog system. The `wave` and `pwave` commands both target `Wac_PlayScriptedVoiceWave`:

- guards on `g_local_player_entity` (no-op without a local player, like
  `Dialog_PlayByName`);
- `AudioChannel_ResetByHandle(dword_C6EC30)` — **resets the single scripted-voice
  channel first**, so a new `wave` *interrupts* the previous one (it is NOT a queue) —
  and frees the previous wave's decoded buffer (`AudioMem_SafeFree @ 0x4ed637`); the
  channel's anchor entity is set to the local player (`dword_C6EC34`), so plain
  `wave` plays at zero distance while the frame updater (`Audio_UpdateAmbientStream
  @ 0x4ed9c0`) spatializes waves anchored at other entities;
- `Audio_LoadWavFileFromArchive @ 0x766480` loads the named `.wav` from the archive (RIFF
  fmt/data, 8-bit→signed / 16-bit / IMA-ADPCM, `'AOA1'`), then plays it at
  `volume = (voiceVolume * 0xD2 + 0x80) >> 8`, pitch 1.0 (0x10000) — `0x24d20c8` is a
  settings-written volume option (written by `apply_session_settings_to_globals
  @ 0x5515a2` and the in-game options dialog `@ 0x554f83`; one slot below the SFX
  option `g_SoundVolumeOption @ 0x24D20CC`), so the ~0.82 factor rides the option→bus
  mapping our reimpl makes (D-SND-8).

So `wave` is a **separate interrupting channel** (`dword_C6EC30`) from the `.DBF` dialog
channel (`dword_A895FC`); the two can sound at once and `wave` does not serialize with
`PlayWavList`. `pwave` is the network-broadcast twin (same handler). The filename is a
direct `.wav` name, not a `.DBF` dialog id.

### Divergence

| ID | Ours | Original | Why / consequence |
|---|---|---|---|
| D-SND-5 | WAC `wave`/`pwave` emit a `"dialog_wav"` effect carrying the filename; the reimpl (`NovaMissionAudio.play_wac_wave`) reads the wav through the VFS, decodes via `NovaWavLoader`, and plays it on a single `_wac_voice` `AudioStreamPlayer` that `play()` restarts (interrupt-on-new) | `Wac_PlayScriptedVoiceWave` resets `dword_C6EC30` then plays the loaded wav | reimpl-side reproduction of the single interrupting voice channel. Previously WAC `wave`/`pwave` fell through `vm.cpp`'s default case to an unrouted `kind="wave"` effect and never played. Scope: `wave`/`pwave`; positional `SSNwave`/`SSNradio` (voice at an entity) and the rest of the sound family (`sound`, `sound2tgt`, `SS2SSN`, `waveready`) remain parsed-but-unconsumed, tracked follow-ups. |

## items.def marker sounds

`ItemDef_ParseProperty @ 0x49eb00` parses the marker-item sound keys: `soundloop_1..7`
(prefix match @ 0x49fec4, stored 24 bytes each at ItemDef+0x783..+0x813) and the time-of-day
one-shots `nightshot` / `duskshot` / `dawnshot` (@ 0x49fdee — their runtime CONSUMER is
unwalked; markers built only from shot keys, e.g. JOX "snd: CRIT-Froggy 1-shots", are
silent in our port until that grill happens: open follow-up). An `envs`-class item whose
current region slot is null registers nothing — there is NO fallback in the original
(JOX data agrees: of 166 `envs` items, 60 ship empty `soundloop_1..4` and ZERO of them
carry a `sound_profile` key — the profile key appears only on 78 non-envs items for the
entity-attached path below; an unwitnessed reimpl fallback to `sound_profile` was removed
2026-07-11).
`ItemDef_ResolveAllResources @ 0x49e7f0` resolves each non-empty name to a live set pointer
(`itemDef.soundLoopId[0..6]` @ +0x82C) via `SoundBank_FindSetByNameAnyBank @ 0x5274f0` — a
first-match search of the six loaded bank slots (renamed 2026-07-10; the kong name
`SoundProfile_FindLoadedByName` was wrong — the sound-PROFILE system is a separate,
XML-loaded layer whose category names include the unrelated `Soundloop_1..7` string table
@ 0x7d0788). Slot USE is per entity class: for `envsnd` markers slots 1..4 are the
time-of-day variants (next section); vehicles read slots 1..3 as skid/spray/dust loop sets
(`update_vehicle_effect_emissions @ 0x528f20` reads +0x82C/+0x830/+0x834), the movement
driver reads 1..3 (`Entity_ProcessMovementSoundEffects @ 0x5294a0`). Entity-attached sounds
use a separate composite-name path (`SoundProfile_FindByEntityAndType @ 0x528180`,
`"<EntityDefName>_<SoundType>"`).

## The sound-profile system — SndProf.def (witnessed + ported 2026-07-17)

The per-item slot-sound layer the footsteps/screams/foley ride. Port surfaces:
`libs/audio/sound_profile.{h,cpp}` (`SoundProfileTable`), `libs/def` (the two
profile keys), `libs/world/src/infantry.cpp` (`AiSystem::infantry_anim_sound_pass`
/ `emit_slot_sound`), `NovaSimulation::set_sound_profiles`/`drain_slot_sounds`,
`fire_present_pass.gd` `_drain_slot_sounds`. Evidence ctests: `sound_profile`
(parse + lookup + the asset-gated 49-profile retail sweep), `slot_sound`
(emission semantics).

**Load.** `SoundProfile_LoadAll @ 0x527490` allocates 128 slots x 2152 bytes
(grow-on-demand via `SoundProfile_AllocSlot @ 0x526f80`) and parses
**`SndProf.def`** through the shared ASCII-file tokenizer with
`sound_profile_xml_callback @ 0x526fc0` per line. Called at boot
(`Game_InitSubsystems @ 0x4a7141`, after `SoundProfile_ResetState @ 0x526de0` —
whose real body resets the profile globals + `g_SoundEmitterMixScale = 255`;
its old "Bink" IDB gloss was wrong) and from the three expansion-reload paths
(`@ 0x5527de/0x568425/0x5689e2`).

**Entry layout** (2152 bytes, all witnessed from the callback stores):
`+0` name[64] (a >=64-char `begin` name truncates `@ 0x527043`); `+64`
id[51] — the resolved per-slot sound-set pointers; `+268/+472` param2/param3[51]
(float columns x 65536 `@ 0x527122-0x527169`); `+676` param4[51] (atol);
`+880` the 12 med/crs loop fade/pitch percents x 655 (`@ 0x5270a9-0x527481`);
`+928` name24[51][24] — the authored set names. 928 + 51x24 = 2152 exactly.

**The 51-slot keyword table** (`@ 0x82F3B0-0x82F54C`; index == slot, the
authoritative slot map — mirrored as `audio::SoundProfileSlot` +
`kSlotKeywords`): 0-6 `Soundloop_1..7`, 7 `sounddeath`, 8 `SSNightDead`,
9/10 `door_open/close_sound_id`, 11-14 `dawnshot/dayshot/duskshot/nightshot`,
15 `SSFallDead`, 16 `SSFallAlive`, 17/18 `SSLFootGND`/`SSRFootGND`,
19/20 `SS*FootSnow`, 21/22 `SS*FootOBJ`, 23 `SSFootWater`, 24-29 `SSAudio1..6`,
30-33 engine start/stop/reverse/highrev, 34 `warning`, 35-40 the impact family
+ `rotor_impact`, 41-44 `ChuteOpen/ChuteClose/ChuteFlap/FreeFall`, 45
`drive_repeat`, 46 `swivel_shift`, 47-50 the tumble family. JO ships 49
profiles; the SP player pair authors dirt/OBJ/water feet, the chute family,
and `BM1_DEATH`/`BM1_DEATH_K`.

**Resolve.** At mission start (`Game_StartMission @ 0x525468`)
`resolve_sound_profile_triggers @ 0x528210` fills id[slot] =
`SoundBank_FindTriggerByName(name24[slot])` across the active banks
(first bank wins; empty name -> 0). Our reimpl resolves by NAME at play time
(NovaSoundBank is name-keyed), so the port's "id" IS the authored set name and
an empty slot is the id-0 no-op.

**Item binding.** `ItemDef_AllocateWithDefaults @ 0x49e3f5` seeds BOTH def
profile pointers (+0x268 primary / +0x26C female) to
`SoundProfile_FindSlotByName("default")`; `SoundProfile_FindSlotByName
@ 0x526e30` is a first-stricmp-match scan whose MISS returns the array base
(the first profile). `ItemDef_ParseProperty` binds `sound_profile`
(`@ 0x49fafd`: writes +0x268, and rewrites +0x26C only while it still equals
+0x268) and `sound_profileFemale` (`@ 0x49fb76`: writes +0x26C) — JOX authors
the female key exactly on the two SP/MP player defs.
`ItemDef_ResolveAllResources @ 0x49e5f0` then copies profile ids into the def's
own sound fields (soundLoopId[0..6] = id[0..6], death = id[7], doors =
id[9]/id[10], shots = id[11..14] when nonzero, + the shot param2/3 pairs) with
the per-item name overrides (soundloop_/doorsound/xshot/sounddeath keys)
re-resolving over them, and stores the slot-table base pointers
def+2148/+2152 = profile+64.

**Runtime accessor + play.** `Entity_GetProfileSlotSound @ 0x528300` (renamed
2026-07-17; the kong name `Entity_GetWeaponSlotTableValue` was a misnomer —
the table is the sound-profile slot array, not the weapon loadout): def+2148, or
def+2152 when the entity's character entity carries the female byte
(`CharacterEntity[12]` `@ 0x52831c`); returns id[slot]. Consumers play through
`Entity_PlaySound3D_FullVolume @ 0x528e20` = `Sound_Play3DPositional(id, &pos,
entity, 255)`. The complete accessor xref set is the two infantry body
updaters (all slots 7-29 + 15/16, witnessed in
[world-wac-ai-re.md](../world/world-wac-ai-re.md) §17.4b) — the vehicle slots
30-50 are read by the vehicle-physics family through def+0x864 directly
(`Entity_ProcessVehicleSuspension`, air/wheeled/light/tracked physics,
`Entity_ProcessInfantryPhysics` tumble legs) and ride the vehicle-sound grill.
`SoundProfile_FindByEntityAndType @ 0x528180` (the `"<DefName>_<Type>"`
composite path, §items.def above) is a SEPARATE mechanism; the org2 player
death edge uses it with type 5 (night) / 0 `@ 0x4b4c4a-0x4b4c6a` — unported
(D-SND-14).

### Divergences

| ID | Ours | Original | Why / consequence |
|---|---|---|---|
| D-SND-10 | slots 43/44 (ChuteFlap/FreeFall) refire every body tick; the reimpl bank declines to RESTART the set while its previous voice still plays (`play_oneshot_3d` exclusive key) | the refires land in a finite channel pool and steal their own channels — audibly one continuous rush | reimpl voice model (AudioStreamPlayer3D per fire); audibly equivalent, no 62-voice pileup |
| D-SND-11 | footstep OBJ slots 21/22 never picked — the pick falls through to the terrain surface | `entity+0x28 groundEntity` (written by the ground probes, e.g. `Entity_RaycastGroundHeightAndObject @ 0x525fd0`) selects `SS*FootOBJ` when standing on an entity | the platform link is unmodeled in the sim (`InfantryState::standing_on_entity` is wired but never set); lands with the platform slice |
| D-SND-12 | the female profile (def+2152) is never selected — primary always | the character entity's female byte picks it `@ 0x52831c` | avatar gender is unmodeled sim-side; JO NPC female defs author their own `sound_profile`, so only the shared player defs are affected |
| D-SND-13 | SndProf.def parses per mission load off the mission resource root | one boot-time load + expansion reloads | same file, same table; no observable difference |
| D-SND-14 | the local player's death scream rides the NPC slot-7/8 leg | org2 plays the composite `SoundProfile_FindByEntityAndType(def, 5/0)` name `@ 0x4b4c61` | the composite-name chain is unported (P2b player-death presentation); the profile scream is the same authored voice family |
| D-SND-15 | `Terrain_GetSurfaceTypeAtPosition`'s placed-tile override leg is not modeled (`terrain/surface_type_map.h` samples the charmap only) | `.til` placements remap the surface through `byte_319F7D8` `@ 0x6065ca-0x60660c` | placed-tile data is not sim-plumbed; feet on roads/runways read the underlying charmap class |

## Ground vehicle movement sounds (grilled 2026-07-29)

Ground engines are not a one-shot fired merely because the local player entered a
vehicle. They are three keyed sound-profile lanes whose eligible loops are
re-registered from signed vehicle speed and admitted by the vehicle's
primary-occupant latch.

**Ground caller and occupancy gate.** The ground physics family calls
`Entity_ProcessMovementSoundEffects @ 0x5294a0` once per vehicle tick from
`Entity_UpdateVehiclePhysics @ 0x48d1b6`. The caller passes the entity's signed
current speed (`entity+0x29C`) and the item-def `player_speed` (`def+0x8E8`) as both
the speed denominator and maximum sound-speed input. Magnitudes below 256 in
16.16 units are snapped to zero. A non-`PlayerControl` vehicle always reaches the
sound pass; a `PlayerControl` vehicle reaches it only while its primary occupant
(`entity+368`) is non-null (`@ 0x48d181..0x48d1c7`). That occupant may be an NPC:
the sound gate tests the vehicle-side claim, not local-player identity.

**The three shared-emitter lanes.** Active lanes register the authored
`Soundloop_1..3` sets through
`SoundEmitter_RegisterSetLayers @ 0x528340`, with a literal 30-tick lifetime
(0.48 s at 62.5 Hz). The lane number is the emitter slot-type key:

| Lane | Authored source | Direction / mix |
|---|---|---|
| 0 | `Soundloop_1` / sound-profile slot 0 | idle, pitch 1.0; while `abs(speed) < P`, volume is `clamp16(0x10000 - (abs(speed) << 16) / P)`, so the idle loop fades inversely to speed |
| 10 | `Soundloop_2` / slot 1 | forward drive; volume rises linearly through the first `P/16` of speed as `(abs(speed) << 16) / (P/16)`, then stays at `0xFFFF` |
| 20 | `Soundloop_3` / slot 2 | reverse drive; the same moving-volume ramp, registered only for reverse motion |

Here `P` is the item-def `player_speed`. Moving pitch starts at the slot's authored
`p2`, adds the rounded Q16 interpolation
`(abs(speed) / P) * (p3 - p2)`, and treats a zero `p3` as 1.0. The forward lane's
`p4 > 1` branch subdivides that interpolation into the witnessed gear sawtooth
instead of one continuous sweep. The ordinary moving leg registers exactly one
direction: `speed >= 0` selects lane 10 (`@ 0x529787` -> call `@ 0x5297c0`);
negative speed selects lane 20 (call `@ 0x5297a3`). Stationary motion skips that
leg under the normal non-collision/entity-def gate. The previously active
opposite lane is therefore not zero-cleared on an ordinary direction or
stationary change; it retires through the 30-tick lifetime. The idle registration
is separate at `@ 0x529882`.

These are not private vehicle channels. The registrations occupy the same
767-entry `(entity, slot type, layer)` table as placed ambience and are ranked
with it by `SoundEmitter_UpdateAndMixTop8 @ 0x5284a0`. Consequently vehicle and
ambient layers share retail's eight real-channel budget. A registration whose
pitch or volume is zero is the common clear operation. Both moving lanes receive
explicit zero registrations only on the collision branch
(`@ 0x5297db..0x52981a`) and the top all-zero/wreck branch
(`@ 0x5298d4..0x52995e`); otherwise a refreshed lane gets another 30 ticks.

**Direction shift and detach.** The ground caller keeps the reverse-direction
latch in `moveData+0x318` bit 2. It enters reverse only after both commanded speed
(`+0x220`) and actual speed (`+0x29C`) are negative, and leaves reverse when the
command becomes positive; either edge plays sound-profile slot 32
(`enginereverse`, used as the shift sound) once at `@ 0x48d21f`. Claimant detach
zero-calls `Entity_ProcessMovementSoundEffects` at `@ 0x435716`, clearing the
forward/reverse lanes through zero registrations, then plays slot 31
(`enginestop`) only while the vehicle is strictly above
`Env_WaterHeightFixed`, before clearing the primary occupant
[orig: `Entity_DetachFromVehicle @ 0x4355f0`].

Ground vehicles do **not** call the aircraft-only PlayerControl occupancy spawner
`entity_update_damage_accumulator_and_shadow @ 0x48fa70`. That function's slot-30
engine-start edge is reached only by the `CHel`/`cpln` updater. A parity port must
therefore drive a ground vehicle's persistent loops from the per-tick movement
pass and primary-occupant gate; synthesizing the aircraft engine-start one-shot
for `cveh`/`ctank`/`cbike` would invent behavior.

### Divergence

| ID | Ours | Original | Why / consequence |
|---|---|---|---|
| D-SND-17 | **PORTED 2026-07-29 (structural ground slice).** `world::update_ground_vehicle_sound` emits generic `SoundEmitterEvent` rows from the final ground-motor state: profile/item-override slots 0/1/2, lanes 0/10/20, the witnessed Q16 gain/pitch and p4 gear branches, 30-tick lifetime, validated primary-occupant gate, collision/wreck behavior, and slot-32 direction edges. `stop_ground_vehicle_sound` emits claimant-detach clears plus the above-water slot-31 stop. A bounded 767-key `SoundEmitterMailbox` retains only the newest source/lane intent and its producer tick; non-allocating clear/anchor controls displace allocation intents at saturation. `NovaSimulation::drain_sound_emitters` and `NovaMissionAudio.apply_sound_emitters` replay those ticks into the existing `AmbientMixer` and persistent eight-voice bank. Source-only anchor rows preserve retail's entity-position-pointer behavior while an unrefreshed lane expires. | the occupied ground-physics path refreshes the eligible idle/direction lanes with speed-derived volume/pitch; an ordinary inactive moving lane expires after 30 ticks, while collision/wreck/detach explicitly clear both; collision forces a full idle refresh; direction edges play slot 32 and claimant detach plays slot 31 only above water | An NPC claimant now keeps the same engine loops alive as a player claimant, keyed by registry lifetime + lane and competing in the shared 767-slot/loudest-eight mix. Focused proof covers stationary NPC occupancy on authority and joiner paths, stale-claim expiry, no-claimant silence, item override, forward/reverse gain and pitch, the p4 gear branch, collision/wreck behavior, direction and water-gated detach one-shots, bounded/coalesced and saturation-safe control transport, 64-bit lifetime keys, moving residual anchors, catch-up chronology, simulation/presentation drain, ID recycling, and shared reimpl ranking (`world`, `ai`, `vehicle_motor`, `ambient_mixer`, `nova_simulation_test.gd`, `nova_mission_audio_test.gd`, `fire_present_pass_test.gd`). Remote compact vehicle rows do not yet restore `veh.speed` or collision contact, so a joiner can reproduce the stationary NPC-truck idle case but moving remote gain/pitch and collision refresh remain open. Scope otherwise stays on the witnessed ground caller; the aircraft-only slot-30 start spawner and other vehicle families/slots remain unclaimed. |

Follow-ups: the remaining vehicle slot consumers (30, 33-50; direct def+0x864
readers outside the ground movement pass) ride their own vehicle-family grills.
Moving remote-vehicle sound needs either speed in the compact vehicle row or a
pose-derived signed-speed witness; remote collision sound behavior likewise
needs a replicated contact edge. Remote-client footstep presentation rides the
client anim path (net seam — the host never emits for net-snapped peers, same
as retail's per-client body updaters); the chute family 41-43 + the chute
brake physics ride the parachute slice (witness in world-wac-ai-re §17.4b).

## Placed ambient markers — the envsnd emitter system (grilled 2026-07-10)

How a placed "snd:" marker actually sounds: a per-tick class update registers transient
emitters, a per-frame mixer distance-ranks them onto 8 channels, and a shared curve computes
each voice's volume/pan.

**The marker update (`Entity_UpdateEnvSoundEmitter @ 0x4a8080`).** Sound markers are
entities whose items.def `move_function`/`ai_function` tag is **`envs`** (corrected
2026-07-11; earlier notes said "ai envsnd" — the string `envsnd` does not appear in JO
data). The class table (refined 2026-07-28) is 34 12-byte `{name[8], callback}` entries
@ 0x82ABC8 (count @ 0x82AD60; entry 0 `null` -> `nullsub_2 @ 0x4a8070`, which the pool
walks skip; entry 1 `envs` -> this function, its name string @ 0x82ABD4; entry 2 `ewep`).
`EntityDef_LookupPhysicsCallback @ 0x4a9240` resolves each def's `move_function` name
into `def+0x158` at init (`EntityDef_InitAllCallbacks`, `"Null"` default), and
`Entity_SpawnFromBMSRecord` copies `def+0x158 -> entity+0x1C4` (`updateCallback`) and
`def+0x138 -> entity+0x1C8` (`deathCallback`) at placed-item spawn (@ 0x40ec5a..0x40ec7a).
JOX ships 166 `envs`-class items; the `snd:` markers are `type marker` +
`move_function envs` blocks in ITEMS.DEF. Each visit (cadence below) it:

- picks the time-of-day region (below) and takes `soundLoopId[region]` — soundloop_1..4 are
  the **morning/day/evening/night** ambient variants (a flourescent-light marker fills only
  the night slot; jungle beds fill several). A null slot registers nothing — the marker is
  silent in that region.
- registers the set into the emitter table with volume = the region **crossfade blend**
  as the ROUNDED word `(0xFFFF * blend_q16 + 0x8000) >> 16` (@ 0x4a81c6; full blend is
  the 0xFFFF sentinel → word 0xFFFE → high byte 255; the mixer reads the word's HIGH
  byte as the emitter volume, slot byte +25 @ 0x52865e), pitch 1.0 (0x10000), and a
  per-class keep-alive lifetime (`*(def+92)`: classes 2/5/6 = 31, 4 = 72, 7 = 62,
  else 10 — in **62.5 Hz TICKS**, not ms (corrected 2026-07-28): the mixer decrements
  lifetimes by elapsed `current_tick` deltas @ 0x528541, so these are ~0.5 s / 1.15 s /
  1 s / 160 ms) — continuous sound = re-register every pool-2 visit (every 8th tick,
  cadence below); the marker default of 10 ticks outlives the 128 ms revisit gap by
  exactly two ticks, and dead owners expire within their keep-alive. The slot-type
  byte is the REGION index, so a region
  flip re-registers under a new key and the old region's slots expire through those
  lifetimes — that is the audible crossfade mechanism.
- suppresses the crossfade when the adjacent region's slot is the SAME set (@ 0x4a819d;
  the compare is on the resolved set pointers, null == null included).
- markers with a def bone index (`def+1354`) register at the bone-transformed position;
  placed `snd:` markers have none and use the entity position + attach offset.

**The driver cadence (witnessed 2026-07-28).** `Game_MainLoop @ 0x52b630` banks wall
clock in 1/16-ms fixed point and drains it in 4 ms quanta (`frame_time -= 64` @ 0x52ba21),
firing the active mode's UPDATE callback (mode struct +36) only on quanta where the
phase counter `& 3 == 0` (@ 0x52ba47) — every 16 ms = 62.5 Hz; the RENDER callback
(+40) runs ONCE per outer frame after the drain (@ 0x52bab8-0x52bac6). (A Sleep(1)
loop @ 0x52b8a6 caps the outer frame at 16 ms when `dword_2550744` is set — an
optional whole-loop limiter, off in the fast-retail reference configs.) The in-mission
mode struct is `"Game Loop"` @ 0x82f340: its update is `Game_ProcessMainFrame
@ 0x5263f0` (increments `current_tick` @ 0x5265b4, pumps net, runs
`Entity_UpdateAllEntities @ 0x4c2100`), its render is `GameLoop_RenderFrame
@ 0x521310` (renamed 2026-07-28 from the `render_loading_frame` misnomer, maintainer
OK) — the per-frame in-mission render callback:
listener update `Audio_UpdateListenerFromView @ 0x43b400` ->
`Audio_UpdateListenerPosition @ 0x527960`, engine sounds, the Top8 mix @ 0x521341
gated off for dedicated servers via `dword_A87050`, then the scene render).

`Entity_UpdateAllEntities` runs per-pool cadences inside the 62.5 Hz update: pool 0
(players/organics) calls `updateCallback` (+0x1C4) every tick (@ 0x4c2460); pool 1
(vehicles/mounted items) gets the per-tick parent-chain transform walk; **pool 2
(statics — the placed markers) is walked starting at `tick & 7` with stride 8
(@ 0x4c225a-0x4c228c): each pool-2 entity's `updateCallback` runs every 8th tick
(~7.8 Hz), cohort-staggered so 1/8 of the pool is visited per tick**, with the age
word (+0x2AC) decremented by 8 per visit gating the separate `deathCallback` think;
pool 3 (projectiles) strides `tick & 0x3F` (every 64th tick) as a fallback lane. So a
placed `snd:` marker's TOD/crossfade/registration eval runs at ~7.8 Hz — NOT per tick
and NOT per render frame — on top of the per-marker clock stagger below, while
attached/vehicle emitters re-register per tick through the direct legs
(`Entity_UpdateParentTransform @ 0x4a8d4f`, `Entity_UpdateWaterPhysicsAndEffects
@ 0x4a93d6`). `Entity_UpdateEnvSoundEmitter` itself has no internal rate gate — the
caller defines the cadence.

**Time of day (`Entity_CalcTimeOfDayRegion @ 0x408110`).** The env clock
(`Env_GetTimeOfDayHoursQ16 @ 0x57d5b0` = `Env_CurTimeFixed24 >> 8`, hours Q16.16) is cut at
4h / 10h / 17h / 21h into regions 0..3; the intervals are OPEN at the low cut (unsigned
range-check idiom starting one tick past each cut @ 0x408175/0x4081b0), so the exact cut
instant classifies as night at full blend for that single tick. Each region fades IN over
its first ~5 game-minutes (margin 5460 Q16 hours @ 0x408203) and OUT over its last ~5,
`blend = (dist << 16)/margin` (Q16) with a zero-distance guard reading the 0xFFFF full
sentinel (@ 0x408251); night (region 3) wraps 21h->4h and only its 21h edge blends
(@ 0x40820f — the wrapped high-edge test `regionHigh > t` can only pass on night's
pre-dawn leg where the fade window can't reach). A per-entity stagger —
`(poolHandle & 0xF) << 11` added to the clock (@ 0x408158, low nibble of the pool slot,
up to ~28 game-minutes) — de-syncs markers so they do not all flip at once.

**The emitter slot table (`SoundEmitter_RegisterSetLayers @ 0x528340`).** 767 x 48-byte
slots @ 0x24D66A8 (`g_SoundEmitterSlots`), keyed (entity, slot-type byte, layer index) — one
slot per LAYER of the registered set. Param block: `{+0 entity, +4 set, +8 pos ptr,
+12 velocity ptr (doppler, nullable), +16 lifetime ms, +20 pitch Q16, +24 volume 8.8
(hi byte = 0..255), +26 flag, +27 slot type}`. Registering with pitch 0 OR volume 0 CLEARS
the (entity, type) slots — the witnessed unregister (`SoundEmitter_ClearByEntityAndSlot
@ 0x527a50`; `SoundEmitter_ClearAllByEntity @ 0x527a90` on entity death). The MP-session
gate `g_napi_np_ctx.is_mp_session_peer` guards registration like the rest of the sound
stack (our D-MUS-SPGATE decision applies: we play in ALL sessions).

**The per-frame mix (`SoundEmitter_UpdateAndMixTop8 @ 0x5284a0`, called once per RENDER
frame from the "Game Loop" mode's render callback @ 0x521341 — cadence above).** Its
clock argument is `current_tick` (pushed @ 0x52133a), so lifetimes advance in ticks
regardless of frame rate. Every frame, each live slot: decrements its lifetime by the
elapsed ticks (@ 0x5284aa / 0x528541; expired slots self-clear), lazily caches the layer's `falloff_radius << 16` as its range (@ 0x52856a),
culls axis-wise then euclidean against it, inflates the distance by occlusion (below),
computes the volume through the two-radius curve on the layer's **member 0** — the emitter
path does NOT run the member-selection machine (@ 0x528649 reads layer+16). The arms pass
WHOLE-UNIT distances truncated AFTER the Q16 subtraction: rebased falloff
`CalcDistanceVolPan(HIWORD(d - min), HIWORD(falloff - min), ...)` (@ 0x528693-0x5286b9),
proximity `((min << 16) - d) >> 16` vs `HIWORD(min << 16)` (@ 0x528691) — i.e.
`floor(min - d)`, not `min - floor(d)` — with member volume and clamp pre-scaled by the
emitter byte `(emitterVol * member) >> 8` (@ 0x5286b9). The result scales by
`g_SoundEmitterMixScale` — whose only setter `sub_527ac0 @ 0x527ac0` is UNREFERENCED, so
it is constant 255 in retail (init `SoundProfile_ResetState @ 0x526de0`; a ~no-op fold) —
then the mixer **sorts every candidate by volume and keeps only the loudest 8** on real channels
(`g_AmbientChannelHandles` @ 0x24D6688): live channels get per-frame vol/pan/pitch updates
(`AudioChannel_SetAndPlay @ 0x766c40`), drop-outs are STOPPED (`AudioChannel_ResetByHandle
@ 0x767160`), entrants open via `AudioChannel_OpenSlotChecked @ 0x767060`. Pan is a bearing
byte from atan2 vs the listener yaw (near-field and own-entity read centered, @ 0x5289c0);
the options SFX volume (`g_SoundVolumeOption @ 0x24D20CC`, written by the options dialog)
scales every channel write.

**The volume curve (`SoundBank_CalcDistanceVolPan @ 0x75ca20`).** `d >= radius` returns 0 —
hard silent. Otherwise `inv = 1 - d/r` (Q0.16) and `volume = ((masterFade * vol) >> 24) *
inv^2 >> 32` — a QUADRATIC falloff — clamped at `clamp_volume` (the same parameter is the
pan amplitude); returns `(volume << 8) | pan`. `g_SoundMasterFadeQ24 @ 0x85A3E4` is a
mission-start fade ramp to 0xFF0000 (so steady state = `(vol * 255) >> 8`), stepped by
`Audio_UpdateListenerPosition @ 0x527960`, which also maintains the listener position/yaw/
velocity globals and the underwater flag (`g_SoundListenerUnderwater @ 0x33429A8` = listener
Z under `Env_WaterHeightFixed`) that HALVES volume and pan (@ 0x75ca7d). The two-radius
model in the emitter path (@ 0x528667..0x5286df): `min_distance` set and `d >= min` runs the
falloff REBASED over min..falloff; `d < min` runs the rising proximity fade `(d/min)^2`;
no min runs plain 0..falloff; BOTH radii zero is silent as an emitter (the packed volume
byte's `>> 8` is 0, @ 0x528704) while the one-shot path plays it at the raw emitter volume.
The one-shot path (`Sound_Play3DPositional @ 0x527cb0` -> `SoundBank_PlayTriggerEntries
@ 0x75ccd0`; the event-action route is `ActionSlot_PlaySound @ 0x4010c0` ->
`Entity_PlaySound3D_FullVolume @ 0x528e20`, emitter volume 255) differs deliberately:

- the SET's cull range (`Multi` dword 18, in-memory set+72) gates the fire entirely —
  axis checks then euclidean then the occlusion-inflated distance, all `<= range << 16`
  (@ 0x527cd1-0x527da1; every JOX set carries a nonzero range, e.g. 10000 on dialog sets);
- vol computes ONCE at fire; the proximity stage keeps FULL Q16 precision
  (`(min << 16) - d` vs `min << 16`, gate `HIWORD(d) < min` @ 0x75cf2b-0x75cf55) and
  feeds the falloff stage, which is NOT rebased (@ 0x75cf75);
- member volume composes with the emitter byte `+1` (@ 0x75cf25:
  `(member * (emitter + 1)) >> 8`);
- a layer with NO falloff radius plays at the RAW emitter volume — member volume (and a
  min-only layer's proximity result) is NOT consulted (@ 0x75cf88; zero JOX layers are
  min-only);
- PAN is not fire-time-only: the mixer's tail walks the audio-channel anchor registry
  (`0x3345B90`, 24-byte entries written by `AudioChannel_Open @ 0x7668f0` /
  `AudioChannel_Submit @ 0x766b70`) and re-pans every live entity-anchored channel from
  its CURRENT bearing each frame (@ 0x528af1-0x528ba4) — volume stays fire-time
  (corrects the earlier "vol/pan once at fire" note; Godot's spatial panner gives our 3D
  one-shots the same live pan);
- a per-layer view gate (`listenerViewFlags & layer_flags & 6`, plus `Multi.set_flags`
  bit0 requiring layer flag 0x20 to match the listener view bit, @ 0x75cd54) filters
  which layers fire — unported, and data-inert for JO: of 6464 JOX layers, 0 lack both
  view bits and only 8 are view-dependent (4 internal-only + 4 external-only, vehicle
  scope); 0 sets carry set_flags bit0;
- the set pitch compose `(member_pitch * (set_base + set_jitter)) >> 16` (@ 0x75ce9e..)
  is unity across ALL 5950 JOX sets (pitch_base 0xFFFF/0x10000, jitter 0 everywhere), so
  the reimpl's member-only pitch is data-exact.

**Occlusion (`Sound_ApplyOcclusionDistance @ 0x529970`; full witness re-pulled 2026-07-16 —
[render-occlusion-re.md](../render/render-occlusion-re.md) §6).** Two LOS raycasts
listener->source, target z lifted +0x2000 for both; ray 2 runs with a -0x8000 height offset
(the whole segment raised 0.5u). The inflation COMPOUNDS through one final add — `base =
min(d/8, 10u)`; ray 1 blocked -> `base = 2*base + 5u`; then `d += base` (ray 2 clear) or
`d += 2*base + 5u` (ray 2 blocked). Net: both clear `+base0`, exactly one blocked
`+2*base0+5u`, both blocked `+4*base0+15u` (the pre-2026-07-16 "each ray adds" reading was a
simplification). The LOS legs: terrain heightfield ray, skipped as clear when BOTH entities
are indoors (Flags 0x800000) `@ 0x53b0a0`; then the entity leg over the LISTENER's proximity
candidate slice where only def-type-5 (building-kind) candidates block (`allowAllTypes = 0`
pushed at both sound sites `@ 0x52999e/0x5299c3`). Applied in both the emitter mix
(`@ 0x528659`) and the positional one-shot path (`@ 0x527d95`).

Reimpl port (2026-07-10, `NovaSoundBank` + `NovaMissionAudio`; re-grilled 2026-07-11): the
witnessed curve, the two-radius model, member-0 selection, the time-of-day
slots/crossfade/stagger, and the loudest-8 budget are structural translations driven from
`NovaMissionAudio.tick`; voices are persistent `AudioStreamPlayer3D`s with
`ATTENUATION_DISABLED` (Godot must not attenuate on top — its inverse-distance curve
amplifying inside `unit_size` is what buried mission dialog under a +21 dB ambient wall once
the loop-region fix made the bed audible). The 2026-07-11 re-grill fixed four port
divergences to the witnessed forms: the emitter proximity arm now subtracts in Q16 before
truncating (`floor(min - d)`, was `min - floor(d)` — one whole unit off at fractional
distances); the crossfade volume byte is the rounded `(0xFFFF * blend + 0x8000) >> 24`
(was a floor, off by one at midpoints); the region intervals are open at the low cut (the
exact cut instant reads night at full blend); and the one-shot path gained the set cull
range + the no-falloff emitter-volume source, and dropped an unwitnessed `sound_profile`
slot fallback (zero envs-class JOX items carry the key — it could never fire on retail
data). Divergences D-SND-6..8 below; regression seams `sound_runtime_test.gd` (curve
integers + the Q16/rounding/no-falloff pins), `nova_mission_audio_test.gd` (mix budget /
slots / crossfade), and the `dialog_vs_ambient_probe.gd` bed-vs-dialog gate.

| ID | Ours | Original | Why / consequence |
|---|---|---|---|
| D-SND-6 | persistent per-marker `AudioStreamPlayer3D`s, paused/volume-driven by `NovaMissionAudio.tick`; a voice re-entering the mix RESUMES its loop position | transient emitter slots re-registered per tick; a drop-out's channel is STOPPED and a re-entrant reopens from the wave start (`AudioChannel_ResetByHandle @ 0x767160` / `AudioChannel_OpenSlotChecked @ 0x767060`) | reimpl architecture: Godot voices are cheap to keep; resume-vs-restart on a looping bed is inaudible. Whether a retail channel loops natively (AUD1 descriptor flag) or restarts per registration is an open follow-up (the DirectSound service layer was not walked). |
| D-SND-7 | **PORTED 2026-07-16** (reviewed 2026-07-17): `CollisionWorld::sound_occlusion_inflate` (libs/world/src/collision_los.cpp) + `terrain_raycast_los_clear` (libs/terrain_query) implement the compounding two-ray form; `NovaSimulation.sound_occlusion_distance_q16` feeds the emitter mix (`NovaMissionAudio.tick`, rays only for markers already audible at the raw distance) and the one-shot cull recheck + volume snapshot (`NovaSoundBank.play_oneshot_3d`). Authored marker ids, remote-fire ids, and the local-player sentinel now preserve source exclusion and the both-indoors terrain bypass; static emitters refresh blink state at this boundary. | compounding two-LOS-ray distance inflation (`Sound_ApplyOcclusionDistance @ 0x529970` — the corrected form above; terrain leg + building-only entity leg with the -0x8000 radius-slot reuse on ray 2) in both the emitter mix and positional one-shots | evidence: `collision` ctest sound-occlusion cases (both-blocked/one-blocked/clear/clamp), `collision` nearest-point bias, `terrain_raycast` LOS cases, and GUT ambient/one-shot provider wiring; residue = D-SND-9. |
| D-SND-9 | the entity-leg clip applies the raw radius to EVERY plane | flagged planes (BPLN flags byte nonzero) clamp the clip radius at 0 (`@ 0x538bd8-0x538e1b` — only flag-0 planes read 0.5u thin on ray 2) | plane flags aren't plumbed through the collision feed yet; the remaining difference is sub-0.5u in the ray-2 entity clip. The LOS terrain callback now reads the witnessed nearest 0.5u-quantized texel. |
| D-SND-8 | master fade ramp, underwater vol/pan halving, options SFX volume, and the bearing-byte pan map to reimpl territory (Ambient bus volume, Godot's spatial panner); doppler (emitter/listener velocity feed) unported | `g_SoundMasterFadeQ24 @ 0x85A3E4` (255/256 steady), `g_SoundListenerUnderwater @ 0x33429A8` halving @ 0x75ca7d, `g_SoundVolumeOption @ 0x24D20CC` per channel write, atan2 bearing pan @ 0x5289c0, `calculate_3d_sound_attenuation @ 0x527f60` doppler | reimpl playback/bus routing (not grillable address-by-address); the underwater duck and doppler are candidates once an underwater/vehicle pass needs them. |
| D-SND-16 | **PORTED 2026-07-28** (`libs/audio` `AmbientMixer` + the `NovaAmbientMixer` binding): marker eval/registration runs on the logic-tick clock through the witnessed `tick & 7` cohort walk (each placed marker every 8th tick), layers register into a faithful 767-slot transient table with TICK-unit keep-alives (marker default 10; vol-0 register clears), and the per-render-frame call is only the live-slot mix — lazy range cache, axis+euclid cull, one lazy LOS per raw-audible marker, member-0 two-radius volume, loudest-first ranking. `NovaMissionAudio` keeps stream resolution, decode-failure fallback, and the eight persistent voices (D-SND-6/8); hosts with no ticking runtime (editor idle) free-run an autonomous 62.5 Hz eval clock (the weather world-driven/autonomous split). The curve statics (`calc_distance_volume`/`emitter_layer_volume`/`crossfade_volume_byte`/`time_of_day_region`) moved to libs/audio; the GDScript seams delegate | registration and mix on split clocks (§driver cadence): the pool-2 `tick & 7` stagger @ 0x4c225a (attached emitters per tick), tick-unit lifetimes, the per-frame render-lane mix @ 0x521341 | evidence: `ambient_mixer` ctest (curve integer pins, cohort stagger, tick-lifetime expiry/revisit, region-flip overlap, same-set suppress, occlusion-once, ranking, vol-0 clear, autonomous clock); GUT `nova_mission_audio_test.gd` / `sound_runtime_test.gd` on the new seam. Measured A/B (ASH_I5A spawn, 143 markers, 10 s windows, same box): the world tick's audio leg 1.19 ms -> 0.21 ms avg per frame (p95 1.41 -> 0.29 ms); frame wall 10.4 -> 8.8 ms. Reimpl residues: candidate-id tie-break for deterministic membership (retail ties by slot order), the range cull compares reimpl-float axes (Q16 at the curve boundary), and min-only layers cull like retail (zero JOX layers are min-only). Was: the full marker x layer eval every render frame in GDScript — the measured 1.2-1.5 ms/frame F3 "Audio" row. |

**IDB changes (2026-07-10 session):** renamed `Entity_SpawnBoneEffect -> Entity_UpdateEnvSoundEmitter @ 0x4a8080`,
`Entity_CalcTerrainRegion -> Entity_CalcTimeOfDayRegion @ 0x408110`, `Env_GetTimeOfDayHoursQ16 ->
Env_GetTimeOfDayHoursQ16`, `register_effect_slot_entry -> SoundEmitter_RegisterSetLayers
@ 0x528340`, `sub_529270 -> SoundEmitter_Register`, `SoundProfile_FindLoadedByName ->
SoundBank_FindSetByNameAnyBank @ 0x5274f0`, `sub_529970 -> Sound_ApplyOcclusionDistance`,
`update_positional_sound_emitters -> SoundEmitter_UpdateAndMixTop8 @ 0x5284a0`, `AudioChannel_OpenSlotChecked ->
AudioChannel_OpenSlotChecked`, `BinkVideo_ResetState -> SoundProfile_ResetState @ 0x526de0`
(kong misnomers, all behavior-witnessed); globals `g_SoundEmitterSlots @ 0x24D66A8`,
`g_AmbientChannelHandles @ 0x24D6688`, `g_AmbientChannelEmitterIdx @ 0x24D6668`,
`g_SoundEmitterMixScale @ 0x24E089C`, `g_SoundVolumeOption @ 0x24D20CC`, `g_SoundMasterFadeQ24
@ 0x85A3E4` (+step @ 0x33429AC), `g_SoundListenerUnderwater @ 0x33429A8`, `listener_pos_y/z`,
`g_SoundListenerYaw @ 0x24D663C`, `g_SoundListenerVelX/Y/Z @ 0x24D6644..4C` (ex kong
`source_near_pos`); witness comments on the curve, the register, the mixer, the loader's
set+72 copy, and the occlusion helper.

**IDB changes (2026-07-11 re-grill session):** defined + named the WAC voice/console
handlers `Wac_PlayScriptedVoiceWave @ 0x4ed610` (ex undefined code at the wave table
entry) and `Wac_ConsolDebugMessage @ 0x4edbe0`; renamed `noop_stub ->
Audio_AdpcmDecodeNibbleStep @ 0x7bf250` (the kong "empty stub" label was wrong — it is
the register-convention IMA-ADPCM stepper inlined into the SBF stream split and the
4-bit wav decode); witness comments at `0x4ed610` (wave semantics), `0x4edbe0` (consol =
debug channel), `0x4edb50` (WAC text -> player chat feed, color -1/type 0x3A2),
`0x4a4767` (the pff-only music-pair reselect), `0x75cf88` (no-falloff one-shot volume
source), `0x766735` (AOA1 device-relative pitch ratio / 44100 device rate). IDB saved.

**IDB changes (2026-07-28 cadence session):** renamed `sub_43B400 ->
Audio_UpdateListenerFromView @ 0x43b400` (the render-cb listener leg: cinematic/fade
camera transform else `g_view_pos_x`, MP-session-gated); cadence witness comments at
`0x4a8080` (pool-2 stagger + tick-unit lifetimes), `0x5284a0` (`current_tick` time
base), `0x521310` (the "Game Loop" mode-table witness + misnomer note), and the class
table `0x82abc8` (layout + `def+0x158 -> entity+0x1C4` plumbing). Applied with
maintainer OK (same day): the curated misnomer `render_loading_frame @ 0x521310 ->
GameLoop_RenderFrame`, entry comment rewritten to the mode-table witness (the old
"loading frame / previous name confirmed correct" note was wrong). IDB saved.

## Verdict

**matching** — LWF container layout, DBF layout, member-selection modes, the selection RNG
(algorithm + seed + scaling), name-keyed case-insensitive set resolution, items.def soundloop
semantics, and the global bank-slot order are all engine-witnessed and implemented faithfully.

- dialog playback serialization (grilled 2026-06-15): the engine plays one dialog channel
  at a time (`Dialog_Register @ 0x44d980` queue, `Dialog_UpdatePlayback @ 0x44e470` gate);
  the reimpl reproduces this with a FIFO queue (D-SND-4) instead of firing every
  `PlayWavList` at once.
- WAC `wave`/`pwave` scripted voice (D-SND-5): routed to a single interrupting reimpl voice
  channel; the positional `SSNwave`/`SSNradio` family remains a tracked follow-up.
- divergence (bank scope / D-SND-1): merged chain vs dialog-scoped co-named bank — accepted.
- divergence (coverage / D-SND-2): CLOSED 2026-07-05 — expansion bank slots load in slot order off the runtime mount's expansion.
- divergence (strictness / D-SND-3): parser rejects out-of-range single_index, >8 counts, bad
  magic, nonzero trigger/id-def tables that the engine tolerates or ignores — deliberate
  authoring-side strictness.
- placed ambient markers (grilled 2026-07-10; re-grilled 2026-07-11 with fresh
  decompiles): the `envs` time-of-day slot pick, the two-radius quadratic distance curve,
  member-0 selection, and the loudest-8 mix budget are witnessed and ported
  (`Entity_UpdateEnvSoundEmitter @ 0x4a8080`, `SoundEmitter_UpdateAndMixTop8 @ 0x5284a0`,
  `SoundBank_CalcDistanceVolPan @ 0x75ca20` — the curve is bit-exact including the
  `^ 0xFFFF` inverse and the 64-bit `>> 32`). The re-grill fixed four port forms (Q16
  proximity subtraction, rounded crossfade byte, open-low region cuts, one-shot
  no-falloff/set-cull) and removed the unwitnessed `sound_profile` fallback; divergences
  D-SND-6 (reimpl voice lifecycle), D-SND-7 (ported occlusion; D-SND-9 plane-flag residue),
  D-SND-8 (reimpl-mapped
  globals/pan/doppler — now explicitly including the wave channel's
  `(voiceVolume * 0xD2 + 0x80) >> 8` option fold and the dead-in-retail
  `g_SoundEmitterMixScale`).
- ambient driver cadence (witnessed 2026-07-28): the split clock is pinned — marker
  eval/registration every 8th 62.5 Hz tick per placed marker (pool-2 `tick & 7`
  stagger in `Entity_UpdateAllEntities @ 0x4c2100`; per tick for attached emitters),
  keep-alive lifetimes in TICKS (the old ms reading corrected), the Top8 mix once per
  render frame on a `current_tick` clock from the "Game Loop" mode render callback
  (`0x521310`), listener per frame. The cadence-faithful port landed the same
  day: `libs/audio` `AmbientMixer` (staggered eval + slot table + live-slot mix,
  curve statics included) with `NovaMissionAudio` reduced to stream resolution +
  the persistent voice binds — D-SND-16 PORTED (the `ambient_mixer` ctest and the
  GUT audio suites pin it).
- one-shot view gating and set-pitch compose (witnessed 2026-07-11, unported,
  data-inert): the `& 6` view-flag gate and `set_flags` bit0 filter fires on 8 of 6464
  JOX layers (vehicle-view scope) and 0 sets; the set pitch compose is unity across all
  5950 JOX sets. Port when a vehicle/view pass needs them.
- unknown: the `nightshot`/`duskshot`/`dawnshot` one-shot marker consumer (parse
  @ 0x49fdee witnessed; the runtime path is unwalked — such markers are silent in ours).
- unknown: the menu-side bank collection semantics and `.pwf` packed-wave banks (no JO assets
  ship one) — deferred to the menu-slice grill.
- unknown: whether an ambient channel loops the wave natively (AUD1 flag) or restarts per
  registration — the DirectSound channel service layer (`sub_766AA0` queue consumers) is
  unwalked; folded into D-SND-6.
- ground vehicle movement sounds (witnessed + ported 2026-07-29): the
  `Entity_UpdateVehiclePhysics` occupant gate, `Entity_ProcessMovementSoundEffects
  @ 0x5294a0` lanes 0/10/20, exact gain/pitch/gear math, direction latch, and
  claimant detach clear/stop are D-SND-17. `Soundloop_5..7` consumers remain
  unknown and are not claimed by the ground port.
- the sound-profile system (witnessed + ported 2026-07-17): SndProf.def parse,
  the 51-slot table, item binding (incl. `sound_profileFemale`), the slot
  accessor, and the infantry consumers (footsteps by surface, SSAudio foley,
  landing pair, death scream night gate) are engine-witnessed and ported
  (D-SND-10..15; ctests `sound_profile` + `slot_sound`); ground slots 31/32 are
  now consumed by D-SND-17, while slots 30/33-50 and the composite entity-type
  path remain vehicle/P2b scope.
- follow-up: rename the `libs/lwf` `Multi.target_id` field (and its NovaLwfData/ONED
  exposures) to its witnessed one-shot-cull-range meaning; the ONED sound workspace shows
  it as "(id N)" today.
