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
| Infantry movement | Remaining terrain-gradient movement, water, ladder and parachute behavior | world-wac-ai-re.md; D-INF and D-AI records |
| Vehicle AI and motors | Remaining carrier spawn-marker localization, ground probes, brain-step consumers and emplacement dispatch | vehicle-client-movers-re.md; D-NET-161 |
| Perception and combat | Remaining cheat-rule and combat solver behavior, concealment, ray behavior and guided projectile flight | world-wac-ai-re.md sections 16..18 |
| NPC presentation | Held weapons and remaining aim/fire/death presentation | world-wac-ai-re.md; D-WPN-32 |
| BMS actions | MP POI/live-marker ownership and remaining shared script/native effect-slot consumers | bms-event-runtime-re.md |
| WAC execution | Complete registered runtime semantics and presentation routing; replace silent effect fallthrough with explicit disposition | formats/wac command registry; runtime/wac VM |
| Mission lifecycle | Playthrough, objective feedback, success/failure, retry and transitions; fix adjacent blockers | MissionKernel, Session and mission-end tests |

Sound checkpoint (2026-09-09): all three trigger-set commands bind mounted SOUNDSET names and reach their correct direct or positional audio path. Retry clears old one-shot playback and replays initial sound queues; first-frame listener initialization and positional source identity are fixed. Full native CTest passes 438 tests with one existing motorcycle asset skip (npc-sound-full-ctest.log, 79.00 seconds). Full rebuilt-extension GUT passes 1,752 tests across 182 scripts, with 25 pending, 58,081 assertions and no collection errors (npc-gut-sound-range-full.log, 114.536 seconds). The initial impact-audio regression was reproduced in the isolated GameWorld file: its fixture authored a zero set range and had relied on the missing first-frame listener. The fixture now authors its intended range. Existing orphan/resource shutdown caveats and normal mission playthrough acceptance remain open.

Script-state checkpoint (2026-09-09): squad query/clear, mutable SquadSSN/SquadWho/RND, full-width random arithmetic and the witnessed dormant music return are implemented. The full native suite passes 439 tests with one existing motorcycle asset skip (npc-wac-state-full-ctest.log, 72.14 seconds). Full rebuilt-extension GUT passes 1,753 tests across 183 scripts, with 25 pending, 58,093 assertions and no collection errors (npc-gut-wac-state-full.log, 121.28 seconds). The loaded-mission regression verifies the post-startup random stream repeats after normal retry. The registry now has 155 explicit branches and 10 diagnostic fallbacks. Existing shutdown caveats and normal mission playthrough acceptance remain open.

Attention checkpoint (2026-09-09): staggered idle spotting, independent head/look chase, all mounted NPC seat gaze and body-oriented root motion are implemented. Full native CTest passes 440 tests with one existing motorcycle asset skip (npc-attention-final-full-ctest.log, 22.94 seconds). Full rebuilt-extension GUT passes 1,754 tests across 184 scripts, with 25 pending, 58,122 assertions and no collection errors (npc-gut-attention-final-full.log, 111.21 seconds). The passenger fixture failure was reproduced in the isolated Simulation file, then corrected to assert the authored seat body heading separately from live gaze. Existing shutdown caveats and normal mission playthrough acceptance remain open.

Player-command checkpoint (2026-09-09): piskills, pisvar, psetvar, AddExp, ppunt, pkillpunt and the witnessed clear pisgold result are implemented. Experience shares through both occupant links and punts reach the existing disconnect protocol. A loaded-mission regression exposed stale sender and Godot score-feedback caches on retry; both now reset with the local client. Full native CTest passes 441 tests with one existing motorcycle asset skip (npc-wac-players-final-full-ctest.log, 113.71 seconds). Full rebuilt-extension GUT passes 1,755 tests across 185 scripts, with 25 pending, 58,147 assertions and no collection errors (npc-gut-wac-players-full.log, 116.35 seconds). Normal mission playthrough acceptance and the existing shutdown caveats remain open.

Help checkpoint (2026-09-09): the command now generates help.wac and events.xml from the shared command registry, including the original XML IDs, category filters and independent file-open failures. Full native CTest passes 442 tests with one existing motorcycle asset skip (npc-wac-help-full-ctest.log, 124.90 seconds). Full rebuilt-extension GUT passes 1,755 tests across 185 scripts, with 25 pending, 58,147 assertions and no collection errors (npc-gut-wac-help-full.log, 115.60 seconds). The registry has 163 explicit branches and two diagnostic fallbacks, face and ssnface. Existing shutdown caveats and normal mission playthrough acceptance remain open.

