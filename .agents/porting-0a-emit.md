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
| **0x5A ammo bytes (real counts)** | **OPEN — D-NET-141** | needs the weapon.def parse + the adm INDEX-allocation witness (§5.57 OPEN) |
| **C2S 0x25 → S2C 0x49 reload relay** | **OPEN — D-NET-142** | host silently ignores 0x25 → one reload attempt wedges the joiner's weapon (§5.58) |
| **off-14/16 anim defaults (43 idle / adm index)** | **OPEN — D-NET-143** | we send 0/0xFF; retail spawn defaults = state 0x2B + WPN_M4AUTO index [orig: @0x4B1116]; uplink carries +0x2B0 (our `reserved_24`) |
| joiner spawn health (tier byte 0x28) | OPEN — D-NET-144 | seed 150/150 or re-stamp item traits on player spawn |
| deploy gate / eye-pos ref / budget ramp | not ported | `emit_connection_s2c` anchors to entity pos, no `state==6` gate |
| body motor for net-snapped peers | not ported (D-NET-143 tail) | retail host SIMULATES remote players; ours net-snaps — anim state/ratio stay defaults |

**Next (round 3): the four v15 defects — D-NET-141..144.** Order: witness the adm index-allocation
rule (the `weapon` block-open handler in the loc_543680 parse callback + `AdmDef_FindFreeSlot
@0x53FC50`; does `loadout_subclasses N` reserve N slots?), then the weapon.def parser (libs/def) +
engine feed, then 0x5A ammo resolve + 0x2F validation, the 0x25→0x49 relay (needs a broadcast path
in the npruntime dispatch — replies currently go only to the requesting connection), the anim
defaults + `reserved_24`→equipped-adm uplink ingest/echo, and the spawn-health seed.

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
