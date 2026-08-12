# HUD overlay — reverse-engineering record

Witness record for the Joint Operations in-game **HUD overlay** render pipeline.
Binary: retail **Jointops.exe** (IDB `Jointops.exe.kong.i64`, imagebase
`0x400000`). All addresses below are that binary's.

Implementing code: `engine/formats/def` (`hudpos.def` / `weapon.def` parsing);
`engine/runtime/hud` — `hud_math.h` (the policy math), `game_font.h` (the
CGameFont text engine), and `hud_frame.h` (`HudFrameCompiler`: the whole
witnessed element walk, fade/flash/message state, and per-frame draw-list
compile — the ADR 0033 R2 home of every element previously ported in
`game_hud.gd` + the `hud_*.gd` helpers, which the 2026-08-09 cutover deleted);
the `HudPos`/`HudOverlay` GDExtension bindings (`godot/src/hud/` — the overlay
keeps texture upload and draw-list rasterization only); the shell-side
`godot/game/world/hud_sights_card.gd` (per-row blend child controls) fed with
the overlay by `godot/game/world/game_hud_presenter.gd`; and the ONED preview
workspace (`godot/modtools/hud/`, over the same `HudPos` statics plus the kept
`godot/game/ui/hud_text.gd` FontFile preview helper). The 2026-06-22 session witnessed the
core pipeline read-only; the 2026-07-09 session witnessed the weapon-coupled
elements and ported them (the weapon FSM of net-re §5.62 supplies the live
clip/reserve/ADS state); the 2026-07-11 re-grill (the extraction-train slice
gate) fresh-decompiled every ported function, fixed seven port divergences
in-source (ALPHAFADE atof, positional 4-field parse, stance frame-0 shared
scale + gates, capacity-1 reserve fold, triggered-text white, mission-bin
exists-gate, divisor byte wrap) and minted D-HUD-9/-10. No raw decompilation
is committed; behavior is summarized and cited. The 2026-07-11 session applied
two auto-name renames + two comments to the IDB (logged at the end); the held
curated-name proposals (D-HUD-1 `HUD_DrawStanceIndicator` and the crosshair
globals comment) were applied 2026-07-16, along with the scope-circle rename
`Hud_DrawScopeCircleMask @0x5d17a0` (logged at the end). The 2026-07-18
session witnessed the **waypoint HUD chain** end to end (list build, current
selection + auto-advance, name resolve, the HUDWPDINFO label, the
show-waypoints mission gate) and resolved the `@0x599700` "minimap" misnomer —
it is the **weapon heat bar**; the real map element is
`HUD_DrawMapOverlay @0x5a5f40` (located + structured, port deferred).
The 2026-07-31 recoil/spread grill then closed the write side behind the
crosshair's two dynamic terms: the round-spawn impulse, the infantry-body
decay/drift, the local movement/weapon-weight accumulator, and their distinct
projectile-versus-HUD shifts are now witnessed and ported (D-HUD-7).

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| Render pipeline + two-struct model | confirm-only (read-only grill) | `[orig: HUD_RenderAllOverlays @0x5a8070]` → `[orig: HUD_RenderOverlays @0x5a7bb0]` → element draws; per-frame `[orig: HUD_BuildEntityInfo @0x4b8440]` |
| Virtual coordinate space (1024×768) | ported (`engine/runtime/hud/hud_math` — the virtual-coords scale) | `[orig: Viewport_ScaleToVirtualCoords @0x5d2b20]` exact formula; `hud_helpers_test.gd` |
| Health bar | ported (`HudFrameCompiler::element_health` + `hud_math::health_color_band_fp16`) | `[orig: HUD_DrawHealthBar @0x5a2e50]` rect/fill/threshold-color; `hud_helpers_test.gd` thresholds |
| Stance indicator + cross-fade (IDB-misnamed "compass") | ported (`HudFrameCompiler::element_stance` — frame draw, offsets, fade pair, prev/current state) | `[orig: HUD_DrawStanceIndicator @0x599f10]` full witness incl. fade pair + per-frame offsets; `hud_helpers_test.gd` fade curve |
| HUD text + half-bright | ported (`engine/runtime/hud/game_font` — the CGameFont text engine; `godot/game/ui/hud_text.gd` survives as the ONED FontFile preview helper) | `[orig: HUD_DrawTextRightAligned_HalfBright @0x580850]` → `[orig: CGameFont_DrawText @0x6752c0]` |
| Ammo count + weapon name text | **ported** (`HudFrameCompiler::element_weapon_cluster` + `hud_math::format_ammo`) | `[orig: hud_draw_weapon_ammo_and_name @0x5939d0]`; format/hide/alignment/nudge witnessed; `hud_helpers_test.gd` format_ammo |
| Clip + rounds indicator (HUDCLIPGFX/HUDRNDGFX) | **ported** (`HudFrameCompiler::element_clip_indicator`, D-HUD-5) | `[orig: draw_hud_ammo_indicator @0x599a30]`; parse `[orig: @0x5442fc]`; `hud_helpers_test.gd` round_icon_count + flash |
| Crosshair / reticle + spread | **ported** (`HudFrameCompiler::element_crosshair`, D-HUD-7 CLOSED; D-HUD-8/9/10; target cursor / aim-point quad / lock brackets unported) | `[orig: HUD_DrawCrosshair @ 0x592640]` + `[orig: HUD_DrawCrosshairCornerQuad @ 0x590f50]`; accumulator producers `[orig: RoundData_SpawnRound @ 0x4ec0d0]` + `[orig: Entity_UpdateInfantryPlayerBody @ 0x4b40e0]`; `npruntime_round_sim`, `infantry`, `netsim_client_replica_pipeline_recoil`, and `hud_helpers_test.gd` |
| Standard weapon SIGHTS card | **ported** (`world::weapon_sights_card_eligible` → sim `scope_card_active`; `HudFrameCompiler::element_sights_card` + `godot/game/world/hud_sights_card.gd` materialize the authored rows) | `[orig: Render_ProcessMainSceneFrame @0x5ca299..0x5ca304 / @0x5caaf3..0x5cab15]`; Scoped/Sighted selectors + SWITCHFROM + NoCardSwitch/ForceScoped suppression; `nova_simulation_test.gd` + `game_hud_test.gd` |
| ALPHAFADE semantics | **ported** (`hud_math::fade_decay`/`fade_flash_alpha`) | `[orig: parse @0x5a086c]` ×2.55/×2.55/×62; flash curve `[orig: @0x599af9]`; `hud_helpers_test.gd` |
| Attach labels (seat/armory floats) | **ported** (`world::collect_attach_labels` + `HudFrameCompiler::element_attach_labels` + `game_hud_presenter.gd`, D-HUD-11/12/13) | `[orig: draw_vehicle_seat_and_armory_labels @0x5a3290]` full witness; label strings `[orig: HUD_InitOverlaySystem @0x5a479c..0x5a481e]`; `attachtextid` parse `[orig: @0x544d6c]`; the bold Arial label font + slot scale `[orig: @0x5a3680; HUD_InitAllFonts @0x51ee20]` ported 2026-08-11; ctest `vehicle_mount` + `def_parse_weapons`/`def_parse_items`; GUT `nova_simulation_test.gd`/`hud_helpers_test.gd` |
| Friendly tags (overhead name labels) | **ported** (`world::collect_friendly_tags` + `HudFrameCompiler::element_friendly_tags` + `game_hud_presenter.gd`, D-HUD-20) | `[orig: HUD_DrawFriendlyTagsPass @0x5a4480]` → `[orig: HUD_DrawEntityLabel @0x5a39b0]` full witness; names `[orig: Entity_SpawnFromBMSRecord @0x40ecbf]` + the 36-name fallback `[orig: g_fallbackPeopleNames @0x840a78]`; modes/toggle `[orig: @0x49b573]`; eye-offset anchor `[orig: @0x4bf078..0x4bf14c]` + Arial label font `[orig: HUD_InitAllFonts @0x51ee20]` witnessed + ported 2026-08-11; ctest `hud_math`/`hud_frame_compiler`/`infantry`/`promote` |
| Armory/vehicle-bay/FARP bottom prompts | witnessed — deferred with their systems (D-HUD-14) | `[orig: HUD_DrawGameplayOverlays @0x5bde60]` — preround/0x0A armory prompt, Flags 0x800 bay prompt, FARP wait/reload |
| Mission triggered text (WAC/BMS `text`) | **ported** (`HudFrameCompiler::element_messages` + `game_hud_presenter.gd`, D-HUD-6) | `[orig: HUD_DisplayTriggeredText @0x51f190]` → `[orig: Chat_AddDebugMessage @0x4987f0]`; `hud_helpers_test.gd` expiry |
| `hudpos.def` parser token map + 4-field positions | ported (`engine/formats/def`) | `[orig: loc_59F370; AMMOCOUNTPOS @0x59fc3d]`; ctest `def_parse_hudpos` |
| Parachute / armor status icons | witnessed — port pending (entity+44 flag writer unwalked) | `[orig: HUD_DrawParachuteAndArmorIcons @0x5925c0]` — entity+44 `&0x10` parachute / `&0x8` armor through the info struct's entity ptr; `ParachuteIcon`/`ArmorIcon` tokens |
| MP objective status text + team tile | confirm-only — MP HUD phase | `[orig: draw_objective_status_text @0x59aa30]` client/strcli* strings witnessed |
| Weapon heat bar (HUDHEAT) | **ported** (`HudFrameCompiler::element_heat`, D-HUD-15) | `[orig: HUD_DrawWeaponHeatBar @0x599700]` (ex kong "draw_minimap_overlay" — a misnomer; there is no radar here) full witness: border + proportional fill in the HUDHEAT rect |
| Waypoint HUD label (HUDWPDINFO) | **ported** (`HudFrameCompiler::element_waypoint` + `game_hud_presenter.gd`, D-HUD-16/17) | `[orig: HUD_DrawWaypointNameAndDistance @0x5947a0]` + `[orig: get_waypoint_name @0x594630]` full witness; gates `[orig: @0x5a7daf]` |
| Waypoint track (list/current/advance/mission gate) | **ported** (`engine/runtime/world` waypoint track + `Simulation`, D-HUD-16/17) | list `[orig: NetPacket_WriteWorldStateLoad0x0F @0x502d10 @0x502e41]` (nav channel `flags&2`); BMS marker fields `[orig: Entity_SpawnFromBMSRecord @0x40f0aa]`; advance `[orig: Player_UpdatePerFrame @0x4de5f7]`; done-mark `[orig: EventTrigger_MarkLinkedSpawnPoints @0x452ce0]`; cycle `[orig: Spectator_CycleTarget @0x4dc1d0]` + input case 23 `[orig: @0x49b3de]`; `ShowWaypoints` `[orig: Game_SetShowWaypoints @0x58fb50]` |
| Map overlay (fullscreen map: terrain, grid, blips, waypoints) | confirm-only — located + structured, follow-up | `[orig: HUD_DrawMapOverlay @0x5a5f40]` (7278 B): grid coords, `minimap_draw_ring_blip`, `update_radar_contacts`, `draw_compass_indicator`, WPNames labels, `%01.2fk` distances |
| Objectives panel + subgoal state (MISSION OBJECTIVES) | **ported** (`World::SubgoalState` + `HudFrameCompiler::element_objectives` + `game_hud_presenter.gd`, D-HUD-18) | `[orig: HUD_DrawWinConditions @0x5ba940]` full witness; actions 14/15/35/36 `[orig: EventAction_Dispatch @0x454500/@0x4545e0/@0x4546af/@0x454724]`; toggle `[orig: @0x49b68b]`; ctest `event_runtime_bms` subgoal block |

## Render pipeline — the two-struct model

The HUD keeps **layout** (parsed once) separate from **per-frame state** (rebuilt
each frame); the draw code reads both.

- **Layout globals** — parsed once at load from `hudpos.def` by the parser
  callback `[orig: loc_59F370]` (an undefined `loc_` blob, a `_stricmp`
  token-dispatch chain), registered via `File_ParseASCIIFile("hudpos.def", …)`
  inside `[orig: HUD_InitOverlaySystem @0x5a4620 @0x5a4931]`. The globals occupy
  `dword_27237xx … dword_2723Bxx` (positions/colors/texture-name strings).
- **Per-frame entity-info struct** — base `dword_2723388`, size `0x240` = **576
  bytes**, rebuilt every frame by `[orig: HUD_BuildEntityInfo @0x4b8440]` from
  the local player. The global `entityA @0x27234e8` is `dword_2723388 + 352` —
  i.e. the info struct's "current entity" slot.
- **Dispatch** — `[orig: HUD_RenderAllOverlays @0x5a8070]` (top, per frame) →
  `[orig: HUD_RenderOverlays @0x5a7bb0]` (element sub-dispatcher) → the
  per-element draws. Each element is gated by an enable flag. 2026-07-18
  finding: the whole flag block `0x2723C8C..0x2723CD8` is statically
  initialized to `-1` in `.data` and has **no runtime writer** — these are
  compiled-in always-true master switches, so the gates reduce to their other
  terms:

  | Flag | Element |
  |---|---|
  | `dword_2723C8C` | waypoint name/distance label (`[orig: HUD_DrawWaypointNameAndDistance @0x5947a0]`) |
  | `dword_2723C9C` | health bar |
  | `dword_2723CCC` | objective status text (`[orig: draw_objective_status_text @0x59aa30]`) |
  | `dword_2723CD4` | game timer + score |
  | `dword_2723CA0` | weapon/ammo + stance-indicator + ammo-indicator cluster |
  | `dword_2723CB4` | crosshair cluster (read in `HUD_DrawCrosshair @0x592757`) |
  | `dword_2723C90` | altitude / power bar |
  | `dword_2723CD0` | PowerThrow charge bar (`[orig: HUD_DrawPowerThrowChargeBar @0x599830]` — ex kong "HUD_DrawWeaponReloadBar" misnomer: it gates on the PowerThrow def bit + `g_fireChargeStartTick` and never draws reloads; witnessed + ported, world-wac-ai-re §27.3) |
  | `dword_2723CD8` | weapon slot bar (`[orig: HUD_DrawWeaponSlotBar @0x599cd0]`, unwitnessed) |

  A spectator path (`g_death_screen_active`) rebuilds the info for the *spectated* entity
  and restores: `qmemcpy(tmp, &dword_2723388, 0x240)` → `HUD_BuildEntityInfo` →
  `qmemcpy(&dword_2723388, tmp, 0x240)` `[orig: @0x5a7bf1]` — pinning the 576-byte
  size.

## Virtual coordinate space — 1024×768

All `hudpos.def` positions are authored in a **1024×768** virtual design space
and scaled to the actual screen by `[orig: Viewport_ScaleToVirtualCoords @0x5d2b20]`:

