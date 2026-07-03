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
| deploy gate / eye-pos ref / budget ramp | not ported | `emit_connection_s2c` anchors to entity pos, no `state==6` gate |
| body motor for net-snapped peers | not ported (D-NET-143 tail) | retail host SIMULATES remote players; ours net-snaps — anim STATE stays the 0x2B idle default until the motor drives peers |

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

**Next (round 5 remainder):**
- **D-NET-147** — FIXED 2026-07-03 (live teleport re-verify pending): the 0x10 flag-0x20 dword
  is the entity FLAGS (entity+36) streamed raw — NOT a parent slot; now composed at promote
  (BMS Indestructible/Reflective/NoShadow + Building kind) + item-traits (hp==0 → 0x4000000 +
  subType 0xFF) and streamed with ammo (BMS byte 81) / refNum (byte 153). Deferred: sectioned
  sectionMask rebuild, armory weaponByte/attachRef, scoreFlag gate. See net-re D-NET-147.

**Also open (tracked):** the body motor for net-snapped peers (live off-14 anim states + off-15
channel ratio — remote players render idle-posed); host-side `WeaponSlot_ReloadAmmo` bookkeeping
for the 0x25 relay; the armory-enable restriction table (`unused6 @ 0x24D5600` / player+89688 —
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
