# Mission loading screen — reverse-engineering record

Validation record for the mission loading screen (`godot/engine/ui/nova_loading_screen.gd`,
progress wiring in `godot/engine/world/game_world.gd` + `godot/engine/mission/mission_object_placer.gd`,
shell lifecycle in `godot/game/main_game.gd`) against the original engine as witnessed in IDA
Pro. Binary: retail **Jointops.exe** (IDB `Jointops.exe.kong.i64`). All addresses below are
that binary's. This file is the committed home for the divergence catalog that code comments
cite as `docs/interface/loading-screen-re.md (D-LOADSCR-…)`.

Researched 2026-07-12 (engine-research session; port landed the same session).

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| Sidecar background resolution (`<missionbase>.pcx` → `loadscrn.pcx`) | **MATCHING** (ported) | witness map below; GUT `godot/tests/game/loading_screen_test.gd` (sidecar name, fallback, custom flag) |
| MP session text block (title/mission/game-type band + server message) | **MATCHING** (ported; glyph renderer approximated, D-LOADSCR-2) | layout constants + alignment enum + color tags witnessed; GUT gametype-key + SP/MP-split tests |
| Progress bar (geometry, colors, smoothing, throttle) | **MATCHING** (ported) | exact integer arithmetic ported; GUT smoothing + fill-span tests |
| Present pump during the blocking load | reimpl code (force_draw analog of the witnessed pump) | cadence witnessed at `LoadingScreen_UpdateAndPresent @ 0x586be0`; `RenderingServer.force_draw` + `queue_redraw` stand in for BeginScene/Present |
| Load-flow case handling (SP / host / success / failure / return) | **MATCHING** (ported) | the case matrix below (`Game_StartMission @ 0x524360`): screen resident through the load, released at the end, failure/abort → menu; GUT `main_game_lifecycle_test.gd` (load, return, reload, failed-load rollback) + `game/loading_screen_test.gd` (SP-vs-session `load_info` split) |
| Joiner spawn-gate hold | **MATCHING** (D-LOADSCR-3 fixed 2026-07-24) | local `world_loaded` leaves the loading presentation raised; `ClientRuntime` keeps pumping while hidden and `GameWorld.join_admission_ready` releases it only at authoritative in-match admission, or transitions it to DEATH when the host requests a player-paced deploy pick |
| ESC / disconnect abort during load | **DIVERGENT** (D-LOADSCR-7) | `Client_CheckDisconnectOrEscDuringLoad @ 0x520270` aborts to `Post Menu`; our SP/host load is one synchronous call the SceneTree cannot interrupt — no reachable window on the synchronous path |
| SP start-mission splash (`newarow1.tga` + START_MISSION + `LT_Continue`) | **not yet ported** (D-LOADSCR-4) | witnessed at `show_start_mission_splash @ 0x520820`; the `!is_multiplayer_session && g_loadscreen_has_custom_bg && !is_in_session` gate (decompiler ~1039); follow-up |
| Boot loading screen (`loading.pcx`) | confirm-only (separate boot-time variant) | `Game_ShowLoadingScreen @ 0x4a5420` (already rowed in `docs/required-resources.md`) |

## The sidecar rule