```
out_x = (design_x * screen_w + 512) / 1024
out_y = (design_y * screen_h + 384) / 768
```

`screen_w/screen_h` come from the overlay context `overlayCtx @0x24c1420`. The
`+512`/`+384` are round-to-nearest. Every element scales its rect this way
before drawing. The inverse (`[orig: Viewport_ScreenToVirtual @0x5d2c70]`,
×1024/width) maps screen points (e.g. the crosshair's screen center) back into
the design space. (Matches the 1024×768 design space the oscarmike reference used.)

**Port home (S15, 2026-08-07):** the whole HUD view-helper MATH cluster —
this scaling, the ALPHAFADE decay (quirk included), the stance Q16
scale/centering, the health color bands, the message tick policy,
half-bright, the ammo text fold + narrow-surface nudge, the round-icon
count, and the crosshair spread projection/sum/ERROR-row/gate — is native
`engine/runtime/hud` (`hud/hud_math.h`, bound as `HudPos` statics; the
`hud_math` ctest pins each). The GDScript `Hud*` helpers keep only the
CanvasItem draw work and delegate every decision here. DISPOSITION, avatar
menu-portrait presentation math (`avatar_preview.gd` — the BAM/frame idle
rotation, the 2^28 sway amplitude, the `(rand()%180)*0xB60B60` initial yaw
`[orig: update_player_preview_animation @0x55dba0; PlayerInfo_InitPreviewModel
@0x5600d0]`): granted the shell exception reviewed with this slice — pure
menu-preview presentation, already `[orig]`-cited named constants at the
node, no sim consumer; a lib hop adds nothing observable.

## Per-frame info struct — `HUD_BuildEntityInfo @0x4b8440`

Field offsets into `dword_2723388` that the ported elements read.
This is the model the OpenNova runtime "HUD info" gatherer mirrors
(`godot/game/world/game_hud_presenter.gd` — the per-frame info rebuild).

| Offset | Meaning | Source |
|---|---|---|
| +352 | current entity ptr (= `entityA`) | the local/spectated player |
| +552 | **weapon def ptr** (`dword_27235B0`) | `MountSlot+32`; every weapon element gates on it |
| +556 | weapon MountSlot ptr | `entity+280` |
| +92 | **health fraction** 16.16, capped `0x10000` | `(entity+286 currentHealth << 16) / maxHealth` |
| +374 | **team** byte | `entity+354` |
| +568 | **stance index** | `entity+300 &0x200→1 (crouch)`, `&0x100→2 (prone)`, else `0`; vehicle/mounted→`3`; parachute `entity+36 &0x20→5` |
| +52 | **reserve/pool count** | `-1` for MountSlot slot-type 6/7/8 `[orig: @0x4b8586]`; else the per-player pool (`Entity_GetScoreValueBySlotType(def+216)`) ÷ `def+224` round cost (the divide is skipped when `def+224` is 0 `[orig: @0x4b858b]`); **capacity 1 adds the clip in** `[orig: @0x4b85ef]` — ported 2026-07-11 (`main_game.gd` folds `reserve += clip` for `clipsize == 1`; the ammo text and the round icons both read the folded count) |
| +56 | **clip rounds** | `-1` when capacity (`def+88`) is `-1` `[orig: @0x4b85fa]`; else the pool clip (`def+220` set) or `MountSlot+16` u16 |
| +64 | clip capacity | `def+88` |
| +572 | **rounds-per-icon divisor** byte | `def+727` (the HUDRNDGFX 5th field) `[orig: @0x4b85fd]` |
| +60 | weapon heat (capped `0xFFFF`) | `WeaponSlot_CalcAccumulatedHeat @0x53f780`, clamp `@0x4b854d` — witnessed + ported 2026-07-22 (net-re §5.62) |
| +72 | horizontal **speed** | `300 * sqrt((entity+156>>8)² + (entity+152>>8)²) / 585` |
| +364 | distance to target/cam | sqrt of delta, `→int` |
| +368 | entity name string | `GameText_GetString("item", weapondef+1318)` else `"text_default"` |

`maxHealth` = `[orig: Entity_GetMaxHealthWithDifficulty @0x43b8a0]`. The entity
layout offsets (`+286` health, `+354` team, `+300` posture flags, `+36` flags)
agree with the GamePlayerEntity map in `docs/net/novaworld-net-re.md`. The
posture bits are now pinned by the crosshair's row select: **`0x100` = prone,
`0x200` = crouch** `[orig: HUD_DrawCrosshair @0x592b37]` (the stance *icon*
orders them crouch=1/prone=2; the ERROR table orders prone=0/crouch=1/stand=2).

## Element witness map

### Health bar — `HUD_DrawHealthBar @0x5a2e50`

- Layout rect = `dword_27237C8/CC/D0/D4` (x1,y1,x2,y2). All four zero ⇒ element
  disabled (early return). Scaled to screen via `Viewport_ScaleToVirtualCoords`.
- **Fill**: width = `(barWidth * dword_27233E4 + 0x8000) >> 16`, where
  `dword_27233E4` = `dword_2723388 + 92` = the per-frame health fraction (16.16).
  Drawn as a filled rect from `(x1+1, y1+1)` to `(x1+fillWidth, y2)`
  (`[orig: sub_5D48E0 @0x5d48e0]`) — fills **left → right**, 1px inset.
- **Border**: `[orig: Render_DrawWireframeRect @0x5d4760]` over the full rect,
  color `dword_27237C4` (the `HEALTHBORDER` color).
- **Fill color** by a freshly recomputed `(currentHealth << 16)/maxHealth`
  (currentHealth = `entity+286`): `> 0xC000` (0.75) → `dword_2723ADC` (good);
  `> 28671` (`0x6FFF` ≈ 0.437) → `dword_2723AE0` (mid); else `flt_2723AE4` (bad).
  (Note D-HUD-4: the *fill width* uses the capped `+92` ratio while the *color*
  uses an uncapped recompute — equivalent in range, recorded for fidelity.)

### Stance indicator — `HUD_DrawStanceIndicator @0x599f10` (IDB name `draw_minimap_compass_overlay` is a misnomer; D-HUD-1)

Renders the **stance** icon, keyed by `byte_27235C0` = `dword_2723388 + 568` =
the stance index from `HUD_BuildEntityInfo`. It is **not** a compass.

- Frames defined by `hudpos.def` token **`HUDSTANCE <idx> <xoff> <yoff> <texname>`**
  `[orig: loc_59F370 @0x5a0b4d]`: `xoff→dword_2723B24[idx]`, `yoff→dword_2723B44[idx]`,
  `texname→byte_2723B8C + idx*0x13` (19-byte stride). Texture handles loaded into
  `dword_27239E4[idx*4]`, dims `dword_27239E8[]/EC[]`. Retail JO defines six
  (0..5: stand/crouch/prone/sitting/emplaced/parachute).
- **Gates**: the whole element skips unless the ALPHAFADE ramp is nonzero AND
  all six stance-frame handles (0..5) are loaded `[orig: @0x599f18..0x599f50]`
  — ported 2026-07-11 (`game_hud._draw_stance` early-outs).
- Anchor = `HUDSTANCEPOS → dword_2723AEC (x) / dword_2723AF0 (y)`
  `[orig: loc_59F370 @0x5a0be7]`; each frame draws at **anchor + its own
  HUDSTANCE offset + the shared centering offset** `[orig: @0x59a173]`.
  Vehicle stance anchor `HUDVEHSTANCEPOS → dword_2723AF4/AF8`.
- **Scale and centering are SHARED from frame 0**: one factor
  `scale = 0x800000 / max(w0, h0)` computed from frame 0's dims scales every
  drawn frame's dims (`scaled = (scale*dim + 0x8000) >> 16`), and the
  centering offset comes from frame 0's scaled dims — `(128-scaled)/2`, or 0
  at 127+ `[orig: @0x599fed..0x59a07e]`. (Uniform retail frame sizes hide the
  sharing; the 2026-07-09 port scaled per-frame — fixed 2026-07-11 to the
  exact integer form, `hud_stance.gd scale_q16/scaled_dim/center_offset`,
  pinned in `hud_helpers_test.gd` test_stance_shared_scale.)
- **Cross-fade** (fully witnessed 2026-07-09): on a stance change the previous/
  current bytes (`byte_2723D3C/D3D`) shift and the change tick restamps
  (`dword_2723D38`) `[orig: @0x599f8a]`. With `elapsed` clamped to the ALPHAFADE
  ramp (`dword_272361C` ticks): `fade = 255 − u8(u16((elapsed<<16)/ramp − 1) >> 8)`
  (255 right after the change, 0 at ramp end) `[orig: @0x599fc0]`. The **current**
  frame draws at `min(base + fade, 255)` (base = `dword_2723614`); the
  **previous** frame then ghosts on top at alpha `fade >> 2` (the witnessed
  `(fade<<22)` alpha-byte splice, max 63) `[orig: @0x59a226..0x59a2e3]`. Both
  are tinted the RGB of **STANCEICON_COLOR** (`dword_2723AE8`, parsed ARGB at
  `[orig: @0x5a0ec0]`).
- In an MP session a team tile (frame index 6/7 from `entityA+354` team 1/2) is
  drawn underneath `[orig: @0x59a0cb..0x59a173]` — MP HUD phase, not ported.
- Stance indices: `0`=stand, `1`=crouch (`&0x200`), `2`=prone (`&0x100`),
  `3`=vehicle/mounted, `5`=parachute.

Port: `HudFrameCompiler::element_stance` (frame draw + offsets + the fade pair
+ prev/current state — `engine/runtime/hud/hud_frame.cpp`; originally ported
across `hud_stance.gd`/`hud_fade.gd`/`game_hud.gd`, deleted at the cutover).

### HUD text + half-bright — `HUD_DrawTextRightAligned_HalfBright @0x580850`

- All three wrappers share one body: halve the color, then call
  `CGameFont_DrawText` with a **drawFlags** word whose initial value selects
  the alignment — `HUD_DrawTextLeft_HalfBright @0x5804c0` starts 0,
  `HUD_DrawTextCentered_HalfBright @0x580680` starts 1, the right wrapper
  starts 2 `[orig: @0x580875]` (both siblings renamed from `sub_` 2026-07-11,
  anchored on the flag init + the consumption below). The wrapper's own flags
  arg adds `0x100` (format-tag suppression) and `2` (scaled mode, drawFlags
  `|= 4`); the HUD element calls pass flags `1` — plain, unscaled.
- **The alignment happens inside** `[orig: CGameFont_DrawText @0x6752c0]`:
  flag 1 measures the line and starts at `x − width·scale·0.5`
  `[orig: @0x675518..0x675539]`; flag 2 starts at `x − width·scale`
  `[orig: @0x675566..0x675587]`; neither → `x` (left). (The 2026-07-09
  "the caller measures and subtracts" wording was wrong about the mechanism;
  the drawn result — right anchor = right edge, center anchor = midpoint —
  is what `hud_text.gd` implements, unchanged.)
- **Half-bright** = `(color >> 1) & 0x7F7F7F | 0xFF000000` — halve each RGB
  channel, force opaque alpha. All three alignment paths half-bright.
- Fonts are `font_hi` / `font_lo` named in `hudpos.def` (`.fnt` bitmap fonts).
  Glyph layout/spacing inside `CGameFont_DrawText` (D3D vertex build) is a
  follow-up; the OpenNova port uses `FntResource` (already parses the `.fnt`
  glyph atlas) for the glyphs.

### Ammo count + weapon name — `hud_draw_weapon_ammo_and_name @0x5939d0` (ported 2026-07-09)

Both elements gate on the info struct's weapon-def pointer (`dword_27235B0`,
info+552) and their own token's **hidden** dword, and both draw **half-bright**
in `WEAPON_TEXTCOLOR` (`dword_2723AC4`) with the token's alignment
(0=left→`HUD_DrawTextLeft_HalfBright`, 1=right→`@0x580850`, 2=center→`HUD_DrawTextCentered_HalfBright`).

- **Ammo count** at `AMMOCOUNTPOS` (`dword_27235FC/2600/2604/2608` =
  x/y/hidden/align): reads reserve (info+52, `dword_27233BC`) and clip
  (info+56, `dword_27233C0`). Hidden entirely when `reserve == -1` or capacity
  (`weapondef+88`) `== -1`; formats **`"%d/%d"` clip/reserve** when
  `clip != -1 && capacity >= 2`, else **`"%d"` reserve** `[orig: @0x593a33..0x593ab0]`.
  (The `capacity < 2` compare is **unsigned** — capacity `-1` takes the
  `"%d/%d"` branch but the `-1` hide covers it; the port's signed
  `capacity >= 2` after the `-1` early-out yields the identical display.)
- **Weapon name** at `HUDWEAPONNAME` (`dword_27235EC/F0/F4/F8`): the string is
  `GameText_GetString("WepDes", weapondef+20)` — the **raw weapon id** (e.g.
  `WPN_AK47`) as the key into gametext's `WepDes` section; a miss returns the
  empty string (nothing draws) `[orig: @0x593b7f; GameText_GetString @0x51ebd0
  miss @0x51ec00]`. On surfaces ≤ 640 wide the x nudges −4 (left-aligned) /
  +4 (right-aligned) `[orig: @0x593b36..0x593b4d]`.

Port: `HudFrameCompiler::element_weapon_cluster`; name resolution +
capacity/-1 mapping in `godot/game/world/game_hud_presenter.gd`
(`_resolve_weapon_display_name` + the per-frame info build).

### Clip + rounds indicator — `draw_hud_ammo_indicator @0x599a30` (ported 2026-07-09)

The weapon's magazine graphic at the `HUDCLIP` anchor (`dword_27237B8/BC`;
either component nonzero enables), gated on the ALPHAFADE ramp and the ammo
`-1` sentinels.

- **Weapon-def fields** (parsed from `weapon.def`): `HUDCLIPGFX <x> <y> <tex>`
  → offset `weapon+620/624` (via `atol`), texture record `+608..616`
  `[orig: parse @0x54427f]`; `HUDRNDGFX <x> <y> <stepx> <stepy> <divisor> <tex>`
  → round start `+644/648`, step `+652/656`, **rounds-per-icon divisor byte
  `+727`** (byte store — out-of-range file values wrap mod 256; ported
  `PlayerHudWeaponDef` masks `& 0xFF`), texture record `+628..640`
  `[orig: parse @0x5442fc]`. Both parses gate on
  `FileSystem_FileExists(texture)` FIRST and skip the whole token (offsets
  included) with a parse warning when the art is missing
  `[orig: @0x544295 / @0x544316]` — behaviorally equal to the port's
  null-texture draw gates (`engine/formats/def` has no VFS; the load-time miss lands in
  the `HudOverlay` texture upload, `godot/src/hud/hud_overlay.cpp`). Retail JO
  sample: `hudrndgfx 9 0 18 0 1 H_round.tga`.
