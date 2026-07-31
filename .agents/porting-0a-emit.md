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

## What is ported vs not (updated 2026-07-04, post round 14c / v33)

> Historical snapshot: this table predates the #300-series work (loadout kit
> seam D-NET-168, respawn/deploy re-pick D-NET-186/187, remote weapon channel
> D-NET-188, and later rows through D-NET-195). The ledger's D-NET table and net-re §8 are
> authoritative for current ported-vs-not state; read this table as the
> emit-side witness map, not live status.

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
| off-14/15/16 anim bytes (LIVE states + ratio + adm index) | DONE (D-NET-143 → D-NET-159) | off-14/15 = the motor mirror (`AiSystem::mirror_wire_anim`, pending-wins); the AUTHORITY selection runs for net-snapped peers from the replicated input (`remote_player_body_anim` [orig: @0x4B40E0]); stance rides C2S 0x1D → `Entity::net_stance_bits` + the tail echo; off-16 = `Entity::equipped_adm_index` (category<11-gated [orig: @0x4C20A3]). Deferred inside D-NET-159: run/jog promotion (ADM gait class), prone lean, real .adm channel rate for data-less hosts, deathAnim variants |
| joiner spawn health (tier byte 0x28) | DONE (D-NET-144) | spawns seed `World::player_item_hp` at full (150/150) [orig: Entity_InitFromItemDef @0x49e550] |
| **grounded-on-entity carrier replication** | DONE (D-NET-151; v27 user-confirmed) | the player record's carrier = mount-else-`groundEntity` [orig: @0x4c0a08] with CARRIER-LOCAL pos + local yaw byte; the uplink apply lifts local→world via the 22-bit pose transforms [orig: @0x4c1de1/@0x43BD00] and mirrors the carrier into `Entity::ground_target`; flags bits 2-4 are a REPLACE, not an xor [orig: @0x4c1e4d]. `apply_player_intent` + `build_0a_frame` + `NetClientView`; pinned by `netsim_two_peer_fanout` (grounded_uplink_apply_and_echo, pose_transform_roundtrip) |
| **C2S 0x06 fire → tag-2 round-event echo** | DONE (D-NET-152; **live-verified v28 + the v30 positive witness** — 95/95 0x06 armory-resolved, no reload wedge; v30: 25 tag-2 round-events on the wire with two observers, every fire echoed exactly once; rounds select FIRST in the frame budget since D-NET-154) | dispatch case 0x06 (anti-spoof + armory clip authority + `world::RoundRing` append [orig: @0x513310 → Server_ClientFiredRound @0x50baa0 → the adm fire action → RoundData_AddRound @0x4fdb40]); 0x25 refills the clip [orig: WeaponSlot_ReloadAmmo @0x541720]; netsim `select_round_events` = per-connection watermark + own-shooter skip + line-of-fire scoring [orig: @0x4ffee0] feeding `build_0a_frame`'s tag-2 records [orig: NetPacket_SerializeRoundEvent @0x504820]. Pinned by `npruntime_client_fire_test` + `netsim_two_peer_fanout` (round_event_fanout). Deferred: round SPAWN + damage (RoundData_SpawnRound @0x4ec0d0), fire-rate stamp (adm[276] unparsed), ammo pools, cease-fire |
| deploy gate / eye-pos ref / budget ramp | partial | the deploy-screen HOLD + release are DONE (D-NET-156: `Connection::respawn_pending` → flags1 bit1 + hidden bit; 0x0E dead-or-pending gate; the 0x5A+0x61+0x1E release bundle — D-NET-156 tail); eye-pos anchor + budget ramp still not ported |
| victim death cycle (tail health + dead bit) | DONE (D-NET-160; verify v34) | the 0x0A tail carries the recipient's LIVE health (`FrameHeaderState::tail_health` [orig: @0x4305df]); `route_round_deaths` sets the entity dead bit (flags\|=2 → record byte13 0x02 [orig: @0x4c1005]; the 1→0 edge = the client spawn hook [orig: @0x4c1109]), lifted by the deploy/respawn reset |
| vehicle attach/detach (C2S 0x26/0x27) | DONE (D-NET-157; emplacements live-verified v33) | dispatch → `world::entity_process_vehicle_attach/_detach` [orig: @0x502390/@0x4FC980 → @0x435AA0/@0x4946D0]; the 0x0A mounted branch echoes bone byte0 + carrier + the tail mount handle |
| **vehicle DRIVE (host motor off the driver's replicated input)** | DONE — ground family (D-NET-161; verify v35). The round-15 witness REFUTED the prior model: NO vehicle uplink exists (modes 3/4 return −1; the client serializes only g_local_player_entity @0x42c482; golden 344/344), the §5.13 flags&4 short form is the DEAD-pose (wreck) form, and vehicle +0x1CC is an effect-emitter handle, not a session grant | `world::tick_vehicle_motor` [orig: @0x48af00] per pool-1 traits entity in the AiSystem tick; items.def physics parse [orig: @0x49d870] -> `world::VehicleTraits`; `drain_connection_c2s` stays player-only (CORRECT). Deferred: air/helo family (Super Pumas parked), skid, vehicle collision, water, autopilot, engine states, wheel-contact pitch/roll |
| **AS capture loop (slice 2: control/flips/0x6F/0x53/0x1E/0x40)** | DONE (D-NET-162; verify v35) | `world::zone_capture_tick` + the npruntime 1 Hz wire block; map colors + LFP capture; deferrals in the D-NET row (timed engine/0x6C, waves, catch-up, scoring) |
| body motor for net-snapped peers | DONE (D-NET-159; live: death anims seen by others in v33) | `AiSystem::remote_player_body_anim` runs the anim selection for wire-snapped peers on the authority (position stays wire-owned); hidden entities skip [orig: @0x4b411b] |
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
  zones, arm-age child, 0x52/0x54/0x32, scoring. **v29 LIVE (same day, 2 retail clients):**
  kills + death broadcast worked (one 0x13+0x1E pair on the wire to both clients; victim
  redeployed) but three defects surfaced, all fixed same-day: **D-NET-153** fire-direction
  frame (0x06 yaw = the MISSION BEARING, not the euler_z 90−yaw frame — wire-proven off the
  duel geometry; caused "first joiner kills second, second can't kill first" via the 45°
  coincidence), **D-NET-154** tag-2 starvation (rounds got only the entity walk's budget
  leftovers = 0 with a real vehicle set + the watermark still advanced — rounds now select
  FIRST), **D-NET-155** stale 0x16 roster (one-shot re-push to the joiner only → HUD count
  stuck at 2; now generation-driven to every in-match client, golden 31→39 preserved).
  Anims = the tracked body-motor item. **v30 LIVE (same day): D-NET-153/154 wire-VERIFIED —
  kills BOTH directions (0x13: victim 0x0001 killer 2 / victim 0x0002 killer 1 / the HOST
  player 0x0000 killed too) + 25 tag-2 round-events = the §5.9.1 positive witness; no ghost
  bodies (3 player entities all session, redeploy reuses the entity). D-NET-155's first cut
  was half-broken (the version check sat below tick_connections' spawned-peer skip → only
  ran on each conn's own completion tick; joiner 1 never got the 47-B list) — re-fixed:
  spawned conns evaluate the roster version every tick.**

- **Round 13 (2026-07-03): AS spawn selection + the zone-capture loop — witnessed end-to-end
  (net-re §5.61) AND slice 1 (spawn selection) PORTED, same session.** The kong
  "CWeaponSlotManager" cluster is the AS ZONE-SLOT CHAIN (renamed `ZoneSlotChain_*`, 46 IDB
  renames): zones = pools-1/2 entities with def attribs ChangeTeam(0x20000)/SpawnPoint(0x40000)
  + zone number entity+538 ← BMS byte 155 `lfp_group`; frontier = assigned-slot/mask/Z±1
  adjacency; deploy advertising = 0x0F/0x0A phase-0 owned-zone mask (golden 0x8 EXPLAINED:
  team 2 wholly owns zone 3) + 0x40/0x6F/0x53/0x6E; the pick = C2S 0x0E [i16 handle]
  (0xFFFE=frontier auto) gated on team + control≥1.0; placement = zone origin/6007
  scatter/per-team markers (Server_PositionPlayerForSpawn, ex-CMap misnomer); waves =
  g_spawn_wave_list drip (host options, default off); the whole capture loop runs in the
  1 Hz Server_TickUpdate block (control delta formula pinned incl. the underdog catch-up).
  ASH_I5A authors zones 1(t1)/2×2(neutral)/3(t2) as type-1359 bunkers. PORTED slice 1:
  `world::ZoneChain` + zone fields + 0x0E handler (deploy at pick, dead-only, computed 0x1E
  ev-0x3A hint) + per-recipient uniform mask + PER-TEAM join markers (both AS teams
  previously spawned in team 1's base — 6003 first-family-wins). `zone_chain_test` pins the
  ASH shape; 230/230 ctest; GDExtension rebuilt. Slice 2 = the capture loop emits
  (0x6F/0x53/0x6C + 0x1E zone events + flips), waves, seat deploys.
  **v31 LIVE (same day): FOUR FAILED riders → the next round is an IDA ALIGNMENT PASS over
  the v31 wire.** (1) Deploy screen still absent — ZERO C2S 0x0E all session (the picker
  never appeared client-side; slice-1 handler unexercised). The advertising gap, ranked
  leads: the 0x0A phase-0 mask can't reach a deploy-screen client (state==6 gate in
  Server_SendEntityStateToPlayer @0x517BA0, D-NET-134) → the pre-deploy carrier is likely
  the 0x0F BODY (`NetPacket_WriteWorldStateLoad0x0F @0x502D10` — UNWITNESSED, reads
  g_respawn_requires_team_dead); the 1359 zone objects' 0x0D records may lack the D-NET-70
  team-gated `spawn_flags & 0x10` bit; the CLIENT deploy-list builder is unwitnessed.
  (2) Vehicle attach dead: 2 C2S 0x26 on the wire (bodies `02 00 | 04 10 | 01 00` /
  `2d 10`), NO dispatch case 0x26/0x27 exists — port HandleVehicleAttach @0x502390 /
  Detach @0x4FC980 → Entity_AttachToVehicleSlot @0x4946D0 + the mount replication
  (0x0A op1 mount-else-ground echo, D-NET-151). (3) HUD player count STILL wrong —
  D-NET-155 fix #2 failed live too (only 6 S2C 0x16 all session, 2 joiners); STOP guessing
  0x16: IDA the client HUD count SOURCE (which global the HUD draw reads, which handler
  writes it). (4) Body anims (the tracked body-motor off-14 states / off-15 channel ratio
  echo) — promote into the pass: witness the C2S 0x0C extended-uplink anim fields → entity
  → 0x0A compact echo chain and port the echo. Artifacts:
  `.scratch/retail_join_v31_game.pcapng` (8.1 MB, udp.port==32768 filter of the 664 MB
  dual raw), host logs `host_std{out,err}_v31.log`, capture script `start_v31_capture.ps1`;
  histogram: 11046×0x0A / 921×0x0C / 45×0x06 / 6×0x16 / 0×0x0E / 2×0x26 / 4×0x13.

- **Round 14 (2026-07-03): the v31 ALIGNMENT PASS — all four riders witnessed AND ported, one
  session (D-NET-156..159; net-re §5.9/§5.10/§5.11/§5.20/§5.29/§5.33/§5.61 refreshed).**
  (1) **Deploy screen (D-NET-156)**: the picker is HELD by the 0x0A header `flags1` bit1
  (`slot+89912 & 0x10`, set at join iff `SpawnZoneList_GetCount() > 0` @0x51a6f2, cleared on
  deploy @0x517791) re-asserted EVERY frame (`g_deploy_screen_active = flags1 & 2` @0x42ff82) —
  our hardcoded `flags1 = 0x00` was the whole defect; the picker ROWS are client-local
  (`Entity_BuildSpawnZoneList @0x43EAE0` from local BMS). Ported: `Connection::respawn_pending`
  → per-connection flags1 + the entity hidden bit0 (byte13 0x01) + the 0x0E dead-or-pending
  gate/clear + 0x6E 1 Hz empty-group to pending/dead + optional parity (0x0F location names ←
  def-2044 markers, 0x0D zone byte/radius, live 0x04 slot bytes).
  (2) **Vehicle attach (D-NET-157)**: dispatch cases 0x26/0x27 →
  `world::entity_process_vehicle_attach/_detach` (anti-spoof word0, the @0x435AA0 validation
  order, the @0x4946D0 writes, detach clamped to the sender); the 0x0A mounted branch echoes
  bone byte0 + carrier + the header-tail mount handle — NO confirm tag exists.
  (3) **HUD count (D-NET-158)**: `count = accepted 0x16 rows − spectatorCount`; rows accepted
  only for 0x46-known slots. Fixed the trifecta: live 0x04 capacity byte (the walk terminator
  `g_max_player_slots` — was hardcoded 2), the join-time 0x46 broadcast (0x1CF7 @0x51D296),
  live 0x16 trailer counts (was `{2,0}`); 0x46 now answers the requested fieldFlags verbatim.
  (4) **Body anims (D-NET-159)**: the server RECOMPUTES anims — `Entity_UpdateInfantryPlayerBody
  @0x4B40E0` runs the selection for every player on the authority from the REPLICATED input;
  stance arrives via the newly witnessed **C2S 0x1D stance-change** (169/170/172 → MoveOrder
  bits 8-9), and the 0x0A TAIL state byte echoes the recipient's own stance back (our 0x00
  tail was force-standing crouched clients every frame — the long-standing crouch/prone bug).
  Ported: `AiSystem::remote_player_body_anim` + `mirror_wire_anim` + the 0x1D case + live
  record bytes 14/15 + the local player's move-input export. Deferred in D-NET-159: run/jog
  promotion (ADM gait class unplumbed), prone lean, real .adm channel rate for data-less hosts.
  19 IDB renames applied (Vehicle_HasEnemyOccupant, NetPacket_SerializeScoreboard0x16/
  PlayerSync0x46/WriteWorldStateLoad0x0F, Entity_RequestVehicleAttach/AttachToUseGunSlot,
  Entity_BuildMapPoiLists, the scoreboard/deploy globals, 6 GamePlayerEntity members).
  Tests: netsim_two_peer_fanout (hold + attach echo + anim bytes), infantry_test (remote
  selection), zone_chain_test (zone-info byte); 230/230 ctest; GDExtension rebuilt. **NEXT =
  v32 live**: picker appears + pick lands at the bunker, buggy enter (0x1004-family), HUD 3 +
  leave update, remote anims move (animState varies / animRatio sweeps), standard sweep
  (0 C 0x0F, no 0xC9).

- **Round 14b (2026-07-03): v32 LIVE + the same-day wire fix.** v32 verdicts: deploy
  picker APPEARS + picks land (C 0x0E on the wire, both joiners — D-NET-156 hold chain
  works); crouch/prone WORK (user-confirmed shadow change — the 0x1D + tail-echo fix);
  roster contract fully wire-correct (0x04 mySlot/max=4, walk + 0xC000 removals, the
  f=3016 join broadcast for slot 2, live 0x16 rows 1→2→3→2 with matching trailer counts)
  — the user's "HUD only right on the final joiner" is NOT visible in the wire, re-check
  v33; vehicle attach untestable (movement broken). **THE BUG: both joiners' C2S 0x0C
  stopped EXACTLY at their pick frame and never resumed** (j1: trickle f=204-771 every
  ~14f, 0x0E f=786, then zero for 3.5 min; j2 same at f=3274) — the pick sets the client's
  dword_81474C wait-gate and the deploy-RELEASE bundle is what clears it: golden f=240018 =
  **0x5A + 0x61 + 0x1E one datagram** (the 0x5A apply resets 81474C, §5.30). Ours sent only
  the 0x1E → host entity pinned at the deploy spot (input=0) → rubber-band. FIXED: the 0x0E
  success path re-sends the retained granted 0x5A + the session-seed 0x61 before the 0x1E
  (SessionReplyState::last_loadout_reply; round_sim_test pins it). Bonus witnesses: the
  body motor's hidden-bit skip @0x4b411b (pending player's channel frozen — golden ratio 40
  const; our remote pass now gates on it), fresh-join deploys have NO byte13 bit-0x02 edge
  (golden pre-deploy = 0x01 exactly), ratio 255 = retail long-idle. **NEXT = v33 live**:
  run/strafe after deploy (0x0C resumes at ~pick+1), then the v32 checklist (buggy enter,
  HUD 3 + leave, remote anims).

