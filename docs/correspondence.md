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
| MNU/MNS menu UI (`libs/mnu`, `libs/mnu_xml`, `libs/mns`, `godot/engine/mnu`) | 2026-06-01 format + 2026-06-09 menu slice + 2026-06-12 mns spec pass | **matching** (D-MNU-1..3 accepted; D-MNS-1..4 lenient-with-diagnostic, loader grill pending) | [menu-re.md](mnu/menu-re.md) |
| LWF banks / DBF dialogs / member selection (`libs/lwf`, `libs/dbf`, `libs/audio`) | 2026-06-09 | **matching** (D-SND-1..3) | [lwf-dbf-sound-re.md](audio/lwf-dbf-sound-re.md) |
| MUS VM + compiler / SBF codec / SCR container (`libs/mus`, `libs/sbf`, `libs/scr`) | 2026-06-09 | **MATCHING** per component (D-SCR-1/2; host audio glue not grillable) | [mus-sbf-re.md](audio/mus-sbf-re.md) |
| Environment / time-of-day (`libs/env` + `env_render`; `nova_environment`/`nova_sky`/`nova_weather`/`nova_celestial`) | 2026-06-09/11 | per subsystem: parse/TOD/sun-moon/fog/load-order **matching**; weather **divergent → ported**; sky dome combine **divergent — recovered (C6), port = C7**; celestial + glare new-from-witness; iris (auto-exposure) + terrain_rgb (terrain tint stack) **consumers recovered, unimplemented — tracked** | [env-tod-re.md](env/env-tod-re.md) |
| RTXT string tables (`libs/rtxt`, NovaStrings) | 2026-06-09 | **matching** at byte level (98/98 retail bins roundtrip; D-RTXT-4 strictness retained) | [rtxt-strings-re.md](interface/rtxt-strings-re.md) |
| HUD overlay render (originals; OpenNova HUD in flight) | 2026-06-22 (engine-research) | **confirm-only** — originals witnessed, port pending (D-HUD-1..4; weapon-coupled + radar deferred) | [hud-re.md](interface/hud-re.md) |
| BMS event runtime + mission→world promotion (`libs/mission` event_runtime/promote) | 2026-06-10 | **MATCHING** (D-EVT-1..4) | [bms-event-runtime-re.md](mission/bms-event-runtime-re.md) |
| World / WAC VM / AI + infantry motor (`libs/world`, `libs/wac`) | 2026-06-07..10 (+ 2026-06-22 player slide/gravity) | **MATCHING** (infantry D-INF-1..10; vehicle/HELO movement physics are tracked `not_yet_ported` stubs) | [world-wac-ai-re.md](world/world-wac-ai-re.md) — carries its own correspondence map (§2 there) |
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
| scroll `ORIENTATION` in `parse_window` | `CUIScrollWidget_ParseExtendedXMLDef` | `0x64c6d0` | scroll extended attrs | wide `ORIENTATION` @ 0x7e1494, `HEIGHT` @ 0x64c766 | divergent (scroll `<HEIGHT>` dropped) |
| `mnu::parse_screen` (`mnu.cpp`) | `parse_scene_node_attributes` (+ SCREEN cb `0x63b800`) | `0x639630` | SCREEN children NAME/MUSICVAR/WINDOW | wide `MUSICVAR` @ 0x7e072c | matching |
| `mnu_xml::parse` / `skip_bom` (`mnu_xml.cpp`) | `XML_ParseWithBOMDetection` | `0x76a690` | SAX parse + BOM detect | callee of `0x63c830`; BOM logic | matching (corpus) |
| `mnu_xml::decode_entity` (`mnu_xml.cpp`) | `XML_ParseCharEntity` (+ table `0x85a628`) | `0x769cc0` | entity decode | entity table layout | divergent (edge cases) → closed by menu slice |
| `MnsStyleSheet::substitute` (`godot/engine/mnu/mns_stylesheet.cpp`) | `NapiXML_ExpandVariablesInText` | `0x63a000` | `%VAR%` expansion | whole-buffer pre-parse | divergent (scope/timing) → accepted as D-MNU-1 ([ADR 0005](adr/0005-mnu-var-expansion-policy.md)); covers colors/fonts/textures/literal text |
| `mns::Document::parse` (`libs/mns/src/mns_document.cpp`) | `sub_552500` (.mns stylesheet load) | `0x552500` | stylesheet parse | ref'd from `0x63c830`; loader body unwitnessed — built from the in-file NovaLogic spec | probable (lossless model [ADR 0009](adr/0009-mns-lossless-document-model.md); D-MNS-1..4 in [menu-re.md](mnu/menu-re.md); grill pending) |
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
| `NovaSoundBank._make_player` distances | distance/pan math in `SoundBank_PlayTriggerEntries` + `SoundBank_CalcDistanceVolPan` | `0x75cf1a` / `0x75ca20` | inner@+4 proximity, max@+6 fade, clamp = sndparm+16 | arg flow | matching (Godot attenuation model is host policy) |
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