- **Flash restamp**: the stamp tick (`dword_2723D48`) resets when the ammo
  class (`weapondef+220`), the reserve count, or the pool id byte
  (`weapondef+216`) changes `[orig: @0x599ab2]` — i.e. weapon switch or reload,
  NOT per shot. Alpha = `base − u8(u16((elapsed<<16)/ramp − 1)>>8) + 255`,
  clamped to the ALPHAFADE **max** (`dword_2723618`) `[orig: @0x599af9]` — a
  flash toward `base+255` decaying to `base` over the ramp.
- **Background** (clip graphic) draws at anchor+offset with constant alpha
  `base`; **round icons** draw with the flash alpha, one per round, stepping by
  the step vector: count = clip (or **reserve when capacity == 1** — single-shot
  weapons), `(n+1)/divisor` when divisor > 1, capped at **40**
  `[orig: @0x599b9c..0x599c0a]`. Both tinted the STANCEICON_COLOR RGB.

Port: `hud_clip_indicator.gd` (restamp key proxy: D-HUD-5).

### Crosshair / reticle — `HUD_DrawCrosshair @0x592640` → `HUD_DrawCrosshairCornerQuad @0x590f50` (ported 2026-07-09)

- **Texture**: NOT per-weapon. `HUD_LoadAllTextures @0x59dda0` loads
  `sprintf("cross%02d.tga", style+1)` where `style` = the user's crosshair-style
  config (`dword_25510DC`) `[orig: @0x59e3d6]`, into the block `dword_2723980`
  ({…, handle, w, h}). The weapon-def `crosshair` field feeds a different
  (scope/lock) surface. The crosshair color is the user config
  `dword_25510E0`, overridden to `0xFFFF5050` in one mode (`dword_A8235C`)
  `[orig: @0x592bd5]`; defaults unwitnessed (D-HUD-8 / follow-up).
- **Visibility**: the cluster gates on `dword_2723CB4` and the weapon-def ptr;
  the spread crosshair draws when the player **cannot** take an aimed shot —
  `!Player_CanFireWeapon() || vehicle auto-aim || (dword_A8235C && gunner
  scoped)` `[orig: @0x592adc..0x592b01]`. `Player_CanFireWeapon @0x5cf780`
  requires either the **settled Scoped** view (`Player_IsEquippedWeaponScoped
  @0x4dcc80` = `WeaponDef.Flags & 1` plus `g_weaponScopeActive`) or the separate
  settled **Sighted** predicate (`@0x4dcd30` = `Flags & 2`, promoted active,
  and current action is not SWITCHFROM). The ordinary Scoped leg rejects
  movement and drowning/below-water state; Sighted bypasses those two gates.
  Both reject external camera and dead/airborne state, and card-switch reload
  is an earlier return `[orig: @0x5cf7be..0x5cf874]`. ForceScoped overwrites
  the scope/movement/air/water verdicts in first person, but not the early
  slot/seat/reload rejects. Thus the crosshair draws throughout ADS ease and
  whenever the applicable CanFire gate fails. Its can't-fire path also resets
  `g_cameraFovDeg = 5242880` = **80.0 deg** 16.16
  `[orig: @0x5cf88e]` — the port's `fov_deg` default.
- **Anchor**: the offset applies to the **virtual-space** projection of the
  aim point (`Viewport_ScreenToVirtual`). For the on-foot local player with no
  camera mode that point is the literal screen center
  `[orig: @0x5928a0 — overlayCtx/2, dword_24C1424/2]`; a spectated entity or
  `g_camera_mode` (external/3P) projects `Entity_BuildCameraView` instead
  `[orig: @0x592910..0x59295e]` (D-HUD-10 CLOSED 2026-07-11: the reimpl's
  `aim_screen_point()` returns no projection in first person — the HUD pins the
  exact design center — and projects the aim ray's 1000.0-unit far point
  `[orig: 65536000 q16 @0x592910]` in third person). Arms draw at top `(x, y−off)`, bottom
  `(x, y+off)`, left `(x−off, y)`, right `(x+off, y)`, center `(x, y)`
  `[orig: @0x592c50..0x592cd2]`.
- **Spread**: with `mp_CrossHairSpread` enabled (`dword_25510E4`; disabled
  still draws the assembled reticle at offset 0 `[orig: fldz @0x592bcc]` — the
  port models the enabled state):
  `row = stance + 3*Player_CanFireWeapon()` where stance = 0 prone (`&0x100`)
  / 1 crouch (`&0x200`) / 2 stand, forced 2 when swimming/under water
  (`entity+36 & 0x108020` or below `Env_WaterHeightFixed`), forced 1 when
  mounted `[orig: @0x592b35..0x592b87]`. Because the draw gate and the row
  select share the CanFire predicate, **on foot every drawn crosshair reads
  the hip rows 0..2**; the scoped rows 3..5 are reachable only through the
  vehicle auto-aim / gunner-scoped cases (witness comment left at
  `@0x592b87`; a train-side fix that keyed +3 on the port's `scope_engaged`
  contradicts this — re-adjudicate when the ADS-ease plumbing lands). Then
  `spread = C·(ERROR[row] + (player+0x380 >> 7) + (player+0x384 >> 7))` with
  `C = flt_7D76D0 = 11930464.0 = 2^31/180`, and
  `pixel = int(spread · screen_w / fov_scale) >> 16` where
  `fov_scale = 11930464 · SHIWORD(fov 16.16)` `[orig: @0x592b07..0x592bf5]` —
  the two binary-angle factors cancel:
  **`pixel = (ERROR_16.16[row] + recoil terms) · screen_w / int(fov_deg) >> 16`**.
  `ERROR` is parsed as six sequential 16.16 **degree** values into
  `weapon+0xB0..0xC4` via `Math_ParseFixedPoint16 @0x6131f0` (a digit parser,
  not atof) `[orig: WeaponDefs_ParseLineCallback @ 0x543b21]` — rows hip
  prone/crouch/stand then scoped prone/crouch/stand (retail sample `error
  0.05 0.2 0.25 0.05 0.1 0.15`). `engine/formats/def` and the runtime weapon table now
  retain those exact integers; no float round-trip sits on the parity path.
  The two live terms are likewise carried as signed BAM/fixed-point integers
  through the sim and HUD. The below-water row gate compares retail's
  `Position.Z + CameraOffset.Z` since 2026-08-12: `eye_offset_z` (the ported
  `entity+0x74`) projects the eye height in the round-source classifier and
  the crosshair row selector (the D-INF-18 water leg, closed there).
  D-HUD-7 is closed.

#### Recoil and movement-spread terms (witnessed and ported 2026-07-31)

The two entity fields used by the HUD have different producers and different
roles in projectile spread:

- `R = entity+0x380` (`pitchBlend`) is the shot-recoil accumulator. Ammo
  `recoil a b c` parses with `atol` into three bytes at AmmoDef
  `+0xE3..+0xE5`; narrowing is modulo 256, not clamped. A shot selects prone,
  crouch, or standing from `MoveOrder & 0x100/0x200`, forces standing while
  airborne or submerged, and forces crouch while attached. Retail tests
  submersion at `Position.Z + CameraOffset.Z`; the portable world projection
  currently uses body position plus Drowning (D-INF-18). Its impulse is
  `ammo.recoil[category] << 18`, or `<< 20` while drowning/underwater; a raised
  scope multiplies it by the exact binary32 `0.75` and truncates toward zero.
  The impulse is added after an ordinary round allocation or after the shotgun
  fan call, so spawned pellets see the pre-shot accumulator. It is not gated by the
  `mp_NoWeaponRecoil` rule. Instant/detonator/designator and claymore returns do
  not add it. `[orig: AmmoDef_ParseProperty @ 0x40a2d0]`
  `[orig: RoundData_SpawnRound @ 0x4ec0d0]`
- Every infantry-body tick decays `R` with signed x86 wrap/arithmetic-shift
  semantics: `t=(R+4)>>3`, `half=t>>1`, `R-=half`, then zero at `R<=0x300`.
  The same pass adds `t>>3` to entity pitch, consumes one PRNG value even when
  `R` is zero, and adds `half` to yaw for an even value or subtracts it for an
  odd value. There is no upper clamp.
  This body pass precedes camera construction and the later weapon action/spawn;
  the firing round therefore uses the previous value, and the next body update
  starts from the newly stamped impulse.
  `[orig: Entity_UpdateInfantryPlayerBody @ 0x4b40e0]`
- `M = entity+0x384` is movement/weapon-weight instability. Its local-player
  writer is active only while moving (`MoveOrder & 8`), on foot, with a live
  equipped definition. It starts from the wrapping 16.16 sum
  `clipweight + weaponweight`, then wrap-adds one third when an aimed shot
  is available, one third while prone and not drowning, two thirds while
  crouched and not drowning, otherwise one-and-a-half. The one-third leg is
  signed integer division; the two-thirds and one-and-a-half legs use the
  retail floating conversion and truncate toward zero. Rising while
  airborne performs a second wrapping add of `0x01000000`; there is no upper
  clamp. The shared decay follows those local-player adds in the same tick:
  `M -= (M+4)>>4`, zeroing it at `M<=0x300`. Remote players and AI skip the
  producer but run the same decay, so a local shot later in the tick sees the
  already-decayed new movement contribution.
  `[orig: WeaponDefs_ParseLineCallback @ 0x5440db]`
  `[orig: WeaponDefs_ParseLineCallback @ 0x54410d]`
  `[orig: Entity_UpdateInfantryPlayerBody @ 0x4b40e0]`

The HUD deliberately uses `R>>7` and `M>>7`; ordinary projectile magnitude
uses `R>>8` and `M>>7`. The projectile ERROR selector is also independent of
the HUD selector: `verticalSpread ? 3 : category`, whereas the HUD uses
`stance + 3*Player_CanFireWeapon()`. Keeping those two consumers separate is
required for retail parity. `[orig: RoundData_SpawnRound @ 0x4ec0d0]`
`[orig: HUD_DrawCrosshair @ 0x592640]`
- **Unported sub-elements of the same function** (witnessed 2026-07-11,
  explicitly out of the SP weapon-cluster port):
  - the **target-tracking cursor** — with a tracked entity (`ptr @0x27234F0`)
    and the cursor art loaded, a quad draws at the projected
    `Entity_ComputeWeaponFireOrigin` of the target, color/texture switching on
    same-team (`dword_2723900` vs `dword_27238F0` records) with an MP team
    gate and a tick-blink `[orig: @0x592790..0x592875]`;
  - the **aim-point quad for flagged weapons** — `weapondef+12 & 0x80` swaps
    the spread reticle for the weapon's own crosshair record (`weapon+376`)
    drawn at a raycast-projected aim point
    (`Entity_ComputeUserpointTransform` → `physics_raycast_entity_pools…` →
    project) `[orig: @0x592973..0x592ac8]`;
  - the **lock brackets** — four clipped 2D lines blinking around the tracked
    target when its mount state reads 3, team- and blink-gated
    `[orig: @0x592ce2..0x592dd7]`.
  - a mode flag `dword_24C1930 & 0x10000` replaces triggered/gametext strings
    with the literal `"&"` `[orig: @0x51f1c8 / @0x51ebe3]` — writer
    unwitnessed; not modeled.
- **Region geometry** (`HUD_DrawCrosshairCornerQuad @0x590f50`): each region's
  quad rect is the texture's size centered on the (offset) point, corners
  scaled to screen; the arms are 5-vertex triangle strips and the center a
  4-vertex strip, with the **inner vertices at the quad midpoint pulled back by
  0.1 × half-extent**, and **every vertex's UV = its normalized position within
  the quad rect** — which lands the inner vertices exactly on the witnessed
  0.45 / 0.5 / 0.55 atlas bands (center band 0.45..0.55, arms the outer bands).
  Vertex format: `rhw = 0.9`, `diffuse = 1.0`, `specular = color`; emitted via
  `[orig: GDynamicVB_DrawPrimitive @0x6788e0]` (D-HUD-8 on the color stage).

