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
`HUD_DrawMapOverlay @0x5a5f40`. The 2026-08-12 follow-up then recovered and
ported the normal-gameplay call: `HUD_RenderAllOverlays @0x5a8070` supplies the
authored `HUDSPINMAP*` rectangle and flags `0x000D07FF`; this is distinct from
the still-deferred fullscreen/CMAP/DEATH map surfaces.
The 2026-07-31 recoil/spread grill then closed the write side behind the
crosshair's two dynamic terms: the round-spawn impulse, the infantry-body
decay/drift, the local movement/weapon-weight accumulator, and their distinct
projectile-versus-HUD shifts are now witnessed and ported (D-HUD-7).
The 2026-08-15 post-merge review witnessed and ported the **HUD declutter**
system (`huddetail`/`HUDDECLUT_*` — refuting the July "compiled-in `-1`
master switch" reading), corrected the map-pointer blink and 253/254
ring glosses, and applied the previously parked IDB renames (logged at the
end).

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
| Attach labels (seat/armory floats) | **ported** (`world::collect_attach_labels` + `Simulation::local_player_can_fire_weapon` + `HudFrameCompiler::element_attach_labels` + `game_hud_presenter.gd`, D-HUD-11/12/13 CLOSED) | `[orig: draw_vehicle_seat_and_armory_labels @0x5a3290]` full witness; nearest-entity branch consumes complete `Player_CanFireWeapon @0x5cf780`; label strings `[orig: HUD_InitOverlaySystem @0x5a479c..0x5a481e]`; `attachtextid` parse `[orig: @0x544d6c]`; the bold Arial label font + slot scale `[orig: @0x5a3680; HUD_InitAllFonts @0x51ee20]` ported 2026-08-11; ctest `vehicle_mount` + `def_parse_weapons`/`def_parse_items`; GUT `nova_simulation_test.gd`/`hud_helpers_test.gd` |
| Friendly tags (overhead name labels) | **ported** (`world::collect_friendly_tags` + `HudFrameCompiler::element_friendly_tags` + `game_hud_presenter.gd`, D-HUD-20) | `[orig: HUD_DrawFriendlyTagsPass @0x5a4480]` → `[orig: HUD_DrawEntityLabel @0x5a39b0]` full witness; names `[orig: Entity_SpawnFromBMSRecord @0x40ecbf]` + the 36-name fallback `[orig: g_fallbackPeopleNames @0x840a78]`; modes/toggle `[orig: @0x49b573]`; eye-offset anchor `[orig: @0x4bf078..0x4bf14c]` + Arial label font `[orig: HUD_InitAllFonts @0x51ee20]` witnessed + ported 2026-08-11; ctest `hud_math`/`hud_frame_compiler`/`infantry`/`promote` |
| Armory/vehicle-bay/FARP bottom prompts | witnessed — deferred with their systems (D-HUD-14) | `[orig: HUD_DrawGameplayOverlays @0x5bde60]` — preround/0x0A armory prompt, Flags 0x800 bay prompt, FARP wait/reload |
| Mission triggered text (WAC/BMS `text`) | **ported** (`HudFrameCompiler::element_messages` + `game_hud_presenter.gd`, D-HUD-6) | `[orig: HUD_DisplayTriggeredText @0x51f190]` → `[orig: Chat_AddDebugMessage @0x4987f0]`; `hud_helpers_test.gd` expiry |
| Message feed — the SYSTEM ring (kills / objectives / medic) | **ported** (`HudFrameCompiler::element_feed` + `hud::feed_format` + the `netsim` 0x1E fold, D-HUD-23) | `[orig: HUD_DrawMessageFeeds @ 0x59ad30]` fed by `[orig: NetPacket_HandleGameEvent @ 0x426270 -> HUD_FormatKillEventMessage @ 0x422DA0 -> Chat_FormatMessage @ 0x422C60]` |
| `hudpos.def` parser token map + 4-field positions | ported (`engine/formats/def`) | `[orig: loc_59F370; AMMOCOUNTPOS @0x59fc3d]`; ctest `def_parse_hudpos` |
| Parachute / armor status icons | witnessed — port pending (entity+44 flag writer unwalked) | `[orig: HUD_DrawParachuteAndArmorIcons @0x5925c0]` — entity+44 `&0x10` parachute / `&0x8` armor through the info struct's entity ptr; `ParachuteIcon`/`ArmorIcon` tokens |
| MP objective status text + team tile | confirm-only — MP HUD phase | `[orig: HUD_DrawTeamIdLine @0x59aa30]` (ex "draw_objective_status_text") client/strcli* strings witnessed |
| Weapon heat bar (HUDHEAT) | **ported** (`HudFrameCompiler::element_heat`, D-HUD-15) | `[orig: HUD_DrawWeaponHeatBar @0x599700]` (ex kong "draw_minimap_overlay" — a misnomer; there is no radar here) full witness: border + proportional fill in the HUDHEAT rect |
| Waypoint HUD label (HUDWPDINFO) | **ported** (`HudFrameCompiler::element_waypoint` + `game_hud_presenter.gd`, D-HUD-16/17) | `[orig: HUD_DrawWaypointNameAndDistance @0x5947a0]` + `[orig: get_waypoint_name @0x594630]` full witness; gates `[orig: @0x5a7daf]` |
| Waypoint track (list/current/advance/mission gate) | **ported** (`engine/runtime/world` waypoint track + `Simulation`, D-HUD-16/17) | list `[orig: NetPacket_WriteWorldStateLoad0x0F @0x502d10 @0x502e41]` (nav channel `flags&2`); BMS marker fields `[orig: Entity_SpawnFromBMSRecord @0x40f0aa]`; advance `[orig: Player_UpdatePerFrame @0x4de5f7]`; done-mark `[orig: EventTrigger_MarkLinkedSpawnPoints @0x452ce0]`; cycle `[orig: Spectator_CycleTarget @0x4dc1d0]` + input case 23 `[orig: @0x49b3de]`; `ShowWaypoints` `[orig: Game_SetShowWaypoints @0x58fb50]` |
| Gameplay spinmap (`HUDSPINMAP*`: heading-up terrain, blips, pulse markers, waypoint tether/distance, compass ring) | **ported** (`HudMinimapCompiler` → `HudFrameCompiler::element_spinmap` → `HudOverlay`; retained 0x40/0x6B state in `ClientReplicaPipeline`; in-map indicator/label legs = D-HUD-21) | `[orig: HUD_RenderAllOverlays @0x5a8070 (gate @0x5a86e8, mask 0xD07FF @0x5a86f0)]` → `[orig: HUD_DrawMapOverlay @0x5a5f40]`; transform `[orig: Terrain_FixedPointToWorldFloat @0x607060]`; terrain `[orig: render_terrain_decal @0x6071C0]`; blips `[orig: MapOverlay_RenderAllByLayer @0x5be590 → render_minimap_slot_blip @0x5be240]`; compass `[orig: draw_compass_indicator @0x59c900]`; ctests `hud_frame_compiler`/`client_minimap_overlay`/`minimap_overlay` + GUT `hud_overlay_test.gd` |
| Fullscreen / CMAP / DEATH map surfaces | witnessed — deferred (D-HUD-19) | shared fullscreen `HUD_DrawMapOverlay @0x5a5f40` legs plus windowed `MapOverlay_DrawView @0x5a58e0`: pan/zoom, grid coordinates, command/deploy labels and window hosting |
| Objectives panel + subgoal state (MISSION OBJECTIVES) | **ported** (`World::SubgoalState` + `HudFrameCompiler::element_objectives` + `game_hud_presenter.gd`, D-HUD-18) | `[orig: HUD_DrawWinConditions @0x5ba940]` full witness; actions 14/15/35/36 `[orig: EventAction_Dispatch @0x454500/@0x4545e0/@0x4546af/@0x454724]`; toggle `[orig: @0x49b68b]`; ctest `event_runtime_bms` subgoal block |
| HUD declutter (`hud_detail` + `HUDDECLUT_*` masks) | **ported** (`engine/runtime/hud/hud_declutter.*` + `HudFrameCompiler` per-slot gates + the shell's persisted `hud_detail`) | `[orig: HUD_ParseHudposToken @0x59F370 mask arms → CRenderState_SetLayerVisibility @0x59B0F0 → dword_2723C80]`; cycle `[orig: Input_HandleActionBinding_0 @0x4e060b..24]`; level-3 blackout `[orig: @0x5a80c4]`; death force-3 `[orig: @0x42e410]`; the full section below |

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
  per-element draws. Each element is gated by a per-slot visibility flag —
  the dword array `dword_2723C80[24]`, which is the **HUD declutter
  system's** output (the full witness is the "HUD declutter" section below):
  `CRenderState_SetLayerVisibility @0x59B0F0` rebuilds
  `visible[slot] = ((1 << hud_detail) & mask[slot]) != 0` over the 24 hudpos
  `HUDDECLUT_*` mask bytes `byte_2723CE0[24]`. The 2026-07-18 "statically
  initialized to `-1` in `.data`, no runtime writer, compiled-in always-true
  master switches" finding is REFUTED (2026-08-15): the `-1` was an IDA
  unloaded-BSS read — the same class of misread this record documents for the
  two label suppressors. The array is BSS, and JOX's authored masks make
  every consumed element visible at the default `hud_detail 0`. The flags the
  July sessions had named, with their declutter slots:

  | Flag (= `visible[slot]`) | Slot / token | Element |
  |---|---|---|
  | `dword_2723C8C` | 3 WAYPOINT | waypoint name/distance label (`[orig: HUD_DrawWaypointNameAndDistance @0x5947a0]`, gate `@0x5A7DB8`) |
  | `dword_2723C9C` | 7 DMGBAR | health bar (gate `@0x5A7C99`) |
  | `dword_2723CCC` | 19 TEAMID | team-id status line (`[orig: HUD_DrawTeamIdLine @0x59aa30]`, ex "draw_objective_status_text"; gate `@0x5A7DE0`) |
  | `dword_2723CD4` | 21 CLOCK | game timer + score (one gate `@0x5A7D6E`) |
  | `dword_2723CA0` | 8 WPNGRP | weapon/ammo + stance-indicator + ammo-indicator cluster (gates `@0x5A7CC8`/`@0x5A7D04`/`@0x5A7D42`; the seat-mode-2/5 leg falls through `@0x5A7D34`) |
  | `dword_2723CB4` | 13 XHAIRS | crosshair cluster (read in `HUD_DrawCrosshair @0x592757`, AND !binoculars) |
  | `dword_2723C90` | 4 ALTGRP | altitude / power bar (gate `@0x5A7D81`) |
  | `dword_2723CD0` | 20 PWRBAR | PowerThrow charge bar (`[orig: HUD_DrawPowerThrowChargeBar @0x599830]` — ex kong "HUD_DrawWeaponReloadBar" misnomer: it gates on the PowerThrow def bit + `g_fireChargeStartTick` and never draws reloads; witnessed + ported, world-wac-ai-re §27.3; gate `@0x5A7DD2`) |
  | `dword_2723CD8` | 22 HUDLS | weapon slot bar (`[orig: HUD_DrawWeaponSlotBar @0x599cd0]`, unwitnessed; gate `@0x5A7DEE`) |

  A spectator path (`g_death_screen_active`) rebuilds the info for the *spectated* entity
  and restores: `qmemcpy(tmp, &dword_2723388, 0x240)` → `HUD_BuildEntityInfo` →
  `qmemcpy(&dword_2723388, tmp, 0x240)` `[orig: @0x5a7bf1]` — pinning the 576-byte
  size.

## HUD declutter — `huddetail` / `HUDDECLUT_*` (witnessed + ported 2026-08-15)

The per-element visibility system behind the flag table above. State: the
persisted config int `hud_detail` (0..3, cfg cell `@0x24D20BC`, parse
`@0x550339`, default 0 `@0x54d3d8`, apply `@0x55154d`, saved `@0x54c80d`) and
the 24 hudpos mask bytes `byte_2723CE0[24]`. Rule:
`visible[slot] = ((1 << hud_detail) & mask[slot]) != 0`, rebuilt into
`dword_2723C80[24]` by `CRenderState_SetLayerVisibility @0x59B0F0` on every
mask or level change.

- **Parser arms** — the `HUD_ParseHudposToken` family: each
  `HUDDECLUT_<TOKEN> a b c d` row ORs bit `1/2/4/8` per nonzero field into
  the slot's mask byte (the MSNTITLE arm `@0x5A1614..83`; the SPINMAP arm
  `@0x5A2159..DE` writes `byte_2723CE0[17]`). The table is BSS-zero, so an
  UNAUTHORED slot is hidden at every level. JOX authors
  `HUDDECLUT_SPINMAP 1 1 0 0` (visible at levels 0/1 — the earlier
  `1 0 0 0` reading in this record was MSNTITLE's row); a 25th JOX row
  `HUDDECLUT_CTAPE` is a dead token — no parser arm consumes it.
- **The input** — `huddetail` (catalog row 50, dispatch code 19, default F6)
  cycles the level `(level + 1) % 4` `@0x4e060b..24`. The in-game dispatch is
  **per item class**: `Input_HandleActionBinding @0x49AD40` consults
  `itemDef+0x170` FIRST (`@0x49aea0..b6`) → `Input_HandleActionBinding_0
  @0x4e0420` (installed for inputFunctionClass troop/tank), whose live arms
  are 14 `showhud` `@0x4e0561`, 19 `huddetail` `@0x4e060b..24`, and 28
  `map_toggle` `@0x4e0662`; the menu-context default arm `@0x49c27d` is only
  the unconsumed fallback. The earlier "codes 14/19/28 resolve to the no-op
  arm" adjudication read that default switch and is corrected.
- **Level 3 blanks the ENTIRE gameplay overlay pass** `@0x5a80c4` — nothing
  draws. The "death exception" arm inside that gate is a structural no-op:
  its callee's own `level < 2` guard can never pass at level 3.
- **Forced levels** — death forces level 3 `@0x42e410..1c`; the HUD reset
  re-applies the persisted level `@0x59dd75`.
- **`showhud`** (code 14, unbound by default) cycles `g_FpWeaponViewFlags =
  (v + 1) & 3` `@0x4e0561`: bit 0 = the FP gun, bit 1 = ONLY the
  FP-weapon+spinmap sub-pass `@0x5a8635`. The whole-overlay master gate is
  the separate `/NOHUD` `dword_840B18 & 2`.
- **The WAC side apply** — mission event action 37 force-applies visibility
  through `RenderState_SetLayerVisibilityByIndex @0x5A3020` plus the 16-dword
  flash-timer array `dword_2723CF8`, WITHOUT touching the level global — a
  named tracked residual, unported.
- **F6 shadowing** — retail dispatches the FIRST catalog row matching a key,
  so F6 = `huddetail` (row 50 precedes `hudcolor` row 76). OpenNova's
  `hudcolor` row stays live and remappable but yields the shared default key
  the same way (D-CTRL-4, re-adjudicated below).

The 24 slots in parse-arm order, with every retail draw-site consumer
(slots 0/1/5/9/10/11/12/14/15/16 are parsed but have NO retail reader —
nothing may gate on them):

| Slot | Token | Retail consumer |
|---|---|---|
| 0 | MSNTITLE | none |
| 1 | FARPINFO | none |
| 2 | BREATHTIME | breath bar (`HUD_DrawBreathBar @0x59D6F0`, ex "CaptureProgressBar" — label Overlays/STROVER91) |
| 3 | WAYPOINT | waypoint name/distance label (gate `@0x5A7DB8`) |
| 4 | ALTGRP | altitude / power bar (gate `@0x5A7D81`) |
| 5 | SPEED | none |
| 6 | CKPTCOND | look-mode label (`HUD_DrawLookModeLabel @0x594103`) |
| 7 | DMGBAR | health bar (gate `@0x5A7C99`) |
| 8 | WPNGRP | weapon/ammo + stance + ammo-indicator cluster (gates `@0x5A7CC8`/`@0x5A7D04`/`@0x5A7D42`; seat-mode-2/5 fallthrough `@0x5A7D34`) |
| 9 | EXPPOINTS | none |
| 10 | NWSTAT | none |
| 11 | CFGDISP | none |
| 12 | LOLLIS | none |
| 13 | XHAIRS | crosshair cluster (`@0x592757`, AND !binoculars) |
| 14 | VELVECT | none |
| 15 | FARPIND | none |
| 16 | TARGDMGDISP | none |
| 17 | SPINMAP | gameplay spinmap (gate `@0x5A86E8`, nested in `showhud` bit 1) |
| 18 | TRGTCNT | the "In the Zone" indicator (`@0x59CCF1`) |
| 19 | TEAMID | team-id status line (`HUD_DrawTeamIdLine @0x5A7DE0`) |
| 20 | PWRBAR | PowerThrow charge bar (gate `@0x5A7DD2`) |
| 21 | CLOCK | game timer + score (one gate `@0x5A7D6E`) |
| 22 | HUDLS | weapon slot bar (gate `@0x5A7DEE`) |
| 23 | CHAT | chat feed (`@0x59AD66` — which ALSO carries a second hard `level >= 2` cull of its own) |

**Port** (lands with this record's 2026-08-15 revision): the engine declutter
module `engine/runtime/hud/hud_declutter.*` (`HudDeclutter` — the mask/level
model, the parse-arm token table, `cycle_level`, and the rebuild rule) feeds
`HudFrameCompiler`'s per-slot gates; the shell persists `hud_detail` like the
retail config token and cycles it on the polled `huddetail` binding
(`game_hud_presenter.gd`, default F6 — winning the shared key from `hudcolor`
by the same first-match rule).

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

### MP objective status — `HUD_DrawTeamIdLine @0x59aa30` (ex "draw_objective_status_text"; witnessed, MP HUD phase)

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
`g_friendlyTagsMode @0x24C18C4 != 0` and `g_rules_flags @0x24D1E34` bit
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
  hudpos `tagcolor_good/middle/bad`. A non-default `cfg_hud_color_index`
  replaces the good tier with `g_hudColorTable[index]` (`@0x5a3c9e`); the
  index's real producer is the `hudcolor` action row — code 10, default F6,
  retail-shadowed by `huddetail` (see "The hud_color_index scheme"; both
  earlier H mappings are refuted by the catalog walk). Enemy team = `0xFF00FF`
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

## The hud_color_index scheme (witnessed + ported; binding adjudicated 2026-08-13)

The binary has a two-global pair over one 16-dword table
(`g_hudColorTable @0x24C1838`, filled by `HUD_InitTeamColorTable @0x51f240`).

- **Entries 0..5**: 0 white `FFFFFFFF`, 1 green `FF00FF00`,
  2 `FF010101` (an init placeholder — refreshed per frame from the hudpos
  `hud_textcolor`), 3 light blue `FF80A0FF`, 4 yellow `FFF0F000`,
  5 salmon `FFFF5050`. (The tail entries 6..15 are the team/status colors —
  6 `FFFF40FF`, 7 `FFFF8020`, 8 gray `FFA0A0A0`, 9 `FF4040FF`, 10 `FF00EAE7`,
  13 `0000FF00`, 14 `41018101`, 15 `1E093309`; slot 12 IS `g_hudActiveColor`'s
  own cell.)
- **The snapshot global** `g_hudActiveColor @0x24C1868` =
  `table[cfg_hud_color_index] | 0xFF000000` at init `@0x51f2de` (and again by
  `HUD_InitOverlaySystem @0x5a4970`), restamped WITHOUT the OR at the cycle
  `@0x49afe0`. Read by the label/chat drawer family
  (`draw_vehicle_seat_and_armory_labels @0x5a362d/@0x5a3851`,
  `HUD_DrawEntityLabel @0x5a3cb1`, the chat text `@0x5930e0`, score/kill-list,
  the spinmap's current-waypoint tether `@0x5a77fe`).
- **The per-frame global** `g_hudFrameOverlayColor @0x840B1C` (ex `unused2`):
  `HUD_RenderAllOverlays @0x5a8100-0x5a8125` refreshes
  `table[2] = g_hudposTextColor` then stamps `= table[index]` every frame.
  Read by the 0x593xxx-0x596xxx drawer family (waypoint pair
  `HUD_DrawWaypointNameAndDistance @0x5949dd..`, reticles,
  `HUD_DrawTeamIdLine @0x59aa4c`, the `STROVER_DIST` readout, the
  spinmap grid label and at-marker distance).
  Every initialized entry 0..11 carries FF alpha, so the twins agree for those
  entries.
- **The cycle's real producer — adjudicated against the binary 2026-08-13.**
  The action catalog (108-byte records at `0x8159A8`; the byte-witnessed
  `aAbsoluteTurnLe @0x8159cb` block) ships row 76 `hudcolor` ("Hud Color"),
  whose record layout is `+0x00` dispatch code, `+0x04` flags, `+0x08`
  context mask, `+0x0c` class, `+0x10` sort key, `+0x14/+0x16` the keyboard
  VK pair. `hudcolor` = dispatch code **10**, flags `0x04000000`, default
  VK `0x75` (**F6**). Dispatch code 10 is the cycle case `@0x49afc7`
  (idx+1, >5 wraps 0, no toast) — the action IS real and reachable by
  dispatch. Two gates make it dormant on stock installs: the D-CTRL-2 flag
  test hides it from the rebind UI (no `0x800`), and the keyboard scan
  (`Input_ProcessKeyboardEvents @0x49d1f0`) dispatches only the FIRST
  matching row `@0x49d42f` — `huddetail` (row 50, also VK `0x75`, dispatch
  code 19 = the LIVE declutter-cycle arm of `Input_HandleActionBinding_0
  @0x4e0420`, `@0x4e060b..24` — the HUD declutter section; the old "default
  no-op arm `@0x49c27d`" premise read the menu-context switch) precedes it,
  so stock F6 cycles `hud_detail` and the color cycle never fires without a
  keyfile rebind.
- **H does not touch this system — or any HUD toggle.** VK `0x48` (H)
  appears in the catalog only as `pause`'s secondary (row 70, code 25:
  SP-only pause + audio mute `@0x49b520`; nothing in-session).
  `showhud`/"Hide Gun" (row 27, code 14) is unbound but LIVE — its arm
  cycles `g_FpWeaponViewFlags = (v + 1) & 3` `@0x4e0561` (bit 0 = the FP
  gun, bit 1 = the FP-weapon+spinmap sub-pass). The whole-overlay master
  gate is the boot `/NOHUD` switch (`@0x4a7a09` → mask `@0x840B18`); the
  per-element visibility control is the declutter system (`huddetail`).
  Both prior key mappings — #491's KEY_H cycle stand-in and #492's H
  visibility toggle — are refuted by this catalog walk.
- **Config**: token `hud_color_index` `@0x5502eb` (cfg cell `@0x2550BCC`),
  default 2 `Config_SetDefaults @0x54d2a6`, applied to the live index
  `dword_24D20B8` by `apply_session_settings_to_globals @0x55152f`.
- **The friendly-tag good tier** (`HUD_DrawEntityLabel @0x5a3c9e..0x5a3cf5`):
  `tagcolor_good` only when the index == 2, else `table[index]`; the
  middle/bad tiers never swap.

Ported: `HudFrameCompiler::active_color` derives the one per-frame color
(entry 2 sourced live from `hud_textcolor`); `HudOverlay.set_hud_color_index`;
the presenter persists the index like the retail config token and cycles it on
the polled `hudcolor` binding row — which, like retail, YIELDS the shared
default F6 to `huddetail` by the first-match rule (D-CTRL-4, re-adjudicated
2026-08-15: code 19 is a live arm, not a dispatcher no-op — F6 drives
`huddetail` in OpenNova too, and `hudcolor` stays a live remappable row);
ctest `test_compiler_hud_color_schemes` + GUT `hud_overlay_test` pin the
schemes.


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
  `@0x58fb60`) `&& dword_2723C8C` (the declutter WAYPOINT `visible[3]` —
  authored visible at default `hud_detail`) `&& g_GameType != 0x10010`,
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

## Gameplay spinmap — `HUD_RenderAllOverlays @0x5a8070` → `HUD_DrawMapOverlay @0x5a5f40` (grilled + ported through 2026-08-15)

The normal in-world map was previously conflated with both the dead compass
strip and the command/deploy map. Retail does have a gameplay spinmap: the
overlay pass builds a parameter struct {mask `0xD07FF` @`0x5a86f0`, player pos,
focus pos, the raw authored `HUDSPINMAPX1/X2/Y1/Y2` rect
(`@0x27235C8/CC/D0/D4`, parsed `@0x59f7f1..0x59f87b`; JO:CA authors
`810/1020/552/762`), yaw, RGBA `255×4`, the spinmap zoom float} and calls the
shared map renderer. The pass is gated by `g_FpWeaponViewFlags & 2` (the
`showhud` bit-1 FP-weapon+spinmap sub-pass `@0x5a8635`, cycled by the live
code-14 arm `@0x4e0561`), the `/NOHUD` mask (`@0x840B18`, boot init 3, bit1
gates all overlays `@0x5a81ce`; set only by the command-line switch
`@0x4a7a09`), and the declutter SPINMAP visibility slot (`dword_2723CC4` =
`visible[17]`, stamped by `CRenderState_SetLayerVisibility @0x59B0F0`; JOX
authors `HUDDECLUT_SPINMAP 1 1 0 0`, so the spinmap hides at
`hud_detail >= 2` — the HUD declutter section carries the full system;
the earlier "compiled-in `-1` master switch / codes 14/19/28 are no-op"
story is refuted there). The committed comparison evidence for this section
is `screenshots/pr-492/` (the synchronized 00TRa spinmap pair).

- **The mask is a content selector, not a gate.** Witnessed bits: 0 backing
  disc, 1 marker banks, 2 objective tether lines (`source & 0xC0` markers,
  team-colored, suppressing the bit-8 line), 5 entity labels
  (`draw_entity_labels_and_markers @0x5a49e0`), 6 compass ring (paired —
  the gate is bit9 && bit6, with the bit10 legs nested inside), 7
  tracked-target pointer in `g_hudActiveColor` (`@0x5a77fe`, ctx
  `g_trackedTargetPos @0x272350C`, drawer args no-line/tip-when-ahead), 8
  the waypoint state line in `g_waypointAltitudeColor @0x2723D7C` (ctx
  `g_waypointPosXY @0x2723518`; drawer args line+tip), 9 pairs into the
  compass gate, selects `render_terrain_decal`'s enable_fog_pass
  water-overlay variant (the TILES themselves draw UNMASKED — the decal
  call is unconditional, bit9 only picks its argument form), and gates
  the player grid-coordinate label with !bit12
  (`HUD_DrawPlayerGridLabel @0x59cb40`:
  `"(%s,%d)"` right-aligned half-bright at the authored `mapcoords` position
  `g_mapCoordsLabelX/Y @0x27236F4/F8`, gated `g_mapCoordsLabelOff
  @0x27236FC == 0` — the token's 3rd value; the global is BSS (no file
  bytes → ZERO = label LIVE; the earlier "initializer −1 = suppressed"
  gloss read undefined bytes, the same misread the bit-18 suppressor
  got), so only an authored nonzero value suppresses;
  300-unit cells = `19660800` Q16, column letters from
  `x − 0x960000 − origin_x` via `HUD_FormatGridCoordinate @0x598600`
  (base-26 A..Z/AA.., negatives folding back from ZZ), row =
  `y/300 − origin_y/300 − 1.0` (+1 above zero); origin = the cached
  item-2043 (`0x7FB`) pool-3 entity `@0x5a4980..0x5a4999`, position snapped
  to whole cells), 10 radar-contact tick + weapon-direction indicators +
  round-timer box (inside the compass pair gate), 12 the 300-wu GRID leg
  (rules + letters/numbers + the on-map large-font player readout run
  before the marker walk when set and ctx+48 ≠ 1 — NOT marker
  suppression; the grid branch falls through into the bank walk; the
  2026-08-13 "marker-bank suppress" gloss is corrected), 15 KOTH route
  lines (GameType `0x10010`), 16 fullscreen centering, 17 pool-4 location
  labels (`g_location_names`, `0x9F9F9F`), 18 ring-edge waypoint distance
  (`"%03dm"`/`"%01.2fk"`, `flt_7C69E8 = 0.001`, position/distance from the
  slot fields the pointer drawer stores — see the pointer bullet; drawn only
  when `g_spinmapWpDistLabelOff @0x27237C0 == 0`: the global is BSS
  (uninitialized .data, no file bytes -> ZERO = label LIVE; the earlier
  "static 0xFFFFFFFF" gloss read undefined bytes — re-adjudicated
  2026-08-14 against the segment map) and only an authored NONZERO
  `SPINMAPWPDISTOFF` value
  replaces it `@0x59fc1f` — neither JO:CA nor JOTAC authors the token, so
  retail never suppresses this label), 19 tracked-target distance at the
  `0x2721ED0/ED8/EDC/EE4` slot (written by
  `render_laser_sight_effect`/`HUD_SetTrackedEntityTarget`; the sibling
  `HUD_DrawTrackedTargetDistance @0x594b60` element draws the gametext
  `hud_farp` label + `"%d"` distance to `g_trackedTargetPos` in a wireframe
  box at `0x2723628/262C`), 20 the waypoint altitude nub above the rect (see
  the pointer bullet; NO witnessed caller mask carries bit20 — `0xD07FF`
  and `0xAF937` both lack it, so the leg ships dormant in retail JO; the
  earlier "big-map mask" attribution is corrected). The spinmap's
  `0xD07FF` sets bits 0-10 and 16/18/19.
- **Waypoint/tracked pointer** [orig: `HUD_DrawMapTargetPointer @0x599220`
  (ex "CTerrainTile_UpdateShadowState"), call sites `@0x5a7835` (bit 7,
  args `0,0,1`, color `g_hudActiveColor`) and `@0x5a7894` (bit 8, args
  `1,1,1`, color `g_waypointAltitudeColor`)]: computes the bearing from the
  slot entity to the ctx position through the Q22 BAM tables, draws a
  2-vertex line from the entity's map position along the bearing (bit 8
  only — the first bool arg), and a `TSDicon` strip cell via
  `Render_DrawIconStripCell_Debug @0x67bae0 → render_tiled_image_strip
  @0x67b540`: ONE strip cell per call, index = the `lodLevel` local —
  default **7 (chevron)** at the clamped tip while the target projects
  OUTSIDE the clip, switched to **1 (dot)** drawn AT the target once the
  inside branch takes (`@0x59935e` replaces the clamped endpoint with
  the raw target position). There is NO second cell at the player — the
  2026-08-13 "anchor dot" gloss over-read the two cell constants; the
  32-m 00TRa capture (dot at the waypoint, nothing at center) plus the
  single `Render_DrawIconStripCell_Debug` submit `@0x59953d` settle it.
  **Color path**: the INSIDE-dot branch BLINKS on `g_hudFrameCounter &
  0x20` (ex `dword_A87064` — the per-main-frame counter, `++`ed from
  `Game_ProcessMainFrame @0x5265d5` via `Game_TickHudFrameCounters
  @0x434c23`; NOT device caps — the earlier "2X-modulate caps flag" gloss
  is refuted): one 32-frame phase submits the halved color
  (`(c >> 1) & 0x7F7F7F` `@0x599397`), the other the per-channel
  2c-saturated one (`c >= 0x80 ? 0xFF : 2c` `@0x5993c6..0x5993f8`); the
  outside chevron always submits the 2c-saturated form. Through the map
  stage's MODULATE2X that nets raw-vs-saturated — the banked capture (the
  tether measuring raw `0x007000` over grass) caught the halved phase.
  The strip cell takes the phase color `@0x59953d`. The drawer stores a
  FIXED-LENGTH radial anchor along the bearing. Its exact radius is
  `baseRadius - scaleX((flags >> 8) & 2) + scaleY(10) + floor(scaleY(10)/2)`:
  the map setup subtracts the bit-9 compass inset at `@0x5a64c0..0x5a650d`,
  then the pointer adds the scaled span and its integer half at
  `@0x5995c7..0x5995d8`. At 1920x1080 this is
  `baseRadius - 4 + 14 + 7 = baseRadius + 17`, matching the synchronized
  00TRa label's 11-pixel outward delta from the former `baseRadius + 6`
  approximation. The result is stored to `slot[17]/[18]`
  (`@0x5995c7..0x599616`, gated on
  the min-relative-bearing race `slot[21]`) and the clamped distance to
  `slot[20]` — the bit-18 label's inputs (so the "032m" label rides the
  ring edge along the bearing, NOT the tether tip; the 32-m capture pins
  it there while the tip dot sits mid-disc). The bit-18 wrapper halves the
  overlay color in `HUD_DrawTextCentered_HalfBright @0x580680`; the active
  fixed-function map/font stage then doubles RGB, making the final diffuse
  `2 * half(color)` (for example `0xFFFFD000 -> 0xFFFED000`). OpenNova folds
  both stages into its Canvas glyph color.
  **Altitude tricolor** [orig: `HUD_UpdateWaypointAltitudeColor @0x590970`]:
  `wp_z − player_z` vs ±`0x20000` (2.0 wu) → level `0xFF007000`
  (`extra = 2`), above `0xFF7F5000` (`extra = 0`), below `0xFF20407F`
  (`extra = 1`). **Altitude nub** (bit 20): `WPIndctr.tga`
  (`g_texWpIndicator`, loaded `HUD_LoadAllTextures @0x59e079`; a 4-frame
  vertical strip — up-triangle/down-triangle/circle/blank) drawn as a
  20×20-design quad ending at `y1 − rect_h/32`, x = center ∓10 shifted
  ±8 by `extra`, frame = `extra`, in the tricolor.