Facial checkpoint (2026-09-09): both remaining WAC handlers, mounted GRM parsing/writing, native facial state, automatic expressions and display scheduling are implemented. JO's generated face targets have no material consumer; the port preserves that witnessed inactive endpoint. All 165 WAC entries now have explicit handlers. Full native CTest passes 444 tests with one existing motorcycle asset skip (npc-facial-full-ctest.log, 159.94 seconds). Full rebuilt-extension GUT passes 1,756 tests across 186 scripts, with 25 pending, 58,170 assertions and no collection errors (npc-gut-wac-faces-full.log, 112.422 seconds). The loaded-mission regression verifies both commands and two retries. No retail GRM assets exist in the mounted corpus; format validation uses authored data. Existing shutdown caveats and normal mission playthrough acceptance remain open.

Escort checkpoint (2026-09-09): the 12000/12001 approach offsets, backward drag gaits and state-139 corpse follow now use native head/hand anchors. Full native CTest passes 445 tests with one existing motorcycle asset skip (npc-escort-full-ctest.log, 119.39 seconds). Full rebuilt-extension GUT passes 1,756 tests across 186 scripts, with 25 pending, 58,170 assertions and no collection errors (npc-gut-escort-full.log, 113.677 seconds). At this checkpoint BMS action 39 still needed its operation owner and fully initialized dynamic spawns. Existing shutdown caveats and normal mission playthrough acceptance remain open.

Teammate and rotor-wash checkpoint (2026-09-09): BMS action 39 now owns the pickup/flyover state machine, dynamic helper construction, retry state and the active-operation query. Shared removal releases mounts, targets, collision, scars, facial slots and native effects. NPC and player motors consume the original rotor-wash animation query. Full native CTest passes 447 tests with one existing motorcycle asset skip (npc-teammate-wash-full-ctest.log, 179.30 seconds). Full rebuilt-extension GUT passes 1,757 tests across 187 scripts, with 25 pending, 58,235 assertions and no collection errors (npc-gut-teammate-wash-full.log, 153.604 seconds). Missing retail medic assets are explicitly unexercised; authored scene coverage is separate. General organic initialization, existing shutdown caveats and normal mission playthrough acceptance remain open.

Organic initialization checkpoint (2026-09-09): placed NPCs and scripted helpers now share the initial posture, facing, animation warmup, grounding and control-point respawn-link callback. PreMission events run after the NPCs' ADM, definition, collision and weapon bindings are ready; the existing early network bring-up order is retained. Full native CTest passes 448 tests with one existing motorcycle asset skip (npc-organic-init-full-ctest.log, 175.78 seconds). Full rebuilt-extension GUT passes 1,757 tests across 187 scripts, with 25 pending, 58,235 assertions and no collection errors (npc-gut-organic-init-final-full.log, 145.106 seconds). The isolated listen-server fixture now expects the witnessed level NPC pitch/roll. Four-family ammunition/launch-point routing, existing shutdown caveats and normal mission playthrough acceptance remain open.


Ammunition checkpoint (2026-09-09): the four organic ammo bytes and three launch points now bind from definitions and feed the shared NPC round/presentation entry. Animation events and the every-tick secondary latch preserve order, byte/word wrap and caller state on rejected shots. Person target/LOS points use the actual tick-phased eye offset. Full native CTest passes 449 tests with one existing motorcycle asset skip (npc-ammo-full-ctest.log, 155.03 seconds). Full rebuilt-extension GUT passes 1,757 tests across 187 scripts, with 25 pending, 58,235 assertions and no collection errors (npc-gut-ammo-full.log, 131.678 seconds). All ten CI lint gates pass. Normal playthrough acceptance, the complete combat solver, guided flight and the existing shutdown caveats remain open.

## Evidence rules

Record behavior, original address, test and observed result in the owning RE
record as each capability lands. Delete completed rows from the outstanding
table; git history preserves the work sequence. Keep mission playthrough
results separate from parser, compiler and focused behavior tests. A green
compilation corpus is not evidence of script execution or mission completion.

## Baseline

Implementation started 2026-09-08. The existing native build directory belongs
to this checkout, but its prior executables were built on 2026-09-01. Current
source configuration and the full native build succeeded. The baseline CTest
run passed 432 tests; motorcycle_gravity_06tr was skipped by its asset gate.
No mission playthrough has yet been accepted under these completion gates.

The current registry inventory finds 165 WAC entries: all 165 have explicit VM
branches and none reach fallback. This is a dispatch inventory, not a parity
claim; some explicit branches still have incomplete semantics. The first
implementation pass is recorded in world-wac-ai-re.md section 33.

The [routing inventory](npc-script-coverage.md) lists all 165 WAC entries, 150 named BMS trigger/action values, the 24 brain rows and motor/presentation acceptance families. Explicit dispatch remains distinct from verified consumer behavior.