Port: `HudFrameCompiler::element_crosshair` (spread_px / error_row / the 5
UV'd strips, visibility + row select — `engine/runtime/hud/hud_frame.cpp`),
fed by the sim's shared `Player_CanFireWeapon` projection. Scoped/Sighted,
promotion timing, movement, camera, reload, air/water, ForceScoped, and the
seat gates therefore select visibility and the ERROR triplet together.

### Standard weapon SIGHTS card — `Render_ProcessMainSceneFrame @0x5ca299..0x5cab15` (corrected 2026-07-19)

The standard 2D card is chosen dynamically after ADS settles; neither the
presence of a `SIGHTS` block nor a static Scoped bit alone is its gate:

- The **Scoped** predicate is `Player_IsEquippedWeaponScoped @0x4dcc80`
  (`Flags & 1` plus `g_weaponScopeActive`). `Render_ProcessMainSceneFrame`
  begins this path at `0x5ca299`; Inset (`Flags2 & 0x200`) does not set the
  ordinary Scoped-card byte, while the standard path sets it at `0x5ca2c7`.
- The separate **Sighted** predicate at `0x4dcd30`, called at `0x5ca2cc`,
  requires `Flags & 2`, `g_weaponScopeActive`, and
  `MountSlot.currentAction != SWITCHFROM (7)`. It sets the second selector byte
  at `0x5ca2d5`.
- The predicate at `0x4dcce0`, called at `0x5ca2f6`, recognizes
  `NoCardSwitch && !ForceScoped` and clears **both** standard-card selectors at
  `0x5ca2ff/0x5ca304`. ForceScoped overrides this suppression.

Only the post-clear bytes reach the drawing branch. Sighted calls
`draw_weapon_sight_overlays @0x4dce00` at `0x5caaf3/0x5caafa`; otherwise
Scoped calls it at `0x5cab01/0x5cab08`, followed by
`Hud_DrawScopeCircleMask @0x5d17a0` at `0x5cab15` as the Scoped fallback. When
both selectors are zero, the first-person viewmodel path remains available
(`0x5ca32c..0x5ca343`, consumed at `0x5ca822..0x5ca829`).

`draw_weapon_sight_overlays` itself reads only the authored count
(`WeaponDef+0x258`) and rows (`WeaponDef+0x1c8`, stride `0x24`). Those rows are
card **content**, including draw order/blend/scale, not selection policy. A
nonzero count is reported even if a row's texture handle is missing, which can
suppress the circle fallback and leave a blank reticle. REVX02's
`WPN_M4AUTO_EOTECH` confirms the Sighted path: it is Sighted, not Scoped, has no
NoCardSwitch, and authors `M4ET_SGT.TGA` plus additive/scaled `et_rtcle.tga`;
both textures exist in the retail resource root.

Port: `world::weapon_sights_card_eligible` owns the dynamic selector, the sim
publishes it as `scope_card_active`, and `HudFrameCompiler::element_sights_card`
+ `godot/game/world/hud_sights_card.gd` always materialize the authored rows
and use that selector only for visibility.

### Mission triggered text — `HUD_DisplayTriggeredText @0x51f190` (ported 2026-07-09)

The WAC/BMS `text` action (our `event_runtime` `OutputText` effect, id in
`param1`) resolves `sprintf("ID%03i", id)` against the **per-mission string
table** `g_TextMission @0xB4C2B4`, section **`"Triggered Text"`** — read
directly, no override-table consult; a miss shows nothing. The table loads at
mission start from the map file name with its extension replaced by `.bin`,
**falling back to `medmssn.bin` only when that file does not exist**
(`FileSystem_FileExists` picks the name; a present-but-unloadable file stores
null with no fallback) `[orig: TextResource_LoadMissionTextBin @0x51ed90,
exists-gate @0x51ede3]` — ported exactly (`main_game._load_hud_text_tables`
via `has_file`). The resolved line goes to the system/debug chat channel:
`Chat_AddDebugMessage(text, -1, 930)` `[orig: @0x51f216]` — the `-1` color is
stored raw in the 128-byte slot, i.e. packed ARGB `0xFFFFFFFF` opaque white
(the port pushes white; whether the unwitnessed drawer treats `-1` as a
channel-default sentinel is part of the D-HUD-6 follow-up). Slots carry 119
text chars, word-wrapped via font metrics, per-line life **930 ticks** (~15 s)
with consecutive expiries floored to **prev + 186 ticks** `[orig: @0x49894e]`;
display buffers rebuilt by `[orig: Chat_RebuildDisplayBuffers @0x498bd0]`
(40 wrapped slots per channel, continuation lines indented two spaces). The
channel's on-screen geometry table (`dword_28E4DF8`) writer is unwitnessed —
follow-up.

Port: `hud_messages.gd` feed + `main_game.gd _show_triggered_text` /
`_load_hud_text_tables` (D-HUD-6 on the reduced altitude).

### Parachute / armor icons — `HUD_DrawParachuteAndArmorIcons @0x5925c0`

- Parachute: `entityA flags &0x10` → draw `ParachuteIcon` at `(x@0x272383C, y@0x2723840)`,
  handle `dword_27239A4`.
- Armor: `entityA flags &8` → draw `ArmorIcon` at `(dword_2723844, dword_2723848)`,
  handle `dword_27239B4`.
- The static HUD frame background is `STATICFRAME` → pos `dword_2723B1C/B20`,
  name `byte_2723C24`, drawn at the top of `HUD_RenderOverlays`
  (`[orig: draw_textured_quad_with_border @0x590c40]`).
- Port pending: the runtime does not yet surface the entity flag bits.

### MP objective status — `draw_objective_status_text @0x59aa30` (witnessed, MP HUD phase)

The `GAMEINFO`-anchored status line for MP game types (`g_GameType & 0x10000`),
switching on the objective state byte (`byte_27234FE`): gametext `client`
section keys `strcli19/05/06/17/18/01` with per-state colors; KOTH appends
`strcli20/21` by flag-carrier state; the death screen shifts the draw up by the
measured text height. Not an SP element; ported later with the MP HUD.

### Attach labels — `draw_vehicle_seat_and_armory_labels @0x5a3290` (ported 2026-07-17)

The floating "indicator near where you attach": a wireframe box + centered text
drawn at the **screen projection** of every eligible seat/armory userpoint on
nearby entities. Called **unconditionally** by `HUD_RenderOverlays`
`[orig: @0x5a7daa]` — no hudpos enable flag; its own gates decide visibility.

**Label strings** — resolved once at `[orig: HUD_InitOverlaySystem
@0x5a479c..0x5a481e]` from the gametext **Overlays** section via
`GameText_GetStringWithFallback` (fallbacks are the `!`-marked literals):

| Global (renamed this session) | Key | Retail text | Fallback |
|---|---|---|---|
| `g_hudLabelTextSit @0x2723860` | `STROVER_SIT` | "Sit" | `!sit` |
| `g_hudLabelTextControl @0x2723864` | `STROVER_CONTROL` | "Control" | `!Control` |
| `g_hudLabelTextUseGun @0x2723868` | `STROVER_USEGUN` | "UseGun" | `!UseGun` |
| `g_hudLabelTextUseArmory @0x272386C` | `STROVER_USEARMORY` | "Armory" | `!UseArmory` |
| `g_hudLabelFmtArmoryDelay @0x2723870` | `STROVER_USEARMORYD` | "Armory in %d Seconds" | `!ArmoryDelay %d` |

**Selection** (the ported half — `world::collect_attach_labels`,
`engine/runtime/world/src/vehicle_attach.cpp`):

- Bracket gate: no `Entity_FindNearestSeatOrArmory` hit → no labels at all
  `[orig: @0x5a32e2]`. The searchMode is the player's **armory-zone flag**
  (`Flags & 0x400000`) `[orig: @0x5a32c4]` — in the zone the pass switches to
  armory labels; the armory leg of the scan (`seatType 4 @0x436417`) exists
  ONLY for this highlight (both mount-toggle callers pass searchMode 0).
- Per-entity: iterate the player's proximity list (`entity+0x1BC/+0x1C0`,
  read through `entityA @0x27234e8` — the current-entity POINTER, spectator-
  aware) `[orig: @0x5a32a9]`; a ready weapon limits labels to the **nearest
  entity** — `!Player_CanFireWeapon() || entity == nearest` `[orig: @0x5a3354;
  Player_CanFireWeapon @0x5cf780]`; dead/destroyed skip, enemy-occupied
  vehicles reject (`Vehicle_HasEnemyOccupant @0x4359f0`), carrier rules
  `[orig: @0x5a33d9]` (unmodeled — D-AI-11 b).