- **Round 14c (2026-07-03): v33 LIVE — the release bundle VERIFIED (3260 C 0x0C vs v32's 51;
  movement works, users ran to vehicles/emplacements) + the next defect wave triaged off the
  wire.** v33 verdicts: emplacement attach WORKS (0x26 bone-3 UseGun accepted, mounted echo
  clean); vehicle MOUNT replicates correctly (the other client saw `state=0x40
  carrier=0x1030`) but a SECOND vehicle model appears for the rider and helos/buggies can't
  be driven — no client ever uplinked a VEHICLE handle (0x0C handles were 0x0001/0x0002
  only), so the whole **vehicle drive-authority chain is unwitnessed/unported**: the ridden
  vehicle's record mounted form (flags bit 0x04 + rider-euler tail, §5.13 — ours stays
  unmounted-form with weap zeros), the vehicle ownerSession (+0x1CC) grant at ctrl/drvr
  attach, and the driver's vehicle-state uplink (Entity_SerializeVehicleState modes
  3/4) + the host's owner-gate extension for it (drain_connection_c2s only accepts the
  connection's own PLAYER handle) → NEXT ROUND'S WITNESS TARGET. Killee-never-knows FIXED
  same-day = **D-NET-160** (tail health was hardcoded 150 + no server-side dead bit; now
  live tail + `flags|=2` on death, lifted by the deploy reset → the wire 1→0 edge is the
  client spawn hook — death screen + redeploy through the dead-or-pending 0x0E gate).
  Map icons/waypoints/LFP colors + LFP capture = **slice 2** (the 1 Hz capture block:
  0x40 ×305-golden minimap colors, 0x6F ×268 control values, 0x53/0x6C, proximity, flips) —
  the declared next big port. "No spare magazines": the wire grants are golden-equivalent
  (M4=10 clips etc.; jox01 126-weapon indices consistent with what the clients fired) and
  identical at join AND deploy — needs the CLIENT-side 0x5A apply witness (@0x4290E0: where
  ammoPrimary lands vs what the HUD mag counter reads) before touching anything.
  Artifacts: `.scratch/retail_join_v33{,_game}.pcapng` (28 MB filtered), host logs
  host_std{out,err}_v33.log.

