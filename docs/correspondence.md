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
| MNU/MNS menu UI (`libs/mnu`, `libs/mnu_xml`, `libs/mns`, `godot/engine/mnu`) | 2026-06-01 format + 2026-06-09 menu slice | **matching** (D-MNU-1..3 accepted) | [menu-re.md](mnu/menu-re.md) |
| LWF banks / DBF dialogs / member selection (`libs/lwf`, `libs/dbf`, `libs/audio`) | 2026-06-09 | **matching** (D-SND-1..3) | [lwf-dbf-sound-re.md](audio/lwf-dbf-sound-re.md) |
| MUS VM + compiler / SBF codec / SCR container (`libs/mus`, `libs/sbf`, `libs/scr`) | 2026-06-09 | **MATCHING** per component (D-SCR-1/2; host audio glue not grillable) | [mus-sbf-re.md](audio/mus-sbf-re.md) |
| Environment / time-of-day (`libs/env` + `env_render`; `nova_environment`/`nova_sky`/`nova_weather`/`nova_celestial`) | 2026-06-09/10 | per subsystem: parse/TOD/sun-moon/fog **matching**; weather + sky dome **divergent → ported/aligned**; celestial + glare new-from-witness; iris/terrain_rgb unknown → documented | [env-tod-re.md](env/env-tod-re.md) |
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
| `MnsStyleSheet::substitute` (`godot/engine/mnu/mns_stylesheet.cpp`) | `NapiXML_ExpandVariablesInText` | `0x63a000` | `%VAR%` expansion | whole-buffer pre-parse | divergent (scope/timing) → accepted as D-MNU-1 ([ADR 0005](adr/0005-mnu-var-expansion-policy.md)) |
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

## 6. Host Command wiring ([ADR 0001](adr/0001-mnu-action-command-boundary.md), matches)

`UI_DispatchScreenEvent @ 0x54e6a0`, `UI_ShowPreGameMenuByState @ 0x568d10`,
`UI_RegisterOptionsCallbacks @ 0x55d610`, `Expansion_SwitchTo @ 0x5688c0`,
`Expansion_ReloadAllAssets @ 0x568370`.

## 7. Reading the tables

- **addr** is the join key — names drift across IDB passes, addresses don't.
- MNU tag literals are UTF-16 wide; to re-locate one after an IDB rebuild, search
  ASCII-interleaved-with-0x00 bytes or re-anchor via `make_signature_for_function`.
