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
| BMS event runtime + mission→world promotion (`libs/mission` event_runtime/promote) | 2026-06-10 | **MATCHING** (D-EVT-1..4) | [bms-event-runtime-re.md](mission/bms-event-runtime-re.md) |
| World / WAC VM / AI + infantry motor (`libs/world`, `libs/wac`) | 2026-06-07..10 | **MATCHING** (infantry D-INF-1..5; vehicle/HELO movement physics are tracked `not_yet_ported` stubs) | [world-wac-ai-re.md](world/world-wac-ai-re.md) — carries its own correspondence map (§2 there) |
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
| `Entity_InitFromItemDef` | `0x49e550` | item-template → entity field copy: `entity+28` idx → `gItemDefs[idx]`; +286 healthMax, +288 armorMax, +48/52/56 counters, +432 timing, +452/456 callbacks; dispatches item init cb | decompile; §5.2b / §6.9 | confirm-only |
| `Entity_ResetToSpawnState` | `0x4B9610` | spawn/respawn reset: backs up pos as spawn point, splats Yaw across heading fields, **clears `Flags & 2` (entity+36 movement gate)**, zeroes vel/AI refs, detaches vehicle, removes pool 0/1 cross-refs | decompile; §5.2b / §5.6 | confirm-only |
| `Server_SendInitialGameStateToPlayer` | `0x51bba0` | **server-side source of the S2C loading sequence** (0x2C/08/2A/1C/0B/66/76/11 + 0x10/0D/0C/20/45/7E/1A) → game-state 9 | decompile; §5.2a (P6 emitter spec) | confirm-only |
| `NapiClient_WaitForGameStart` | `0x42cc10` | shared host+client loading-wait loop; pumps in-process until spawn gate `dword_24C1928` set | decompile; §5.2/§5.2a | confirm-only |

Local-player input→pose locomotion (Phase 2, 2026-06-20; net-re §5.38):

| original | addr | role | evidence | status |
|---|---|---|---|---|
| `Entity_UpdateInfantryAI` | `0x4b9910` | infantry motor; branch @0x4b9a74 jumps to `loc_4B9C3E` (SIMULATE) when `is_authority` OR `entity==g_local_player_entity` — so the local player ALWAYS simulates; fall-through @0x4b9a8c = network interpolation toward smooth-target +0x234 (REMOTE entities on a client only) | disasm; §5.38 (paired to `AiSystem::tick_infantry`, world-wac-ai-re) | confirm-only |
| `Input_ProcessMouseAxisBindings` | `0x499680` | LOOK: mouse deltas × sensitivity (`dword_24D207C<<11`, 16.16) → `Input_TryTriggerMouseAxisBinding(.., entity, dX, dY)` → Yaw(+0x10)/Pitch(+0x14) | disasm; §5.38 | confirm-only |
| `Player_PackInputStateToEntity` | `0x4df450` | MOVE: `g_inputFlags` → 8-way move index → `entity->pad7[12]` (= entity+0x12C): index, +8 is_moving, fire/lean/scope/grenade bits; analog → pad7[16..19] | disasm; §5.38 | confirm-only |
| `Input_ProcessPlayerFrame` | `0x49d4c0` | per-frame input binding dispatch (keyboard/mouse-axis/toggle/analog); calls `Input_ProcessMouseAxisBindings` | disasm; §5.38 | confirm-only |
| `Client_ProcessNetworkFrame` | `0x42c180` | client net frame: `Player_PackInputStateToEntity` (@0x42c3e9) then build C2S 0x0C via `Player_BuildTag0CInputBody` (@0x42c482) | disasm; §5.38 | confirm-only |
| `Player_BuildTag0CInputBody` | `0x42a550` | serializes the live pose into C2S 0x0C; gate `entity+286 (healthMax)!=0 && (entity+36 & 2)==0` → `NetPacket_SerializePlayerState` | disasm; §5.38 / §5.6 | confirm-only |

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

## 6. Host Command wiring ([ADR 0001](adr/0001-mnu-action-command-boundary.md), matches)

`UI_DispatchScreenEvent @ 0x54e6a0`, `UI_ShowPreGameMenuByState @ 0x568d10`,
`UI_RegisterOptionsCallbacks @ 0x55d610`, `Expansion_SwitchTo @ 0x5688c0`,
`Expansion_ReloadAllAssets @ 0x568370`.

## 7. Reading the tables

- **addr** is the join key — names drift across IDB passes, addresses don't.
- MNU tag literals are UTF-16 wide; to re-locate one after an IDB rebuild, search
  ASCII-interleaved-with-0x00 bytes or re-anchor via `make_signature_for_function`.
