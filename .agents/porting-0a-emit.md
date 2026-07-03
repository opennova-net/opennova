# Porting the per-frame S2C 0x0A emit (faithful, IDA-driven)

The in-match replication core is the per-frame S2C `0x0A`. The retail host builds it in a small,
fully-witnessed chain; our job is to port that chain into `libs/netsim` **structurally**, not to invent
a broadcast. This doc is the runbook + current state so you can continue without re-deriving.

Read first: `docs/net/novaworld-net-re.md` §5.9 (wire format), §5.46 (the 0x0F flood context), and
**§5.47 (the server emit path — the port spec)**. Divergences: D-NET-134.

## The original chain (the port target)

Per recipient, per frame — all in `Jointops.exe` (IDA @ 127.0.0.1:13337):

- `Server_SendEntityStateToPlayer @0x517ba0` — gate on `playerSlot.state(+0x20) == 6` (DEPLOYED); set
  `g_priority_ref_{x,y,z}` = the recipient's **eye** position (`entity.pos + camera_offset`);
  `Server_BuildEntityPriorityList @0x50e590` (distance-sorted); write header + entity loop; send via
  `NapiNPServer_SendFiltered` mask `0xA0`, tag `0x0A`. New/stale recipient (`uptime > 2000t`) →
  `g_entity_send_budget >> 1` (ramp-up). The per-slot phase byte `playerSlot+100566` is `++`'d here
  (`@0x517be8`).
- `NetPacket_WritePlayerState @0x4ff6b0` — header: `[i32 ref x/y/z][u8 state_flags][u8 phase]`, then
  `phase & 3` selects the sub-block (**0** weapon/ammo/uniform · **1** server-status · **2** env · **3**
  gametype), then the recipient tail; `phase & 0xF == 8` adds a mounted-vehicle/turret tail every 16th
  frame. Env scales: `wire_tod = time_of_day(hours·2^16) >> 5`, `wire_fog = fog_dist(16.16) >> 16`
  (verified vs golden `todFixed=0x7905`).
- `serialize_entity_states_to_packet @0x50f070` — entity loop: for EVERY priority-list entity with a
  serialize callback (`itemDef+0x164` — players, vehicles, AI), `[1][handle][type=*(itemDef+0x50)]
  [compact]`; projectiles `[2]…`; `[0]` terminator. **Budget-limited round-robin** across frames.

## What is ported vs not (updated 2026-07-02, post v11-v15 live rounds)