- **Ring/disc radius (capture-closed 2026-08-14)**: the `@0x5a5f40`
  rect block loads the four scaled rect corners, forms the truncated
  centers with `flt_7C3B94 = 0.5`, and uses the scaled rect
  **half-height** as the base radius. The backing and terrain/marker
  stencil share the observed four-physical-pixel inset
  (`disc = half-height − 4`), while `draw_compass_indicator @0x59c900`
  uses the uninset half-height times the witnessed `1.25`. The rect
  scales per axis through `Viewport_ScaleToVirtualCoords @0x5d2b20`
  (x·w/1024, y·h/768, rounded), but all circular radii come from its
  height. The completed-pass 1920×1080 probe pins the authored rect at
  `(1575,28)..(1950,309)`, center `(1762,168)`, stencil radius `136.5`
  before rasterization, and compass half-extent `175.625`. This closes
  the earlier `0.9275 × base`, mean-half-extent, and “10% smaller”
  hypotheses.
- **View transform** (`render_terrain_decal`'s tail `@0x607ac1..0x607b13`
  writes the shared globals `0x319A278..294`; the unreferenced twin is
  `MapView_SetTransform @0x607130`, defined + named this session): screen
  centers = truncated midpoints of the `Viewport_ScaleToVirtualCoords`-scaled
  rect; world-per-pixel scale = `zoom / (rect_height_px × 200.0)`
  (`flt_7D2290`). The completed-pass probe pins 00TRa to
  `25559 / (281 × 200) = 0.45478648` wu/px;
  rotation angle = `yaw + g_mapYaw180 − 0x40000000` folded to BAM16 × 2π/65536
  (`flt_7C7988`), where `g_mapYaw180 @0x2723EB0` = `0x80000000` iff
  `Bms_AttribFlags & 0x20` (the mission's RotateMap180 attribute,
  `@0x5a49bc`). Projection [orig: `Terrain_FixedPointToWorldFloat @0x607060`]:
  `lx = (x−px)·inv/65536`, `ly = (y−py)·inv·(−1/65536)` — mission +Y is
  NEGATED into screen space — then `sx = ctr_x + lx·cos − ly·sin`,
  `sy = ctr_y + lx·sin + ly·cos`. Zoom steps are dispatcher cases:
  `radarout` (row 48 → case 361 `@0x49beaf`) multiplies by `dbl_7C7AA8 = 1.15`
  clamped ≤ `0x100000`; `radarin` (row 49 → case 360 `@0x49bcb0`) by
  `dbl_7C7AB0 = 0.85` clamped ≥ `4096`; the value resets to
  `65536 × clamp(1 − Bms_MapZoom, 0.0625, 1.0)` at
  `Player_InitPlayer @0x4e1741..0x4e1763` — `flt_A7640C` is NOT a
  static zero: it is the BMS HEADER's `map_zoom` float
  (`g_BmsHeaderBlock @0xA761D0` + 0x23C, bulk fread, no per-field
  xref — the field our `bms.h` already parses), so the spawn zoom is
  MISSION-SCALED: 00TRa authors 0.61 → `X = 0.39`, spawn spin zoom
  `25559`, and big zoom `524288 × X` the same way. The live completed
  pass closes the ambiguous FPU ordering in favor of `1 − f`; the
  pose-matched atlas registration then finds global terrain scale
  `1.00`, independently rejecting the former width-denominator/
  direct-`f` pairing. A zero/unauthored value retains the retail
  default `X = 1`. Ported as
  `HudMapControl::set_mission_map_zoom`; a wire-only joiner has no
  header and keeps X = 1, a D-NET-194-shaped residual (the
  deploy/cine mode flags `0x24C18B8/BC` swap the pair onto the
  big-map zoom `@0xB76490`).
