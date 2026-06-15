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
| Multi dword 18 (`target_id`) | authoring-tool id; runtime resolves by NAME only | dead (no read in play path) |
| Multi dword 19 (`set_flags`) | bit0: require layer flag 0x20 to match the listener view bit | @ 0x75cd54 |
| Playlist u16 @4 (`inner_distance`) | full-volume / proximity-pan radius (tool column "Falloff") | @ 0x75cf5c |
| Playlist u16 @6 (`max_distance`) | outer audible fade radius (tool column "Min distance") | @ 0x75cf1a..0x75cf55 |
| Playlist dword @12 | disk scratch; runtime random-seq cycle anchor | @ 0x75cd9b |
| Sndparm @4 (`pitch_scaled`) | member pitch base, Q16 | @ 0x75c109 |
| Sndparm @8 (`random_pitch_scaled`) | additive jitter `(range * rand8) >> 8` | @ 0x75cedc |
| Sndparm @12 (`volume`) | member volume 0..255 | @ 0x75cf25 |
| Sndparm @16 (`clamp_volume`) | ceiling on the distance-scaled volume; also the pan amplitude | `SoundBank_CalcDistanceVolPan` @ 0x75ca65 |

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
machine (the stream is pinned by `tests/audio/sound_selector_test.cpp`); the host does not
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
- **D-SND-2 (expansion banks):** `<exp>L.lwf` / `<exp>.lwf` are not loaded yet (no expansion
  name is plumbed into the world); host follow-up.
- **D-SND-3 (strictness):** parser rejects what the engine silently tolerates (see above).

## DBF dialog banks ('DLG0')

`DialogManager_LoadFromFile @ 0x44e650`: 28-byte header (magic `'DLG0'` = 0x30474C44 checked
@ 0x44e6f7; version and header_size are never checked), `id_def_count` @ +12 (32-byte
"DlgMgr IDDEFS" records @ +16's offset — zero in every JO mission bank), group count @ +20 and
groups offset @ +24. Each dialog is a 52-byte group record followed by its 68-byte line
records, re-read as one `68*n + 52` blob @ 0x44e79d. A mission `PlayWavList` action carries a
dialog id; the group's lines name the sound definitions (sets) to play from the dialog bank
(`ActionSlot_PlaySound @ 0x4010c0` for the positional action path).

## Dialog playback is serialized — one channel at a time (grilled 2026-06-15)

A `PlayWavList` mission action does not play immediately; it **enqueues** a dialog,
and a per-frame updater plays the queue **one audio channel at a time**:

- `EventAction_Dispatch @ 0x4542e0` (PlayWavList case @0x454461) -> `Dialog_PlayByIndex
  @ 0x527ae0` formats the id as `"dlg%03i"` -> `Dialog_PlayByName @ 0x44d9f0` (matches the
  loaded dialog group by name, locale-suffixed first) -> `Dialog_Register @ 0x44d980`.
- `Dialog_Register` appends to a 256-entry queue (`dword_A89600` / count `dword_A895F8`)
  and claims one of 16 active slots (`dword_A8A248[]` data ptr, `dword_A8A24C[]` line/entry
  index, `dword_A8A250[]` countdown timer; active count `dword_A8A244`).
- `Dialog_UpdatePlayback @ 0x44e470` runs each frame. For each active slot it only loads
  and advances to the next clip when **no dialog channel is currently sounding** — the gate
  `!dword_A895FC || !AudioChannel_ValidateHandle(dword_A89DFC[dword_A895FC])` (@0x44e53a).
  `dword_A895FC` is the single live dialog channel index, set by `Dialog_LoadAudioClip
  @ 0x44dcc0`. A dialog *group* advances through its line records (entry index `+28` count)
  one line per free-channel cycle, with an inter-line countdown derived from the line byte
  `+53`. So **dialog never overlaps dialog**: lines within a group and across queued groups
  play strictly sequentially.
- The pre-mission pass (`EventTrigger_UpdateAllWithFlag2 @ 0x454dc0`) dispatches actions
  through the **same** `EventTrigger_UpdateEntry @ 0x454c30` as normal events, so a
  PreMission `PlayWavList` *is* registered at mission start in the original — it just plays
  back serially through the one dialog channel, not all at once.

### Divergence

| ID | Ours | Original | Why / consequence |
|---|---|---|---|
| D-SND-4 | `NovaMissionAudio.play_dialog` **enqueues** the resolved line set-name(s) and plays them one at a time, starting the next on the previous voice's `finished` (`_dialog_queue` + `spawn_oneshot_2d`) | one dialog channel, `dword_A895FC`-gated, advanced by `Dialog_UpdatePlayback` | host-side serialization that reproduces the observable behavior (no dialog overlap). The prior host played every drained `dialog` effect immediately and non-blocking, so a mission's PreMission/early `PlayWavList` actions blared simultaneously at t=0. We do not model the 16-active-slot table or the per-line countdown timing (host presents on stream `finished`); the *id -> "dlg%03d" -> .DBF group lines* resolution matches the engine's `"dlg%03i"` path. |

## items.def marker sounds

`ItemDef_ParseProperty @ 0x49eb00` parses the marker-item sound keys: `soundloop_1..7`
(prefix match @ 0x49fec4; the 7-slot count matches the engine's `Soundloop_1..7` sound-type
name table @ 0x7d0788) and the time-of-day one-shots `nightshot` / `duskshot` / `dawnshot`
(@ 0x49fdee). A "snd:" marker entity resolves item_id -> soundloop set name -> Multi by name in
the loaded banks. Entity-attached sounds use a separate composite-name path
(`SoundProfile_FindByEntityAndType @ 0x528180`, `"<EntityDefName>_<SoundType>"`).

## Verdict

**matching** — LWF container layout, DBF layout, member-selection modes, the selection RNG
(algorithm + seed + scaling), name-keyed case-insensitive set resolution, items.def soundloop
semantics, and the global bank-slot order are all engine-witnessed and implemented faithfully.

- dialog playback serialization (grilled 2026-06-15): the engine plays one dialog channel
  at a time (`Dialog_Register @0x44d980` queue, `Dialog_UpdatePlayback @0x44e470` gate); the
  host reproduces this with a FIFO queue (D-SND-4) instead of firing every `PlayWavList` at
  once.
- divergence (bank scope / D-SND-1): merged chain vs dialog-scoped co-named bank — accepted.
- divergence (coverage / D-SND-2): expansion bank slots not loaded — host follow-up.
- divergence (strictness / D-SND-3): parser rejects out-of-range single_index, >8 counts, bad
  magic, nonzero trigger/id-def tables that the engine tolerates or ignores — deliberate
  authoring-side strictness.
- unknown: the menu-side bank collection semantics and `.pwf` packed-wave banks (no JO assets
  ship one) — deferred to the menu-slice grill.