| piece | status | where |
|---|---|---|
| phase counter (`playerSlot+100566`) | DONE | `netsim::Connection::s2c_phase`; advanced in `emit_connection_s2c` |
| header sub-block dispatch (`phase & 3`) | DONE | `build_0a_frame` switch, `connection_fan.cpp` |
| sub-block 1 (server-status, C6EAE4 fall-dmg) | DONE | load-bearing; first send is phase 1 |
| sub-block 0 (weapon) | DONE (golden steady: slots 0, uniformMask 8) | recipient weapon-slot model pending |
| sub-block 3 (gametype) | DONE (0 B for non-objective) | objective-gametype body pending |
| **sub-block 2 (env)** | **DEFERRED — D-NET-134** | host doesn't author `world.env`; would clobber client sky |
| passenger tail (`phase & 0xF == 8`) | deferred | needs vehicle-mount modeling |
| entity loop: priority + aging + 600-B budget | DONE (D-NET-134 step 2) | `select_frame_entities`, `connection_fan.cpp`; round-robin is EMERGENT from aging (no cursor) |
| replication classes from items.def | DONE (load-bearing) | `resolve_item_traits` (NovaSimulation) stamps `Entity::net_class_code` via `class_from_tag`; pool alone NEVER selects a record (ewep desync, v13) |
| field-17 health byte (packed tier\|class) | DONE (D-NET-138 FIXED) | `health_classification_byte` [orig: @0x4AD4E0]; killed the 0x0F flood (v11: 0 C 0x0F) |
| vehicle record health word (+286) | DONE (D-NET-63 corrected) | live items.def hp via `resolve_item_traits`; 0 = wreck, small = burning (v12/v13 regressions) |
| player record field sources (input/state/pitch/yaw-trunc/mount) | DONE | witnessed apply map in §5.10; off-12 is MOVEMENT INPUT (uplink-echoed), off-13 bit2 = dead/undeployed |
| 0x5A loadout reply derived from C2S 0x2F | DONE (set+order) | `build_tag_5a_weapon_loadout` (server_message_dispatch) |
| 0x5A ammo bytes (real counts) | DONE (D-NET-141; index rule CLOSED §5.57) | `world::WeaponTable` fed via `NovaSimulation::load_weapon_table`; rules in `weapon_table_build.cpp` [orig: WeaponSlot_GetTotalClips @0x5425F0]; table-less hosts keep the echo fallback |
| C2S 0x25 → S2C 0x49 reload relay | DONE (D-NET-142) | dispatch case 0x25 stages the relayed 0x49 on EVERY in-match transport incl. the requester [orig: @0x514DF0 → SendFiltered @0x4C87E0]; host-side clip bookkeeping deferred |
| off-14/16 anim defaults (43 idle / adm index) | DONE (D-NET-143) | off-14 = 0x2B idle default; off-16 = `Entity::equipped_adm_index` (the uplink byte-24 echo — ex-`reserved_24` — category<11-gated [orig: @0x4C20A3]; spawn default = the armory's WPN_M4AUTO index [orig: @0x4B1116]) |
| joiner spawn health (tier byte 0x28) | DONE (D-NET-144) | spawns seed `World::player_item_hp` at full (150/150) [orig: Entity_InitFromItemDef @0x49e550] |
| **grounded-on-entity carrier replication** | DONE (D-NET-151; v27 user-confirmed) | the player record's carrier = mount-else-`groundEntity` [orig: @0x4c0a08] with CARRIER-LOCAL pos + local yaw byte; the uplink apply lifts local→world via the 22-bit pose transforms [orig: @0x4c1de1/@0x43BD00] and mirrors the carrier into `Entity::ground_target`; flags bits 2-4 are a REPLACE, not an xor [orig: @0x4c1e4d]. `apply_player_intent` + `build_0a_frame` + `NetClientView`; pinned by `netsim_two_peer_fanout` (grounded_uplink_apply_and_echo, pose_transform_roundtrip) |
| **C2S 0x06 fire → tag-2 round-event echo** | DONE (D-NET-152; **live-verified v28** — 95/95 0x06 armory-resolved, no reload wedge; zero tag-2 = correct single-observer, positive fan-out = the netsim pin; v29 wants a 2nd observer client) | dispatch case 0x06 (anti-spoof + armory clip authority + `world::RoundRing` append [orig: @0x513310 → Server_ClientFiredRound @0x50baa0 → the adm fire action → RoundData_AddRound @0x4fdb40]); 0x25 refills the clip [orig: WeaponSlot_ReloadAmmo @0x541720]; netsim `select_round_events` = per-connection watermark + own-shooter skip + line-of-fire scoring [orig: @0x4ffee0] feeding `build_0a_frame`'s tag-2 records [orig: NetPacket_SerializeRoundEvent @0x504820]. Pinned by `npruntime_client_fire_test` + `netsim_two_peer_fanout` (round_event_fanout). Deferred: round SPAWN + damage (RoundData_SpawnRound @0x4ec0d0), fire-rate stamp (adm[276] unparsed), ammo pools, cease-fire |
| deploy gate / eye-pos ref / budget ramp | not ported | `emit_connection_s2c` anchors to entity pos, no `state==6` gate |
| body motor for net-snapped peers | not ported (D-NET-143 tail) | retail host SIMULATES remote players; ours net-snaps — anim STATE stays the 0x2B idle default until the motor drives peers |
| platform physics (host-side grounding) | not ported (D-NET-151 residual) | retail sets `Flags\|=0x100000` + `groundEntity` in the collision pass [orig: @0x4b3291]; our motor has no platform pass, so OUR OWN player never reports grounded and peer ground links mirror the owner's uplink |

**Round 5 (2026-07-02): D-NET-146 FIXED — the DBuggy1 shadow.** The joiner's 0x0C
`animSlot`/`netId` are the joiner's OWN uploaded per-side character selection: 0x42 CU vars
CI0/CI1/TR/CTA/CTB/VCA/VCB (net-re §5.0b) → `NapiNetConfig_LoadFromConnTags @0x4c7260` →
`Server_PlayerAdd @0x51cbc0` picks by ASSIGNED team (side A = teams 1/3 or a non-team
gametype) and stamps entity+0x374 / entity+0x15C; the 0x0C serializer echoes them raw
(@0x5032b8). Ours echoed the body-anim CLIP slot + the netId shim — the client's
character-slot registry (`MinimapSlot_FindOrAllocByEntityId @0x42eb1d`, keyed by the packed
NetId) then bound a vehicle-archetype entry → the DBuggy1 shadow decal under a correct mesh.
Reimpl: `Entity::anim_slot` SPLIT from the new `Entity::body_anim_slot` (present-pass clip;
never wire), `minimap_net_id` added, `handle_client_join` parses the CU vars, the spawn
stamps per side, our joiner uploads the fresh-profile default set (CI0=512 CI1=33287 TR=-1
CTA=CTB=8 VCA=1 VCB=4), and the NW_LAN_HOST boot seeds `gametype 0x10010` (the golden
ASH_I5A session g_GameType [orig: seeded from the host settings @0x4a6657]; its team-based
bit 0x10000 drives the side pick; NW_LAN_GAMETYPE overrides). `nw_pp --handshake` dumps the
outer 0x41/0x42/0x81/0x82 incl. the CU chunks; `NW_PP_HEXCAP_MAX` widens raw dumps. Full
chain + deferrals (char-slot registry realloc, BMS AnimSlot promote, WAC set_ssn_anim
target) in net-re D-NET-146.

**Rounds 9-10 (2026-07-03):**
- **D-NET-147** — FIXED (0x10 statics stream golden-shaped: entity FLAGS dword raw — NOT a
  parent slot — + ammo/refNum/subType); v26 re-verify was NEGATIVE for the teleport symptom,
  which turned out to be D-NET-151. Deferred: sectioned sectionMask rebuild, armory
  weaponByte/attachRef, scoreFlag gate. See net-re D-NET-147.
- **D-NET-151** — FIXED, v27 user-confirmed (no wire trace — the capture window expired
  before the morning test session): the building/vehicle-deck teleport was the unported
  grounded-on-entity loop (see the table row above + net-re D-NET-151 for the full witness
  chain). §5.10 decoder corrections rode along: `carrier_handle` (ex vehicle_handle),
  `state_flags_byte` replace-bits (ex flags_xor xor-delta — the crouch/prone family lives in
  those bits), `anticheat_flags` (ex reserved_18), priority `(handle,score)` pairs (ex
  "weapon/fire counters"); the 0x0A header anchor is the recipient EYE pos [orig: @0x517bf5],
  not a map origin; pool strides = per-pool entity struct sizes [orig: EntityPool_Allocate
  @0x442168: 904/1360/812/988/988].
- **Round 11 (2026-07-03): D-NET-152 client fire PORTED**, **live-verified v28** (95/95 0x06
  armory-resolved, shot_seq monotonic 513→607, no reload wedge; zero tag-2 = the correct
  single-observer outcome; D-NET-151 wire re-verified in the same trace — 4 carriers incl. a
  pool-1 vehicle deck, zero teleport signature across 923 uplinks). The full
  chain witnessed: the "+684 fire callback" is the adm **'fire' ACTION** (action table
  admEntry+676, suffix table 0x830B94 → `WeaponAction_Fire @0x542b10`); a net primary fire
  re-enters `Server_ClientFiredRound` in LOCAL mode via `Entity_FireWeaponAndSendPacket
  @0x42bd80` and lands in `RoundData_AddRound @0x4fdb40` → the 256-record ring
  `g_round_ring @0xC8D848` → per-recipient tag-2 (`Server_BuildRoundEventListForPlayer
  @0x4ffee0` + `NetPacket_SerializeRoundEvent @0x504820`). §5.9.1 was systematically
  mis-read ("weapon-hit"): the record is a ROUND-FIRED event — SHOOTER handle (not target),
  fire origin + direction (not impact), optional word = the shooter's live target; the
  receiving client re-simulates the round (`RoundData_SpawnRound @0x4ec0d0` — the renamed
  ex-`ProcessHit`). Rename sweep landed (IDB + `RoundEventRecord` codec + nw_pp + docs).
  **NEXT = the authoritative round spawn + damage** (`RoundData_SpawnRound` port: projectile
  entity, spread, `Projectile_Handle*Impact` → health → the death family 0x13/0x26/0x54).
  Scope in memory `project_retail_join_0a_fidelity`.
- **Round 12 (2026-07-03): the authoritative round sim + damage + death family — witnessed
  end-to-end (net-re §5.60) AND PORTED at the MVP altitude, same session.** Keystones:
  AddRound INLINE-spawns (the ring is only the fan-out log); damage is AUTHORITY-ONLY
  (three independent gates); the damage model is KINETIC — `min(62·|vel|,1219) ·
  weight_in_grains / 875 · zone` (falloff emerges from drag, no range table); death routes
  per-tick health<=0 → players S2C 0x13 + 0x1E kill feed (0x52/0x54/0x32 deferred), AI
  0x13, non-players 0x26 via the single Server_SendEntityStatePacket emit. Port: libs/def
  ammo.def parse (§5.60 token subset) → `world::AmmoTable` + round_type resolve →
  `world::RoundSim` (spawn on 0x06, per-tick flight/terrain/organics, kinetic damage) →
  Server_TickUpdate death routing + host respawn release → NovaSimulation::load_ammo_table.
  Pinned by `npruntime_round_sim_test` + `def_parse_ammo` (real 5.56 fixture fields).
  MVP deferrals in round_sim.h: bone zones, drag/gravity, spread, vehicles, explosion kill
  zones, arm-age child, 0x52/0x54/0x32, scoring. **NEXT = v29 live verify — TWO retail
  clients (shooter + observer: the positive tag-2 wire witness) + shoot the host player
  (death + kill feed + respawn).**

**Also open (tracked):** the body motor for net-snapped peers (live off-14 anim states + off-15
channel ratio — remote players render idle-posed; the v28 diff confirms `animRatio` still zero);
the armory-enable restriction table (`unused6 @ 0x24D5600` / player+89688 —
4th 0x5A byte, observed 0); the env sub-block (D-NET-134); the passenger tail; a weapon.def feed
for table-less hosts (headless `nw_server`). NOTE the armory truth is the HOST'S RESOLVED
weapon.def (VFS view — a live JO:CA root resolves 126 weapons; the committed fixture is a
94-weapon extract), so live index anchors belong to wire gates, not unit tests (§5.57 "Index
numbering").

## Verify loop (fast iteration)

- **Unit:** `netsim_two_peer_fanout` (`run_0a_subblock_phase_cycle`) pins the header cycle deterministically
  — no live client needed.
- **Shape diff:** `python scripts/net/diff_0a.py --ours <cap> --golden <golden> --items ~/Desktop/JOX/ITEMS.DEF`
  compares sub-block distribution + record-class mix + per-field population vs the retail-host golden. The
  golden profile caches to `<golden>.0a.json` (instant re-runs; `--refresh` after a decoder change). This
  is the machine-readable "are we sending what retail sends" check.
- **Golden:** `.scratch/retail-ashi5a-*.pcapng` = retail host + retail joiner on ASH_I5A (the spec; C 0x0f
  = 0). Live re-capture runbook: `[[reference_retail_join_test_stack]]` / the memory
  `project_0x0f_flood_root_cause`.

## Rules

- Port the witnessed original; cite `[orig: Name @ 0xADDR]` at the port site. Don't invent field values —
  where our world lacks a source (env, weapon slots), DEFER with a tracked D-NET divergence rather than
  sending guessed bytes that regress the client (env sub-block would darken the sky).
- Never carry raw capture bytes through the encoder (ADR 0003).
- After a `libs/netsim` change, rebuild BOTH `build/` (ctest) and the GDExtension (`scripts/build_godot.sh`,
  kill the running Godot host first) before a live test.