- **Backing disc**: a 33-vertex/96-index fan (indices built inline), ring
  vertices from the Q22 BAM sin/cos tables starting at BAM `0x200000` in
  `0x8000000` steps — exactly 32 uniform segments (the offset vanishes below
  the table resolution). The disc pass lays the stencil that crops every later
  leg; the triangles themselves clip only against the rect
  (`clip_triangle_and_emit_vertices @0x688e30` is a min/max box clip). The
  OpenNova compiler clips the terrain quads against the same 32-gon instead —
  the software equivalent of that stencil.
- **Terrain** [orig: `render_terrain_decal @0x6071C0`]: cover bound =
  `diag(rect_px) × 0.8 (flt_7C6F9C) × scale` world units; 512-unit tiles
  (`0x2000000` Q16 snap); the row index derives from **negated mission Y**
  (`(−0x1000000 − y) >> 25`) and the column from `x >> 25`, offset by
  `Terrain_SectorOrigin*` with OOB clamp masks, then
  `Terrain_SectorGrid[16×(row & 0xF) + (col & 0xF)] − 1` — the shared TRN
  routing table. The base pass selects the corresponding one of the four
  original 512×512 `Colormap0..3` textures — it does **not** sample the
  PolyTrn per-cell render-target cache. OpenNova packs those four independent
  clamp textures into one 1024×1024 atlas and applies a half-texel quadrant
  inset so filtering cannot cross their seams. The tile color: the
  map pass hands `0xD0606060`, the decal renderer FORCES the alpha opaque
  (`color | 0xFF000000` `@0x6071C4` head), the fixed-function output stage
  QUADRUPLES texture × diffuse for this pipeline — the reference captures
  measure the map interior at exactly 2.00×/2.02×/2.04× (R/G/B) a single
  MODULATE2X 0.7529-pass, i.e. `tex × 0x60×4 = tex × 1.5058`, saturating.
  OpenNova emits the doubled vertex color (`0xFFC0C0C0`, canvas modulates
  1×) and draws the terrain a second time on an additive child item —
  per-pixel identical to the ×4 stage under saturation. The
  `enable_fog_pass = 1` both call sites pass (`@0x5a59c8`/`@0x5a6677`) is
  the separate WATER pass, not the brightness. `PolyTrn_InitTextures
  @0x60BA20` builds the 256×256 `depthspin` texture directly from the raw
  1024×1024 CPT height words: output `(x,z)` averages the four taps
  `(4x,4z)`, `(+2,0)`, `(0,+2)`, and `(+2,+2)`, then uses `sum >> 10` as
  the integer terrain height. The redraw selects one of its four 128px
  quadrants with literal UV scale `127/256` and second-half offset `130/256`
  (`@0x6077A2..0x6077E6`). Bilinear filtering is followed by the fixed-function
  stage's UNORM8 writeback; its ADDSIGNED/ADD alpha chain plus alpha-ref 192
  therefore reduces to `round(sampled_terrain_height) <= water_height_int`.
  Comparing the unquantized sample instead incorrectly makes the fractional
  band from `water_height` through `water_height + 0.5` dry. Zero water
  suppresses the pass and the plane clamps at 254. The passing texels replace
  the completed terrain pixel with the synchronized-capture water tone
  `0xFF16476B`. OpenNova implements the same four-tap reduction as
  `TerrainData::build_minimap_water_mask`, retaining reduced terrain height
  and integer water height in an RG8 data texture. A dedicated linearly
  filtered Canvas shader performs the height comparison after sampling, at
  the same raster stage as retail; a pre-thresholded binary mask is not
  equivalent at shoreline texels and produced the observed broad halo. The
  water pass redraws the same clipped sector fans after both Canvas brightness
  legs. `GameWorld` emits
  `minimap_water_changed` when that mask is rebuilt. Streamed/authored `.til`
  art and the PolyTrn per-cell cache do not participate in the gameplay
  spinmap. The 2026-08-13 per-cell/ortho atlas surrogate and its registration
  offsets are therefore deleted. The synchronized 00TRa comparison now has
  the same shoreline footprint while retaining the source colormap detail.