- **Round 15 (2026-07-04): the VEHICLE DRIVE round — the task's assumed chain REFUTED, the
  real chain witnessed AND ported, + slice 2 landed the same session (D-NET-161/162).**
  Witness keystones: `Client_ProcessNetworkFrame @0x42c180` uplinks exactly ONE entity
  (g_local_player_entity, @0x42c482) — vehicles NEVER ride a C2S 0x0C (modes 3/4 return −1
  @0x460578; golden 344/344 player-handle uplinks); the §5.13 `flags&4` short form is the
  DEAD-pose form (death sets `Flags |= 6`; golden f=237868 = 16 buggies dying in one frame;
  the golden's RIDDEN vehicle streams the 21-B full form its whole drive) — serializer
  renamed `Entity_SerializeVehicleState`, decoder `is_mounted` -> `is_dead_pose`, nw_pp
  labels `DEAD-POSE`/`live`; vehicle `+0x1CC` is the smoke/burn EFFECT-EMITTER slot
  (`CEffectEmitter_ReleaseSafe @0x5f69f0`, renamed from the CNapiSession kong misname) —
  no ownership grant exists; DRIVE = the host runs `Entity_UpdateVehiclePhysics @0x48af00`
  off the CONTROLLING occupant's (+0x170) replicated MoveOrder/Yaw/analog (gate
  `(Flags&0x100) && (local || authority)` @0x48b0ff — the driver's client is prediction).
  v33's "second model + can't drive" = our missing host motor (prediction-vs-pinned-wire
  fight). PORTED: libs/def physics block (scaled per @0x49d870), `world::VehicleTraits` +
  the resolve_item_traits stamp, `world::tick_vehicle_motor` (the ground-family authority
  core; buggy drives, helos = tracked deferral), `Entity::net_analog_*` through
  PlayerIntent, the AiSystem vehicle pass. SLICE 2 (D-NET-162): `world::zone_capture_tick`
  (secure latch + the @0x501120 delta formula + instant flips via neutral + enforcement) +
  the npruntime 1 Hz block (0x6F change-gated + deploy refresh, 0x1E zone family, 0x53,
  0x40 zone+vehicle overlay — `Entity_ClassifyForMinimap @0x50FA70` witnessed with the
  whole producer chain, §5.19). Tests: vehicle_motor_test (def-scaling + launch/clamp/
  reverse/turn-in-place/steer/coast/dead/selector pins), netsim_two_peer_fanout
  vehicle_drive_authority (remote-driver input spins the host vehicle; the streamed record
  pose goes live), zone_chain_test (delta pins + flip/secure/contest/neutralize/retake).
  IDB: 2 renames + 7 witness comments, saved. **NEXT = v35 live**: buggy drives under a
  retail driver (no second model), map/LFP colors on the deploy map + minimap, an LFP
  neutralize->take->secure cycle end-to-end, plus the v34 leftovers (death cycle) and the
  standard sweep.