A mission's loading image is the mission file's basename with the extension replaced by
`.pcx` — `00TRg.bms` → `00trg.pcx` (VFS-case-insensitive) — probed with
`FileSystem_FileExists`, falling back to the literal `loadscrn.pcx`
[orig: render_loading_screen @ 0x521d10 — copy of the mission name, `PathRemoveExtension
@ 0x617510`, `Path_ReplaceOrAppendExtension(path, "pcx") @ 0x53c780`, probe @ 0x521db5,
fallback @ 0x521e20]. The name source is `g_map_file_name` for the host and single player,
and the client's received `MISSIONFILENAME` session variable (`g_sessionvar_mission_file_name
@ 0x24c1178`) when joining. A global custom-background flag (`g_loadscreen_has_custom_bg
@ 0x24d4dfd`) records which case hit; it later gates the SP start-mission splash. The
`reason == 8` branch in the fallback path is behaviorally inert — both legs end at
`loadscrn.pcx` (compiler artifact, not logic).

The retail images (both the per-mission briefing art and the stock `loadscrn.pcx`) are
800×600 PCX; SP training-mission images carry their briefing text baked into the art —
which is why the original draws **no dynamic text at all in single player**.

The wider sidecar family sharing a mission basename (witnessed elsewhere): `<base>.bin`
(mission text, `TextResource_LoadMissionTextBin @ 0x51ed90`), `<base>.wac` (script,
`WacScript_InitAndLoad @ 0x4f91f0`), `<base>.LWF`/`<base>.DBF` (mission sounds/dialog,
audio records), `<base>.mis`, `<base>.til` — plus save files built from
`g_map_file_name` (`SaveFile_BuildFilename @ 0x4aad60`).

## Witness map

### Composition — render_loading_screen @ 0x521d10

Builds the composited screen once per call: picks the background (sidecar rule above), loads
it via `Texture_LoadPCXFromPFF32 @ 0x56ea30` (a 0x30+0x100-byte slot: +0x20 w, +0x22 h,
+0x28 pixel ptr, +0x2C name[256]); a load failure returns with **no screen at all**. Not in
a session: hands the texture straight to the effect (image only). In a session: loads
`Arials18.fnt` (spacing arg 120) and `Arial22.fnt` (spacing arg 0) as `CGameFont`s
(`CGameFont_Init @ 0x673a60`) pointed **at the texture surface** — text is composited into
the image pixels, then the composite is stretched. The per-call stage argument its callers
push (2..7, 16) is never read. On the authority it first refreshes
`Server_BuildStatusReport @ 0x530a60` (into `g_session_status @ 0x24e3e88` — a different
struct from the sessionvar text cluster).

Text layout (texture space, 800×600 retail art):

- Top band, rect (21, 29) → (right, 500) where right = **661** with a custom background,
  **782** with the stock one [orig: @ 0x521ec4]. Three strings drawn into the same band via
  `render_draw_wrapped_text_block_ex @ 0x580eb0` with alignment 3/4/5 = left/center/right
  [orig: the alignment switch @ 0x58106c/0x581071]: **server name** (left), **mission
  name** (center), **game type** (right), all Arial22, all prefixed `<cFFFFFF>` (white).
- Server-message block, gated on a non-empty `CUSTOMTEXT`: label =
  gametext `LoadingText`/`LT_SERVERMSG` (fallback literal "Message from Game Server")
  formatted `<c80E0FF>%s:\r\n<cFFFFFF>` at (0.02·w, 0.87·h); body at (0.02·w, 0.90·h);
  box right/bottom edge (0.98·w, h). Constants are doubles at `0x7D01A0/0x7D0198/0x7D0190/0x7C4878`.
- The wrap routine breaks at the last space, advances one line cell, stops when the next
  line would pass rect_bottom [orig: 0x580eb0].

### Text provider — HUD_GetLoadingScreenTextByGameType @ 0x51f300

Maps `g_GameType @ 0x24d2128` to a `LoadingText` string key, looked up via
`TextResource_FindEntryBySectionAndKey @ 0x75d250` against gametext:

| g_GameType | key |
|---|---|
| 0 | `LTGT_DM` |
| 0x10000 | `LTGT_TDM` |
| (& 0xFFFDFFFF) == 0x10020 | `LTGT_COOP` |
| 0x00001 | `LTGT_KOTH` |
| 0x10001 | `LTGT_TKOTH` |
| 0x90002 | `LTGT_SD` |
| 0x10002 | `LTGT_AD` |
| 0x10004 | `LTGT_CTF` |
| 0x10008 | `LTGT_FB` |
| 0x10010 | `LTGT_AAS` |
| 0x50010 | `LTGT_CAC` |
| anything else | none (empty line) |

Outputs three static buffers (renamed `g_loadscreen_title_buf/mission_buf/gametype_buf
@ 0x24d5f28/0x24d5d28/0x24d5b28`): title ← `g_sessionvar_server_name`, mission ←
`g_sessionvar_mission_name`, game type ← the lookup, each `<cFFFFFF>`-prefixed when color
tags are requested.

### Session variables — parse @ 0x5202f0 / serialize @ 0x523620

`parse_server_session_variables @ 0x5202f0` (client receive) and
`serialize_mission_info_to_datastream @ 0x523620` (host send — which also mirrors every
field into the same globals) move a length-prefixed KV stream:

| key | size | global | loading-screen use |
|---|---|---|---|
| `SERVERNAME` | 32 | `g_sessionvar_server_name @ 0x24c1400` | title line (host source: `g_server_name_str @ 0x24d1fc4`) |
| `MISSIONNAME` | 64 | `g_sessionvar_mission_name @ 0x24c13c0` | mission line — host source: mission text .bin `[info]/title` via `g_TextMission`; coop game types use the override string `dword_24D1FA4`; an empty title falls back to `dword_A761D4` |
| `GAMETYPE` | u32 | `g_sessionvar_game_type @ 0x24c13b8` | (the render path reads `g_GameType` for the key) |
| `CUSTOMTEXT` | 512 | `g_sessionvar_custom_text @ 0x24c11b8` | the server-message body (host source: `g_server_custom_text @ 0x24d21c4`) |
| `MISSIONFILENAME` | 64 | `g_sessionvar_mission_file_name @ 0x24c1178` | the client's sidecar basename |
| `EXP_FANFARE` | u16 | `g_sessionvar_exp_fanfare @ 0x24d5a10` | (not loading-screen) |

### Effect lifecycle — LoadingScreen_{Ensure,Release,DrawEffectFullscreen} @ 0x586b20/0x586b80/0x586ba0

The composited texture becomes a CEffect held in the render manager's slot
(`off_840960`+0x28). `LoadingScreen_EnsureEffect @ 0x586b20` **first releases the previous
effect** (virtual call = vtbl slot 13 = `LoadingScreen_ReleaseEffect @ 0x586b80`), then
creates a new one from the slot's pixels/dims — so per-stage re-renders update the visible
text. `LoadingScreen_DrawEffectFullscreen @ 0x586ba0` draws it at rect
**(0, 0, backbuffer w, backbuffer h)** — a full stretch, no aspect preservation — with
modulate `0xFF7F7F7F` (the fixed-function MODULATE2X neutral). `Game_StartMission` releases
the effect at load end (@ 0x525d52), after the optional SP splash.

### Present pump — LoadingScreen_UpdateAndPresent @ 0x586be0 (vtbl slot 15)

Pumps window messages, handles a lost device, and only draws when the effect exists. Redraw
gate: **100 ms elapsed, OR the reported progress changed, OR the displayed value trails the
reported one** (the trailing case redraws unthrottled). Each draw advances the displayed
value by exactly +1, allowed up to **reported + 10**, capped at 100 — the bar creeps ahead
of the last reported value as a liveness animation while a stage grinds. Draw pass:
BeginScene → text overlay → the fullscreen effect → the bar → (optional seven-segment
percentage, `Render_DrawSevenSegmentDisplay @ 0x5d4a20`, only under
`g_ShowLoadBarCommandLineArg @ 0x24d1df0`) → Present.

Bar geometry: virtual **1024×768** overlay coordinates x=368, y=732, w=286, h=15 (scaled to
the real viewport via `Viewport_ScaleToVirtualCoords @ 0x5d2b20`); fill color override
**0xEB0000** (red).

### Bar primitive — draw_progress_bar_0 @ 0x5d4c40

Layered filled rects with 1-real-pixel insets: black outer frame spanning (w+6, h+6), gray
`0xC0C0C0` frame, black track, then the fill after one more inset. Fill right edge =
`pct·(w+2)/100 + left + 2` (integer divide), clamped to the track, minus the final inset —
so 100% fills exactly `w`. (Without the override color the primitive's own ramp is green
≤90 / yellow ≤100 / red ≤150 / flashing red above — the loading screen always passes the
red override.)

### Progress schedule — Game_StartMission @ 0x524360 (+ connect path)

Witnessed reported values in order: 2, 3, 4, 6, 20, 7/26 (constant values pumped from
inside the two model-load loops @ 0x524d9c/0x524e09 and 0x524f32/0x524fe0), 26, 28, 30, 31,
34, 35, 36, 37, 38, 39, 40, 41, 45, 50, 60, 62..69 (slot++ per subsystem inside
`CRenderManager_ShutdownAllSubsystems @ 0x587000`, base 0x3E), 70, 90, 95, 100.
`Game_LoadTerrainDuringConnect @ 0x520710` reports 10 and 25 on the MP join path.
`render_loading_screen` itself is re-invoked at nine points across the load (each rebuild
recomposites the text).

### The load-flow case matrix — Game_StartMission @ 0x524360

`Game_StartMission` is the single entry for **single-player and every authority
(host) load**; the **joiner** enters the same function and diverges into a
network-wait sub-path. The whole body runs with the loading screen resident —
`LoadingScreen_UpdateAndPresent @ 0x586be0` is pumped at each progress value and
`render_loading_screen @ 0x521d10` recomposites at nine points — and the effect
is released (`LoadingScreen_ReleaseEffect @ 0x586b80`) only at the very end,
after the optional SP splash. Every case:

- **Single-player / host** (the straight-line path): load terrain, models,
  weapons, anims, HUD, subsystems, spawn the player, then the final release
  `@ 0x525d45` (decompiler line ~1045). The world is built entirely behind the
  loading screen; nothing is presented until the release. Host is a session
  (`g_napi_np_ctx.is_in_session`), single-player is not.
- **Joiner** (the `else` sub-path `@ 0x524..`, decompiler lines ~620–654): after
  the initial present, two blocking network waits bracket the terrain load:
  1. `NapiClient_WaitForDisconnect @ 0x42cb20` — the connect handshake. Return 1
     → nav-push `GameLoop`, return ("aborted 1"); return 2/3/4 → `reason = 1`,
     nav-push `Post Menu` (`scene_entry`), return ("aborted 2").
  2. `Game_LoadTerrainDuringConnect @ 0x520710` (reports 10, 25), then
  3. `NapiClient_WaitForGameStart @ 0x42cc10` — the SPAWN gate: pumps net + input
     and returns 1 only when `g_spawn_success_gate @ 0x24c1928` is set (S2C 0x1D,
     net-re §5.2), 3 on ESC/`g_loading_cancel_flag`, 4 on
     `g_loading_timeout_flag`, 0 on reconnect. The caller nav-pushes `GameLoop`
     on the spawn leg and `Post Menu` (`reason = 1`) on the cancel/timeout legs.

  So the joiner **holds the loading screen through the connect handshake AND the
  spawn gate**, entering the game only when the server admits the spawn; any
  disconnect/ESC/timeout at either wait returns to the menu.
- **ESC or disconnect at any point during the load**:
  `Client_CheckDisconnectOrEscDuringLoad @ 0x520270` (called at four model/asset
  points — decompiler "aborted 5..8") pumps window messages, and on `reason == 2`
  (a recorded disconnect) or a `27`/ESC keypress sets `reason = 1`,
  `g_loading_cancel_flag = 1`, nav-pushes `Post Menu`, and returns 1 → the load
  aborts to the menu.
- **Any load-step failure** (mission too large, missing asset, etc.): the same
  early return with `reason = 1` / `Post Menu`.

**Port mapping (the case handling in `main_game.gd`)**:

| Case | Original | Port |
|---|---|---|
| SP start | `Game_StartMission`, not-in-session, background-only screen | `_on_start_requested` → `load_mission`; `load_info` carries only `mission_file` (no session text) — MATCHING |
| Host start | `Game_StartMission`, in-session, session text composited | `_on_lan_host_start_requested` / `_on_novaworld_host_requested` → `load_mission_as_host`; `load_info` carries the SERVERNAME/MISSIONNAME/GAMETYPE/CUSTOMTEXT band — MATCHING |
| Joiner | two-wait hold to the spawn gate, then reveal | `_on_lan_join_requested` → `load_mission_as_joiner`; `world_loaded` keeps the loaded world hidden while its runtime pumps, then `join_admission_ready` reveals at in-match or `join_deploy_pick_required` replaces loading with DEATH — **MATCHING** (D-LOADSCR-3 fixed) |
| Load success | release effect at end, reveal game | `_on_world_loaded` drops the screen immediately for SP/host; a joiner waits for the authoritative edge above — MATCHING |
| Load failure / abort | `reason = 1`, nav-push `Post Menu` | `_on_world_load_failed` → `_teardown_world_to_menu` — MATCHING |
| ESC / disconnect DURING load | `Client_CheckDisconnectOrEscDuringLoad` → abort to menu | our SP/host load is a single synchronous call the SceneTree cannot interrupt; ESC is swallowed while `_world_load_pending` — **D-LOADSCR-7** (unreachable window, not a behavioral loss on the synchronous path) |
| Return to menu (pause → abort) | nav-push `Post Menu` | `_on_return_to_menu` → `_teardown_world_to_menu` — MATCHING |

### SP start-mission splash — show_start_mission_splash @ 0x520820 (not yet ported)

At the end of a **single-player** load with a custom background
(`g_loadscreen_has_custom_bg` gate @ 0x525d38): loads `newarow1.tga`, plays the
`START_MISSION` sound set, loops presenting [loading-screen effect background → text
overlay (`LoadingText`/`LT_Continue`) → the arrow quad scaled in 800×600-relative space]
until any input or the sound completes. Also reachable at the pre-spawn gate via the start
key (`Input_HandleSpecialKeys @ 0x49c5c0`, key `dword_B3B744`, @ 0x49c887).

## Port notes (the structural translation)

- `NovaLoadingScreen` (godot/engine/ui/nova_loading_screen.gd) draws the texture stretched
  over the display and the MP text in image space under the image's scale transform — the
  same net composite the original gets by rendering glyphs into the texture then
  stretching. The bar arithmetic, colors, throttle and creep are ported integer-exact.
- The blocking-load present pump maps to `DisplayServer`-guarded
  `RenderingServer.force_draw()` after `queue_redraw()` — the reimpl-side analog of
  Game_PumpWindowMessages + Present.
- `GameWorld.load_progress` emits the witnessed anchor values at our stage boundaries
  (2 → 6 → 26 → 41 → 70 → 90 → 95 → 100), and `MissionObjectPlacer` pulses the constant
  stage value from inside its per-model loops — the original's exact pump shape
  (constant-per-stage + creep), on a coarser stage set (D-LOADSCR-1).
- Shell lifecycle (`main_game.gd`): mount on `_begin_world_load`, dismiss on
  `world_loaded`/`load_failed`/return-to-menu; the world + HUD stay hidden until the load
  lands (the original presents only the loading screen during the load).

## Divergence catalog

| ID | Ours | Original | Why / consequence |
|---|---|---|---|
| D-LOADSCR-1 | 8 stage-boundary progress values + per-model pulses at the stage constant | ~30 call sites incl. per-subsystem slot++ ticks (62..69) and separate 7/26 loop constants | our load pipeline decomposes differently; the value set and the pump mechanism (constant + creep) match, granularity doesn't. Cosmetic-only. |
| D-LOADSCR-2 | Godot FontFile view of the .fnt fonts, drawn under the image scale transform; Godot line metrics + word wrap | CGameFont glyph composite into the texture, `sub_674740`/`sub_6741C0` spacing params (120 small / 0 large, semantics unwitnessed) | glyph-exact spacing is the standing CGameFont follow-up shared with [hud-re.md](hud-re.md); positions/alignments/colors/wrap box are witnessed and ported |
| D-LOADSCR-3 — **FIXED 2026-07-24** | `world_loaded` completes local assets but does not release a joiner's presentation. `ClientRuntime` continues the real session under the hidden world; `NovaSimulation::is_joined_in_match` / `is_join_deploy_pick_pending` feed edge-triggered `GameWorld.join_admission_ready` / `join_deploy_pick_required`, and `MainGame` releases only at one of those authoritative boundaries. The same deploy edge rearms after death without replaying the loading screen | retail holds through TWO blocking waits after the local load — `NapiClient_WaitForDisconnect @ 0x42cb20` (connect handshake) then `NapiClient_WaitForGameStart @ 0x42cc10` (spawn gate `g_spawn_success_gate @ 0x24c1928`, S2C 0x1D — net-re §5.2) — revealing on the spawn leg or entering the DEATH picker when a spawn choice is owed | real-UDP `main_game_lifecycle_test.gd::test_join_loading_stays_raised_until_authoritative_admission` proves the old early-reveal boundary and the fixed release |
| D-LOADSCR-4 | SP start-mission splash not ported | `show_start_mission_splash @ 0x520820` (arrow + START_MISSION + LT_Continue) | follow-up; the loading screen itself is unaffected |
| D-LOADSCR-5 | seven-segment numeric percentage not ported | drawn only under the `g_ShowLoadBarCommandLineArg` command-line flag | debug-only surface; revisit if the launch-flag work wants it |
| D-LOADSCR-6 | background drawn unmodulated | effect draw modulate `0xFF7F7F7F` = MODULATE2X neutral | net-identical color; documented so nobody "fixes" a half-bright that isn't there |
| D-LOADSCR-7 | ESC / disconnect during the SP/host **map load** cannot abort it — that load is a single synchronous `operation.call()` the SceneTree cannot interrupt; ESC is swallowed for its duration | `Client_CheckDisconnectOrEscDuringLoad @ 0x520270` polls at four asset points and aborts to `Post Menu` (`reason = 1`, `g_loading_cancel_flag = 1`) on ESC/disconnect | no reachable interruption window on a synchronous host load — the original's blocking `.bms`/model load is likewise uninterruptible except at its network-wait points. Scope corrected 2026-07-25: this row covers ONLY the synchronous map load. Both joiner waits are coroutines that await `process_frame` every iteration, so both are interruptible and both now honour ESC — the pre-load connect/session wait via `GameWorld.cancel_join_preload()` and the post-load admission tail via `GameWorld.cancel_join_admission()` (previously ESC was consumed and did nothing there for up to `JOIN_CONNECT_TIMEOUT_MS`). Revisit if the map load is ever chunked across frames. |

## Follow-ups / unknowns

- Which wire message invokes `parse_server_session_variables @ 0x5202f0` (its callers were
  not traced this session) — likely the mission-info leg of the load sequence; pin it from
  the net side.
- `dword_24D1FA4` (the coop `MISSIONNAME` override string): writer unknown.
- `dword_A761D4` (empty-title fallback): assumed the mission-header title (cf. net-re §5.5
  title-cased basename note); unverified.
- `sub_6741C0(font, 120)` spacing semantics (CGameFont) — shared follow-up with hud-re.md.
- D-LOADSCR-4 (SP splash) above.

## IDB changes made during the session

Renames (anchored, all previously auto-named): `LoadingScreen_EnsureEffect @ 0x586b20`,
`LoadingScreen_ReleaseEffect @ 0x586b80`, `LoadingScreen_DrawEffectFullscreen @ 0x586ba0`;
globals `g_sessionvar_mission_file_name @ 0x24c1178`, `g_sessionvar_custom_text @ 0x24c11b8`,
`g_sessionvar_game_type @ 0x24c13b8`, `g_sessionvar_mission_name @ 0x24c13c0`,
`g_sessionvar_server_name @ 0x24c1400`, `g_sessionvar_exp_fanfare @ 0x24d5a10`,
`g_loadscreen_has_custom_bg @ 0x24d4dfd`, `g_loadscreen_title_buf @ 0x24d5f28`,
`g_loadscreen_mission_buf @ 0x24d5d28`, `g_loadscreen_gametype_buf @ 0x24d5b28`,
`g_server_custom_text @ 0x24d21c4`. Entry comments appended at 0x521d10 (sidecar rule +
layout), 0x586be0 (bar geometry + creep + schedule), 0x586b20 (release-then-create),
0x587000 (slot++ ticks are the progress bar, not profiling), 0x523620 (KV source map).
IDB saved.
