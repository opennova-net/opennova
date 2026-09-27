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
the overlay by `godot/game/world/game_hud_presenter.gd`. The former ONED HUD
preview was removed by ADR 0037. The 2026-06-22 session witnessed the
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
The 2026-09-16 pass witnessed and ported the **scoped-view circle mask**
(`Hud_DrawScopeCircleMask @0x5d17a0` + the reticle cross/grid
`draw_minimap_crosshair_and_grid @0x5d1160`), refuting this record's
"rowless-weapon fallback" gate reading — the mask draws on every Scoped frame
and the SIGHTS row count gates only the inner cross and grid — and corrected
the HUD declutter level arithmetic to retail's unclamped 8-bit form.
The 2026-08-15 post-merge review witnessed and ported the **HUD declutter**
system (`huddetail`/`HUDDECLUT_*` — refuting the July "compiled-in `-1`
master switch" reading), corrected the map-pointer blink and 253/254
ring glosses, and applied the previously parked IDB renames (logged at the
end).

The 2026-09-19 [weapon and vehicle HUD validation](weapon-vehicle-hud-validation.md)
corrected seat-dependent group dispatch, mounted stance, the Inset reticle
predicate, and capacity-one reload flashing (D-HUD-28). It also records the
completed launcher targeting, Inset scene, mortar impact HUD, and pilot instruments.

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| Scope camera zero + range/elevation/magnification text | MATCHING (D-HUD-27, D-HUD-30) | `[orig: Render_ProcessMainSceneFrame @ 0x5ca0f0; HUD_DrawScopeOverlayDetails @ 0x59e420]`; `local_player_view`, `hud_frame_compiler`; windowed default L115A prone probe |
| Render pipeline + two-struct model | confirm-only (read-only grill) | `[orig: HUD_RenderAllOverlays @0x5a8070]` → `[orig: HUD_RenderOverlays @0x5a7bb0]` → element draws; per-frame `[orig: HUD_BuildEntityInfo @0x4b8440]` |
| Virtual coordinate space (1024×768) | ported (`engine/runtime/hud/hud_math` — the virtual-coords scale) | `[orig: Viewport_ScaleToVirtualCoords @0x5d2b20]` exact formula; `hud_helpers_test.gd` |
| Health bar | ported (`HudFrameCompiler::element_health` + `hud_math::health_color_band_fp16`) | `[orig: HUD_DrawHealthBar @0x5a2e50]` rect/fill/threshold-color; `hud_helpers_test.gd` thresholds |
| Stance indicator + cross-fade (IDB-misnamed "compass") | ported (`HudFrameCompiler::element_stance` — frame draw, offsets, fade pair, prev/current state) | `[orig: HUD_DrawStanceIndicator @0x599f10]` full witness incl. fade pair + per-frame offsets; `hud_helpers_test.gd` fade curve |
| HUD text + half-bright | ported (`engine/runtime/hud/game_font` — the CGameFont text engine) | `[orig: HUD_DrawTextRightAligned_HalfBright @0x580850]` → `[orig: CGameFont_DrawText @0x6752c0]` |
| Ammo count + weapon name text | **ported** (`HudFrameCompiler::element_weapon_cluster` + `hud_math::format_ammo`) | `[orig: hud_draw_weapon_ammo_and_name @0x5939d0]`; format/hide/alignment/nudge witnessed; `hud_helpers_test.gd` format_ammo |
| Clip + rounds indicator (HUDCLIPGFX/HUDRNDGFX) | **ported** (`HudFrameCompiler::element_clip_indicator`, D-HUD-5) | `[orig: draw_hud_ammo_indicator @0x599a30]`; parse `[orig: @0x5442fc]`; `hud_helpers_test.gd` round_icon_count + flash |
| Crosshair / reticle + spread | **ported** (`HudFrameCompiler::element_crosshair`, D-HUD-7 CLOSED; D-HUD-8/9/10; target cursor / aim-point quad / friendly brackets ported 2026-09-19) | `[orig: HUD_DrawCrosshair @ 0x592640]` + `[orig: HUD_DrawCrosshairCornerQuad @ 0x590f50]`; accumulator producers `[orig: RoundData_SpawnRound @ 0x4ec0d0]` + `[orig: Entity_UpdateInfantryPlayerBody @ 0x4b40e0]`; `npruntime_round_sim`, `infantry`, `netsim_client_replica_pipeline_recoil`, and `hud_helpers_test.gd` |
| Standard weapon SIGHTS card | **ported** (`world::weapon_sights_card_eligible` → sim `scope_card_active`; `HudFrameCompiler::element_sights_card` + `godot/game/world/hud_sights_card.gd` materialize the authored rows) | `[orig: Render_ProcessMainSceneFrame @0x5ca299..0x5ca304 / @0x5caaf3..0x5cab15]`; Scoped/Sighted selectors + SWITCHFROM + NoCardSwitch/ForceScoped suppression; `simulation_test.gd` + `hud_overlay_test.gd` + ctest `weapon_fsm` |
| Scoped-view circle mask + reticle cross/grid | **ported** (`engine/runtime/hud/scope_circle_mask.*` → the `HudPos.scope_mask_*` statics → `godot/game/world/hud_scope_circle_mask.gd`; the record's old “rowless-weapon fallback” gate reading corrected 2026-09-16) | `[orig: Hud_DrawScopeCircleMask @0x5d17a0]` + `[orig: draw_minimap_crosshair_and_grid @0x5d1160]`; the unconditional Scoped-arm call + the `!rows` crosshair argument `[orig: Render_ProcessMainSceneFrame @0x5cab08..0x5cab15; render_hud_overlay @0x5d82e5..0x5d82f2]`; ctest `sight_overlay` |
| First-person view effects: binocular mask/crosshair/rangefinder, NVG mask + gain scale | **ported**, layout witnessed 2026-09-24 (`engine/runtime/hud/view_effects.h` → `godot/game/world/player_view_effects.gd`); the NVG image under the mask is FrameFX's render-to-texture chain, the underwater murk is the render overlay stage's (§First-person view effects) | `[orig: Binoculars_DrawMask @0x5cfe95..0x5cff5b; NVG_DrawMaskAndGain @0x5cffab..0x5d004a; Binoculars_DrawRangefinder @0x5908c0; NVG_Composite @0x5d107e..0x5d1080]`; GUT `hud_overlay_test` |
| ALPHAFADE semantics | **ported** (`hud_math::fade_decay`/`fade_flash_alpha`) | `[orig: parse @0x5a086c]` ×2.55/×2.55/×62; flash curve `[orig: @0x599af9]`; `hud_helpers_test.gd` |
| Attach labels (seat/armory floats) | **ported** (`VehicleSystem::collect_attach_labels` + `LocalPlayer::local_player_can_fire` + `HudFrameCompiler::element_attach_labels` + `game_hud_presenter.gd`, D-HUD-11/12/13 CLOSED) | `[orig: draw_vehicle_seat_and_armory_labels @0x5a3290]` full witness; nearest-entity branch consumes complete `Player_CanFireWeapon @0x5cf780`; label strings `[orig: HUD_InitOverlaySystem @0x5a479c..0x5a481e]`; `attachtextid` parse `[orig: @0x544d6c]`; the bold Arial label font + slot scale `[orig: @0x5a3680; HUD_InitAllFonts @0x51ee20]` ported 2026-08-11; ctest `vehicle_mount` + `def_parse_weapons`/`def_parse_items`; GUT `simulation_test.gd`/`hud_helpers_test.gd` |
| Friendly tags (overhead name labels) | **ported** (`world::collect_friendly_tags` + `HudFrameCompiler::element_friendly_tags` + `game_hud_presenter.gd`, D-HUD-20) | `[orig: HUD_DrawFriendlyTagsPass @0x5a4480]` → `[orig: HUD_DrawEntityLabel @0x5a39b0]` full witness; names `[orig: Entity_SpawnFromBMSRecord @0x40ecbf]` + the 36-name fallback `[orig: g_fallbackPeopleNames @0x840a78]`; modes/toggle `[orig: @0x49b573]`; eye-offset anchor `[orig: @0x4bf078..0x4bf14c]` + Arial label font `[orig: HUD_InitAllFonts @0x51ee20]` witnessed + ported 2026-08-11; ctest `hud_math`/`hud_frame_compiler`/`infantry`/`promote` |
| Armory/vehicle-bay/FARP bottom prompts | draw/feed ported; remaining menu/system integration in D-HUD-14 | `[orig: HUD_DrawGameplayOverlays @0x5bde60]` — preround/0x0A armory prompt, Flags 0x800 bay prompt, FARP wait/reload |
| BMS triggered text (OutputText) and the console lines | **ported** (`HudFrameCompiler::push_message` → the SYSTEM ring drawn by `element_feed`, D-HUD-6) | `[orig: HUD_DisplayTriggeredText @0x51f190]` → `[orig: Chat_AddMessageChannel2(text, -1, 930) @0x51f216]`, the SAME ring as the 0x1E lines; the WAC consol/pconsol/consol# and forceanim lines (`debug_text`) post there too; `hud_helpers_test.gd` expiry |
| WAC text/ptext/text#, the WAC lose line, the BMS subgoal won/lost lines | **ported 2026-09-23** (`HudOverlay.push_chat_line(line, -1)` → the CHAT ring, D-HUD-6) | `[orig: Chat_AddSystemMessage @0x4EDB50]` (the `Chat_AddMessageChannel1` call `@0x4EDB5C`, color −1 `@0x4EDB59`, 930 ticks `@0x4EDB54`; text# through `Chat_AddFormattedIntMessage @0x4EDB70`, the sprintf `@0x4EDB9E`) and `[orig: GameMsg_AddChatLineAndRelay @0x5BA170]` (the `Chat_AddMessageChannel1(line, -1, 930)` call `@0x5BA197`; callers `WacAction_Lose @0x4ED3F0` and `EventAction_Dispatch @0x4542E0` cases 14/15, the calls `@0x454578` / `@0x454632`) → `[orig: Chat_AddMessageChannel1 @0x4985D0]`, the CHAT ring |
| Message feed — the SYSTEM ring (kills / objectives / medic) | **ported** (`HudFrameCompiler::element_feed` + `hud::feed_format` + the `netsim` 0x1E fold, D-HUD-23) | `[orig: HUD_DrawConsoleMessages @ 0x59ad30]` (second loop) fed by `[orig: NetPacket_HandleGameEvent @ 0x426270 -> HUD_FormatKillEventMessage @ 0x422DA0 -> Chat_FormatMessage @ 0x422C60]` |
| `hudpos.def` parser token map + 4-field positions | ported (`engine/formats/def`) | `[orig: HUD_ParseHudposToken @0x59f370; AMMOCOUNTPOS @0x59fc3d]`; ctest `def_parse_hudpos` |
| Parachute / armor status icons | witnessed — port pending (entity+44 flag writer unwalked) | `[orig: HUD_DrawParachuteAndArmorIcons @0x5925c0]` — entity+44 `&0x10` parachute / `&0x8` armor through the info struct's entity ptr; `ParachuteIcon`/`ArmorIcon` tokens |
| MP objective status text + team tile | confirm-only — MP HUD phase | `[orig: HUD_DrawTeamIdLine @0x59aa30]` (ex "draw_objective_status_text") client/strcli* strings witnessed |
| Weapon heat bar (HUDHEAT) | **ported** (`HudFrameCompiler::element_heat`, D-HUD-15) | `[orig: HUD_DrawWeaponHeatBar @0x599700]` (ex kong "draw_minimap_overlay" — a misnomer; there is no radar here) full witness: border + proportional fill in the HUDHEAT rect |
| Waypoint HUD label (HUDWPDINFO) | **ported** (`HudFrameCompiler::element_waypoint` + `game_hud_presenter.gd`, D-HUD-16/17) | `[orig: HUD_DrawWaypointNameAndDistance @0x5947a0]` + `[orig: get_waypoint_name @0x594630]` full witness; gates `[orig: @0x5a7daf]` |
| Waypoint track (list/current/advance/mission gate) | **ported** (`engine/runtime/world` waypoint track + `Simulation`, D-HUD-16/17) | list `[orig: NetPacket_WriteWorldStateLoad0x0F @0x502d10 @0x502e41]` (nav channel `flags&2`); BMS marker fields `[orig: Entity_SpawnFromBMSRecord @0x40f0aa]`; advance `[orig: Player_UpdatePerFrame @0x4de5f7]`; done-mark `[orig: EventTrigger_MarkLinkedSpawnPoints @0x452ce0]`; cycle `[orig: Spectator_CycleTarget @0x4dc1d0]` + input case 23 `[orig: @0x49b3de]`; `ShowWaypoints` `[orig: Game_SetShowWaypoints @0x58fb50]` |
| Gameplay spinmap (`HUDSPINMAP*`: heading-up terrain, blips, pulse markers, waypoint tether/distance, compass ring) | **ported** (`HudMinimapCompiler` → `HudFrameCompiler::element_spinmap` → `HudOverlay`; retained 0x40/0x6B state in `ClientReplicaPipeline`; in-map indicator/label legs = D-HUD-21) | `[orig: HUD_RenderAllOverlays @0x5a8070 (gate @0x5a86e8, mask 0xD07FF @0x5a86f0)]` → `[orig: HUD_DrawMapOverlay @0x5a5f40]`; transform `[orig: Terrain_FixedPointToWorldFloat @0x607060]`; terrain `[orig: render_terrain_decal @0x6071C0]`; blips `[orig: MapOverlay_RenderAllByLayer @0x5be590 → render_minimap_slot_blip @0x5be240]`; compass `[orig: draw_compass_indicator @0x59c900]`; ctests `hud_frame_compiler`/`client_minimap_overlay`/`minimap_overlay` + GUT `hud_overlay_test.gd` |
| Fullscreen / CMAP / DEATH map surfaces | witnessed — deferred (D-HUD-19) | shared fullscreen `HUD_DrawMapOverlay @0x5a5f40` legs plus windowed `MapOverlay_DrawView @0x5a58e0`: pan/zoom, grid coordinates, command/deploy labels and window hosting |
| Objectives panel + subgoal state (MISSION OBJECTIVES) | **ported** (`world::SubgoalState` + `HudFrameCompiler::element_objectives` + `game_hud_presenter.gd`, D-HUD-18; the toggle is polled from catalog row 55 `Goals` (dispatch 31, default G) since 2026-09-10 — the earlier hard-coded O collided with `dotsize`) | `[orig: HUD_DrawWinConditions @0x5ba940]` full witness; actions 14/15/35/36 `[orig: EventAction_Dispatch @0x454500/@0x4545e0/@0x4546af/@0x454724]`; toggle `[orig: @0x49b68b]`; ctest `event_runtime_bms` subgoal block; the objective notification and the 0x3F relays since 2026-09-23 (`[orig: HUD_ShowObjectiveNotification @0x5BA2E0]`; ctests `bms_hud_relay`, `hud_game_text`) |
| HUD declutter (`hud_detail` + `HUDDECLUT_*` masks) | **ported** (`engine/runtime/hud/hud_declutter.*` + `HudFrameCompiler` per-slot gates + the shell's persisted `hud_detail`) | `[orig: HUD_ParseHudposToken @0x59F370 mask arms → CRenderState_SetLayerVisibility @0x59B0F0 → dword_2723C80]`; cycle `[orig: Input_HandleActionBinding_0 @0x4e060b..24]`; level-3 blackout `[orig: @0x5a80c4]`; death force-3 `[orig: @0x42e410]`; the full section below |
| Mounted-vehicle panel (VEHICLE_HUD silhouette + seat markers) | **ported end to end** (2026-08-21: `HudFrameCompiler` vehicle-panel leg + `hud_vehicle_panel.h` band/marker policy + the `def_hudpos` VEHICLE_HUD blocks + `world/vehicle_panel_feed` (the re-root, the slot list, the three marker arms) + `HudOverlay::set_vehicle_panel` (the per-sid `interface` upload) + `vehicle_panel_presenter.gd` — the device + lane landed 2026-08-21, D-HUD) | `[orig: HUD_DrawVehicleHealthBars @0x5a4fd0; Entity_BuildWeaponSlotList @0x434c60; Entity_GetMountSlotBoneIndex @0x546680; the VEHICLE_HUD arms of HUD_ParseHudposToken @0x59f370 (@0x59f380..0x59f5cb)]`; ctest `hud_vehicle_panel`, `hud_frame_compiler`, `vehicle_panel_feed`, `def_parse_hudpos` |
| Recent Messages window (the J-key `OldMessages` history) | **ported** (2026-08-21: `hud_frame_message_log.cpp` over the two display-slot rings, `hud_message_log.h` layout; the `OldMessages` toggle lane `message_log_presenter.gd` landed 2026-08-21, D-HUD) | `[orig: HUD_DrawMessageLog @0x5b9d70]` (IDB-renamed 2026-08-21, ex `draw_credits_scroll`); the `g_showMessageLog`-only gate `[orig: Server_DrawStatusScreen @0x50b211..0x50b21f]`; ctest `hud_message_log`, `hud_frame_compiler` |
| Message feed — the CHAT ring (player chat, S2C 0x14) | **ported** (2026-08-21: the netsim 0x14 fold → `ClientChatLine`, `HudFrameCompiler::push_chat_line` + `chat_wrap_text` (the display-slot sink), `hud::chat_channel_sink/color`, the HUDCHATTEXT first loop of `element_feed`; D-HUD-6 narrowed to the announce banner) | `[orig: Chat_AddMessageChannel1 @0x4985d0; Chat_DispatchToChannel @0x42b910; HUD_DrawConsoleMessages @0x59ad30 (first loop); HUD_GetChatBoxCoord @0x5bbe90]`; ctest `hud_frame_compiler`, `client_replica_chat` |
| AAS zone status panel (LFP objective markers + Under Attack / Ready text) | **ported** (2026-08-21: `hud_frame_lfp_panel.cpp` + `hud_lfp_panel.h` + `world/lfp_feed` + the 0x6F contest bytes retained in `ZoneState`; `HudOverlay::set_lfp_panel` + `lfp_panel_presenter.gd` landed 2026-08-21, D-HUD; residuals in the section below) | `[orig: HUD_DrawZoneStatusPanel @0x5a2480; HUD_DrawZoneMarker @0x5986f0; HUD_LoadAllTextures @0x59e0a3..0x59e11f; ZoneTimerList_SetEntryValue @0x537ec0]` (IDB-renamed 2026-08-21, ex `draw_capture_point_*`); ctest `hud_lfp_panel`, `lfp_feed`, `hud_frame_compiler`, `npruntime_client_runtime` |
| Map medic marker (the teammate-blip replacement) | **ported** (2026-08-21: `HudMinimapMarker::medic` from the charattr Medic bit → `medic_cross_quads` overlays in `hud_minimap.cpp`; snapshot v4 + the decoder landed 2026-08-21, D-HUD; the charattr ATTRIBUTES loader behind `World::tables.class_attribute_flags` — `CharAttr_LoadFromDef @0x412140` — is ported in this PR (R2): the field had no loader before the final review) | `[orig: draw_entity_labels_and_markers @0x5a49e0 (the call @0x5a4d40); AnimMap_IsSlotActive @0x4125e0; CharAttr_LoadFromDef @0x412140]`; ctest `hud_frame_compiler` (the spinmap medic case) |

## Render pipeline — the two-struct model

The HUD keeps **layout** (parsed once) separate from **per-frame state** (rebuilt
each frame); the draw code reads both.

- **Layout globals** — parsed once at load from `hudpos.def` by the parser
  callback `[orig: HUD_ParseHudposToken @0x59f370]` (once an undefined `loc_59F370` blob, a `_stricmp`
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

The per-element visibility system behind the flag table above. State: TWO
level cells plus the 24 hudpos mask bytes `byte_2723CE0[24]`. The persisted
config value `hud_detail` (0..3) lives in the config struct
(`g_GameConfigState.hudDetail_518`: parse `@0x550339`, default 0 `@0x54d3d8`,
written to `game.cfg` by `Game_SaveConfig @0x54c80d`); the LIVE layer level is
`layerIndex @0x24D20BC`, written by the mission-start apply
`apply_session_settings_to_globals @0x55154d` (config -> live, called from
`Game_StartMission @0x524662`, `SinglePlayer_StartMission @0x561c28` and the
session create/join paths), by the `huddetail` cycle `@0x4e060b..14` and by
the death force `@0x42e412` — neither of the last two touches the config
struct, so a cycled or death-forced level ends with its mission and the
config value alone survives a restart. (Corrected 2026-09-12 during the 00TRa
playthrough acceptance: this record had folded both cells into one "persisted
cfg cell", and the port persisted every live write, which left the HUD blank
in every mission after a death screen; `game_hud_presenter.gd` now keeps the
two states and re-seeds the live level at every world load.) Rule:
`visible[slot] = ((uint8_t)(1 << hud_detail) & mask[slot]) != 0`, rebuilt into
`dword_2723C80[24]` by `CRenderState_SetLayerVisibility @0x59B0F0` on every
mask or level change.

**The level arithmetic is unclamped and 8-bit (witnessed 2026-09-16).**
`CRenderState_SetLayerVisibility` is `mov ecx,[esp+level]; mov edx,1;
shl edx,cl; ... mov cl,byte_2723CE0[eax]; and cl,dl` `@0x59B0F0..0x59B10A`: the
shift is a full 32-bit `shl` whose count x86 takes modulo 32, and the
`and cl,dl` that follows keeps only the LOW BYTE of the result. So levels 4..7
select bits no `HUDDECLUT_*` parse arm ever authors and hide every gated
element, levels 8..31 leave a zero byte and hide everything too, level 32
aliases level 0, and a negative level lands on `cl = 31` (hidden). Nothing
clamps on the way in either: the cfg token is stored raw (`call atol;
mov g_GameConfigState.hudDetail_518,eax` `@0x550330..0x550339`), applied raw
(`mov layerIndex,edx` `@0x55154d`), and the `huddetail` cycle compares the SUM
(`add eax,ebx; cmp eax,3; jle` `@0x4E0606..0x4E0610`), so any parked
out-of-range level wraps back to 0 on the first press.
`HudDeclutter::set_level` / `HudDeclutter::rebuild` mirror that exactly since
2026-09-16 (the earlier 0..3 clamp is gone, and `HudOverlay::set_hud_detail_level`
no longer clamps either); ctest `hud_frame_compiler` pins levels 4..7, 8, 31,
32, -1 and the wrap from 9. The shell's persisted-cfg read no longer clamps
either (`hud::clamp_hud_detail_level` was deleted with this fix), so a
hand-edited `hud_detail`, negative included, reaches the compiler raw like
every other level.

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
- **Forced levels** — death forces the live level to 3 `@0x42e410..1c`; the
  HUD reset re-applies the LIVE level `@0x59dd75`; the persisted config value
  returns only through the next mission start's settings apply `@0x55154d`.
- **`showhud`** (code 14, unbound by default) cycles `g_FpWeaponViewFlags =
  (v + 1) & 3` `@0x4e0561`: bit 0 = the FP gun, bit 1 = ONLY the
  FP-weapon+spinmap sub-pass `@0x5a8635`. The whole-overlay master gate is
  the separate `/NOHUD` `dword_840B18 & 2`. The FP draw's bit-0 test is
  skipped for an Emplaced weapon def (`@0x4dedd9..0x4dedf1`), and a scoped
  Inset weapon the player can fire draws no FP model
  (`Player_CanFireWeapon && IsScoped && def+0xC & 0x200`, `@0x4dedf7..0x4dee19`);
  both ported 2026-09-24 (`world::fp_viewmodel_retail_submit`,
  `FpViewmodelSubmitGates`).
- **The mission HUD item flash** (corrected 2026-09-23: BMS action 28's sub 37,
  `EventAction_HandleSpecialTypes @0x4535A0`, the case `@0x4535CD`, not a WAC
  action): `RenderState_SetLayerVisibilityByIndex @0x5A3020` (a misnomer) stores
  `dword_2723CF8[p1] = p2` with the index unchecked (`@0x5A302A`), then
  `CRenderState_SetLayerVisibility @0x59B0F0` with 0 (the call `@0x5A3031`) rebuilds the HUD layer table
  at declutter level 0 without writing the stored level. The sixteen timers tick
  in `sub_59A9E0 @0x59A9E0` (from `HUD_RenderAllOverlays @0x5A8070`, the call
  `@0x5A80FB`): each nonzero timer drops by the tick delta since the last HUD frame,
  clamped at 0; the compare is signed (`jle` `@0x59AA08`) and its last-tick static
  `dword_2723EA8` is never reset, so a clock that restarts lower grows an armed
  timer. Readers draw only while a timer is 0 or has bit 0x10: the altitude bar
  (`HUD_DrawAltitudeBar @0x59F050`, [0] `@0x59F168`, [1] `@0x59F340`), the compass
  strip (`HUD_DrawCompassStrip @0x595470`, [5] `@0x595CAC`, [14] `@0x5958D0`), the
  map overlay's tracked-target pointer (`HUD_DrawMapOverlay @0x5A5F40`,
  [14] `@0x5A77FE`) and waypoint state line ([5] `@0x5A785B`), and the minimap
  blips, each skipped while its timer has bit 0x10 (`draw_minimap_blip @0x597890`: [12]
  `@0x597E0A` for the 0xFF204080 blips, [13] `@0x597E29` for 0xFF802020, [15]
  `@0x597E45` class 5, [10] `@0x597E65` class 3, [11] `@0x597E84` class 0).
  Sub 38 zeroes the input word (`@0x4535C2`); sub 39 stores `p1 == 0` into
  `dword_AE0718` (`@0x4535B6..0x4535BC`), a dead store; every other sub returns
  (`@0x4535B4`). Ported 2026-09-23: the engine emits `hud_item_flash`, the
  presenter hands it to `HudOverlay.set_item_flash` (`HudItemFlash::set` plus
  `HudDeclutter::apply_level(0)`, the stored level untouched), and the timers
  tick on the logic tick each HUD frame; wired consumers are [5] (the spinmap
  waypoint state line) and [0] (the whole altitude bar, every draw of which sits
  after the `@0x59F168` gate). Not wired yet: [1] (its tail `@0x59F340` only calls
  a device-state setter, not a draw), the compass strip's [5]/[14] (the element is
  unported), the tracked-target pointer [14] (no tracked-target source) and the
  blip gates, which key on the blip color and class arguments and need those
  mapped onto `HudMinimapMarker` first.
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
| 2 | BREATHTIME | breath bar (`HUD_DrawBreathBar @0x59D6F0`, ex "CaptureProgressBar" — label Overlays/STROVER91; ported, "Breath bar" below) |
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
| 23 | CHAT | chat feed (mask test `@0x59AD33` — which ALSO carries a second hard `level >= 2` cull of its own `@0x59AD43`) |

**Port** (lands with this record's 2026-08-15 revision): the engine declutter
module `engine/runtime/hud/hud_declutter.*` (`HudDeclutter`: the mask/level
model, the parse-arm token table and the rebuild rule) feeds
`HudFrameCompiler`'s per-slot gates. The `huddetail` cycle is the one
`hud::next_hud_detail_level` (`engine/runtime/hud/hud_config_tokens.h`, with
`kHudDetailLevelMax` the one level constant), stepped by the HUD-toggle poll
(`hud::hud_toggles_poll`, driven by `game_hud_presenter.gd`'s
`HudToggles.poll`); the shell persists `hud_detail` like the retail config
token, and the binding defaults to F6, winning the shared key from `hudcolor`
by the same first-match rule.

### Breath bar

`[orig: HUD_DrawBreathBar @0x59D6F0..0x59D9C9]`, whose only caller is
`HUD_DrawGameplayOverlays @0x5BDED3`, skipped while `g_spawn_success_gate` is
set (`@0x5BDECA..0x5BDED1`). Port: `HudFrameCompiler::element_breath_bar` over
`emit_progress_bar` (`engine/runtime/hud/hud_frame.cpp`); ctest
`hud_frame_compiler` (`test_compiler_breath_bar`).

- **Anchor.** The hudpos token is `BREATHTIME x y align`, three fields (JO
  authors `512,70,center`): atof, atof, then `HUD_ParseTextAlignment` on the
  third token (right = 1, center = 2, else 0)
  `[orig: HUD_ParseHudposToken @0x59FB3B..0x59FB84]`; `DefHudPosDef::breath_time[3]`
  through `defscan::parse_pos_align3`. An absent token leaves (0, 0, left),
  as retail has no presence gate.
- **Gate and counter.** Declutter slot 2 visible (`@0x59D6F3`) and a positive
  `breathtime` (`@0x59D70F`); the count is the S2C 0x0A breath sample word
  (`word_A85B7C`, written only by `NapiNPClientMsg_0x00A @0x430104`), and a zero
  count draws nothing (`@0x59D742`). The arm forcing the count to 1 under
  `dword_24C1930 & 0x8000000` is dead: nothing sets that bit.
- **Math and colour.** limit = 4 * breathtime (four samples a second); red
  (0xFFFF0000) over the last 40 samples (10 s), else green (0xFF00FF00). The bar
  is skipped once the integer `100 - 100 * count / limit` is not positive
  (`@0x59D763`); the label still draws. The fill fraction is
  `1.0 - count / limit` in double (`@0x59D80A` / `@0x59D8C7` / `@0x59D984`).
- **Geometry.** 200 x 10 design px from the anchor, left (align 0), right (1)
  or centred (2), each corner scaled on its own through
  `Viewport_ScaleToVirtualCoords` (`@0x59D794..0x59D817`, `@0x59D90E..0x59D991`,
  `@0x59D851..0x59D8D4`). `draw_progress_bar @0x59B340` draws three untextured
  quads in one 12-vertex draw (`@0x59B5EB`): the border in the colour, the inner
  rect in opaque black (`@0x59B4DB`), then the fill, centred:
  mid ± (xr - xl - 4) * fraction * 0.5 about the integer mid
  (`@0x59B520..0x59B56B`).
- **Label.** `Overlays/STROVER91` at design (x, y + 15) in the BOLD label slot
  (`@0x59D8F7`), aligned like the bar and drawn half-bright,
  `(colour >> 1) & 0x7F7F7F | 0xFF000000` (the left, right-aligned and centred
  scaled text helpers, called `@0x59D83F` / `@0x59D9B9` / `@0x59D8FC`, all draw
  through `HUD_DrawTextLeft_HalfBright @0x5804C0`).

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
It is integer arithmetic, signed and truncating toward zero:
x = (x · 1024 + w/2) / w (`shl` `@0x5D2C7E`, `sar` `@0x5D2C83`, `idiv`
`@0x5D2C88`), y = (y · 768 + h/2) / h (`@0x5D2C95..0x5D2CA2`), ahead of every
projected cue (`@0x592852`, the bracket anchor `@0x592D32`, the commander
`@0x59E77E`). The commander clamp is integer too: dx/dy `@0x59E78B..0x59E791`,
the distance `ftol` `@0x59E7C9`, `334 · d / dist` by `idiv`
`@0x59E7D6..0x59E80F`, and the connecting line ends at the integer screen
centre (w/2, h/2) `@0x59E855..0x59E86F`, not the rounded design (512, 384).
Ported 2026-09-22 (`hud_math::screen_to_design_x/_y`; `hud_combat`
`integer_screen_mapping`).

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
  (currentHealth = `entity+286`): `> 0xC000` (0.75) → `g_stanceColorGood @0x2723ADC` (good;
  named 2026-08-21, ex `dword_2723ADC`); `> 28671` (`0x6FFF` ≈ 0.437) →
  `g_stanceColorMiddle @0x2723AE0` (mid); else `g_stanceColorBad @0x2723AE4` (bad).
  (Note D-HUD-4: the *fill width* uses the capped `+92` ratio while the *color*
  uses an uncapped recompute — equivalent in range, recorded for fidelity.)

### Stance indicator — `HUD_DrawStanceIndicator @0x599f10` (IDB name `draw_minimap_compass_overlay` is a misnomer; D-HUD-1)

Renders the **stance** icon, keyed by `byte_27235C0` = `dword_2723388 + 568` =
the stance index from `HUD_BuildEntityInfo`. It is **not** a compass.

- Frames defined by `hudpos.def` token **`HUDSTANCE <idx> <xoff> <yoff> <texname>`**
  `[orig: HUD_ParseHudposToken @0x5a0b4d]`: `xoff→dword_2723B24[idx]`, `yoff→dword_2723B44[idx]`,
  `texname→byte_2723B8C + idx*0x13` (19-byte stride). Texture handles loaded into
  `dword_27239E4[idx*4]`, dims `dword_27239E8[]/EC[]`. Retail JO defines six
  (0..5: stand/crouch/prone/sitting/emplaced/parachute).
- **Gates**: the whole element skips unless the ALPHAFADE ramp is nonzero AND
  all six stance-frame handles (0..5) are loaded `[orig: @0x599f18..0x599f50]`
  — ported 2026-07-11 (`game_hud._draw_stance` early-outs).
- Anchor = `HUDSTANCEPOS → dword_2723AEC (x) / dword_2723AF0 (y)`
  `[orig: HUD_ParseHudposToken @0x5a0be7]`; each frame draws at **anchor + its own
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
  is what `engine/runtime/hud/game_font` implements.)
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
  `[orig: @0x592bd5]`; default white witnessed at Config_SetDefaults @0x54D461; D-HUD-8 corrected below.
- **Visibility**: the cluster gates on `dword_2723CB4` and the weapon-def ptr;
  the spread crosshair draws when the player **cannot** take an aimed shot —
  `!Player_CanFireWeapon() || equipped Inset || (dword_A8235C && promoted
  Sighted)` `[orig: @0x592adc..0x592b01]`. Rechecked 2026-09-19:
  `Player_IsVehicleHasAutoAim @0x4dccb0` actually reads equipped `flags2 & 0x200`,
  and `Player_IsVehicleGunnerScoped @0x4dcd30` is the promoted Sighted selector;
  neither name implies that a vehicle seat is required.
  `Player_CanFireWeapon @0x5cf780`
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
  `g_cameraFovTargetQ16 = 5242880` = **80.0 deg** 16.16
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
  still draws the assembled reticle at offset 0 `[orig: fldz @0x592bcc]`; the
  config default is ON `[orig: Config_SetDefaults @0x54d472]` and the options
  checkbox seeds from the global `[orig: options_screen_init @0x554d15]` — the
  port carries the toggle as `HudLayout::crosshair_spread_enabled` off the
  XHAIR_SPREAD option):
  `row = stance + 3*Player_CanFireWeapon()` where stance = 0 prone (`&0x100`)
  / 1 crouch (`&0x200`) / 2 stand, forced 2 when swimming/under water
  (`entity+36 & 0x108020` or below `Env_WaterHeightFixed`), forced 1 when
  mounted `[orig: @0x592b35..0x592b87]`. Because the draw gate and the row
  select share the CanFire predicate, the ordinary un-aimed crosshair reads
  hip rows 0..2. Inset and the Sighted hit-feedback exception can draw aimed
  rows 3..5 **on foot as well as mounted**. The earlier vehicle-only gloss
  relied on misleading function names (corrected 2026-09-19). Then
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
- **Targeting sub-elements** (witnessed 2026-07-11; ported 2026-09-19):
  - the **target-tracking cursor** — with a tracked entity (`ptr @0x27234F0`)
    and the cursor art loaded, a quad draws at the projected
    `Entity_ComputeWeaponFireOrigin` of the target, color/texture switching on
    same-team (`dword_2723900` vs `dword_27238F0` records) with an MP team
    gate; bracket admission uses mpattrib bits, not a tick-blink `[orig: @0x592790..0x592875]`;
  - the **aim-point quad for flagged weapons** — `weapondef+12 & 0x80` swaps
    the spread reticle for the weapon's own crosshair record (`weapon+376`)
    drawn at a raycast-projected aim point
    (`Entity_ComputeUserpointTransform` → `physics_raycast_entity_pools…` →
    project) `[orig: @0x592973..0x592ac8]`;
  - the **friendly brackets** — four clipped 2D lines around the main aim
    anchor when the target item-definition type reads 3, team- and mpattrib-gated
    `[orig: @0x592ce2..0x592dd7]`.
  - a mode flag `dword_24C1930 & 0x10000` replaces triggered/gametext strings
    with the literal `"&"` `[orig: @0x51f1c8 / @0x51ebe3]` — writer
    unwitnessed; not modeled.
- **Region geometry** (`HUD_DrawCrosshairCornerQuad @0x590f50`): each region's
  quad rect is the texture's size centered on the (offset) point, corners
  scaled to screen; the arms are 5-vertex triangle strips and the center a
  4-vertex strip, with the **inner vertices at the quad midpoint pulled back by
  0.1 × half-extent**, and **literal UVs at the witnessed
  0.45 / 0.5 / 0.55 atlas bands**; integer corner/midpoint rounding can make
  geometry-derived UVs differ (center band 0.45..0.55, arms the outer bands).
  Vertex format (corrected 2026-09-19): `z = 0.9`, `rhw = 1.0`,
  `diffuse = color`, `specular = 0`; FVF `0x2C4` via
  `[orig: GDynamicVB_DrawPrimitive @0x6788e0; SetFVF @0x678962/@0x678A3E]`.

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
Scoped calls it at `0x5cab01/0x5cab08` and then **always**
`Hud_DrawScopeCircleMask @0x5d17a0` at `0x5cab15`. When both selectors are
zero, the first-person viewmodel path remains available
(`0x5ca32c..0x5ca343`, consumed at `0x5ca822..0x5ca829`).

**Corrected 2026-09-16 (the circle mask is not a fallback).** This record
previously read the mask as a rowless-weapon fallback that "a nonzero count
suppresses". The byte sequence refutes it: `Hud_DrawScopeCircleMask` is called
unconditionally on the Scoped arm, and the SIGHTS row count only forms its
single argument — `v11 = draw_weapon_sight_overlays(...);
Hud_DrawScopeCircleMask(!v11, ...)` `@0x5cab08..0x5cab15`, and the same shape
in the second caller `render_hud_overlay @0x5d82e5..0x5d82f2`
(`xor ecx,ecx; cmp eax,ebx; setz cl; push ecx`). Inside the drawer that
argument reaches ONLY the tail `if (draw_crosshair)
draw_minimap_crosshair_and_grid(...)` `@0x5d1cc9..0x5d1cf4`, after the ring has
already been submitted `@0x5d1cc4`. So every scoped frame gets the annulus, and
an authored SIGHTS row suppresses only the inner reticle cross and grid ticks.
The consequence of the old reading — a scoped weapon with rows drawing with no
circular mask at all — was the visible symptom in OpenNova before the port. The
full geometry is the new section below.

`draw_weapon_sight_overlays` itself reads only the authored count
(`WeaponDef+0x258`) and rows (`WeaponDef+0x1c8`, stride `0x24`). Those rows are
card **content**, including draw order/blend/scale, not selection policy. A
nonzero count is reported even if a row's texture handle is missing, so a
weapon whose card art fails to load still suppresses the cross and grid.
REVX02's `WPN_M4AUTO_EOTECH` confirms the Sighted path: it is Sighted, not
Scoped, has no NoCardSwitch, and authors `M4ET_SGT.TGA` plus additive/scaled
`et_rtcle.tga`; both textures exist in the retail resource root.

`WeaponDef_CreateBlendNamedMaterial @0x540180` recognizes six tokens. The port
keeps their transport values stable as `blend=0`, `add=1`, `blendat=2`,
`multiply=3`, `addat=4`, and `multiplyat=5`. The decoder at `0x680f00` maps
`multiply[at]` to D3D `SRCBLEND=DESTCOLOR`, `DESTBLEND=SRCCOLOR`, producing
`2 * source * framebuffer`; this makes an authored 128-gray texel neutral.
Each `at` spelling also carries flag `0x40000` and uses the global alpha-test
reference 128 (`CGfxDevice_SetAlphaTestRef @0x5ccdae`, comparison GREATER).
The Godot card reproduces those modes per row; in particular, transparent
pixels in `multiplyat` textures are discarded instead of blacking out the
scene.

Each row is drawn in one of three modes (2026-09-10, jo-c cross-check):
**scaled** (row+0x1C, the `scale` token) draws half-extents
`((x2-x1)*(idx+2))>>3` / `((y2-y1)*(idx+2))>>3` about the row centre
`((x1+x2)>>1, (y1+y2)>>1)` `[orig: @0x4dce8d gate; @0x4dcead..0x4dcf2c]`, where
`idx = dword_B76780` is the per-player sight-scale index — default 1 at
`Player_InitPlayer @0x4e160f/@0x4e178c` (so 3/4 of the authored box), cycled
by action 216 = catalog row 38 `dotsize` ("Sights Dot Size", default O):
`idx + 1`, signed `cmp eax,3; jl` keeps it else 0 `[orig:
Input_HandleActionBinding_0 @0x4e0c31..0x4e0c46]`; **slide** (row+0x20, the
`slide` token, row+0x18 = slide frames): `y1 += frames*m`, `y2 += frames*m`, x
untouched `[orig: @0x4dcf42 gate; @0x4dd014/@0x4dd043]`, where the multiplier
keys on the MountSlot zero word +0x60 `[orig: @0x4dcf4c]`: word 0 →
`Weapon_GetScopeZoomLevel(0, (def+0xA0<<16)/1638400) * 25 / def+0x9C` when
def+0xA0 != 0 and `byte_24D217C == 0` (`@0x4dcf57..0x4dcfa6`); word 0xFFFF →
`dword_B76808 / (def+0x9C<<16)`, negative → 0, capped at def+0x84
(`@0x4dcfb7..0x4dcff7`); else the signed word itself (`@0x4dcff9`); **plain**
otherwise (`@0x4dd050`). def+0x84/+0x9C/+0xA0 are the three `atol`'d tokens of
the weapon.def key `scope_max_zero <maxSteps> <stepMetres> <defaultMetres>`
`[orig: WeaponDefs_ParseLineCallback @0x544e8b..0x544ed9]`; `dword_B76808` is
the aim ray's hit distance (Q16) restamped by `Entity_UpdateInfantryPlayerBody
@0x4b5056..0x4b50ad` and seeded with def+0xA0 on mounting a Flags 0x20000000
weapon (`Player_MountWeaponSlot @0x4dfb3b..0x4dfb44`); `byte_24D217C` has no
writer in the image. Retail honours `scale`/`slide` only as token 7 with >= 8
tokens (`@0x544b7a`); our parser scans any trailing token, a superset. After
`Viewport_ScaleToVirtualCoords` the drawer also applies an aspect y-correction
`y' = (y - H/2) * 3/(4*flt_8409EC) + H/2` (`@0x4dd0cd..0x4dd0f7`, the factor
from `Render_SetAspectRatioMode @ 0x58d870`; mode 0 = 0.75 = identity).
The port uses native aspect, `flt_8409EC = H/W` [orig: Render_SetAspectRatioMode
@ 0x58d870, native store @ 0x58d8c9..0x58d8d9]. `sight_rect_to_viewport`
applies per-corner pixel rounding first, then this correction about H/2;
`HudFrameCompiler` and the thin `HudPos::sight_scale_rect` binding share it.
Ctest `hud_frame_compiler` pins plain and sliding rows at 1920x1080; GUT
`hud_overlay_test` pins a square reticle at both 4:3 and 16:9. Explicit aspect-mode
selection remains absent; the native viewport path is ported. In the NVG composite's
Sighted arm (2026-09-24) the rows lay out over the 512-square NVG target with the
frame's selected ratio in the Y correction (`hud::sight_rect_to_viewport_at_ratio`,
`HudPos.nvg_scene_sight_rect`; `NVG_RenderSceneToTarget @0x5d08d8..0x5d0927`) and draw
into the NVG scene, never over the frame (the circle-mask section below).