**THE GAME-TYPE DECISION (2026-07-03, user-locked): ONE game type until it plays end-to-end —
ADVANCE AND SECURE on ASH_I5A, gametype 0x10010 (65552 = AS + team flag, the golden retail
value).** The wire has advertised AS all along; the internal "COOP" label (main_game.gd /
mp_menu_companion.gd boot dicts) was a bring-up leftover and is renamed to "AS"; the menu-shell
path now seeds the numeric gametype 0x10010 explicitly (it previously advertised 0). The
FULL-AS-GAME gap list, in rough order:
  1. **Spawn selection** — witnessed §5.61 + slice-1 ported round 13 (0x0E pick path,
     owned-zone mask, per-team join markers, frontier hint) but **v31 live FAILED: the
     picker never appears (zero C2S 0x0E)** — the missing piece is the pre-deploy
     ADVERTISING (the 0x0F body @0x502D10 / the 0x0D spawn_flags 0x10 bit / the client
     deploy-list builder — see the round-13 v31 bullet). Then the deferred riders: waves,
     vehicle-seat deploys, 6007 scatter, the D-NET-150 spawn-menu re-stream.
  2. **AS zone capture loop** — WITNESSED round 13 (§5.61: the 1 Hz block — proximity
     @0x5086A0, secure pass @0x519690 w/ 0x6F + 0x1E 0x3B/0x3C, timed engine @0x53B8F0 w/
     0x53×4 + 0x6C, control formula @0x501120, flip events 43/44/50-53/56/57, team
     enforcement @0x519600); PORT = slice 2 (world-side control delta + flips + the four
     emits; the chain/latch/registries landed with slice 1).
  3. **Scores** — 0x16 scoreboard refresh cadence (Server_BuildAndBroadcastScoreboard
     @0x50de00 trigger set) + the 0x52 kill-stat pairs + kill/death tallies.
  4. **Round end** — Server_CheckWinConditions @0x51ad40 (AS = zone-hold win) →
     Server_ProcessRoundEnd (0x1D/0x61/0x25 family) → the next-round reset.
  5. **Death presentation** — stream the death anim state (the body-motor off-14/off-15
     item) so bodies lie down instead of idling at the death spot.
  6. Combat completeness already tracked: drag/falloff, bone zones, spread, 0x52/0x54/0x32,
     scoring, vehicles.

**Also open (tracked):** the D-NET-159 anim stand-ins (run/jog promotion — the ADM gait class
`dword_24E808C[adm*0x460]` is not in our weapon table; prone lean 41/42; deathAnim variants;
the 62-tick channel-rate stand-in on anim-data-less hosts); the D-NET-157 seat gaps (gun
seatType byte via carrier +0x326/+0x312, the weapon-busy gate, ctrl-seat def attribs);
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
  kill the running Godot instance first) before a live test.