After the respawn and distance-command changes, the full native rebuild and CTest run pass: 431 passed, one asset-gated skip, zero failures (npc-wac-distance-ctest.log, 125.32 seconds). The rebuilt Godot extension passes the isolated 21-test co-op file after correcting the pre-uplink player death regression. The subsequent full GUT run passes 1,743 tests across 178 scripts, with 25 pending/skipped tests and no parse errors or dropped scripts (npc-gut-distance-full.log, 141.572 seconds). The pending cases include unavailable headless rendering, fixture and existing deploy gates. GUT also reports one LAN-session orphan and 13 resources still in use at shutdown; this run does not establish leak-free teardown or normal mission playability.


Door follow-up: WAC DoorOpen/OpenDoors/CloseDoors and BMS actions 30/31 now drive a mission-owned door pool. The shared phase reaches NPC/player collision, sound and the rendered door part; restart restores the pool. The retail Iblock01 fixture passes both native collision-pose and Godot render-pose checks. Full native validation passes 432 tests with one existing asset skip; full GUT passes 1,745 with 25 pending/skipped and the same shutdown caveats (npc-door-full-ctest.log and npc-gut-doors-full.log). Door network synchronization and the separate shoot-to-open target callback remain tracked in world-wac-ai-re section 33.14.


Infantry/WAC follow-up: movement now consumes the collision-triggered detour state, and combat precedes the common animation selector on the 16-tick think cadence. A synthetic route walker reaches its waypoint around a solid wall through normal collision and root motion. WAC player/outcome caches and delayed BMS event queries are verified. All 432 runnable native tests pass after correcting the final alert-cadence fixture; one existing asset skip remains. Full rebuilt-extension GUT passes 1,745 tests with 25 pending/skipped (npc-gut-detour-full.log). This is still subsystem evidence, not a normal SP playthrough.


Actor/audio follow-up: all 435 runnable native tests pass, with one existing asset-gated skip (npc-actor-channel-full-ctest.log, 93.85 seconds). The rebuilt extension passes all four WAC voice tests, including synthetic AOA1 playback. The full GUT run passes 1,749 tests across 179 scripts, with 25 pending and no collection errors (npc-gut-actor-channels-recovery-fixed.log, 117.959 seconds). The repeated Windows fixture-publication failure was reproduced in a 100-iteration isolated loop: the first rename failed with a valid reservation and absent destination, while the immediate retry succeeded. A bounded retry revalidates paths and competing claims; the complete fixture contract passes with the repetition regression. The existing orphan/resource shutdown caveats remain. These are subsystem checks, not an accepted mission playthrough.


Projectile/color checkpoint: the full Release build succeeds. The complete native run covers 437 tests: 435 pass, one motorcycle asset gate skips, and one old lightning-color assertion fails because it masked the alpha byte. Correcting that assertion to the witnessed full-DWORD store makes its isolated rerun pass; all 436 runnable native tests are covered (npc-wac-projectile-full-ctest.log, 112.82 seconds). The rebuilt extension passes the scheduled WAC projectile test through a loaded mission, including the mounted ammo name and exact presentation origin/direction. Full GUT passes 1,750 tests across 180 scripts, with 25 pending, 58,050 assertions and no collection errors (npc-gut-wac-projectiles-full.log, 118.804 seconds). The existing orphan/resource shutdown caveats remain. These results predate the FOV follow-up and do not establish normal SP playthrough acceptance.


FOV/actor checkpoint (2026-09-09): the shared camera current/target, startup WAC weather boundary, optical visibility reads, ssnturn and tele are implemented. The complete native run passes 436 tests with the existing motorcycle asset gate skipped (npc-turn-tele-full-ctest.log, 66.15 seconds). Full rebuilt-extension GUT passes 1,751 tests across 181 scripts, with 25 pending, 58,054 assertions and no collection errors (npc-gut-turn-tele-full.log, 130.854 seconds). The two underwater scope regressions found during this work were reproduced in the isolated 92-test Simulation file and fixed. The same existing orphan/resource shutdown caveats remain. The remaining particle, sound, actor, BMS and motor capabilities and normal mission playthrough gates are still open.


Particle/source-fire checkpoint (2026-09-09): ammo2ssn, all four WAC particle handlers and the BMS marker particle consumer are implemented. Retry clears pending round records while preserving network sequence watermarks, and normal mission restart replays initial script particle descriptors after resetting presentation. The full native rebuild and CTest run pass 437 tests with one existing motorcycle asset skip (npc-fx-full-ctest.log, 95.06 seconds). Full rebuilt-extension GUT passes 1,751 tests across 181 scripts, with 25 pending, 58,058 assertions and no collection errors (npc-gut-fx-full.log, 112.949 seconds). The existing orphan/resource shutdown caveats remain. The script routing count is 150 explicit branches and 15 diagnostic fallbacks; D-PTL-24 keeps shared native/script entity effect-slot lifecycle work open. No normal SP playthrough has been accepted.
