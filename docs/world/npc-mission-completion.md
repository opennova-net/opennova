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
| Mission lifecycle | Objective feedback, the win path's epilog (00TRa has no win objective), retail's in-game RESTART (`UI_IngameRestartCommand @0x555410` -> `Game_RestartRoundSP`, the SP epilog/respawn flow, D-AI-10 / D-LOADSCR-8) and the playthroughs of the later training and co-op missions; 00TRa's failure, exit and repeat launch are accepted below | MissionKernel, Session and mission-end tests; the `mission_playthrough` probe |
| Runtime shutdown | A forced SceneTree quit (including `--quit-after`) with streaming music bypasses the orderly, 1000 ms-bounded drain that normal quit performs (Godot shutdown ordering, not an engine divergence) | audio/mus-sbf-re.md Godot playback note; `music_service.gd` `await_playback_stopped` |

## Evidence rules

Record behavior, original address, test and observed result in the owning RE
record as each capability lands. Delete completed rows from the outstanding
table; git history preserves the work sequence. Keep mission playthrough
results separate from parser, compiler and focused behavior tests. A green
compilation corpus is not evidence of script execution or mission completion.

## Playthrough instruments

The `mission_playthrough` runtime probe ([docs/mcp.md](../mcp.md), `godot/probes/runtime/`)
plays a mission's authored sequence through a fixed list of gates whose
interactions ride the real input path — the real key bindings, the look
accumulator and the mouse for the movement gate, USE, the rounds, ESC and the
menu — and writes a per-gate verdict, a sample log and a PNG per gate under
`user://probe-runs/<run>/` (never committed). Only the traversal between gates
is staged: `travel=teleport` (the default for now) moves the player through
the debug teleport, `travel=walk` steers on the camera through the real
movement keys; no other debug staging (crew, kill-group, mission-variable or
time-scale) is ever used, and the verdict records the travel mode. `auto`
drives every key itself; `observe` watches the maintainer play. A run resumed
with `start_gate` is an iteration aid and never acceptance evidence. The engine
half of the same sequences is the mission-parametric `lose_flow` ctest
(`tests/mission/lose_flow_test.cpp`: `lose_flow_04tr`, `lose_flow_00tra`; its
`--events` mode prints a mission's BMS event table with trigger and action
names, the Triggered Text each OutputText resolves to, the area zones and the
events the PreMission pass fires) and `vehicle_ride_00tra`.

## Baseline

Implementation started 2026-09-08.

**2026-09-12: the first playthrough acceptance — `00TRa.bms` (Training: Basic
Controls / Armory, the retail catalogue's first mission).** Run
`mission_playthrough-20260912 151609-1c4c` of the `mission_playthrough` probe in `auto` mode on commit
this slice over master a460d0c6c (branch `playthrough-00tra`), the retail install mounted through `OPENNOVA_JO_DIR`, windowed
1280x720. The retail sources read for the gates: `00TRa.bms`
(sha256 `64dd18b3…`), `00TRa.wac` (`b9a30b13…`, the single line `If
true(bluekills) then Lose (1)`), `00TRa.bin` (`fcb9d47d…`), all through
`opennova-extract`.

| Gate | Result | What the run witnessed |
| --- | --- | --- |
| 1 load and the retail-initialized player | PASS | the single-player role, 49 BMS events, the briefing title resolving from the mission table, the player at the barracks spawn, truck 11 present, the round not ended |
| 2 movement, view, stance | PASS | W moved the player 5.5 u; the look moved the camera (45.5 px per degree on both axes); X, Z, C set the stance record to crouch, prone, stand |
| 3 from spawn to truck 11 | PASS | the debug teleport to 2.8 u beside the truck (`travel=teleport`, the navigation stand-in this run used between gates). The `walk` mode was exercised in the iterations before it: steering on the camera through the real keys, with detour legs around the market stalls and the range fence, it reached the truck in 12 to 55 s |
| 4 board through USE | PASS | the release edge mounted the player on SSN 11 (seat 7, type 1) and event 2 (PLYRATTACHED 11: RedirectGroupTo 3 list 2 + PatrolSpeed 40) fired |
| 5 the instructor-driven ride | PASS | the truck drove 423.5 u, the rider carried at a gap of at most 0.8 u; events 3, 4 (list-2 waypoints 15 and 19: speed 20, then stop + ShowWaypoints) and 45 (the attached-player dialog 28) fired; the boarding dialog 29 (event 38) had played at the truck |
| 6 arrival and dismount | PASS | the truck at rest at the range; USE released the seat |
| 7 the armory | PASS | the player in a type-6 armory volume at the range (by teleport beside the placed armory items; on foot in the earlier iterations), USE opened weapon.mnu over live play, ESC closed it back to the world |
| 8 the authored friendly-fire failure | PASS | real rounds through the input path at the seated instructor (net 1, the driver of truck 11, the truck parked at the range) from the fourth of eight firing spots around him: the three before it landed nothing through the cab; `bluekills` 2 (see below), the WAC `Lose(1)`, the `lose` effect with `STRMISC_KILLEDBLUE` and `round_end` winner 2 exactly once, gameplay input parked, the MISSION FAILED screen after the beat with the banner "You have killed a teammate." |
| 9 the clean exit | PASS | ESC on the end screen returned to the menu with the world unloaded |
| 10 the repeat launch | PASS | the same mission again from the menu: the spawn and the truck's rest pose identical, the round not ended, the fired-event set the boot set of the first launch (the trigger-less events the PreMission pass fires). This is a repeat launch from the menu, not retail's RESTART, which is unported (the Mission lifecycle row above) |

What the run found, each witnessed in IDA and fixed in the same slice: the HUD
declutter level persisted the death screen's forced blank across missions and
sessions, where retail's cycle and death force write only the live layer level
and every mission start re-seeds it from the config
([interface/hud-re.md](../interface/hud-re.md), the HUD declutter section); the
script tick ran on through the SP lose epilog, so the WAC `Lose` re-fired every
second during the end-screen beat, where retail's `g_epilog_screen_active` halts
it two frames after the round end ([world-wac-ai-re.md §20.8a](world-wac-ai-re.md)).
Observed and faithful: the range-arrival dialogs (events 46/47) never play for a
truck-11 rider because of the flat left-to-right trigger fold retail shares
([bms-event-runtime-re.md §1.4](../mission/bms-event-runtime-re.md)).

Two observations from the run that still need a retail witness, not claimed
as divergences: the round-outcome record read `bluekills` 2 for the one
person killed (the `lose_flow_00tra` ctest's staged single round reads 1), and
a seated person's card health does not move while the occupant damage
accumulates through the cab (the probe counts bursts per firing spot instead).

Not exercised by this playthrough and still owed: the on-foot tour narration
(events 13 through 37, zones near the barracks), the second truck's ride
(events 5 through 7 — stepping into zone 25 beside the armory truck launches
it), the retail RESTART, walking traversal as the acceptance mode, and every
later mission.

The registry inventory finds 165 WAC entries: all 165 have explicit VM branches
and none reach the unsupported-command fallback. This is a dispatch inventory,
not a parity claim; the per-entry witnesses live in world-wac-ai-re.md section
33 and the [routing inventory](npc-script-coverage.md), which also lists the 150
named BMS trigger/action values, the 24 brain rows and the motor/presentation
acceptance families.