Port: `world::weapon_sights_card_eligible` owns the dynamic selector, the sim
publishes it as `scope_card_active`, `engine/runtime/hud/sight_overlay.h`
evaluates the three row modes (`sight_row_rect`, `sight_slide_multiplier`, the
index policy `next_sight_scale_index`), `HudFrameCompiler::element_sights_card`
+ `godot/game/world/hud_sights_card.gd` materialize the evaluated rows and use
the selector only for visibility, and `game_hud_presenter.gd` polls the
`dotsize` binding (`HudOverlay::cycle_sight_scale`). The slide multiplier's
rangefinder (0xFFFF), manual-word and default arms consume the live slot
zero and aim range through `PlayerLocalView`; the `ScopeZeroInc/Dec` rows
41/42 are ported. `WeaponDef::get_sight_slide_multiplier` remains the
standalone definition/default helper. The main-view camera consumer and
HUD readouts are recorded under D-HUD-27 below. Shipped data: the sole `slide` author,
`WPN_M16M203HE`, authors `scope_max_zero 10 50 0 0`, so its default arm resolves
to multiplier 0 (`Weapon_GetScopeZoomLevel(0, d)` returns `d` untouched
`@0x422fd1..0x422fd5`); the `10 100 200 x` weapons resolve to 2 (a 33-frame row
shifts 66 px), `1 100 100 1` and `1 300 300` to 1, `10 100 300 1` to 3.

### Scoped-view circle mask — `Hud_DrawScopeCircleMask @0x5d17a0` (witnessed + ported 2026-09-16)

The near-black annulus every Scoped frame draws over the scoped view, and the
reticle cross + cardinal grid it chains through
`draw_minimap_crosshair_and_grid @0x5d1160` when the SIGHTS card drew no
authored row. Both callers (`Render_ProcessMainSceneFrame @0x5cab15`,
`render_hud_overlay @0x5d82f2`) pass `draw_crosshair = (row count == 0)`; see
the correction in the SIGHTS-card section above for why this is not a fallback.

**Frame.** All of it derives from the viewport rect
`dword_24C1428..0x24C1434` (x0/y0/x1/y1) and the full surface width
`overlayCtx @0x24C1420`. The rect is retail's INCLUSIVE overlay rect
(0, 0)..(W - 1, H - 1) (`Viewport_SetFullScreen @0x5d30e0`; corrected
2026-09-24, the port had passed (0, 0)..(W, H)):