- **Marker banks** (net-re §5.19/§5.35 carries the wire/retention story):
  draw order per layer is persistent (buildings with interior models first,
  via `render_collision_wireframe @0x596800` footprints — PORTED
  (`engine/runtime/world/minimap_footprint.*`): the model's 60-byte
  **OOBJ occlusion records** at `model+0xDC/+0xE0`, not COBJ collision
  sections. Records with type byte `<= 1` pass `@0x596803`; faces whose
  OPLN Y-up normal exceeds `0.5` emit OVRT X/Z triangles. The record
  position is portal metadata and is not added to the already-model-local
  vertices. Fills use opaque team colors (t1 `0x4050A0`, t2 `0xA05040`,
  attrib-bit17 ChangeTeam `0x609F60`, else `0xA0A0A0`): the map caller's
  zero alpha override is promoted to `0xFF` at `@0x596884..0x596891`,
  matching the retail capture's exact `0xA0A0A0` runs. OFAC edge words
  toggle by their low 15 bits per OOBJ record (low byte / high 7 bits =
  vertex pair, bit15 winding), and fewer than four survivors suppress the
  outline. Retail builds `0x80000000` boundary vertices, but completed-pass
  captures show no observable stroke; OpenNova therefore retains the
  parity result in the feed but does not submit black lines. Retail hands the
  unfurled `map_heading - entity_heading` directly to
  `render_collision_wireframe` (`@0x5a636e`, `@0x5be55b..0x5be57b`), whose
  local screen formula is reflected (`@0x596844..0x596bbb`). OpenNova places
  the outline in mission space first, so it subtracts 90 degrees from the
  entity heading to compensate for the map projection's separate -90-degree
  fold; that projection then supplies retail's one screen-Y reflection. Then
  come persistent rest,
  transient, special [orig: `MapOverlay_RenderAllByLayer @0x5be590`]. The
  icon→layer table `{10,11,15,18,25}→1, {2,4,12,13,16,17,29}→2,
  {3,8,14,23,24}→3, else 0` is byte-witnessed (`@0x5be681`), and the special
  bank routes by flags bit7 instead. The special-bank walk SKIPS slots
  whose lifetime floored to zero — expired-but-claimed slots stay in the
  bank but never draw [orig: `@0x5be794` — `!slot[+24] → skip`]; ported as
  the compiler's expired-special gate. The 0x6B link walk keeps that slot
  ALIVE rather than re-allocating it: it rewrites the slot's HANDLE each
  tick (`@0x5bfd95..c8`), and a link lapse ASSIGNS `flags = 0x20`
  (`@0x5bfdf3`) — so a lapsed-then-refreshed link RESURRECTS through the
  lifetime-only draw gate. (Two witnessed quirks stay
  unported under D-HUD-21: the persistent bank walks twice with a
  building/non-building split across two layer functions, and layers 1/2
  re-draw every live special slot `@0x5be7ad`.) `render_minimap_slot_blip @0x5be240`:
  special icons 253/254 draw FIXED CENTER rings over the slot's ZEROED z —
  `minimap_draw_ring_blip @0x597320` takes `max(min_radius, height_px)`
  with `height_px = 0`, so the radii are the fixed minima: the 2/1/0-px
  yellow ring trio for 254, plus a second 1-px pulse ring for 253. The
  pulse color is the 64-frame triangle wave `phase = (frame − 8) & 0x3F`
  folded at `0x20`, each channel `c += phase·(255−c) >> 5` (toward white).
  The former "shrinking `×0.75` ring stack" gloss was a dead-code misread:
  the `49152`/`65536` Q16 folds multiply the already-zeroed z. Ring colors
  submit OPAQUE (`| 0xFF000000` `@0x597392`). Other special icons draw
  **unrotated 6-px half-extent** billboards (`draw_billboard_decal
  @0x5975f0`, `size_override = 6.0`, BAM16 rotation × 2π/65536 when
  nonzero) and take the caller's drawMode `0xFF` alpha (`@0x597775`; call
  sites `@0x5a6d20`/`@0x5a5a02`) — special sprites submit opaque. Regular
  (non-special) markers draw from
  the LIVE pool entity gated on `entity[538]`
  (`draw_minimap_blip @0x597890`): team colors from the HUD globals
  (`g_hudColorLightBlue @0x24C1844` / `@0x24C184C` / `@0x24C183C`), sizes
  from the model footprint with class fallbacks (generic 10.0 wu = `655360`,
  person 2.0 wu, def-flag overrides 4/8 wu) and pixel floors (6.0 default,
  4/8/12/16 by class), icon 9 × 1.2, spectate icons 26/27 at ×2. The ordinary
  icon-strip call submits those raw colors into TSDicon's `MODULATE2X`
  texture stage; OpenNova bakes that saturating RGB doubling into the sprite
  diffuse while leaving alpha unchanged. The waypoint-pointer path is
  distinct: its pre-half and the same stage cancel, so its altitude color
  remains net-raw. A live call-site capture at the map center identifies the
  disputed blue glyph as the local deployed player — regular-bank source,
  live Person reclassification to cell 3, raw team-1 `0xFF304080`, and
  6×6px half extent — not an armory, objective, waypoint, or tracked target.
  `Simulation.get_hud_minimap_snapshot()` restores that client-local retained
  row only when the loopback feed does not already contain the same handle.
  The synchronized green-marker peak is retail `(88,255,65)` versus OpenNova
  `(86,255,64)`; apparent shade changes between captures come from filtering,
  blending, and live state rather than a different configured base color. The
  `TSDicon.tga` sheet is a **30-cell vertical strip of square cells at its
  authored physical resolution** — stock JO ships 16×480, JOTAC's RevX02
  authors 64×1920 — indexed by `render_tiled_image_strip @0x67b540`, whose
  half-texel cell insets are derived from the loaded tile's stored physical
  dimensions (`0.5 / tile_dim`; the right/bottom bounds intentionally reach
  half a texel past the cell under the clamp sampler). `Texture_LoadFromFile_0
  @0x58fe00` (the TSDicon call `@0x59e065` in `HUD_LoadAllTextures`) leaves
  that physical resolution intact;
  `GTexture_CreateFromPixelData_0 @0x6877ba..0x6878be` allocates the full mip
  chain and fills it with `D3DXFilterTexture` filter 5 (box), while
  `CGfxDevice_ApplyRenderStates @0x67e3e1..0x67e421` selects linear mip
  filtering. OpenNova generates the same mip chain, stamps the loaded strip's
  measured physical dimensions into the compiler for those endpoints
  (`HudMinimapInput::icon_strip_*`), and linearly mip-filters with clamp on
  both map canvas items. Treating the logical ~16px badge size as the source
  dimensions and uploading the strip without mips caused the visibly aliased
  armory glyphs.
  The per-class hide/rotate gates
  `0x2723D0C..D34` are unwritten statics (always pass). Capture-zone def ids
  6027/6028 add team rings (`0xFF2020`/`0x4060FF`, alpha pair `0x50/0x40`,
  min radius 64 wu `@0x5a7027..0x5a7061`).