- Seat labels (not armory mode): per `itemDef->seatBoneIndex[0..9]` slot —
  occupied seats (`mountHandles[slot] != 0xFFFF`) never label
  `[orig: @0x5a348f]`; name prefixes `sitex`/`ctrlx`/`drvrx` (strnicmp 5) pick
  Sit/Control/Control, exact `USEGUN` (stricmp) reads the gun's weapon slot0
  def `+0x3A0` attach text via `Entity_GetWeaponSlots @0x5460e0`
  (vehicle +1140 / infantry +692 / parent's when mounted), null → the UseGun
  default `[orig: @0x5a350c..0x5a3544]`.
- Armory labels (armory mode): items with **attrib 0x80000 Armory** walk ALL
  model userpoints for the `armory` prefix (strnicmp 6) `[orig: @0x5a36f5,
  @0x5a372b]`; with `dword_A85B6C` nonzero the label is
  `sprintf(g_hudLabelFmtArmoryDelay, A85B6C)` `[orig: @0x5a377f]` (the MP
  armory-delay state; SP always 0 → plain "Armory").
- Per point: world pos = the posed userpoint through the entity basis
  (`build_bone_attachment_matrix @0x56c630`) **+ 12288 (0.1875 u) Z lift**
  `[orig: @0x5a3585]`; distance gate = full 3D from the **entity position**
  (not the eye) `< 0x40000` (4.0 u) `[orig: @0x5a35f0]`; LOS raycast from the
  player position to the lifted point, the gun's carrier substituted as the
  ignored entity (`Physics_RaycastTerrainAndSectors @0x5a3609`).

**Draw** (the GDScript half — `game_hud_presenter.gd _build_attach_labels` projects +
resolves text, `hud_attach_labels.gd` draws):

- Project world→screen (`Math_FixedPointTransformPoint22 @0x5a3628` +
  `clip_point_to_frustum_and_project @0x5a3655` — behind-frustum points skip).
  The label geometry is **raw screen pixels**, not the 1024×768 design space.
- Color: the nearest (entity, bone) pair draws the master overlay color
  (`alpha @0x24c1868` = `overlayCtx+0x448`); every other label draws
  `((rgb & 0xFEFEFE) | 0xFE000001) >> 1` — RGB halved, alpha forced 0x7F
  `[orig: @0x5a3640..0x5a364e]`.
- Geometry: measure w/h (`HUD_MeasureTextWH @0x580ab0` →
  `CGameFont_MeasureText @0x674e70` with the bold slot's
  (`g_hudLabelFontBold @0xB4C394`, ex "fontObj") scale pair); box
  `(x−w/2, y−2)..(x+w/2+5, y+h+1)` (`Render_DrawWireframeRect @0x5a36ad`);
  text centered through the half-bright text path
  (`HUD_DrawTextCentered_HalfBright @0x5a36c1`).

**Data chain** (ported end to end): weapon.def `attachtextid <key>` →
`[orig: WeaponDefs_ParseLineCallback @0x544d6c]` resolves
`GameText_GetString("overlays", key)` AT PARSE into `AdmDef+0x3A0` (a missing
key stores "" — the label then draws an empty box; an absent line stores null
→ the UseGun default). Retail JO ships 28 `attachtextid` rows, all emplaced
guns/turrets (`attach_50cal` ".50 Cal", `attach_minigun` "Minigun",
`attach_mk19` "Grenade Launcher", …). items.def `primary_weapon` links the
ewep item to its weapon.def entry (`ItemDef+0x54B`). Our split keeps the KEY
through `DefWeaponDef.attach_text_id` → `WeaponTableEntry.attach_text_id` and
resolves in the HUD host against the same Overlays section with the same
miss semantics. (`Gametext.bin` also carries `STRMISC_USEGUNMSG` "Press '$A'
to attach to the $B" — **unreferenced by this binary**; no code cites it.)

### Friendly tags — `HUD_DrawEntityLabel @0x5a39b0` via `HUD_DrawFriendlyTagsPass @0x5a4480` (ported 2026-08-10)

The overhead entity name labels (retail's **FRIENDLYTAGS** feature — the toast
strings name it). The pass runs from `HUD_RenderAllOverlays @0x5a87cc`, after
the overlay cluster and before the console messages, gated on
`g_friendlyTagsMode @0x24C18C4 != 0` and `g_mpattrib_flags @0x24D1E34` bit
`0x400` clear (the host option `FriendlyTag 0` sets it
`[orig: ServerConfig_ApplyHostSetting @0x4a6358]`).

**Selection** `[orig: HUD_DrawFriendlyTagsPass @0x5a4480]`: walk 1 = pool-0
entities WITHOUT `Flags & 0x100` (player-controlled ones ride walk 2), team
`0`/local/death-screen, gametype set → `HUD_DrawEntityLabel(entity, NULL)`;
walk 2 = the player-slot table (`g_playerSlotPtrTable @0xA822D0`, entries
`{+13 active, +36 entity}`) with the same gates →
`HUD_DrawEntityLabel(entity, slot)`.

**The drawer** `[orig: HUD_DrawEntityLabel @0x5a39b0]`:

- Entry bails: null/local entity, `Flags & 1`, null itemDef. The
  `PlayerSlot_FindByEntityPtr` call at `@0x5a3a5a` discards its result (dead
  code); the `Player_CanFireWeapon`/`Player_IsEquippedWeaponScoped` pair at
  `@0x5a3b1a` runs for its auto-aim side effect only.
- Anchor `(x, y, z + entity[+116] + 0x4000)` `[orig: @0x5a3a84..0x5a3a98]` —
  +116 (+0x74) is the z of the entity's **eye/camera-offset triple**
  (+0x6C/+0x70/+0x74), restamped per body tick from the anim capsule (writers
  walked 2026-08-11): the NPC updater stores
  `max(out_transform[4] − out_transform[3], 0x9000) · cosQ22(leanAngle +0xB0)`
  with the lateral pair `delta·sinQ22(lean)·3/4` rotated by heading
  `[orig: Entity_UpdateInfantryAI @0x4bf078..0x4bf14c]`; the non-local player
  leg caps the extent at `0xD000`, tilts by lean/heading, and floors the store
  at `0x2000` `[orig: Entity_UpdateInfantryPlayerBody @0x4b6984..0x4b68f5]`;
  the local player takes the exact head-bone z − origin
  `[orig: @0x4b6908..0x4b696c]`. Seeds: `0xB333`
  `[orig: Player_InitPlayer @0x4e18a1]`, deploy reset `0xD000`
  `[orig: NapiNPClientMsg_0x00A @0x42ffc9]`. Port: the infantry motor restamps
  `Entity::eye_offset_z` (z only) each clip advance and the gather feeds it per
  tag; the presenter lifts eye + 0.25 u. The head-bone local leg and the
  lateral lean shift are residues (tags never draw the local player).
- Gates: view distance ≥ `0x8000` (0.5 u, spectate target exempt)
  `[orig: @0x5a3b0c]`; fog cull vs `Env_FogDistCurrent` `[orig: @0x5a3b28]`;
  frustum project-or-bail (`Math_FixedPointTransformPoint22` +
  `clip_point_to_frustum_and_project @0x5a3b47`); the death screen pins the
  spectated entity to `(screenW/2, 2·fontH)` `[orig: @0x5a3b74]`.
- Line metric = the `'0'` glyph's height × the slot's scaleY
  `[orig: GameFont_MeasureCharHeight @0x580a80 ('0', font) @0x5a3a36]`; fonts
  `g_hudLabelFont @0xB4C388` / spectated `g_hudLabelFontLarge @0xB4C3A0`.
  The overlay font slots are `{CGameFont*, scaleX float, scaleY float}` loaded
  by `HUD_LoadFontIntoSlot @0x580400` (scale float = `scaleFP/65536`
  `[orig: @0x58045d]`) from `HUD_InitAllFonts @0x51ee20`:
  `g_hudLabelFont` = `Arial14n.fnt` (width ≤ 800) / `Arial16n.fnt` (> 800),
  the bold slot `g_hudLabelFontBold @0xB4C394` = `Arial12b/14b/16b.fnt` per
  the same 640/800/1024 tiers, both at scale
  `(screenWidth<<16)/{640,800,1024}` `[orig: @0x51ef26]`;
  `g_hudLabelFontLarge` = `Impac22b.fnt`, `g_hudLabelFontImpact38 @0xB4C3AC`
  = `Impac38b.fnt`, both over 800. The draw/measure helpers pass the slot
  scales into `CGameFont_DrawText`/`_MeasureText`
  `[orig: HUD_DrawTextCentered_HalfBright @0x580680;
  HUD_DrawTextHalfBrightF @0x580720; HUD_MeasureTextWH @0x580ab0]`.
  Attach labels draw with the BOLD slot `[orig: @0x5a3680/@0x5a38a1]`.
  Port: `HudOverlay::ensure_label_fonts_` loads the Arial pair per surface
  width tier and `HudFrameCompiler::configure_label_fonts` draws the
  friendly-tag and attach-label elements with it at the witnessed scale
  (page-namespaced per font in the shared draw list).
- **Colors**: health tier by the health bar's exact bands
  (`HUD_ClassifyHealthBand @0x59c1f0` — good > 0xC000, middle > 0x6FFF, both
  callers pass health ratios; the "distance LOD" name was a misnomer) →
  hudpos `tagcolor_good/middle/bad`; the good tier reads `tagcolor_good` only
  when `cfg_hud_color_index == 2` (the config DEFAULT `[orig: @0x54d28b]`),
  else `g_hudColorTable[index]` (`HUD_InitTeamColorTable @0x51f240`: 0 white,
  1 green, 2 = per-frame hudpos `hud_textcolor`, 3 light blue, 4 yellow,
  5 salmon; input case 10 cycles `[orig: @0x49afc7]`). Enemy team = `0xFF00FF`
  drawn only under the server-granted `g_enemyTagsVisible @0x24D1DF4`
  (spectator-mode / S2C 0x00A writers). Death screen: team 1
  `tagcolor_blueteam`, team 2 `tagcolor_redteam`, else `0xFF208020`
  `[orig: @0x5a3c3a..0x5a3c60]`. Squad override: `g_squadColors @0x83B450`
  (8 pastel entries) by slot+51, the middle tier × 0.7/channel
  (`dbl_7D9DE8`) `[orig: @0x5a3d14]`; the flag-2 legs read
  `g_hudColorLightBlue` (table[3], pulsing to white on the
  `((tick-8) & 0x3F)` triangle when slot+44) or `g_hudColorGray` (table[8])
  `[orig: @0x5a3dcb..0x5a3e7f]`.
- **Speaking pulse**: entity == `g_voicePlaybackEntity @0xC6EC38` (stamped at
  scripted positional voice start `[orig: Audio_StartEntityPlayback
  @0x4ece03]`) → each channel saturates at `c/2 + g_audioOutLevelStage1/4`
  (the MMX blend `@0x5a3e98..0x5a3ebf`) — the training sergeant's label
  pulses with his voice.
- **Distance alpha**: `255 − clamp(192·(dist_m − 50)/250, 0, 192)`
  (255 at ≤ 50 m → 63 at ≥ 300 m) `[orig: @0x5a3eeb..0x5a3f18]`.
- **Name**: slot → callsign(+20) + `<ch>` tag(+32) `<co>` (GameFont format
  tags); else the entity Name (+244), authored at BMS spawn — record
  `name_index`(+4) ≠ 0 → `sprintf("STRNAME%03i")` → mission-RTXT
  `[PeopleNames]` value, `strncpy` 15 `[orig: Entity_SpawnFromBMSRecord
  @0x40ecbf..0x40ed0a]`; else a literal `'^'` + the compiled-in 36-name table
  `[orig: g_fallbackPeopleNames @0x840A78; @0x5a4047..0x5a40cd]` indexed
  `(pool<<12|slot) % 36` — the shared generator is `Entity_GetDisplayName
  @0x59bf70`. (The TR capture's `^SGT. Brown` is the AUTHORED path — the
  caret and period live in the rtxt value; the fallback table's row 24 is
  `SGT  Brown`.)
- **Modes** (`g_friendlyTagsMode`, boot default **2**
  `[orig: Game_Run @0x4a7fed]`; input action case 30 cycles 0→1→2→3→0 with
  `GameText("Misc", STRMISC_FRIENDLYTAGS_{OFF,FARBRIEF,FULL,BRIEF})` through
  `Chat_AddDebugMessage` `[orig: @0x49b573..0x49bc60]`): 1 = text under 300 m
  (`0x12C0000`), 2 = text always, 3 = three 1-px vertical tick lines at
  `x−1/x/x+1` spanning ±fontH/4 `[orig: @0x5a40eb..0x5a4160]`; an empty slot
  callsign draws a single fontH bar `[orig: @0x5a4398]`.
- **Text**: CENTERED on the projected x (`CGameFont_DrawText` flags bit 1),
  top at `y − fontH/2`, half-bright with the caller's alpha PRESERVED
  (`HUD_DrawTextHalfBrightF @0x580720` —
  `(c & 0xFF000000) + ((c>>1) & 0x7F7F7F)`); flag-2 + slot+16 appends
  `"%s: %ld"` `[orig: @0x5a4212]`, the tick form draws the bare `"%ld"` at
  `y − fontH` `[orig: @0x5a442e]`.
- **Medic plate**: `CharAttr[playerClass].flags & 0x8` (charattr.def
  `ATTRIBUTES` — the flag table `@0x813F18`: AutoScope 1, SpreadBonus 2,
  KnifeBonus 4, **Medic 8**, WaterGirl 0x20; loader `CharAttr_LoadFromDef
  @0x412140`) → a fontH/2 square at `(x − textW/2 − fontH, text top)`:
  `HUD_DrawMedicCrossQuad @0x59bcb0` builds a WHITE quad + two RED bars inset
  by an eighth — literally a red cross on white — at the tag alpha
  `[orig: @0x5a4309..0x5a436c]`.
- **Wounded icon** (unported): `entity+885 && !Entity_FindChildByDefType(e,1,1)`
  with the viewer gate (local mount kind ∈ {2,5} or local +885) → the rotated
  icon quad `HUD_DrawRotatedIconQuad @0x599630` (texture id 0x17, table[3]
  light blue, forced full alpha, half-size fontH/4·0.5). The +885 writer is
  unwitnessed (net-re notes a sector-action 30-tick timer at the same offset).

**Port** (D-HUD-20): gather `world::collect_friendly_tags`
(engine/runtime/world/friendly_tags.cpp) → `Simulation::get_friendly_tags` →
`game_hud_presenter.gd _apply_friendly_tags` (projection, view distance, live
fog feed, the KEY_F cycle + toast) → `HudOverlay::set_friendly_tags` →
`HudFrameCompiler::element_friendly_tags` + the hud_math policy helpers (the
alpha ramp, mode rules, fallback table, speaking blend, alpha-preserving
half-bright). The anchor rides the witnessed eye offset: the infantry motor
restamps `Entity::eye_offset_z` per clip advance (NPC/player clamps above),
the gather carries it per tag, and the presenter lifts eye + 0.25 u
(2026-08-11; replaces the earlier 2.15 u standing-constant stand-in). Text
draws with the witnessed Arial label font at the resolution-tier scale
(`HudOverlay::ensure_label_fonts_` → `configure_label_fonts`; attach labels
take the bold face). Names ride BMS `name_index` → the mission `.bin`
`[PeopleNames]` harvest → `PromoteOptions::people_name_resolver` →
`Entity::display_name`. Pinned by ctest `hud_math`, `hud_frame_compiler`
(label-font faces/scale/page namespaces), `infantry` (the eye-offset
restamp), `promote`. Residues in the D-HUD-20 row.

### Gameplay prompts — `HUD_DrawGameplayOverlays @0x5bde60` (witnessed, deferred)

The bottom/top-center "Press …" prompt cluster, drawn at virtual x=512 via
`HUD_DrawTextAtVirtualPos @0x5d3ec0` (the inline `(x*w+512)/1024` scale),
master-gated on `dword_840B18` and skipped while `layerIndex == 3`:

- **Armory prompt** — `dword_A85B64` (the preround/deploy timer, an S2C 0x0A
  header field; net-re §5.47) nonzero → `STROVER_ARMORY_INFO` "Press '%s' to
  access armory menu", `%s` = `KeyBinding_FormatDisplayString @0x496bd0` over
  `g_bindingRow_useitem @0x81A454` (binding row 44 `useitem`, retail SHIFT);
  suppressed while a menu is open (`sub_54B970` → the active-menu index
  `dword_255110C`). The `STROVER_ARMORY_WAIT` "Armory available again in %d
  seconds" leg `[orig: @0x5bdf61]` is **dead code** in this build: the
  A85B64==0 recheck `@0x5bdef8` cannot pass inside the A85B64!=0 branch
  (`sub_54DF60` between the reads is a return-0 stub). SP never streams the
  0x0A header, so retail SP never shows this prompt.
- **Vehicle-bay prompt** — preround inactive and `Flags & 0x800` (type-11
  vehicle-loadout volume) with a groundEntity whose team is 0 or the player's
  → `STROVER_VEHICLEBAY_INFO` "Press '%s' to activate vehicle bay menu"
  `[orig: @0x5bdf69..0x5bdfc9]`.
- **FARP overlays** — seated (parentSlot 2/5) on a vehicle whose groundEntity
  is FARP-capable (`itemDef attrib2 & 0x2000`, pad flag 0x40, the
  `weaponByte` unlock-group vs `dword_A85BBC`, team/game-type gates) →
  `STROVER_FARP_WAIT` "Reload available in %d seconds" (`dword_A85B70`) /
  `STROVER_FARP_RELOADING` "Reloading" (`!dword_A85B74`)
  `[orig: @0x5bdff8..0x5be10e]`.

All three legs ride systems we haven't ported (MP preround state, vehicle.mnu,
FARP rearm) — recorded as D-HUD-14 and deferred with them.

## `hudpos.def` parser token → global map — `loc_59F370`

A `_stricmp` token-dispatch; each token reads decimal fields via `atof → ftol`
(1024×768 ints) or copies a texture-name string. Cross-checked against
`DefHudPosDef` in `engine/formats/def/include/def/def.h`.

| Token | Writes |
|---|---|
| `HEALTHPOS` | `dword_27237C8/CC/D0/D4` (x1,y1,x2,y2) |
| `HUDSTANCE <idx> <x> <y> <tex>` | `dword_2723B24[idx]`, `dword_2723B44[idx]`, `byte_2723B8C+idx*0x13` |
| `HUDSTANCEPOS` | `dword_2723AEC` (x), `dword_2723AF0` (y) |
| `HUDVEHSTANCEPOS` | `dword_2723AF4` (x), `dword_2723AF8` (y) |
| `STATICFRAME` | name `byte_2723C24`, pos `dword_2723B1C/B20` |
| `ParachuteIcon` | name `byte_2723C34`, pos `0x272383C/0x2723840` |
| `ArmorIcon` | name `byte_2723C44`, pos `dword_2723844/0x2723848` |
| `AMMOCOUNTPOS <x> <y> <hidden> <align>` | `dword_27235FC/2600/2604/2608` — the **4-field positioned-text layout**, read strictly positionally: fields 1-3 `atof→ftol` (a word in field 3 reads 0), field 4 via `HUD_ParseTextAlignment @0x59d6b0` (full-string stricmp: "right"=1, "center"=2, anything else — including a missing field — 0=left) `[orig: @0x59fc3d]`. Retail 2-field lines (`GAMEINFO`, `HUDCHATTEXT`) render visible/left in retail JO, pinning missing fields to 0. `HUDWEAPONNAME` → `dword_27235EC/F0/F4/F8`, `GAMEINFO` → `dword_272382C/30/34/38`; the other positioned tokens follow the same shape |
| `ALPHAFADE <base%> <max%> <seconds>` | `dword_2723614` = base×**2.55**, `dword_2723618` = max×**2.55**, `dword_272361C` = seconds×**62** (ticks) `[orig: @0x5a0882..0x5a08c2; dbl_7D9A20 = 2.55, dbl_7C88C0 = 62.0]`. Each field goes through **atof**, so fractional file values (`1.5` s → 93 ticks) survive into the converts — `engine/formats/def` stores the raw fields as floats and the consumers apply ×2.55/×62 with the same truncation (fixed 2026-07-11; `def_parse_hudpos` pins the fractional case) |
| `STANCEICON_COLOR <a> <r> <g> <b>` | `dword_2723AE8` packed ARGB `[orig: @0x5a0ec0]` — the stance/clip-indicator tint |
| `HUDCLIP` | `dword_27237B8/BC` — the clip-indicator anchor |
| `HUDWPDINFO <x> <y> <hideBox> <align>` | `g_hudWpdInfoX/Y/HideBox/Align @0x2723694/98/9C/A0` — the waypoint label anchor; field 3 hides the box only `[orig: @0x5a02c3]` |
| `HUDHEAT <x1> <y1> <x2> <y2>` | `g_hudHeatRectX1/Y1/X2/Y2 @0x27237DC/E0/E4/E8` — the heat-bar rect `[orig: @0x5a1449]` |
| `HUDHEATBORDER <a> <r> <g> <b>` | `g_hudHeatBorderColor @0x27237D8` packed ARGB `[orig: @0x5a14b3]` |
| `stancecolor_middle` / `stancecolor_bad` | `g_stanceColorMiddle @0x2723AE0` / `g_stanceColorBad @0x2723AE4` packed ARGB — the shared bar colors (health-bar tiers, heat fill, vehicle bars) `[orig: @0x5a0dd5/@0x5a0e4b]` |
| `hud_textcolor` | `g_hudposTextColor @0x2723AC0` `[orig: parse @0x5a0f37]` — copied into `g_hudColorTable[2]` every frame `[orig: HUD_RenderAllOverlays @0x5a8100]`; with `hud_color_index` default 2 this IS the master overlay color `g_hudActiveColor @0x24c1868` (the ex-D-HUD-13 open writer) |
| `tagcolor_blueteam`/`redteam`/`good`/`middle`/`bad` | `g_hudposTagcolor* @0x2723AC8/ACC/AD0/AD4/AD8` — packed `(r<<16)\|(g<<8)\|b` from three decimal fields `[orig: @0x5a1040..0x5a11d1]`; the friendly-tag health tiers + death-screen team colors (D-HUD-20) |
| `HUDTIMECLOCK`, `mapcoords`, `HUDPOWERBAR` | recon-confirmed token set (timer/map — witness when those elements land) |

## Weapon heat bar — `HUD_DrawWeaponHeatBar @0x599700` (witnessed 2026-07-18)

Ex kong `draw_minimap_overlay` — a **misnomer** (renamed this session): the
function draws no map. It is the weapon **heat bar**:

- Gate: `hudInfo+60` (`0x27233C4`) nonzero — the accumulated weapon heat,
  written per frame by `[orig: HUD_BuildEntityInfo @0x4b8533]` from
  `[orig: WeaponSlot_CalcAccumulatedHeat @0x53f780]`, clamped `0xFFFF`. Zero
  heat = no bar (the element self-hides).
- Rect: the `HUDHEAT x1 y1 x2 y2` hudpos token (`g_hudHeatRectX1..Y2
  @0x27237DC..0x27237E8`), scaled to virtual per corner. A wireframe border
  always draws first in `HUDHEATBORDER` color (`g_hudHeatBorderColor
  @0x27237D8`) `[orig: Render_DrawWireframeRect @0x599787]`.
- Fill: a solid rect (`[orig: sub_5D48E0]` — viewport-offset color fill) inset
  by 1px, proportional to `heat/0x10000` with round-to-nearest
  (`(span*heat + 0x8000) >> 16`): **horizontal** bars (`h <= w`) fill
  left→right; **vertical** bars fill bottom→up `[orig: @0x5997a9..0x59981f]`.
  Fill color = `g_stanceColorBad @0x2723AE4` — the `stancecolor_bad` hudpos
  token, shared with the health-bar/vehicle-bar "bad" color.

## Waypoint HUD — the track + the HUDWPDINFO label (witnessed 2026-07-18)

The player-facing waypoint system: a **track** (list + current entry) over
pool-3 marker entities, the name+distance HUD label, and the mission-logic
show/hide gate. JO:CA ships **no in-world waypoint markers and no compass
strip** — both exist in the binary as dead code (below); the HUD label and the
map overlay are the entire display surface.

### The waypoint list

- **Source (waypoint gametypes `(g_GameType & 0xFFFDFFFF) == 0x10020`, which
  includes SP/co-op `0x30020`)**: the server serializes the S2C 0x0F
  world-state-load waypoint block from **the nav-channel table** (`@0xA71DD4`,
  the same 34-dword `{flags, count, entries[32]}` waypoint-route records the
  AI patrol port models — net-re §5.29): the **first channel with `flags & 2`**
  is the player route (bit0 = the one-shot flag; bit1 = player-route), its
  index stamped into the local player's AiSlot+148
  `[orig: NetPacket_WriteWorldStateLoad0x0F @0x502e41]`. Records (≤128):
  `{u16 pool-3 slot, u16 nameId = entity+672, u8 done = entity+536}`, present
  only when the recipient's team byte (+354) is 1. The client apply resolves
  each slot via `Pool_GetEntryUnchecked(3, slot)` into `g_waypointList
  @0xB76570` (count `g_waypointCount @0xB76568`) and re-resolves
  `WPNames/STRWPNAME%03i(nameId)` into the entity's inline name (entity+244,
  15 chars + NUL) `[orig: NapiNPClientMsg_0x00F @0x42e4b8]`.