| Term | Value | Witness |
|---|---|---|
| centre | `((x1 + x0) >> 1, (y0 + y1) >> 1)` — arithmetic shifts of the summed edges | `@0x5d17cc` / `@0x5d17e1` |
| `scaleX` | `(double)cx / (double)cy * 0.75` | `@0x5d17f5` |
| `scaleY` | `3.0 / (flt_8409EC * 4.0)` — `Render_GetTargetAspectRatio @0x58a920` returns the selected H/W ratio `Render_SetAspectRatioMode @0x58d870` stores (0.75 / 0.6 / 0.5625 / 0.625, or the viewport's own H/W in the native mode) | `@0x5d1811` |
| ring size | `((y1 - y0) >> 3) + ((y1 - y0) >> 1)` = five eighths of the viewport height, as an INTEGER shift pair | `@0x5d1830` |
| inner radius | `0.71f * ring_size` | `@0x5d184d` |
| outer radius | `ring_size * 1.5f` | `@0x5d1857` |
| cross/tick unit `t` | `(W + W) * flt_7D00A8` = `W / 320` | `@0x5d12ad..0x5d12b5` |
| tick pitch | `(W * flt_7C44B4) * flt_7D00A8` = `W / 64` (`flt_7C44B4 = 10.0f`) | `@0x5d160a..0x5d1635` |

At the NATIVE ratio `scaleY` is 1 and `scaleX` is the rect's centre ratio x
0.75, a hair over 1 (the ring is a circle to within a pixel); a forced
4:3 / 16:10 / 16:9 / 5:4 ratio pins `scaleY` and widens `scaleX` with the
surface, so the ring becomes an ellipse that keeps the same fraction of the
width and height it covered at the authored ratio. A 1024x768 screen gives
centre (511, 383), ring size (767 >> 3) + (767 >> 1) = 478, inner radius
339.38 px and outer 717 px (past the corner distance, so the annulus really does
mask the whole surface outside the circle), and `scaleX = 511 / 383 x 0.75 =
1.00065`. The port's native-ratio proxy is (y1 - y0 + 1) / (x1 - x0 + 1).

**The ring.** 64 quad segments over 65 angle stops (the 65th closes the loop),
two vertices per stop, submitted as ONE 130-vertex `D3DPT_TRIANGLESTRIP`
through the dynamic VB (`GDynamicVB_DrawPrimitive(5, &unk_2BE1088, 0x82)`
`@0x5d1cc4`). Segment `s` reads BAM table index `16 * s`:
`g_bam_sin_table_q22 @0x31bfbc0` holds 1024 Q22 entries per revolution and
`off_849934 = &g_bam_sin_table_q22[256]` is its cosine view, both read as
`table[i] * 2^-22` (`@0x5d18cd` / `@0x5d18e7`). Each stop emits
`x = cos*r*scaleX + cx`, `y = cy - sin*r*scaleY` (`@0x5d18f6` / `@0x5d190d`),
inner first at `0xFF181820` (`@0x5d18cf`) then outer at `0xFF040408`
(`@0x5d1936`) — an opaque near-black annulus with a slight inward lift.

**The reticle cross.** Four tapered spokes (left, up, right, down in submit
order), each a 7-vertex / 18-index block drawn as a `D3DPT_TRIANGLELIST`
(`GDynamicVB_DrawIndexedPrimitive(4, &flt_2BE0F68, 7, indices, 0x12)`
`@0x5d1435` / `@0x5d14bf` / `@0x5d153b` / `@0x5d15b7`). With
`A = scaleX * ring_size` and `B = scaleY * ring_size`, the spoke runs from the
exact viewport centre out to `0.71` of the radii and breaks at `0.4`
(`flt_7C56A0 = 0.4f` `@0x5d11d5`, `flt_7DC624 = 0.71f` `@0x5d1261`); every
endpoint passes through `_ftol2_sse` and comes straight back in through `fild`
(`@0x5d121f..0x5d1298`), so the break points are integer pixels — at 1024x768
(A = 1.00065 x 478 = 478.31) the left spoke breaks at
`trunc(511 - 0.4*A) = 319` and ends at `trunc(511 - 0.71*A) = 171`. The half-thickness `t` spreads across the spoke axis. Colours:
the shared centre vertex `0x20000000` (`@0x5d132c`), the two on-axis break
vertices opaque black `0xFF000000` (`@0x5d1344` / `@0x5d1364`), the four
off-axis vertices fully transparent (`@0x5d1338` and friends). Index block
`{0,1,2, 0,2,3, 2,1,4, 2,4,5, 3,2,5, 3,5,6}` (`@0x5d11bf..0x5d1218`).

**The cardinal grid.** Four ticks in each of four directions
(switch order `@0x5d167c`: 0 = +X, 1 = -X, 2 = +Y, 3 = -Y), each a 5-vertex /
12-index diamond of half-size `t` at `i * (W/64)` from the centre, `i = 1..4`
(`@0x5d1653..0x5d171d`). The offsets are plain screen pixels — neither
`scaleX` nor `scaleY` touches them. Centre vertex `0xFF000000` (`@0x5d162b`),
the four points `0x10000000` (`@0x5d1639`). Index block
`{0,1,2, 0,2,3, 0,3,4, 0,4,1}` (`@0x5d15c7..0x5d1626`).

**Device shape (not ported as such).** Both batches ride the XYZRHW + DIFFUSE +
SPECULAR + TEX2 dynamic-VB vertex (stride 40) with `z = 0.5`, `rhw = 1`, the
specular dword left stale and — for the cross and grid — no texcoords written
at all, which is what pins effect pass `0x700000` as the untextured
alpha-blended overlay pass. The ring takes device render-state slot 1
(`@0x5d185b`), the cross/grid slot 2 (`@0x5d139e`); the ring's own texcoords
(`u = s/64` on both sets, `v = 0` inner / `1` outer) are therefore vestigial.
OpenNova submits the same vertex colours as flat vertex-coloured triangles.

**The feature switch.** `dword_843480` is a shipped constant `1` with three
readers and no writer: this drawer (`@0x5d17fe`), `sub_5CF3F0 @0x5cf3f0` (which
just returns it) and `sub_5D27F0 @0x5d27f0`. It selects the modern scope
treatment; the zero arm (`sub_5D27F0` re-rendering the terrain scene through
the weapon's elevation offsets, and the `!sub_5CF3F0()` branch `@0x5ca74c`) is
unreachable in the shipped image. Not ported, and nothing should gate on it.

**Port.** `engine/runtime/hud/scope_circle_mask.{h,cpp}` builds the three
vertex-coloured triangle batches plus the frame terms from the viewport rect,
the surface width, the aspect mode and `draw_crosshair`; it also owns the
scene-frame overlay fork (`scoped_view_overlay` — binoculars, then Sighted,
then Scoped) and the two selector bytes' weapon.def halves
(`scoped_selector_from_def` = Scoped without Inset, `sighted_selector_from_def`
= the Sighted bit). The typed statics `HudPos.scope_mask_points` /
`_colors` / `_indices` (one batch per call, over a memoised build) plus
`HudPos.scope_mask_frame` and `HudPos.scoped_view_overlay` bind it,
`PlayerHudWeaponDef` carries the two def halves, and
`godot/game/world/hud_scope_circle_mask.gd` rasterises the batches with
`RenderingServer.canvas_item_add_triangle_array` as a child of the HUD overlay
directly after the SIGHTS card, so the card draws first and the mask covers its
corners — retail's submit order. `game_hud_presenter.gd` feeds the fork next to
the card switch with `draw_crosshair = the AUTHORED row array is empty` (a row
whose texture fails to load still counts, like retail's). ctest
`sight_overlay` pins the fork, the frame terms, the ring stops and colours, the
truncated spoke endpoints and the tick lattice.

**Under the NVG composite (2026-09-24, the rendering parity pass).** Under the
NVG composite's Scoped arm the circle mask does not draw: the lens draws its
own ring, then the SIGHTS card on top, then (no authored row)
`draw_minimap_crosshair_and_grid(ring, cx, cy, 1.0, 1.0)`, the cross and grid
at unit scale (`NVG_DrawScopedLens @0x5d2798..0x5d27bc`;
`hud::build_nvg_lens_reticle`, `HudPos.scope_mask_*` `nvg_lens`). No NVG mask
draws under the lens or on the death screen (`NVG_Composite
@0x5d1077..0x5d1080`); `LocalPlayerViewFrame::nvg_mask_visible` carries that
gate. Under the Sighted arm the card draws into the NVG scene (tinted, glowing)
and never over the frame (`nvg_sights_in_scene`). The arms themselves are
recorded in [render-order-re.md](../render/render-order-re.md) (the FrameFX
screen effects).

Residual: the third Sighted term (`MountSlot.currentAction != SWITCHFROM`
`@0x4dcd30`) is not a def field, so the shell feeds the fork the def bit alone.
It can only matter for a weapon.def that sets BOTH the Scoped and Sighted bits,
and none ships; the exact term is already computed in
`world::weapon_sights_card_eligible` and wants exporting on
`LocalPlayerViewFrame` when that file is next touched.

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
`Chat_AddMessageChannel2(text, -1, 930)` `[orig: @0x51f216]` — the `-1` color is
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

Only the BMS triggered text and the console lines take this SYSTEM ring. The
WAC `text`/`ptext`/`text#` lines, the WAC lose line and the BMS subgoal won/lost
lines post into the CHAT ring instead, raw white with the 930-tick timer
(`[orig: Chat_AddSystemMessage @0x4EDB50]` and
`[orig: GameMsg_AddChatLineAndRelay @0x5BA170]` → `[orig: Chat_AddMessageChannel1
@0x4985D0]`; the KYLE.WAC header documents "text# ... chat - right side" and
"consol ... consol - left side"); `text#`/`consol#` carry the handler's "%s %i"
line (`[orig: Chat_AddFormattedIntMessage @0x4EDB70]`, the sprintf `@0x4EDB9E`;
`[orig: WacCmd_ConsolNumber @0x4EDC00]`, the sprintf `@0x4EDC2E`). The presenter
routes them that way since 2026-09-23 (`game_hud_presenter.gd`: the `text`
literals, `lose` and `subgoal_won/lost` → `HudOverlay.push_chat_line(line, -1)`;
the triggered-text id and `debug_text` → the SYSTEM ring). The shipped MP
scripts use `consol`/`Consol#` for live objective lines and a countdown, which
reach the screen through the SYSTEM ring.

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

**Selection** (the ported half — `VehicleSystem::collect_attach_labels`,
`engine/runtime/world/vehicle_attach.cpp`):

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
  the full `Entity::eye_offset_{x,y,z}` triple each clip advance — the LOCAL
  player takes the exact shell-fed posed-head-minus-Position triple (head z
  terrain-floored on foot; no `0x2000` floor on the exact legs `[orig: on-foot
  @0x4b6bb3..0x4b6cc8, mounted @0x4b6908..0x4b696c]`, 2026-08-19), NPCs take
  the org1 capsule z plus the witnessed lateral pair (`x = +lat·sin(yaw)`,
  `y = −lat·cos(yaw)`, `lat = (delta·sinQ22(lean)·3) >> 2` `[orig: stores
  @0x4bf141/0x4bf149/0x4bf14c]`) — and the gather feeds z per tag; the
  presenter lifts eye + 0.25 u. The remaining residue is the sample-less
  player legs' full 3-angle lateral tilt (`[orig: on-foot @0x4b69ab..0x4b6b7c,
  mounted @0x4b66fc..0x4b68e5]`; the z lane is ported with its witnessed
  `0x2000` floor `[orig: @0x4b6b98]`).
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
  earlier H mappings are refuted by the catalog walk). Unequal team =
  `0xFF00FF`, drawn only under `g_enemyTagsVisible @0x24D1DF4`; outside the
  death screen the pass gate has already dropped real enemies, so magenta can
  only reach team 0. Its writers: S2C 0x0A (set on the rising edge
  `@0x42ffb2`, cleared on the falling edge `@0x430025`),
  `NapiNPClientMsg_SetSpectatorMode @0x425a3a` (set only), the mission-start
  clear unless the death screen is up (`@0x526388`), and the action-130
  toggle (`Input_HandleActionBinding @0x49bbce..0x49bbd9`, no catalog row). Death screen: team 1
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
  `Chat_AddMessageChannel2` `[orig: @0x49b573..0x49bc60]`): 1 = text under 300 m
  (`0x12C0000`), 2 = text always, 3 = three 1-px vertical tick lines at
  `x−1/x/x+1` spanning ±fontH/4 `[orig: @0x5a40eb..0x5a4160]`; an empty slot
  callsign draws a single fontH bar `[orig: @0x5a4398]`.
- **Text**: CENTERED on the projected x (`CGameFont_DrawText` flags bit 1),
  top at `y − fontH/2`, half-bright with the caller's alpha PRESERVED
  (`HUD_DrawTextHalfBrightF @0x580720` —
  `(c & 0xFF000000) + ((c>>1) & 0x7F7F7F)`). **The revive count** (settled
  2026-08-24 by disassembly): under the gate `dead && slot && slot+0x10 != 0`
  the TEXT forms (modes 1/2) append it to the name — `sprintf("%s: %ld")`
  `[orig: @0x5a400e]`, else the plain `"%s"` `[orig: @0x5a422e]` — while the
  BRIEF ticks `[orig: @0x5a41c0..0x5a41df]` and the empty-callsign bar
  `[orig: @0x5a4407..0x5a441b]` draw the BARE count as a separate centered
  string one fontH ABOVE the point: `sprintf("%ld")` (`aLd_reviveCount
  @0x7c3818`, which the IDB had mis-typed as an offset) `[orig: @0x5a41f0 /
  @0x5a4428]` → `HUD_DrawTextHalfBrightF(x, y − fontH)` `[orig: @0x5a4453]`.
  The earlier `@0x5a4212` cite was the mode-3 jump into that shared draw.
- **Downed recolor (the bad tier)** `[orig: @0x5a3dc9..0x5a3e85]`: with the
  dead latch (`Flags & 2` `@0x5a3c1c..0x5a3c27`) a slot-owned entity inside
  its revive window (`slot+0x10 != 0`) draws `g_hudColorTable[3]` light blue
  (`0xFF80A0FF`), pulsing toward white while the medic-request latch
  `slot+0x2C` stands — `t = (g_hudFrameCounter − 8) & 0x3F; if (t > 0x20)
  t = 0x3F − t; c += ((255 − c) × t) >> 5` per RGB channel
  `[orig: @0x5a3dfb..0x5a3e6d]` — and `g_hudColorTable[8]` gray
  (`0xFFA0A0A0`, `HUD_InitTeamColorTable @0x51f295`) without a slot or window;
  an alive bad-tier entity keeps `tagcolor_bad`. The slot bytes are fed by
  S2C 0x54 / 0x46 bit 0x0008 (`PlayerSlot_SetDownedState @0x4348d0`) and
  counted down CLIENT-side at 1 Hz: `Client_ProcessNetworkFrame
  @0x42c27e..0x42c2da` bumps `g_slotRefreshTimer @0xa85b80` per frame and,
  past 62, decrements every active slot's nonzero window (entity required),
  re-storing the request latch, then zeroes the timer. Ported: the walk
  (`world::collect_friendly_tags` slot walk / `replication::collect_roster_tags`),
  the countdown (`ClientRuntime::tick_roster_revive_countdown`), the recolor
  + pulse (`friendly_tag_revive_pulse`), and both count forms.
- **The walks' gates** `[orig: HUD_DrawFriendlyTagsPass @0x5a44b0..0x5a4597]`:
  BOTH walks require `g_GameType != 0 || g_death_screen_active`
  (`@0x5a44e8`/`@0x5a456d`) after the team gate (`team == 0 || team == local
  || death screen`); the drawer's entry bails are `entity == playerEntity`,
  `Flags & 1` (CARRIED — a DEAD entity is still labelled) and a NULL itemDef
  `[orig: @0x5a39df..0x5a39fb]`. The drawer then takes its death-screen arm,
  which draws every team (`@0x5a3c33..0x5a3c3a`), or compares the entity Team
  with `g_local_player_entity`'s (`@0x5a3c6b..0x5a3c7d`): an unequal team,
  neutral 0 included, draws magenta only under `g_enemyTagsVisible` and
  otherwise bails (`@0x5a3c7f..0x5a3c95`). In ordinary play the net gate is
  therefore `team == local && g_GameType`; a team-0 local player still labels
  team-0 neutrals (corrected 2026-09-23: applying only the pass gate labelled
  03TR's neutral birds and civilians as allies). Native `friendly_tags`,
  `client_roster_tags` and `training_gameplay` pin it.
- **Medic plate**: `CharAttr[playerClass].flags & 0x8` (charattr.def
  `ATTRIBUTES` — the flag table `@0x813F18`: AutoScope 1, SpreadBonus 2,
  KnifeBonus 4, **Medic 8**, WaterGirl 0x20; loader `CharAttr_LoadFromDef
  @0x412140`) → a fontH/2 square at `(x − textW/2 − fontH, text top)`:
  `HUD_DrawMedicCrossQuad @0x59bcb0` builds a WHITE quad + two RED bars inset
  by an eighth — literally a red cross on white — at the tag alpha
  `[orig: @0x5a4309..0x5a436c]`. The same primitive (12 verts / 18 idx / 3
  quads; white corners `unk_FFFFFF + alpha<<24 @0x59be64`, red bars `unk_FF0000
  + alpha<<24 @0x59be76`, the 0.125 insets `@0x59bd1c/@0x59bd7e`) has two
  more callers: the MAP medic marker for a teammate whose AnimMap slot 8 is
  active (`draw_entity_labels_and_markers @0x5a49e0`, the call `@0x5a4d40`)
  and the help-screen icons (`HUD_DrawHelpScreenIcons @0x497480`, the call
  `@0x497620`). Ported once as `engine/runtime/hud/hud_medic_cross.h`
  (`medic_cross_quads`), which the plate and the map marker share — the map
  marker itself landed 2026-08-21 (`HudMinimapMarker::medic`, the charattr
  Medic bit replacing the teammate blip in `hud_minimap.cpp`; the feed is
  `MissionTables::class_has_attribute` over `MissionTables::class_attribute_flags` — the
  parsed charattr.def ATTRIBUTES, the 31-dword class records with the flags
  word at +28; the final review found that field had NO loader, so the
  ATTRIBUTES reader (`CharAttr_LoadFromDef @0x412140`) is ported in this PR
  (R2) `[orig: CharAttr_LoadFromDef
  @0x412140; the map test AnimMap_IsSlotActive(playerClass, 8) @0x5a4ab3,
  local team only @0x5a4ac6, enemies forced off @0x5a4acf, the blip replaced
  @0x5a4cd6..0x5a4d48 at (px−3.5, py−3.5)..(px+4.5, py+4.5) map px]`) —
  it is NOT a map "target bracket" (the 2026-08-21 misattribution, D-HUD-21).
- **Radio-request icon** (ported 2026-09-12; the ex-"wounded icon"):
  `entity+885 && !Entity_FindChildByDefType(e,1,1)` with the viewer gate (local
  mount kind ∈ {2,5} or local +885) → the rotated icon quad
  `HUD_DrawRotatedIconQuad @0x599630` (TSDicon cell 0x17, table[3] light blue,
  forced full alpha, half-size fontH·0.5 centered on (x, y)). The gate builds
  `@0x5a3bba..0x5a3c1a` (`[eax+168h]` = 2/5 `@0x5a3bcb..0x5a3bd9`, own +885
  `@0x5a3bdf`; the tag's +885 `@0x5a3bfe`, the child walk `@0x5a3c0e`). The
  icon draws after the text/tick/bar and before the medic plate: the text form
  shifts x by −(fontH/2 + textW/2) and the plate then reads that shifted x
  (`@0x5a4288..0x5a42ee`, plate read of `axis` `@0x5a430e`); the tick/bar
  forms pre-shift a medic tag by −fontH/2, center the icon at
  (x − fontH/2, y − fontH/2) and shift +fontH before the bare count
  (`@0x5a4165..0x5a41bc` / `@0x5a43a0..0x5a4403`). The +885 writer is the S2C
  0x6D radio-call latch (`NapiNPClientMsg_HandleEntityDeath @0x430C50`, event
  6 → 1; the audio record). Port: `HudFriendlyTag::radio_request` (the fold)
  + `HudFrameState::radio_request_icon_viewer` (the viewer gate) →
  `HudFrameCompiler::element_friendly_tags`, the strip cell through the shared
  `hud_icon_strip_cell_uv` / MODULATE2X fold of the map blips. The two feeds
  are ported: `world::FriendlyTagSource::radio_request` (the world builder's
  fold, `friendly_tag_aboard_vehicle` = the 19-link groundEntity walk) and the
  joiner's `replication::collect_roster_tags` fold over the materialized
  twins; the viewer word is `world::friendly_tag_radio_request_viewer` over
  the local player (`Simulation::local_player_radio_request_icon_viewer` ->
  `HudOverlay::set_radio_request_icon_viewer`, stamped by the HUD presenter
  beside the friendly-tag fill).

**Port** (D-HUD-20): gather `world::collect_friendly_tags`
(engine/runtime/world/friendly_tags.cpp; a joiner's roster walk joins it in
`inmatch::collect_friendly_tags`) → `Simulation::fill_friendly_tags` →
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

### Mounted-vehicle panel — `HUD_DrawVehicleHealthBars @0x5a4fd0` (ported end to end 2026-08-21)

The silhouette-and-seats panel drawn while the local player is mounted.

**2026-09-05 integration correction (D-NET-157):** the mounted view now sends
the authored items.def ID to the shell's SID lookup. Runtime vehicle type 1301
must resolve definition 101301; handing through 1301 made the real ATV panel
vanish. The remote seat HP lookup applies the same `kItemIdOffset` conversion.
The engine already uses this mapping for item traits; these two consumers had
missed it. `simulation_test::test_vehicle_panel_resolves_authored_item_id_after_seat_selection`
and `inmatch_joiner_role` both failed with real ID conventions before the fix.
The live retail LAN retest then displayed the truck silhouette, its AI-occupied
driver marker and the local passenger X; the pre-fix screenshot had no panel.
Numbered seat actions share the panel's ordered slot list; see world-wac-ai-re
section 23.1. This ID handoff is shell plumbing / not grillable; retail's texture gate
and health-band rules below are unchanged.

Witnessed in #536/#537/#540/#541, re-witnessed in the post-merge review, and
completed in the wire-up round (the list builder, the gate, the label digits):

- **Caller gate** (2026-09-22, PR #671 fix round): `HUD_RenderOverlays
  @0x5A7BB0` draws no panel without a root entity (`@0x5A7CAE`) and calls the
  drawer only on the slot-3 arm with the weapon group visible (`@0x5A7CE4`),
  the slot-1 arm (`@0x5A7CFF`) and the slot-2/5 arm (`@0x5A7D23`); every other
  slot falls to `@0x5A7D5F` (`cmp eax,1; jnz` `@0x5A7CF5..0x5A7CF8`). The port's
  extra `mount_slot == 0` arm is removed. The gate reuses
  `hud_stance_group_visible`, whose slot-0-with-WPNGRP arm is retail's no-root
  stance case (`@0x5A7D42..0x5A7D55`); a shown panel with slot 0 cannot occur in
  play (a mounted rider's seat type is 1..5), and only the synthetic
  `hud_overlay_test.gd` vehicle-panel fixture relies on it.
- **Gate** `@0x5a5038`: the panel draws iff the root vehicle's def has the
  VEHICLE_HUD `interface` texture loaded with nonzero size (`itemDef+0x960/
  +0x964/+0x968`). There is NO seat-class gate inside the drawer: the root is `rootEntity
  @0x27235bc`, whose writer is UNWITNESSABLE (all five xrefs are reads — the
  HUD entity-info block is filled from outside the image), and the list
  builder re-roots a mount on an attached gun child to its parent vehicle
  `[orig: Entity_BuildWeaponSlotList @0x434c77..0x434c7e]`, so every mount
  takes the panel.
- **The seat list** `[orig: Entity_BuildWeaponSlotList @0x434c60]`: [0] = the
  vehicle as the control seat (type 8) `@0x434ca9`; then, in child-list order,
  the UseGun children (def+84 & 0x20, not 0x40, parent == vehicle, bone via
  `Entity_GetMountSlotBoneIndex @0x546680` — ex `sub_546680`, the cached byte
  +0x319 indexing the parent's def+0x214..0x217 table — childDef+0x266 != 0)
  as type 9 `@0x434cf4..0x434d5e`; then the present passenger seats s = 0..7
  (`def[0x25D + s] != 0`) as type s `@0x434d8d..0x434da8`; the tail zeroed to
  type 10, cap 10 `@0x434dad`. The marker arms: `mountHandles[0..7]` ↔ the
  `seats` pair i `@0x5a5112..0x5a5364`, `mountHandles[8]` ↔ the `driver` pair
  `@0x5a568e..0x5a5793`, the pool-1 children with `itemDef+84 & 0x20` whose
  slot byte matches `itemDef[0x214+i]` ↔ the `emplace` pair i `@0x5a53b7..
  0x5a5547`.
- **Labels**: `sprintf("%1d", n)` (the format at `0x7d8e64`, IDB-labelled
  `off_7D8E64` — the dword 0x00643125 IS "%1d") centred half-bright white via
  `HUD_DrawTextCentered_HalfBright` in the BOLD label font, the text top at
  `centre + 1 − h/2` `@0x5a52f0..0x5a5322` (the same rule for the seat digits
  and the own-seat X): a passenger seat = its 1-based list
  position mod 10 `@0x5a5283`, an emplacement = i + 2 `@0x5a5602`, the driver =
  1 `@0x5a57ee`; the own seat draws the literal "X" (`0x7d9f4c`) `@0x5a586b`.
  The emplacement digit redraws (2026-09-22): for each gun slot i whose
  `def[0x214 + i]` matches the child's bone, the WHOLE slot list is walked with a
  type-9 counter starting at -1 (`@0x5A5593`), incremented on type-9 entries
  (`@0x5A55A0..0x5A55A7`), and the digit is drawn at EVERY list position whose
  counter equals i (`cmp` `@0x5A55AC`, walk end `@0x5A5670`); the label repeats
  once per following non-emplacement entry and never draws when the list holds
  fewer than i + 1 emplacements (`emplace_label_draws`, `vehicle_panel_feed`).
- **Bands**: riders and the driver `(health << 16) / max` (max → 1) with the
  unsigned/signed asymmetry below; emplacement occupants through
  `HUD_ClassifyHealthBand(min(ratio, 0x10000))` `@0x5a54e3` (2 Good, 1
  Middle); the silhouette takes the HULL's band `@0x5a50d1`.
- **Silhouette window** (2026-09-22): the panel's
  `draw_textured_quad_with_border @0x590C40` call (`@0x5A50D1`) passes the
  silhouette's authored width/height as both the texture and the quad extent;
  u0 = 0.05 / tex_w (`flt_7C68E8` = 0.05f `@0x590D36`, divides
  `@0x590D3C..0x590D40`), u1 = 1 − u0 + 1 / (right − left) in scaled screen
  pixels (`@0x590D5E..0x590D77`), v the same (`@0x590D7B..0x590D8C`), with
  (u0, v0) at the top-left vertex and (u1, v1) at the bottom-right
  (`hud_math::bordered_quad_uv`; the helper's other callers still draw 0..1).

- **Data**: the `hudpos.def` `VEHICLE_HUD … VEHICLE_END` blocks (one per item
  `sid`; 50 shipped, 28 in the tracked fixture) — see the token map below.
  `emplace`/`seats` lead with a COUNT (`emplace 1,27,220` is one pair), capped
  4/8; the retail drawer derives the seat count from the entity, so the
  authored counts are ours (`DefVehicleHudBlock::emplace_count/seat_count`).
- **Base**: `HUDVEHSTANCEPOS (dword_2723AF4/AF8)` + the rider's stance offset
  from the shared `HUDSTANCE` tables (`dword_2723B24/B44[byte_27235C0]`)
  `[orig: @0x5a509b..0x5a50b9]` — the panel moves with the stance icon.
- **Seat box**: `g_hudVehSeatMarkerW/H = 11` (named 2026-08-21, ex
  `dword_27237FC/2723800`; set from `eax = 0Bh` `@0x5a47b0/@0x5a47b5` in
  `HUD_InitOverlaySystem @0x5a4620`); labels centre at `floor(11/2) = 5`.
- **Band**: `(health << 16) / max` with `max` forced to 1 BEFORE the divide;
  the Good test is UNSIGNED vs `0xC000` `@0x5a507f`, the Middle test SIGNED vs
  `0x6FFF` `@0x5a508a`, so a negative ratio wraps large and reads Good;
  colours are the stance triple `g_stanceColorGood/Middle/Bad`
  `@0x5a5130/@0x5a513b/@0x5a5095`. Riders classify through
  `HUD_ClassifyHealthBand` with the `0x10000` clamp `@0x5a54db` —
  behaviourally equal on the tested edges.
- **Draw order**: silhouette (the hull's band), occupied seat boxes banded by
  the rider's health, the empty seats' select digits (`0xFF7F7F7F` via
  `HUD_DrawTextCentered_HalfBright @0x5a5322`), and the local player's own
  seat X LAST.
- **Port**: `hud_vehicle_panel.h` (policy + pins), `hud_frame_vehicle_panel.cpp`
  (the element; joined into the overlay cluster before the feed and the Tab
  board), `world/vehicle_panel_feed.{h,cpp}` (`vehicle_panel_root`,
  `build_vehicle_panel_slots`, `fill_vehicle_panel_seats` — the re-root, the
  list order, the three marker arms, the digits, the hull/rider/emplacement
  bands; ctest `vehicle_panel_feed`), and the device/shell legs landing in
  this PR: `HudOverlay::set_vehicle_panel(shown, block, stance, sim)` loads the
  block's `interface` silhouette into `kHudTexVehiclePanel` per carrier `sid`
  (the `set_weapon` idiom), maps `stancecolor_good/middle` from the def beside
  `bad`, and `vehicle_panel_presenter.gd` drives it off the local view's
  mount state. The empty-seat digit draws `0xFF7F7F7F` — white through the
  half-bright draw — like the own-seat X (corrected in the wire-up round).

### AAS zone status panel — `HUD_DrawZoneStatusPanel @0x5a2480` → `HUD_DrawZoneMarker @0x5986f0` (witnessed + ported 2026-08-21)

The objective ("LFP" = `Overlays/LFP` = "Objective") status element: one marker
per contestable spawn zone, grouped by owning team, with the per-group
`STROVER_UNDERATTACK` / `STROVER_READYFORTAKEOVER` text. Both functions were
IDB-renamed 2026-08-21 (ex `draw_capture_point_status_overlays` /
`draw_capture_point_detail_panel`); the marker's prototype was corrected from
0 args to `(int x, int y, entity *zone, int letterIndex)` — the caller pushes
four `@0x5a2797..0x5a279b`, cleanup folded `@0x5a27a0`.

- **List**: `Entity_BuildSpawnZoneList @0x43eae0` order (pools 2/1, def attrib
  `0x40000`), walked via `SpawnZoneList_GetCount/GetByIndex @0x43b920/@0x43b930`;
  a zone draws only with a `g_zone_timer_list` entry (`CProximityList_FindEntryById
  @0x537f50`). The marker letter is `'A' + list index`.
- **Layout**: HUD space, never a world projection. Markers step ACROSS 98 px
  within a team group (`x += 0x62 @0x5a27a3`), groups step DOWN 86 px
  (`y += 0x56 @0x5a2667/@0x5a2781`); the group is right-anchored: first marker
  at `g_hudZonePanelX − 98·zonesInGroup` `@0x5a2589..0x5a259d` and at the
  group's OWN y = `g_hudZonePanelY + 86·group` (no offset); only the status
  TEXT sits at `(x − 4, y + 12)` — `ebx = y + 0Ch` `@0x5a25b9..0x5a25bd` is
  the text row, `x − 4` `@0x5a2601` (the final review corrected the port's
  marker-row +12 and the "y − 4" slip; the IDB comment likewise); the anchor
  is the `LFP_FLAGS`
  token (`g_hudZonePanelX/Y`, named 2026-08-21). The conquest arm
  (`g_GameType == 0x50010 @0x5a24a1`) is unmodelled.
- **Marker** `@0x5986f0`: the sheet by the zone's `+0x162` team byte — 1 →
  `JO_LFP.tga` (`dword_27231A4`), 2 → `R_LFP.tga` (`dword_27231A0`), else
  `N_LFP.tga` (`dword_272319C`) `[orig: HUD_LoadAllTextures @0x59e0a3..
  0x59e0f3; the pick @0x59893d..0x598957]`; the team colour is
  `g_hudColorTable[3]` / `[5]` / `[1]` = `FF80A0FF` / `FFFF5050` / `FF00FF00`
  (`HUD_InitTeamColorTable @0x51f240` — the BSS globals `g_hudColorLightBlue
  @0x24c1844` / `dword_24C184C` / `dword_24C183C` are those table slots; the
  port's literals are the witnessed values); in-cylinder = 2D distance ≤
  `+0x15E << 16` with `|dz|` ≤ half `@0x5987a6..0x598810` — the capture radius
  IS on the wire (`client_state.h` `zone_radius`, the pool-1 0x0D block), so
  the frame is computable; dimmed when neither in-cylinder nor contested;
  atlas ROW passed to `CEffect_Begin_Debug @0x67bb50` (a thunk to
  `draw_tiled_texture_strip @0x67aed0 (effect, rect[4], colour, frame)`):
  0 default, 1 in-cylinder `@0x598911..0x598915`, **2 = own zone under
  attack** (timer rate `entry[11] < 0` and `entry[1] == team`) on the
  off-blink phase `(g_hudFrameCounter & 0x18) == 0` `@0x59891a..0x598926`,
  **3 = another team's zone ready** (`entry[9] == 0`) on the on-blink phase
  `@0x59892d..0x598934` — 8 frames of phase A in every 32 (the counter is
  `++` per MAIN FRAME in `Game_TickHudFrameCounters @0x434c00`, so retail's
  blink is frame-rate dependent; the port's counter ticks at the 62 Hz logic
  rate — a device fold, named here); colour:
  under-attack off-phase `0xFFFFFF00` `@0x5988ea`, ready on-phase half-bright
  `@0x5988fd..0x59890d`, out of range and no state quarter+half dim
  `@0x5988b9..0x5988d6`; the half-bright modulate `0xFF7F7F7F` pushed
  `@0x598975`; the flag tile `draw_textured_quad_centered(x+34, y+52, 36, 36,
  zoneTeam == localTeam ? lfp_dlf.tga (dword_27239D0/D4) : lfp_alf.tga
  (dword_27239C0/C4), colour)` `@0x5989de` — both tiles loaded by
  `HUD_LoadAllTextures @0x59e104..0x59e11f` (the own-zone tile is NOT the
  team sheet; corrected in the final review); letter `'A' + index` at `(x+32, y+44)` `@0x5989ed`; progress
  bar `x+56..x+76 / y+30..y+82` with fraction `entry[8]/entry[10]` when
  `entry[12]` `@0x598a27..0x598a3a`; contest counts `entity[544]/[545]` at
  `(x+16, y+66)` / `(x+47, y+66)` — written by the 0x6F handler
  `NapiNPClientMsg_ZoneTimerValue @0x428e79/@0x428e7f`; the distance subtracts
  the zone entity's own bound radius `@0x5990f3` (not the zone radius),
  `%1dm` / `%01.2fk` at `(x+16, y+18)`. The marker publishes
  `g_hudZoneStatusKind` (1 attack / 2 ready) and `g_hudZoneStatusColor` for
  the caller's text (named 2026-08-21).
- **The timer entry** `[orig: ZoneTimerList_SetEntryValue @0x537ec0]`, 13
  dwords: [0] zone entity, [1] = [2] team, [8] value (creation only), [9]
  control target, [10] limit, [11] rate (i16), [12] active, [3..7] the window
  — matches `client_runtime.cpp apply_zone_timer_value`, which now also
  retains the two contest bytes.
- **Port** (2026-08-21): `hud_lfp_panel.h` (policy + pins) +
  `hud_frame_lfp_panel.cpp` (the element: the group walk in
  `SpawnZoneRegistry` order, a new group when the team byte changes
  `@0x5a25f6`, the right-anchored 98-px marker pitch and the 86-px group pitch,
  the status text at `(x − 4, y + 12)` under markers at the unshifted group
  y, the three team sheets + the two flag tiles, the rows/frames,
  the bar, the counts, the distance) + `world/lfp_feed.{h,cpp}`
  (`build_lfp_zones` — the deploy-zone join WITHOUT the joiner gate, the
  in-cylinder test from the wire radius, the counts' sides by the viewer's
  team) + `ZoneState::Entry::contest_owner/other`; `HudOverlay::set_lfp_panel`
  + the texture uploads + `lfp_panel_presenter.gd` (shown iff the game
  type is Advance-and-Secure `0x10010`) landed 2026-08-21 (D-HUD). Pinned by ctest
  `hud_lfp_panel`, `lfp_feed`, `hud_frame_compiler`, `npruntime_client_runtime`.
  **Residuals** (named at their sites, no D-row): the capture bar's FPU
  vertex positions and the count-differential arrows (n = min(|Δ|, 5),
  `x+76..x+90`, 7-px rows from `y+57 − (7n >> 1)`) `@0x598dc0..0x598fe4` are
  not drawn; the letter font slot `dword_2723C74` is unwitnessed (the panel
  has NO declutter bit: its call in `HUD_RenderAllOverlays @0x5a8530` is
  unconditional and the head tests only `g_GameType`); the conquest arm (`g_GameType == 0x50010`, every marker its own
  row) is unmodelled and hidden.

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
- **Text (2026-09-22, PR #671 fix round)**: `service_prompt_text`
  (`engine/runtime/hud/hud_game_text.h`) builds the line with CRT sprintf
  semantics and each call's own arguments: the key name for the armory and bay
  lines (`@0x5BDF45`, `@0x5BDFC9`), the seconds for the FARP wait (`@0x5BE09C`),
  none for FARP reloading (`@0x5BE0E9`). The armory and FARP templates come
  through `GameText_GetString`, whose miss is "" (`@0x51EC08`; calls
  `@0x5BDF37`, `@0x5BE08E`, `@0x5BE0DE`), so the invented English fallbacks the
  Godot presenter carried are gone; only the vehicle bay keeps its compiled-in
  fallback `"!Press '%s' to activate vehicle bay menu"`
  (`GameText_GetStringWithFallback` call `@0x5BDFBB`, string pushed
  `@0x5BDFAC`). Ctest `hud_game_text`.

Every line is one call of the same shape: design anchor (0x200, 0x118) =
(512, 280), the **large** overlay slot `g_hudLabelFontLarge @0xB4C3A0`
(Impac22b), `g_hudColors.active`, draw mode 2 `[orig: the slot push @0x5BDFD7
(armory / vehicle bay), @0x5BE0AA (FARP wait), @0x5BE0F7 (FARP reloading);
HUD_DrawTextAtVirtualPos @0x5D3EC0 -> the mode dispatch @0x5D2EA0, mode 2 =
HUD_DrawTextCentered_HalfBright @0x5D2ECE]`. The unlock word `dword_A85BBC`
has ONE writer, the S2C 0x0A phase-0 store `[orig: NapiNPClientMsg_0x00A
@0x430136]`, fed by the host's per-frame owned-zone walk (net-re §5.61): a
host that sends a constant there unlocks or locks FARPs the chain never
decided.

The received-state feed and all three draw legs are ported (2026-09-19; the
slot corrected the same day: the first port drew them in the hudpos font).
D-HUD-14 retains active-menu suppression and the vehicle-bay/FARP service
integration; drawing the timer does not implement the rearm service.

### Weapon / vehicle combat cues: font slots, the LollyPop head, the death gate, the Inset scene (witnessed 2026-09-19)

The #655 review's leftovers, each re-witnessed before it was changed. The mode
matrix and the validation runs live in
[weapon-vehicle-hud-validation.md](weapon-vehicle-hud-validation.md); this
section owns the witnesses.

**Which overlay slot each text rides.** The slot table is
`g_hudLabelFont @0xB4C388` + 12 per slot (normal, bold `@0xB4C394`, large
`@0xB4C3A0`, Impact38 `@0xB4C3AC`); the hudpos-named HUD font is its own slot
`dword_2723C74`, loaded at scale 1.0 (`0x10000`) from the hudpos font name for
the surface width tier, and a copy of the BOLD slot when hudpos names none
`[orig: sub_591890 @0x591890 -- the load @0x5918B4, the fallback copy
@0x5918D6..0x5918E1]`. It is the ammo/name drawer's font.

| Text | Slot | Draw | Witness |
| --- | --- | --- | --- |
| Control-seat gear label (Low / Med / High) | large (Impac22b) | left-aligned at the scaled design anchor (`gear_x`, `gear_y - 50`), `g_hudColors.active`, flags 1 | `hud_draw_target_entity_overlay @0x59A5D0`: string select `@0x59A694..0x59A6BB` (byte 0 -> Med, 2 -> High, else Low; strings loaded `@0x5A4871..0x5A48CF`), anchor `@0x59A6C1..0x59A6E6`, slot push `@0x59A6F9`, `HUD_DrawTextLeft_HalfBright @0x59A6FE` |
| Armory / vehicle-bay / FARP prompts | large (Impac22b) | centred on design (512, 280) | the Gameplay prompts section above |
| Inset friendly name | bold (Arial bold) | centred on the aperture centre in SURFACE pixels, colour `0xFFFF0000` through the half-bright fold | `Render_WeaponInsetScene`: slot push `@0x5CA0C0`, `HUD_DrawTextAligned_HalfBright @0x5D2F20` mode 2 `@0x5CA0CF` |
| Mortar impact distance (`STROVER_DIST`) | drawn in the hudpos slot, MEASURED in the bold slot | design x = `impact_x - (width >> 1)` where width = the bold slot's unscaled extent x its scale, truncated; then the design pair scales to the surface and the line draws LEFT-aligned, flags 1, `g_hudFrameOverlayColor` | `HUD_RenderAllOverlays`: `GameFont_MeasureTextWidth @0x580A50` with slot `0xB4C394` `@0x5A897E..0x5A8984`, `sar 1` `@0x5A8995`, `Viewport_ScaleToVirtualCoords @0x5A89B0`, hudpos slot push `@0x5A89D0`, draw `@0x5A89D5` |

`HUD_DrawTextLeft_HalfBright @0x5804C0` folds the colour to
`(c >> 1) & 0x7F7F7F | 0xFF000000` `[orig: @0x5804D8..0x5804F0]` and passes the
slot's scale pair into the draw `[orig: scaleY @0x58052B, scaleX @0x580539]`.
Flag 0x100 forwards the font drawer's 0x100 option `[orig: @0x5804E1..0x5804E9]`
and flag 2 adds its option 4 and swaps a constant 0.5 argument for a global
`[orig: @0x5804F5..0x58050A]`; the literal 1 every caller here passes sets
neither, so these are plain left-aligned runs.

**The LollyPop head is a 2:1 ellipse.** `HUD_RenderAllOverlays` admits the
marker on weapon flag 0x8000 alone `[orig: Entity_CheckWeaponSeatFlags
@0x5A8805]`, colours it `0xFF0000` with alpha 0xFF when the terrain ray from
the player (+2 units up) to the impact point (+2 up) is clear and 0x60 when it
is blocked `[orig: @0x5A8865..0x5A88C3]`, scales it
`clamp(1 / (distance_m * 0.005), 0.25, 2.0)` `[orig: @0x5A88CE..0x5A8910]` and
calls `draw_entity_marker(position, type 0, scale, colour, 0, 0, 0, 0)
@0x5A8924`. Type 0 draws a stem from the projected point up by
`half = ftol(scale * 20.0)` `[orig: @0x5931E0..0x59321D]` and one
`draw_ring_overlay @0x5D4270` record centred `2 * half` above the point
`[orig: @0x59322D..0x593283]`. The ring record is eight dwords:
`{x, y, z = 0.5, radius = half, stroke = 2.0, fan texture = 0, colour,
x-scale = 2.0}`. The stroke and the x-scale are the same `2.0` constant stored
twice `[orig: +0x1C @0x59327B, +0x10 @0x59327F]`. `draw_ring_overlay` multiplies
ONLY the x term by record `+0x1C` `[orig: @0x5D4513]`, so the head is twice as
wide as it is tall; its segment count keys on the unscaled radius
(`clamp(ftol(radius * (1/3) * 4pi), 4, 95)`), and the four radial stops are
`radius -+ (stroke/2 - 1)` with a 1-pixel transparent fringe either side. The
Inset aperture ring is the same record with `+0x1C` = the viewport aspect
factor `Render_GetViewportScaleY` and stroke 2.0 `[orig: @0x5C9E16..0x5C9E7F]`.

**The HUD's death gate is `g_death_screen_active @0xA860EC`, alone.** Neither
the local dead bit (`Flags & 2`) nor the death lerp camera (`g_camera_mode`
4, which the main-scene arbiter selects from that bit `@0x5CA217..0x5CA24B`) is
a HUD gate; between the death and the latch the passes below still run.

| Pass | Gate | Witness |
| --- | --- | --- |
| Crosshair, tracked-target cursor, CustomAim, friendly brackets | `!g_death_screen_active` | `HUD_DrawCrosshair @0x592646` |
| Weapon / vehicle silhouettes, gear label, ammo, stance, heat, parachute/armor/cargo, altitude ladder | `!g_death_screen_active` (the death-screen arm draws the spectated entity's health, the team line and the timer instead) | `HUD_RenderOverlays @0x5A7BBC` |
| Scope overlay details (commander reticle) / binocular speedometer | `!g_death_screen_active` | `HUD_RenderAllOverlays @0x5A850D` |
| Scope selection incl. the Inset scene | `!g_death_screen_active` | `Render_ProcessMainSceneFrame @0x5CA26A` |
| Controller forward pip | none of the above; `!g_binocularsViewActive && g_camera_mode == 0` | `draw_weapon_sight_crosshair @0x59ECA0`, gate `@0x59ECE0` |
| Turret lag pip | no death, camera or binocular test at its head; reached only with a parent whose `attrib2 & 0x1000` | `HUD_draw_crosshair @0x59EA20`, call site `@0x5A84DF` |
| LollyPop marker, impact distance | **none**: the tail of the walk runs unconditionally after the takeover-status test | `HUD_RenderAllOverlays @0x5A87EF..0x5A89DA` |
| The impact preview that feeds them (and the 2DImpact map slot) | **none**: weapon-flag admission only | `Player_UpdatePerFrame @0x4DE760..0x4DE79D`, the preview `@0x4DE80A..0x4DE929` |

`Entity_CheckWeaponSeatFlags @0x540D00` is the only admission those last rows
have: OnlyScoped (0x80000) on the local player's slot requires
`g_weaponScopeActive`. Every stock weapon that authors the impact flags also
authors OnlyScoped, so the missing death test is unobservable on stock data;
the port keeps the structure rather than the accident.

**The Inset scene is the whole world pass, particles included.**
`Render_WeaponInsetScene @0x5C9740` (ex `Render_RadarCompassOverlay`, called
`@0x5CA949`) recomputes the view
(`Camera_ComputeThirdPersonView`, the call `@0x5C9841`, slot offsets `@0x5C98F7..0x5C9903`),
draws the two depth-mask fans, then renders the scene with the SAME function
the main view uses: `Terrain_RenderWorldScene(view, 0, 0, 0)
@0x5C9DE9`. Its arguments switch off only the sun glow (second argument,
tested `@0x5C970E`) and the water-mirror subpasses (fourth argument); the
sector models and entities, both `CEffectEmitterPool_RenderMainPass` +
`EffectWorld_RenderParticlePass` pairs (far side `@0x5C95AC/@0x5C95B5`, camera
side `@0x5C9687/@0x5C9690`), the projectile trails, the weather trail
particles `@0x5C96A6`, the coronas and the scars all run. The first-person
viewmodel is not part of that function (the main frame draws it in its own
viewmodel-first step), so the aperture never shows the gun. Port: the Inset
camera takes its own particle view group in `godot/src/particle/particle_renderer`
beside the main and water-mirror groups, and (2026-09-26) every tail draw of that
scene runs per view (beams, precipitation, coronas, glint; no glare,
`renderer::kInsetOverlayOrder`), under the Inset's own fog
(`ApplyFogAndAmbient(0, eye below water)` `@0x5C9D41..0x5C9D4F`, never the thermal
grey); the Inset also collects, selects its LODs and draws its terrain and foliage per
view ([render-occlusion-re.md](../render/render-occlusion-re.md) §8a).

## `hudpos.def` parser token → global map — `HUD_ParseHudposToken @0x59f370`

A `_stricmp` token-dispatch; each token reads decimal fields via `atof → ftol`
(1024×768 ints) or copies a texture-name string. Cross-checked against
`DefHudPosDef` in `engine/formats/def/def.h`.

| Token | Writes |
|---|---|
| `HEALTHPOS` | `dword_27237C8/CC/D0/D4` (x1,y1,x2,y2) |
| `HUDSTANCE <idx> <x> <y> <tex>` | `dword_2723B24[idx]`, `dword_2723B44[idx]`, `byte_2723B8C+idx*0x13` |
| `HUDSTANCEPOS` | `dword_2723AEC` (x), `dword_2723AF0` (y) |
| `HUDVEHSTANCEPOS` | `dword_2723AF4` (x), `dword_2723AF8` (y) |
| `LFP_FLAGS <x> <y>` | `g_hudZonePanelX @0x2723D94` / `g_hudZonePanelY @0x2723D98` (named 2026-08-21) — the AAS zone status panel anchor `[orig: @0x5a0563 / @0x5a057b]` |
| `VEHICLE_HUD` … `VEHICLE_END` | the 0xDC staging block at `dword_2723DC0` (`sid` +0x04/16, `icon`/`interface`/`statictexture` +0x7C/+0x9C/+0xBC ×32, `driver x y`, `emplace n pairs` ≤4, `seats n pairs` ≤8), committed against the item table at `VEHICLE_END` and memset back to zero `[orig: open @0x59f380, close @0x59f3b8]`; parsed into `DefHudPosDef::vehicle_huds` (ctest `def_parse_hudpos`) |
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
| `stancecolor_good` / `stancecolor_middle` / `stancecolor_bad` | `g_stanceColorGood @0x2723ADC` / `g_stanceColorMiddle @0x2723AE0` / `g_stanceColorBad @0x2723AE4` packed ARGB — the shared bar colors (health-bar tiers, heat fill, the vehicle panel's seat/hull bands `@0x5a5130/@0x5a513b/@0x5a5095`) `[orig: @0x5a0dd5/@0x5a0e4b]`; all three reach `HudLayout` since 2026-08-21 |
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
story is refuted there). The comparison evidence for this section is
[`screenshots/pr-492/`](https://github.com/opennova-net/opennova/tree/8881f61d7cdf7f848cea85393a3858f6e2866dbd/screenshots/pr-492)
(the synchronized 00TRa spinmap pair), removed from the tree on 2026-09-26.

- **The mask is a content selector, not a gate.** Witnessed bits: 0 backing
  disc, 1 marker banks, 2 objective tether lines (`source & 0xC0` markers,
  team-colored, suppressing the bit-8 line), 5 entity labels
  (`draw_entity_labels_and_markers @0x5a49e0`), 6 compass ring (paired —
  the gate is bit9 && bit6, with the bit10 legs nested inside), 7
  tracked-target pointer in `g_hudActiveColor` (`@0x5a77fe`, ctx
  `g_trackedTargetPos @0x272350C`, drawer args no-line/tip-when-ahead), 8
  the waypoint state line in `g_waypointAltitudeColor @0x2723D7C` (ctx
  `g_waypointPosXY @0x2723518`; drawer args line+tip), 9 pairs into the
  compass gate, selects `render_terrain_decal`'s adjacent `use_alt_blend`
  argument, and gates the player grid-coordinate label with !bit12
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
  earlier "big-map mask" attribution is corrected). The bit9 terrain branch
  does **not** gate water: `HUD_DrawMapOverlay` tests it `@0x5a6670`, pushes
  `enable_fog_pass = 1` unconditionally `@0x5a6677`, selects
  `use_alt_blend = 1/0` `@0x5a6684/@0x5a6696`, and converges on the decal
  call `@0x5a66a5`. The spinmap's
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
  stencil share the inset the map setup subtracts —
  `scaleX((flags >> 8) & 2)`, i.e. TWO DESIGN pixels through the
  width-axis scaler `[orig: @0x5a64c0..0x5a650d; the literal edi = 2
  @0x5a60d8; the disc-radius subtract @0x5a6512..0x5a651d]`, the same
  expression the waypoint-pointer radius uses
  above. The 1920x1080 completed-pass probe reads it as four physical
  pixels because `scaleX(2)` IS 4 at that width; it is 3 at 1280 and 5
  at 2560, so a constant only matched the probed display (corrected
  2026-08-19). `draw_compass_indicator @0x59c900` still
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
  the separate WATER pass, not the brightness. In `HUD_DrawMapOverlay`, the
  bit9 branch selects only the neighboring `use_alt_blend` argument
  (`@0x5a6670`, `@0x5a6684/@0x5a6696`) before the shared call
  `@0x5a66a5`; downstream, the water redraw checks `enable_fog_pass` and
  generated vertices `@0x6079DC`. `PolyTrn_InitTextures
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
  `half-height − scaleX((flags >> 8) & 2)` — two design pixels through the
  width-axis scaler, 4 physical at the probed 1920 (the ring/disc paragraph
  above); the compass quad is `half-height × 1.25`, with the
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
in-world markers, `draw_directional_indicator_ring @0x598180`).
Radar topology re-read 2026-09-16 (live IDB): `Radar_AddBlip @0x59b280` (skipped when
`g_rules_flags & 1`) stamps ONE of four compass-edge WORD timers
`@0x2721EEC..0x2721EF3` to 31 ticks (`quadrant = (atan2 BAM - Yaw - 0x1FFFFFE0) >> 30`)
and takes the first free row of the 128 x 24-byte blip table `@0x2721F40` (type, xyz, life
62, colour). `update_radar_contacts @0x59a7e0` runs from `HUD_DrawMapOverlay @0x5a791c`
only when the map element flags carry `0x200 | 0x40 | 0x400` (`@0x5a78ff..0x5a790f`), and
from `HUD_RenderAllOverlays @0x5a817d`; it decays the edge timers and rebuilds the sector
bytes (type 0 -> 12 `@0x2721F30`, 1 -> 12 `@0x2721F24`, 2 -> 24 `@0x2721F0C`, 3 -> 24
`@0x2721EF4`, type 255 = all 12). `Player_OnDamageReceived @0x4dd880` produces type 255 for
self-damage, 2 when the attacker's item def `+0x294 == 6`, else 0. Under that same gate
`draw_weapon_direction_indicators @0x59c350` draws the 12 + 24 sector marks and
`draw_timer_overlay_box @0x59c7b0` ends in `draw_directional_indicator_ring @0x598180`
(12 segments: 1 = friendly list `@0x27233E8`, 2 = the `sub_59B200` contact list
`@0x2722B40`, 2 blinking on `tick & 8` for the type-0 damage sectors). The
four-quadrant edge drawer `draw_damage_direction_indicators @0x59a300` (four screen-edge
triangles, colour `0xFB441A`, visible while `timer/31 > 0.375`) has **no callers** in JO:
the edge timers are written and decayed but never drawn. The HUDDECLUT_* consumer is
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
  NO bit18 label. The absent bit9 does not suppress water: the caller's
  `enable_fog_pass = 1` push `@0x5a6677` is outside that bit's argument
  branch. mode 1 exists for a caller-supplied entity (rotating,
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
    `win_scores[slot]` × 100 from `byte_A763FB[slot]` `@0x454526`, into
    `Score_TallySubGoalWon @0x4FD100`, ex `Score_AccumulateBandwidth`, the call
    `@0x454532`: on the authority outside a session the won count `0xC846D0`
    +1 and the bonus `0xC846D4` += the value, scaled 3/4 at difficulty −1 and
    3/2 at 1; unported, with its SP epilog and end-round statistics consumers)
    + when the round is still running (`g_spawn_success_gate == 0` `@0x45453a`)
    a chat line = mission text `WinConditions/STRWINMSG%03i(win_id)`, posted to
    the CHAT ring and relayed to the joiners as S2C 0x3F kind 1 with team 1
    through `GameMsg_AddChatLineAndRelay @0x5BA170` (the call `@0x454578`); a
    header unknown5[2] per-slot mask (`byte_A762D6`, slot−1 bits) adds a
    team-banner leg (unported).
  - **15 SubGoalLost(slot)** `@0x4545e0` — NO already-set guard; set + chat
    `LoseConditions/STRLOSEMSG%03i(lose_id)` (relayed with team 0 through
    `GameMsg_AddChatLineAndRelay`, the call `@0x454632`) **+ the persistent
    banner** (the `GameMsg_SetBannerText @0x5BA200` call `@0x454647`); `byte_A762D7`
    team-banner leg (unported).
  - **35/36 ShowWin/LoseSubgoal(slot, bool)** `@0x4546af/@0x454724`:
    set/clear the show bit, then the objective notification (ported
    2026-09-23, `World::show_objective_notification`; ctests `bms_hud_relay`,
    `hud_game_text`, `nw_message_coverage`). Case 35 calls
    `HUD_ShowObjectiveNotification @0x5BA2E0` with (slot, 1, p2, 1) (the call
    `@0x4546E2`) and, with a local player and p2 set (`@0x4546E7..0x4546FB`),
    plays the NEW_GOAL set at the player (the call `@0x45470C` to the misnamed
    `HUD_DrawDefaultProgressBar @0x527E60`, a wrapper around
    `Sound_Play3DPositional @0x527CB0`); case 36 passes (slot, 0, p2, 0)
    (`@0x454757`) with no sound.
    The notification returns while inactive (`@0x5BA2F3`), resolves the
    directive `WinConditions/STRWINDIRECTIVE%03i` or
    `LoseConditions/STRLOSEDIRECTIVE%03i` of the slot's text id
    (`@0x5BA30E..0x5BA360`; a miss is ""), drops a directive of length <= 1
    (`@0x5BA37B`), and under `is_mp_session_peer || !is_in_session`
    (`@0x5BA382` / `@0x5BA38B`) posts two CHAT-ring lines, the gametext
    `Misc/STRMISC_NEWOBJECTIVE` header (`@0x5BA3AE`, 0x3A2 ticks) then the
    directive (`@0x5BA3C2`); the authority relays it as S2C 0x3F kind 0
    (`@0x5BA3CA..0x5BA3DF`, net record). There is no separate toast
    widget.
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

### Scope camera zero and readouts (2026-09-16, D-HUD-27)

**MATCHING for the ordinary Sighted/Scoped view and textual readouts.**
`LocalPlayer::present_view_frame` (the rendered frame) applies the active slot's
elevation/parallax in `Render_ProcessMainSceneFrame`'s modern camera branches,
and the `LocalPlayer::view_frame` observation mirrors it. Sighted takes
precedence and applies the offsets only when `WeaponDef+0x84` (maximum zero
steps) is nonzero; Scoped applies them whenever the slot exists. Both subtract
slot+4 from camera pitch and add slot+8 to BAM yaw, after camera composition;
binoculars bypass them. Mission yaw is `90 - BAM heading`, so both ported degree
adjustments subtract. Body/input aim and the separate targeting query remain
unadjusted by this camera consumer. `[orig: Render_ProcessMainSceneFrame
@ 0x5ca0f0; Sighted @ 0x5ca452..0x5ca465; Scoped @ 0x5ca494..0x5ca4a0]`
The similar `sub_5D27F0` predecessor discussed above remains unreachable; it is
not the implementation witness for this port.

`HudFrameCompiler::element_scope_details` draws through the existing HUD font,
half-bright color and authored `HUDSCOPERANGEXY`, `HUDSCOPEZEROXY`,
`HUDSCOPEMAGXY` positions. The entry gate is equipped player + CanFire + either
promoted scope selector. Its caller suppresses it for death-screen/binocular
views and HUD detail >= 3; the weapon declutter byte does not gate it.
`[orig: HUD_DrawScopeOverlayDetails @ 0x59e420, gate @ 0x59e47f;
HUD_RenderAllOverlays @ 0x5a8070, call @ 0x5a8526]`

- Rangefinder flag `0x400`: integer metres, floored at 1; strictly over 1000 m
  uses `Overlays/STROVER_DIST1KM`, otherwise `STROVER_DIST`. A nonzero baked
  `WeaponDef+0xF0` limit turns an over-range readout `0xFFFF5050` before the
  half-bright transform. `[orig: @ 0x59e4a9..0x59e59e]`
- Elevation flag `0x800`: signed slot word >= 0 times the authored metre step
  uses `hud/hud_scope_zero`; -1 uses `hud_scope_zero_auto`, lower negatives
  `hud_scope_zero_none`. `[orig: @ 0x59e8a2..0x59e974]`
- Only the Scoped selector draws `hud/hud_scope_mag` using slot+0x0C. A Sighted
  weapon can have magnification and an authored scope card while omitting
  this label, exactly as retail does. `[orig: @ 0x59e97c..0x59e9f6]`

The 2026-09-22 formatting follow-up (D-HUD-30) rechecked the HUD's CRT
`sprintf @ 0x76A9E4` call sites. Installed `Overlays/STROVER_DIST` uses
`Distance: %ldm`; the reimplementation recognized only the unmodified `%d`,
`%i` and `%u` forms, leaving the long conversion visible. The PR #671 review
fix round replaced the first formatter with `hud_sprintf`
(`engine/runtime/hud/hud_game_text.h`), which runs each template with its own
call's argument list, as CRT sprintf does:

- One int, the whole integer specification (flags, width, precision; `l`/`h`/
  `I32` size prefixes reduced to 32/16 bits; `%%`): the scope range at
  `@ 0x59E530` (reached by the 1 m floor push `@ 0x59E4E7` through the `jmp`
  `@ 0x59E504`, and by the signed divide `@ 0x59E506..0x59E511`),
  `hud_scope_zero` `@ 0x59E8F6`, `hud_scope_mag` `@ 0x59E9BD` (the raw slot
  word `@ 0x59E99E`), the mortar impact `@ 0x5A8972` and the FARP wait
  `@ 0x5BE09C`.
- No argument: `STROVER_DIST1KM` `@ 0x59E4D5`, `hud_scope_zero_auto` /
  `hud_scope_zero_none` `@ 0x59E938` and FARP reloading `@ 0x5BE0E9`; `%%`
  collapses, and a conversion with no argument prints literally (retail would
  read an unrelated stack word; no installed template has one).
- The key name: the armory and vehicle-bay prompts `@ 0x5BDF45` / `@ 0x5BDFC9`.

A missing mortar template is `GameText_GetString`'s miss, "" (`@ 0x51EC08`),
not the invented `"%d m"` default the port carried.
`hud_frame_compiler::test_optical_distance_long_format` checks the emitted
characters for both callers, including the 1000m boundary and the no-argument
collapse; `hud_game_text` covers the prompt lines. The earlier citation of
`@ 0x59E504` as a call was wrong: it is the floor arm's `jmp`. This was a
read-only IDA check; no IDB changes were made.
[orig: HUD_DrawScopeOverlayDetails @ 0x59E420, sprintf @ 0x59E530;
HUD_RenderAllOverlays @ 0x5A8070, text lookup @ 0x5A8961 and sprintf @ 0x5A8972]

Evidence: ctests `local_player_view` (standing/prone camera offsets, Sighted
maximum-zero gate, unchanged input/body aim and hip view) and
`hud_frame_compiler` (range boundary, over-range color, manual/auto/none zero,
magnification selector and visibility). A windowed `weapon_round` probe on
Training: Sniper (`00TRe.bms`) with its default `WPN_L115A` verifies prone ADS,
authored 200 m elevation and the unchanged aimed ERROR row 3, 524 Q16 units
(about 0.008 degrees). This install authors L115A as Sighted, so elevation is
visible and the magnification label is intentionally absent. A second prone
probe increments the zero from 200 m to 250 m and verifies the live readout.
This is not a claim of measured bullet-group parity. The separate flag-8 vehicle target
reticle and D-HUD-26 Inset renderer landed in the 2026-09-19 HUD follow-up.

### Binocular sway render latch (2026-09-16 review)

**MATCHING for activation timing.** The render branch seeds once on its first
active binocular frame and clears the latch when a rendered frame sees the
view down. Input toggles, fixed ticks and target-lock queries must not consume
the shared mission PRNG. A lower/raise pair between renders preserves the
previous displacement; a rendered movement/seat/death suppression permits a
fresh seed at the next active frame. `[orig: Render_ProcessMainSceneFrame
@ 0x5ca0f0, seed @ 0x5ca3e1..0x5ca3f3, clear @ 0x5ca4b0;
Binoculars_RandomizeSwayOffsets @ 0x4dd830]`
`LocalPlayer::view_frame` owns `local_player_binocular_sway_latch`; the pure
frame composer remains usable by targeting. Ctest `local_player_view` pins
render versus tick PRNG consumption, uninterrupted activation and suppression.
The randomizer's assembly keeps the signed BAM angle on the x87 stack across
its first integer conversion. Yaw is `8 * trunc(sin(angle) * 4194304)` and pitch
is `8 * trunc(cos(angle) * 4194304)`; the decompiler's `cos(4194304)` expression
is an x87-stack reconstruction error. The angle uses the binary's
`dbl_7C3608 = 1.4629627251502471e-9` radians per BAM unit (about 30.5 ppm above
exact `2pi / 2^32`). The port now preserves axis order and Q22 truncation, reverses the yaw delta for mission coordinates, and adds both
after camera composition. `[orig: Binoculars_RandomizeSwayOffsets
@ 0x4dd83c..0x4dd874; Render_ProcessMainSceneFrame @ 0x5ca403..0x5ca407]`
Ctest `player_view` pins the cardinal and eighth-turn offsets. No IDB edits
were made for this review.

## Divergence catalog (D-HUD)

| ID | Ours / reference | Original (Jointops.exe) | Why / consequence |
|---|---|---|---|
| D-HUD-1 | IDB curated name `draw_minimap_compass_overlay`; the oscarmike reference models a "spinmap" compass | `HUD_DrawStanceIndicator @0x599f10` renders the **stance** indicator, keyed by `byte_27235C0` = `hudInfo+568` stance index | The function is mis-named in the IDB and mis-modeled in oscarmike. The OpenNova stance widget must be the discrete cross-faded `HUDSTANCE` frames, not a compass. Rename proposed (held). |
| D-HUD-2 | oscarmike `spinmap.gd` conflated the IDB-misnamed stance function with a rotating compass-ring texture | the stance widget is discrete pre-rendered frames cross-faded on stance change. The `@0x599700` "radar" is separately the weapon heat bar, while the actual normal gameplay spinmap is `HUD_RenderAllOverlays @0x5a8070` → `HUD_DrawMapOverlay @0x5a5f40` in the authored HUDSPINMAP rect, gated by the declutter SPINMAP slot `dword_2723CC4` = `visible[17]` (visible at JOX's default `hud_detail 0` — the "compiled-in-true master switch" reading was the unloaded-BSS misread, corrected 2026-08-15; the 2026-07-18 "no in-HUD radar" gloss overstated: only the compass STRIP `@0x595470` and `draw_entity_labels @0x593820` are dead) | **FIXED 2026-08-13.** Stance and gameplay map are separate elements: discrete `HUDSTANCE` frames plus the heading-up terrain/blip spinmap with its counter-rotating `compring` overlay. The dead `HUD_DrawCompassStrip @0x595470` remains unported. |
| D-HUD-3 | — | Design space is fixed **1024×768**, scaled with round-to-nearest (`Viewport_ScaleToVirtualCoords @0x5d2b20`) | OpenNova authors HUD positions in 1024×768 and scales to the actual surface with the `(p*s+½s)/dim` rounding. |
| D-HUD-4 | — | Health bar *fill width* uses the capped `+92` ratio; *fill color* uses an uncapped recomputed ratio (`HUD_DrawHealthBar @0x5a2e50`) | Equivalent over `[0,1]`; recorded so the port matches both reads rather than collapsing to one. |
| D-HUD-5 | `hud_clip_indicator.gd` restamps its flash on (`round_type`, reserve) change | restamp keys are (`weapondef+220` ammo class, reserve, `weapondef+216` pool id) `[orig: @0x599ab2]` | Our weapon model runs a single ammo pool (net-re D-WPN-2), so the ammo-class/pool ids aren't distinct state yet; the proxy fires on the same reload/switch transitions. Revisit with per-class pools. |
| D-HUD-6 | **NARROWED 2026-09-11: the centre announcement banner is now ported.** The SYSTEM ring (D-HUD-23) and now the player-CHAT ring are ported: the S2C 0x14 fold → `HudFrameCompiler::push_chat_line` (the display-slot sink with the witnessed wrap), the `Chat_DispatchToChannel` channel → sink/colour table, the HUDCHATTEXT first loop of `element_feed`, and the J-key Recent Messages window over both display rings | the chat writer `[orig: Chat_AddMessageChannel1 @0x4985d0]` (raw ring 40×128 B, the display buffer 41×128 B with the newest message's last line in slot 1 and two-space continuation lines at timer 0, only slot 1's timer = `max(930, slot2 + 186)`); the dispatcher `[orig: Chat_DispatchToChannel @0x42b910]` — no local-team colour term; the geometry table `g_hudChatBoxCoords @0x28e4df8` (ex `dword_28E4DF8`) rows 1/2 = HUDCHATTEXT x1/x2 read through `HUD_GetChatBoxCoord @0x5bbe90` — its writer has no xref in the image; the involved-line copy `[orig: strncpy @0x427B8B]` feeding `HUD_DrawKillAnnounceBanner @0x59dc90` | The centre banner now uses the large white font at virtual (512,30), signed age <=186, and retained text after expiry (see the follow-up section below). Residuals: channel 13's `HUD_SetTrackedEntityTarget @0x59D050` (a world leg); `HUD_DrawOverlayPanels (ex sub_5C0060) @0x5c0060`, a third reader of the J toggle (unwitnessed). The geometry table's writer sits outside the image, so the authored HUDCHATTEXT/HUDSYSTEXT rows are the faithful source (`HudLayout::chat_box_x1/x2`). Narrowed 2026-09-23: the script chat lines (WAC text/ptext/text#, the WAC lose line, the BMS subgoal won/lost lines) post into the CHAT ring in raw white with the 930-tick timer (`Chat_AddSystemMessage @0x4EDB50` and `GameMsg_AddChatLineAndRelay @0x5BA170` → `Chat_AddMessageChannel1 @0x4985D0`); the SYSTEM ring keeps the triggered text and the console lines (consol/pconsol/consol#, forceanim; `Chat_AddMessageChannel2 @0x4987F0`). |
| D-HUD-7 | **CLOSED 2026-07-31.** The HUD consumes the exact ERROR integer plus the sim's signed `pitchBlend(+0x380)>>7` and movement/weapon-weight `(+0x384)>>7`; the same live `pitchBlend` feeds the first-person camera and local/decoded-player aim overlays | spread adds `(player+0x380 >> 7) + (player+0x384 >> 7)` `[orig: HUD_DrawCrosshair @ 0x592640]`; the write/decay side is `[orig: RoundData_SpawnRound @ 0x4ec0d0]` + `[orig: Entity_UpdateInfantryPlayerBody @ 0x4b40e0]` | **FIXED.** Exact integer carriers now run ammo/weapon parse → runtime tables → round/body sim → local HUD/camera/overlay; decoded rows stamp and decay recoil, while their movement term remains the retail zero of a local-only producer. The projectile's intentionally different `R>>8` stays separate. The water-height category's position-only `CameraOffset.Z` projection remains D-INF-18, not D-HUD-7. Pinned by `npruntime_round_sim`, `infantry`, `netsim_client_replica_pipeline_recoil`, and `hud_helpers_test.gd`. |
| D-HUD-8 | **FIXED / premise corrected 2026-09-19.** User color and hit feedback modulate the texture through vertex diffuse | FVF `0x2C4`, diffuse +16 and zero specular +20 `[orig: @0x5914CC..0x591500; @0x678962/@0x678A3E]` | The decompiler mislabeled the vertex fields. Literal UVs and integer corner/midpoint snapping are ported; native and GPU checks cover the reticle. The XHAIR_COLOR option stays live: persisted as the RGB value, the spinlist row selected by value `[orig: SpinList_SelectItemByValue(…, dword_25510E0) @0x554cec]`, default white `[orig: Config_SetDefaults @0x54d461 = 0xFFFFFF]`; the strip's colour store is `[orig: @0x5914d7]` and the draw `[orig: GDynamicVB_DrawPrimitive @0x6788e0]`. Not witnessed: the texture-stage setup inside `GfxShader_ApplyPassChecked @0x677020`; MODULATE is inferred from the colour riding the diffuse channel. |
| D-HUD-9 | **CLOSED 2026-07-31.** The crosshair previously hid from generic settled ADS | it draws while an aimed shot is NOT available — `!Player_CanFireWeapon() @0x5cf780`, whose promoted predicates are Scoped (`Flags & 1`) or Sighted (`Flags & 2`, except SWITCHFROM); movement/water reject only the ordinary Scoped leg, while reload-card-switch, camera, dead/airborne, ForceScoped, and seat gates complete the verdict | **FIXED.** The sim now stamps that bounded retail verdict once and feeds both visibility and ERROR row selection. The reticle remains through ADS ease and follows the witnessed Scoped/Sighted failure/override gates rather than raw `scope_engaged`. |
| D-HUD-10 | the crosshair anchors at the fixed design center (512, 384) | the anchor is the projected aim point through `Viewport_ScreenToVirtual`: the literal screen center only for the on-foot local player with no camera mode `[orig: @0x5928a0]`; spectate / `g_camera_mode` (external/3P) project `Entity_BuildCameraView` (far point 65536000 q16 = 1000.0) `[orig: @0x592910..0x59295e]` | FIXED 2026-07-11 (weapon round): `LocalPlayerPresenter.aim_screen_point()` — `Vector2.INF` in first person (the HUD pins the exact center, matching `@0x5928a0`), the projected aim in third person; `GameHudPresenter` feeds it to both shells. |
| D-HUD-11 | **CLOSED 2026-08-16.** `Simulation::fill_attach_labels` (ex `get_attach_labels`) now consumes the same complete `LocalPlayer::local_player_can_fire` verdict used to stamp the body/HUD aimed row: alive/equipped, passenger-or-borrowed-UseGun seat, reload-card-switch, promoted Scoped/Sighted/SWITCHFROM, movement, air, eye/water, ForceScoped, binocular, and camera gates | `Player_CanFireWeapon @0x5cf780`; the label branch is `!Player_CanFireWeapon() || entity == nearest_entity @0x5a32df..0x5a3354` | **FIXED.** The promoted scope bit is the body mirror committed for the current sim tick, while camera/binocular state is queried live so a presentation-time third-person toggle cannot lag. `simulation_test.gd::test_attach_labels_share_complete_can_fire_verdict` pins unraised ADS, settled ADS, immediate third person, and underwater behavior across two candidate entities. |
| D-HUD-12 | **FIXED 2026-08-11.** Attach labels lay out through the ported CGameFont engine with the witnessed BOLD Arial label font at the slot scale (`HudFrameCompiler::element_attach_labels` + `configure_label_fonts`) | `HUD_MeasureTextWH @0x580ab0` measures through the bold slot's (`g_hudLabelFontBold @0xB4C394`, ex "fontObj") `{handle, scale_x, scale_y}` pair (`CGameFont_MeasureText @0x674e70`); labels draw at raw screen pixels | Same glyph walk, same font file, same scale; box arithmetic `(x−w/2,y−2)..(x+w/2+5,y+h+1)` ported verbatim. Pinned by ctest `hud_frame_compiler` (label-font faces/scale). |
| D-HUD-13 | **CLOSED 2026-08-10.** The label color base is the hudpos `hud_textcolor` | the master overlay color `g_hudActiveColor @0x24c1868`; table slot 2 is refreshed per frame from hudpos `hud_textcolor` (`@0x5a8100`) and is the observed/default source | **The reimpl base is exact for the shipped observable path** and the dim transform stays ported (`HudAttachLabels.dim`). The scheme port (see "The hud_color_index scheme") is restored 2026-08-13 with the byte-witnessed producer: the `hudcolor` action row (code 10, default F6, retail-shadowed by `huddetail`) — neither of the earlier H mappings survives the catalog walk. |
| D-HUD-14 | **NARROWED 2026-09-19.** Localized armory, vehicle-bay and FARP wait/reload draw commands and retained phase-0 timer/zone inputs are ported | `HUD_DrawGameplayOverlays @0x5BDE60`; `STROVER_ARMORY_WAIT` remains unreachable | Active-menu suppression and integration with the vehicle-bay menu/host FARP rearm service remain. Native tests cover team/zone/timer gates and retained received state. The USE key's vehicle-loadout arm stays ported `[orig: Input_HandleActionBinding_0 @0x4e0a91..0x4e0aeb]`; the floating armory-delay label variant remains with this row. |
| D-HUD-15 | **CLOSED 2026-07-22.** The drawer was already parity-complete; the missing half was the source. The accumulator is now witnessed and ported (D-WPN-4, net-re §5.62): heat is a DEADLINE on the slot, `def+880 × (slot+0x14 − tick)`, stamped once per shot by the recoil arbiter | heat = `WeaponSlot_CalcAccumulatedHeat @0x53f780` per frame, clamped to `0xFFFF` into `hudInfo+60` `[orig: HUD_BuildEntityInfo @0x4b852e, clamp @0x4b854d]` | Fed sim → weapon view → HUD with the clamp applied where the original's info builder applies it. The bar fills on the thirteen emplaced/vehicle guns that author `heat_values` and stays hidden on foot, because no infantry weapon authors heat in retail either. |
| D-HUD-16 | the SP waypoint track is built sim-side at mission load from the BMS nav channel (`flags & 2`) + pool-3 markers — no 0x0F wire leg in the loop | retail always routes the list through the S2C 0x0F apply, even in SP mode 3 (the local server serializes, the local client applies) | Same data, same selection rule, no serialization round-trip. The npwire 0x0F waypoint block already decodes (net-re §5.29); wire-parity for MP join is the npwire follow-up, not a HUD divergence. |
| D-HUD-17 | proximity advance ports the distance/last-entry/skip-done legs; `SpawnPoint_CheckWeaponRestrictions @0x4dbe80` (the AAS spawn-point weapon-restriction pass gate) is modeled as always-pass; the MP POI list (`Entity_BuildMapPoiLists @0x42de40`) and spectate reuse are unported | the restriction check reads the 4 weapon slots vs the event-system restriction mask and can force-advance | SP missions author no weapon restrictions on route markers; port the check with the AAS/MP HUD phase. |
| D-HUD-18 | GEOMETRY CLOSED 2026-08-12: the checkbox (16×16, 4 lines, 0xFFE0E0E0), the done-mark RED X (6 lines, 0xFFFF0000 — the old "checkmark" gloss was wrong), the `HUD_DrawLabelBox` rect (y−0x18 / +0x48 / +0x30), the measured-height row advance, and the exact alpha/gray color folds are ported into `element_objectives` with the panel alpha byte carried in the frame state. The "New Objective" notification is walked and ported 2026-09-23 (`HUD_ShowObjectiveNotification @0x5BA2E0`: two CHAT-ring lines, the header and the directive, a directive of length <= 1 dropped, NEW_GOAL for action 35, the S2C 0x3F kind-0 relay; no separate toast widget). Remaining: the win-score add `@0x454526` (`Score_TallySubGoalWon @0x4FD100`; no SP score consumer), the header unknown5[2]/[3] team-banner legs (`byte_A762D6/D7`), the KEY_O reimpl binding (input layer), `HUD_DrawLabelBox`'s internal box-shader styling (fill+wire stand-in at the witnessed rect), and the fontLarge/fontBold slot plumb (single HUD font stand-in) | disasm 2026-08-12 (the full drawer); score add `@0x454526`, the notification call `@0x4546e2`, banner masks `byte_A762D6/D7`, binding row = the input layer | The state machine, row walk, geometry, color folds, the chat/banner announcements and the objective notification are exact; the residuals each ride an unported system (score / banner / input binding) plus the two cited drawer stand-ins. |
| D-HUD-19 | the DEATH deploy screen (`DeployScreenPresenter`, death.mnu) ships the authored chrome, the witnessed SPAWNPOINTS_LIST populate, and the pick flow — its MAP window renders no map image | the MAP window's render pass draws the windowed map view `MapOverlay_DrawView @0x5a58e0` (terrain layers + blips + labels; pan/zoom via `command_map_overlay_input_handler @0x554310`), the sibling of the fullscreen `HUD_DrawMapOverlay @0x5a5f40` | The pick behavior is complete without the image (the list is the pick surface); the map draw internals are the tracked next map-phase witness — port `MapOverlay_DrawView` and feed both the CMAP and DEATH windows from it; the 2026-08-13 gameplay-spinmap port (D-HUD-21) supplies the reusable compiler and banks to host there. **2026-08-24 — the populate's second loop + statics PORTED** (`world/deploy_screen_feed`, `Simulation::get_deploy_list_rows` / `get_deploy_status`, `DeployScreenPresenter._apply_statics`): after the zone loop `ListWidget_SortRows(list, 0, 1) @0x553c5a` sorts the WHOLE list (Default row included) with `cmp @0x6448a0` in string mode ascending = `stricmp(rowA.text, rowB.text)` (NULL text last); the occupant loop `@0x553c5f..0x553de3` walks the team zones with attrib 0x40000 and NO secured gate, finds the row whose value is `index+1` (else `insert_pos = 0` — the rows then land right after row 0, kept), inserts each `dword_A85BC4[idx]` member (`unk_A85CC4[idx*8+i]`, named `entity->Name`; self = `"<b><cFF4040>** %s **"`) at `insert_pos + 1` (UIList_AddRow returns the landed row = `list_insert_row @0x644f20` `return insert_index`) then ONE empty row; `sub_644AF0/@0x644b00` save/restore the scroll offset (list+796, clamped). STATIC_RESPAWN_MSG1 `@0x5538e7..0x553a7b`: hidden, penalty `dword_A85B5C` → `"%s  <cFF4040>%i"` over STROVER_PENALTYTIMER, else the wave zone listing the local player (`word_A85BC0 != -1`, `SpawnZoneList_IndexOf >= 0`): numbered (`entity+538`) `"'<WPNames/STRWPNAME%03d>':  <cFF4040><entity+548>"`, lettered `"%c:  <cFF4040>%d"`. STATIC_PSPRESPAWN_MSG1: `dword_A85B68` → STROVER_PSPRESPAWN. STATIC_MEDIC_MSG1 / STATIC_CALLMEDIC_MSG: `dword_A85B60 && entity+0x1E0 == 0` (`@0x553ec5`; the layout's carried/mounted parent reference — the IDB's `weaponSlots[16]` label is the struct's array overrun) → STROVER_MEDICTIMER and STROVER_CALLMEDIC formatted with `KeyBinding_FormatDisplayString(bindingEntry @0x81B534 = the MedicReq row) @0x496bd0` (three arms: modified slots joined by the localized `OR` separator with per-slot `Ctrl-`/`Shift-` prefixes, every keyed slot re-resolving the ONE keyName scratch `@0x496c07/@0x496ca7`; a modifier-less slot RESETS the buffer and prints the LAST RESOLVED key `@0x496f01` (the secondary when both slots are keyed; `Key 0` for an empty primary) behind either slot's Ctrl/Alt/Shift; the mouse arm; `" *"` on entry flag 0x200; every prefix, separator, mouse name and key name is a `KeyHelp_GetStringWithFallback("Keys", key, fallback) @0x51ed40` lookup over keyhelp.bin (the shipped table: `Ctrl-`, `Shift-`, ` or `, `Mouse 1`, `Key`; corrected 2026-09-12); port `controls::format_display_string` over `controls::key_string`). The three timers are the 0x0A sub-block-0 bytes `@0x430084/@0x43009f/@0x4300c3` (`ClientState::respawn_penalty_seconds/local_revive_seconds/spawn_hold_seconds`); the wave facts are the 0x6E fold (`ClientSpawnWaveStatus::self_zone_handle` = `word_A85BC0`). Remaining: the STATIC_INSTRUCTIONS arms and the permanent-death arm (ledger row). **2026-08-31 — the deploy-map OVERLAY open/close latch PORTED** (`ClientState::deploy_overlay_active`, `Simulation::is_join_deploy_overlay_active`, the MainGame frame-loop latch + the presenter's overlay open/close): retail arms `g_deploy_screen_active @0xA860DC` from the 0x0F game_flags bit0 unless the death screen is up (`NapiNPClientMsg_0x00F @0x42e2d8/@0x42e2f8`), then host-ASSIGNS it every per-frame 0x0A from flags1 bit1 (`@0x42ff82`); the frame loop opens death.mnu's DEATH once off it OR the local entity's undeployed bit (+0x24 & 2), latched `dword_24C1894` (`Render_ProcessMainSceneFrame @0x5cab5e..0x5cab8b`; suppressed while a menu is open `@0x5cab67` or the spawn-success gate `0x24C1928` is set), auto-closes the latched screen when both triggers clear (`@0x5cac8e` → the close-all-screens helper `@0x54b954` — the IDB's `Server_ResetBalanceCounters` label is a misnomer), and resets the latch at mission start (`Game_StartMission @0x525b31`). The deploy-screen keys 'X'/SPACE and the row select all route input case 12 — dialogs reset + one C2S 0x0E (`Input_HandleSpecialKeys @0x49c9fd/@0x49ca06`, letter keys pick a same-team `SpawnZoneList` row `@0x49ca42..0x49ca73`; the list select `@0x55364d`). The overlay dismiss's 0x0E SEND half is deliberately not ported yet: case 12 re-arms the client uplink hold, and how a host releases that hold for an already-deployed player is unwitnessed (the stock wave-join capture carries zero 0x0E) — the reimpl dismisses locally and stays silent, which a stock host cannot distinguish from a player who never pressed the keys. **2026-09-11:** both instruction statics and the permanent-death timer/player-count overrides are ported; see the follow-up section below. |
| D-HUD-20 | **FIXED 2026-08-10** (core). The friendly tags (overhead name labels) are ported end to end for the SP/AI path: `world::collect_friendly_tags` → `inmatch::collect_friendly_tags` → `Simulation::fill_friendly_tags` → presenter projection/fog/KEY_F cycle → `HudFrameCompiler::element_friendly_tags` — the witnessed gates, health-tier colors, distance alpha, centered alpha-preserving half-bright text, the `'^'`+36-name fallback, the BMS→`[PeopleNames]` authored names, the BRIEF ticks, and the medic cross plate (feed pending) | the drawer is `HUD_DrawEntityLabel @ 0x5a39b0` off `HUD_DrawFriendlyTagsPass @ 0x5a4480` (full witness: the element map section). The 2026-08-10 hunt's dead ends stay recorded: `hud_draw_target_entity_overlay @ 0x59a5d0` = the targeted-GEAR overlay, `sub_599C20 @ 0x599c20` = the scope quad | Residues, each with its owning system: (a) PORTED 2026-08-24 — the player-slot walk (authority: the connection table via `PlayerSlotLookup`; joiner: `replication::collect_roster_tags` over the 0x46 roster + decoded rows), the dead latch, the downed light-blue/gray recolor, the slot+44 medic-request pulse, the slot+16 revive count in both text forms, and the client 1 Hz countdown (`ClientRuntime::tick_roster_revive_countdown`); still open in (a): squad colors (`g_squadColors @0x83B450`, middle × 0.7), the slot+32 `<ch>`…`<co>` wrap (its writer is not the 0x46 clan/vehicle-name field: those land at slot+0x18/+0x1C `@0x434840`/`@0x434870`), the decoded remote player's class (no medic plate on joiner roster rows) and eye offset (the anchor rides the origin + 0x4000 there); (b) the magenta leg behind `g_enemyTagsVisible @0x24D1DF4`, which outside the death screen can only reach team-0 neutrals (the S2C 0x0A edge is mirrored but only rises with the death screen; the spectator-mode and action-130 writers are unported, so ordinary play takes the drawer's unequal-team bail); (c) CLOSED 2026-08-21 — the medic-plate FEED is the parsed charattr.def `ATTRIBUTES` Medic (0x8) by `playerClass` (`world::class_has_attribute`; `CharAttr_LoadFromDef @0x412140`, the flags word at +28 of the 31-dword class record), feeding the plate AND the map marker; (d) PORTED 2026-09-12 — the radio-request icon (TSDicon cell 0x17 beside the label, the ex-"wounded icon"; `entity+885` is the S2C 0x6D latch `@0x430C50`), the compiler arm with its two feeds (`HudFriendlyTag::radio_request`, `HudFrameState::radio_request_icon_viewer`) ported with the arm (2026-09-12: the world fold `FriendlyTagSource::radio_request` via `friendly_tag_aboard_vehicle`, the joiner's `collect_roster_tags` fold, and the viewer word `friendly_tag_radio_request_viewer` -> `Simulation::local_player_radio_request_icon_viewer` -> `HudOverlay::set_radio_request_icon_viewer`); (e) the good-tier scheme swap is live again with the witnessed `hudcolor` producer (D-CTRL-4 tracks the reachability divergence); (f) the speaking-pulse LEVEL feed (formula ported; the dialog-channel amplitude is a device follow-up); (g) the eye-offset triple is ported 2026-08-19 (local = exact head−Position, terrain-floored on foot; NPC = capsule z + witnessed lateral pair) — only the sample-less player legs' 3-angle lateral tilt (on-foot `@0x4b69ab..0x4b6b7c`, mounted `@0x4b66fc..0x4b68e5`) remains, plus the difficulty term of `Entity_GetMaxHealthWithDifficulty`; (h) the death-screen recolor/center-pin legs and the `0x27233DC/E0` latch bits (consumers unwitnessed). |
| D-HUD-21 | Gameplay spinmap ported and synchronized against retail JOTAC 00TRa (`HudMinimapCompiler` → `element_spinmap` → `HudOverlay`, snapshot v3): mission spawn zoom `65536/524288 × clamp(1 − Bms_MapZoom, 0.0625, 1)` with world-per-pixel `zoom/(rect height × 200)`, the true-pixel-circle disc `half-height − scaleX((flags >> 8) & 2)` (two design px width-scaled; 4 physical at 1920) + compass ×1.25 with the 0.05..0.95 UV crop, OOBJ footprints (opaque team fills, no boundary stroke), direct Colormap0..3 sampling + the depthspin water pass, the TSDicon MODULATE2X fold, the 253/254 fixed center rings, the M-cycle modes 2/3 + bit-12 grid, and 0x6B range-valid liveness | `HUD_RenderAllOverlays @0x5a8070` → `HUD_DrawMapOverlay @0x5a5f40` + the full witness map above | The residual in-map legs are this record's "Unported in-map legs" bullet (weapon-direction/timer/radar-contact, objective tether lines, entity/location labels, the tracked-target legs, the persistent-bank split + special layer-1/2 redraw quirks, the out-of-map siblings) plus big-map pan/drag, mask bits 11/13/14/15, modes 1/4, and the objectives-above-big-map ordering; the backing-disc color stays capture-calibrated pending a pass-state witness; 2026-08-21: the `hud_map_bracket` "target bracket" port (#545) was a misattribution of `HUD_DrawMedicCrossQuad @0x59bcb0` — the medic cross, drawn on the map by `draw_entity_labels_and_markers @0x5a4d40` for teammates with AnimMap slot 8 active — now `hud_medic_cross.h` shared with the friendly-tag plate; its map-scale half duplicated `hud_minimap`'s `zoom/(height×200)` and was dropped; the map medic marker itself PORTED in the wire-up round (`HudMinimapMarker::medic` replaces the teammate blip, snapshot v4) — `draw_entity_labels_and_markers @0x5a49e0`'s other legs (the pickup pulse icons 8/14, the parachute icon 23, the selection icon 28, the name/clan labels, the second `unit_type == 3` loop) remain in the in-map tails |
| D-HUD-22 | The HUDWPDINFO element renders "761 Marketplace" where retail (JOTAC 00TRa, identical pose) shows "760 m to Alley Corner" — same selected waypoint (distances agree within truncation) | the localized "m to" infix is an INDEXED string-table entry (witnessed in the RevX02 strings blob next to "m to FARP"; the composing drawer's table/index is unwitnessed), and the name pick is `get_waypoint_name @0x594630`'s raw-id vs +1-remap branch for gametype 0x30020 (or the mission-table source) | witness pass owed against the live JOTAC session; at the 32 m spawn waypoint retail shows the map's at-tip "032m" label while the HUDWPDINFO text row is ABSENT — a range or state gate on the info row to witness alongside the name/infix pass |
| D-HUD-23 | The kill/objective/medic message feed: S2C 0x1E folds to typed client events (`replication::ClientGameEvent`), each line is the game's own "Canned Msg" template with the witnessed substitution, and the line posts to the SYSTEM ring (see "The message feeds") with the per-case color table; the verbose gate is held at the verbose-on session default | `[orig: Chat_FormatMessage @0x422C60 -> String_ReplaceAllCaseInsensitive @0x422970]` (sequential case-insensitive `$A` then `$B`), the `STRCND48` bonus re-compose when aux is the local player `[orig: @0x422CA2]`, `STRCLI01` "Unknown" for a null actor `[orig: @0x422DDA]`; the `@0x426270` color switch (own white `-1` / other grey `0xFFAFAFAF`; friendly-fire 7/8/9 and 16-18/27-31/35-37 white; bonus 32/33/34 yellow `-256`; medic trio 38/39/45 + SSKB 46/47 `0xFF008CEE`; PSP/LFP blue `0xFF00AFFF` / red `0xFFFF0000`; 40 red, 48 orange `-32768`; camp 59/60 by team byte with the `WPNames[level+1]` `%s` compose `[orig: @0x4272EC/@0x427327]`); the suppression set (50-53 format-and-return `@0x42702E-@0x42716D`; 58 tip-only `@0x427202`); the verbose gate on uninvolved kill lines `[orig: g_MpVerbose2 @0x24D2154, 13 tests @0x426472..@0x4267C6]` | **OPEN (partial).** The 0x1E fold, the SYSTEM ring and the witnessed line/color policy are ported. Runtime team/gametype keys (19/20/21/46/47), signed SSKB counts, and flag-event immediate/delayed sounds are ported (2026-09-11; details below). Residuals: join/leave lines + the host-exclusion filter have their roster (D-HUD-24 folds S2C 0x46 into `ClientState`) and only need wiring; player-slot names with `<ch>clan<co>` tags `[orig: @0x422E1D]` (roster names serve today); the verbose keybind toggle `[orig: @0x49B78F]`; the other PSP/LFP/camp team sounds, tips and effect spawns beside the lines. |
| D-HUD-24 | The Tab scoreboard. DATA lane: S2C 0x16 folds to `ClientScoreboard` (flags, rows in wire order, the team table, the in-game/spectator trailer) and S2C 0x46 to a connection-slot roster, both in `ClientState`; joiners receive all three lanes on the reducer stream, the host's own view binds via the loopback self-0x46 (D-NET-114 form). PANEL (drawn 2026-08-19): `HudFrameCompiler::element_scoreboard` + `hud::hud_scoreboard` carry the witnessed layout in raw design-space constants through the shared scaler, every string on `g_hudLabelFontBold`; a press-TOGGLE on the playerlist action; the stdbox geometry (pieces, fill insets, the title notch, the screen-anchored wrap-tiled fill) pinned by ctest `hud_frame_compiler`; the monogram watermark deliberately not drawn (pure additive over a measured all-black sheet) | data: `[orig: NapiNPClientMsg_PlayerList @0x42FAE0; NapiNPClientMsg_PlayerSync @0x431370; Server_BuildAndBroadcastScoreboard @0x50D960 every 311 ticks]` — every well-formed 0x16 applies unconditionally (an empty update EMPTIES the board `@0x42fb46`), roster-unknown rows drop `@0x42fc05`, name/clan join at apply time `@0x42fd4c..0x42fd8f`, a 0x46 removal deactivates + wipes the slot `@0x434730/@0x4346c0`; the second row u16 is a STATUS BITFIELD not a ping `@0x42fdb4`, the fourth is accumulated points/EXP `@0x52C8E0`, the team-row bytes are kothHold/ctfFlag `@0x50dc62/@0x50dd30`. panel: `HUD_DrawKillList @0x423A30` + the header block `@0x423060` — stdbox (20,78)-(1004,550) `HUD_DrawLabelBox @0x423a90`, header rungs 105..185 stepping 0x14 `@0x42315c..0x42322a`, rows from base + 18 `@0x423d30` while y < 490 `@0x424168`, non-team modes (types 0/1/8) alternate x190/x690 with a SIGNED score, team modes column by team and draw only live-entity rows `@0x423d1b`, spectators at x440 with no score/rank `@0x423e04`, one GLOBAL rank counter `@0x42424f`, the neticon2.tga band `NetIcon_DrawConnectionQualityBand @0x4c2ee0`, the status-glyph append order `@0x423f29-0x4240e5`; the toggle `Scoreboard_TogglePlayerList @0x4244c0` from `@0x49bb68`, the drawer gate `HUD_DrawKillListIfVisible @0x424300`; the stdbox scale `s = surface_w / 1600` `@0x51f02e`, the fill cell extraction `@0x56adbd-0x56ae44` -> `stdbox_draw_fill_wrap_tiled @0x56b5d0` | **OPEN (partial).** One capability gap, not a missing witness: the eight border pieces bind border x boxtile as ONE combined material with a screen-anchored second stage `[orig: CGfxTexture_Create @0x56af3c, applied @0x56b902; draw_textured_quad_0 @0x56b3e0]`, so retail's pieces read camo where ours read plain stencil (needs a second texture stage the HUD quad stream does not carry). PORTED 2026-08-29: the C2S 0x22 unknown-row retry (reducer-queued slot ids, one reliable `{slot, 0x1CF7}` per dropped row framed by the joiner runtime `@0x42fc05..0x42fc3a`) and the 4-team page (`g_num_teams_config > 2 && dword_A87060 & 0x80` flips the team board to teams 3/4 with 0xFFFFFF00/0xFFFF027F every 128 HUD frames `@0x423cd0-0x423cf1`; the joiner's side count is the 0x16 team-table byte `@0x42fdda`). Unported tails: the per-mode team-score header block `@0x4232bf-0x423a12`; the per-recipient SU status gate `@0x423ef8`; host-side sessionvar strings; the host-side 0x16 serializer's `CPlayerStats` sources (`encode_player_list` emits zero status/score words, so an opennova-HOSTED board shows zero scores; retail-server joins are unaffected); the slot+0x20 label `@0x434870`; the same-team class suffix `@0x423d8a`; the KOTH countdown row `@0x423e7d`; the PgUp/PgDn page fold `@0x423c1c`. Full row text: the ledger's D-HUD-24 entry. |
| D-HUD-25 | **FIXED 2026-08-24.** The MP end-of-round presentation: both S2C 0x1D header forms decode (the non-team top-three names/scores form included) into the overlay ladder (`hud/end_round_overlay.h`, `HudFrameCompiler::element_end_round_overlay`, `EndRoundPresenter`); the S2C 0x56 stat board pulled over C2S 0x2B feeds the stat.mnu STAT screen (`engine/runtime/inmatch/stat_screen_feed.h`); the toggled Show Score statistics panel (`hud/end_round_statistics.h`; catalog row 99 `ShowScore`, F5, action 422) and the joiner's `g_round_time_remaining` fold are live; the stat.mnu exit is confirmed and player-initiated: HIDDEN_BACK's authored actions raise CONFIRM_EXIT and the CONFIRM_YES command exits the mission (`[orig: UI_StatConfirmExitCommand @0x562210]` — the same close-screens + action-3 pair as the pause menu's confirm; `EndRoundPresenter.exit_to_menu_requested` → the shell's return-to-menu teardown), while the round cycle's own transitions stay the host's | `EndRoundScoreboard_SerializeHeader @0x505280` sent from `Server_ProcessRoundEnd @0x516839`; the non-team form `@0x43086c..0x430883` staged into `byte_A81B40/60/80` `@0x430889..0x4309af`; the ladder `draw_endround_stats_overlay @0x5b7cd0`; `populate_stat_results_list @0x562240`; `HUD_DrawEndRoundStatistics @0x5b7600` behind `g_showEndRoundStatistics @0x24C18AC`; the 0x0A sub-block 1 host projection `@0x4ffa81..0x4ffaca`; the post-STAT once-only latch `@0x5b864a`; the host's linger-expiry mission exit, reason 3 `@0x51db63` | Closed on the ledger's 2026-08-24 closure line; the full transaction is net-re §5.68 (the 0x56 chunk pull) plus the 0x1D / 0x56 catalog rows. One recorded residual rides the npwire protocol-cursor contract: the decoder REJECTS a short stream where retail zero-fills. |
| D-HUD-26 | **FIXED 2026-09-19.** Scoped + FLAGS2 Inset uses a separate scene viewport, aperture/ring/cross, friendly label, slot offsets and a full second compose (its own shake step and, mounted, its own look-ahead step; the Inset offsets a copy of the composed view `@0x5C9846..0x5C9903`, corrected 2026-09-22 from "a third shake sample") | Definition `+0x0C & 0x200` `[orig: @0x5CA2B1..0x5CA2B4]`, scene `@0x5C9740..0x5CA0E1` | This is not the mortar view. Native geometry and live viewport lifecycle/declutter tests cover the port; mortar impact prediction/designator/map callbacks are separate. |
| D-HUD-27 | **FIXED 2026-09-16.** Rendered scope camera omitted the active slot offsets; scope range/elevation/magnification text was absent | Modern main-scene Sighted/Scoped camera branches `[orig: Render_ProcessMainSceneFrame @ 0x5ca452..0x5ca4a0]`; HUD text/gates `[orig: HUD_DrawScopeOverlayDetails @ 0x59e420]` | Camera consumer and typed HUD feed ported; standing/prone and text policy regressions pass. The flag-8 vehicle target reticle and D-HUD-26 Inset scene were added in the 2026-09-19 follow-up. |
| D-HUD-28 | **FIXED 2026-09-19.** Missing seat-specific HUD dispatch and mounted stance; vehicle proxy for Inset reticle; repeated capacity-one ammo folding for flash | `HUD_RenderOverlays @0x5A7CBE..0x5A7D55`; `HUD_BuildEntityInfo @0x4B8440` (seat switch `@0x4B863D..0x4B8767`, EmplacedStance override `@0x4B8539..0x4B8549`; the carrier-is-a-vehicle leg `@0x4B84D1..0x4B8507` is DEAD because `HUD_RenderAllOverlays` zeroes the struct `@0x5A80A5..0x5A80B1` first, so a gunner always reads Emplaced); Inset `@0x4DCCB0` (called `@0x592AE5`); centred cues slide on-screen and recolour `draw_textured_quad_centered @0x5909E0`; flash `@0x599A30` | Native seat/view and real presenter regressions pass; [mode matrix and remaining gaps](weapon-vehicle-hud-validation.md). |
| D-HUD-29 | Pointer row: owned by the [tank record](../world/tank-parity-re.md#divergence-catalog). Live HUD observation recomposed the camera and advanced shake/drift state; the HUD consumes the displayed snapshot, and every non-rendering reader observes the last composed view. | `Camera_ComputeThirdPersonView @0x437D10` callers `@0x526781` / `@0x5CA34D` / `@0x5C9841` | Minted-and-closed 2026-09-22 (FIXED) in PR #671; GUT `game_hud_presenter_declutter_test.gd` (`test_hud_uses_the_presented_camera_frame`). |
| D-HUD-30 | **Minted-and-closed 2026-09-22 (FIXED).** Scope and mortar Distance labels left the installed `%ld` conversion literal | Both callers pass the localized `STROVER_DIST` and one integer to `sprintf`: scope `@ 0x59E530` (the floor arm's `jmp` `@ 0x59E504` joins it); impact `@ 0x5A8972`; the no-argument labels `@ 0x59E4D5` / `@ 0x59E938` | `hud_sprintf` runs each HUD template with CRT sprintf semantics and its call's own argument list (the whole integer specification, `l`/`h`/`I32` prefixes reduced to 32/16 bits, `%%`); glyph-output regressions cover both callers, the no-argument collapse and the 1000m boundary. |

## Follow-ups (not yet witnessed / deferred)

- **Zone status panel residuals** (2026-08-21): the capture bar's FPU vertex
  positions and the count-differential arrows `@0x598dc0..0x598fe4`; the
  letter font slot `dword_2723C74`; the conquest arm (`g_GameType ==
  0x50010`). (No declutter bit — the `@0x5a8530` call is unconditional.)
- **Recent Messages window**: `HUD_DrawOverlayPanels @0x5c0060` is a third, unwitnessed
  reader of the `OldMessages` toggle.
- **Chat**: channel 13's `HUD_SetTrackedEntityTarget(sender)` (a world leg);
  the centre announce banner (D-HUD-6). The C2S chat SENDER is unported, and
  its gate is witnessed (2026-08-25) for when it lands: `Chat_SendGlobalMessage
  @0x49a6b0` sends only when `(!g_death_screen_active || g_spawn_success_gate)
  && message[0]` (a dead player who has never spawned cannot send), after
  `Chat_StripHtmlTags @0x4983f0` and `Chat_CheckFloodControl @0x498f60` (a
  17-entry ring of the last texts, 68 bytes each: the SAME text within 0x500
  ticks is refused and echoed locally through `Chat_AddMessageChannel1` with
  `dword_24C183C` instead of sent; the team/squad/admin/all senders share both
  callees). There is no client-side "muted" gate — muting is the host's
  (`Server_SendValidatedChatToPlayer @0x50a210`).
- **Vehicle panel**: the `icon` / `statictexture` VEHICLE_HUD strings have no
  witnessed drawer (only the `interface` silhouette draws).

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
### The message feeds (`HUD_DrawConsoleMessages @ 0x59ad30`)

One routine paints BOTH message channels — the player-chat ring first, then
the system ring — behind ONE shared gate: the CHAT declutter mask bit
`[orig: @0x59AD33]` plus the hard `hud detail level >= 2` cull
`[orig: @0x59AD43]`. Witnessed and ported (D-HUD-23):

* **Only three ring rows are walked per channel** `[orig: the chat walk
  @0x59adbc..0x59ae2e from 0xB3FFB4; the system walk @0x59ae5e..0x59aebf
  stepping -0x80 from 0xB427BC]`, and because the sink puts the newest line in
  slot 0, the OLDEST of the three sits at the anchor with each newer line
  **18 design px BELOW** it `[orig: the 0x12 store @0x59ad97, scaled through
  Viewport_ScaleToVirtualCoords @0x5d2b20]` — the feed grows downward. The
  ≥186-tick expiry stagger keeps recency and expiry in the same order, so
  "the newest three slots, live only" equals the last three live lines.
* **The alpha ramp belongs to the CHAT ring only.** Both loops compute
  `clamp(timer * 255 / 186)` as the liveness test, but the system draw passes
  the STORED color `[orig: @0x59ae97]`; only the chat path folds the computed
  alpha over the stored RGB `[orig: @0x59adef]`. A system line therefore holds
  its color for its whole 930-tick life and vanishes, while a chat line fades
  over its last 186 ticks.
* **The anchors differ**: the system ring sits at `HUDSYSTEXT` (parsed into
  `HudposFile.hud.sys_text`; parser stores `@0x5a0a18`), the chat ring at
  `HUDCHATTEXT` (`@0x5a09d4`).
* **`Chat_AddMessageChannel2 @0x4987f0` is the SYSTEM ring's one sink** — the
  0x1E feed lines and mission triggered text
  `[orig: HUD_DisplayTriggeredText @0x51f190 -> the (text, -1, 930) post
  @0x51f216]` all land in the same ring and interleave by arrival. The reimpl
  merges them the same way (`push_message` forwards into the feed ring).
* **The CHAT ring is ported too (2026-08-21).** Its writer
  `[orig: Chat_AddMessageChannel1 @0x4985d0]` keeps a raw 40×128-byte ring
  (`byte_B3EA38`, slot 0 newest, colour +120, timer +124) AND a 41×128-byte
  DISPLAY buffer (`byte_B3FDBC`, slots 1..40) of font-wrapped lines: the
  newest message's LAST line sits in slot 1, its continuation lines above it
  prefixed two spaces, the wrap width `box[2] − (box[1] − 4)` from
  `g_hudChatBoxCoords` (`HUD_GetChatBoxCoord @0x5bbe90` — the `hud.def`
  `chat_message x1 y1 x2 y2` / `sys_message` table, zero in retail, which
  ships no `hud.def`: see Chat channel geometry below; the wrapper is
  `HUD_WordWrapText` over `CGameFont_GetCharExtent`, measuring the RAW font object,
  not the label slot; the per-byte extent is
  `floor(((u1 - u0) * 256 + glyph_spacing - 1) * (800 / design_width) + 0.5)`
  for bytes >= 0x20 (a tab = the font's `this+0x168` tab width when nonzero,
  else the SPACE glyph; control bytes 0; no scale argument)
  `[orig: CGameFont_GetCharExtent @0x674dc0 @0x674de4..0x674e25]`, and the
  walk adds +1 per byte on top `@0x5809e4` while the fit test measures the
  whole remainder through `CGameFont_GetTextExtent @0x675290`
  (= `CGameFont_MeasureText` at scale 1, which strips one trailing pad); port
  `GameFont::char_width` + `chat_wrap_text`, ported 2026-09-12 with the pad);
  only display
  slot 1's timer is written, `max(930, slot2.timer + 186)`, the continuation
  slots carry timer 0 — so the feed (first loop `[orig: display slots 3,2,1
  top→bottom, alpha = min(255, 255·timer/186), a slot with timer ≤ 0 skipped
  WITHOUT advancing the row]`) shows only a wrapped message's last line. The
  timers of all 40 slots of the four buffers decrement (floor 0) once per tick
  `[orig: Input_DecrementCooldownTimers @0x498440 from Game_ProcessMainFrame
  @0x5267a6]`. The sink is the S2C 0x14 fold (`engine/runtime/replication/
  client_replica_feed.cpp` → `ClientChatLine{channel, sender_slot, text}`)
  routed by `[orig: Chat_DispatchToChannel @0x42b910]` — NO local-team colour
  term: 0 and ≥ 15 → the SYSTEM ring white; 1/4/5 → CHAT `g_hudColorTable[3]`;
  2 → [1]; 3 → [4]; 6/10 → [0]; 7 → [7]; 9 → [5]; 11 → [10]; 12 → [6]; 13 →
  [0] + `HUD_SetTrackedEntityTarget(sender)` (unported world leg); 8 → the
  message queue (no ring); 14 → `Chat_AddMessageChannel3` (a third ring, never
  drawn). Ported as `HudFrameCompiler::push_chat_line` + `chat_wrap_text`,
  `hud::chat_channel_sink/color`, and the HUDCHATTEXT first loop of
  `element_feed`; pinned by ctest `hud_frame_compiler` + `client_replica_chat`.

The line content is never composed by the client: the 0x1E handler picks a
"Canned Msg" template and `Chat_FormatMessage @0x422C60` substitutes `$A`
(attacker) then `$B` (victim) — two sequential case-insensitive replace-all
walks `[orig: String_ReplaceAllCaseInsensitive @0x422970; the scan resumes
after each inserted name @0x422ADD]`. `HUD_FormatKillEventMessage @0x422DA0`
resolves each name from the player-slot table with the clan tag appended as
`<ch>tag<co>` `[orig: @0x422E1D..0x422E64]`, falls back to the entity name,
and formats a NULL actor as `Client/STRCLI01` ("Unknown")
`[orig: @0x422DDA/@0x422E91]`. When the AUX slot resolves to the LOCAL player,
the template is first re-composed through `STRCND48` ("%s - Bonus for %s")
with their name `[orig: the extra gate @0x422C84 -> sprintf @0x422CA2]`.

The per-case color/policy table of the `@0x426270` switch (jump table
`@0x427BB8`), all ported in `hud::feed_format`:

| types | policy | color |
|---|---|---|
| 1/2/3 (suicide), 4/5/6 (kill), 10/11/12 (vehicle), 13/14/15 (knife) | own white / other grey; the grey branch returns unless `g_MpVerbose2` `[orig: 13 test sites @0x426472..@0x4267C6 over 15 gated types — 10/11/12 funnel through ONE shared test @0x42664A; 39/45 jump to their canned post before any gate @0x426454/@0x426468]` | `-1` / `-5263441` (0xFFAFAFAF) |
| 7/8/9 (friendly fire) | always posts | `-1` white |
| 16/17/18 | always posts | `g_hudColorTable[0]` = `-1` `[orig: HUD_InitTeamColorTable @0x51F245]` |
| 19/20/21 | team/gametype-keyed strings, positional/interface sound cues and delayed interface voices; the progress-bar label is refuted below | `g_hudColorTable[0]` (gametype 65544 team literals in 20) |
| 22/23 ("$A is dead."), 25/26 | own white / other grey, NO verbose gate | `-1` / `-5263441` |
| 24 (explosive), 49 | own white / other grey, NO verbose gate | `-1` / `-5263441` |
| 27-31, 35, 36, 37 | always posts | `-1` white |
| 32/33/34 (multi-kill bonus) | verbose-gated for the uninvolved | `-256` yellow `[orig: @0x4266C5]` |
| 38/39/45 (medic trio) | always posts; ONE shared sink call | `-16741138` (0xFF008CEE) |
| 40 | always posts | `-65536` red `[orig: @0x4263E8]` |
| 41-44 (PSP), 54-57 (LFP) | posts + team sounds/effect spawns | BLUE `-16732161` (0xFF00AFFF) / RED `-65536` `[orig: e.g. @0x427519/@0x427717]` |
| 46/47 (SSKB, runtime-keyed) | posts | `0xFF008CEE` `[orig: @0x427A68/@0x427AD8]` |
| 48 (mortar designation) | always posts | `-32768` orange `[orig: @0x427AE5]` |
| 50/51/52/53 (LFP results) | **format-and-return — never posted** `[orig: @0x42702E-@0x42716D]` | — |
| 58 | tip system only `[orig: @0x427202 -> CTipSystem_HandleEvent 17 @0x427286]` | — |
| 59/60 (camp) | team 1/2 only (no else branch `@0x4272F4/@0x427338`); `sprintf(tmpl, WPNames["STRWPNAME%03d" % (level+1)])` `[orig: @0x4272EC/@0x427327]` + team sounds/tips | team 1 `-16732161` / team 2 `-65536` |

`g_MpVerbose2 @0x24D2154` is seeded verbose-on from the session settings
`[orig: apply_session_settings_to_globals @0x551D0F]` and flipped by the
keybind that announces `STRMISC_VERBOSE_ON/OFF` `[orig: @0x49B78F]`; the
reimpl holds the seed default (the toggle is unported).

Two classification corrections landed with the port: the medic trio (38/39/45
— 39 has no emitter in the image but shares the handler path) is NOT a kill,
and the killer-less deaths (1/2/3 suicide, 22/23/25/26) are their own class
whose victim/aux slots are LITERAL ZERO on the wire `[orig:
GameEvent_PlayerDeath @0x516DD0 zeroes v36/v37 for 22/23/26 and leaves
v41/v42 = 0 for the suicides]` — reading them charges the death to entity 0
(the host). The same emitter shows the type families: suicide `rand(0-2)+1`,
standard kill `+4`, friendly fire `+7`, vehicle `+10`, knife `+13`,
multi-kill bonus `+32`, explosive-weapon `24`, drowned/crashed/environment
`22/23/26`.

An involved line (attacker or victim is the local player) is ALSO copied into
`g_killAnnounceText` with a tick stamp `[orig: the shared sink call @0x427B71,
then strncpy @0x427B8B + the g_killAnnounceTick stamp @0x427B96]`, drawn
centred at x=512 in the large HUD font for 186 ticks by the centre announce
banner `[orig: HUD_DrawKillAnnounceBanner @0x59dc90]` — banner unported
(D-HUD-6 residual).

- **Chat channel geometry** — RESOLVED 2026-08-21, the writer found in the
  final review: `g_hudChatBoxCoords @0x28e4df8` (ex `dword_28E4DF8`; rows 1/2
  chat, 3/4 system) is read through `HUD_GetChatBoxCoord @0x5bbe90` and
  WRITTEN by the `hud.def` parser — `File_ParseASCIIFile("hud.def", cb,
  0x2A5A8EAD)` `@0x5be210..0x5be228`, callback `@0x5bb7a0`, the `chat_message
  x1 y1 x2 y2` / `sys_message x1 y1 x2 y2` stores `@0x5bb7d1/@0x5bb7ed/
  @0x5bb825/@0x5bb841` (`dword_28E51FC` is `chat_message`'s y1 — the earlier
  "no writer in the image" / "never authored" readings were wrong). No shipped
  title carries a `hud.def` (JO:CA's resource/localres/language pffs, the JOX
  expansion assets and the JOTAC root were all checked), so the table stays
  zero and retail never wraps a chat line; the port leaves
  `HudLayout::chat_box_*` unset and does not wrap either (the review removed an
  earlier hudpos-`HUDCHATTEXT` stand-in that wrapped where retail does not). A
  `hud.def`-equipped mod would need a `formats/def` reader — the named residual
  (no D-row); the path is fully witnessed for it: `File_ParseASCIIFile
  @0x53d810` reads the whole file and decrypts in place ONLY when the caller's
  key is nonzero AND the first four bytes are `'S','C','R',0x01` (`@0x53d899`;
  `Scr_DecryptBuffer @0x53d090` = our `scr_decrypt`; any other leading bytes
  parse as plain text), splits on CR LF only (`@0x53d8de`), tokenizes each line
  with `Terrain_TokenizeConfigLine @0x53cb60` (space/comma/tab separators
  outside `"`, `//` or `;` ends the line, max 30 tokens, 1000-char clamp) and
  delivers non-empty lines whose first token does not start with `/`; the
  callback `@0x5bb7a0` matches `token[1]` case-insensitively and `atol`s the
  values into two parallel dword tables (X `@0x28E4DF8+4·row`, Y
  `@0x28E51F8+4·row`): `chat_message` → rows 1/2, `sys_message` → rows 3/4,
  plus two-value rows `lower_plate 0x1C, gun_icon 0x1D, waypoint_elevation 0x1A,
  waypoint_range 0x1B, weapon_name 0x1F, weapon_clips 0x20, weapon_rounds 0x21,
  position_icon 0x22, position 0x23, waypoint_name 0x24, extra1..4 0x25..0x28,
  map_center 0x29, map_radius 0x2A, cargo_icon 0x2B, cargo 0x2C` and four-value
  `health_bar 0x2D/0x2E`, `mana_bar 0x2F/0x30`. Only the X table's rows 1..4 are
  ever read (`HUD_GetChatBoxCoord @0x5bbe90` from the two message sinks, channel
  3 and the display rebuild) and only Y[1] (`dword_28E51FC`), which
  `HUD_DrawMessageLog` adds in screen px to the Recent Messages BOX top/bottom
  (`@0x5b9df2/@0x5b9e1f`, not to the text rows) and `HUD_DrawClassRosterOverlay
  @0x5b9f8e` reads; every other `hud.def` key is parsed and never consumed by
  the JO HUD.
- **Crosshair color config** — D-HUD-8 is closed after correcting the
  vertex field interpretation; the `dword_25510E0` default is witnessed
  (`[orig: Config_SetDefaults @0x54d461]` = 0xFFFFFF) and the style write is
  `sprintf("cross%02d.tga", selected_value + 1)`
  `[orig: ingame_options_dialog_event_handler @0x5551f4]`.
- **Crosshair sub-elements (ported 2026-09-19)** — the target-tracking cursor
  (`@0x592790..0x592875`), the `weapondef+12 & 0x80` aim-point quad
  (`@0x592973..0x592ac8`), and the lock brackets (`@0x592ce2..0x592dd7`);
  witnessed 2026-07-11 and implemented in the 2026-09-19 follow-up.
- **`dword_24C1930` flag 0x10000** — replaces triggered/gametext strings with
  `"&"` (`@0x51f1c8`/`@0x51ebe3`); the writer is unwitnessed.
- **Scope overlay** — the scope view's reticle/mask (`scopexh.tga @0x59e133`,
  weapon sights), which replaces the HUD crosshair when scoped.
- **`entity+885` (the radio-request latch the tag icon reads)** — WITNESSED
  and ported 2026-09-12: the S2C 0x6D handler writes +885 (event 6 → 1) and
  +886 (30) `[orig: NapiNPClientMsg_HandleEntityDeath @0x430C50]`; the tag
  icon consumer is the D-HUD-20 element-map entry (its feeds are ported;
  see that entry). Net-re's sector-action 30-tick timer at the same
  offset is the pool-3 (item) use of the byte.
- **CGameFont glyph layout** `[orig: CGameFont_DrawText @0x6752c0]` — per-glyph
  D3D vertex build / spacing, to confirm `FntResource` layout parity.
- **Weapon slot bar** — `HUD_DrawWeaponSlotBar @0x599CD0` is witnessed;
  the optional authored slot-selection strip remains separate from this
  weapon/vehicle HUD pass. The aircraft AGL ladder/readout is now ported
  (`@0x59F050`); its other input fields gate admission rather than drawing
  separate power/velocity instruments. The ex-"reload bar" is the ported
  PowerThrow charge bar (world-wac-ai-re §27.3).
- **MP objective status + stance team tile** — witnessed
  (`HUD_DrawTeamIdLine @0x59aa30`, `@0x59a0cb`); port with the MP HUD.
- **Parachute/armor icons** — ported 2026-09-19; witnessed
  (`HUD_DrawParachuteAndArmorIcons @0x5925c0`): entity+44 `&0x10` parachute /
  `&0x8` armor (through the info struct's entity pointer; the +44 writer is
  unwalked — the earlier "+36" note was a recon guess, corrected 2026-07-18).
- **Armory/FARP prompts** — the S2C 0x0A preround, reload timer and zone
  mask now feed the bottom-prompt cluster. D-HUD-14 retains menu/gameplay
  service integration and the floating armory-delay label variant.

### The Recent Messages window — `HUD_DrawMessageLog @0x5b9d70` (witnessed + ported 2026-08-21)

The J-key history panel behind the `OldMessages` action (catalog action 56,
default VK 0x4A; `g_showMessageLog @0x24c18c0` is toggled `xor … , 1`
`@0x49b55a` in `Input_HandleActionBinding` (jumptable case 29) and cleared by
`Game_InitRespawnState @0x49939a`). The IDB had it as `draw_credits_scroll`
with a "NOT network-related, name confirmed" comment; the body walks the two
message rings, and it was renamed 2026-08-21.

- Called from `Server_DrawStatusScreen @0x50a2d0` (the call `@0x50b211..
  0x50b21f`, gated on `g_showMessageLog` ONLY — no `hud_detail` or declutter
  test; drawn after every HUD element and before `HUD_DrawClassRosterOverlay
  @0x50b23d` / the Tab board `HUD_DrawPlayerScoreList @0x50b281`; that caller's
  name is a standing proposal) and `HUD_DrawOverlayPanels @0x5c0060` (a third reader of
  the toggle, unwitnessed).
- It reads the DISPLAY buffers (the font-wrapped lines — chat `byte_B3FDBC`,
  system at `+0x2808`), not the raw rings: rows = display slots 16..1
  top→bottom (slot 0 never drawn), continuation lines carry timer 0 and are
  listed all the same.
- One `HUD_DrawLabelBox` stdbox titled `Overlays/STROVER43` from design x 8 to
  1016 `@0x5b9e1e/22`, top `120·w/1024` `@0x5b9da3..0x5b9ded` minus 0x20
  `@0x5b9e42`, bottom = top + 16 line steps plus 0x20 `@0x5b9e46`; both
  corners pushed through `Viewport_ScreenToVirtual @0x5d2c70`.
- The line step is `12·w/640` `@0x5b9dab` while the columns shift by 10
  (`32·w/1024 + 2` `@0x5b9dbc..0x5b9e7e` left, `990·w/1024` right-aligned
  `@0x5b9ec0..0x5b9f02`) — the mixed denominators are witnessed.
- Sixteen rows per column, walked HIGH → LOW (`entry −= 128` from
  `unk_B405BC` down to `byte_B3FDBC` `@0x5b9e8a..0x5b9f1a`): the chat ring
  (writer `Chat_AddMessageChannel1 @0x498750`) in the left column and the
  system ring at `+0x2808` in the right, each entry's colour at `+120`, with
  NO expiry gate — the oldest shown line is the top row and a short history
  leaves the TOP rows blank.
- Two terms are zero in retail and not carried: `fixedZ @0x24c18f4`
  (provably — single writer `Renderer_SetDisplayModeWithFallback @0x587622`,
  edi zeroed `@0x58761a`) and the chat-box table's `dword_28E51FC`
  (`chat_message`'s y1, written only by the `hud.def` parser `@0x5bb7a0`;
  JO:CA ships no `hud.def`).
- **Port** (2026-08-21): `hud_message_log.h` (layout policy +
  `message_log_row_source`, ctest `hud_message_log`) + `hud_frame_message_log.cpp`
  (`element_message_log`: the titled stdbox, 16 display slots per column, the
  screen-px rows, NO expiry gate — a line past its 930-tick life is still
  listed; walk order after the feed, before the Tab board; ctest
  `hud_frame_compiler`); the `OldMessages` toggle lane `message_log_presenter.gd`
  (`ControlsBindings.pressed("OldMessages")` edge latch, cleared on respawn as
  `Game_InitRespawnState @0x49939a` does) and `HudOverlay::set_message_log_shown`
  landed 2026-08-21 (D-HUD).

## IDB changes

Applied 2026-08-21 (the wire-up round — the witness pass that closed the
staged HUD modules; IDB saved):

- **Renames** (anchored: bodies read): `sub_546680` →
  `Entity_GetMountSlotBoneIndex` (the attached child's gun-slot bone,
  cached +0x319), `sub_5BBE90` → `HUD_GetChatBoxCoord`, `dword_28E4DF8` →
  `g_hudChatBoxCoords` (the `hud.def` `chat_message`/`sys_message` x1 y1 x2
  y2 table — rows 1/2 chat, 3/4 system — written by the parser callback
  `@0x5bb7a0`; the "HUDCHATTEXT/HUDSYSTEXT rows" gloss was the port's
  stand-in).
- **Comment corrected by the orchestrator** (the final review, 2026-08-21):
  the entry comment on `HUD_DrawZoneStatusPanel @0x5a2480` read "y + 12 +
  86*group" and "(x, y−4)" — both wrong: the markers sit at `g_hudZonePanelY
  + 86·group` and only the status text is at `(x − 4, y + 12)` (`ebx = y +
  0Ch` `@0x5a25b9..0x5a25bd`).
- **Comments** (`[opennova 2026-08-21 …]` entry comments + `reimpl:` links):
  `Entity_BuildWeaponSlotList @0x434c60` (the list order + the digit rule),
  `Chat_AddMessageChannel1 @0x4985d0` (raw ring vs display buffer, the slot-1
  timer), `Chat_DispatchToChannel @0x42b910` (the channel table, no team
  term), `HUD_DrawConsoleMessages @0x59ad30` (the display-slot walk),
  `HUD_DrawMessageLog @0x5b9d70` (display-buffer source, the `g_showMessageLog`
  gate), `Input_DecrementCooldownTimers @0x498440`, `ZoneTimerList_SetEntryValue
  @0x537ec0` (the entry map), `CharAttr_LoadFromDef @0x412140` (the 31-dword
  records, Medic 0x8), `HUD_DrawVehicleHealthBars @0x5a4fd0` (the `"%1d"`
  labels), `HUD_DrawZoneMarker @0x5986f0` (the sheets + atlas rows). Proposed,
  not applied: `off_7D8E64` → `aFmt1d` (it is the "%1d" string, not a pointer).
- **Unwitnessable, recorded**: the writer of `rootEntity @0x27235bc` (all five
  xrefs read; the panel's only witnessed gate is the interface-texture test).

Applied 2026-08-21 (the post-merge tidy of #536..#552; IDB saved):

- **Renames** (anchored: bodies read): `draw_credits_scroll @0x5b9d70` →
  `HUD_DrawMessageLog` (the ring walk, not credits — the old "name confirmed"
  comment was wrong); `draw_capture_point_status_overlays @0x5a2480` →
  `HUD_DrawZoneStatusPanel`; `draw_capture_point_detail_panel @0x5986f0` →
  `HUD_DrawZoneMarker`, with its prototype corrected from 0 args to
  `int __cdecl (int x, int y, int zoneEntity, int letterIndex)` (four pushes
  at both call sites). `HUD_DrawMedicCrossQuad @0x59bcb0` KEPT — the body is
  the medic cross; the 2026-08-21 "target bracket" reading was wrong.
- **Data names**: `g_showMessageLog @0x24c18c0`, `g_hudVehSeatMarkerW/H
  @0x27237fc/@0x2723800`, `g_hudZonePanelX/Y @0x2723d94/@0x2723d98` (the
  `LFP_FLAGS` token), `g_hudZoneStatusKind/Color @0x2721db4/@0x2721db0`,
  `g_stanceColorGood @0x2723adc`.
- **Comments**: `Game_TickHudFrameCounters @0x434c00` (the stale "death
  counters" comment replaced — it is the HUD blink clock), entry comments +
  `reimpl:` links on `HUD_DrawVehicleHealthBars @0x5a4fd0`,
  `HUD_ParseHudposToken @0x59f370` (first comment; VEHICLE_HUD/VEHICLE_END
  marked `@0x59f380`), the three renamed functions, `HUD_DrawMedicCrossQuad`
  (its three callers), `CEffect_Begin_Debug @0x67bb50` (thunk →
  `draw_tiled_texture_strip @0x67aed0`), `render_tiled_image_strip @0x67b540`
  (the row select `@0x67b5c2`), `Entity_BuildSpawnZoneList @0x43eae0`.


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
  the 64-segment circular scope mask (net-re §5.62). Re-witnessed
  2026-07-19, and **corrected 2026-09-16**: the `Render_ProcessMainSceneFrame
  @0x5cab15` caller is NOT a fallback — the mask draws on every Scoped frame and
  the SIGHTS-row count only forms the `draw_crosshair` argument that gates the
  inner cross/grid. The function comment carried the old fallback gloss and was
  rewritten this session. The second caller remains `render_hud_overlay
  @0x5d82f2`. The sibling reticle drawer `draw_minimap_crosshair_and_grid
  @0x5d1160` keeps its name (its second caller is `NVG_DrawScopedLens
  @0x5d27bc`, which IS a minimap element) and carries a candidate-rename
  comment for the next HUD grill.
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

Applied 2026-09-16 (the scoped-view circle-mask grill; comments only, IDB saved):

- **Comment** at `0x5d17a0` — replaces the old "drawn for Scoped weapons that
  author no SIGHTS rows" gloss with the refuting byte sequence (both callers
  compute `arg1 = (row count == 0)` and it only gates the tail
  `draw_minimap_crosshair_and_grid` call `@0x5d1cc9`), plus the full ring
  geometry, the two colours, the 130-vertex strip and the vertex stride.
- **Comment** at `0x5d1160` — the four tapered spokes and the sixteen tick
  diamonds with their fractions, ftol truncation, `W/320` / `W/64` units and
  colours; records that the second caller `NVG_DrawScopedLens
  @0x5d27bc` IS a minimap element, which is why the name stays.
- **Comment** at `0x59b0f0` — the 8-bit `shl`/`and cl,dl` arithmetic and the
  absence of any clamp on the level (cfg `@0x550339`, apply `@0x55154d`, cycle
  `@0x4E0608`).
- **Comment** at `0x843480` — the shipped-constant-1 scope-treatment feature
  switch, its three readers and the dead zero arm.

No renames were applied: `draw_minimap_crosshair_and_grid` keeps its name now
that its minimap caller is witnessed, and the 2026-07-19 held proposals for
`0x4dcd30` / `0x4dcce0` remain held.

Applied 2026-09-19 (the #655 leftovers, comments only): the slot each combat
text pushes (`0x59A6F9`, `0x5BDFD7`, `0x5CA0C0`, `0x5A897E`), the ring
record's x-scale (`0x59327B`), the Inset scene call's pass coverage
(`0x5C9DE9`) and the ungated tail of the overlay walk (`0x5A87EF`). The
comment at `0x59A6F9` also records that the jo-c sync's name for the label
font slot table, `g_NetQualityIndicators @0xB4C388`, is a misnomer: the table
is normal `+0`, bold `+0xC`, large `+0x18`, Impact38 `+0x24`. It was not
renamed back, since the sync would reapply it.

## Ledger de-table transplants (2026-08-06)

Closed ledger rows whose full text previously lived only in the divergence
ledger, transplanted verbatim at the 2026-08-06 compaction (Standing rule 6).

- **D-HUD-10** [FIXED 2026-07-11 (weapon round: `aim_screen_point()` = INF in 1P -> the HUD pins the exact center; the 3P projection uses the witnessed 1000.0 far point)] Crosshair anchors at the fixed design center — the original anchors at the projected aim point (screen center only on-foot first-person `@ 0x5928a0`; spectate/`g_camera_mode` project `Entity_BuildCameraView` `@ 0x592910`)


## #645 follow-ups: flag feed, announcement banner, and death instructions (2026-09-11)

**WITNESSED / PORTED.** The feed and deploy policies live in portable
hud/feed_format and world/deploy_screen_feed; ClientState retains the
packet facts and last involved kill text. The Godot presenter resolves the
game text and applies the native text/show results.

NetPacket_HandleGameEvent @0x426270 selects these runtime keys:

| Event | Game/team condition | Canned Msg key |
| --- | --- | --- |
| 19 | type 8 | STRCND49 |
| 19 | other type, actor team 1/2/3/4 | STRCND14/13/25/26 |
| 20 | type 8 or 65544 | STRCND27 |
| 20 | other type, actor team 1/2 | STRCND16/15 |
| 21 | type 8 | STRCND50 |
| 21 | other type, actor team 1/2 | STRCND18/17 |
| 46 | signed wire X <=1 / >1 | STRCND_SSKB1 / STRCND_SSKBX |
| 47 | signed wire X <=1 / >1 | STRCND_YRSSKB1 / STRCND_YRSSKBX |

Other team values in the team-key branches produce no line. Type 20 in game
65544 uses blue 0xFF00AFFF, red 0xFFFF0000, yellow 0xFFFFFF00, or
0xFFFF027F for actor teams 1–4. Both SSKB lines use 0xFF008CEE. Event 46
substitutes actor name and signed decimal count; event 47 substitutes the
count for $A and the empty string for $B. These do not use the ordinary
aux-player bonus composition. [orig: NetPacket_HandleGameEvent @0x426270;
sub_422D00 @0x422D00]

**REFUTED IDB labels:** HUD_DrawDefaultProgressBar @0x527E60 wraps
Sound_Play3DPositional, with null entity and volume 255.
EffectSlot_AllocateAndInit @0x527C30 allocates a pending sound; flags mask 2
selects interface playback. Server_TrackEntityInTable @0x527B30 is a
32-entry duplicate-sound suppression table. These flag-event paths draw no
progress bar or particle effect.

The fixed sound names come from the trigger table at 0x82F590, resolved
by DialogSystem_Init @0x5275E0. Local actors play interface cues; others
play positional cues at signed whole-unit wire X/Y and **Z=0**. The team
branch requires game type bit 0x10000 and matching actor/local teams.
Event 19 selects DO versus WIN families with the exact 65540/65544/8
branches (including teammate FLAG_WIN_T with FLAG_VXDO_T in 65540).
Event 20 selects PU cues for every mode but VXPU voices only in 65540.
Event 21 selects SV/VXSV for every mode. Voice names are reserved for
3720 ticks, then allocated into the shared 128-slot pending pool with a
62-tick delay and interface flag. A full pending pool still consumes the
suppression reservation. Countdown **old value 1** releases both tables;
zero allocation delay seeds one. Missing sound sets reserve nothing.
[orig: NetPacket_HandleGameEvent @0x426270; Server_TrackEntityInTable
@0x527B30; sub_5292D0 @0x5292D0; Sound_TickPendingSlots @0x529310]

Event 19 for the local actor also selects the nearest eligible enemy base,
retaining the existing selection if the mode/scan produces no candidate.
The new-round caller separately clears the selection before using the same
scan. [orig: SpawnPoint_FindNearestEnemyBasePoint @0x4DD290;
Game_InitNewRound @0x422740]

The involved kill/death cases (1–18, 22–26, 32–34, 49) copy at most 255
bytes into the announcement buffer and stamp the HUD clock.
HUD_DrawKillAnnounceBanner @0x59DC90 draws it in the large white font,
centered at virtual X=512 and Y=30, while the nonzero stamp has signed
age <=186. Expiration clears the stamp and keeps the text. The panel pass
calls it after the other panels, independently of gameplay HUD declutter.
The font's existing -0.5 pixel vertex offset still applies.
[orig: HUD_DrawOverlayPanels @0x5C0060]

UI_UpdateDeathScreenContent @0x5536A0 now supplies both instruction statics:

- Permanent death (0x08 flags 0x8000) with a dead local body shows
  STROVER_PERMANENTDEATH and either STROVER_SPECTATORSPAWN or
  STROVER_NORESPAWN (0x08 flags 0x2000). It returns before the ordinary
  list/medic refresh. Nonnegative remaining round ticks show STROVER50 plus
  red hours:mm:ss using /62, and Client/STRCLI25 plus the remaining
  player count; a negative time hides both status widgets.
- The ordinary first instruction is hidden for
  (game_type & 0xFFFDFFFF)==0x10020. If 0x0F game_flags bit1 is set and
  a team-owned registered spawn has an actual timer entry with word 9
  >=word 10, the first instruction retains its old text and the second
  is hidden. The intermediate RESPawn2 write is overwritten by RESPawn1.
- Otherwise the first text is the retained kill announcement when dead.
  Alive players see INITIALSPAWN with their team name in non-Co-op team
  modes, or WELCOMESPAWN otherwise. Player names retain the
  &lt;ch&gt;clan&lt;co&gt; suffix, capped at 255 bytes.
- Any registered spawn selects RESPawn1 for instruction two. Without one,
  neutral players get SPECTATORSPAWN; other modes get RESPawn3; the
  0x10020 family gets RESPawn4 for Co-op/alive and RESPawn5 for dead
  non-Co-op. sub_43B910 @0x43B910 tests zone count, not player count.

**REFUTED global name:** g_scoreboardDeadRowCount @0xA85B44 counts
**living nonspectating players**, only in permanent-death mode and at
accepted 0x16 row parse time. Dead bodies and spectator-bit rows do not
increment it. Unknown roster rows are dropped before the count.
[orig: NapiNPClientMsg_PlayerList @0x42FAE0, increment @0x42FD2A;
NapiNPClientMsg_HandleSessionConfig @0x4281D0]

Validation: native feed_format, hud_frame_compiler, deploy_screen_feed,
client_replica_death, client_replica_scoreboard, fire_sound,
npruntime_client_runtime, and waypoint_track; GUT real presenter runs
deploy_screen_presenter (9 tests, 327 assertions) and
game_hud_presenter_declutter (5 tests, 41 assertions).
Malformed actor/entity bindings safely omit sound/count contributions;
retail's unchecked/null dereferences are not reproduced. The already
tracked map image, other feed-event side effects, roster join/leave text,
and verbose-toggle residues remain separate.

## Local damage feedback (fullscreen flashes)

Witnessed 2026-09-16 against retail `Jointops.exe` (IDB `Jointops.exe.kong.i64`).
Three per-tick words drive three viewport-filling quads at the very end of the
scene frame, and one of them blanks the whole HUD overlay pass while it burns.
They sit beside the camera-shake counter `dword_B764B0` in both memory and code:
all four are zeroed together by the local respawn and decayed together in one
instruction run.

### The words

| word | meaning | arm | per-tick decay (`Player_UpdatePerFrame @0x4DE5A7..0x4DE5F7`) |
|---|---|---|---|
| `dword_B764B4` | RED damage vignette | `+= 120`, cap 255, in `Player_OnDamageReceived @0x4dd88f..0x4dd896`; the joiner's own S2C 0x0A tail health DROP adds the same `0x78` inline `@0x4305a3..0x4305bb` | `if (v > 1) v -= 2; else v = 0;` `@0x4DE5BF..0x4DE5D0` |
| `dword_B764B8` | WHITE hit flash | `= 255` from BOTH legs of `Entity_ApplyCollisionForce`'s kz_physics-3 blackout `@0x4af729` / `@0x4af769`; raised to a FLOOR of 128 by `Entity_OnDamageReceived @0x4af828..0x4af82a` when the ammo's `kz_physics` byte (+0xE1) reads 3 | `if (v > 3) v -= 4; else v = 0;` `@0x4DE5A7..0x4DE5B9` |
| `dword_B764BC` | REVIVE tint | `= 255` in `NapiNPClientMsg_0x03A @0x422685` (a medic is reviving me); also cleared by `NapiNPClientMsg_GameReset @0x422843` | `if (v > 1) { v -= 1; if (v < 0xC4) v = 0xC4; }` `@0x4DE5D6..0x4DE5ED` |

Sibling arm in the same function: `g_CameraShakeCounter += 10`, cap 255
`[orig: @0x4dd8a6..0x4dd8ad]`.

Three details the decay run hides:

- The order is white, red, revive, immediately after the shake decay
  `[orig: @0x4DE590]` - one basic block, one client frame.
- A white flash of 255 runs exactly 64 ticks (63 subtractions of 4 land on 3,
  which snaps to 0); a red flash of 255 runs 128 ticks.
- The revive leg has NO `else` branch, and its `0xC4` floor is a HOLD, not a
  clamp toward zero: 255 slides to 196 over 59 ticks and then stays at 196
  forever. Only `Game_InitNewRound @0x422790` (the local respawn), the mission
  start, or the 0x3A reset ever puts it out.

Clear: `Game_InitNewRound @0x422778` / `@0x422784` / `@0x422790`.

### The HUD suppression

`HUD_RenderAllOverlays @0x5A8070` early-returns on
`if (g_spawn_success_gate || dword_B764B8 || !g_local_player_entity) return`
`[orig: @0x5a8084..0x5a80a3]`. A collision or explosive hit therefore BLANKS the
entire gameplay HUD overlay pass for up to 64 ticks. The SIGHTS card rows and the
FP viewmodel are drawn by `Render_ProcessMainSceneFrame`, not by that pass, so
they keep drawing.

### The draw

`Render_ProcessMainSceneFrame @0x5CAB9A..0x5CAC48`, only while
`!g_death_screen_active` `[orig: @0x5cab9a..0x5caba1]`, AFTER the HUD overlay
pass and the death-menu latch and BEFORE
`Environment_ApplySunVeilAndExposureStopdown @0x5cac4b`. Each quad goes through
`render_fullscreen_decal_quad(x, z, colour, mode) @0x5C6590`: the viewport rect
as 4 x 40-byte vertices at `0x29D6000`, `uv (0,0)(1,0)(0,1)(1,1)`, drawn as a
triangle strip with pass flags `0x300000` (NOWRITEDEPTH | NOCHECKDEPTH).

| # | word | condition | colour | quad mode |
|---|---|---|---|---|
| 1 | white | `!= 0` `@0x5caba7` | `(word << 24) \| 0xFFFFFF` | 2 - device render state 2, the untextured iterated-colour material (the same one the sun-glare veil uses) |
| 2 | red | `!= 0` AND `g_camera_mode != 3` `@0x5cabd5..0x5cabe5` | `a = min(word, 0xC0)`; `(a << 24) \| 0xFF0000` | 3 - `g_vignetteMaterial`, `vignette.tga` loaded by `sub_5C36B0 @0x5c36c1` with material flags 593 = 0x251 = AFUNC_BLEND \| ASRC_TEXTURExITERATED \| COLOR_ITERATED (texture alpha x vertex alpha, vertex colour) |
| 3 | revive | `!= 0` `@0x5cac18` | `0xFFFFFFFF - ((word >> 1) * 0x10100)` = A 255, R = G = `255 - (word >> 1)`, B 255 | 0 - device render state 3 |

Camera mode 3 is the retail free/spectator camera; the red vignette is the only
one of the three it suppresses.

**Residual - the revive quad's blend.** Device render state 3 has exactly one
user in the binary (this quad), so its blend mode is not readable from a call
site the way states 1 and 2 are. An alpha-255 source-over quad is ruled out by
behaviour, not by disassembly: the word HOLDS at 196 until the round clears, and
a source-over `(157, 157, 255)` wall would leave a revived player staring at
opaque blue for the rest of the round. The port draws it as a MULTIPLY tint
(R = G = `channel/255`, B = 1.0), which is the only reading consistent with the
hold. Re-witness it if the state-3 material is ever pinned.

### The five Player_OnDamageReceived call sites

See [docs/world/world-wac-ai-re.md](../world/world-wac-ai-re.md)
"Player_OnDamageReceived" for the arm body, the two unported producers inside it
(the radar damage blip, D-HUD-21, and the `unk_26C77A0` per-player-slot words)
and the per-call-site gates.

### Port status

| piece | status |
|---|---|
| the three words, their arms, decays, the `0xC4` hold and the round clear | **ported** - `world::ScreenFlashState` + `screen_flash_*` in `engine/runtime/world/player_view.h`, decayed in `LocalPlayer::apply_player_input_pre_tick` beside the shake, cleared in `LocalPlayer::reset_for_new_round`; ctest `screen_flash` |
| `Player_OnDamageReceived` (red 120 + shake 10) | **ported** - `world::player_on_damage_received` |
| its four ported call sites | **ported** - `destruction.cpp` (the person path and the pool-0 explosion sweep), `infantry.cpp` (the org2 landing leg) |
| `Projectile_ProcessDamageOnTarget @0x4e8213` | **open** - the fifth call site, in `round_sim.cpp` |
| `Entity_UpdateInfantryPlayerBody @0x4b61e8` (org1 death leg) | **open** - our unified infantry motor has no separate org1 death leg |
| the white flash's two collision-blackout writes | **ported** - `collision_force.cpp`, both legs, inside the NoFriendlyFire gate |
| the explosive white FLOOR + the near-miss shake | **ported** - `world::entity_on_damage_received` (`collision_force.cpp`), called from the outer-band flinch `@0x4eb05c` |
| the joiner 0x0A tail health-drop arm | **ported** - `world::screen_flash_arm_health_drop`, called from `JoinerRole::apply_authoritative_health` |
| the S2C 0x3A revive arm | **ported** - `screen_flash_track_revive` off `ClientState::local_medic_reviving`'s rising edge, from `JoinerRole::pump` |
| the three quads + the HUD suppression | **ported** - `godot/game/world/player_view_effects.gd` (`update_damage_feedback`) and `godot/game/world/game_hud_presenter.gd` |
| the radar damage blip | **open** - D-HUD-21, the radar blip system is unported |
| the `unk_26C77A0` slot words +11 / +12 | **open** - no witnessed consumer |

**Shell stacking divergence.** Retail draws the three quads after
`HUD_RenderAllOverlays` and before the sun veil. In our shell the quads live on
`PlayerViewEffects`, which is mounted behind the gameplay HUD, and the sun veil
is a behind-parent sibling of that node. The three quads therefore sit BELOW the
HUD and ABOVE the sun veil rather than the other way round. It only shows when
the HUD is visible at the same time as the red vignette or the revive tint (the
white flash suppresses the HUD outright). The suppression itself is applied as
`self_modulate` alpha 0 on the overlay Control, which blanks that Control's own
draw list while leaving its children (the view effects, the SIGHTS card) drawing
- the same split retail's two passes have - but it does not short-circuit the
draw-list build, so `hud_hidden_capture_witness` still reports the quads it would
have drawn.

## First-person view effects: binoculars, NVG and the underwater murk (2026-09-24)

The rendering parity pass (PR #678) witnessed the layout the view effects had carried as
"pending witness" and moved the two scene-space effects out of the HUD shell.
`engine/runtime/hud/view_effects.h` cites every constant below;
`godot/game/world/player_view_effects.gd` keeps only the texture loads and the CanvasItem
draws, behind the ordinary HUD (so health, stance and ammo stay visible), in the 1024x768
virtual overlay space.

| Element | Witness | Port |
|---|---|---|
| Binocular mask (`Binoculr.tga`) over the overlay rect, then `BinoCH.tga` stretched into (384, 256)-(640, 512) | `Binoculars_DrawMask @0x5cfe95..0x5cfee0` (the mask), `@0x5cfee5..0x5cff17` (the rect: `flt_7DC618` / `flt_7D1D70` / `flt_7DC188` / `flt_7C59AC`), drawn `@0x5cff48..0x5cff5b` | `kBinocularCrosshair*` |
| Binocular rangefinder digits (`BNumbers.tga`): x0 486, y 683, cell 16, step 10 | `Binoculars_DrawRangefinder @0x5908c0` (x), `@0x5908cf` (cell), `@0x5908de` (y), `@0x590926` (step) | `kBinocularDigit*`, `binocular_range_step` |
| NVG mask (`NVG.tga`, state `dword_2BDFADC`) over the overlay rect, then the gain scale (`Nvgscale.tga`, row = `g_NVGBrightnessLevel`) at (960, 32)-(1008, 64) modulated `0xFF7F7F7F` | `NVG_DrawMaskAndGain @0x5cffab..0x5cfff1` (the mask), `@0x5d0005..0x5d001d` (the rect), `@0x5d003e` (the row), `@0x5d004a` (the modulate) | `kNvgScale*` |
| The NVG mask draws only with the full-screen composite, never on the death screen nor under the Scoped lens | `NVG_Composite @0x5d1077..0x5d1080` | `LocalPlayerViewFrame::nvg_mask_visible` |

**The NVG image.** The picture under the mask is not a HUD effect: it is FrameFX's NVG
render-to-texture chain (the 512² scene, the persistent 256² glow, the tint + MODULATE2X
composite), recorded with the FrameFX screen effects in
[render-order-re.md](../render/render-order-re.md). The former CanvasItem NVG post
(`nvg_view.gdshader`) and its "four-frame history" reading are deleted; the Scoped arm's lens
and reticle are in the circle-mask section above.

**The underwater murk** is no longer a HUD / `PlayerViewEffects` quad (the `UnderwaterMurk`
rect and `MissionEnvironment`'s `underwater_overlay_changed` signal are deleted). Retail draws
it in the scene core's post-particle tail (the `Render_DrawViewportColorQuad @0x5c38e0` call `@0x5c96f5`, after the
coronas and the water glint, before the sun glare), so it draws per view from that view's
render eye, before the bloom; the port draws it in the scene overlay stage
(`engine/runtime/renderer/scene_overlay.h`, `SceneOverlayCompositorEffect`), recorded in
[render-order-re.md](../render/render-order-re.md) and
[env-tod-re.md](../env/env-tod-re.md) (the underwater murk composite).

**Damage feedback.** The three fullscreen quads of §Local damage feedback stay on
`PlayerViewEffects`; the same red word also drives FrameFX's type-1 damage blur, and the
death state its type-4 blur (the FrameFX screen effects in render-order-re.md), so the
vignette is not the only damage feedback on screen.
