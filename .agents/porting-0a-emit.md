# Porting the per-frame S2C 0x0A emit (faithful, IDA-driven)

The in-match replication core is the per-frame S2C `0x0A`. The retail host builds it in a small,
fully-witnessed chain, and the port in `engine/runtime/replication` (`connection_fan.cpp`:
`build_0a_frame`, `select_frame_entities`, `select_round_events`) is a structural translation of
that chain, never an invented broadcast. This file is the runbook only: what to read, the chain
to port against, and the verify loop. Ported-vs-not state lives in `docs/divergence-ledger.md`'s
D-NET table and `docs/net/novaworld-net-re.md` §8; the 2026-07 rounds that built the emit are
recorded there and in `engine/runtime/inmatch/ROADMAP.md`, not here.

Read first: `docs/net/novaworld-net-re.md` §5.9 (wire format), §5.46 (the 0x0F flood context), and
**§5.47 (the server emit path: the port spec)**. Closure: D-NET-134 (FIXED 2026-08-03).

## The original chain (the port target)

Per recipient, per frame, all in `Jointops.exe` (IDA @ 127.0.0.1:13337):

- `Server_SendEntityStateToPlayer @0x517ba0`: gate on `playerSlot.state(+0x20) == 6` (DEPLOYED); set
  `g_PriorityRef{X,Y,Z}` = the recipient's **eye** position (`entity.pos + camera_offset`);
  `Server_BuildEntityPriorityList @0x50e590` (distance-sorted); write header + entity loop; send via
  `NapiNPServer_SendFiltered` mask `0xA0`, tag `0x0A`. New/stale recipient (`uptime > 2000t`) ->
  `g_EntitySendBudget >> 1` (ramp-up). The per-slot phase byte `playerSlot+100566` is `++`'d here
  (`@0x517be8`).
- `NetPacket_WritePlayerState @0x4ff6b0`: header `[i32 ref x/y/z][u8 state_flags][u8 phase]`, then
  `phase & 3` selects the sub-block (**0** weapon/ammo/uniform, **1** server-status, **2** env, **3**
  gametype), then the recipient tail; `phase & 0xF == 8` adds a mounted-vehicle/turret tail every 16th
  frame. Env scales: `wire_tod = time_of_day(hours*2^16) >> 5`, `wire_fog = fog_dist(16.16) >> 16`
  (verified vs golden `todFixed=0x7905`).
- `NetPacket_SerializeEntityStatesToPacket @0x50f070`: entity loop; for EVERY priority-list entity with a
  serialize callback (`itemDef+0x164`: players, vehicles, AI), `[1][handle][type=*(itemDef+0x50)]
  [compact]`; projectiles `[2]...`; `[0]` terminator. **Budget-limited round-robin** across frames.

## Verify loop (fast iteration)

- **Unit:** `netsim_two_peer_fanout` (`run_0a_subblock_phase_cycle`) pins the header cycle
  deterministically; no live client needed.
- **Coverage diff:** `scripts/net/diff_vs_golden.ps1` was retired with the capture gates
  (ca1cef465); the shape diff below is the surviving golden comparison.
- **Shape diff:** `python scripts/net/diff_0a.py --ours <cap> --golden <golden> --items <ITEMS.DEF>`
  compares sub-block distribution + record-class mix + per-field population vs the retail-host golden. The
  golden profile caches to `<golden>.0a.json` (instant re-runs; `--refresh` after a decoder change). This
  is the machine-readable "are we sending what retail sends" check.
- **Golden:** `retail-ashi5a-*.pcapng` = retail host + retail joiner on ASH_I5A (the spec; C 0x0f
  = 0). Captures are machine-local and never committed (`docs/asset-gated-tests.md`); the live
  re-capture runbook is `.agents/retail-lan-parity.md`.

## Rules

- Port the witnessed original; cite `[orig: Name @ 0xADDR]` at the port site. Where our world lacks a
  source, defer with a tracked D-NET divergence rather than guessing bytes. D-NET-134 is closed: phase 2
  uses the live mission/runtime environment owner and quantizes only at the wire boundary.
- Never carry raw capture bytes through the encoder (ADR 0003).
- After an `engine/runtime/replication` change, rebuild BOTH `build/` (ctest) and the GDExtension
  (`scripts/build_godot.sh`, kill the running Godot instance first) before a live test.