- **Non-waypoint gametypes**: the same storage holds the MP POI list instead —
  spawn/capture/flag entities by def attribs (0x8000, 0x80000) and type ids
  (4091–4103 flags/bays, 6026–6028/6092/6093 zones)
  `[orig: Entity_BuildMapPoiLists @0x42de40]`, rebuilt on net team events.
  Spectator target cycling reuses the list.
- **BMS marker fields** `[orig: Entity_SpawnFromBMSRecord @0x40f0aa]`
  (record → pool-3 entity): radius `entity+0 = (rec+0x1C) << 16`, default
  `0x8000` (0.5 u) when unset; wp name id `entity+672 = rec+0x60`; linked
  event `entity+528 = word rec+0x44`; chain-back flag `entity+535 =
  rec+0xC bit 22`; done flag `entity+536 = 0`; the resolved
  `WPNames/STRWPNAME%03i` inline name at entity+244. Type `0x7FC` (2044)
  markers also register deploy-map location names (net-re §5.29).

### Current waypoint + advance

`g_currentWaypoint @0xB7656C` (ex kong "entityDef" — renamed; it is the
current waypoint/POI **entity pointer**), reset by
`[orig: Game_InitNewRound @0x422772]`.

- **Proximity auto-advance** `[orig: Player_UpdatePerFrame @0x4de5f7]` — runs
  only for waypoint gametypes with `count > 1`: a non-authority client first
  forces `current->linkedTrigger = -1`; a waypoint whose linkedTrigger is
  still `>= 0` (authority side) does **not** proximity-advance — its
  completion is event-driven. Otherwise, when
  `SpawnPoint_CheckWeaponRestrictions @0x4dbe80` passes: advance
  (`Spectator_CycleTarget(1)`) when the NovaLogic horizontal approx distance
  (`max(|dx|,|dy|) + min(|dx|,|dy|)/2`, entity X/Y deltas) is `< radius`
  (entity+0) **and** current is not the LAST list entry. A null current
  latches `list[0]` then skips done entries.
- **Event-driven completion** `[orig: EventTrigger_MarkLinkedSpawnPoints
  @0x452ce0]` — authority-side, called immediately after a BMS event fires
  its actions `[orig: EventTrigger_UpdateEntry @0x454cbd/@0x454d25]`: every
  waypoint whose `linkedTrigger` equals the fired event's index gets
  `done=1`, then prior entries chain backward while their `+535` flag is set;
  `SpawnPoint_SkipBlocked @0x4de310` then forward-cycles current past done
  entries.
- **Manual cycle** (input action 23 `[orig: @0x49b3de]`): in-session = plain
  `Spectator_CycleTarget(dir)` (skips null-def entries, wraps; KOTH modes
  also skip dead flag-0x8000 entities). SP (`!is_in_session`) is gated on
  `Game_GetShowWaypoints` and constrained: forward only while current is
  `+535`/`+536` flagged; backward never below `list[0]` and only onto
  `+535`-flagged entries (else the cycle reverts).

### The HUDWPDINFO label — `HUD_DrawWaypointNameAndDistance @0x5947a0`

- **Gates** `[orig: @0x5a7daf; death-screen leg @0x5a7c29]`:
  `g_showWaypoints @0x27238BC` (init **1** at
  `[orig: HUD_InitOverlaySystem @0x5a4913]`; the BMS `ShowWaypoints` action 40
  writes it via `[orig: Game_SetShowWaypoints @0x58fb50]`, getter
  `@0x58fb60`) `&& dword_2723C8C` (static -1) `&& g_GameType != 0x10010`,
  plus a non-null current that is present in the list.
- **Layout**: the `HUDWPDINFO <x> <y> <hideBox> <align>` hudpos token
  (`g_hudWpdInfoX/Y/HideBox/Align @0x2723694..0x27236A0`,
  `[orig: parse @0x5a02c3]`; align via `HUD_ParseTextAlignment` right=1
  center=2). Field 3 hides only the wireframe **box**, not the element.
- **Distance** = 2D `sqrt(dx²+dy²)` of entity X/Y deltas (through the info
  struct's entity pointer), fixed→int meters (`>>16` with the signed
  correction) `[orig: @0x5947e5..0x594836]`, rendered `"%d"` right-aligned at
  the anchor.
- **Name** = `[orig: get_waypoint_name @0x594630]`: wp_index = entity+672,
  **+1 unless `g_GameType & 0x20000`** (co-op keeps the raw index). In-session
  specials from gametext `WPNames`: attrib `0x80000` → `STRWPNAMEARMORY`,
  `0x8000` → `STRWPNAMETARGET`, type 4091/4093/4095–4097 → `STRWPNAMEFLAG`,
  4098/4100–4103 → `STRWPNAMEFLAGBAY`. Fallback: **mission text**
  `WPNames/STRWPNAME%03i` (the `<mission>.bin` table); empty or `"null"` →
  gametext `STRWPNAMEDEFAULT`. CTF (`g_GameType == 0x10004`) wraps flag names
  in team colors (`<c4050FF>`/`<cFF3535>` by type id); a squad suffix
  `"%c-%d"` (letter = low 5 bits + 'A'-1, number = top 3 bits of entity+538)
  applies for def+84 `&0x40000` entities.
- **Draw shape**: align 0 = name left-aligned at `x + distWidth + 4`; align 1
  = name right-aligned at x, anchor shifts left by `textW + 4`; align 2 =
  name left at `x - 4`. Unless hideBox: a wireframe rect
  `(x, y-2)..(right, y+textH-1)` `[orig: @0x594b0c]`. Distance draws
  right-aligned at the (possibly shifted) anchor. Text via the half-bright
  pair `[orig: HUD_DrawTextRightAligned_HalfBright @0x580850 /
  HUD_DrawTextLeft_HalfBright @0x5804c0]` in the master overlay color.

### Dead code (do not port)

- `[orig: draw_entity_labels @0x593820]` — in-world number/line/circle
  markers over the current + next waypoint (terrain-clamped): **no callers**
  in JO:CA.
- `[orig: HUD_DrawCompassStrip @0x595470]` — the heading strip with waypoint
  carets (reads `g_showWaypoints @0x595c9f`): **no callers** in JO:CA.

## Objectives panel — `HUD_DrawWinConditions @0x5ba940` + the subgoal state (witnessed 2026-07-18)

The SP/co-op **MISSION OBJECTIVES** overlay and the subgoal state machine
behind it.

- **State**: four per-slot bit masks — won `@0xAC86F4` / lost `@0xAC86F0`
  (reader `EventSystem_GetEntityCounts @0x452e10`), show-win `@0xAC86EC` /
  show-lose `@0xAC86E8` (reader `EventSystem_GetTeamCounts @0x452e30`) — all
  cleared by `EventSystem_FreeAll @0x453362`. Bits use the **RAW 1-based
  slot** (slots 1..8 → bits 1..8). The per-slot text-id tables are BMS header
  bytes: `byte_A7628B[1..8]` = header `win_conditions[0..7]` (+0xBC),
  `byte_A76293[1..8]` = `lose_conditions[0..7]` (+0xC4).
- **Actions** `[orig: EventAction_Dispatch]`:
  - **14 SubGoalWon(slot)** `@0x454500` — the already-won bit SKIPS the whole
    case (no re-announce) `@0x45450a`; else set + a score add (header
    `win_scores[slot]` × 100 — the callee `@0x454532` carries a kong
    "Score_AccumulateBandwidth" misnomer; unported) + when the round is still
    running (`g_spawn_success_gate == 0` `@0x45453a`) a chat line = mission
    text `WinConditions/STRWINMSG%03i(win_id)`; a header unknown5[2] per-slot
    mask (`byte_A762D6`, slot−1 bits) adds a team-banner leg (unported).
  - **15 SubGoalLost(slot)** `@0x4545e0` — NO already-set guard; set + chat
    `LoseConditions/STRLOSEMSG%03i(lose_id)` **+ the persistent banner**
    (`GameMsg_SetBannerText @0x454647`); `byte_A762D7` team-banner leg
    (unported).
  - **35/36 ShowWin/LoseSubgoal(slot, bool)** `@0x4546af/@0x454724` —
    set/clear the show bit; always fire
    `HUD_ShowObjectiveNotification @0x5ba2e0`-family ("New Objective" toast +
    authority broadcast; unported toast).
- **The panel** `@0x5ba940`, called from `HUD_DrawGameplayOverlays @0x5be163`
  gated on the toggle `dword_24C18CC` (an alpha byte 0/255, flipped
  `^= 0xFF` by the co-op input action `@0x49b68b`; the binding row rides the
  unported input layer): gametype gate co-op/waypoint
  (`(g & 0xFFFDFFFF) == 0x10020 || g & 0x20000`); header = gametext
  `Overlays/STROVER_MISSIONOBJECTIVES`; rows = slots 1..8 until win id 0/255,
  each drawn only while its show-win bit holds; text = mission
  `WinConditions/STRWINCOND%03i(win_id)`. FULL GEOMETRY (disasm 2026-08-12 —
  the previously elided operands; x/y are the CALLER's arguments, x=15,
  y=+0xF0 off `dword_24C1900` at the `@0x5be163` site):
  - Measure pass `@0x5ba9c9`: per shown row, `HUD_MeasureTextWH` with
    `g_hudLabelFontLarge`, height scaled `(h<<10)/overlayCtx` accumulates the
    panel height; max width over the rows AND the header (header measured with
    fontLarge but DRAWN with `g_hudLabelFontBold` — a witnessed asymmetry).
  - Backing box `@0x5baaba`: `HUD_DrawLabelBox(ctx, x, y−0x18,
    x+scaledMaxW+0x48, y+totalTextH+0x30, 0, (alpha<<24)+0xFFFFFF)`.
  - Header `@0x5baae4`: `Render_DrawTextScaled` at (x+0x18, y), fontBold,
    `(alpha<<24)+0xFFFFFF`; rows start at y+0x18 and each advances by ITS OWN
    scaled measured height (`esi += heights[slot]` `@0x5bacc1`), not a constant.
  - Checkbox `@0x5bab47..0x5bab9a`: a 16×16 outline at (x+0x18, rowY), four
    `draw_clipped_2d_line` calls, color `0xFFE0E0E0` with the panel alpha as
    the separate modulate arg.
  - The done mark `@0x5babc1..0x5bac5b` is a **RED X** (color `0xFFFF0000`),
    NOT a checkmark: both diagonals of the box, each drawn three times with
    one-pixel offsets for thickness — six lines total.
  - Row text `@0x5bacb5`: (x+0x30, rowY−2), fontLarge, color =
    `(won ? 0xFF808081 : 0) + 0xFFFFFF + alpha·0x1000000` (wraps to the
    **gray 0x808080** for completed rows at any alpha).
  (KOTH's separate directive list = `draw_koth_win_lose_directives @0x5ba370`,
  unported with KOTH.) PORT (2026-08-12): `HudFrameCompiler::element_objectives`
  carries the witnessed rect/checkbox/red-X geometry, the exact color folds,
  and the panel alpha byte (`HudFrameState::objectives_alpha`, folded into
  every draw color); the presenter's show/hide toggle stands in for the input
  binding row (alpha 0 ≡ hidden), `HUD_DrawLabelBox`'s internal box-shader
  styling keeps the fill+wire stand-in at the witnessed rect, and the
  fontLarge/fontBold slots ride the compiler's single HUD font until the font
  plumb lands.

## Divergence catalog (D-HUD)