`/PROFILE` `.sph` server-log recorder cluster (§5.22; decoder `libs/novaworld/serverlog_decode.cpp`):

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
`encode_full_entity_spawn`/`decode_full_entity_spawn` in `libs/novaworld`, `build_full_entity_spawn`
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

Wire-coverage sweep (§5.48–§5.56; grill 2026-07-01; reimpl `libs/novaworld/ingame_decode` decoders +
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
| `Server_ProcessClientRequestRespawn` | `0x519AF0` | C2S 0x0E respawn/deploy request ([i16 spawnHandle], 0xFFFE = auto) | — | matching (read-only) |
| `NapiNPServerMsg_HandleVehicleAttach` / `_HandleVehicleDetach` | `0x502390` / `0x4FC980` | C2S 0x26/0x27 vehicle attach (anti-spoof word0 overwrite) / detach | — | matching (read-only) |
| `NapiNPServerMsg_HandleVehicleSpawnRequest` | `0x51C4C0` | C2S 0x40 vehicle spawn → S2C 0x18 broadcast (mask 0x90) at the source model's boat/helo userpoint | — | matching (read-only) |
| `NapiNPClientMsg_TeamAssign` / `_ScoreDeltaSound` | `0x431910` / `0x42A0B0` | S2C 0x50 team assign / 0x81 score-delta hit-confirm sound | — | matching (read-only) |

Server per-frame S2C 0x0A emit (§5.47; witnessed 2026-07-01; reimpl in `libs/netsim/src/connection_fan.cpp`
+ `netsim::Connection::s2c_phase`; `netsim_two_peer_fanout` `run_0a_subblock_phase_cycle` + shape harness
`scripts/net/diff_0a.py`):

| original | addr | role | D-NET | status |
|---|---|---|---|---|
| `Server_SendEntityStateToPlayer` | `0x517ba0` | per-recipient 0x0A build: `state==6` deploy gate, eye-pos priority ref, build priority list, header + entity loop, budget save/halve, SendFiltered mask 0xA0 | D-NET-134 | header ported; deploy-gate/eye-ref/budget = step 2 |
| `NetPacket_WritePlayerState` | `0x4ff6b0` | 0x0A header: ref pos + state_flags + phase byte (`playerSlot+100566`), `phase&3` sub-block (0 weapon/1 status/2 env/3 gametype), recipient tail | D-NET-134 | phase counter + sub-blocks {1,0,3} ported; env(2)/passenger deferred |
| `serialize_entity_states_to_packet` | `0x50f070` | 0x0A entity loop — all callback entities from the priority list, `[1][handle][type][compact]`, budget-limited round-robin, `[0]` terminator | — | players only; priority/budget/all-class = step 2 |
| `Server_BuildEntityPriorityList` | `0x50e590` | distance-sorted priority pairlist per recipient (eye-pos ref) | — | not yet ported (step 2) |

Wave 8 branch-validation grill (2026-07-01; net-re §8 Wave 8; reimpl in `libs/novaworld/protocol_message.{h,cpp}`,
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
| `NetPacket_SerializeWeaponOverlaySlotState` | `0x505e80` | 0x46 body: 0x8000 removal, 0x4000 ECHO from request, `fieldFlags & 0x7FFF` pass-through, per-bit fields (slot-state sources pinned) | D-NET-127 | matching (echo fix applied; per-field values remain approximated) |
| `NapiNPClientMsg_PlayerSync` | `0x431370` | client 0x46 handler: variable-length strings, removal = no entity byte, ack-walk re-request `[slot+1][0x5CF7]` while `< byte_A860D1` | — | matching (decode side) |
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
| `WeaponAction_Fire` | `0x542b10` | the adm 'fire' action (admEntry+684, action table +676 / suffix table 0x830B94): latched-pose fire position, Entity_FireWeaponAndSendPacket, consume_weapon_ammo, burst counter, RECOIL chain | §5.16 D-NET-152 | witnessed (the ported dispatch collapses the re-entry inline) |
| `Entity_FireWeaponAndSendPacket` | `0x42bd80` | the shared fire entry: authority → LOCAL-mode Server_ClientFiredRound; client → RoundData_SpawnRound (own-fire prediction) + C2S 0x06 send | §5.16 D-NET-152 | witnessed |
| `consume_weapon_ammo` | `0x540850` | ammo decrement: clip u16 slot+16 (adm+220 == 0), else per-player pool playerSlot+89176+4*adm220 / global data @0xB761E8 (AI), vehicle store via mode 3 | §5.16 D-NET-152 | ported (clip path; pools deferred) |
| `WeaponSlot_ReloadAmmo` | `0x541720` | host+client clip refill: clears slot+90 0x80, refunds clip×adm224 to the adm+216 pool, refill = capacity(adm+88)×cost clamped by the pool → slot+16 | §5.58 D-NET-142/152 | ported (refill-to-capacity; refund/clamp deferred) |
| `RoundData_AddRound` | `0x4fdb40` | the ONLY g_round_ring writer: 36-B record (stat, SHOOTER handle, pre-spread origin + direction BAMs, shot-seq, mode/subtype/slot/adm bytes) + inline RoundData_SpawnRound | §5.9.1 D-NET-152 | ported (`world::RoundRing`; the spawn half deferred) |
| `RoundData_SpawnRound` | `0x4ec0d0` | spawns the authoritative round: score multiplier, tracer interval adm+226, ammo-class dispatch (trail/hitscan 0x400, guided 0x2000000, bursts 0x1/2 0000), projectile-pool entity + spread + velocity (ex `RoundData_ProcessHit`) | §5.9.1 D-NET-152 | witnessed (port = next round) |
| `Server_BuildRoundEventListForPlayer` | `0x4ffee0` | per-recipient round staging: watermark playerSlot+97544 (arm gate), own-shooter skip @0x4fff97, line-of-fire proximity score (2π/2^32 BAM, 2^22 trig, 1000u clamp, z half-weight, 0x4000−lateral>>12), top-255 → g_round_event_refs (ex `compute_entity_angular_priority`) | §5.9.1 D-NET-152 | ported (netsim `select_round_events`) |
| `NetPacket_SerializeRoundEvent` | `0x504820` | the §5.9.1 tag-2 writer: flags gates (0x40 live fire-target, 0x80 slot byte), SHOOTER handle, shot-seq, origin compressed vs g_priority_ref (recipient eye), direction BAM high words (ex `serialize_projectile_to_packet`) | §5.9.1 D-NET-152 | ported (`encode_round_event_record` + netsim conversion) |
| `NetPacket_DeserializeRoundEvent` | `0x42f270` | the client read side (ex `NetPacket_DeserializeWeaponHit`); dispatches into RoundData_SpawnRound — the receiving client re-simulates the round | §5.9.1 D-NET-152 | matching (`decode_round_event_record`; byte-witnessed 2026-06-16d, relabeled) |
| `Weapon_UpdateAllProjectiles` | `0x4ec020` | the per-tick live-round iterator → Projectile_UpdatePhysics per round | §5.60 | confirm-only (port = round sim) |
| `Projectile_UpdatePhysics` | `0x4e9d70` | per-tick round step: velocity advance, segment ray (terrain hi-res + entity proximity + water), drag, 5-way hit switch → impact handlers; min proximity radius 0.1u @0x4ea263 | §5.60 | confirm-only (port = round sim) |
| `Weapon_RaycastAndSpawnImpact` | `0x4e8460` | the hitscan leaf (adm halfword 22 == 1): ray to weaponDef+56 range, terrain/entity/water hit class, impact effect + sound — EFFECTS ONLY, no damage call | §5.60 | confirm-only |
| `Projectile_HandleEntityImpact` | `0x4e9390` | entity hit: vehicle parent-chain resolve, penetration budget (+676−+684 vs ammo dword 3) + child-ammo spawn (ammoDef+241), type-15 bone/section damage, pass-through 0x18000000 → ProcessDamageOnTarget | §5.60 | confirm-only (port = round sim) |
| `Projectile_ProcessDamageOnTarget` | `0x4e7fb0` | the bullet damage gate: CalcImpactDamage, zeroing gates (indestructible/armor-class ammoDef+196 vs itemDef+400/already-dead +292/occupant scale), AUTHORITY-ONLY health −= damage @0x4e8127 + team matrices, kill → Score_ProcessKillEvent | §5.60 | confirm-only (port = damage) |
| `Weapon_CalcImpactDamage` | `0x4ec920` | KINETIC damage: authority-only (returns 0 else @0x4ec933), zone multiplier (head 0-4 flag 0x100 / body / limbs / special 13-14 flag 0x800 / vehicle seats), (62·speed)>>16 clamp 1219, g_OneShotKill 2000, ammoDef+192 max clamp — falloff emerges from drag | §5.60 | confirm-only (port = damage) |
| `Projectile_ProcessExplosionQueue` | `0x4ead80` | the AoE queue: fn-ptr tables @0x4eadd0/@0x4eae44 → Entity_ApplyWeaponDamage | §5.60 | confirm-only (deferred behind bullet damage) |
| `Entity_ApplyWeaponDamage` | `0x4e6820` | explosion damage applicator: authority gate + g_destroy_buildings, team attrib 0x8000 protection, weaponDef+46 base, LINEAR blast falloff, infantry/vehicle section paths | §5.60 | confirm-only (deferred behind bullet damage) |
| `Score_ProcessKillEvent` | `0x4fd400` | authority-gated scoring dispatch (gametype scoring + SP classify) — emits NO messages | §5.60 | confirm-only |
| `Entity_CheckAndProcessDeath` | `0x51b550` | the death router (from the per-tick infantry updates @0x4b40e0/@0x4b9910 on health<=0): Flags&0x100 → GameEvent_PlayerDeath, else AI S2C 0x13 + scoring | §5.60 | confirm-only (port = death) |
| `GameEvent_PlayerDeath` | `0x516dd0` | the player-death orchestrator [authority]: live-target clears, S2C 0x13, respawn timer (620-tick rule, floor 3), scoring, kill-type classify (headshot 0x100/vehicle 0x800/knife 0x400/explosive 4091/4093/4095/...), S2C 0x52 ×2 + 0x1E kill feed + 0x54 ×2 | §5.60 | confirm-only (port = death) |
| `Server_KillPlayerAndNotify` | `0x519e00` | slot dead-mark (+100567, entity+292=−1) → Server_ProcessPlayerDeath; optional S2C 0x32 sub 5 (player name) | §5.60 | confirm-only (port = death) |
| `Server_ProcessPlayerDeath` | `0x517740` | death→respawn: killer resolve, detach (+0x40000 seat re-attach), spawn camera, Entity_ResetToSpawnState, scoring, weapons re-init + 0x5A resend, 0x1E, KRBP marker | §5.60 | confirm-only (port = death/respawn) |
| `Server_SendEntityStatePacket` | `0x509d70` | the SINGLE S2C 0x26 emit (entity handle+team, mask 0x90, tick stamp +560); staged by all 16 non-player death/destruction handlers | §5.60 | confirm-only (port = death) |
| `NapiNPServerMsg_HandleSectorAction` | `0x514330` | C2S 0x13 = pool-3 def-type-2044 sector actions — NOT a death message (table row corrected) | §5.60 | confirm-only |
| `NapiNPClientMsg_WeaponReload_0x049` | `0x42C0A0` | client 0x49 apply: local -> WeaponSlot_ReloadAmmo @0x541720 (the ONLY client clip refill; clears the 0x80 pending flag), remote -> 3P reload anim | §5.58 D-NET-142 | witnessed |
| `Entity_UpdateVehiclePhysics` (family) | `0x48AF00` | vehicle damage states off entity+286 vs itemDef: wreck at hp<=0, burn `0 < hp <= criticalHp(+0x180)` (+ authority drain +0x182 every 64t), smoke `< healthMax>>2`, regen (+0x184) | §5.13 | witnessed (drives the 0x0A vehicle health word requirements) |
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
| `Entity_TransformWorldToLocal` | `0x43bb50` | 6-dword POSE transform world→carrier-local: delta, then yaw→pitch→roll inverse rotation in 22-bit fixed point (BAM × 2π/2³² angles; sines folded through the −2²² scale `dbl_7C57B0`); out[3] = heading − carrier heading, out[4]/out[5] pass through | disasm (full imul/shrd-22 chain) | matching — `network_transform_world_to_local` (`libs/novaworld/src/ingame_decode.cpp`; `netsim_two_peer_fanout` pose_transform_roundtrip) |
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
| `Entity_AttachToVehicleSeat` | `0x4364a0` | seats the occupant on the chosen bone slot | decompile; world-wac-ai-re §9.1 (paired to `EntityCommands::mount`) | confirm-only |

On-foot ground settle (D-INF-6, world-wac-ai-re; re-witnessed 2026-06-20):

| original | addr | role | evidence | status |
|---|---|---|---|---|
| `Entity_ProcessCollisionAndPlatformPhysics` | `0x4b2bd0` | on-foot settle: resettles `entity[3] = entityRadius + groundHeight` (@0x4b3da3) so pos[2] = ground + capsule_bottom; on-foot callers org1 @0x4bf7fa / org2 @0x4b7cf9 | disasm; D-INF-6 (paired to `AiSystem::tick_infantry` floor_z) | confirm-only |
| `AnimMap_UpdateEntity` | `0x40b5f0` | feeds `entityRadius` = anim frame `capsule_bottom × 65536` (out-transform block @0x40b82f, out_transform[3] store @0x40b84d); out_transform[4] = top×65536 + 0x2000 | disasm; D-INF-6 (paired to godot `InfantryRootMotion`) | confirm-only |
| `AI_ProcessMovementStep` | `0x466db0` | id-3 death-fall mover: `ai_comp[131] = ground + 0x50000` (@0x466e2d) — a TARGET slot, NOT the live pos[2] (refutes the +0x50000 render-Z reading) | disasm; D-INF-6 | confirm-only |

First/third-person player camera (Phase 2.5, 2026-06-20; net-re §5.39):

| original | addr | role | evidence | status |
|---|---|---|---|---|
| `Camera_ComputeThirdPersonView` | `0x437d10` | master view placement; mode-0 (FP) sets g_view_pos = Position + (0,0,0x10000 eye), g_view_rot = Yaw/Pitch/Roll | disasm; §5.39 | confirm-only |
| `ThirdPersonCamera_Update` | `0x437af0` | 3P follow: target = Position + CameraOffset@+0x6C + smoothing/distance/bone-collision | disasm; §5.39 | confirm-only |
| `Player_UpdateFirstPersonCamera` | `0x4dd380` | builds g_view_matrix @0xB764E0; offset = `ftol(WeaponDef.Bone.pos)` (`pos`, @0xF4) + g_view_pos_bias, rotated by `BuildRotationYXZ(view_rot_bias + Bone.rot)`, + clamped velocity lead + prone Z-drop −0x500; ADS path (entity Flags & 2 ‖ dword_24C1970) swaps offset → `AltCamOffset` (`tpos`, @0x10C) | disasm; §5.39/§5.40 | confirm-only |
| `Input_HandleActionBinding_0` | `0x4e1330` | applies scaled mouse to entity Yaw@+0x10 (wraps) / Pitch@+0x14 (clamp ±80° = ±954437120) | disasm; §5.39 | confirm-only |
| `Camera_SetTrackedEntity` | `0x4391d0` | sets the camera mode dword_A890C8 (0=FP on-foot, 1=vehicle 3P) + tracked entity dword_A890CC | disasm; §5.39 | confirm-only |

First-person weapon viewmodel placement — weapon.def `pos`/`tpos` (net-re §5.40, 2026-06-21):

| original | addr | role | evidence | status |
|---|---|---|---|---|
| `Player_RenderFirstPersonViewModel` | `0x4ded60` | builds root_matrix from g_view_euler_translation_out (the `pos`/`tpos`-biased view from `Player_UpdateFirstPersonCamera`), renders gfx1 gun + character arms at it | disasm; §5.40 (paired to `main_game._update_player_camera` / `GameWorld.build_local_player_viewmodel`) | confirm-only |
| weapon.def `pos`/`tpos` parser | `0x54471f` | `tpos` token handler (mirror of `pos`): POSITION = `atof(str) × 256.0` (flt_7D1D70 @0x544770) → 16.16 world coord (net world offset = file/256); ROTATION = `ParseFixedPoint16 × 0x0B60B60` (=2³²/360) → 32-bit BAM; → `WeaponDef.Bone` (`pos`, @0xF4) / `AltCamOffset` (`tpos`, @0x10C) | disasm; §5.40 | confirm-only |
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