- **Compass ring** [orig: `draw_compass_indicator @0x59c900`]: the
  `compring.tga` quad at ×1.25 the map radius (`flt_7C6F18`),
  counter-rotated `(0x3FFFFFC0 − yaw) >> 16` × 2π/65536 so its north marker
  points at world north. OpenNova's draw-list angle negates that numeric delta
  at the renderer boundary because the Godot canvas is +Y-down; the registered
  yaw −90° 00TRa frame (retail image SHA-256 `6211fe22a5ebb51178ff8253941cfd98795afce370351d407a354015679c6306`)
  pins W at twelve o'clock. Its UVs are centered at 0.5 with ±0.45 extent
  (`flt_7D93A8`), so retail samples only `0.05..0.95` of the texture. Cropping
  that authored transparent padding makes the visible ring `1/0.9` larger on
  the unchanged quad; this is why the same eastern and northwest landmarks
  touch the ring in the retail capture.
- **Pixel-circle geometry** (retail captures, JOTAC 00TRa, both headings):
  the map is a true circle in screen pixels on a widescreen surface because
  radius and world-per-pixel both use the scaled rect **height**, never its
  wider X extent. The backing/terrain/marker disc is
  `half-height − 4 px`; the compass quad is `half-height × 1.25`, with the
  centered-90% UV crop above. This exact
  rule replaces the former mean-radius/`0.9275 × base` approximation and
  closes the apparent ~10% widget-size and zoom deltas. The compass ring
  bakes the FF MODULATE2X into its texture (white-modulated static sprite;
  the band/letters read ~2× ours before, luma 27 vs 29 after).
- **Presentation ABI.** `Simulation.get_hud_minimap_snapshot()` returns a
  versioned packed array `{version=3, stride=16, count}` with rows
  `{bank, handle, x, y, z, heading, icon, argb, flags, source,
  remaining_ticks, entity_known, policy_flags (bit0 rotate / bit1
  footprint), half_x_q16, half_y_q16, floor_px}` — the policy tail is the
  `draw_minimap_blip @0x597890` per-class table resolved against the local
  entity at snapshot build, retail's own client-side resolve site;
  `HudOverlay` rejects unknown versions/short strides atomically. Static
  footprint polygons ride the separate once-per-mission
  `get_hud_minimap_footprints()` feed (version 1: per-entity OOBJ fill fans +
  retained boundary edges in mission Q16, opaque team-colored fills), and
  the depthspin-equivalent water mask rides `set_minimap_terrain`'s second
  argument; `minimap_water_changed` refreshes that binding. The engine
  compiler emits the disc,
  terrain, ordered marker sprites/rings, the waypoint state line + anchor
  dot/tip chevron cells + the altitude nub, the labels (per-pass glyph
  lists — bold slot for the corner map's distance/MAPCOORDS labels, the
  `Impac22b` LARGE slot for the grid letters/numbers and the big-map
  player readout, all CPU-half-bright then restored by the map/font
  MODULATE2X stage), and the compass sprite. The grid
  origin rides `Simulation.get_hud_map_grid_origin()` — the promotion
  stash on a host, the replicated pool-3 type-2043 entity on a joiner
  (D-NET-194 wire-header missions carry no markers) — the waypoint
  altitude rides `set_waypoint`, and the player altitude rides
  `set_minimap_state`. The device leg draws the corner map's base under
  the flat HUD with its additive/top children just above it, and the
  whole big-map trio ABOVE the flat pass (the witnessed frame order).

