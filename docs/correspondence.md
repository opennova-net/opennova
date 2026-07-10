# RE correspondence + parity matrix

> Consolidated from `notes/correspondence.md` (grill sessions 2026-06-01 → 2026-06-10) on 2026-06-10.

Binary: retail `Jointops.exe` (Joint Operations: Combined Arms), imagebase `0x400000`,
IDB `Jointops.exe.kong.i64`. All addresses are absolute in that image. Per-row status:
matching | divergent | unknown, as of the cited grill.

One page for two questions: **which original function does our X correspond to**, and
**how verified is that correspondence**. The per-domain RE records own the format
layouts, divergence catalogs (`D-…` ids), and verdict rationale; this page joins down
into them.

## 1. Per-system parity matrix

Systems whose verdict tables already live in a tracked RE doc are not repeated here —
the row links to the authoritative record.

| System (reimpl) | Grilled | Verdict | Authoritative record |
|---|---|---|---|
| MNU/MNS menu UI (`libs/mnu` (incl. the mnu_xml reader), `libs/mns`, `godot/engine/mnu`) | 2026-06-01 format + 2026-06-09 menu slice + 2026-06-12 mns spec pass + 2026-06-23c combo-dropdown grill | **matching** (D-MNU-1..3 accepted, D-MNU-7/8 fixed; D-MNS-1..4 lenient-with-diagnostic, loader grill pending) | [menu-re.md](mnu/menu-re.md) |
| LWF banks / DBF dialogs / member selection (`libs/lwf`, `libs/dbf`, `libs/audio`) | 2026-06-09 | **matching** (D-SND-1..3) | [lwf-dbf-sound-re.md](audio/lwf-dbf-sound-re.md) |
| MUS VM + compiler / SBF codec / SCR container (`libs/mus`, `libs/sbf`, `libs/scr`) | 2026-06-09 | **MATCHING** per component (D-SCR-1/2; host audio glue not grillable) | [mus-sbf-re.md](audio/mus-sbf-re.md) |
| Environment / time-of-day (`libs/env` + `env_render`; `nova_environment`/`nova_sky`/`nova_weather`/`nova_celestial`) | 2026-06-09/11 (+ REN-6 2026-07-06/07, REN-7 2026-07-07) | per subsystem: parse/TOD/sun-moon/fog/load-order **matching**; weather **divergent → ported**; sky dome combine **divergent — recovered (C6), port = C7**; celestial + glare new-from-witness; iris (auto-exposure) **live since REN-5** (#17 FIXED); terrain_rgb **ported** (#19); the REN-6 leftovers ALL FIXED (#27 scalar springs, #29 strip-march water + the embedded ps.1.1, #30 reflection RTT, #33 star field); the frame clear **renders since REN-7** (#21 close-note — BG mode + post-blend doubling + smoothed blend input; D-TERRAIN-3's substance; sky-pass leg witness `Terrain_RenderSkyboxPass @ 0x610ac0` → `Terrain_RenderSectorBatchLit @ 0x60c670`) | [env-tod-re.md](env/env-tod-re.md) |
| RTXT string tables (`libs/rtxt`, NovaStrings) | 2026-06-09 | **matching** at byte level (98/98 retail bins roundtrip; D-RTXT-4 strictness retained) | [rtxt-strings-re.md](interface/rtxt-strings-re.md) |
| HUD overlay render (`godot/engine/ui/hud_*.gd`, `NovaHudPos`, `game_hud.gd`) | 2026-06-22 (engine-research) + 2026-07-09 (weapon-coupled elements witnessed AND ported) | **ported** — health/stance cross-fade/text/ammo count+name/clip indicator/crosshair spread/ALPHAFADE/triggered text live (D-HUD-1..8; recoil terms D-HUD-7, chat altitude D-HUD-6); radar/MP objective/heat/timer + parachute-armor icons deferred | [hud-re.md](interface/hud-re.md) |
| Object materials / render state (`libs/oed` tag registry, `libs/renderer` classify/composer/uv_anim, `NovaObjectShaderCache`, `nova_object_model.gd`) | 2026-07-06 (REN-2 grill + REN-4 shader/TSS decode: HLSLEffect registry @ 0x5af790/0x5ae690/0x5afed0, resolution @ 0x5ade70/0x5b03c0, state application @ 0x5d9f50/0x6770a0, the pass loop + probe union + UV anim @ 0x5b1990/0x5de6b0, mode decoders @ 0x680f00/0x680b00/0x681080/0x681d00; the color-pipeline sweep — sampler states @ 0x679c1b/0x67e3ec, render states, gamma ramp @ 0x677be0; + REN-7 2026-07-07 detail-stage corpus sweep) | **matching** for registry/resolution/flag-byte state/blend/depth + capability words + tracer look + UV-anim math + composed lighting (D-RMAT-1/-2/-3/-4 fixed; the 5 OED-dump drift rows corrected; D-RMAT-5 fixed at REN-5 — the witnessed FF MODULATE2X model) and the **gamma-space color pipeline** (D-RMAT-7 + the D-RMAT-9 fog table fixed at the model-parity slice — raw sampling, gamma math, exact-inverse output, calibrate proof 256/256; D-RMAT-8 blend-space residual permanent); the `_MT` detail stage at witnessed strength (D-RMAT-10 fixed at REN-7 — corpus-uniform `Modulate2x(Texture, Current)` + alpha modulate, verified across `_FFP.fx`/`BDiffT2.fx`/`SkBDiffO2.fx` re-derived from retail `localres.pff`); D-RMAT-6 residual host mappings tracked | [render-material-re.md](render/render-material-re.md) |
| Runtime shader/TSS resource sets (terrain surface host shader; split FAR/MODEL foliage shaders; water surface `water.gdshader`; sky pass-1) | 2026-07-06 (REN-4 engine-research: PolyTrn_InitTextures @ 0x60aaa0, compile_terrain_pixel_shaders @ 0x605260, Terrain_InitSectors @ 0x601260, Terrain_CreateFoliageVertexShaders @ 0x5ff630, Foliage_CreateLightmapBlendPS @ 0x5ff7a0, Terrain_SetupSectorModelDraw @ 0x6007c0, Water_InitSurfaceShaders @ 0x5c19b0, create_water_shaders @ 0x5dfc30 = the EffectWorld particle-water trio, GfxShader factory family @ 0x679030..0x683650, the state permutation cache @ 0x681d00) | terrain shading remains **witnessed (confirm-only)**; foliage's local wind/material mechanics are **ported** with upstream feeds tracked by D-FOLIAGE-7; water surface blend/alpha-test **ported** (env #34 fixed); sky pass-1 TSS **closed** (mode 0x200 = flat diffuse — the existing port was already faithful); embedded-shader census complete | [terrain-re.md](terrain/terrain-re.md) §Runtime surface shading + [foliage-re.md](foliage/foliage-re.md) §FAR wind and material passes + [env-tod-re.md](env/env-tod-re.md) §Water surface |
| Foliage FAR/MODEL tiers (`libs/foliage` + `NovaFoliageDispatcher` + split FAR/MODEL shaders) | 2026-07-08/09/10 (MODEL placement/draw witnesses at `0x600980`/`0x601f50`/`0x601d90`/`0x600f00`; corrected FAR full-mesh generator `generate_foliage_instances_0 @ 0x5ffdd0`, raw surface mask `@ 0x6066d0`, wind/pass chain `@ 0x5ff630`/`0x5ff7a0`; the FAR feed/slot pool `Terrain_CollectNearFoliagePatches @ 0x603e60` / `Foliage_UpdateFarCellSlots @ 0x601b30` and the per-patch fade/pass draw state `render_terrain_lightmaps @ 0x60a171..0x60a53b`; + the 2026-07-10 regrill: tile-RT bake `PolyTrn_RenderTile @ 0x60dce5/0x60e38a` (`PolyTrn_TileBakeBasePass`/`PolyTrn_TileBakeDot3LightPass`), mesh t0 identity `CD3DDevice_FindBestTexturePermutation @ 0x604392`, MODEL FOGENABLE-off `@ 0x601e33`/`CGfxShader_ApplyPass @ 0x68324f`, avg-detail scope `@ 0x60ab00`) | **matching for hosted mechanics + witnessed T1 content** - FAR feed/pool/fade/pass, full-source emission, terrain bend, wind, the colormap-only tile-RT T1, MODEL scale/fold/pass/draw cadence, unfogged-black MODEL pass, and the last-submission dedup (D-FOLIAGE-10) are ported; the jodemo per-entity dispatcher was deleted; fold-alpha stand-in/entity-stream/blocker/wireframe-resubmit approximations ride D-FOLIAGE-7 | [foliage-re.md](foliage/foliage-re.md) |
| Draw order / batching (`libs/renderer` render_order; the engine priority-ladder application at `nova_celestial.gd`/`nova_water.gd`/`nova_object_model.gd`) | 2026-07-06 (REN-3 engine-research: batch family @ 0x5d8b40/0x5d8f20/0x5d94b0/0x5dad80/0x5dae40, frame @ 0x5ca0f0/0x5c93a0, viewmodel @ 0x4ded60/0x58a8f0/0x58a7b0) | **matching** for the ported ladder + sort-key/pass-class semantics (D-RORD-1 fixed in-slice); frame sequence confirm-only; D-RORD-2..6 tracked (opaque state-sort host-internal, per-strip water split, viewmodel depth trick, glow queue, original key quirks) | [render-order-re.md](render/render-order-re.md) |
| Runtime terrain queries — height samplers + segment raycast (`libs/terrain_query` raycast = the ENG-3 B1 port target; adopters: mission picking `terrain_editor.raycast_terrain_at`, celestial glare `nova_celestial._glare_ray_clear`) | 2026-07-07 (ENG-3 B0 engine-research: `Terrain_SampleHeightBilinear @ 0x6067b0` ex-`null_stub` retyped — ~40 callers un-elided; `Terrain_GetHeightAtPosition @ 0x606720`; `Terrain_RaycastHeightmapLoRes @ 0x60cb80`; `Terrain_RaycastHeightmapHiRes_0 @ 0x60e710`; `Terrain_RaycastLoResNoNormal @ 0x610860`; sibling `Terrain_RaycastHeightmapHiRes @ 0x60c760` callers-only; substrate globals renamed `Terrain_HeightAtlasPtr`/`Terrain_SectorGrid`/origins/OOB masks/seam flags/last-ray-step) | **witnessed (confirm-only)** — the ~1-unit major-axis march + point-sample coarse test + bilinear confirm, the ≤8/≤8/8-iteration refine with ÷4 steps, the column-shortcut crossing rule (buried-segment asymmetry), the height-0 empty-cell floor, return 0=HIT/1=CLEAR; port pending (ENG-3 B1) | [terrain-re.md](terrain/terrain-re.md) §Runtime terrain queries |
| World lighting / modulator chain (`libs/renderer/light_runtime`, `libs/env::ModulatorChain` + `iris_gain`, `NovaWeatherCore`, the composer's FF model, `terrain_lighting.gdshaderinc`) | 2026-07-06 (REN-5 engine-research + grill: modulator @ 0x58db30/0x5aaef0/0x57d940/0x57ef97, world block @ 0x5c8090/0x5d89e0, entity uniforms @ 0x5d98a0/0x5d8cb0/0x5c6800, point lights @ 0x5a9180/0x5aa450/0x5abc50, terrain c0/c1 @ 0x604420/0x604ee0/0x610c80, cubes @ 0x58f290/0x685bb0/0x6106a0, textures @ 0x5a94f0) | **matching** for the ported chain (env #17 + D-RMAT-5 FIXED; the composed FF MODULATE2X model, the terrain c0/c1 = sky/light correction, the exposure live in the weather core); D-RLIT-1..6 tracked (unhosted blocks, interior sampling, per-entity sun raycasts, dynamic-light hosting, reflection stand-ins, terrain shadow variants); render-slot shadow lighting witnessed confirm-only (out of REN scope) | [render-lighting-re.md](render/render-lighting-re.md) |
| BMS event runtime + mission→world promotion (`libs/mission` event_runtime/promote) | 2026-06-10 | **MATCHING** (D-EVT-1..4) | [bms-event-runtime-re.md](mission/bms-event-runtime-re.md) |
| World / WAC VM / AI + infantry motor (`libs/world`, `libs/wac`) | 2026-06-07..10 (+ 2026-06-22 player slide/gravity) | **MATCHING** (infantry D-INF-1..10; ground-vehicle drive ported net-side, D-NET-161; AI-driven + HELO movement physics stay tracked `not_yet_ported`) | [world-wac-ai-re.md](world/world-wac-ai-re.md) — carries its own correspondence map (§2 there) |
| Avatars.def / player-info avatar selection (`libs/avatars` + `NovaAvatarDatabase`) | 2026-06-15 engine research + 2026-06-16 implementation grill + 2026-06-23 `PLAYER_INFO` orchestration grill | **matching for parser/data model/editor bridge + full screen orchestration** (init/registration/cascade/team/voice/ACCEPT/loadout witnessed, D-PLAYERINFO-7..12; runtime `player.mnu` host wiring is the next phase; D-PLAYERINFO-1 combo→spawned-player binding still open) | [avatars-re.md](playerinfo/avatars-re.md) — carries its own orchestration witness map (§ Screen orchestration there) |
| Particles `.ptl` format + simulator + render chain + runtime effect world (`libs/particle`, `godot/engine/particle`, `godot/modtools/particle`, `NovaEffectWorld` + the `game_world.gd` fx routing) | 2026-04-27/28 format+render passes, consolidated 2026-06-10; runtime load/spawn chain witnessed 2026-07-10 (CEffectSystem_Init @ 0x5f6070 from Game_StartMission @ 0x524980; spawn descriptor @ 0x5f6df0; handle intern @ 0x5f7310 ex-kong `CEffect_FindOrCreateMaterial`; WAC fx handlers @ 0x4f23a0/0x4f7fd0 ex-kong `*Sound*` misnomers) | **matching** parse/write (77-file corpus) + **match (semantic)** simulator/render; runtime load+intern+fx2ssn spawn **ported** (D-PTL-1..8; fx2tgt target-id field + weapon-action chain = §8 follow-ups) | [ptl-format-re.md](particles/ptl-format-re.md) |
| `.bms` mission loader (`libs/mission/src/bms.cpp`) | 2026-06-02 (+ 2026-06-08 foundation pass) | per function — §3 below | this page |

## 2. MNU function table (format grill 2026-06-01)

Statuses in this table are frozen at the 2026-06-01 format grill. The 2026-06-09
menu-slice grill ([menu-re.md](mnu/menu-re.md)) subsequently closed the deferred
render/sound/type-factory divergences; its Verdict section is authoritative — the
divergences that remain by decision are D-MNU-1..3 there. The symbol↔address join
below stays valid.

| reimpl symbol (file) | original | addr | signature / role | evidence | status (2026-06-01) |
|---|---|---|---|---|---|
| `mnu::parse_window` (`libs/mnu/src/mnu.cpp`) | `CUIElement_ParseXMLDefinition` | `0x648120` | element attr/child parser | wide `POSITION`/`APPEARANCE`/`MOUSEOVER_FG`/`MONOGRAM` + `FORM`/`GLOBAL_VAR` literals xref | divergent → closed by menu slice |
| edit attrs in `parse_window` (NUMBER/MINVAL/MAXVAL/MAXCHAR/READONLY/PASSWORD) | `parse_edit_widget_xml_properties` | `0x661d10` | edit-widget override | wide `NUMBER`/`MINVAL`/`MAXVAL`/`MAXCHAR`/`READONLY` literals | divergent (PASSWORD dropped) |
| checkbox attrs in `parse_window` (AS_BUTTON/CHECKED) | checkbox override | `0x64ad90` | checkbox override | `CHECKED` literal @ 0x7e12f4 | matching |
| `mnu::parse_type_string` + `window_type_name` (`mnu.cpp`) | `CUIScene_CreateWidgetByType` | `0x64f630` | TYPE→widget class factory | full-word wide literals (`get_bytes`) | matching; 4 missing tokens since modeled (all 16 in menu-re.md) |
| `mnu::parse_table_header` / `parse_table_column` (`mnu.cpp`) | `CTableWnd_ParseXMLContentDefinition` | `0x6427d0` | table parser | wide `APPEARANCE`/`HEADER` literals | divergent (HEADER `type="id"`) |
| `mnu::parse_listbox` (`mnu.cpp`) | `CListWnd_ParseXMLDefinition` | `0x645770` | list parser | wide `APPEARANCE` + list class | matching |
| `NovaMnuCombo` + `build_combo` `set_popup_rect` (`nova_mnu_combo.cpp`, `nova_mnu_builder.cpp`) | `CComboWnd_Construct` + `CComboWnd_ParseXMLDefinition` | `0x65be40` / `0x65c0d0` | combo + embedded CListWnd popup (`this+1536`) at authored `<LIST_BOX>` POSITION | ctor lays out `CButtonWnd@+764`/`CListWnd@+1536`; parse feeds LIST_BOX to `this+384` | matching (2026-06-23c grill; was divergent — D-MNU-7) |
| `NovaMnuCombo::open_popup` + `effective_item_height` (`nova_mnu_combo.cpp`) | `CListWnd_DrawItems` (+ `CListWnd_Construct` defaults) | `0x643f30` / `0x643bb0` | dropdown row layout: rect `this+13`, row height font-"W"/`this+201`, truncation | row_height from `sub_653680("W")` else `this+201` (`<MIN_ITEM_HEIGHT>`) | matching (2026-06-23c grill; was 16px default — D-MNU-8) |
| scroll `ORIENTATION` in `parse_window` | `CUIScrollWidget_ParseExtendedXMLDef` | `0x64c6d0` | scroll extended attrs | wide `ORIENTATION` @ 0x7e1494, `HEIGHT` @ 0x64c766 | divergent (scroll `<HEIGHT>` dropped) |
| `mnu::parse_screen` (`mnu.cpp`) | `parse_scene_node_attributes` (+ SCREEN cb `0x63b800`) | `0x639630` | SCREEN children NAME/MUSICVAR/WINDOW | wide `MUSICVAR` @ 0x7e072c | matching |
| `mnu_xml::parse` / `skip_bom` (`mnu_xml.cpp`) | `XML_ParseWithBOMDetection` | `0x76a690` | SAX parse + BOM detect | callee of `0x63c830`; BOM logic | matching (corpus) |
| `mnu_xml::decode_entity` (`mnu_xml.cpp`) | `XML_ParseCharEntity` (+ table `0x85a628`) | `0x769cc0` | entity decode | entity table layout | divergent (edge cases) → closed by menu slice |
| `MnsStyleSheet::substitute` (`godot/engine/mnu/mns_stylesheet.cpp`) | `NapiXML_ExpandVariablesInText` | `0x63a000` | `%VAR%` expansion | whole-buffer pre-parse | divergent (scope/timing) → accepted as D-MNU-1 ([ADR 0005](adr/0005-mnu-var-expansion-policy.md)); covers colors/fonts/textures/literal text |
| `mns::Document::parse` (`libs/mns/src/mns_document.cpp`) | `sub_552500` (.mns stylesheet load) | `0x552500` | stylesheet parse | ref'd from `0x63c830`; loader body unwitnessed — built from the in-file NovaLogic spec | probable (lossless model [ADR 0014](adr/0014-mns-lossless-document-model.md); D-MNS-1..4 in [menu-re.md](mnu/menu-re.md); grill pending) |
| frame draw (`godot/engine/mnu/nova_mnu_builder.cpp`) | `CUIElement_DrawFrame` (draw) + STENCIL parse `0x648120` | `0x64a210` | 9-piece frame draw | center + 8-piece geometry; INSETX/INSETY | divergent → closed by menu slice (8-piece rework) |
| menu load + CRC cache | `UIScene_LoadAndParseContent` | `0x63c830` | load/parse orchestrator | refs `main.mnu` + `menu_style.mns` via `sub_552500` | matching |
| `NovaMnuMenu::dispatch_action` (`nova_mnu_menu.cpp`) | `CUIWidget_HandleScriptedAction` | `0x649790` | action dispatch (int code) | action enum @ 0x648f21 | matching (nav subset) |
| `<STRING type="id">` resolve | `RTXT_GetString` / `CUIStringTable_LookupString` | `0x51ebd0` (table @ 0x6434df) | string-table lookup | menutxt.BIN, 485 xrefs | probable; RTXT side since verified ([rtxt-strings-re.md](interface/rtxt-strings-re.md)) |
| `NovaMnuMenu::show_screen` / `navigate_to_screen` | `CUIScene_SelectNodeByName` | `0x63b6b0` | select screen by name + deselect/select events | nav model | matching |
| Command boundary (host callbacks) | event dispatcher `sub_63B0D0` | `0x63b0d0` | generic callback-list dispatch | confirms [ADR 0001](adr/0001-mnu-action-command-boundary.md) | matching |
| `mnu::parse_window` group/checked (radio) | `CRadioWnd_ParseXMLDefinition` | `0x656c40` | GROUP int attr (widget+0x308), CHECKED child | wide `GROUP` @ 0x7ca808 | matching |

## 3. Mission `.bms` loader function table (grilled 2026-06-02)

The loader/format slice. The event runtime, tick cadence, and promotion are the
2026-06-10 grill in [bms-event-runtime-re.md](mission/bms-event-runtime-re.md).

| reimpl symbol (file) | original | addr | signature / role | evidence | status |
|---|---|---|---|---|---|
| `bms::parse` / `parse_header` (`libs/mission/src/bms.cpp`) | `Mission_LoadBMSFile` | `0x40f4e0` | full `.bms` loader: header, loadout, 4 entity pools, waypoints, groups/layers, triggers, bboxes | section order + `fread` sizes; fixture byte-match | divergent → fixed (version, 2nd chunk) |
| magic+version gate in `parse_header` | `BMS_LoadAndValidateHeader` | `0x40e250` | magic 'BMS' + version ≥ 19 + count clamps | `byte_A761D3 < 19` @ 0x40e30a; "Too many" strings @ 0x40e326+ | divergent → fixed |
| `bms::parse_entity` | `Entity_SpawnFromBMSRecord` | `0x40e9f0` | 172B record → entity; pos/rot/AI/type branches | offset-by-offset; `(90−yaw)` @ 0x40eb66 | matching (DOC refinements) |
| events/triggers/actions in `bms::parse` | `EventTrigger_LoadAllData` | `0x453eb0` | 3 i32 counts + 24/32/32-byte records, contiguous | `AE0700/08/10` sizes; event +4/+8 fixup | matching |
| `bms::parse_waypoint_record` | waypoint loop in `Mission_LoadBMSFile` | `0x40fb72` | 136B record; 1-marker → DoesNotLoop fixup | `int* += 136` (4 rec/iter); `dword@4==1 → dword@0\|=1` | matching (runtime fixup not applied at parse, by design) |
| `bms::parse_bounding_box` | bbox loop in `Mission_LoadBMSFile` | `0x40fcf4` | 36B nav-zone; min/max canonicalize on load | per-axis swap `if min>max` | matching (no parse-time swap, by design) |
| `bms::parse_area_trigger` | area-trigger loop in `Mission_LoadBMSFile` | `0x40fc45` | `word_A76410` × 0x20 into `unk_A32D10` | count = 0 in all fixtures | unknown (layout + Y/Z swap untested) |
| `bms_to_godot_position` (`mission_object_placer.gd`) | `Mission_LoadBMSAndExtractSpawnPoints` | `0x40d650` | axis convention: (x,y) = plane, z = up, y inverted | grid `(x>>18)+512`, `512−(y>>18)` | matching |
| `bms_to_godot_rotation` (`mission_object_placer.gd`) | `Entity_SpawnFromBMSRecord` (heading) | `0x40eb66` | heading = `(90−yaw)`; pitch/roll direct | fixed-point angle conv | matching (was `180−yaw`; fixed 2026-06-08 mission-foundation pass, IDA-verified) |
| mission orchestration | `Game_StartMission` | `0x524360` | validate → load → terrain/net/HUD | xrefs to `0x40e250`/`0x40f4e0` | confirm-only (not reimplemented) |

## 4. Sound stack function table (.lwf / .dbf / selection, grilled 2026-06-09)

Layouts, RNG, and the divergence catalog live in
[lwf-dbf-sound-re.md](audio/lwf-dbf-sound-re.md).

| reimpl symbol (file) | original | addr | signature / role | evidence | status |
|---|---|---|---|---|---|
| `lwf::parse_lwf_buffer` (`libs/lwf/src/lwf.cpp`) | `SoundBank_OpenFile` (+ thunk `j_` @ 0x75cba0) | `0x75caa0` | open + header + 52B singles + 12B triggers read | "SNDTRIG DATA/TRIGGERS" allocs; strides 52/12 | matching (trigger table guarded, not modeled) |
| `lwf::parse_lwf_buffer` multis/playlists/sndparms | `SoundBank_LoadTriggerSets` | `0x75c370` | 80B set / 48B playlist / 28B member reads + offset→ptr patch | read sizes 0x50/0x30/0x1C; fname 256B/slot at entry+48; magic 'LWF1' @ 0x75c671 | matching |
| `audio::SoundSelector::select` (`libs/audio/src/sound_selector.cpp`) | member pick in `SoundBank_PlayTriggerEntries` / `SoundBank_SelectTriggerEntryFromBank` | `0x75ccd0` / `0x75bf20` | seq (0x10) / random-anchor cycle (0x80) / random default | three-branch flow @ 0x75cd5c..0x75cdfc, identical in both | matching |
| `audio::SoundSelector::scaled_random` | `PRNG_ScaledRandom` (+ 3 inline copies) | `0x75be50` | `s = ROL32(s + ROL32(s,11), 3); (n*lo8)>>8` | formula + seed 0x2B0749C1 @ 0x85A3DC (`g_SoundRngState`) | matching (stream-pinned test) |
| `NovaSoundBank._find_set` (`nova_sound_bank.gd`) | `SoundBank_FindTriggerByName` | `0x75be90` | case-insensitive set-name lookup | stricmp over 84B sets, name@+4 | matching |
| `NovaSoundBank.calc_distance_volume` / `oneshot_distance_volume` (`nova_sound_bank.gd`) | `SoundBank_CalcDistanceVolPan` + the `SoundBank_PlayTriggerEntries` distance stages | `0x75ca20` / `0x75cf1a..0x75cf80` | `vol*(255/256)*(1-d/r)^2`, hard 0 at r, clamp=sndparm+16; min@+6 proximity stage, falloff@+4 | integer-exact expectations in `sound_runtime_test.gd` (corrected 2026-07-10: the old inner/max reading was inverted) | matching |
| `NovaSoundBank.emitter_layer_volume` + `NovaMissionAudio.tick` mix | `SoundEmitter_UpdateAndMixTop8` | `0x5284a0` | per-frame range cull, member-0, two-radius rebased curve, loudest-8 channel budget | branch map @ 0x528667..0x5286df; top-8 sort @ 0x5287ab; `nova_mission_audio_test.gd` | matching (D-SND-6/7/8 host lifecycle, occlusion, bus globals) |
| `NovaMissionAudio._resolve_slot_sets` / `time_of_day_region` | `Entity_UpdateEnvSoundEmitter` + `Entity_CalcTimeOfDayRegion` | `0x4a8080` / `0x408110` | envsnd ai tick: soundLoopId[region], blend vol, per-class lifetimes; 4/10/17/21h cuts, 5460 Q16 margin, nibble stagger | dispatch table @ 0x82ABD4 ('envs'); same-set suppress @ 0x4a819d | matching |
| `SoundEmitter_RegisterSetLayers` slot model | emitter registration | `0x528340` | 767x48B slots, per-layer, pitch-0/vol-0 = clear | param block layout; host keeps persistent voices instead (D-SND-6) | matching (behavioral proof) |
| set pitch fields (`lwf.h` `Multi.pitch_base/_random_range`) | set pitch compose | `0x75c0be` | `(member*(base+jitter))>>16`, 0xFFFF ≈ 1.0 | multiply+shift | matching |
| `dbf::parse_dbf_memory` (`libs/dbf/src/dbf.cpp`) | `DialogManager_LoadFromFile` | `0x44e650` | 'DLG0' 28B header + 52B group + 68B lines | magic @ 0x44e6f7 (kong's 'DLH0' comment was wrong); blob re-read @ 0x44e79d | matching (id-def table guarded, not modeled) |
| `NovaMissionAudio.setup` bank chain (`nova_mission_audio.gd`) | `Game_StartMission` slot loop + `DialogSystem_Init` | `0x525448` / `0x5275e0` | 6 slots @ 0x82A5B0 table: expL, exp, gamelocl, game, game3, game2 (+ dialog co-named .lwf/.pwf @ 0x44e7d4) | name-table stride 0x104; `SoundBank_LoadIfExists` @ 0x527530 | divergent-documented (D-SND-1/2: merged single chain; expansion banks deferred) |
| `def.cpp` soundloop_1..7 / nightshot/dawnshot/duskshot (`libs/def`) | `ItemDef_ParseProperty` | `0x49eb00` | items.def marker sound keys | "soundloop_" @ 0x49fec4, time-of-day @ 0x49fdee; 7-name table @ 0x7d0788 | matching |
| `NovaMnuMenu` sound profile | menu bank load + UI bank collection | `0x5613bf` / `0x652b40` | "menu.lwf" → `g_MenuSoundBank` @ 0x25DC3E0; per-element add-or-ref from `CUIElement_ParseXMLDefinition` @ 0x648ada | call sites | matching — closed by the 2026-06-09 menu-slice grill ([menu-re.md](mnu/menu-re.md), Sound section); `.pwf` banks still unwitnessed |

## 5. NovaWorld net function table (gate + session, grill wave 1, 2026-06-11)

Verdicts and the NW-G1/NW-L2 findings live in
[net/novaworld-net-re.md §8](net/novaworld-net-re.md).

| reimpl symbol (file) | original | addr | role | evidence | status |
|---|---|---|---|---|---|
| `gate_response_parse` (`libs/novaworld/src/gate_response.cpp`) | `CNapiGateManager_ProcessResponse` | `0x4ced20` | parse `VAR`-tagged gate response; 19-key set; POST IP/port required | per-key `Napi_StrCaseEqual`; "NO NW POST IP/PORT" @ 0x4cf5xx | divergent → fixed (6 keys added; CUS/PVT flagged; addr citation corrected from 0x4ad330) |
| `build_gate_response` (`apps/novaworld_server/gate_listener.cpp`) | `CNapiGateManager_ProcessResponse` (emit side) | `0x4ced20` | emit gate response w/ required POSTIPADDRESS/POSTIPPORT | required-field gate `dword_B5F490`/`B5F494` | matching |
| gate-manager defaults (§6.7 struct) | `CNapiGateManager_InitDefaults` | `0x4d1460` | `gs.novaworld.net` @+8, 7597 @+72, `jop:cus2` @+76 | `Napi_CopyString` literals | matching |
| (base init, not reimplemented) | `CNapiGateManager_Init` | `0x633f90` | base default `novaworld.net` @+64, overridden by InitDefaults | `Napi_CopyString` @ 0x634021 | confirm-only |
| `session_hello.cpp` TLV reads | `NapiNPProtocol_HandleClientHello` | `0x6213b0` | NVS/CO/AP/BDAT/PN/PG/PV1/PV2 + retail PV3/PM/CI/EIP/EPN/ET | per-tag `Napi_StrCaseEqual`; NWU key @ 0x7DFC50; version @ 0x7DFCF0 | matching |
| `lobby_session.cpp` dispatch | NOVAWORLDUDP containers (§3) | — | all 10 containers + replies | `lobby_session_test`; §3 | matching (spot-checked) |

## 5.1 NWU cipher (grill wave 3, 2026-06-11)

Re-anchored to retail (was jodemo-only). Verdict + NW-C1 in [§8](net/novaworld-net-re.md).

| reimpl symbol (file) | original | addr | role | evidence | status |
|---|---|---|---|---|---|
| `nwu_decrypt` (`libs/novacrypto/src/nwu.cpp`) | `NapiNP_EncryptBuffer` | `0x6187b0` | ADD chain (retail "encrypt") | 4-phase add: key→reverse→prog→LCG | matching (byte-exact) |
| `nwu_encrypt` (`nwu.cpp`) | `NapiNP_DecryptBuffer` | `0x618880` | SUBTRACT chain (retail "decrypt") | inverse of 0x6187b0 | matching (byte-exact) |
| `nwu_compute_seed` (`nwu.cpp`) | `NapiNP_ComputeKeySeed` | `0x618430` | seed: null→3252, Σ(i+key[i]²)+len+50 | signed key bytes | matching |
| `clcg_init`/`LCGState` (`nwu.cpp`) | `NapiPRNG_Init` | `0x62e430` | LCG `{state,mult,counter}`, mult 78665521 (`0x04B05731`) | struct + multiplier | matching (doc hex fixed) |
| `add_with_keystring` (`nwu.cpp`) | `Crypto_AddWithKey` | `0x6182d0` | `buf[i]+=key[i%klen]` | — | matching |
| `add_with_progression` (`nwu.cpp`) | `Crypto_AddProgressive` | `0x618250` | `buf[i]+=seed+i; seed+=step` | — | matching |
| `scramble_with_lcg` (`nwu.cpp`) | `Crypto_AddLCG` | `0x6183b0` | `state=u16(mult·state+1); buf[i]+=state` | — | matching |
| `reverse_in_place` (`nwu.cpp`) | `NapiNP_ReverseBuffer` | `0x618210` | `len>>1` front/back swaps | — | matching |

## 5.2 EPASK login-form encrypt (grill wave 3, 2026-06-11)

NW-C2 in [§8](net/novaworld-net-re.md). Polymorphic edit-widget `+0x38` dispatch resolved.

| reimpl symbol (file) | original | addr | role | evidence | status |
|---|---|---|---|---|---|
| `epask_encrypt` (`libs/novacrypto/src/epask.cpp`) | `sub_6669A0` | `0x6669a0` | NWU → modexp → NWU → A-P | golden vectors == test fixtures | matching (byte-exact) |
| `modexp_encrypt` (`epask.cpp`) | `sub_666600` / `modular_exponentiation` | `0x666600` / `0x666470` | `pow(byte+2,exp,mod)`, 32-bit LE/byte | `+2` byte-confirmed | matching |
| `encode_ap` (`epask.cpp`) | `NapiNP_EncodeToHexAlpha` | `0x666570` | A-P low-nibble-first | — | matching |
| `epask_from_string` (`epask.cpp`) | `parse_colon_delimited_string` | `0x666710` | `exp:mod:key` split | — | matching |
| (EPASK NWU copy) | `NapiNP_EncryptBufferAlt` | `0x6668e0` | UI-module NWU ADD chain | == `0x6187b0` | matching |
| call site | `build_form_field_query_string` ← `build_url_and_submit_request` | `0x657760` ← `0x63e3f0` | edit-widget vtable `+0x38`; `?EPASK=`, out buf `8·len` | — | matching |

## 5.3 PUBcrypto PUB* fields (grill wave 3, 2026-06-11)

NW-C3 in [§8](net/novaworld-net-re.md). Proves `ticket_transform` == NWU.

| reimpl symbol (file) | original | addr | role | evidence | status |
|---|---|---|---|---|---|
| `encode_pub_value` (`libs/novacrypto/src/pubcrypto.cpp`) | `NapiNP_EncryptAndEncodeToHexAlpha` | `0x618fd0` | CRC-append → NWU → A-P (single key) | — | matching (byte-exact) |
| `decode_pub_value` (`pubcrypto.cpp`) | `NapiNP_DecodeEncryptedString` | `0x619130` | inverse, right-to-left keys | — | matching |
| `crc32_be` (`pubcrypto.cpp`) | `NapiNP_ComputeCRC` | `0x618770` | CRC-32/MPEG-2, table `dword_849938` | check value `0x0376E6E7` | matching |
| `ticket_transform` (`pubcrypto.cpp`) | `NapiNP_EncryptBuffer`/`DecryptBuffer` | `0x6187b0` / `0x618880` | inlined NWU copy | proven identical | matching |

## 5.4 url_cipher NK/CK join tokens (grill wave 3, 2026-06-11)

NW-C4 in [§8](net/novaworld-net-re.md). Re-anchored to retail (was jodemo-only).

| reimpl symbol (file) | original | addr | role | evidence | status |
|---|---|---|---|---|---|
| `url_cipher_decode` (`libs/novacrypto/src/url_cipher.cpp`) | `parse_connection_query_string` | `0x54dfb0` | `plain=cipher-key+'0'`, `'&'`-terminated | — | matching (byte-exact) |
| `URL_CIPHER_KEY_NK` | `aDiheijefhgcdjc` | `0x7d3f30` | `"diheijefhgcdjcgcjcfbd"` | byte-identical | matching |
| `URL_CIPHER_KEY_CK` | `aCfhdcegjigecje` | `0x7d3f04` | `"cfhdcegjigecjehcgjdhe"` | byte-identical | matching |

## 5.5 Full client parity sweep (grill wave 7, 2026-06-14)

All 24 client systems re-grilled (3 passes, adversarially verified). Verdict table + the full
`D-NET-1..85` catalog live in [§8 "Wave 7"](net/novaworld-net-re.md) — not repeated here
(`D-NET-76..81` from the probe3_again controlled capture: RTT/trio transport, minimap `0x6B`,
reload `0x49`/death `0x13`/checksum `0x30`, deployed-item `0x59`/routed `0x44`; `D-NET-82..85`
from the first stock retail Co-op capture: every decoder byte-validated on organic content, S2C
`0x45` terrain-load promoted (§5.37), the `0x20`/`0x45` load-only cadence reframe of D-NET-55;
`D-NET-58..62` from the 2026-06-17 controlled-capture validation: in-game 0x0D team/bone +
0x20 movementVal label corrections, 0x0B icon offset, the `/PROFILE` `.sph` server-log
cross-validation, and the authored-mission cross-validation of pools 1/2/3 incl. the S2C 0x0C
organic-spawn field map; `D-NET-63` the §5.13 vehicle compact Euler-triple / weapon-aim label
rename; `D-NET-64` the §5.15 guided-weapon per-(mode×field-group) matrix + structural port
(selector = sub_op byte; wire integration deferred); see §5.11/§5.12/§5.13/§5.15/§5.19-§5.24).
Newly-grilled originals (join key = addr):

| original | addr | role | D-NET | status |
|---|---|---|---|---|
| `CNapiGameSession_InitNPConnection` | `0x4d3e1f` | CS field template (identical both dirs) | D-NET-1 | fixed |
| `NapiNPConnection_SendClientHello` | `0x61fe20` | 0x42 builder; CI/HK/CK gate | D-NET-2 | fixed |
| `NapiNP_HandleServerJoinResponse` | `0x629840` | JFC/JFP/JFS rejected-join read | D-NET-3 | fixed |
| `NapiNPConnection_SendSessionInit` | `0x620ef0` | 0x82 builder; RIP/RPN gate | D-NET-4 | fixed |
| `NapiNPConnection_DispatchMessage` | `0x622570` | high-table 0x80 selector | D-NET-5 | fixed |
| `NapiNPConnection_ParseMessages` | `0x625bc0` | LEN8/LEN16 precedence | D-NET-6 | fixed |
| `CNapiGameSession_HandleConnectVerifyResponse` | `0x4d5800` | Success = atol != 0 | D-NET-19 | fixed |
| `NapiGameList_ProcessEncryptedResponse` | `0x63d740` | GSB chunk format (prefix magic) | D-NET-32..35 | divergent |
| `CNapiGameSession_SendPlayRequest` | `0x4d3920` | ClientPlayRequest shape (CurrentlyPlaying + ClientVarList) | D-NET-37..39 | fixed |
| `NapiStatement_SerializeVarList` | `0x4d0660` | ClientVarList(VarList)+ClientVar(VarFNum/VarName/VarValue) | D-NET-38 | fixed |
| `Lobby_UpdateServerInfo` | `0x4fe8c0` | host-registration blob | D-NET-40..46 | fixed |
| `String_SanitizeForLobby` | `0x4fe750` | lobby field sanitize | D-NET-41 | tracked |

`/PROFILE` `.sph` server-log recorder cluster (§5.22; decoder `libs/npwire/src/replay/serverlog_decode.cpp`):

| original | addr | role | D-NET | status |
|---|---|---|---|---|
| `Game_ProcessMainFrame` | `0x5263f0` | per-frame recorder branch (8-tick, iterates pool 0) | D-NET-61 | matching |
| `CServerLog_WritePositionRecord` | `0x4e1b00` | PDAT entity sample (pos 16.16 / BAM32 yaw) | D-NET-61 | matching |
| `CServerLog_WritePlayerNameRecord` | `0x4e1cc0` | PDEF roster (team@entity+354) | D-NET-61 | matching |
| `CServerLog_WriteTimestampRecord` | `0x4e1aa0` | FBEG frame marker (tick>>3) | D-NET-61 | matching |
| `CServerLog_WriteDeathMarker` / `CServerLog_WriteDisconnectMarker` | `0x4e1e00` / `0x4e1c50` | PBRK / PREM event markers | D-NET-61 | matching |
| `CServerLog_CloseAndFree` | `0x4e1a10` | `.END` close | D-NET-61 | matching |

Crypto/TLV/envelope (NW-C1..C4, CRC32, NAPI TLV/envelope) re-confirmed byte-exact — no change.

Authored-mission cross-validation of pools 1/2/3 (§5.23/§5.24; D-NET-62; the dvxi5 probe's known
`mission.bms` decoded field-for-field on the wire — `nw_pool_groundtruth_test` reads the real pcap
via `apps/common/pcap_reader`, `nw_pool_decode_unit_test` inline-pcap round-trip):

| original | addr | role | D-NET | status |
|---|---|---|---|---|
| `NapiNPClientMsg_0x00C` | `0x42E730` | S2C 0x0C pool-0 organic spawn batch — flat slotId-first field map; team@entity+354 | D-NET-62 | matching |
| `NapiNPClientMsg_0x00D` | `0x432C40` | S2C 0x0D pool-1 spawn — type/pos/team reproduce vs authored knowns | D-NET-58/62 | matching |
| `NapiNPClientMsg_0x020` | `0x425C00` | S2C 0x20 pool-3 sync — type/pos/team/heading (90−facing) vs authored knowns | D-NET-59/62 | matching |
| `serialize_entity_pool_to_packet_0` | `0x503940` | team source = entity+354 (onhook +146/+196 ruled out) | D-NET-62 | matching |

C2S 0x0F entity-info query → S2C 0x18 full-entity-spawn self-heal (§5.46; grill 2026-07-01; reimpl
`encode_full_entity_spawn`/`decode_full_entity_spawn` in `libs/npwire`, `build_full_entity_spawn`
in `libs/netsim`, dispatch `case 0x0F` in `libs/npruntime`; `nw_ingame_encode` +
`netsim_world_stream_extractors` + `nw_message_coverage` pin the layout):

| original | addr | role | D-NET | status |
|---|---|---|---|---|
| `NapiNPServerMsg_HandlePlayerInfoRequest` | `0x514180` | C2S 0x0F handler — validates pool ≤ 1 + slot < capacity, serializes the queried entity, replies S2C 0x18 to the requester only (send_mask 0x20) | D-NET-133 | matching |
| `serialize_object_to_buffer` | `0x504D10` | the S2C 0x18 body writer — 26-field single-entity record (§5.46 field map); link ptrs → handles; name attrib-gated; seat block per itemDef+604 | D-NET-133 | matching |
| `NapiNPClientMsg_FullEntitySpawn` | `0x433780` | S2C 0x18 handler — destroy + memset(0x2B4) + full rebuild (itemDef/models/playerClass/minimap/ADM anim registration); type-0 record = clear slot | — | matching (decode side; client-side rebuild is retail-only) |

2026-07-01 re-grill of the §5.46 pair: `serialize_object_to_buffer @ 0x504D10` re-prototyped to its
true 4-arg form (arg 4 = serialized entity; the 3-arg IDB prototype mis-rendered all callsites); S2C
0x18's SECOND emitter witnessed (C2S 0x40 vehicle spawn, mask 0x90); wire item_type gate is !=0 only
(pool-derived approximation provably safe). Verdicts unchanged (matching).

Wire-coverage sweep (§5.48–§5.56; grill 2026-07-01; reimpl `libs/npwire/ingame_decode` decoders +
`nw_pp` printers + catalog rows; `nw_message_coverage` pins every layout; all three retail goldens decode
with zero unnamed tags in both directions):

| original | addr | role | D-NET | status |
|---|---|---|---|---|
| `NapiNPClientMsg_SessionStatus` → `SessionStatus_ParseFromBuffer` | `0x4228C0` / `0x530ED0` | S2C 0x58 session-status block (names + up-time sync + STROVER_STATVAR score rules; ex-"TerrainTexDef", renamed) | — | matching (decode side) |
| `SessionStatus_GetStatPointValue` | `0x52D5D0` | stat-point accessor block[30+i], i ≤ 0x26 (end-game stats) | — | matching (read-only) |
| `NapiNPClientMsg_ZoneTimerValue` → `ZoneTimerList_SetEntryValue` | `0x428D60` / `0x537EC0` | S2C 0x6F zone-timer value (16.16 s ×62 → ticks; capture/takeover HUD; ex-"cinematic camera", renamed) | — | matching (decode side) |
| `NapiNPClientMsg_ZoneTimerWindow` → `ZoneTimerList_SetEntryWindow` | `0x428AE0` / `0x537DE0` | S2C 0x53 zone-timer window (+ 20.0-unit nearest-zone adoption gate) | — | matching (decode side) |
| `ZoneTimerList_AdvancePerTick` | `0x537D60` | per-frame value += rate, clamp [0, limit] (consumer HUD_DrawTakeoverStatus @ 0x59B630) | — | matching (read-only) |
| `NapiNPClientMsg_PlaySoundByName` | `0x4283A0` | S2C 0x34 play-sound (profile name + optional 3D pos; ex-"GotoTeleport", renamed) | — | matching (decode side) |
| `NapiNPClientMsg_MissionMapNames` | `0x427E10` | S2C 0x2C session + BMS-file names (ex-"chat entry" note corrected) | — | matching (decode side) |
| `NapiNPServer_HandleChatMessage` | `0x513760` | C2S 0x0D chat uplink (ex-"replication frame ACK" corrected) → S2C 0x14 fan-out, channel routing + 1000 ms rate limit | — | matching (decode side) |
| `NapiNPClientMsg_ChatMessage` | `0x42F240` | S2C 0x14 chat receive → Chat_DispatchToChannel | — | matching (decode side) |
| `NapiNPClientMsg_SessionSlotConfig` | `0x425410` | S2C 0x04 slot config (maxPlayers → PlayerSlotTable_Reallocate) | — | matching (decode side) |
| `NapiNPClientMsg_HandleSessionConfig` | `0x4281D0` | S2C 0x08 session config, fixed 51 B (ex-"~2 KB snapshot" corrected) | — | matching (decode side) |
| `NapiNPClientMsg_HandleJoinResponse` | `0x42E0F0` | S2C 0x02 position-ack + padding probe (client echoes C2S 0x02 + N random bytes) | — | matching (decode side) |
| `NapiNPServerMsg_HandlePlayerLoadout` | `0x515790` | C2S 0x2F loadout submit (soldierType → entity+660 playerClass; restriction mask; → S2C 0x5A) | — | matching (decode side) |
| `NapiNPServerMsg_HandlePlayerSpawnRequest` | `0x513260` | C2S 0x0A spawn-menu request → S2C 0x19 timestamp ack | — | matching (read-only) |
| `Server_ProcessClientRequestRespawn` | `0x519AF0` | C2S 0x0E respawn/deploy request ([i16 spawnHandle], 0xFFFE = auto frontier pick; zone control/team/vehicle-seat gates; **dead-or-pending gate @0x519cc7**; wave-queue else deploy — §5.61) | D-NET-156 | ported (dispatch case 0x0E — pick/gates/deploy + pending clear + computed 0x1E ev-0x3A; waves/vehicle-seat deferred) |
| `Server_OnPlayerJoin` | `0x51A690` | join contract: stateByte=1/3, `|= 0x10` iff SpawnZoneList non-empty (respawn-pending — the deploy-screen hold source), slot state 6, pre-place + 620-tick respawn timer, 0x42→0x0A→0x0F→0x4D→seed | D-NET-156 | ported (`Server_BuildPlayerInfoAndAdd` sets `Connection::respawn_pending` via `world_has_spawn_zone` + the hidden bit) |
| `NapiNPServerMsg_HandleVehicleAttach` / `_HandleVehicleDetach` | `0x502390` / `0x4FC980` | C2S 0x26/0x27 vehicle attach (anti-spoof word0 overwrite @0x502415) / detach (word0 TRUSTED in retail) | D-NET-157 | ported (dispatch cases 0x26/0x27 → `world::entity_process_vehicle_attach/_detach`; detach subject clamped to the sender — hardening divergence) |
| `Entity_ProcessVehicleAttach` → `Entity_AttachToVehicleSlot` / `Entity_AttachToUseGunSlot` | `0x435AA0` → `0x4946D0` / `0x546B80` | attach validation order (dead gates, bone slotType via the model bone table @0x434ED0, `Vehicle_HasEnemyOccupant @0x4359F0`, seat-block occupancy @0x435ba9, detach-first) + the mount writes (Flags 0x40, +0x16C/+0x157/+0x168, stance clear) | D-NET-157 | ported (libs/world/vehicle_attach.cpp; seat classification via the userpoint seat table — tracked divergence; gun path deferred) |
| `NapiNPServerMsg_HandleStanceChange` | `0x501C60` | C2S 0x1D `[i16 stanceCode]`: 169 crouch / 170 prone / 172 stand → MoveOrder bits 8-9 (the ONLY stance replication leg — the 0x0C uplink cannot carry it) | D-NET-159 | ported (dispatch case 0x1D → `Entity::net_stance_bits`; echoed in the 0x0A tail state byte) |
| `Entity_UpdateInfantryPlayerBody` (anim selection) | `0x4B40E0` | authority body-anim selection for EVERY player, every 4th tick: bases 1/11/19 + 8-dir off MoveOrder, idles 43/44/45/48, run/jog 9/10 by ADM gait class, lean 41/42, commit vs `g_animStateFlagsTable @0x8139E8`; feeds the 0x0A record bytes 14/15 | D-NET-159 | ported subset (`AiSystem::remote_player_body_anim` + `mirror_wire_anim`; run/jog + lean + deathAnim variants deferred) |
| `NetPacket_SerializeScoreboard0x16` | `0x504B80` | the 0x16 scoreboard writer (flags byte, rows exclude not-yet-in-game, trailer [inGame][spectator]; 311-tick cadence via `Server_BuildAndBroadcastScoreboard @0x50D960`) | D-NET-158 | ported (`encode_player_list` live counts; in-match row gate) |
| `NetPacket_WriteWorldStateLoad0x0F` | `0x502D10` | the 0x0F body writer (tick, spawn pose, gameFlags bit0 = zones exist, 128-i32 slot-type score table, pool3 block, location names ← def-2044 markers) | D-NET-156 | ported (serialize_world_state_load + location names from the 2044 marker registry) |
| `NetPacket_WriteSpawnWaveStatus` | `0x507490` | S2C 0x6E wave/deploy status; recipient mask includes the respawn-pending bit4 (@0x5074c2); 1 Hz from `Server_TickUpdate @0x51e089` | D-NET-156 | ported (empty-group form at 1 Hz to pending/dead; wave groups with the wave system) |
| `Entity_BuildSpawnZoneList` / `Entity_BuildMapPoiLists` | `0x43EAE0` / `0x42DE40` | the CLIENT-LOCAL deploy picker list (pools 2+1, def attrib 0x40000, sort key zone#/def+406/typePrio) / the command-map POI + per-team lists (ex-"Entity_BuildSpawnPointList" — it never fed the deploy picker) | — | confirm-only (client-side; our `world_has_spawn_zone` is the server-count twin) |
| `NapiNPServerMsg_HandleVehicleSpawnRequest` | `0x51C4C0` | C2S 0x40 vehicle spawn → S2C 0x18 broadcast (mask 0x90) at the source model's boat/helo userpoint | — | matching (read-only) |
| `NapiNPClientMsg_TeamAssign` / `_ScoreDeltaSound` | `0x431910` / `0x42A0B0` | S2C 0x50 team assign / 0x81 score-delta hit-confirm sound | — | matching (read-only) |

Advance & Secure — spawn selection + the zone-capture loop (engine-research 2026-07-03; net-re §5.61;
kong misnomer cluster `CWeaponSlotManager*` renamed `ZoneSlotChain_*` — it manages capture-zone slots):

| original | addr | role | D-NET | status |
|---|---|---|---|---|
| `ZoneSlotChain_BuildFromMission` | `0x4A2DE0` | mission-start zone registration: pool-3 6003/6004/6090/6091 + 6096-6099 markers → per-team assigned slots; pools-1/2 numbered `0x20000` entities → the chain vector | — | ported (`world::zone_chain_build_from_mission`; `zone_chain_test`) |
| `ZoneSlotChain_IsZoneCapturableByTeam` | `0x4A2450` | the AS frontier rule (assigned slot / owned mask / Z±1 adjacency; 0x50010 exempt) | — | ported (`zone_chain_is_capturable`; `zone_chain_test`) |
| `ZoneSlotChain_FindFrontierZone` / `_GetOwnedZoneMask` | `0x4A2AC0` / `0x4A2620` | first capturable zone number per team (0x1E ev-58 hint) / u32 wholly-owned-zone mask (the 0x0F variant-0 dword) | — | ported (`zone_chain_frontier_zone` / `zone_chain_owned_zone_mask`; golden 0x8 pinned in `zone_chain_test`) |
| `Server_ResolveSpawnTargetHandle` | `0x4FE110` | 0x0E pick resolve: pools 0/1/2, def attrib `0x40000`, team gate (teamless passes) | — | ported (`world::resolve_spawn_target`; `zone_chain_test`) |
| `Server_PositionPlayerForSpawn` | `0x50CF60` | spawn placement (ex-`CMap_SetupSpawnCamera`): picked entity pose + userpoint/z+1; numbered zone → round-robin over ≤32 in-radius pool-3 6007 markers; else §5.2c marker chain; revive latch +89932 | D-NET-88 | ported (pick path `spawn_pose_for_target` + per-team `select_player_spawn_for_team`; 6007 scatter/userpoint deferred §5.61) |
| `find_spawn_entity_for_team` | `0x4FC810` | 0xFFFE auto-deploy: owned frontier zone with control ≥ 1.0 (co-op: last team zone) | — | ported (`world::find_spawn_zone_for_team`; `zone_chain_test`) |
| `SpawnWaveList_*` (`g_spawn_wave_list @ 0x24E0E48`) | `0x52A330..0x52AB60` | spawn-wave groups (56-B entries): queue on pick, 1 Hz one-release-per-interval, flush on control<1.0, reset on flip; S2C 0x6E status | — | confirm-only |
| `Server_OnPlayerTouchCaptureZone` | `0x500BA0` | capture request from the physics touch (def `0x20000`; gate un-numbered/owner/control≤0) → presence mark + request queue | — | confirm-only |
| `Server_UpdateCaptureZoneProximity` | `0x5086A0` | 1 Hz proximity bits (`player+89868`), 0x81 score sync, presence counters → `GameEvent_ProcessScoring` | — | confirm-only |
| `Server_UpdateCaptureZoneEntities` | `0x519690` | 1 Hz secure pass: control latch/delta, S2C 0x6F emit, 0x1E ev 0x3B/0x3C edges, in-radius def+88&2 team convert | — | confirm-only |
| `calculate_capture_zone_control_delta` | `0x501120` | control delta: presence sign × 65536/(teamSize×base 12/24/48 ± underdog catch-up ÷ shared-N), clamp [0,1.0], counts → +544/+545 | D-NET-162 | ported (`world::zone_capture_control_delta`, formula pins in `zone_chain_test`; catch-up term deferred) |
| `Server_UpdateCaptureZones` (`captureCtx @ 0xC947A8`) | `0x53B8F0` | 1 Hz timed-capture engine: request queue + 152-B active entries, S2C 0x53 ×4 + 0x6C on presence change; numbered zones flip instantly (control→0) | — | confirm-only |
| `GameEvent_FlagCapture` | `0x50F6F0` | 0x1E flip events: 50/51 (frontier unchanged) or 52/53 (+new frontier), 56/57 banner, 43/44 un-numbered; suppressed post-`GetWinningTeamIfAllOwned @ 0x4A2920` | — | confirm-only |
| `Server_EnforceZoneEntityTeams` | `0x519600` | 1 Hz: numbered entities forced to the owned-mask team (flips co-located `0x40000` spawn objects) | — | confirm-only |
| `Server_ChangeEntityTeam` | `0x518D70` | entity team retarget + S2C 0x50 broadcast (ex-`Server_ChangePlayerTeam`; zones/spawn objects included) | — | confirm-only |
| `NetPacket_WriteZoneTimerWindow` / `_WriteZoneTimerValue` / `_WriteZonePresenceCount` / `_WriteSpawnWaveStatus` | `0x506D00` / `0x506E70` / `0x506DE0` / `0x507490` | the 0x53 (9 B) / 0x6F (15 B) / 0x6C (3 B) / 0x6E payload builders | — | confirm-only |
| `Entity_BuildSpawnZoneList` | `0x43EAE0` | deploy/spawn registry `g_spawn_zone_list` (pools 2+1 `0x40000`), deploy-map AABB, sort (ex-`Entity_BuildSortedRenderList`) | — | confirm-only |
| `apply_session_settings_to_globals` | `0x551500` | host options → `g_capture_duration`/`g_capture_speed_setting`/`g_spawn_wave_time_base+zone`/`g_respawn_requires_team_dead` | — | confirm-only |

Server per-frame S2C 0x0A emit (§5.47; witnessed 2026-07-01; reimpl in `libs/netsim/src/connection_fan.cpp`
+ `netsim::Connection::s2c_phase`; `netsim_two_peer_fanout` `run_0a_subblock_phase_cycle` + shape harness
`scripts/net/diff_0a.py`):

| original | addr | role | D-NET | status |
|---|---|---|---|---|
| `Server_SendEntityStateToPlayer` | `0x517ba0` | per-recipient 0x0A build: `state==6` deploy gate, eye-pos priority ref, build priority list, header + entity loop, budget save/halve, SendFiltered mask 0xA0 | D-NET-134 | header ported; deploy-gate/eye-ref/budget = step 2 |
| `NetPacket_WritePlayerState` | `0x4ff6b0` | 0x0A header: ref pos + state_flags + phase byte (`playerSlot+100566`), `phase&3` sub-block (0 weapon/1 status/2 env/3 gametype), recipient tail | D-NET-134 | phase counter + sub-blocks {1,0,3} ported; env(2)/passenger deferred |
| `serialize_entity_states_to_packet` | `0x50f070` | 0x0A entity loop — all callback entities from the priority list, `[1][handle][type][compact]`, budget-limited round-robin, `[0]` terminator | — | players only; priority/budget/all-class = step 2 |
| `Server_BuildEntityPriorityList` | `0x50e590` | distance-sorted priority pairlist per recipient (eye-pos ref) | — | not yet ported (step 2) |

Wave 8 branch-validation grill (2026-07-01; net-re §8 Wave 8; reimpl in `libs/npwire/protocol_message.{h,cpp}`,
`libs/npruntime/{batch_chunker.h,game_config.h,server_message_dispatch.cpp}`, `libs/netsim/entity_wire_bridge.cpp`,
`connection_fan.cpp`; full ctest 226 green incl. `npruntime_golden_lan_join`, `novaworld_protocol_message`,
`npruntime_batch_chunker`, `netsim_two_peer_fanout`, `nw_message_coverage`):

| original | addr | role | D-NET | status |
|---|---|---|---|---|
| `CNapiNPConnection_SendSessionPacket` | `0x61edd0` | session framing: opcode 0x43/0x83, header `[remote_key][seq][ack][u8 0]`, inner SCRK (TX key conn+0xCC) + outer static NWU key → `frame_session_packet` | — | matching |
| `CNapiNPConnection_ParseMessages` | `0x625bc0` | session deframing: outer/inner decrypt (RX key conn+0x10c), `recv_ack_seq` latch, msg walk → `deframe_session_packet` | — | matching |
| `CNapiNPConnection_BuildOutgoingPackets` | `0x628430` | `++out_packet_seq` per packet; per-packet message assignment (resend-capable); max_packet_bytes ≥26 | — | matching (wire; our frame-at-send model resends none) |
| `CNapiNPConnection_GenerateTxKey` | `0x61dfe0` | 63-char self TX SCRK into conn+0xCC — pins the TX/RX key direction split `SessionCrypto` models | — | matching (read-only) |
| `serialize_pool2_static_to_buffer` | `0x5042F0` | S2C 0x10 pool-2 static batch, 650-B budget margin 40 (was `loc_5042F0` code-island; defined + named this session) | D-NET-135 | matching (model) |
| `Server_PlayerAdd` | `0x51cbc0` | player add: `entity+36 \|= 1` per-entity (remote adds), minimap ids slot+442/444, playerClass [5,9]-else-8 clamp, entity+120 = connection_id, 0x46 broadcast fieldFlags 0x1CF7 | D-NET-136/137 | witnessed (flags model divergence recorded) |
| `lookup_entity_slot_and_pack_entry` | `0x57ad40` | char-registry packer: `type(0-4)\|subtype(5-8)\|index(9-14)\|SIDE(15)` (bit-15 "alive" reading corrected 2026-07-02), param 2 = side matched vs entry+280, 288-B stride, no-match → entry-0 fallback | D-NET-137/148, §5.59 | witnessed |
| `MinimapSlot_FindByPackedId` | `0x57a270` | packed-id → registry-entry lookup (renamed from `sub_57A270`); side-B ids only match side≠0 entries; `MinimapSlot_HasEntity @0x57b140` wrapper | D-NET-137, §5.59 | witnessed |
| `MinimapSlot_FindOrAllocByEntityId` | `0x57b1e0` | blip table find-or-alloc (256×36 B inside `count_and_entries @0x26A7748`, keyed by ENTITY POINTER +28); result ptr → `entity->CharacterEntity` (+0x3C) | §5.59 | witnessed |
| `MinimapSlot_InitBlipFromPackedId` | `0x57b080` | resolves a packed char id and copies the registry entry's descriptor (floats +96/184/272, side +280, avatar +284, triples +44/132/220) into the blip; miss → zeroed + avatar 1 (renamed from kong `HUD_DrawAllMinimapEntities`) | §5.59 | witnessed |
| `NapiNPClientMsg_CharMinimapUpdate` | `0x427D00` | S2C 0x29 apply: `[u8 pool0Idx][u8 team→+354][u8 flags7→+692][u16 packedCharId→NetId+0x15C]` + CharacterEntity rebind (renamed from `handle_entity_minimap_update`) | §5.59 | witnessed |
| `NapiNPClientMsg_HandlePlayerSpawn` | `0x431BB0` | S2C 0x51 apply: FIELD-PARSES 8-B team-change confirm, acks C2S 0x29 (ackSeed+1 @0x431c99), REBINDS CharacterEntity @0x431cf3 — refutes the "spawn signal only" claim | D-NET-148, §5.59 | witnessed |
| `NapiNPServerMsg_0x029` | `0x514F10` | C2S 0x29 handler: `[u16 idx]` → `g_team_change_entity_list @0xC947C8` lookup; 0x51 reply ONLY for a pending entry (`write_entity_packet @0x506bb0` body, gates `!g_net_spawn_suspended && !g_spawn_success_gate`); plain join → no reply | D-NET-148, §5.59 | matching (`npruntime_handshake_server` no-0x51 pin) |
| `Server_HandlePlayerDisconnect` | `0x51B5C0` | disconnect teardown: team spawn-token return, 0x32 minimap-slot removal (mode 2), 0x6A squad, slot memset + 0x46 fieldFlags-0x1CF7 re-serialize → 0x8000 removal broadcast (mask 128) | D-NET-149 | matching (despawn + 0x46 removal; 0x32/0x6A/token deferred) |
| `NetPacket_SerializePlayerSync0x46` | `0x505e80` | 0x46 body: 0x8000 removal, 0x4000 ECHO from request, `fieldFlags & 0x7FFF` pass-through, per-bit fields (slot-state sources pinned) | D-NET-127 | matching (echo fix applied; per-field values remain approximated) |
| `NapiNPClientMsg_PlayerSync` | `0x431370` | client 0x46 handler: variable-length strings, removal = no entity byte, ack-walk re-request `[slot+1][0x5CF7]` while `< g_max_player_slots` | — | matching (decode side) |
| `CNapiServer_ProcessPendingPlayerSpawns` | `0x4c8dc0` | spawn pump: team-balance gate; emits 0x03 → PlayerAdd → 0x05 → 0x04 → 0x7B | D-NET-127 | matching (order + bodies) |
| `NetPacket_WriteWeaponRestrictionFlag` | `0x502ac0` | 0x03 body `[u8 1][u16 node count][u16 weapon mask]` / `[u8 0]` | D-NET-127 | witnessed |
| `NetPacket_WriteBoolTrue` | `0x502c00` | 0x05 body = `{0x01}` | D-NET-127 | matching |
| `NetPacket_WriteSlotAssignment` | `0x502b30` | 0x04 body (24 B): stat dwords, g_mode, player_slot, slot capacity, team → `build_tag04_slot_assignment` (rename from `sub_502B30` proposed) | D-NET-127 | matching |
| `CNapiNPConnection_SendConfigUpdate` | `0x6286e0` | settings-flag 0x00 msg: `[u8 dir==0][u32 mask][u32 cs value per bit]` | D-NET-127 | matching |
| `write_entity_packet` | `0x506bb0` | 0x51 spawn confirm: `[u16 hdr][u16 handle][u8 team][u16 NetId][u8 animSlot]` when Flags&0x100 | D-NET-137 | layout matching; NetId/anim zeros tolerable |
| `Entity_SetHealthFromDifficultyByte` | `0x4AD580` | client apply of the 0x0A player field-17 byte: `[tier(4-5) \| playerClass(0-3)]`, tier ≈ 21.9/59.4/87.5 % of healthMax; the read path then re-resolves itemDef FROM playerClass (@0x4c1248 — the 0x0F-flood re-break, §5.46) | D-NET-138 | witnessed (apply side) |
| `Entity_GetHealthClassification` | `0x4AD4E0` | server-side field-17 pack: `(tier<<4) \| (playerClass & 0xF)`, tier from `(Health<<16)/max(healthMax,1)` vs boundaries 49152/28671; called from the case-1 compact write @0x4c0d71, stored @0x4c0d89 | D-NET-138 | matching (`netsim::health_classification_byte`; boundary tests `netsim_two_peer_fanout`; live v11: 0 C 0x0F) |
| `NapiNPServerMsg_HandlePlayerLoadout` | `0x515790` | C2S 0x2F: parse class/type/slot + adm entries, validate (charfilter/teamfilter masks + armory-enable `unused6`), clamp type [5,9]-else-8, stamp entity+660, rebuild weapon slots, reply via 0x502550 | §5.57 D-NET-141 | ported (derived reply; masks/armory validation pending weapon.def table) |
| `Server_SendWeaponSlotListToPlayer` | `0x502550` | S2C 0x5A builder: walks the 780-slot table in AdmDef-index order, per slot `[admIdx][ammo][alt][restriction]`, 0xFF terminator | §5.57 | ported (set+order matching; ammo bytes echoed — D-NET-141) |
| `NapiNPClientMsg_HandleWeaponLoadoutSync` | `0x4290E0` | client 0x5A apply: per-slot AdmDef validate, display list, WeaponSlotTable_LoadAllFromDefs, ammo clamp `min(byte, maxclips)×clipsize` else startrounds fallback, local model re-resolve, Player_SelectWeaponSlot/MountWeaponSlot | §5.57 | witnessed (apply side) |
| `AdmDef_GetEntryByIndex` / `AnimDef_InitAll` | `0x53FC80` / `0x5435C0` | the weapon.def table: 255×1120 B @0x24E7FE0, "null" entry 0, loaded per mission via File_ParseASCIIFile @0x53D810 (SCR key 0x2A5A8EAD); field map §5.57 | §5.57 | witnessed (parser port pending; index allocation OPEN) |
| `NapiNPServerMsg_HandleReloadRequest` | `0x514DF0` | C2S 0x25 `[u16 handle][u16 combo]` -> broadcast S2C 0x49 (same payload) + host-side WeaponSlot_ReloadAmmo (remote requesters only, @0x514f03) | §5.58 D-NET-142 | ported (relay + clip refill; pool refund deferred) |
| `NapiNPServerMsg_0x006_ClientFiredRound` | `0x513310` | C2S 0x06 handler: authority/cease-fire/slot gates, anti-spoof (claimed shooter == slot entity @0x51358d), PlayerSlot_IsActive fire-rate gate, fireRequest build, cooldown stamp slot+96472 = tick + adm[276] | §5.16 D-NET-152 | ported (dispatch case 0x06; cooldown stamp deferred — adm[276] unparsed) |
| `Server_ClientFiredRound` | `0x50baa0` | the fire validator: guards −3..−18, WeaponSlot_CanFire ammo gate, savedLivePose warp comp, equipped-adm + fire-target stamps; net primary → the adm 'fire' action, alt/local → RoundData_AddRound | §5.16 D-NET-152 | ported (validate subset at reimpl altitude; spawn/damage + pools deferred) |
| `WeaponSlot_CanFire` | `0x541ba0` | can-fire: weapon-child busy, underwater ban, clip u16 slot+16 / adm+220 pool, adm+224 score-lock (ex `should_send_entity_update`) | §5.16 D-NET-152 | ported (clip check; pools/score-lock deferred) |
| `WeaponAction_Fire` | `0x542b10` | the adm 'fire' action (admEntry+684, action table +676 / suffix table 0x830B94): latched-pose fire position, Entity_FireWeaponAndSendPacket, consume_weapon_ammo, burst counter, RECOIL chain | §5.16 D-NET-152 | ported (§5.62 `world::weapon_fsm` fire handler; the §5.16 net dispatch still collapses the re-entry inline) |
| `WeaponAction_ProcessFrame` | `0x540e60` | THE per-tick weapon-action FSM pump: counter tick-down, idle reseed, overheat deny, rescope-after-reload, the phase {4,0}/0x40 transitions into `Def->actionTable[next]` | §5.62 | ported (`world::weapon_fsm_tick`) |
| `WeaponAction_ProcessAllEntities` | `0x542690` | the frame driver: pumps every pool-0 equipped slot + pool-1 unmounted weapons with a live muzzle flash | §5.62 D-WPN-6 | ported (local player's slot only) |
| `Anim_InitActions` | `0x541fa0` | binds the 12 action slots (Def+0x2A4) by `<weapon>_<suffix>` lookup vs the default table @0x830B90; bakes `auto` delays from Anim_GetDurationTicks; plays global 241 | §5.62 | ported (`weapon_fsm_bake`) |
| `ActionDef_ParseScriptLine` | `0x4023c0` | the ACTION block parser: find-or-create by prefixed name, keys function/anim/delaystart/delayend (+ the bare `delay` alias → +40)/soundset/particle/ctrlreg/...; a nested `action` while one is open → "forgot an end" @0x402409, row NOT created, old row stays current (NO implicit closure) | §5.62 | ported (libs/def rows + the bake; refusal = fall-through to raw_lines; FUNCTION registry D-WPN-1) |
| `WeaponDefs_ParseLineCallback` | `0x543680` | the weapon.def line driver: `weapon`/`end` blocks, key dispatch, the in-ACTION latch `g_weaponParseInActionBlock @0x252DB88` forwarding every in-block line to the ActionDef parser BEFORE the `action` open dispatch (@0x54388d); stores the opened row per-suffix at WeaponDef+0x2A4; weapon `end` → Anim_InitActions | §5.62 | ported (libs/def `parse_weapons_buf` ST_WEAPON/ST_ACTION) |
| `ActionDef_InitDefaults` | `0x4022b0` | memset(0xCC) + handler=placeholder, anim slot (+24) = −1, +32 = 0x2000 — absent delaystart/delayend default to 0 (only explicit `auto` writes −1) | §5.62 | ported (rows memset in libs/def; bake defaults) |
| `ActionDef_GetCurrent` | `0x401ef0` | returns `g_currentActionDef @0xA2E8E8` — the row opened by the last `action` line (ex `sub_401EF0`) | §5.62 | witnessed (driver plumbing) |
| `AnimMap_FindSlotByName` | `0x40cfa0` | anim-name → global slot: stricmp (CASE-INSENSITIVE) from name+5 (skips `anim_`) vs the unprefixed 252-entry `g_animStateNameTable @0x8135F0` | §5.62 D-WPN-10 | ported (`NovaSkeletalAnim::find_clip` nocasecmp, fixed 2026-07-10) |
| `ActionFuncDef_FindByName` | `0x401040` | the FUNCTION-name registry lookup over `g_actionFuncDefTable @0x829E58` (18 rows: wpn_std_* + *_map + powerup_*) | §5.62 D-WPN-1 | witnessed (std-only in all shipped data; registry unported) |
| `ActionSlot_BeginActivePhase` | `0x53f830` | phase 1(|0x40)→2 + play the action's anim slot on the equipped WeaponDef's OWN adm channel (`WeaponDef+0x174`, the FP viewmodel rig; CORRECTED 2026-07-09 from "owner's adm") (LOCAL player only) + sound/ctrlreg; phase-2 path advances the channel (the effect shims @0x541860/@0x5419e0 share protocol and play target) | §5.62; world-wac-ai-re §14.8.3 | ported (`begin_active`) |
| `ActionSlot_FinishActivePhase` | `0x53f7b0` | counter=delayEnd, nextAction=arg4, the ACTIVE→DONE kick bump (skipped for RELOAD), phase=4 | §5.62 | ported (`finish_active`) |
| `WeaponAction_Idle` | `0x542920` | the idle LOOP handler (also OVERHEATED's default): replay global 241 on entry, empty-mag → auto-reload / EMPTYIDLE + one-shot unscope | §5.62 | ported |
| `WpnAction_Recoil` | `0x542dd0` | THE ARBITER at clip end: burst refire / idle / auto-reload (reserve ≥ clip) / emptyidle + one-shot unscope + def+0x168 auto-switch; held-trigger re-queue of binding 149 | §5.62 | ported |
| `WeaponAction_Reload` | `0x5430b0` | first tick: C2S 0x25 + phase|=0x80 + scope stash/unscope; clip end → Finish(next=IDLE), burst=0 | §5.58/§5.62 | ported (authority zero-latency refill; joiner uplink D-WPN-8) |
| `WeaponSlot_RequestFire` | `0x53efa0` | the fire request (ex `sub_53EFA0`): AUTO {0,3,9,10}→FIRE / {1}→EMPTY / {2}→defer; SEMI {0}→FIRE / {1}→EMPTY; charge byte slot+0x5C | §5.62 | ported (`weapon_fsm_request_fire`) |
| `Player_RequestPrimaryFire` | `0x5414c0` | RequestFire(slot, 149, 0) wrapper (ex the kong-misnamed `Terrain_UpdateColorInterpolation`) | §5.62 | ported (the host's fire input path) |
| `WeaponSlot_RequestReload` | `0x53f110` | queue RELOAD when no 0x80 pending && next ∈ {0,1,11} | §5.58/§5.62 | ported |
| `WeaponSlot_TryQueueScopeUp` / `..Down` | `0x53f050` / `0x53f080` | queue SCOPEUP(9)/SCOPEDOWN(10) when phase ∈ {0,4} else idle (ex the "TryQueueReload/Unload" misnomers) | §5.62 D-WPN-9 | ported |
| `Player_ToggleWeaponScope` | `0x4df0c0` | the ADS toggle: Flags&3 + interp gates, g_scopeEngaged, the 15/7-step camera interp toward AltCamOffset (tpos), FOV 80/elevation (Flags&2), scope-state C2S 0x1D | §5.41/§5.62 | ported (host scope state + eased tpos + FOV; residuals D-WPN-9) |
| `Player_SwitchToWeaponByHandle` | `0x4e0170` | weapon select by category×65+rank: stance gate, category scan over the 100-B `weaponSlotArrayBase @0xB75FD4` (eligibility def+932 type 1/2 / kill-score, !(def+12&1)), deny-sound wrap → MountWeaponSlot | §5.62 D-WPN-5 | witnessed (the loadout slice ports the pool + swap) |
| `Player_MountWeaponSlot` | `0x4dfa40` | the mount: g_pendingWeaponSlot write @0x4dfb16, same-category → TryQueueSwitchRank(8) / cross → ForceQueueSwitchFrom(7), mount-scoped auto-engage (Flags&0x20000000), view-bias zero + UpdateFirstPersonCamera, seat 0x40000 → C2S 0x1D/169 | §5.62 D-WPN-5 | witnessed |
| `WeaponSlot_ForceQueueSwitchFrom` / `..TryQueueSwitchRank` | `0x53f170` / `0x53f1c0` | queue 7 (with the hard slot reset) / queue 8, phase-gated {0,4,0x40} (ex `sub_53F170`/`sub_53F1C0`) | §5.62 D-WPN-5 | witnessed |
| `Anim_GetDurationTicks` | `0x53ee10` | clip trunc(ms × 62.5/1000 + 0.5) + 1 → the FSM's 62.5 t/s delays (62.5 = `flt_7C3B3C`, 0.5 = `flt_7C3B94`; byte-witnessed 2026-07-10) | §5.62 | ported (`weapon_anim_ticks_from_ms`) |
| `Entity_FireWeaponAndSendPacket` | `0x42bd80` | the shared fire entry: authority → LOCAL-mode Server_ClientFiredRound; client → RoundData_SpawnRound (own-fire prediction) + C2S 0x06 send | §5.16 D-NET-152 | witnessed |
| `consume_weapon_ammo` | `0x540850` | ammo decrement: clip u16 slot+16 (adm+220 == 0), else per-player pool playerSlot+89176+4*adm220 / global data @0xB761E8 (AI), vehicle store via mode 3 | §5.16 D-NET-152 | ported (clip path; pools deferred) |
| `WeaponSlot_ReloadAmmo` | `0x541720` | host+client clip refill: clears slot+90 0x80, refunds clip×adm224 to the adm+216 pool, refill = capacity(adm+88)×cost clamped by the pool → slot+16 | §5.58 D-NET-142/152 | ported (refill-to-capacity; refund/clamp deferred) |
| `RoundData_AddRound` | `0x4fdb40` | the ONLY g_round_ring writer: 36-B record (stat, SHOOTER handle, pre-spread origin + direction BAMs, shot-seq, mode/subtype/slot/adm bytes) + inline RoundData_SpawnRound | §5.9.1 D-NET-152 | ported (`world::RoundRing` + the synchronous world::RoundSim spawn, §5.60) |
| `RoundData_SpawnRound` | `0x4ec0d0` | spawns the authoritative round: score multiplier, tracer interval adm+226, ammo-class dispatch (trail/hitscan 0x400, guided 0x2000000, bursts 0x1/2 0000), projectile-pool entity + spread + velocity (ex `RoundData_ProcessHit`) | §5.9.1 D-NET-152 | ported (world::RoundSim spawn — MVP §5.60; npruntime_round_sim_test) |
| `Server_BuildRoundEventListForPlayer` | `0x4ffee0` | per-recipient round staging: watermark playerSlot+97544 (arm gate), own-shooter skip @0x4fff97, line-of-fire proximity score (2π/2^32 BAM, 2^22 trig, 1000u clamp, z half-weight, 0x4000−lateral>>12), top-255 → g_round_event_refs (ex `compute_entity_angular_priority`) | §5.9.1 D-NET-152 | ported (netsim `select_round_events`) |
| `NetPacket_SerializeRoundEvent` | `0x504820` | the §5.9.1 tag-2 writer: flags gates (0x40 live fire-target, 0x80 slot byte), SHOOTER handle, shot-seq, origin compressed vs g_priority_ref (recipient eye), direction BAM high words (ex `serialize_projectile_to_packet`) | §5.9.1 D-NET-152 | ported (`encode_round_event_record` + netsim conversion) |
| `NetPacket_DeserializeRoundEvent` | `0x42f270` | the client read side (ex `NetPacket_DeserializeWeaponHit`); dispatches into RoundData_SpawnRound — the receiving client re-simulates the round | §5.9.1 D-NET-152 | matching (`decode_round_event_record`; byte-witnessed 2026-06-16d, relabeled) |
| `Weapon_UpdateAllProjectiles` | `0x4ec020` | the per-tick live-round iterator → Projectile_UpdatePhysics per round | §5.60 | ported (world::RoundSim — MVP altitude; npruntime_round_sim_test) |
| `Projectile_UpdatePhysics` | `0x4e9d70` | per-tick round step: velocity advance, segment ray (terrain hi-res + entity proximity + water), drag, 5-way hit switch → impact handlers; min proximity radius 0.1u @0x4ea263 | §5.60 | ported (world::RoundSim — MVP altitude; npruntime_round_sim_test) |
| `Weapon_RaycastAndSpawnImpact` | `0x4e8460` | the hitscan leaf (adm halfword 22 == 1): ray to weaponDef+56 range, terrain/entity/water hit class, impact effect + sound — EFFECTS ONLY, no damage call | §5.60 | confirm-only |
| `Projectile_HandleEntityImpact` | `0x4e9390` | entity hit: vehicle parent-chain resolve, penetration budget (+676−+684 vs ammo dword 3) + child-ammo spawn (ammoDef+241), type-15 bone/section damage, pass-through 0x18000000 → ProcessDamageOnTarget | §5.60 | ported (world::RoundSim — MVP altitude; npruntime_round_sim_test) |
| `Projectile_ProcessDamageOnTarget` | `0x4e7fb0` | the bullet damage gate: CalcImpactDamage, zeroing gates (indestructible/armor-class ammoDef+196 vs itemDef+400/already-dead +292/occupant scale), AUTHORITY-ONLY health −= damage @0x4e8127 + team matrices, kill → Score_ProcessKillEvent | §5.60 | ported (calc_impact_damage / the authority health apply — MVP zones; npruntime_round_sim_test) |
| `Weapon_CalcImpactDamage` | `0x4ec920` | KINETIC damage: authority-only (returns 0 else @0x4ec933), zone multiplier (head 0-4 flag 0x100 / body / limbs / special 13-14 flag 0x800 / vehicle seats), (62·speed)>>16 clamp 1219, g_OneShotKill 2000, ammoDef+192 max clamp — falloff emerges from drag | §5.60 | ported (calc_impact_damage / the authority health apply — MVP zones; npruntime_round_sim_test) |
| `Projectile_ProcessExplosionQueue` | `0x4ead80` | the AoE queue: fn-ptr tables @0x4eadd0/@0x4eae44 → Entity_ApplyWeaponDamage | §5.60 | confirm-only (deferred behind bullet damage) |
| `Entity_ApplyWeaponDamage` | `0x4e6820` | explosion damage applicator: authority gate + g_destroy_buildings, team attrib 0x8000 protection, weaponDef+46 base, LINEAR blast falloff, infantry/vehicle section paths | §5.60 | confirm-only (deferred behind bullet damage) |
| `Score_ProcessKillEvent` | `0x4fd400` | authority-gated scoring dispatch (gametype scoring + SP classify) — emits NO messages | §5.60 | confirm-only |
| `Entity_CheckAndProcessDeath` | `0x51b550` | the death router (from the per-tick infantry updates @0x4b40e0/@0x4b9910 on health<=0): Flags&0x100 → GameEvent_PlayerDeath, else AI S2C 0x13 + scoring | §5.60 | ported (route_round_deaths 0x13/0x1E + respawn queue — MVP; npruntime_round_sim_test) |
| `GameEvent_PlayerDeath` | `0x516dd0` | the player-death orchestrator [authority]: live-target clears, S2C 0x13, respawn timer (620-tick rule, floor 3), scoring, kill-type classify (headshot 0x100/vehicle 0x800/knife 0x400/explosive 4091/4093/4095/...), S2C 0x52 ×2 + 0x1E kill feed + 0x54 ×2 | §5.60 | ported (route_round_deaths 0x13/0x1E + respawn queue — MVP; npruntime_round_sim_test) |
| `Server_KillPlayerAndNotify` | `0x519e00` | slot dead-mark (+100567, entity+292=−1) → Server_ProcessPlayerDeath; optional S2C 0x32 sub 5 (player name) | §5.60 | ported (route_round_deaths 0x13/0x1E + respawn queue — MVP; npruntime_round_sim_test) |
| `Server_ProcessPlayerDeath` | `0x517740` | death→respawn: killer resolve, detach (+0x40000 seat re-attach), spawn camera, Entity_ResetToSpawnState, scoring, weapons re-init + 0x5A resend, 0x1E, KRBP marker | §5.60 | ported (the respawn release — spawn-point snap + template health; npruntime_round_sim_test) |
| `Server_SendEntityStatePacket` | `0x509d70` | the SINGLE S2C 0x26 emit (entity handle+team, mask 0x90, tick stamp +560); staged by all 16 non-player death/destruction handlers | §5.60 | ported (route_round_deaths 0x13/0x1E + respawn queue — MVP; npruntime_round_sim_test) |
| `NapiNPServerMsg_HandleSectorAction` | `0x514330` | C2S 0x13 = pool-3 def-type-2044 sector actions — NOT a death message (table row corrected) | §5.60 | confirm-only |
| `AmmoDef_LoadAll` / `AmmoDef_ParseProperty` | `0x40b0b0` / `0x40a2d0` | the ammo.def table: 276-B records @0xA2ECE8, SCR key 0x2A5A8EAD, two-pass count→parse; token→offset map §5.60 (flags/velocity/max_age/arm_age/error/drag/kztype/damage/penetration/tracerRate/notarmmedammo) | §5.60 | ported (`libs/def def_parse_ammo` extended subset + `world::AmmoTable`) |
| `NapiNPClientMsg_WeaponReload_0x049` | `0x42C0A0` | client 0x49 apply: local -> WeaponSlot_ReloadAmmo @0x541720 (the ONLY client clip refill; clears the 0x80 pending flag), remote -> 3P reload anim | §5.58 D-NET-142 | witnessed |
| `Entity_UpdateVehiclePhysics` (family) | `0x48AF00` | vehicle damage states off entity+286 vs itemDef: wreck at hp<=0, burn `0 < hp <= criticalHp(+0x180)` (+ authority drain +0x182 every 64t), smoke `< healthMax>>2`, regen (+0x184); THE DRIVE CORE: attrib&0x40 occupant(+0x170) gate `(Flags&0x100) && (local \|\| authority)`, 8-way input block, speed-scaled steer chase, cos²-slope speed, accel branch tree, gravity −324/t | §5.13, D-NET-161 | ported (ground-family authority core = `world::tick_vehicle_motor`; `vehicle_motor_test` + `netsim_two_peer_fanout`) |
| `ItemDef_ParsePhysicsProperty` | `0x49D870` | items.def physics block scaled at parse: turn_rate deg/s×192426 BAM/tick, player_speed km/h×293 16.16-u/t, slopes deg×11930464, accel/decel ×4 (decel default 2×accel) | D-NET-161 | ported (libs/def `parse_items_buf`; scaling pins in `vehicle_motor_test`) |
| `Entity_SerializeVehicleState` | `0x460560` | the §5.13 vehicle record, modes 1/2 only (3/4 → −1: NO vehicle uplink exists); flags&4 short form = the DEAD-pose (wreck euler) form; renamed 2026-07-04 from Entity_SerializeMountedVehicleState | §5.13, D-NET-63/161 | ported (encode/decode; nw_pp `DEAD-POSE`/`live` labels) |
| `Entity_DispatchPhysics_cveh` | `0x48EFC0` | cveh tick: `itemDef->physics(+0x8DC)` non-zero → Entity_UpdateVehiclePhysics else infantry physics | D-NET-161 | ported (the traits `physics` selector gate) |
| `EntityAI_ProcessVehicleStateMachine` | `0x4583C0` | the cveh class tick: per-state enter/tick/exit tables @0x815238/3C/40; authority runs all states, clients only 21/23 | — | confirm-only (non-drive states deferred, D-NET-161) |
| `CEffectEmitter_ReleaseSafe` | `0x5F69F0` | guarded emitter destroy (CEffectSystem singleton alive → release handle); consumes vehicle +0x1CC/+0x400 emitter slots at detach/respawn; renamed 2026-07-04 from the CNapiSession_FlushSendSafe kong misname | §5.13 | confirm-only (host is headless — effects are client-side) |
| `Server_BuildOverlayStateForPlayer` | `0x517FC0` | the 0x40 producer: per-slot resumable pools-2/1/0 walk → Entity_ClassifyForMinimap → 16-entry staging flush per recipient | §5.19, D-NET-162 | ported (zone + vehicle-blip subset at 1 Hz; budget walk + player/crate entries deferred) |
| `Entity_ClassifyForMinimap` | `0x50FA70` | icon/color classify: team 1→0x0a, 2→0x09, else 0x0c; capture/spawn attribs → icon 0; vehicles by unitType (5-8→15, 3/4→11, 12→25, else 10); persons 3/8; armory 13; eweap 4/12 | §5.19, D-NET-162 | ported (zone + vehicle rules; rest deferred) |
| `Server_UpdateCaptureZoneEntities` | `0x519690` | the 1 Hz secure pass: enemy-frontier latch → control delta → 0x6F emit @0x5197D9 + 0x1E 0x3B/0x3C edges @0x519839/@0x51988E | §5.61, D-NET-162 | ported (`zone_capture_tick` secure pass) |
| `Server_UpdateCaptureZones` | `0x53B8F0` | the timed-capture engine + queue drain: numbered zones flip INSTANTLY (via neutral when owned, control=0); 0x53 emits @0x53BA36/@0x53BA68; un-numbered zones open timed active entries | §5.61, D-NET-162 | ported (the instant-flip drain; timed entries + 0x6C deferred) |
| `GameEvent_FlagCapture` | `0x50F6F0` | flip events: 50/51 frontier-held / 52/53 with new frontier numbers (team-filtered), 56/57 banner; suppressed once GetWinningTeamIfAllOwned | §5.61, D-NET-162 | ported (the npruntime flip emits) |
| `Server_EnforceZoneEntityTeams` | `0x519600` | forces zone-numbered entities onto the owned-mask team (team-1 precedence) — flips co-located spawn objects | §5.61, D-NET-162 | ported (non-trigger objects only — a trigger forced from its sibling's mask bit would snap a fresh neutralize back) |
| `PlayerClass_InitEntity` | `0x4B1116` | player spawn anim defaults: +0x2B0 = AvatarDef_FindIndexByName("WPN_M4AUTO"), +0x2BC = +0x2C8 = 0x2B (43, idle) | §5.10 D-NET-143 | witnessed (the 0x0A off-14/off-16 spawn defaults our host must emit) |
| `apply_session_settings_to_globals` | `0x551500` | settings-UI → live rule globals copy; names the five 0x08-block dwords | — | witnessed (read-only) |
| `Config_ParseSettingsLine` | `0x54f740` | cfg-file setting names: `replay`/`max_team_lives`/`timeout`/`destroybuild`/`deathmes`/`mp_allowsniperscopezoom` | — | witnessed (read-only) |
| `ServerConfig_ApplyHostSetting` | `0x4a6000` | SET-command → `dword_2550A04` mpattrib bitfield (`TeamChoose` bit 0x4, `TeamFF`/`FriendlyTag` inverted 0x200/0x400) | — | witnessed (read-only) |

Kill feed + replay event/environment streams (§5.26/§5.27; one-host/one-client capture 2026-06-17;
`nw_pp` printers + `libs/novaworld` decoders, `nw_replay_timeline` `test_event_stream`):

| original | addr | role | D-NET | status |
|---|---|---|---|---|
| `NetPacket_HandleGameEvent` | `0x426270` | S2C 0x1E game event / kill feed — 8-B body (type/attacker/victim/aux/pos); `event_type`→`STRCNDnn` switch ported (`game_event_kind`/`_strcnd_key`); byte-witnessed `04 05 04 ff`=type-4 kill | — | matching |
| `NapiNPClientMsg_0x026` | `0x42EC30` | S2C 0x26 entity kill — `[u16 victim_slot][u16 attacker]` → `Entity_KillBySlotId` | — | matching |
| `NapiNPClientMsg_HandleBatchSpawn` | `0x431870` | S2C 0x4E batch despawn — count + per-slot kill (Kong-misnamed "spawn") | — | matching |
| `Entity_KillBySlotId` | `0x42BCE0` | arg0 = dying entity, arg1 = attacker — disambiguates the 0x26 field order | — | host code / read-only grill |
| `HUD_FormatKillEventMessage` | `0x422DA0` | kill-feed name/clan formatting (`$A`/`$B` tokens) — confirms 0x1E actor roles (killer/victim/aux) | — | host code / read-only grill |

## 5.6 Single-player listen-server bring-up (engine-research, 2026-06-16)

Witness that JO single player is an in-process listen server (host + client, socketless transport
mode 1). Originals only — none reimplemented yet; these gate a future in-process listen-server
(single-player) host. Findings landed in
[net/novaworld-net-re.md §5.0](net/novaworld-net-re.md).

| original | addr | role | evidence | status |
|---|---|---|---|---|
| `SinglePlayer_StartMission` | `0x561af0` | SP launch: `SetConnectionMode(3)` + `SetTransportMode(1)` + `CreateSession` → "Game Loop" | server_name `"SINGLEPLAYERGAME"`; decompile | confirm-only (not reimplemented) |
| `CGameSession_SetConnectionMode` | `0x4c49f0` | conn mode → `is_authority`/`is_mp_session_peer` (1=host, 2=client, 3=host+client) | switch writes +0x5C/+0x60/+0x64 | confirm-only |
| `CNapiNetwork_SetTransportMode` | `0x4c8750` | socket-state field; opens a UDP socket only for modes 2/3/4 (SP=1 → no socket) | `OpenTransportSocket` gate | confirm-only |
| `CNapiGameSession_CreateSession` | `0x4c97c0` | shared SP/MP creator: installs host callbacks, StartServer, local client conn (type 2) | decompile; host-callback installs | confirm-only |
| `NapiNPProtocol_StartServer` (Kong `sub_62B5E0`) | `0x62b5e0` | host bring-up: session key + `host_start_tick` + "HOST STARTED" log (§6.5) | callees `NapiNP_GenerateSessionKey`/`GetTickCount`/`LogHostStarted`; **renamed in IDB 2026-06-16** | confirm-only |

Host-side spawn flow (R1, 2026-06-16; net-re §5.2a):

| original | addr | role | evidence | status |
|---|---|---|---|---|
| `Server_InitNewRoundState` | `0x51c8e0` | host new-round init: player-slot table + local-player ctx (name from `CHAR` var iff `transport_mode==1`) + clears timeout gate | decompile; §5.2a | confirm-only |
| `CNapiServer_ProcessPendingPlayerSpawns` | `0x4c8dc0` | server spawn acceptor (gated `is_authority && !gate`): builds player entity, sends spawn msgs 3/5/4/0x7B, game-state 8 | decompile; §5.2a | confirm-only |
| `Server_BuildPlayerInfoAndAdd` | `0x51d560` | builds the 226-B player-INFO buffer (name/flags/JSP/PCID/squad); delegates entity registration to `player_ServerAdd` | called by `0x4c8dc0`; §5.2b | confirm-only |
| `player_ServerAdd` | `0x51cbc0` | player-SLOT manager: memsets the 100584-B slot, `Server_AssignPlayerTeam`, ServerLog Name/IpPort/PCID/Team/Type records, holds `g_local_player_entity` | decompile; §5.2b | confirm-only |
| `Entity_InitFromItemDef` | `0x49e550` | item-template → entity field copy: `entity+28` idx → `gItemDefs[idx]`; +286 healthMax, +288 armorMax, +48/52/56 graphic/husk/huskFinal **model ptrs**, +432 timing, +452/456 update/death callbacks; dispatches item init cb | decompile; world/itemdef-re.md | confirm-only |
| `Entity_SpawnFromBMSRecord` | `0x40e9f0` | BMS placement record → entity: pos/yaw(90−)/team(73)/commandGroup(78); **ammoCount(+290) ← byte 81, refNum(+533) ← byte 153, weaponByte(+538) ← byte 155**; BMS attrib dword[3] → entity Flags (Indestructible 1<<21 → 0x4000000, Reflective 1<<23 → 0x400, NoShadow 1<<24 → 0x1000000) — the 0x10 static record's entityFlags sources | decompile; §5.9 / D-NET-147 | MATCHING (behavioral proof: `npruntime_initial_state_burst`) |
| `Entity_InitFromModel` | `0x40dc30` | model+def → entity init: userpoint resolve, bbox/boundRadius; **Flags composition**: attrib&0x100 → 0x8000000, healthMax==0 → 0x4000000 + subType(+532)=0xFF + Health=1, type Building/Decoration/Powerup/no-update-cb → 0x20000, type Vehicle → 0x400 | decompile; §5.9 / D-NET-147 | confirm-only |
| `ItemDef_ParseProperty` | `0x49eb00` | `items.def` property dispatcher → `gItemDefs[idx]+off`; primary source of the `ItemDef` field map, `type`/`attrib`/`attrib2` enums | decompile; world/itemdef-re.md (D-ITEMDEF-1) | confirm-only |
| `EntityDef_LoadModelsAndCallbacks` | `0x439f50` | loads model name strings → model ptrs (`0xf0…0x12c`), binds bone callbacks, resolves seat/control/gun bones from model user-points | decompile; world/itemdef-re.md | confirm-only |
| `ItemDef_ResolveAllResources` | `0x49e5f0` | resolves the `pad_270` sound block (death/door/shot-TOD/`soundloop[7]`) and anim slots to ids | decompile; world/itemdef-re.md | confirm-only |
| `Entity_ResetToSpawnState` | `0x4B9610` | spawn/respawn reset: backs up pos as spawn point, splats Yaw across heading fields, **clears `Flags & 2` (entity+36 movement gate)**, zeroes vel/AI refs, detaches vehicle, removes pool 0/1 cross-refs | decompile; §5.2b / §5.6 | confirm-only |
| `Server_SendInitialGameStateToPlayer` | `0x51bba0` | **server-side source of the S2C loading sequence** (0x2C/08/2A/1C/0B/66/76/11 + 0x10/0D/0C/20/45/7E/1A) → game-state 9; player-sync tail parks at sync-state 3 (`@0x51c134`); both tracks stall on `conn+0x768 >= 20` sent-unacked (`@0x51bbfd/@0x51bf14`) | decompile; §5.2a (P6 emitter spec) / D-NET-150 | MATCHING (behavioral proof: `npruntime_initial_state_burst`) |
| `NapiNPServerMsg_HandlePlayerSpawnRequest` | `0x513260` | C2S 0x0A spawn-menu request = **the world-stream unlock**: game state 9, session+32 → 4, world-stream phase reset, S2C 0x19 reply | decompile; §5.2a / D-NET-150 | MATCHING (behavioral proof: `npruntime_initial_state_burst`, `npruntime_client_runtime`) |
| `CNapiNPConnection_ParseMessages` | `0x625bc0` | inbound session-packet parse: 13-B header (seq/+4 ack_seq/reserved); ACK SWEEP `@0x625d9b` destroys sent msgs with `msg_seq <= ack_seq` → decrements the conn+0x768 sent-unacked count; peer-ack high-water at conn+0x7b0 (`@0x625dbe`) | decompile; D-NET-150 | confirm-only |
| `CNapiNPConnection_OnStateChange` | `0x626060` | stamps `connection_id` (+0x18, the dcb) from `unk_14` (protocol join counter, seeded in `CNapiNPConnection_Create @0x62acb0`); shipped to the client in the 0x82 MI TLV (`CNapiNPConnection_SendSessionInit @0x620ef0`) — the dcb is server-assigned join order | decompile; D-NET-150 (TASK-2 closure) | confirm-only |
| `NapiClient_WaitForGameStart` | `0x42cc10` | shared host+client loading-wait loop; pumps in-process until spawn gate `dword_24C1928` set; the host-local client's C2S 0x0A originates here | decompile; §5.2/§5.2a | confirm-only |

Local-player input→pose locomotion (Phase 2, 2026-06-20; net-re §5.38):

| original | addr | role | evidence | status |
|---|---|---|---|---|
| `Entity_UpdateInfantryAI` | `0x4b9910` | infantry motor; branch @0x4b9a74 jumps to `loc_4B9C3E` (SIMULATE) when `is_authority` OR `entity==g_local_player_entity` — so the local player ALWAYS simulates; fall-through @0x4b9a8c = network interpolation toward smooth-target +0x234 (REMOTE entities on a client only — `is_authority` is GLOBAL, so a host never interpolates: §5.38a/D-NET-89) | disasm; §5.38/§5.38a (paired to `AiSystem::tick_infantry`, world-wac-ai-re) | confirm-only |
| `Input_ProcessMouseAxisBindings` | `0x499680` | LOOK: mouse deltas × sensitivity (`dword_24D207C<<11`, 16.16) → `Input_TryTriggerMouseAxisBinding(.., entity, dX, dY)` → Yaw(+0x10)/Pitch(+0x14) | disasm; §5.38 | confirm-only |
| `Player_PackInputStateToEntity` | `0x4df450` | MOVE: `g_inputFlags` → 8-way move index → `entity->pad7[12]` (= entity+0x12C): index, +8 is_moving, fire/lean/scope/grenade bits; analog → pad7[16..19] | disasm; §5.38 | confirm-only |
| `Input_ProcessPlayerFrame` | `0x49d4c0` | per-frame input binding dispatch (keyboard/mouse-axis/toggle/analog); calls `Input_ProcessMouseAxisBindings` | disasm; §5.38 | confirm-only |
| `Client_ProcessNetworkFrame` | `0x42c180` | client net frame: `Player_PackInputStateToEntity` (@0x42c3e9) then build C2S 0x0C via `Player_BuildTag0CInputBody` (@0x42c482) — fully grilled P5 (frame order + `!is_authority` 0x0C gate + gate polarity), reimpl `np::ClientRuntime` | decompile; §5.38 / §5.44 | MATCHING (behavioral proof: `npruntime_client_runtime`, `npruntime_golden_client`) |
| `Player_BuildTag0CInputBody` | `0x42a550` | serializes the live pose into C2S 0x0C; gate `entity+286 (healthMax)!=0 && (entity+36 & 2)==0` (SEND-side only — no such gate on receive, §5.38a) → `NetPacket_SerializePlayerState` | disasm; §5.38 / §5.6 | confirm-only |

Host read-apply / remote-peer SNAP mover (Phase 4, 2026-06-23; net-re §5.38a):

| original | addr | role | evidence | status |
|---|---|---|---|---|
| `dispatch_entity_packet_callback` | `0x4d6a80` | C2S entity-packet receive dispatch: gates `is_authority` + `owner_ctx` + `entity==*owner_ctx` + `+356` cb, sets ctx mode=4; NO `entity+286`/`+36` health gate (send-side only) | disasm; §5.38a / §5.10 (paired to `drain_connection_c2s` C2S 0x0C drain, `libs/netsim/src/connection_fan.cpp`) | matching (`netsim_loopback_identity`) |
| `NetPacket_SerializePlayerState` (case 4 tail) | `0x4c2000` | host read-apply/SNAP: gates (entity+0x24 bit1 / `g_spawn_success_gate` / conn==6 / `dword_C8D824`), stage smooth-target +0x234/240/244, mirror live +0x10/+0x14, SNAP live +4/8/C iff entity+0x24 bit0, reset +0x27C; heading/pitch = `movsx`+`shl 16` (no 90°), pos = absolute world when free — CARRIER-LOCAL lifted via `Entity_TransformLocalToWorld` when the uplink carries a ground carrier (D-NET-151) | disasm; §5.38a / D-NET-89/90/91/151 (paired to `EntityWireBridge::apply_player_intent`, `libs/netsim/src/entity_wire_bridge.cpp`) | matching (`netsim_loopback_identity`, `netsim_two_peer_fanout`) |
| `Entity_UpdateInfantryAI` (net-snap skip) | `0x4b9a03` | `test [esi+24h],1; jnz loc_4BFC8B` — net-snapped (entity+0x24 bit0) entity full-skips the motor (host never re-simulates a read-applied peer) | disasm; §5.38a / D-NET-89 (paired to `tick_infantry` net-peer skip-guard, `libs/world/src/infantry.cpp`) | matching (`netsim_loopback_identity`) |

Grounded-on-entity replication (D-NET-151, 2026-07-03; net-re §5.10):

| original | addr | role | evidence | status |
|---|---|---|---|---|
| `NetPacket_SerializePlayerState` (op1 carrier echo) | `0x4c0a08` | compact-record carrier select: mount (+0x16C) wins, else groundEntity (+0x28); carrier form = `Entity_TransformWorldToLocal` pos ×3 compressed (@0x4c0b07) + local-heading yaw byte (`sar 24` @0x4c0b85); free form = pos − `g_priority_ref` + raw yaw byte | disasm; §5.10 / D-NET-151 (paired to `build_0a_frame` player case, `libs/netsim/src/connection_fan.cpp`) | matching (`netsim_two_peer_fanout` grounded_uplink_apply_and_echo) |
| `NetPacket_SerializePlayerState` (op4 carrier apply) | `0x4c1de1` | extended-uplink carrier resolve (`g_pool_list`, pools 0-4 @0x4c1d07-0x4c1d26) + `Entity_TransformLocalToWorld` lift; flags bits 2-4 REPLACED from the raw wire byte (@0x4c1e4d) | disasm; §5.10 / D-NET-151 (paired to `apply_player_intent` grounded branch, `libs/netsim/src/entity_wire_bridge.cpp`) | matching (`netsim_two_peer_fanout`) |
| `NetPacket_SerializePlayerState` (op2 ground mirror) | `0x4c1353` | client stores the echoed carrier into its own groundEntity(+0x28) — unconditional, local player included; attach-change hard-apply via `Entity_TryAttachOrDetach` (@0x4c1329-0x4c1345) | decompile; §5.10 / D-NET-151 (paired to `NetClientView::apply_frame_update` carrier lift, `libs/netsim/src/net_client_view.cpp`) | matching (`netsim_two_peer_fanout`) |
| `Entity_TransformWorldToLocal` | `0x43bb50` | 6-dword POSE transform world→carrier-local: delta, then yaw→pitch→roll inverse rotation in 22-bit fixed point (BAM × 2π/2³² angles; sines folded through the −2²² scale `dbl_7C57B0`); out[3] = heading − carrier heading, out[4]/out[5] pass through | disasm (full imul/shrd-22 chain) | matching — `network_transform_world_to_local` (`libs/npwire/src/wire/ingame_decode.cpp`; `netsim_two_peer_fanout` pose_transform_roundtrip) |
| `Entity_TransformLocalToWorld` | `0x43bd00` | the inverse pose transform: roll→pitch→yaw rotation + carrier position; out[3] = carrier heading + local heading (@0x43be7e) | decompile | matching — `network_transform_local_to_world` (existing D-NET-67 port; euler tail composed by callers) |
| `EntityPool_Allocate` | `0x442168` | the five-pool descriptor table `g_pool_list @ 0xA892E0` `{base, stride, used, capacity}` ×16 B: strides = PER-POOL ENTITY STRUCT SIZES — 904 (players) / 1360 (vehicles) / 812 (statics) / 988 / 988, capacities 256/1200/1200/768/128, one malloc heap with a randomized base offset | decompile | confirm-only |
| `Entity_TryAttachOrDetach` | `0x436610` | the 0x0A own-record attach reconciler: bone+carrier record ≠ current attach → `Entity_ProcessVehicleAttach`; bone-less record while `parentEntity`/`parentSlot` set → `Entity_DetachFromVehicle`; returns 1 = changed (gates the LOCAL hard pos apply) | decompile; §5.10 / D-NET-151 | confirm-only |
| `Server_SendEntityStateToPlayer` | `0x517ba0` | per-recipient 0x0A send wrapper: `g_priority_ref_x/y/z` = recipient EYE pos (`entity+4..C` + `CameraOffset +0x6C..`) @0x517bf5 — the 12-B header anchor AND the priority-list distance reference; budget-halving on congestion/uptime | decompile; §5.10 | confirm-only |
| `Entity_ProcessCollisionAndPlatformPhysics` (standing-on write) | `0x4b3291` | the sim-side carrier source: standing on an entity sets `Flags \|= 0x100000` + `groundEntity(+0x28) = platform` from the collision candidate list | disasm; D-NET-151 | confirm-only |

Joiner-side self-identification (D.0, 2026-06-23; net-re §5.38b; `host_and_join_lan.pcapng` cross-read):

| original | addr | role | evidence | status |
|---|---|---|---|---|
| `NapiNPClientMsg_0x00C` (joiner self-ID) | `0x42E730` | the joiner name-matches its own `entity_name` in the S2C 0x0C organic-spawn batch → `g_local_player_entity`; that record's `slot_id` = the joiner's wire handle H (fixed at the named spawn, before the first C2S 0x0C; NOT from 0x46/0x51) | pcap + disasm; §5.38b / D-NET-92 (paired to `JoinerSession` 0x0C name-match, `libs/novaworld/src/joiner_session.cpp`) | matching (`joiner_session`) |
| `NapiNPClientMsg_0x00F` | `0x42E200` | world-state-load (§5.29): applies spawn pos/yaw to `g_local_player_entity` + clears `Flags & 1`; when `!is_authority` caches spawn + queues the `0x28/0x29/0x2D/0x32` reply burst | pcap + disasm; §5.38b / §5.29 (joiner in-match C2S burst driven by `JoinerSession::pump`; the 0x0F-apply itself is the joiner `NovaSimulation` mode, next increment) | confirm-only |
| `NapiNPClientMsg_PlayerSync` | `0x431370` | 0x046 secondary slot↔handle channel — binds player-slot→entity; arrives AFTER the 0x0C name-match, so it is not the primary self-ID | pcap + disasm; §5.38b / §5.21 | confirm-only |

The D.1 `JoinerSession` and D.2 joiner `NovaSimulation` mode (`enable_join`) + wire-direct present are
LIVE-CONFIRMED (two-instance localhost run, 2026-06-23): the remote player renders in-game, not just in the
headless `coop_two_sim_test`. Open faithful-render gaps (net-re §5.38b): the joiner sees only the host
player (no AI/static spawn-batch over the wire yet), remote bodies don't animate, and the joiner's local
player still uses the NPC motor.

Vehicle-board AI command (`waypoint_id` 123–127 = Goto SSN/Group/Player; world-wac-ai-re §11/§4.12; witnessed 2026-06-22):

| original | addr | role | evidence | status |
|---|---|---|---|---|
| `Entity_UpdateInfantryAI` (board sentinel) | `0x4ba9ad` | tests `slot[148]==125` (= BMS `waypoint_id`) → `==125` keep carrier `slot[144]`, else clear; when set, board via `FindBestSeatSlot`→`AttachToVehicleSeat`. List 123/124/125 = Goto SSN (passenger-only / not-driver / any); target SSN = `wp_number` (`slot[152]`) | disasm; world-wac-ai-re §11/§4.12 | confirm-only |
| `Entity_FindBestSeatSlot` | `0x4351f0` | seat search by bone-name class (sitex=1/ctrlx=2/UseGun=3/drvrx=5) with priority weights | decompile; world-wac-ai-re §9.1 (paired to `EntityCommands::find_best_seat`) | confirm-only |
| `Entity_RequestVehicleAttach` | `0x4364a0` | seats the occupant on the chosen bone slot | decompile; world-wac-ai-re §9.1 (paired to `EntityCommands::mount`) | confirm-only |

On-foot ground settle (D-INF-6, world-wac-ai-re; re-witnessed 2026-06-20):

| original | addr | role | evidence | status |
|---|---|---|---|---|
| `Entity_ProcessCollisionAndPlatformPhysics` | `0x4b2bd0` | on-foot settle: resettles `entity[3] = entityRadius + groundHeight` (@0x4b3da3) so pos[2] = ground + capsule_bottom; on-foot callers org1 @0x4bf7fa / org2 @0x4b7cf9 | disasm; D-INF-6 (paired to `AiSystem::tick_infantry` floor_z) | confirm-only |
| `AnimMap_UpdateEntity` | `0x40b5f0` | feeds `entityRadius` = anim frame `capsule_bottom × 65536` (out-transform block @0x40b82f, out_transform[3] store @0x40b84d); out_transform[4] = top×65536 + 0x2000 | disasm; D-INF-6 (paired to godot `InfantryRootMotion`) | confirm-only |
| `AI_ProcessMovementStep` | `0x466db0` | id-3 death-fall mover: `ai_comp[131] = ground + 0x50000` (@0x466e2d) — a TARGET slot, NOT the live pos[2] (refutes the +0x50000 render-Z reading) | disasm; D-INF-6 | confirm-only |

World-object collision + blink boxes (engine-research 2026-07-09; world-wac-ai-re §15):

| reimpl symbol (file) | original | addr | signature / role | evidence | status |
|---|---|---|---|---|---|
| `world::collision_test_blink` (libs/world/src/collision.cpp) | `Entity_TestCollisionSections` | `0x4aef90` | point-vs-type-8 blink query on building models; flags^6 accum + packed hits | decompile; `collision` ctest | ported |
| `world::collision_raycast_model` (collision.cpp) | `Entity_RaycastCollisionModel` | `0x413060` | segment convex clip vs type-1 volumes (dot>>14 planes); progressive end narrowing | decompile; `collision` ctest | ported |
| `world::collision_contact_force` (collision.cpp) | `Entity_ComputeBoneCollisionForce` | `0x4ae150` | capsule-points contact: prev-pos-gated SAT push-out (Q21) + the collidable-type dispatch table | decompile; `collision` ctest | ported (D-COL-2/4/5 tails) |
| `world::CollisionWorld::raycast_ground` (collision.cpp) | `raycast_entity_collision` + `Entity_RaycastGroundHeight(AndObject)` | `0x413760` / `0x4142c0` / `0x414320` | terrain clamp (indoors-skip) + candidate narrow phase; ground probes store groundEntity | decompile + disasm | ported (D-COL-7) |
| `world::CollisionWorld::resolve_entity` (collision.cpp) | `Entity_ProcessCollisionAndPlatformPhysics` | `0x4b2bd0` | the movement resolver: skip-throttle, candidate forces + flag dispatch, repulsion, ground-settle tail | decompile (closes §4 item 5) | ported (D-COL-5/6/8) |
| `world::CollisionWorld::build_tick_tables` (collision.cpp) | `Entity_BuildProximityLists_Pool2` / `_Pool01` / `FromPools` | `0x4b9430` / `0x4b9340` / `0x4b8eb0` | per-tick static/dyn/person tables + per-entity candidate slices (+0x1BC/+0x1C0) | decompile | ported (D-COL-3) |
| `world::CollisionWorld::refresh_blink` (collision.cpp) | `Entity_BuildProximityList` | `0x4b3dc0` | position-only blink refresh (radius 0x8000 vs the building prefix); indoors 0x800000 | decompile | ported |
| `world::CollisionMatrix` helpers (collision.cpp) | `Math_TransformPointWithTranslation22` / `Math_TransformPointFixedPoint22` / `Math_FixedPointTransformPoint22` / `Matrix_Transpose3x3WithNegateCol3` | `0x412f60` / `0x412e90` / `0x615810` / `0x6136d0` | Q22 row-major 3x4 fixed transforms + rigid inverse | decompile | ported |
| — | `Entity_FindNearestByRay` | `0x413af0` | projectile-side ray over the global static+dyn tables | decompile | confirm-only (round_sim follow-up) |
| — | `terrain_sector_compute_lighting` (blink read) | `0x5c7550` | per-sample blink query; INDOOR keyed on hit-slot presence -> interior light group | decompile | confirm-only (REN scope) |

In-game armory (engine-research 2026-07-09; menu-re §In-game armory):

| reimpl symbol (file) | original | addr | signature / role | evidence | status |
|---|---|---|---|---|---|
| `ArmoryMenuHost` (godot/game/armory_menu_host.gd) | `WeaponDef_RegisterUICallbacks` | `0x567020` | the WEAPON-screen control table (PLAYER_CLASS/slots/ammo/ACCEPT/CANCEL) | disasm; GUT `armory_menu_seam_test` | ported (icons/ammo-text tails) |
| `main_game._try_open_armory` + `NovaSimulation.local_player_in_armory_zone` | `Input_HandleActionBinding` case 218 | `0x49b83d` | armory key gated on Flags 0x400000 (type-6 volume); 0x800 -> vehicle.mnu | disasm | ported (armory leg) |
| — | `UI_OpenMenuScreen` | `0x54e520` | opens a .mnu screen in-game (weapon/vehicle/cmap call sites) | disasm (renamed this session) | confirm-only |
| `main_game._on_loadout_accepted` + `apply_local_player_loadout` | `WeaponLoadout_ApplyFromBuffer` | `0x565cd0` | ACCEPT: serialize UI -> per-team buffer; MP sends (0x2F seam), SP applies via the 0x5A-equivalent chain + weapon re-mount | decompile | ported (equipped+class slice; multi-slot inventory tracked) |
| — | `WeaponLoadout_SerializeToBufferTeamBased` | `0x5658b0` | UI -> {name, ammoPri, ammoSec, flags}* string buffer | decompile | confirm-only |

First/third-person player camera (Phase 2.5, 2026-06-20; net-re §5.39):

| original | addr | role | evidence | status |
|---|---|---|---|---|
| `Camera_ComputeThirdPersonView` | `0x437d10` | master view placement; mode-0 (FP) sets g_view_pos = Position + (0,0,0x10000 eye), g_view_rot = Yaw/Pitch/Roll | disasm; §5.39 | confirm-only |
| `ThirdPersonCamera_Update` | `0x437af0` | 3P follow-anchor smoother (per 62 Hz tick from `Game_ProcessMainFrame @0x5263f0`): anchor→Position+CameraOffset@+0x6C (on foot) / vehicle pos + max(1.0, 0.375·boundRadius) Z (seats 2/5); ease ¼ / 1/16+1/32; orbit keys ±0x1000000/tick; dead-target distance reel to 3.0 | decompile 2026-07-08; §5.39 addendum | confirm-only |
| `Player_UpdateFirstPersonCamera` | `0x4dd380` | builds g_view_matrix @0xB764E0; offset = `ftol(WeaponDef.Bone.pos)` (`pos`, @0xF4) + g_view_pos_bias, rotated by `BuildRotationYXZ(view_rot_bias + Bone.rot)`, + clamped velocity lead + prone Z-drop −0x500; ADS path (entity Flags & 2 ‖ dword_24C1970) swaps offset → `AltCamOffset` (`tpos`, @0x10C) | disasm; §5.39/§5.40 | confirm-only |
| `Input_HandleActionBinding_0` | `0x4e1330` | applies scaled mouse to entity Yaw@+0x10 (wraps) / Pitch@+0x14 (clamp ±80° = ±954437120) | disasm; §5.39 | confirm-only |
| `Camera_SetTrackedEntity` | `0x4391d0` | **(entity, mode)** — 2nd arg hidden by the old 1-arg type; sets `g_camera_mode @0xA890C8` (0=FP, 1=vehicle 3P, 3=spectator, 4=lerp) + `g_camera_tracked_entity @0xA890CC`; on target change resets orbit (yaw 0, pitch 22.5°) + distance 3.0; death sub-mode 2 swaps tracking to the killer (kill-cam) | decompile 2026-07-08; §5.39 addendum | confirm-only |
| `Camera_RaycastCollisionOffset` | `0x4378b0` | 0.25u ray march: per-step bone-collision force vs entity list @+0x1bc/+0x1c0 + terrain clearance; returns camera pull-in | decompile 2026-07-08; §5.39 addendum | confirm-only |
| `Camera_ComputeThirdPersonPositions` | `0x438b80` | computes the mode-4 lerp target 3P placement (called from `Camera_SetTrackedEntity` on mode 4) | xref; §5.39 addendum | confirm-only |
| `BoneAnim_TransformBones` | `0x410360` | per-bone channel eval: slerp quat keyframes → 3x3 into the `matB @0xA7834C` scratch (+ translations @0xA78350, zero without flag bit 1) | decompile 2026-07-09; §5.40 addendum | `libs/anim sample_clip` (sample_bone_world_rot) |
| `AnimChannel_ComputeBoneMatrices` | `0x410da0` | per-bone `Transpose(bind 3x3) × channel matrix`; bind records = `*(channel+44)` — the SKELETON-`.bad` override pinned at registration — with the playing anim only as the null fallback (`@0x410dd8`/`@0x410de3`); per-clip self-bind was the 2026-07-08 misreading (T-pose freeze) | decompile 2026-07-09 ×2; §5.40 bind-source correction | `sample_clip(model_bind, bind_source)` |
| `AnimMap_RegisterEntity` | `0x40bb60` | allocates the entity's two anim slots (entity[98]/[99]); channel at slot+4; `channel+44 := .adm slot-0 .bad` (`@0x40bbe3`) — set ONCE, the rig-wide bind reference | decompile 2026-07-09; §5.40 bind-source correction | `NovaSkeletalAnim::build_from_bad_bytes` (reset .bad as bind_source for every clip) |
| `AnimMap_PlayAnimBySlot` | `0x40bda0` | clip switch: re-inits the playing channel (+0..+12) + slot bookkeeping (+60/64/68); NEVER rewrites the +44 bind source | decompile 2026-07-09; §5.40 bind-source correction | clip switching keeps the shared skeleton bind (implicit in per-clip sample_clip(bind_source)) |
| `BoneAnim_BuildWorldMatrices` | `0x40c400` | FP composed builder: S·Aᵀ·S remap + x-negated pivots/translations (the IMPROPER model→render map composed INSIDE the builder — pivot negate `@0x40c953`), `T(−pivot)·delta`, translation-only hierarchy via the parent's FULL matrix — and the RIG IS THE MODEL TABLE: the FK is bounded by `modelDef+52` (never the `.bad` bone count), each row's parent from the `modelDef+56` row `+0x14` and float pivot from `+0x24`; rows past the anim's bone count are pre-filled with bone 0's composed matrix (the padding loop `@0x40c5a1`); `BadBone.position` never read (12/43 JO viewmodel rigs ship zeroed/stale pos — retail renders all; the field is reconstructible from bind+model: §5.40 position-derivation correction, pytest `test_bad_pos_derivation`) | decompile 2026-07-09 ×2; §5.40 addendum + model-table + position-derivation + frame (sixth-pass) corrections | shipped path: `NovaSkeletalAnim` model-table mode = `positions_from_model` reconstruction (libs/anim; ctest `anim_sample`) through the rest-carrying composition + import-flipped meshes — bit-for-bit the healthy-`.bad` pipeline, retail-framing-confirmed on both SKUs; `sample_clip(model_bind)` kept as the composed-builder reference (native frame; ctest); padded-row translations zeroed = D-INF-15 |
| `build_world_bone_matrices` | `0x40c770` | world-entity composed builder — SAME copy-loop remap + negation set as @0x40c400 but NO bone-0 padding loop; FK bounded by `skeletonData+104`, rows from the 108-byte entity bone table (`skeletonData+108`): parent `@+40`, pivots 16.16 fixed `@+56/60/64` consumed in (z,x,y) order with x negated, bind-inverse `T(−parent pivot)` | decompile 2026-07-09 ×2; §5.40 addendum | body unification pending (D-INF-13) |
| weapon.def `pos`/`tpos`/`rot` parse | `0x54476b`/`0x54471f` | POSITION `atof × 256.0` (flt_7D1D70 → 16.16 world = file/256 m); ROTATION degrees → 32-bit BAM (× 0xB60B60) | disasm 2026-07-09 (py_eval walk); §5.40 (values def-driven per weapon since the fifth pass) | `LocalPlayerHost` `_viewmodel_offset` /256 + `_apply_viewmodel_def` (pos/rot/tpos/renderfov from `NovaWeaponDatabase`) |
| `Camera_ResetToLocalPlayer` | `0x4a3d30` | ClearViewState + SetTrackedEntity(player, `g_cfg_default_camera_mode @0x24D20C4`) + distance 1.0, orbit 0 | disasm 2026-07-08; §5.39 addendum | confirm-only |
| `Input_HandleActionBinding` (view cases) | `0x49ad40` @ `0x49c073` | actions 400/401/402/412 = FP/cockpit/3P/toggle (view bits 0x4000000/0x10000000/0x8000000 in `dword_B3B738` + `g_camera_third_person_selected @0xA860DF`); 405–408 = orbit keys | disasm 2026-07-08; §5.39 addendum | confirm-only |
| camera-mode arbiter | `0x5ca1d2` | per-frame desired-mode logic in `Render_ProcessMainSceneFrame`: seats 2/5 → 1 when 3P selected; death/spawn → 4; server force-FP `dword_24D1E34 & 0x40`; kill-cam redirect | disasm 2026-07-08; §5.39 addendum | confirm-only |
| `apply_session_settings_to_globals` (+0x5CC) | `0x5521a8` | session setting 2 → `g_cfg_default_camera_mode = 1` (third-person default) | disasm 2026-07-08; §5.39 addendum | confirm-only |

Third-person body aim overlay — the torso bend (engine-research + local-player port 2026-07-08;
world-wac-ai-re §14/§14.6; D-INF-11 partial, D-INF-12):

| original | addr | role | evidence | status |
|---|---|---|---|---|
| `Entity_BuildBoneTransformMatrices` | `0x4b1290` | per-bone world matrices = anim pose × per-segment aim/body overlay (7 matrices), re-anchored on the model bone-def pivot table (modelDef+56, stride 64, parent +0x14, pivot +0x24); bone-index switch via byte table @0x4b25c0 (Hex-Rays labels shifted −1; bone 0 = default/body); gates: state flag 0x40, entity Flags 0x100/0x100000/2, mount config +0x86c, itemDef attrib 0x200; weapon/sight/muzzle out-matrices from bones 16/15/14 | decompile + disasm 2026-07-08; world-wac-ai-re §14; ctest `aim_overlay` | ported (on-foot branches + map: `opennova::anim::compute_aim_overlay_angles`/`apply_aim_overlay`, libs/anim/src/aim_overlay.cpp → `NovaSkeletalAnim.eval_pose_overlay`; local player only — mounted/attachment paths pending, D-INF-11) |
| leg-chase yaw fields | `+0x2d4/+0x2d8` | consumed as the YAW of the R/L thigh/calf/foot chains with bodyPitch — proves the §3.3 relabel (chase targets +0x2e4/+0x2e8) | decompile 2026-07-08; §3.3/§14; ctest `infantry` (test_player_body_chase_and_legs) | ported (`InfantryState.leg_yaw/leg_target` + the §3.3 chase tick, libs/world/src/infantry.cpp; org2 sources approximated — D-INF-12) |
| `Entity_GetAttachmentWorldPosition` | `0x4b2670` | attachment consumer of the bone builder | xref 2026-07-08 | confirm-only |
| vehicle-3P pitch halving | `0x4b4942` | local player seated (2/5) in mode 1: render Pitch halved before the body pose build | disasm 2026-07-08; §14.3 | confirm-only |
| `AnimMap_UpdateDualChannels` | `0x40b8c0` | updates the entity's SECONDARY AnimMap channel (+0x18C, the weapon layer) first by swapping its state pair +0x2C4/+0x2C8 into the primary fields +0x2B8/+0x2BC (parentEntity=0, +0x377 blend-suppress zeroed), then the primary (+0x188) | disasm 2026-07-09; world-wac-ai-re §14.8.1 | ported (NovaSimulation body-channel tick + NovaSkeletalAnim dual-channel pose; local player) |
| `g_animStateNameTable` | `0x8135F0` | the 253-entry ANIMNUM name table (body 0–239, `wpn_*` 240–251, `EOF` 252); `anim_<name>` .adm keys resolve against it; flags table @0x8139E8 = +4×254 | data dump 2026-07-09 (renamed from `off_8135F0`); §14.8.2 | ported (body slice `kInfantryAnimNames`; wpn_* slice = the §5.62 FSM keys) |
| secondary-state selection | `0x4b5dad..0x4b5ea9` | per-tick weapon-layer state pick in `Entity_UpdateInfantryPlayerBody`: hold kind (record +0xA4 @0x24E8084+0x460×`entity+0x2B0`) → hold poses 50–61 (+scoped via Flags&0x10), Flags&8 → 64 binoculars, `+0x372` window → 65 reload / 66 reload2 (kind==2 pistol); commit defers on current-state flags 4/0x20 to +0x2C4 | disasm 2026-07-09; §14.8.4 | ported (the FULL ladder incl. hold kinds + scoped variants + binoculars override + reload2: `AiSystem::infantry_weapon_channel`, libs/world/src/infantry.cpp; ctest `infantry` test_player_weapon_hold_kinds; live body_holds_probe pistol/knife) |
| weapon.def `special_hold`/`attack_anim` | `0x543cb7` / `0x543ce9` | the kind-dword parsers in `WeaponDefs_ParseLineCallback`: `special_hold` → record +0xA4 (the hold ladder selector; JOX: 1 knife family, 2 pistols, 3 grenades, 4 AT4/Stinger/RPG, 5 designator, 6 P90, 8 javelin), `attack_anim` → +0xA8 (1 knife / 2 grenade fire stamps); `atol`, defaults 0 via memset (`AdmDef_InitEntryDefaults @0x53fef0`; record base `AdmDefs @0x24E7FE0`, stride 0x460, `AdmDef_GetEntryByIndex @0x53fc80`) | decompile 2026-07-09 session 2; §14.8.4 | reimplemented (libs/def `DefWeaponDef.special_hold/attack_anim` + both ctypes mirrors; `NovaWeaponDatabase` → `NovaSimulation`; ctest `def_parse_weapons` + `infantry`) |
| `WeaponSlot_ReloadAmmo` +0x372 stamp | `0x54173c` | seeds the 80-tick 3P reload-clip window on the entity at refill entry; body updater decrements @0x4b5cf9; +0x371 sibling (arms-dip headLookDecay feed) seeded 80 on remote reload @0x42c10b / 20 on weapon switch @0x4b46f5 | disasm 2026-07-09; §14.8.5 | ported (window on the sim reload refill; local player) |
| `WeaponAction_Fire` body stamps | `0x542bcb/0x542be0` | attack kind (record +0xA8 @0x24E8088) 1→state 62 knife_attack / 2→63 grenade_attack into +0x2C8 (+0x2C4=0, ebx zeroed @0x542b22); rifles stamp nothing (fire's only other anim side effect = `ActionSlot_TryAllocCtrlRegAnim @0x401f00` → the .3di control registers `dword_83FCE8`, a weapon-model visual); net mirror `NetPacket_DeserializeRoundEvent @0x42f79d/0x42f7ba` | disasm 2026-07-09 sessions 1–2; §14.8.4 | ported (`infantry_weapon_attack_stamp` on the sim's fired event; repeat stamp keeps the playhead; ctest `infantry` test_player_weapon_attack_stamp; live body_holds_probe knife) |
| arms-dip / head-look decay block | `0x4b5cab..0x4b5ce7` | per body tick: window byte +0x371 nonzero → decrement + `+0x36C -= 0x2800000`; eighth-step ease `+0x36C -= (+0x36C+4)>>3`; SECOND +0x371 decrement (drains 2/tick — a 20 stamp dips 10 ticks); switch stamp 20 @0x4b46d0..0x4b4701 compares the prev/current held records' anim-def dword before latching +0x370 | disasm 2026-07-09 session 2; §14.8.5 | ported (`InfantryState.arms_dip_ticks/head_look_decay` in `infantry_weapon_channel`; mount-edge stamp in `NovaSimulation::set_local_player_weapon`; fed to the §14 overlay's head_look_decay term; ctest `infantry` test_player_arms_dip) |
| binoculars input chain | `0x4e064c` / `0x4de37b` | action binding (jumptable 0x4E048B case 26) toggles `g_binocularsToggle @0xB76539` (refused fire-charging / scoped+0x168==3; cleared on weapon switch @0x4e11d7, round init/reset); `Player_UpdatePerFrame @0x4de37b` copies toggle → `g_binocularsRaised @0xB7653A`, forced 0 when dead / spawn-gated / `g_inputFlags & 0x1E`; consumed @0x4b5d83 → Flags|8 → body state 64 | disasm 2026-07-09 session 2; §14.8.4 | ladder side ported (`InfantryState.binoculars_raised`, ctest-pinned); the host input toggle deferred until a binoculars item exists |
| `g_audioOutLevel` (the §14.3 pitch kick) | `0x7bef3e..0x7bef55` | the software mixer's output power meter: every 0x400 mixed samples, `(acc²)>>15 − 0x400` clamped 0..0x3FF, double-smoothed via `g_audioOutLevelStage1 @0x3346FA4` → `g_audioOutLevel @0x3346FA8` (zeroed by `AudioMixer_Init @0x7bd405`); consumer @0x4b17f0 adds `level<<17` to the POV body's head/aim pitch — the fire "recoil twitch" IS audio loudness | disasm 2026-07-09 session 2 (renamed from dword_3346FA4/FA8); §14.3/§14.5 | witnessed (port needs a host mixer output-level tap; the overlay input carries the term) |

First-person weapon viewmodel placement — weapon.def `pos`/`tpos` (net-re §5.40, 2026-06-21):

| original | addr | role | evidence | status |
|---|---|---|---|---|
| `Player_RenderFirstPersonViewModel` | `0x4ded60` | builds root_matrix from g_view_euler_translation_out (the `pos`/`tpos`-biased view from `Player_UpdateFirstPersonCamera`), renders gfx1 gun + character arms at it | disasm; §5.40 (paired to `main_game._update_player_camera` / `GameWorld.build_local_player_viewmodel`) | confirm-only |
| weapon.def `pos`/`tpos` parser | `0x54471f` | `tpos` token handler (mirror of `pos`): POSITION = `atof(str) × 256.0` (flt_7D1D70 @0x544770) → 16.16 world coord (net world offset = file/256); ROTATION = `ParseFixedPoint16 × 0x0B60B60` (=2³²/360) → 32-bit BAM; → `WeaponDef.Bone` (`pos`, @0xF4) / `AltCamOffset` (`tpos`, @0x10C) | disasm; §5.40 | confirm-only |
| `WeaponDef_ParseProperty` | `0x54d730` | weapon.def → `g_weaponDefTable` (192 B/entry): +0 name, +32 loadout_selectable, +40 loadout_menu_textid (WepDes), +108 weapon_class (slot), +112 teamfilter, +116 charfilter, +120 weaponweight, +128 round_type, +132 clipsize, +136 maxclips, +140 clipweight; loadout consumer `populate_weapon_slot_lists @ 0x560430` | disasm; avatars-re.md D-PLAYERINFO-11; §5.40 fifth pass | reimplemented (libs/def `def_parse_weapons` + `NovaWeaponDatabase` loadout + viewmodel slices; `GameWorld.local_player_viewmodel_def` → `LocalPlayerHost`; ctest `def_parse_weapons`) |
| weapon.def `renderfov` | `0x54482a` / `0x53ff31` | FP projection fov key (HORIZONTAL degrees), parsed `@0x54482a` into WeaponDef+0x148; the record default **80.0** seeded by `AdmDef_InitEntryDefaults @0x53ff31` — no shipped JO def sets the key (REVX defs only comment it out); consumer `Player_RenderFirstPersonViewModel @0x4dee71` → h→v conversion `@0x58d900` | disasm 2026-07-09 (D-RORD-4); §5.40 fifth pass | reimplemented (libs/def parses + defaults 80.0; `NovaWeaponDatabase` exposes; `LocalPlayerHost` renderfov pass; ctest `def_parse_weapons` default+override) |
| `Math_BuildFixedPointToFloatMatrix4x4` | `0x612200` | view euler+translation → float 4×4 for the render; translation ×1/65536, **Y negated**, rotations Z-X-Y at BAM scale | disasm; §5.40 | confirm-only |
| `Math_BuildFixedPointRotationMatrixYXZ` | `0x615400` | 10.22 fixed rotation matrix (0x400000=1.0) from (yaw,pitch,roll), order Z·X·Y; identity at zero angles | disasm; §5.40 | confirm-only |
| `Math_FixedPointTransformPoint22` | `0x615810` | transforms a point by a 10.22 fixed matrix (round 1<<21, >>22); preserves the point's unit scale | disasm; §5.40 | confirm-only |

Held-weapon visibility on mount/attach (engine-research, 2026-06-24; world-wac-ai-re §13; no reimpl — OpenNova renders no third-person held weapon yet):

| original | addr | role | evidence | status |
|---|---|---|---|---|
| `BoneCallback_org0_World` | `0x4e3940` | organic (`org`) render callback; 6 `Render_SubmitEntity` draws — held weapon = draw 5, gated on `Entity_CanFireWeapon`; held-weapon ADM index @ entity+0x2B0, posed at the `prim` hand bone; draws 5/6 skipped when `numEntries & 0x10000000` | disasm; world-wac-ai-re §13.1/§13.2 | confirm-only |
| `Entity_CanFireWeapon` | `0x4dcb10` | weapon shown iff can-fire: `Flags & 2` disables; seat type `parentSlot` (entity+0x168) ∈ {2 control, 3 gunner, 5 driver} hides (remote: any; local: gunner only in 3P `dword_A890C8`); passenger (1) keeps weapon | disasm; world-wac-ai-re §13.3 | confirm-only |
| `Entity_GetBoneSlotType` | `0x434ed0` | user-point prefix → seat type (sitex=1/ctrlx=2/UseGun=3/drvrx=5); fixed emplacements = `UseGun` | decompile; world-wac-ai-re §13.4 / §9.1 | confirm-only |
| `Entity_ProcessVehicleAttach` | `0x435aa0` | assigns `parentSlot` (seat type) on attach; net 0x26 path | decompile; world-wac-ai-re §13.4 | confirm-only |

`Player_*` family naming + decomp grill (2026-06-26; net-re §5.41; read-only, adversarially re-derived):

| original | addr | role | evidence | status |
|---|---|---|---|---|
| `NapiNP_GetLocalConnectionId` (was `Player_MaybeGetLocalSessionId`) | `0x4c6d40` | returns local `NapiNPConnection.connection_id` (@+0x18) = ConnectionId/dcb (int); ret type fixed `NapiNPConnection*`→`unsigned int`; field `unk_18`→`connection_id` | disasm + 2 skeptic passes; §5.41 / D-NET-100 | confirm-only |
| `Player_FindLocalPlayerEntity` | `0x4e0090` | pool-0 self-ID: `Flags&0x100 && ownerConnectionId(@0x78)==local ConnectionId`; sig fixed → `GamePlayerEntity*(void)` | disasm; §5.41 / §5.38b / D-NET-92 | confirm-only |
| `Player_FatalPlayerDcbNotFound` (was `Player_BuildNetIdLookupOrFatalError`) | `0x4dff60` | `__noreturn` "Could not find player dcb" abort; reached from `Player_InitPlayer` on NULL self-ID | disasm; §5.41 / D-NET-102 | confirm-only |
| `Player_InitPlayer` | `0x4e15f0` | local-player init; sig fixed → `int(int isRestore)` (3 phantom params dropped); resolves self-ID, loads weapon slots, sets camera offset | disasm; §5.41 / §5.2a | confirm-only |
| `Server_PlayerAdd` (was `player_ServerAdd`) | `0x51cbc0` | server player-slot manager; writes `entity+0x78` (`ownerConnectionId`) `= joinEvent+76` | disasm; §5.41 (string "server_PlayerAdd()") | confirm-only |
| `PlayerClass_InitEntity` (was `Player_InitLocalPlayer`) | `0x4b1060` | `"plyr"` entity-class init callback (sole xref = class table @0x813054) | disasm; §5.41 | confirm-only |
| `Player_ToggleWeaponScope` | `0x4df0c0` | scope/ADS toggle: drives `g_scopeEngaged@0x82CE94` (mirrored → `g_weaponScopeActive@0xB76478`), `g_cameraFovDeg@0x26C6848`, `g_fpCameraInterp@0x82CE40` | disasm; §5.41 | confirm-only |
| `Player_CanFireWeapon` | `0x5cf780` | fire gate: equipped-slot/seat/water/ADS checks; drives gunner zoom FOV | disasm; §5.41 | confirm-only |
| `Player_GetClampedWeaponElevation` (was `…GetCurrentWeaponAmmoCapacity`) | `0x4dc6b0` | clamps `MountSlot.Elevation`→`WeaponDef.MaxElevation` (zoom level, not ammo) | disasm; §5.41 | confirm-only |
| `Camera_ResetToLocalPlayer` (was `Player_ResetTerrainPosition`) | `0x4a3d30` | camera reset tracking the local player (not terrain) | disasm; §5.41 | confirm-only |

`Server_*` family naming + decomp grill (2026-06-26; net-re §5.42; read-only; 120 fns validated):

| original | addr | role | evidence | status |
|---|---|---|---|---|
| `Server_TickUpdate` | `0x51d7e0` | authoritative-server per-tick lifecycle; gates ~14 cadence timers (per-second block drives capture/win/violation/timeout) | decompile; §5.42 | confirm-only |
| `Server_FindPlayerSlotByNetKeys` (was `Server_ProcessTeamChanges`) | `0x5008b0` | pure slot lookup matching net-player `+48`/`+52`; old name + stale comment wrong | decompile; §5.42 / D-NET-107 | confirm-only |
| `Server_BroadcastMedicRequest` (was `server_broadcast_entity_kill`) | `0x515390` | medic-request broadcast (`STRSRV_MEDREQ`, msg 0x54+0x14, once-only); sig → `(ctx,data,len)`; no kill | decompile; §5.42 / D-NET-108 | confirm-only |
| `Server_ClientFiredRound` (was `Server_ValidateAndFireRound`) | `0x50baa0` | C2S fired-round validate+queue; own string `"server_ClientFiredRound:"`; sig `()`→`(int fireRequest)` | decompile; §5.42 / D-NET-109 | confirm-only |
| `Server_SendWeaponSlotListToPlayer` | `0x502550` | bogus `__stdcall(WndProc)` proto → cdecl `int(void*)` (plain `retn` proof) | disasm; §5.42 / D-NET-110 | confirm-only |
| `Server_UpdateCaptureZoneEntities` | `0x519690` | bogus `__stdcall(display)` proto → cdecl `void(void)` (plain `retn` proof) | disasm; §5.42 / D-NET-110 | confirm-only |
| `Server_CheckWinConditions` | `0x51ad40` | per-gametype win eval → `Server_ProcessRoundEnd`; sig `(int,int*)`→`void(void)` (Tick phantom-arg source) | decompile; §5.42 / D-NET-110 | confirm-only |
| `Server_HandleEntitySync` (was `server_handle_entity_sync`) | `0x510990` | net msg handler (table `@0x82b5d8`); sig `()`→`(int ctx,u8*,int)` | decompile; §5.42 / D-NET-110 | confirm-only |
| `Server_ValidatePlayerJoinRequest` | `0x512100` | join gate (version/ban/expansion/squad/PCID/banned-name/jointicket); names `g_expansion_checksum`/`g_banned_*`/`g_squad_*` | decompile; §5.42 | confirm-only |

`*napinp*` dispatch-surface + crypto naming/typing grill (2026-06-27; net-re §4 / D-NET-118; IDB-only):

| original | addr | role | evidence | status |
|---|---|---|---|---|
| `NapiNPServerMsg_0x03D` (was `Path_ReplaceExtension`) | `0x500ec0` | C2S msg 0x3D handler; authority-gated entity write (`slot+352→entity+192`, stores `dword_A87060`→`entity+97560`); bogus auto-name; sig → `(ctx,data,len)` | decompile; single C2S-table xref; §4 / D-NET-118 | confirm-only (role `unknown`) |
| `NapiNPServerMsg_0x03E` (was `ErrorLog_Write`) | `0x500e10` | C2S msg 0x3E handler; single `retn` no-op stub; bogus auto-name | disasm; single C2S-table xref; §4 / D-NET-118 | confirm-only |
| `NapiNPServerMsg_0x049_ParsePlayerStatus` (was `napi_np_server_msg_0x049_parse_player_status`) | `0x510f40` | C2S msg 0x49 handler; snake-case→family normalize | §4 / D-NET-118 | confirm-only |
| `g_np_opcode_handlers` | `0x849D90` | session opcode table (14 legs+sentinel); legs 0x44–0x47/0x84–0x87 confirmed named (`Nwu_HandleClient/Server_{ResendList,Ping,Goodbye,Probe}`) | read table data + decompile; §4 / D-NET-118 | confirm-only |
| `g_empty_str` | `0x7C08C6` | the msginfo/opcodeinfo "magic" value = `&g_empty_str` (empty-string default, non-null validity marker; NOT a build stamp) | data read; §4 open-q / D-NET-118 | confirm-only |
| `g_napi_prng_state` | `0x31C1078` | NapiNP connection-entropy LCG; typed `LCGState` (seed/multiplier=78665521 NWU_LCG_MAGIC/counter); seeded `PRNG_InitFromTimestamp @0x794260`, drawn 4× in `NapiNPServer_HandleNewConnection @0x4c8040` | decompile; §4 / D-NET-118 | confirm-only |

## 5.7 Per-frame client net role (P5, 2026-06-27; net-re §5.44)

The client counterpart of `Server_TickUpdate`. Witnessed + ported into `libs/npruntime`
(`np::ClientRuntime`) / `libs/netsim`. Verified by `npruntime_client_runtime` (always-on in-process
round-trip + host-as-client) and `npruntime_golden_client` (C2S 0x0C inner + framing byte-parity vs the
gameplay capture; S2C anchor cross-check). Findings landed in [net/novaworld-net-re.md §5.44](net/novaworld-net-re.md).

| original | addr | role | evidence | status |
|---|---|---|---|---|
| `Client_ProcessNetworkFrame` | `0x42c180` | per-frame client net role: recv-pump (`0x42c228`) → raw-input pack (`0x42c3e9`) → C2S 0x0C (`0x42c482`, gated `is_in_session && !is_authority && !dword_81474C && !g_spawn_success_gate`) → send-pump (`0x42c4bc`); NOT authority-gated at its call site (`Game_ProcessMainFrame @0x526692`) | decompile; §5.44 | MATCHING (behavioral proof) |
| `CNapiNetwork_PumpClientProtocolRecv` | `0x4c4fe0` | client recv pump → `NapiNPProtocol_Pump(np, flags=26, 250)` (drain+dispatch S2C) | decompile; §5.44 | confirm-only |
| `CNapiNetwork_PumpClientProtocolSend` | `0x4c5000` | client send pump → `NapiNPProtocol_Pump(np, flags=738, 250)` (flush queued outbound) | decompile; §5.44 | confirm-only |
| `CNapiNetwork_QueueReliableMessage` | `0x4c4fa0` | queues a message on `napi_conn`; drops its `flags` arg → `CNapiNPConnection_QueueMessage(…, msg_flags=0, …, 1024)` | decompile; §5.44 | confirm-only |
| `NapiNPMessage_Create` | `0x627fc0` | builds the outgoing message; `len_field_size` = 3 (LEN8 inner-flag `0x20`) for payload 1..255 / 4 (LEN16) for >255, `msg_flags=0` ⇒ no SKIP bytes — the 0x0C inner-message byte format | decompile; §5.44 | MATCHING (byte-parity: `npruntime_golden_client`) |
| `g_spawn_success_gate` | `0x24c1928` | deploy gate: SET on death/spectator (0x0A `flags1&0x01`) + spawn-select (0x1D), CLEARED on deploy → `!gate` = deployed/alive (the 0x0C send gate) | xrefs; §5.44 / §5.2 / §5.9 | confirm-only |

Reimpl seam (opennova ↔ opennova self-consistent + retail byte-parity where cited):

| reimpl | original anchor | role |
|---|---|---|
| `np::ClientRuntime` / `Client_ProcessNetworkFrame` | `Client_ProcessNetworkFrame @0x42c180` | role-aware headless client (Joiner + host-as-client); recv-fold → C2S 0x0C (`!is_authority`-gated) |
| `np::apply_in_match_c2s` (D-NET-126) | `NapiNPServerMsg_0x00C @0x501c30` | production `PeerC2SInMatch` consumer → `deliver_c2s` onto the owning connection → `Server_TickUpdate` drain |
| `np::is_in_match(conn)` (D-NET-121/122) | `NapiNPServer_SendFiltered @0x4c87e0` | single in-match predicate shared by the drain + emit fan; host loopback no longer starved, anchors to its player |
| `netsim::NetClientView::apply` / `ISessionTransport::deliver_c2s` | `NapiNPProtocol_Pump @0x62a650` | remote-wire fold path / uniform C2S inbound-inject |

## 6. Host Command wiring ([ADR 0001](adr/0001-mnu-action-command-boundary.md), matches)

`UI_DispatchScreenEvent @ 0x54e6a0`, `UI_ShowPreGameMenuByState @ 0x568d10`,
`UI_RegisterOptionsCallbacks @ 0x55d610`, `Expansion_SwitchTo @ 0x5688c0`,
`Expansion_ReloadAllAssets @ 0x568370`.

## 7. Reading the tables

- **addr** is the join key — names drift across IDB passes, addresses don't.
- MNU tag literals are UTF-16 wide; to re-locate one after an IDB rebuild, search
  ASCII-interleaved-with-0x00 bytes or re-anchor via `make_signature_for_function`.
