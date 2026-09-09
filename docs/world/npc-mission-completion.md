# NPC AI and mission scripting completion

This effort targets shared engine behavior against retail `Jointops.exe`,
using `Jointops.exe.kong.i64` at imagebase `0x400000`. Missions are acceptance
coverage, not a source of mission-specific runtime behavior. Persistent
mid-mission savegames are outside this effort; normal playthrough, failure,
retry and mission transitions are included.

## Completion gates

- Every NPC motor, brain handler, authored AI command, WAC registry entry and
  BMS trigger/action has an implemented consumer or a witnessed retail no-op.
- Unsupported behavior is observable across boot and every simulation tick.
- NPC motion, targeting, weapons and presentation use the authoritative
  simulation and the existing mission kernel.
- Available SP and solo-playable co-op missions are exercised from their
  normal spawn through completion. Focused state-staged tests do not count
  as playthrough evidence.
- Restart resets mission state without stale targets, relationships, timers,
  projectiles or presentation ownership.
- Asset-gated tests that skip are recorded as unexercised coverage.

## Outstanding capability work

| Capability | Remaining work | Owning evidence |
| --- | --- | --- |
| Baseline and coverage | Review parity and consumer gaps in the complete routing inventory; cumulative diagnostics and native validation are in place | [Script/brain/motor inventory](npc-script-coverage.md); native/Godot tests |
| Infantry movement | Remaining water, ladder and parachute behavior | world-wac-ai-re.md; D-INF and D-AI records |
| Vehicle AI and motors | Remaining carrier spawn-marker localization, ground probes, brain-step consumers and emplacement dispatch | vehicle-client-movers-re.md; D-NET-161 |
| Perception and combat | Remaining cheat-rule and combat solver behavior, concealment, ray behavior and guided projectile flight | world-wac-ai-re.md sections 16..18 |
| NPC presentation | Held weapons and remaining aim/fire/death presentation | world-wac-ai-re.md; D-WPN-32 |
| BMS actions | MP POI/live-marker ownership and remaining shared script/native effect-slot consumers | bms-event-runtime-re.md |
| WAC execution | Complete registered runtime semantics and presentation routing; replace silent effect fallthrough with explicit disposition | formats/wac command registry; runtime/wac VM |
| Mission lifecycle | Playthrough, objective feedback, success/failure, retry and transitions; fix adjacent blockers | MissionKernel, Session and mission-end tests |
| Runtime shutdown | A forced SceneTree quit (including `--quit-after`) with streaming music bypasses the orderly, 1000 ms-bounded drain that normal quit performs (Godot shutdown ordering, not an engine divergence) | audio/mus-sbf-re.md Godot playback note; `music_service.gd` `await_playback_stopped` |

## Evidence rules

Record behavior, original address, test and observed result in the owning RE
record as each capability lands. Delete completed rows from the outstanding
table; git history preserves the work sequence. Keep mission playthrough
results separate from parser, compiler and focused behavior tests. A green
compilation corpus is not evidence of script execution or mission completion.

## Baseline

Implementation started 2026-09-08. No mission playthrough has yet been accepted
under these completion gates.

The registry inventory finds 165 WAC entries: all 165 have explicit VM branches
and none reach the unsupported-command fallback. This is a dispatch inventory,
not a parity claim; the per-entry witnesses live in world-wac-ai-re.md section
33 and the [routing inventory](npc-script-coverage.md), which also lists the 150
named BMS trigger/action values, the 24 brain rows and the motor/presentation
acceptance families.