Unported in-map legs, each witnessed above and tracked under D-HUD-21:
the in-map weapon-direction indicators (`draw_weapon_direction_indicators
@0x59c350`) + round-timer box (`draw_timer_overlay_box @0x59c7b0`) +
radar-contact state (producers `Radar_AddBlip @0x59b280` ← damage
`@0x4dd8ee` / tracer `@0x4e5cb1`; consumer `update_radar_contacts @0x59a7e0`
building the 12/24-sector rings at `0x2721EF4..0x2721F3B` and the 4-quadrant
damage flashes `@0x2721EEC`), the objective tether lines (bit 2), entity
labels (bit 5), location labels (bit 17), the tracked-target legs (bits
7/19 — no tracked-target source exists in this runtime yet), the remaining
model-extent size feed and pointer line length, and the sibling out-of-map
consumers (`draw_radar_blips @0x5a2c00`
in-world markers, `draw_damage_direction_indicators @0x59a300`,
`draw_directional_indicator_ring @0x598180`). The HUDDECLUT_* consumer is
no longer an open witness: the declutter system is witnessed and ported
(the HUD declutter section above) — JOX authors `HUDDECLUT_SPINMAP 1 1 0 0`,
hiding the spinmap at `hud_detail >= 2` (the earlier `1 0 0 0` reading here
was MSNTITLE's row).

### The M-cycle map modes (witnessed + ported 2026-08-13)

The `map_toggle` action (catalog row 98, code 28, default `M`) cycles the
map overlay mode **0 → 2 → 3 → 0** [orig: `HUD_CycleMapMode @0x520bc0`
(ex "UI_CycleAmmoDisplayMode" misname), dispatched from the IN-GAME binding
arm `@0x4e0662` — the earlier "code 28 is a dispatcher no-op" adjudication
read the MENU-context switch and is corrected]. `g_mapOverlayMode
@0x24C18BC` clears on round init [orig: `Game_InitNewRound @0x42275a`],
respawn-state init [orig: `Game_InitRespawnState @0x499395`], and every
render frame while the local player is dead [orig: `@0x5cac67..0x5cac6d —
Flags & 2 → g_mapOverlayMode = 0`]. Frame order [orig:
`Render_ProcessMainSceneFrame @0x5ca0f0`]: `HUD_RenderAllOverlays
@0x5cad04` (bars, crosshair, messages, tags, the corner spinmap) → the
mode-gated `HUD_BuildMapOverlayView @0x5cad15` (the big map draws ABOVE
the whole overlay set) → `HUD_DrawGameplayOverlays @0x5cae0b`
(objectives/timer/ping draw above the big map).

`HUD_BuildMapOverlayView @0x5a7e10` (ex "render_glow_effect" misname)
builds the mode's ctx and calls the same `HUD_DrawMapOverlay`:

- masks [orig: `@0x5a7e8a..0x5a7ed0`]: the default (glow_params = 0)
  M-cycle call takes `719159 = 0xAF937` for BOTH modes 2 and 3;
  `715063 = 0xAE937` belongs to the PARAMETERIZED variant
  (`BYTE1(params+4)` clear — its callers are unwalked), NOT to a mode.
  `|0x10000` (bit16) applies then modes 3/4 clear it — windowed-only.
  mode 2: the 400×400 design window at (20,20)..(419,419); mode 3: the
  fullscreen 0,0..1023,767 map. The mask has NO bit9 and NO bit6 (no
  compass — the compass gate is bit9 && bit6 `@0x5cab..512-region`), and
  NO bit18 label. mode 1 exists for a caller-supplied entity (rotating,
  entity_ref = entity heading; ctx+48 carries the mode and exempts the
  grid leg) and mode 4 for a caller rect (the DEATH-window sibling).
- bit12 (set in 0xAF937) selects the GRID leg, not marker suppression:
  `(ctx & 0x1000) == 0 || ctx+48 == 1` routes straight to the bank walk,
  else the grid loops run FIRST and fall through into the same walk
  [orig: the `@0x5a5f40` grid-branch gate → `goto` into
  `MapOverlay_RenderAllLayers`]. The big map DOES draw the retained
  banks.
- rotation base `entity_ref = 0x40000000` for modes 2/3 — NORTH-UP after
  the transform's −90 fold. Blip sprites rotate against the VIEW base
  (world-stable facing on the north-up modes), not the player heading.
- zoom = `g_bigMapZoom @0xB76490`, spawn default **524288** (0x80000 =
  8× the spinmap's world extent) [orig: `Player_InitPlayer
  @0x4e1741..0x4e1754 — flt_7CD424 = 524288.0`]; the radar keys step IT
  while a mode is up [orig: the mode-gated case reads
  `@0x49bc98`/`@0x49be97`].
- the big map clips to its RECT (the box clip
  `clip_triangle_and_emit_vertices @0x688e30`); the circular stencil
  belongs to the corner spinmap alone. The corner spinmap keeps drawing
  under an active mode (both passes run per frame).
- the grid leg [orig: the `@0x5a5f40` grid loops off the bit12 branch]:
  vertical rules on the ABSOLUTE −150-wu lattice
  (`19660800·(center/19660800 − half) − 0x960000` start), horizontal
  rules on the plain 300 lattice, in pale yellow `unk_FFFF7F` at the ctx
  RULE alpha: the mode seeds (`@0x5a7e2f..44`) set rules 64 and
  labels/readout 128 for modes 2/3 (the 255-set mode-4 pair is unported);
  the ctx carries the pair at `+0x38`/`+0x40` (`@0x5a6722`/`@0x5a68a9`).
  The column letter is `HUD_FormatGridCoordinate(line_x −
  origin_snap)` drawn centered in the FOLLOWING gap — because the −150
  fold rides the lattice, every gap's letter agrees with the player
  MAPCOORDS formula everywhere inside it. Row numbers are a plain
  sequential counter seeded `(start − origin)/300 − 1` (no zero skip).
  The leg CLOSES with an on-map player readout `"(%s,%d)"` — the
  MAPCOORDS column fold + the zero-skip row — at the projected player
  point −(50,25) px. Letters, numbers, and the readout all draw with
  `HUD_DrawTextCentered_HalfBright` on **`g_hudLabelFontLarge`**. The
  exact label pixel anchors are capture-derived pending a retail M-map
  reference; the values and lattices are byte-witnessed.
- OpenNova: `Simulation.request_hud_map_cycle/get_hud_map_mode/
  get_hud_big_zoom_q16` over the engine `hud::HudMapControl` (mode
  lifecycle incl. the dead-player clear), the router's `map_toggle`
  edge, the second `HudMapPass` (`HudDrawList::big_map` + its own glyph
  list and raised canvas sandwich), and the mode legs in
  `HudMinimapCompiler`. The map-material alpha stage ignores texture
  alpha (the colormap binds as RGB). Witnessed-unported: the pan/drag
  input handlers (`sub_5432D0`/`sub_543360`/`@0x5434e0` family), the
  0xAF937 bits 11/13/14/15 semantics, mode 1, the mode-4 DEATH window
  (D-HUD-19), and the objectives-family draws retail layers above the
  big map (ours ride the flat pass).

Scope boundary: this ports the normal gameplay spinmap AND the M-map modes
2/3 above (the 400×400 window, the fullscreen north-up map, the bit-12 grid
leg). Still deferred: the pan/drag input hosting (the
`sub_5432D0`/`sub_543360`/`@0x5434e0` family), the 0xAF937 mask bits
11/13/14/15, mode 1, the windowed CMAP/DEATH `MapOverlay_DrawView` views
(D-HUD-19 — they share the banks above plus the §5.59 `MinimapSlot_*` player
registry, `MinimapSlot_FindOrAllocByEntityId @0x57b1e0`, fed by the player
wire messages), and the objectives-family-above-big-map frame ordering.


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
  (KOTH's separate directive list = `draw_koth_win_lose_directives @0x5ba500`,
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
| D-HUD-2 | oscarmike `spinmap.gd` conflated the IDB-misnamed stance function with a rotating compass-ring texture | the stance widget is discrete pre-rendered frames cross-faded on stance change. The `@0x599700` "radar" is separately the weapon heat bar, while the actual normal gameplay spinmap is `HUD_RenderAllOverlays @0x5a8070` → `HUD_DrawMapOverlay @0x5a5f40` in the authored HUDSPINMAP rect, gated by the declutter SPINMAP slot `dword_2723CC4` = `visible[17]` (visible at JOX's default `hud_detail 0` — the "compiled-in-true master switch" reading was the unloaded-BSS misread, corrected 2026-08-15; the 2026-07-18 "no in-HUD radar" gloss overstated: only the compass STRIP `@0x595470` and `draw_entity_labels @0x593820` are dead) | **FIXED 2026-08-13.** Stance and gameplay map are separate elements: discrete `HUDSTANCE` frames plus the heading-up terrain/blip spinmap with its counter-rotating `compring` overlay. The dead `HUD_DrawCompassStrip @0x595470` remains unported. |
| D-HUD-3 | — | Design space is fixed **1024×768**, scaled with round-to-nearest (`Viewport_ScaleToVirtualCoords @0x5d2b20`) | OpenNova authors HUD positions in 1024×768 and scales to the actual surface with the `(p*s+½s)/dim` rounding. |
| D-HUD-4 | — | Health bar *fill width* uses the capped `+92` ratio; *fill color* uses an uncapped recomputed ratio (`HUD_DrawHealthBar @0x5a2e50`) | Equivalent over `[0,1]`; recorded so the port matches both reads rather than collapsing to one. |
| D-HUD-5 | `hud_clip_indicator.gd` restamps its flash on (`round_type`, reserve) change | restamp keys are (`weapondef+220` ammo class, reserve, `weapondef+216` pool id) `[orig: @0x599ab2]` | Our weapon model runs a single ammo pool (net-re D-WPN-2), so the ammo-class/pool ids aren't distinct state yet; the proxy fires on the same reload/switch transitions. Revisit with per-class pools. |
| D-HUD-6 | `hud_messages.gd` is a timed line feed (930-tick life, ≥186 stagger, wrap, two-space continuation indent) drawn at the `HUDCHATTEXT` anchor | triggered text rides the full chat system: channel-2 ring buffers `[orig: Chat_AddDebugMessage @0x4987f0]`, display rebuild `[orig: @0x498bd0]`, and a channel geometry table (`dword_28E4DF8`, writer unwitnessed) | Message-line altitude port. The channel's exact screen geometry, per-line fade curve, and the player-chat channel are the chat-pipeline follow-up. |
| D-HUD-7 | **CLOSED 2026-07-31.** The HUD consumes the exact ERROR integer plus the sim's signed `pitchBlend(+0x380)>>7` and movement/weapon-weight `(+0x384)>>7`; the same live `pitchBlend` feeds the first-person camera and local/decoded-player aim overlays | spread adds `(player+0x380 >> 7) + (player+0x384 >> 7)` `[orig: HUD_DrawCrosshair @ 0x592640]`; the write/decay side is `[orig: RoundData_SpawnRound @ 0x4ec0d0]` + `[orig: Entity_UpdateInfantryPlayerBody @ 0x4b40e0]` | **FIXED.** Exact integer carriers now run ammo/weapon parse → runtime tables → round/body sim → local HUD/camera/overlay; decoded rows stamp and decay recoil, while their movement term remains the retail zero of a local-only producer. The projectile's intentionally different `R>>8` stays separate. The water-height category's position-only `CameraOffset.Z` projection remains D-INF-18, not D-HUD-7. Pinned by `npruntime_round_sim`, `infantry`, `netsim_client_replica_pipeline_recoil`, and `hud_helpers_test.gd`. |
| D-HUD-8 | crosshair color multiplies the texture (canvas modulate); default white | the strip writes the color to the **specular** channel with `diffuse = 1.0` `[orig: @0x5914d7]`; the blend-stage setup lives in the HUD shader pass (`GfxShader_ApplyPassChecked @0x677020`, unwitnessed); color source = user config `dword_25510E0` | Identical for the default white; witness the texture-stage state (and the config default) before modeling the user crosshair color. |
| D-HUD-9 | **CLOSED 2026-07-31.** The crosshair previously hid from generic settled ADS | it draws while an aimed shot is NOT available — `!Player_CanFireWeapon() @0x5cf780`, whose promoted predicates are Scoped (`Flags & 1`) or Sighted (`Flags & 2`, except SWITCHFROM); movement/water reject only the ordinary Scoped leg, while reload-card-switch, camera, dead/airborne, ForceScoped, and seat gates complete the verdict | **FIXED.** The sim now stamps that bounded retail verdict once and feeds both visibility and ERROR row selection. The reticle remains through ADS ease and follows the witnessed Scoped/Sighted failure/override gates rather than raw `scope_engaged`. |
| D-HUD-10 | the crosshair anchors at the fixed design center (512, 384) | the anchor is the projected aim point through `Viewport_ScreenToVirtual`: the literal screen center only for the on-foot local player with no camera mode `[orig: @0x5928a0]`; spectate / `g_camera_mode` (external/3P) project `Entity_BuildCameraView` (far point 65536000 q16 = 1000.0) `[orig: @0x592910..0x59295e]` | FIXED 2026-07-11 (weapon round): `LocalPlayerPresenter.aim_screen_point()` — `Vector2.INF` in first person (the HUD pins the exact center, matching `@0x5928a0`), the projected aim in third person; `GameHudPresenter` feeds it to both shells. |
| D-HUD-11 | **CLOSED 2026-08-16.** `Simulation::get_attach_labels` now consumes the same complete `local_player_can_fire_weapon` verdict used to stamp the body/HUD aimed row: alive/equipped, passenger-or-borrowed-UseGun seat, reload-card-switch, promoted Scoped/Sighted/SWITCHFROM, movement, air, eye/water, ForceScoped, binocular, and camera gates | `Player_CanFireWeapon @0x5cf780`; the label branch is `!Player_CanFireWeapon() || entity == nearest_entity @0x5a32df..0x5a3354` | **FIXED.** The promoted scope bit is the body mirror committed for the current sim tick, while camera/binocular state is queried live so a presentation-time third-person toggle cannot lag. `nova_simulation_test.gd::test_attach_labels_share_complete_can_fire_verdict` pins unraised ADS, settled ADS, immediate third person, and underwater behavior across two candidate entities. |
| D-HUD-12 | **FIXED 2026-08-11.** Attach labels lay out through the ported CGameFont engine with the witnessed BOLD Arial label font at the slot scale (`HudFrameCompiler::element_attach_labels` + `configure_label_fonts`) | `HUD_MeasureTextWH @0x580ab0` measures through the bold slot's (`g_hudLabelFontBold @0xB4C394`, ex "fontObj") `{handle, scale_x, scale_y}` pair (`CGameFont_MeasureText @0x674e70`); labels draw at raw screen pixels | Same glyph walk, same font file, same scale; box arithmetic `(x−w/2,y−2)..(x+w/2+5,y+h+1)` ported verbatim. Pinned by ctest `hud_frame_compiler` (label-font faces/scale). |
| D-HUD-13 | **CLOSED 2026-08-10.** The label color base is the hudpos `hud_textcolor` | the master overlay color `g_hudActiveColor @0x24c1868`; table slot 2 is refreshed per frame from hudpos `hud_textcolor` (`@0x5a8100`) and is the observed/default source | **The reimpl base is exact for the shipped observable path** and the dim transform stays ported (`HudAttachLabels.dim`). The scheme port (see "The hud_color_index scheme") is restored 2026-08-13 with the byte-witnessed producer: the `hudcolor` action row (code 10, default F6, retail-shadowed by `huddetail`) — neither of the earlier H mappings survives the catalog walk. |
| D-HUD-14 | no bottom prompts | `HUD_DrawGameplayOverlays @0x5bde60`: the preround armory prompt (`STROVER_ARMORY_INFO`, S2C-0x0A-fed `dword_A85B64`; SP never draws it), the vehicle-bay prompt (`Flags & 0x800` + team-gated groundEntity), the FARP wait/reload overlays (`attrib2 & 0x2000` + unlock mask) | Witnessed, deferred: each rides an unported system (MP preround state / vehicle.mnu / FARP rearm). The `STROVER_ARMORY_WAIT` leg is dead code in retail (the impossible `@0x5bdef8` recheck). |
| D-HUD-15 | **CLOSED 2026-07-22.** The drawer was already parity-complete; the missing half was the source. The accumulator is now witnessed and ported (D-WPN-4, net-re §5.62): heat is a DEADLINE on the slot, `def+880 × (slot+0x14 − tick)`, stamped once per shot by the recoil arbiter | heat = `WeaponSlot_CalcAccumulatedHeat @0x53f780` per frame, clamped to `0xFFFF` into `hudInfo+60` `[orig: HUD_BuildEntityInfo @0x4b852e, clamp @0x4b854d]` | Fed sim → weapon view → HUD with the clamp applied where the original's info builder applies it. The bar fills on the thirteen emplaced/vehicle guns that author `heat_values` and stays hidden on foot, because no infantry weapon authors heat in retail either. |
| D-HUD-16 | the SP waypoint track is built sim-side at mission load from the BMS nav channel (`flags & 2`) + pool-3 markers — no 0x0F wire leg in the loop | retail always routes the list through the S2C 0x0F apply, even in SP mode 3 (the local server serializes, the local client applies) | Same data, same selection rule, no serialization round-trip. The npwire 0x0F waypoint block already decodes (net-re §5.29); wire-parity for MP join is the npwire follow-up, not a HUD divergence. |
| D-HUD-17 | proximity advance ports the distance/last-entry/skip-done legs; `SpawnPoint_CheckWeaponRestrictions @0x4dbe80` (the AAS spawn-point weapon-restriction pass gate) is modeled as always-pass; the MP POI list (`Entity_BuildMapPoiLists @0x42de40`) and spectate reuse are unported | the restriction check reads the 4 weapon slots vs the event-system restriction mask and can force-advance | SP missions author no weapon restrictions on route markers; port the check with the AAS/MP HUD phase. |
| D-HUD-18 | GEOMETRY CLOSED 2026-08-12: the checkbox (16×16, 4 lines, 0xFFE0E0E0), the done-mark RED X (6 lines, 0xFFFF0000 — the old "checkmark" gloss was wrong), the `HUD_DrawLabelBox` rect (y−0x18 / +0x48 / +0x30), the measured-height row advance, and the exact alpha/gray color folds are ported into `element_objectives` with the panel alpha byte carried in the frame state. Remaining: the win-score add `@0x454526` (no score system), the "New Objective" toast `HUD_ShowObjectiveNotification @0x5ba2e0` (internals unwalked), the header unknown5[2]/[3] team-banner legs (`byte_A762D6/D7`), the KEY_O reimpl binding (input layer), `HUD_DrawLabelBox`'s internal box-shader styling (fill+wire stand-in at the witnessed rect), and the fontLarge/fontBold slot plumb (single HUD font stand-in) | disasm 2026-08-12 (the full drawer); score add `@0x454526`, toast `@0x4546e2` call site, banner masks `byte_A762D6/D7`, binding row = the input layer | The state machine, row walk, geometry, color folds, and chat/banner announcements are exact; the residuals each ride an unported system (score / notification / banner / input binding) plus the two cited drawer stand-ins. |
| D-HUD-19 | the DEATH deploy screen (`DeployScreenPresenter`, death.mnu) ships the authored chrome, the witnessed SPAWNPOINTS_LIST populate, and the pick flow — its MAP window renders no map image | the MAP window's render pass draws the windowed map view `MapOverlay_DrawView @0x5a58e0` (terrain layers + blips + labels; pan/zoom via `command_map_overlay_input_handler @0x554310`), the sibling of the fullscreen `HUD_DrawMapOverlay @0x5a5f40` | The pick behavior is complete without the image (the list is the pick surface); the map draw internals are the tracked next map-phase witness — port `MapOverlay_DrawView` and feed both the CMAP and DEATH windows from it; the 2026-08-13 gameplay-spinmap port (D-HUD-21) supplies the reusable compiler and banks to host there. |
| D-HUD-20 | **FIXED 2026-08-10** (core). The friendly tags (overhead name labels) are ported end to end for the SP/AI path: `world::collect_friendly_tags` → `Simulation::get_friendly_tags` → presenter projection/fog/KEY_F cycle → `HudFrameCompiler::element_friendly_tags` — the witnessed gates, health-tier colors, distance alpha, centered alpha-preserving half-bright text, the `'^'`+36-name fallback, the BMS→`[PeopleNames]` authored names, the BRIEF ticks, and the medic cross plate (feed pending) | the drawer is `HUD_DrawEntityLabel @ 0x5a39b0` off `HUD_DrawFriendlyTagsPass @ 0x5a4480` (full witness: the element map section). The 2026-08-10 hunt's dead ends stay recorded: `hud_draw_target_entity_overlay @ 0x59a5d0` = the targeted-GEAR overlay, `sub_599C20 @ 0x599c20` = the scope quad | Residues, each with its owning system: (a) the player-slot walk legs — callsign labels ride our MP roster, plus squad colors (`g_squadColors @0x83B450`, middle × 0.7), the flag-2 `"%s: %ld"` slot+16 count, the `<ch>`channel`<co>` wrap, and the slot+44 pulse; (b) the enemy magenta leg behind the server-granted `g_enemyTagsVisible @0x24D1DF4` (spectator/S2C 0x00A); (c) the medic-plate FEED — charattr.def `ATTRIBUTES` Medic(0x8) by `playerClass` (`CharAttr_LoadFromDef @0x412140`; AI classes unmodeled sim-side); (d) the wounded icon — `entity+885` writer unwitnessed (texture id 0x17); (e) the good-tier scheme swap is live again with the witnessed `hudcolor` producer (D-CTRL-4 tracks the reachability divergence); (f) the speaking-pulse LEVEL feed (formula ported; the dialog-channel amplitude is a device follow-up); (g) `entity+116` display height (writer unwalked; standing-constant stand-in) and the difficulty term of `Entity_GetMaxHealthWithDifficulty`; (h) the death-screen recolor/center-pin legs and the `0x27233DC/E0` latch bits (consumers unwitnessed). |
| D-HUD-21 | Gameplay spinmap ported and synchronized against retail JOTAC 00TRa (`HudMinimapCompiler` → `element_spinmap` → `HudOverlay`, snapshot v3): mission spawn zoom `65536/524288 × clamp(1 − Bms_MapZoom, 0.0625, 1)` with world-per-pixel `zoom/(rect height × 200)`, the true-pixel-circle disc `half-height − 4 px` + compass ×1.25 with the 0.05..0.95 UV crop, OOBJ footprints (opaque team fills, no boundary stroke), direct Colormap0..3 sampling + the depthspin water pass, the TSDicon MODULATE2X fold, the 253/254 fixed center rings, the M-cycle modes 2/3 + bit-12 grid, and 0x6B range-valid liveness | `HUD_RenderAllOverlays @0x5a8070` → `HUD_DrawMapOverlay @0x5a5f40` + the full witness map above | The residual in-map legs are this record's "Unported in-map legs" bullet (weapon-direction/timer/radar-contact, objective tether lines, entity/location labels, the tracked-target legs, the persistent-bank split + special layer-1/2 redraw quirks, the out-of-map siblings) plus big-map pan/drag, mask bits 11/13/14/15, modes 1/4, and the objectives-above-big-map ordering; the backing-disc color stays capture-calibrated pending a pass-state witness |
| D-HUD-22 | The HUDWPDINFO element renders "761 Marketplace" where retail (JOTAC 00TRa, identical pose) shows "760 m to Alley Corner" — same selected waypoint (distances agree within truncation) | the localized "m to" infix is an INDEXED string-table entry (witnessed in the RevX02 strings blob next to "m to FARP"; the composing drawer's table/index is unwitnessed), and the name pick is `get_waypoint_name @0x594630`'s raw-id vs +1-remap branch for gametype 0x30020 (or the mission-table source) | witness pass owed against the live JOTAC session; at the 32 m spawn waypoint retail shows the map's at-tip "032m" label while the HUDWPDINFO text row is ABSENT — a range or state gate on the info row to witness alongside the name/infix pass |

## Follow-ups (not yet witnessed / deferred)

- **Command/deploy map surfaces.** The normal `HUDSPINMAP*` call into
  `HUD_DrawMapOverlay @0x5a5f40` AND the M-map modes 2/3 — including the
  bit-12 300-unit grid with its `HUD_FormatGridCoordinate @0x598600`
  coordinate labels — are now ported above. Still deferred are the pan/drag
  input hosting (the `sub_5432D0` family), the 0xAF937 mask bits
  11/13/14/15, mode 1, the objectives-above-big-map ordering, and the
  WINDOWED sibling
  `MapOverlay_DrawView @0x5a58e0` (ex-sub_5A58E0; `(rect, centerX, centerY,
  scale)`) used by the CMAP command-map and DEATH deploy `.mnu` MAP windows.
  Their marker sources are the same 0x40/0x6B banks plus the §5.19/§5.59
  `MinimapSlot_*` player registry (`MinimapSlot_FindOrAllocByEntityId
  @0x57b1e0`, `MinimapSlot_InitBlipFromPackedId @0x57b080`; fed by
  `NapiNPClientMsg_CharMinimapUpdate`/TeamAssign/HandlePlayerSpawn/
  FullEntitySpawn). Supporting inventory for those legs:
  `minimap_draw_ring_blip @0x597320`, `draw_billboard_decal @0x5975f0`,
  `draw_minimap_blip @0x597890`, `WPNames/STRWPNAME%03d`,
  `STROVER_OBJECTIVEPOINT_SHORT`/`STROVER_DEFENSIVEPOSITION`, `%01.2fk`.
  The gameplay compiler is reusable substrate, but those surfaces require
  their distinct view state and UI orchestration — D-HUD-19.
### The message feeds (`HUD_DrawMessageFeeds @ 0x59ad30`)

One routine paints BOTH message channels, chat first then system. Three
properties are witnessed and now ported (D-HUD-23):

* **Only three ring rows are walked per channel** `[orig: the walk
  @0x59ae5e..0x59aebf stepping -0x80 from 0xb427bc]`, and because the sink puts
  the newest line in row 0, the OLDEST of the three sits at the anchor with each
  newer line **18 design px BELOW** it `[orig: local_4 = 0x12 @0x59ad97, scaled
  through Viewport_ScaleToVirtualCoords @0x5d2b20]` — the feed grows downward.
* **The alpha ramp belongs to the CHAT ring only.** The loop computes
  `clamp(timer * 255 / 186)` for both, but the system draw passes the STORED
  color `[orig: @0x59ae97]`; only the chat path folds the computed alpha into it
  `[orig: @0x59adef]`. A feed line therefore holds its color for its whole
  930-tick life and vanishes, while a chat line fades over its last 186 ticks.
* **The anchors differ**: the system feed sits at `HUDSYSTEXT` (already parsed
  into `HudposFile.hud.sys_text`), the chat ring at `HUDCHATTEXT`.

The line content is never composed by the client: the 0x1E handler picks a
"Canned Msg" template and substitutes `$A` (attacker) / `$B` (victim)
`[orig: HUD_FormatKillEventMessage @ 0x422DA0 -> Chat_FormatMessage @ 0x422C60]`.
Colors are witnessed per class: own-kill white / other grey `[orig: the palette
writer @0x51f240]`, the team palette when the canned key names BLUE/RED, the
medic pair in `0xFF008CEE` `[orig: cases 38/45 @0x426270]`, and camp events
59/60 in LITERAL colors that bypass the palette `[orig: @0x62172/@0x62179 and
@0x62197/@0x62204]`. Four LFP result types format a line and post NOTHING
`[orig: 50/51/52/53 @0x62051-0x62084]`, and type 58 reaches the tip system only
`[orig: CTipSystem_HandleEvent 17 @0x62147]`.

Two classification corrections landed with the port: the medic pair (38/45) is
NOT a kill, and the killer-less deaths (1/2/3 suicide, 22/23/25/26) are their
own class whose victim/aux slots are LITERAL ZERO on the wire `[orig:
GameEvent_PlayerDeath @0x516DD0 leaves v41/v42 = 0]` — reading them charges the
death to entity 0 (the host).

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
- **CGameFont glyph layout** `[orig: CGameFont_DrawText @0x6752c0]` — per-glyph
  D3D vertex build / spacing, to confirm `FntResource` layout parity.
- **Timer/score, altitude/power bar, weapon slot bar** — enable flags
  witnessed; draws (`HUD_DrawWeaponSlotBar @0x599cd0` located) unwitnessed.
  (The ex-"reload bar" is the PowerThrow charge bar — witnessed + ported,
  world-wac-ai-re §27.3.)
- **MP objective status + stance team tile** — witnessed
  (`HUD_DrawTeamIdLine @0x59aa30`, `@0x59a0cb`); port with the MP HUD.
- **Parachute/armor icons** — witnessed
  (`HUD_DrawParachuteAndArmorIcons @0x5925c0`): entity+44 `&0x10` parachute /
  `&0x8` armor (through the info struct's entity pointer; the +44 writer is
  unwalked — the earlier "+36" note was a recon guess, corrected 2026-07-18).
- **Armory-delay label leg** — the `g_hudLabelFmtArmoryDelay % dword_A85B6C`
  variant of the floating armory label and the bottom-prompt cluster
  (D-HUD-14) key off the S2C 0x0A header armory/preround state; wire when the
  net views surface it.

## IDB changes

Applied 2026-08-15 (the post-merge review + declutter session; IDB saved).
The misnomers earlier sessions recorded here "rather than renamed" are now
renamed:

- **Renames this session**: `g_hudFrameCounter @0xA87064` (ex `dword_A87064`
  — the per-main-frame counter, NOT device caps), `Game_TickHudFrameCounters
  @0x434c00` (ex "PlayerStats_IncrementDeathCounters" — the `++` site
  `@0x434c23`, called from `Game_ProcessMainFrame @0x5265d5`),
  `Game_ResetSessionHudState @0x434bd0` (ex "PlayerStats_ClearAll"),
  `HUD_DrawBreathBar @0x59D6F0` (ex "CaptureProgressBar" — it is the Breath
  bar, label Overlays/STROVER91), `HUD_DrawLookModeLabel @0x594100` (ex
  `sub_594100`), `HUD_DrawTeamIdLine @0x59AA30` (ex
  "draw_objective_status_text"), and `ScarOverlay_StoreParams4 @0x5891b0`
  (the ex-"qt_register_signal_spy_callbacks" Qt FLIRT false positive — a
  4-dword parameter stash read by `render_scar_debug_overlay @0x589220`,
  not shadow callbacks).
- **Earlier applied renames this log never recorded** (the spinmap
  sessions): `HUD_DrawMapTargetPointer @0x599220` (ex
  "CTerrainTile_UpdateShadowState"), `HUD_BuildMapOverlayView @0x5a7e10`
  (ex "render_glow_effect"), `HUD_CycleMapMode @0x520bc0`,
  `HUD_UpdateWaypointAltitudeColor @0x590970`, `HUD_DrawPlayerGridLabel
  @0x59cb40`, `g_gridOriginEntity`, `g_spinmapWpDistLabelOff @0x27237C0`,
  `g_mapCoordsLabelX/Y/Off @0x27236F4/F8/FC`, `g_texWpIndicator`,
  `g_mapYaw180 @0x2723EB0`, `Bms_MapZoom @0xA7640C`, `g_squadColors
  @0x83B450`, `g_rules_flags @0x24D1E34`.

Applied 2026-08-13 (the gameplay-spinmap grill; HTTP-fallback session):
`MapView_SetTransform @0x607130` defined (`add_func 0x607130..0x6071c0` —
the region was undefined) and named — the unreferenced twin of
`render_terrain_decal`'s transform-global tail. Read-only elsewhere
(`Server_DumpPuntLogToFile` as the code-48 dispatcher arm was checked and
is correct); the misnomers spotted then are renamed in the 2026-08-15
block above.

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
  (the "static -1 element-switch block" — a comment since REFUTED and
  rewritten by the 2026-08-15 declutter witness), `0x42e4b8` (the 0x0F
  client apply).

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