| ID | Ours / reference | Original (Jointops.exe) | Why / consequence |
|---|---|---|---|
| D-HUD-1 | IDB curated name `draw_minimap_compass_overlay`; the oscarmike reference models a "spinmap" compass | `HUD_DrawStanceIndicator @0x599f10` renders the **stance** indicator, keyed by `byte_27235C0` = `hudInfo+568` stance index | The function is mis-named in the IDB and mis-modeled in oscarmike. The OpenNova stance widget must be the discrete cross-faded `HUDSTANCE` frames, not a compass. Rename proposed (held). |
| D-HUD-2 | oscarmike `spinmap.gd` = a single rotating compass-ring texture | the stance widget is discrete pre-rendered frames cross-faded on stance change. 2026-07-18 correction: the "`@0x599700` radar" this row pointed at is the **weapon heat bar** (`HUD_DrawWeaponHeatBar`); JO:CA has **no in-HUD radar or compass** — the compass strip (`@0x595470`) is dead code, and heading/map display lives only in the map overlay (`HUD_DrawMapOverlay @0x5a5f40`) | Do not port a rotating ring, and do not port any always-on radar element. Stance = frame swap with fade; map/heading = the map overlay (follow-up). |
| D-HUD-3 | — | Design space is fixed **1024×768**, scaled with round-to-nearest (`Viewport_ScaleToVirtualCoords @0x5d2b20`) | OpenNova authors HUD positions in 1024×768 and scales to the actual surface with the `(p*s+½s)/dim` rounding. |
| D-HUD-4 | — | Health bar *fill width* uses the capped `+92` ratio; *fill color* uses an uncapped recomputed ratio (`HUD_DrawHealthBar @0x5a2e50`) | Equivalent over `[0,1]`; recorded so the port matches both reads rather than collapsing to one. |
| D-HUD-5 | `hud_clip_indicator.gd` restamps its flash on (`round_type`, reserve) change | restamp keys are (`weapondef+220` ammo class, reserve, `weapondef+216` pool id) `[orig: @0x599ab2]` | Our weapon model runs a single ammo pool (net-re D-WPN-2), so the ammo-class/pool ids aren't distinct state yet; the proxy fires on the same reload/switch transitions. Revisit with per-class pools. |
| D-HUD-6 | `hud_messages.gd` is a timed line feed (930-tick life, ≥186 stagger, wrap, two-space continuation indent) drawn at the `HUDCHATTEXT` anchor | triggered text rides the full chat system: channel-2 ring buffers `[orig: Chat_AddDebugMessage @0x4987f0]`, display rebuild `[orig: @0x498bd0]`, and a channel geometry table (`dword_28E4DF8`, writer unwitnessed) | Message-line altitude port. The channel's exact screen geometry, per-line fade curve, and the player-chat channel are the chat-pipeline follow-up. |
| D-HUD-7 | **CLOSED 2026-07-31.** The HUD consumes the exact ERROR integer plus the sim's signed `pitchBlend(+0x380)>>7` and movement/weapon-weight `(+0x384)>>7`; the same live `pitchBlend` feeds the first-person camera and local/decoded-player aim overlays | spread adds `(player+0x380 >> 7) + (player+0x384 >> 7)` `[orig: HUD_DrawCrosshair @ 0x592640]`; the write/decay side is `[orig: RoundData_SpawnRound @ 0x4ec0d0]` + `[orig: Entity_UpdateInfantryPlayerBody @ 0x4b40e0]` | **FIXED.** Exact integer carriers now run ammo/weapon parse → runtime tables → round/body sim → local HUD/camera/overlay; decoded rows stamp and decay recoil, while their movement term remains the retail zero of a local-only producer. The projectile's intentionally different `R>>8` stays separate. The water-height category's position-only `CameraOffset.Z` projection remains D-INF-18, not D-HUD-7. Pinned by `npruntime_round_sim`, `infantry`, `netsim_client_replica_pipeline_recoil`, and `hud_helpers_test.gd`. |
| D-HUD-8 | crosshair color multiplies the texture (canvas modulate); default white | the strip writes the color to the **specular** channel with `diffuse = 1.0` `[orig: @0x5914d7]`; the blend-stage setup lives in the HUD shader pass (`GfxShader_ApplyPassChecked @0x677020`, unwitnessed); color source = user config `dword_25510E0` | Identical for the default white; witness the texture-stage state (and the config default) before modeling the user crosshair color. |
| D-HUD-9 | **CLOSED 2026-07-31.** The crosshair previously hid from generic settled ADS | it draws while an aimed shot is NOT available — `!Player_CanFireWeapon() @0x5cf780`, whose promoted predicates are Scoped (`Flags & 1`) or Sighted (`Flags & 2`, except SWITCHFROM); movement/water reject only the ordinary Scoped leg, while reload-card-switch, camera, dead/airborne, ForceScoped, and seat gates complete the verdict | **FIXED.** The sim now stamps that bounded retail verdict once and feeds both visibility and ERROR row selection. The reticle remains through ADS ease and follows the witnessed Scoped/Sighted failure/override gates rather than raw `scope_engaged`. |
| D-HUD-10 | the crosshair anchors at the fixed design center (512, 384) | the anchor is the projected aim point through `Viewport_ScreenToVirtual`: the literal screen center only for the on-foot local player with no camera mode `[orig: @0x5928a0]`; spectate / `g_camera_mode` (external/3P) project `Entity_BuildCameraView` (far point 65536000 q16 = 1000.0) `[orig: @0x592910..0x59295e]` | FIXED 2026-07-11 (weapon round): `LocalPlayerPresenter.aim_screen_point()` — `Vector2.INF` in first person (the HUD pins the exact center, matching `@0x5928a0`), the projected aim in third person; `GameHudPresenter` feeds it to both shells. |
| D-HUD-11 | the label nearest-only gate models `equipped_adm_index != 0xFF` + not-in-a-ctrl/drvr-seat (`Simulation::get_attach_labels`) | `Player_CanFireWeapon @0x5cf780` additionally requires no camera mode (`g_camera_mode`), not underwater (`Position.Z + CameraOffset.Z < Env_WaterHeightFixed` with the 0x8000 flag), and the settled-scope legs | The extra legs are presentation/render state the sim doesn't carry; on foot with a weapon the observable difference is the underwater/camera cases. Wire when those states reach the sim. |
| D-HUD-12 | **FIXED 2026-08-11.** Attach labels lay out through the ported CGameFont engine with the witnessed BOLD Arial label font at the slot scale (`HudFrameCompiler::element_attach_labels` + `configure_label_fonts`) | `HUD_MeasureTextWH @0x580ab0` measures through the bold slot's (`g_hudLabelFontBold @0xB4C394`, ex "fontObj") `{handle, scale_x, scale_y}` pair (`CGameFont_MeasureText @0x674e70`); labels draw at raw screen pixels | Same glyph walk, same font file, same scale; box arithmetic `(x−w/2,y−2)..(x+w/2+5,y+h+1)` ported verbatim. Pinned by ctest `hud_frame_compiler` (label-font faces/scale). |
| D-HUD-13 | **CLOSED 2026-08-10.** The label color base is the hudpos `hud_textcolor` | the master overlay color `g_hudActiveColor @0x24c1868` — writer NOW WITNESSED: `= g_hudColorTable[cfg_hud_color_index] \| 0xFF000000` (`HUD_InitTeamColorTable @0x51f240`; the input case 10 cycle `@0x49afc7`; config token `hud_color_index`, default **2** `@0x54d28b`), and table slot 2 is refreshed per frame from hudpos `hud_textcolor` (`@0x5a8100`) | **The reimpl base was already exact**: under the retail default scheme the master color IS `hud_textcolor`. The dim transform stays ported (`HudAttachLabels.dim`). Residue: the non-default schemes (table 0 white / 1 green / 3 light blue / 4 yellow / 5 salmon) ride the unported `hud_color_index` config cycle — recorded in the D-HUD-20 residue list. |
| D-HUD-14 | no bottom prompts | `HUD_DrawGameplayOverlays @0x5bde60`: the preround armory prompt (`STROVER_ARMORY_INFO`, S2C-0x0A-fed `dword_A85B64`; SP never draws it), the vehicle-bay prompt (`Flags & 0x800` + team-gated groundEntity), the FARP wait/reload overlays (`attrib2 & 0x2000` + unlock mask) | Witnessed, deferred: each rides an unported system (MP preround state / vehicle.mnu / FARP rearm). The `STROVER_ARMORY_WAIT` leg is dead code in retail (the impossible `@0x5bdef8` recheck). |
| D-HUD-15 | **CLOSED 2026-07-22.** The drawer was already parity-complete; the missing half was the source. The accumulator is now witnessed and ported (D-WPN-4, net-re §5.62): heat is a DEADLINE on the slot, `def+880 × (slot+0x14 − tick)`, stamped once per shot by the recoil arbiter | heat = `WeaponSlot_CalcAccumulatedHeat @0x53f780` per frame, clamped to `0xFFFF` into `hudInfo+60` `[orig: HUD_BuildEntityInfo @0x4b852e, clamp @0x4b854d]` | Fed sim → weapon view → HUD with the clamp applied where the original's info builder applies it. The bar fills on the thirteen emplaced/vehicle guns that author `heat_values` and stays hidden on foot, because no infantry weapon authors heat in retail either. |
| D-HUD-16 | the SP waypoint track is built sim-side at mission load from the BMS nav channel (`flags & 2`) + pool-3 markers — no 0x0F wire leg in the loop | retail always routes the list through the S2C 0x0F apply, even in SP mode 3 (the local server serializes, the local client applies) | Same data, same selection rule, no serialization round-trip. The npwire 0x0F waypoint block already decodes (net-re §5.29); wire-parity for MP join is the npwire follow-up, not a HUD divergence. |
| D-HUD-17 | proximity advance ports the distance/last-entry/skip-done legs; `SpawnPoint_CheckWeaponRestrictions @0x4dbe80` (the AAS spawn-point weapon-restriction pass gate) is modeled as always-pass; the MP POI list (`Entity_BuildMapPoiLists @0x42de40`) and spectate reuse are unported | the restriction check reads the 4 weapon slots vs the event-system restriction mask and can force-advance | SP missions author no weapon restrictions on route markers; port the check with the AAS/MP HUD phase. |
| D-HUD-18 | GEOMETRY CLOSED 2026-08-12: the checkbox (16×16, 4 lines, 0xFFE0E0E0), the done-mark RED X (6 lines, 0xFFFF0000 — the old "checkmark" gloss was wrong), the `HUD_DrawLabelBox` rect (y−0x18 / +0x48 / +0x30), the measured-height row advance, and the exact alpha/gray color folds are ported into `element_objectives` with the panel alpha byte carried in the frame state. Remaining: the win-score add `@0x454526` (no score system), the "New Objective" toast `HUD_ShowObjectiveNotification @0x5ba2e0` (internals unwalked), the header unknown5[2]/[3] team-banner legs (`byte_A762D6/D7`), the KEY_O reimpl binding (input layer), `HUD_DrawLabelBox`'s internal box-shader styling (fill+wire stand-in at the witnessed rect), and the fontLarge/fontBold slot plumb (single HUD font stand-in) | disasm 2026-08-12 (the full drawer); score add `@0x454526`, toast `@0x4546e2` call site, banner masks `byte_A762D6/D7`, binding row = the input layer | The state machine, row walk, geometry, color folds, and chat/banner announcements are exact; the residuals each ride an unported system (score / notification / banner / input binding) plus the two cited drawer stand-ins. |
| D-HUD-19 | the DEATH deploy screen (`DeployScreenPresenter`, death.mnu) ships the authored chrome, the witnessed SPAWNPOINTS_LIST populate, and the pick flow — its MAP window renders no map image | the MAP window's render pass draws the windowed map view `MapOverlay_DrawView @0x5a58e0` (terrain layers + blips + labels; pan/zoom via `command_map_overlay_input_handler @0x554310`), the sibling of the fullscreen `HUD_DrawMapOverlay @0x5a5f40` | The pick behavior is complete without the image (the list is the pick surface); the map draw internals are the tracked next map-phase witness — port `MapOverlay_DrawView` and feed both the CMAP and DEATH windows from it. |
| D-HUD-20 | **FIXED 2026-08-10** (core). The friendly tags (overhead name labels) are ported end to end for the SP/AI path: `world::collect_friendly_tags` → `Simulation::get_friendly_tags` → presenter projection/fog/KEY_F cycle → `HudFrameCompiler::element_friendly_tags` — the witnessed gates, health-tier colors, distance alpha, centered alpha-preserving half-bright text, the `'^'`+36-name fallback, the BMS→`[PeopleNames]` authored names, the BRIEF ticks, and the medic cross plate (feed pending) | the drawer is `HUD_DrawEntityLabel @ 0x5a39b0` off `HUD_DrawFriendlyTagsPass @ 0x5a4480` (full witness: the element map section). The 2026-08-10 hunt's dead ends stay recorded: `hud_draw_target_entity_overlay @ 0x59a5d0` = the targeted-GEAR overlay, `sub_599C20 @ 0x599c20` = the scope quad | Residues, each with its owning system: (a) the player-slot walk legs — callsign labels ride our MP roster, plus squad colors `@0x83B450` (middle × 0.7), the flag-2 `"%s: %ld"` slot+16 count, the `<ch>`channel`<co>` wrap, and the slot+44 pulse; (b) the enemy magenta leg behind the server-granted `g_enemyTagsVisible @0x24D1DF4` (spectator/S2C 0x00A); (c) the medic-plate FEED — charattr.def `ATTRIBUTES` Medic(0x8) by `playerClass` (`CharAttr_LoadFromDef @0x412140`; AI classes unmodeled sim-side); (d) the wounded icon — `entity+885` writer unwitnessed (texture id 0x17); (e) the `hud_color_index` scheme swap for the good tier (default 2 modeled; D-HUD-13); (f) the speaking-pulse LEVEL feed (formula ported; the dialog-channel amplitude is a device follow-up); (g) `entity+116` display height (writer unwalked; standing-constant stand-in) and the difficulty term of `Entity_GetMaxHealthWithDifficulty`; (h) the death-screen recolor/center-pin legs and the `0x27233DC/E0` latch bits (consumers unwitnessed). |

## Follow-ups (not yet witnessed / deferred)

- **Map overlay** `[orig: HUD_DrawMapOverlay @0x5a5f40]` (7278 B, called from
  `HUD_RenderAllOverlays @0x5a87bb`) — the fullscreen/deploy map: terrain
  render, grid coordinates (`HUD_FormatGridCoordinate`), ring blips
  (`minimap_draw_ring_blip`, `update_radar_contacts` — the §5.19/§5.59
  MinimapSlot tables), `draw_compass_indicator`, waypoint labels
  (`WPNames/STRWPNAME%03d`, `STROVER_OBJECTIVEPOINT_SHORT`,
  `STROVER_DEFENSIVEPOSITION`), `%01.2fk` distances, `MapOverlay_RenderAllLayers`.
  Structured this session; the draw internals are the next map-phase witness.
  (The old "radar/minimap @0x599700" pointer was the heat-bar misnomer —
  resolved 2026-07-18, see D-HUD-2.) 2026-07-24 addition: the WINDOWED sibling
  `MapOverlay_DrawView @0x5a58e0` (ex-sub_5A58E0; `(rect, centerX, centerY,
  scale)`) is the map view the CMAP command-map screen and the DEATH deploy
  screen draw inside their .mnu MAP windows (net-re §5.61's screen witness);
  the ported deploy screen ships chrome-only until this draw is witnessed —
  D-HUD-19.
- **Chat channel geometry** — the `dword_28E4DF8` table (rows 1/2 chat, 3/4
  system/debug) that `Chat_RebuildDisplayBuffers @0x498bd0` wraps against and
  the drawer anchors with; its writer is unwitnessed (D-HUD-6).
- **Crosshair color config** — `dword_25510E0` default + the HUD shader pass
  texture-stage state (D-HUD-8); the crosshair styles' count (`cross%02d.tga`).
- **Crosshair sub-elements** — the target-tracking cursor
  (`@0x592790..0x592875`), the `weapondef+12 & 0x80` aim-point quad
  (`@0x592973..0x592ac8`), and the lock brackets (`@0x592ce2..0x592dd7`);
  witnessed 2026-07-11, unported (MP/vehicle/lock phases).
- **`dword_24C1930` flag 0x10000** — replaces triggered/gametext strings with
  `"&"` (`@0x51f1c8`/`@0x51ebe3`); the writer is unwitnessed.
- **`mp_CrossHairSpread` default** — `dword_25510E4` (spread disabled still
  draws the assembled reticle at offset 0); the config default is unwitnessed
  (the port models enabled).
- **Scope overlay** — the scope view's reticle/mask (`scopexh.tga @0x59e133`,
  weapon sights), which replaces the HUD crosshair when scoped.
- **`entity+885` (the wounded/assist state the tag icon reads)** — the D-HUD-20
  wounded-icon gate (`+885 && !Entity_FindChildByDefType(e,1,1)`, viewer mount
  kind 2/5 or own +885); its writer is unwitnessed (net-re records a
  sector-action 30-tick timer at the same offset on pool-3 entities).
- **`entity+116` eye-offset writers** — RESOLVED 2026-08-11: the +0x6C/+0x74
  triple is the per-tick eye/camera offset (`Entity_UpdateInfantryAI
  @0x4bf078..0x4bf14c`; `Entity_UpdateInfantryPlayerBody @0x4b6984..0x4b696c`;
  seeds `@0x4e18a1`/`@0x42ffc9`) — see the friendly-tags anchor bullet. Still
  open here: the local head-bone exact leg and the lateral lean shift are
  unported (tag consumers never read them for the drawn population).
- **CGameFont glyph layout** `[orig: CGameFont_DrawText @0x6752c0]` — per-glyph
  D3D vertex build / spacing, to confirm `FntResource` layout parity.
- **Timer/score, altitude/power bar, weapon slot bar** — enable flags
  witnessed; draws (`HUD_DrawWeaponSlotBar @0x599cd0` located) unwitnessed.
  (The ex-"reload bar" is the PowerThrow charge bar — witnessed + ported,
  world-wac-ai-re §27.3.)
- **Weapon heat source** — RESOLVED 2026-07-22. Both halves are now ported:
  the HUDHEAT drawer (D-HUD-15) and the accumulator behind it
  (`WeaponSlot_CalcAccumulatedHeat @0x53f780`, the `heat_values`/`heat_effect` def
  keys, the per-shot deadline stamp, and the overheat deny) — witness in
  `docs/net/novaworld-net-re.md` §5.62, divergences D-WPN-4/-28/-29. The one
  unported leg is the overheat GLOW emitter (D-WPN-28), which is a muzzle-effect
  seam rather than a HUD element.
- **MP objective status + stance team tile** — witnessed
  (`draw_objective_status_text @0x59aa30`, `@0x59a0cb`); port with the MP HUD.
- **Parachute/armor icons** — witnessed
  (`HUD_DrawParachuteAndArmorIcons @0x5925c0`): entity+44 `&0x10` parachute /
  `&0x8` armor (through the info struct's entity pointer; the +44 writer is
  unwalked — the earlier "+36" note was a recon guess, corrected 2026-07-18).
- **Armory-delay label leg** — the `g_hudLabelFmtArmoryDelay % dword_A85B6C`
  variant of the floating armory label and the bottom-prompt cluster
  (D-HUD-14) key off the S2C 0x0A header armory/preround state; wire when the
  net views surface it.

## IDB changes

Applied 2026-08-11 (the D-HUD-20 eye-offset + label-font hunt; anchored
renames; IDB saved):

- **Rename** `sub_580400` → `HUD_LoadFontIntoSlot` (anchored: allocates the
  CGameFont, loads via `File_LoadResource`, stores the slot scale floats).
- **Data renames** `fontObj @0xB4C394` → `g_hudLabelFontBold` (anchored:
  `HUD_InitAllFonts` loads the Arial bold tier into it — the placeholder name
  said nothing), `dword_B4C3AC` → `g_hudLabelFontImpact38` (anchored:
  `Impac38b.fnt`).
- **Comments** at `0x51ee20` (the four font slots, names + divisors),
  `0x4bf078` (the org1 eye-offset restamp math + port back-ref), `0x4e18a1` /
  `0x42ffc9` (the local eye-offset seeds), `0x580400` (the slot layout).

Applied 2026-08-10 (the D-HUD-20 friendly-tags hunt; auto-name renames at
anchored confidence; IDB saved):

- **Rename** `render_entity_glow_labels @0x5a4480` → `HUD_DrawFriendlyTagsPass`
  (anchored: the STRMISC_FRIENDLYTAGS toast strings + the mode global; the old
  agent-era name was a misnomer — there is no glow here).
- **Rename** `HUD_ClassifyDistanceLOD @0x59c1f0` → `HUD_ClassifyHealthBand`
  (anchored: both callers pass health ratios; the thresholds are the health
  bar's 0xC000/0x6FFF bands).
- **Renames** `sub_580720` → `HUD_DrawTextHalfBrightF`, `sub_580A80` →
  `GameFont_MeasureCharHeight`, `sub_599630` → `HUD_DrawRotatedIconQuad`,
  `draw_textured_quad_with_border_0 @0x59bcb0` → `HUD_DrawMedicCrossQuad`
  (anchored: bodies read).
- **Data renames** (anchored: token strcmps / init immediates / toast strings):
  `g_friendlyTagsMode @0x24C18C4`, `g_fallbackPeopleNames @0x840A78` +
  `g_fallbackPeopleNamesCount @0x840A0C`, `g_voicePlaybackEntity @0xC6EC38`,
  `g_enemyTagsVisible @0x24D1DF4`, `g_hudposTagcolorBlueteam/Redteam/Good/
  Middle/Bad @0x2723AC8..AD8`, `g_hudposTextColor @0x2723AC0`,
  `g_hudColorTable @0x24C1838`, `g_hudActiveColor @0x24C1868` (ex "alpha"),
  `g_hudColorLightBlue @0x24C1844` (ex "color_rgb"), `g_hudColorGray
  @0x24C1858` (ex "depth"), `g_hudLabelFont @0xB4C388` (ex "mantissa"),
  `g_hudLabelFontLarge @0xB4C3A0`, `cfg_hud_color_index @0x2550BCC`,
  `g_playerSlotPtrTable/Count @0xA822D0/D4`.
- **Comments** at `0x5a4480`, `0x5a39b0` (the drawer summary + reimpl
  back-ref), `0x51f240` (the color-table semantics), `0x49b573` (the mode
  cycle), `0x4a6358` (the FriendlyTag host option → mpattrib 0x400),
  `0x4ece03` (the speaking-entity stamp), `0x40ecbf` (the PeopleNames name
  resolve), `0x2723ac0` (hud_textcolor → table[2] → the master color).

The 2026-07-31 recoil/spread grill was read-only; it made no IDB changes.

Applied 2026-07-11 (auto-name renames at anchored confidence + appended
comments; IDB saved):

- **Rename** `sub_580680` → `HUD_DrawTextCentered_HalfBright` (anchored:
  drawFlags init 1 `@0x58069d`; `CGameFont_DrawText` flag-1 center
  `@0x675518`).
- **Rename** `sub_5804C0` → `HUD_DrawTextLeft_HalfBright` (anchored: drawFlags
  init 0 `@0x5804da`).
- **Comment** at `0x592b87`: the ERROR row select = `stance +
  3*Player_CanFireWeapon()`. The 2026-07-19 re-witness supersedes the old
  vehicle-only interpretation: `0x4dcd30` is the Sighted predicate with no
  mount gate, alongside the Scoped predicate at `0x4dcc80`.
- **Comment** at `0x51f1c8`: the `dword_24C1930 & 0x10000` → `"&"` replacement
  quirk (writer unwitnessed).

Applied 2026-07-16 (repo hygiene pass — the maintainer's apply-the-held-IDB-updates
call; IDB saved):

- **Rename** `draw_minimap_compass_overlay @0x599f10` → `HUD_DrawStanceIndicator`
  (anchored: indexes `HUDSTANCE` frames by `byte_27235C0 = hudInfo+568` stance
  index; D-HUD-1), with the misnomer + stance-keying comment at `0x599f10`.
- **Rename** `draw_minimap_compass_ring @0x5d17a0` → `Hud_DrawScopeCircleMask` —
  the 64-segment circular scope mask drawn for Scoped weapons that author no
  SIGHTS rows (net-re §5.62). Re-witnessed 2026-07-19: the
  `Render_ProcessMainSceneFrame @0x5cab15` caller is the fallback after the
  post-clear standard Scoped selector's SIGHTS-row call reports no authored
  rows; a nonzero count suppresses it even if a texture handle is missing. The
  second caller remains `render_hud_overlay @0x5d82f2`. The sibling reticle drawer
  `draw_minimap_crosshair_and_grid @0x5d1160` keeps its name (second caller
  unwitnessed) and carries a candidate-rename comment for the next HUD grill.
- **Comment** at `0x59e3d6` noting `dword_25510DC` = the user crosshair-style
  index (`cross%02d.tga`), `dword_25510E0` = the user crosshair color, and
  `dword_25510E4` = the `mp_CrossHairSpread` enable.

Applied 2026-07-17 (the attach-label grill; auto-name renames at anchored
confidence; IDB saved):

- **Rename** `sub_5460E0` → `Entity_GetWeaponSlots` (anchored: def+84 flag
  routing to vehicle +1140 / infantry +692 / parent +1140 slot arrays;
  callers `WeaponSlot_ReloadAmmo`/`Server_ClientFiredRound`/the label draw).
- **Rename** `sub_5D3EC0` → `HUD_DrawTextAtVirtualPos` (anchored: the inline
  `(x*w+512)/1024` / `(y*h+384)/768` virtual-space scale + text dispatch).
- **Rename** `sub_580AB0` → `HUD_MeasureTextWH` (anchored: the
  `CGameFont_MeasureText` wrapper returning the w/h pair).
- **Rename** `dword_2723860/64/68/6C/70` → `g_hudLabelTextSit` /
  `g_hudLabelTextControl` / `g_hudLabelTextUseGun` / `g_hudLabelTextUseArmory`
  / `g_hudLabelFmtArmoryDelay` (anchored: the `HUD_InitOverlaySystem`
  STROVER_* writers + the label-draw readers).
- **Rename** `byte_81A454` → `g_bindingRow_useitem` (anchored: the row whose
  +0x14/+0x16 runtime keys are the armory grill's
  `g_useItemBindingKey0/1 @0x81A468/6A`; both prompt legs format it).
- **Comments** at `0x5a3290` (label sources/gates summary), `0x5bdee2`
  (preround gate + the dead ARMORY_WAIT leg), `0x435d50` (the armory
  searchMode's only consumer), `0x5a479c` (the label-string resolve block).

Applied 2026-07-18 (the waypoint/heat-bar session; renames at anchored
confidence — two kong-misnomer corrections announced inline; IDB saved):

- **Define** the hudpos parser blob `0x59f370..0x5a2480` as a function
  (previously the undefined `loc_59F370`) and **rename** →
  `HUD_ParseHudposToken` (anchored: the `_stricmp` token chain, registered
  against `"hudpos.def"` by `HUD_InitOverlaySystem @0x5a4927`).
- **Rename** `draw_minimap_overlay @0x599700` → `HUD_DrawWeaponHeatBar`
  (anchored kong-misnomer fix: reads hudInfo+60 heat, fills the HUDHEAT rect).
- **Rename** `HUD_DrawTargetNameAndDistance @0x5947a0` →
  `HUD_DrawWaypointNameAndDistance` (anchored: `get_waypoint_name` +
  `g_waypointList` + the HUDWPDINFO anchor globals).
- **Rename** `sub_58FB50/Game_GetShowWaypoints` → `Game_SetShowWaypoints` /
  `Game_GetShowWaypoints` (anchored: the BMS action-40 target + the
  label/cycle gates).
- **Rename** `sub_5A5F40` → `HUD_DrawMapOverlay` (anchored: grid/blip/WPNames
  string cluster + `HUD_RenderAllOverlays` caller).
- **Rename** `sub_5925C0` → `HUD_DrawParachuteAndArmorIcons` (anchored:
  entity+44 bit gates + the ParachuteIcon/ArmorIcon token textures).
- **Data renames**: `dword_27238BC` → `g_showWaypoints`, `dword_B76570` →
  `g_waypointList`, `dword_B76568` → `g_waypointCount`, `entityDef @0xB7656C`
  → `g_currentWaypoint` (kong-misnomer fix), `dword_2723694/98/9C/A0` →
  `g_hudWpdInfoX/Y/HideBox/Align`, `dword_27237DC/E0/E4/E8` →
  `g_hudHeatRectX1/Y1/X2/Y2`, `dword_27237D8` → `g_hudHeatBorderColor`,
  `flt_2723AE4` → `g_stanceColorBad`, `dword_2723AE0` → `g_stanceColorMiddle`.
- **Comments** at `0x599700` (heat-bar semantics), `0x5947a0` (label gates +
  draw shape), `0x594630` (name-resolve ladder), `0x4de5f7` (auto-advance),
  `0x502e41` (0x0F waypoint source = nav channel `flags&2`), `0x40f0aa` (BMS
  marker waypoint fields), `0x49b3de` (cycle key case 23), `0x593820` +
  `0x595470` (dead code), `0x5a4913` (`g_showWaypoints` init 1), `0x2723c8c`
  (the static -1 element-switch block), `0x42e4b8` (the 0x0F client apply).

Read-only re-witness 2026-07-19 (no IDB mutations applied):

- `0x4dcd30` is the settled **Sighted** standard-card predicate, additionally
  blocked during `MountSlot.currentAction == SWITCHFROM (7)`; the current IDB
  name `Player_IsVehicleGunnerScoped` is misleading.
- `0x4dcce0` recognizes **NoCardSwitch without ForceScoped**; its
  `Render_ProcessMainSceneFrame @0x5ca2f6` caller clears both the Scoped and
  Sighted standard-card bytes. The current IDB name
  `Player_IsVehicleHasAttackCapability` is misleading. No replacement names
  were applied in this read-only pass.
- `SIGHTS` rows are the selected card's content/fallback input, not the
  selector. The post-clear bytes gate the calls at
  `0x5caaf3..0x5cab15`.

## Ledger de-table transplants (2026-08-06)

Closed ledger rows whose full text previously lived only in the divergence
ledger, transplanted verbatim at the 2026-08-06 compaction (Standing rule 6).

- **D-HUD-10** [FIXED 2026-07-11 (weapon round: `aim_screen_point()` = INF in 1P -> the HUD pins the exact center; the 3P projection uses the witnessed 1000.0 far point)] Crosshair anchors at the fixed design center — the original anchors at the projected aim point (screen center only on-foot first-person `@ 0x5928a0`; spectate/`g_camera_mode` project `Entity_BuildCameraView` `@ 0x592910`)
