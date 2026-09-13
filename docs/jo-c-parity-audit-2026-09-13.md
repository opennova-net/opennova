# JO-C parity audit and implementation plan

Date: 2026-09-13. OpenNova baseline: `aedf6c091cba0db41dd3eb54f00b17430e5307e5`
(includes PR #648). Branch: `audit/jo-c-parity-2026-09-13`.

The remaining work is primarily **completing state ownership and runtime consumers**:
guided weapons, shared ammunition buckets, numeric network identity, destructible
sections, parachutes, mission lifecycle, and presentation driven by those systems.
Many codecs, mathematical helpers, and local gameplay paths already exist. Repeating
those ports would leave the actual gaps untouched.

The baseline ledger contains **120 active IDs**: 106 OPEN, five NEEDS-RE, and nine
WITNESSED-READY-DEFERRED. This audit tables **seven additional IDs** for gaps previously
described only in prose or missing as a subsystem: four particle behaviors, specialized
texture loading, reverb, and mission saves. It also retires stale D-ITEM-22: the deferred
wire-wreck retry and its regression already landed in `9b24601587`. The resulting
inventory had **126 active IDs** at that baseline. The PR that carries this audit (#649)
then closed D-NET-169 and narrowed D-NET-167 and D-NET-64 (the ledger closure lines and the
net record own that evidence), so the package tables below hold 125; the JSON keeps the
baseline 126.
The [ledger](divergence-ledger.md) remains the status owner; this dated plan groups its
work by dependencies. [The audit inventory](jo-c-parity-audit-2026-09-13.json) records
every ID, its dated ledger description, work package, and evidence level.

## Baselines and confidence

The new worktree starts from committed OpenNova source. The primary checkout's modified
`godot/project.godot` and untracked `art/` and `local-data/` are outside this baseline.
The audit itself changed no gameplay implementation; the PR that carries it does, and the
[ledger](divergence-ledger.md) records those closures.

The reference is the sibling `jo-c` checkout. It started at `614072b` with local renderer
work; that work was committed during inspection as
`2122ee65b0fa182864a47becd0b8dc6391c3eb2c`. The raw `Jointops.exe.kong.c` record and the
pre-existing gameplay oracles did not change in that commit. The inventory pins the
reference files by hash so later reconstruction work does not silently change this
audit's evidence.

Evidence levels used below:

| Level | What it establishes |
|---|---|
| Source comparison | The current OpenNova implementation and the corresponding jo-c original-function record were inspected. A missing branch or different state transition is confirmed in source; live severity is not implied. |
| Existing witness | The current domain record identifies the original address and open behavior. Source and test ownership were surveyed, but this audit did not independently re-execute every original branch. |
| Research boundary | The missing surface is identifiable, but its complete semantics, reachability, or original dependencies need another witness before implementation. |
| Baseline test | A named existing test was compiled and run from this worktree. This proves only its assertions. |

This is a comprehensive subsystem and backlog inventory with focused source comparisons,
not exhaustive binary equivalence or a completed live playthrough matrix. No fresh IDA
grill, retail capture, or jo-c original/linked oracle run is claimed. Addressed original
records and jo-c's pinned instruction/layout manifests are the reference; jo-c's host
adapters and its passing tests are not universal specifications.

In particular, jo-c candidate 256 still uses a 155 m host collection radius, flat terrain,
and incomplete building visibility, shadows, water, and frame ordering
(`jo-c/docs/renderer-reconstruction.md`, remaining boundaries). Its save-state and
single-player records explicitly do not establish a complete live save/load. Use those
projects' recovered functions and bounded fixtures, rather than copying their whole
scene or lifecycle adapters.

## Findings that change the implementation plan

### 1. Guided missiles need a complete entity lifecycle and target-following path

**P1; source comparison; D-NET-64, with D-WPN-25 and D-NET-161 dependencies.**

[`client_replica_guided.cpp`](../engine/runtime/replication/client_replica_guided.cpp)
creates a separate missile row when a guidance packet arrives, seeds flight from the
shooter plus a fixed shoulder lift, uses velocity 300/default turn limits, and advances
against the last received XYZ. Its tick never resolves `lock_target` to a current target
pose. `RoundSim` separately records guided callbacks as unfinished.

The original `NetPacket_DispatchToEntityByNetId @ 0x4D6960` requires an existing active
entity and its class serializer. `Entity_UpdateGuidedMissile_0 @ 0x446060` refreshes a
locked target's fire origin before computing pursuit error. (The group-3/4/5 zero-value store of
`Entity_SerializeGuidedMissileState @ 0x447C50` is ported: the reducer keeps every decoded
coordinate, zero included.)

Fix the spawn/class/ammo/launch-transform owner first, then connect guidance to that
lifetime, refresh valid moving targets, keep straight flight on the no-lock branch, and
port the local-frame pursuit and the distinct Stinger/Hellfire/Javelin
motors. Add the authority seeker, countermeasure and wire producers, then model/trail
presentation. A codec-only change cannot close this ID.

Acceptance: moving target with no repeated guidance packet; guidance before spawn, after death, and after slot reuse; each missile family; loss of
lock and flare diversion; role-correct termination. Reuse `jo-c/tools/guided_missile_oracle.py`
and the existing `guided_missile_flight`, `nw_ingame_guided`, and
`npruntime_client_runtime` test seams, adding the missing integrated cases.

### 2. Ammunition pools exist; shared clip buckets and some owner paths do not

**P1; source comparison; D-WPN-2/-20, D-WPN-5/-6/-7/-8/-21/-23, D-HUD-5.**

[`WeaponInventory`](../engine/runtime/world/weapon_inventory.h) already has per-class
carried pools. Normal local installation reads them; reload refunds and redraws through
[`weapon_inventory_reload_slot`](../engine/runtime/world/weapon_inventory.cpp), and
the server reload path also conserves the represented pool. The old description
"single-pool ammo model" is too broad.

The remaining distinct storage is weapon.def `+0xDC`: original `sub_5405F0 @ 0x5405F0`
and `sub_540670 @ 0x540670` access shared clip buckets. `WeaponSlot_ReloadAmmo @ 0x541720`
and `WeaponSlot_GetTotalClips @ 0x5425F0` select that storage, whereas our eligibility,
fire, reload and total-clips paths still use each slot's clip. Inventory-less install
also retains full-magazine/`startrounds` defaults. Do not replace the existing ordinary
pool implementation; add the missing bucket/owner semantics and connect every consumer.

Acceptance: two carried weapons sharing one bucket, independent ammo classes, units per
round greater than one, partial reload, negative/unlimited sentinels, mounted borrower
switch/restore, a host-local player and a remote player. Check clip count, reserve, weight,
HUD flash and emitted loadout bytes together. Reuse `weapon_inventory`, `weapon_fsm`,
`npruntime_reload_relay`, and jo-c's reload/loadout/weapon-pump oracles.

### 3. Successful handshake and codec coverage still leave admission and dispatch gaps

**P1; existing witnesses with source checks; D-NET-167/-171/-218.**

The join path sends the ordinary server password, the spectator fields and the
side-password `JSP`/`TR` fields (host DPC 18/19/20 validation, ported 2026-09-13), but
not the squad challenge (host DPC 21) recorded at `NapiNPServerMsg` admission
`@ 0x512100`; `FID` is numeric, not a password field. The [193-entry dispatch audit](net/retail-message-dispatch-audit.md)
also distinguishes decoded messages from executed commands/replies: teleport,
batch-kill acknowledgement, text-command tails, form/metrics, squad/admin/profile and
integrity paths remain.

Implement the witnessed squad challenge input and validation, then close the
dispatch table by real consumers and reply/state assertions. Keep game version,
expansion and security prerequisites explicit; a shaped reply without its state change
is incomplete. Do not contact live hosted services to exercise LAN/gameplay work.

Acceptance: correct/incorrect squad passwords; denied and admitted classes;
each open message exercised through host/joiner dispatch, asserting reply bytes, state
and ordering. Inventory the challenge prerequisites before implementing their replies.

### 4. Destruction must drive collision, visibility and replicated section state together

**P1; source checks and existing witnesses; D-COL-2, D-ITEM-3/-8, D-DOOR-1/-3,
D-NET-147, D-OCC-9.**

[`destruction.cpp`](../engine/runtime/world/destruction.cpp) still leaves blast-time
collision-section marking unimplemented. Local door phases exist, but the C2S 0x1A,
S2C 0x37 and late-join/static-state chain is unfinished. The original section-damage
and global phase-bank consumers span collision, door animation, static serialization
and occlusion. Correctly hiding a section is not sufficient to make a destroyed wall
traversable or consistent for a late joiner.

Port one section-state owner and the class-specific tower/crane transitions, route it
through collision/raycast and visibility, then connect incremental and initial wire
state. Preserve the door command's zero-based versus completion packet's one-based
section quirk. Treat exhausted/corrupt bank accesses as an explicit disposition problem,
not an instruction to reproduce out-of-bounds reads.

Acceptance: shoot and blast the same section, cross its former collision plane, view
both sides of an interior, operate several doors, then join late. Compare state, collision
and wire on every peer. jo-c's `tower_sections_oracle.py`, `door_event_oracle.py`,
`door_spawn_oracle.py` and `mission_bone_mask_oracle.py` provide useful bounded fixtures.

### 5. Indoor projectiles and specialized impact/destruction tails remain

**P1; existing witnesses; D-COL-11, D-ITEM-1/-4/-6/-9/-10/-11/-13/-21,
D-WPN-25/-28, D-AI-8/-12.**

`LiveRound` has no projectile blink/indoors lifetime. The original
`Projectile_UpdatePhysics @ 0x4E9D70` updates it and skips the terrain clamp indoors;
our shared flight/collision seam cannot express that distinction. Other remaining
facets include tumble/random-state fidelity, damage/force and special impact consumers,
object-supported wreck settling, fragment mesh/spin/glow, authored impact sounds,
heat emitters. The normal ballistic pre-force sweep and armor/occupant damage fixes
are already present. D-ITEM-22 is also already fixed: the deferred wire-body branch
skips caching a temporary miss, and the existing
`test_retained_husk_pick_retries_and_updates_only_the_husk_section_mask` GUT case
covers later materialization and repeated snapshots. Retain that regression; no
new retry implementation is needed. The test was inspected, not re-run in this audit.

Acceptance: an interior below the terrain heightfield, wall penetration by material,
mounted exclusions, special ammunition, wrecks landing on another object, a cold-spawn
joiner seeing destruction, and a high-rate impact/tracer scene. Compare effects and
damage independently. Resolve unknown exclusion provenance before porting it.

### 6. Parachute, mounted lean and body/camera tails are still visible gameplay gaps

**P1; source checks and existing witnesses; D-INF-3/-11/-13/-17/-18/-20,
D-COL-4/-8/-10, D-NET-196.**

[`infantry.cpp`](../engine/runtime/world/infantry.cpp) explicitly omits the parachute
steering/descent arm and does not supply the normal motor's auto-deploy producer.
Script-set flags are not the full system. Remaining body work also includes mounted
lean/gates, remote eye and body-conform channels, live-lean collision eye position,
rig bind composition, and PANM billboard evaluation's actual inverse-view input.

Port chute state, deploy conditions, descent, steering, animation and landing cleanup
as one vertical slice. Reconcile replicated/local body channels and collision-eye
state at their owning tick. Validate the bind-matrix question on a rig corpus before
altering every character pose.

Acceptance: auto/manual/scripted deployment, shallow fall without deployment,
aircraft exit, water landing, seated lean, a remote avatar on a sloping carrier,
and camera-facing model parts under a moving camera. Existing infantry/view/animation
tests need role and asset coverage, not just more standalone angle tests.

### 7. NPC combat completion still needs behavior and real mission gates

**P1; existing witnesses; D-AI-1/-2/-4/-6/-7/-9, D-INF-2.**

Acquisition, class walks, idle attention and substantial combat/reload are implemented.
Remaining work includes retreat/cover/movement, aim overrides, lead and near-target
origins, muzzle/userpoint and suspended-mount frames, foliage accuracy, death/collision
force and reset ownership. Consult the exact residuals in
[the world record](world/world-wac-ai-re.md), rather than treating every earlier AI
placeholder as current.

Acceptance: authored patrol/acquire/fire/reload/refire; moving target and elevation;
mounted and prone-in-foliage fire; death, revive and respawn; multiple difficulty and
class profiles. Continue beyond the accepted 00TRa mission with CP11 combat and missions
that require these branches. jo-c's CP11 witness modifies a small spawn fixture and
does not establish every mission's authored walkthrough.

### 8. Vehicle work should target the remaining branches, not repeat the motor port

**P1; existing witnesses and spawn-source check; D-NET-161/-196, D-SND-17.**

The remaining vehicle list is concrete: pool-3 deck-marker localization, flare C2S
off28 target identity, north/south/west versus east/center ground-ray kinds,
non-state-machine class callback dispatch, run-over player gates, prediction/body
conform, and the sound-ready/tank-fold sound arguments. The bike launch vector,
selector-zero boat spin block and brain think countdown landed already.

[`vehicle_spawn_markers.cpp`](../engine/runtime/world/vehicle_spawn_markers.cpp)
does not establish retail's mission-start deck-local marker transform
(`Game_StartMission @ 0x524360`, sites `0x525E6D..0x525F58`). Connect support identity and local pose
before testing moving-carrier respawn.

Acceptance: motorcycle, tank, ordinary car, boat, amphibious vehicle, helicopter and
fixed-wing coverage; controller and passenger roles; moving support under a spawn;
flare target; mixed terrain/model contact. Keep jo-c's recorded aircraft contact
instability as an unresolved reference boundary, not a target behavior to copy.

### 9. Script and mission lifecycle work needs its missing consumers

**P1; source checks and existing witnesses; D-WAC-4/-5/-6, D-EVT-1/-3,
D-AI-10, D-LOADSCR-8, D-PTL-26.**

`night` writes, effect intern ordering, malformed-script continuation, remaining
trigger/POI consumers and the shared entity effect-group lifetime need separate closure.
Do not replace the implemented command dispatch with another interpreter. The network
script sound operand has a host-address provenance question; collect a real witness
before promising cross-process interoperability.

The SP epilog still lacks full cinematic/count-up/music behavior and the retail restart/
redeploy sequence. `mission_end_screen.gd` substitutes a composed screen and a zero
objective bonus. Integrate restart through the existing session/kernel teardown and
startup owners, including script clocks, input, audio and presentation reset.

Acceptance: win, lose, death/redeploy, in-game RESTART, quit and re-entry; verify event
ordering and no retained state after each transition. Preserve 00TRa's accepted ten
gates, and add a mission where each new script/trigger branch matters.

### 10. Mission save/load is an additional missing subsystem

**P2, research first; new D-SAVE-1.**

Profile/options/format-file saving is present; mission runtime snapshots and retail
save-slot lifecycle are not. jo-c now has original records for header/block I/O,
entity/vehicle-AI restoration, pool references, timers and music-VM snapshots, plus
single-player save-slot handlers. The [savegame record](mission/savegame-re.md)
defines the recovered entry points and the unsupported boundaries.

Implement header/block codecs and typed state ownership first, then restoration in
dependency order, then the game UI. Reconstruct the complete retail schema; do not
serialize C++ memory or copy pointer-bearing retail blobs. Gate completion on a fresh
process loading a retail save and the reverse retail-load direction if interoperability
is claimed. jo-c's bounded save oracles alone cannot pass that gate.

### 11. HUD, menus, input and profiles need end-to-end closure

**P2, with identity/deploy prerequisites at P1; existing witnesses.**

The remaining UI includes deployment-map rendering; waypoint naming/localized distance,
POI restrictions and MP transport; minimap labels/indicators/pan modes; scoreboard
headers/camo and message-feed tails; friendly-tag class/eye/health/squad/voice inputs;
mortar terrain-ring scopes; authored prompts; class kit memory and five profile slots;
and the options commit/cancel/device behavior. The full IDs are assigned to packages
below and in the inventory.

A specific stale claim: `scope_min_mag` is parsed, and
[`Simulation::request_local_player_weapon_cycle`](../godot/src/simulation/simulation_player_weapon.cpp)
now routes next/previous actions through scope zoom. Keep this work. Verify the separate
action-215 route, mount-time application, host-rule producer, scope notify and the
remaining stance/NVG/zero/rangefinder legs before closing D-WPN-9.

Acceptance uses the real authored MNU controls and input route: enter/change/cancel/
accept/reopen options; remap mouse/joystick; change class/team/profile; deploy using map
and list; scope and zero an appropriate weapon; compare localized HUD output with fixed
camera, resolution and gameplay state. Helper tests do not prove a button is wired.

### 12. Specialized material loaders were absent from the open ledger

**P2; source comparison; new D-RMAT-12.**

[`material_texture_transform`](../engine/runtime/renderer/material_texture.cpp)
raw-loads types 6, 7, 16, 17 and 18. jo-c's pinned `sub_5B16F0 @ 0x5B16F0`
dispatches to distinct environment-map, alpha-overlay and normal-map loaders. The
type-4/5 normal filter and checkerboard work already landed; do not redo them.

First inventory actual material rows in the retail model corpus, then port each used
loader's decode/transform/cache identity and the remaining supported-format cases.
Compare dimensions, channels, alpha, orientation, wrapping and failure behavior, followed
by a rendered material fixture. The missing-MDT uninitialized-texture fallback is a
separate proposed class-D disposition; this plan does not authorize reproducing garbage
or declare it permanently accepted.

### 13. Particle fidelity includes four untabled behavior gaps

**P2; source comparisons/existing witnesses; new D-PTL-27/-28/-29/-30.**

| ID | Current difference | Implementation and acceptance |
|---|---|---|
| D-PTL-27 | `child_id` is parsed and exposed, but the runtime has no per-particle child scheduler or parent inheritance/death chain. `CParticleEmitter_SpawnParticle @ 0x5E7640` has that parent/child path. | Implement child lifetime, scheduled/death emission and authored inheritance through the existing effect scene. Test a parent dying, moving and being destroyed with scheduled children. Effect-level `pdefs` composition is already implemented. |
| D-PTL-28 | `Emitter::orbit_speed` is randomized once per emitter, and integration rotates by that shared rate. `CParticleEmitter_SpawnNewParticle @ 0x5F35B0` randomizes the auxiliary rate per particle; `CParticleEmitter_BuildOrientationMatrix @ 0x5F3970`/`CParticleEmitter_UpdateAllParticles @ 0x5F3BE0` supply the basis/age path. | Model the per-particle channel and port the complete basis/age update. Compare two particles from one emitter with nonzero adjustment and a moving, non-axis-aligned emitter. Preserve the existing accepted platform-RNG disposition. |
| D-PTL-29 | Burst particles are created on update boundaries with no `SpawnParticle` `timeOffset` argument. | Port emission scheduling and initial age/phase/position offsets. Compare one large update with the witnessed sequence of emission times, and irregular display cadence; do not impose an invented timestep-invariance rule. |
| D-PTL-30 | `elastic` and `collide_sounds` survive parsing/bindings but have no runtime collision consumer. | Witness collision masks, restitution, sound selection and termination, then add collision/query and audio outputs to the existing emitter path. Test floor/wall/water contacts and zero/positive elasticity. |

The particle record's older unexplored-flag list is not a reliable implementation
inventory: wind/force paths now exist. Audit remaining flags by producer and consumer.
The EMITVECTOR helper distinction needs a semantic witness without reopening D-PTL-9's
accepted RNG/FPU differences by assumption. Persistent emitter bounds are also a
research follow-up; do not call current snapshot bounds byte-equivalent to retail.

### 14. Renderer visibility and terrain-page composition still have coupled gaps

**P2; existing witnesses; D-TERRAIN-7, D-FOLIAGE-7/-9/-10, D-RORD-7,
D-OCC-9/-12/-13/-14/-15, D-3DI-2.**

Complete the remaining terrain-page producers once and share their result with foliage.
Resolve foliage silhouette membership and immediate depth-mask insertion order,
far-side alpha versus particles/water ordering, window-glow consumption, static blink
refresh and per-instance/avatar section gates. CTRL/RNG/submit-order residuals require
a process/frame witness. Existing gamma-domain blending, high-quality shader techniques,
reflection admission and lighting ports are retained.

Acceptance: synchronized scenes with translucent objects crossing water/particle depth,
an interior containing static and pooled geometry, visible window glows, stance-dependent
foliage silhouettes and a terrain page containing every supported contributor. Assert
semantic admission/order and calibrated pixels; report unsupported contributors instead
of hiding them behind a plausible image. Preserve ADR 0042's single Godot backend.

### 15. Reverb is a confirmed approximation with an unresolved DSP boundary

**P2; source comparison and existing witness; new D-SND-18; D-SND-5/-8/-17.**

[`MissionAudio::_apply_reverb`](../godot/src/audio/mission_audio.cpp) maps the mission
ID to `0.4 + 0.08 * id` room size and fixed wet/dry levels. The witnessed stock retail
coefficient rows do not vary that way. Retail also selects the ID per player tick from
userpoint, building, then mission, whereas our calls are mission setup/clear.

Port region selection and table parsing/ownership, witness the MMX DSP including the
index-zero behavior, then implement the corresponding device processing. Do not tune
unrelated Godot presets by ear and label them parity. Add doppler/listener velocity,
talking-portrait and vehicle sound residuals through the existing audio owners.

Acceptance: move between mission/building/userpoint regions, compare a known signal and
impulse response, test index 0, fades, underwater transitions and moving sources. jo-c's
positional-audio oracle leaves playback as fixture services; its pass is not an audible
reverb witness.

### 16. Keep the remaining formats and low-reachability work explicit

**P3; existing witnesses/research; D-MIS-2/-3, D-CBIN-1, D-MNU-5/-6/-12/-13,
D-LOADSCR-2/-5, D-WPN-1, D-ITEM-2.**

Resolve credits markup/font/image consumers and the remaining authored menu drawing
branches; determine corpus reachability for deferred callbacks and progressive husk
fields. The full `.mis` grammar belongs to `dfx2med.exe`, not the JO game binary:
jo-c cannot establish that editor's semantics. Use the appropriate image and textual
corpus. Preserve already-closed VFS, PFF, fonts, RTXT, tile, music and common format work
unless a new differential case actually contradicts it.

## Implementation sequence

P1 means a normal gameplay/interop failure or a prerequisite for one. P2 means remaining
feature/visual/audio fidelity. P3 means low-reachability or another-image research.
These are proposed priorities, not delivery-date estimates. Each package should land
as bounded reviewed slices; a package is not necessarily one PR.

| Package | Priority | Work and owner | Dependencies | Completion gate |
|---|---|---|---|---|
| W00 | first | Pin corpus/oracle manifests; reconcile stale ledger prose and live callers; inventory untested material/effect/weapon branches. | none | Every active ID assigned; each implementation slice names original inputs, outputs and a failing case. |
| W01 | P1 | Squad-password admission and remaining request/reply consumers in `inmatch`/`replication`/`npwire`. | W00 | Relevant dispatch tests plus mixed retail/OpenNova LAN. |
| W02 | P1 | Full entity repair/initial state, ownership flags, net IDs, phase/priority/resend residuals and remote weapon state. | W01 for identity-dependent legs | Late join, loss/reorder, slot reuse and repair preserve entity/state/bytes across roles. |
| W03 | P1 | Shared clip buckets, weapon owner/pump/fire gates, mount/switch/seed and HUD ammo consumers. | W00; W01/W02 for remote owner legs | Same scenario has matching ammo, action cadence, weight and wire for local/host/joiner. |
| W04 | P1 | Guided families, indoor rounds, damage/impact/wreck and heat/tracer tails. | W02/W03; W05 for section geometry | Actual fire-to-flight-to-hit/death lifecycle, including moving target and countermeasures. |
| W05 | P1 | Shared section/door/destructible state and its collision/visibility/wire consumers. | W00; W02 for synchronization | Walk/shoot/view through the changed section, including a late joiner. |
| W06 | P1 | Parachute, infantry/AI aim/movement/death, body channels and camera/collision composition. | W03/W04/W05 as required by each branch | Real-input infantry and NPC encounter/respawn scenarios on authored missions. |
| W07 | P1 | Vehicle branch residuals, moving spawn supports, mounted prediction and sound. | W02/W03/W05 | Vehicle-family/seat/contact matrix in both pilot/driver roles. |
| W08 | P1/P2 | Script consumer tails, SP restart/epilog and mission save/load. | W02/W03/W05/W06 for state restoration | Restart resets the live kernel; saves restore into a new process; authored mission acceptance retained. |
| W09 | P2 | HUD/map/deploy, scope input, menus/options/profile/controls. | W01/W02/W03/W08 for corresponding feeds | Real authored control-to-engine-to-presentation scenario, with retail comparison. |
| W10 | P2 | Texture loaders, terrain/foliage composition, visibility/order/CTRL and particle simulation. | W05 for section/visibility; W08 for script effects | Semantic and rendered fixtures across interior/water/foliage/effect interactions. |
| W11 | P2 | Reverb, doppler, portraits and remaining sound consumers. | W06/W07 for region/velocity/vehicle feeds | Parameter/output-signal checks followed by bounded live listening/capture. |
| W12 | P3 | Remaining format/credits/rare authored branches and disposition research. | W00; proper original image/data | Corpus-backed behavior or an explicitly ratified permanent disposition. |
| W13 | release gate | Cross-system mission and interoperability acceptance. | affected packages | The acceptance matrix below, with skipped and unknown legs reported. |

Start with W00, then W01/W03 and the section-state foundation of W05. Follow with W02,
W04, W06 and W07 as their dependencies are ready. UI, audio and rendering slices can
proceed independently where their required engine state already exists. Save restoration
should follow the state-owner corrections, rather than freezing today's incomplete state.

## Validation and acceptance

For every fix: capture a red case at the production public seam; compare the original
function or a hash-bound jo-c oracle under stated services; port behavior and connect its
caller; make the case green; run the owning native suite and affected GUT files; update
the owning record and ledger in the same change. Codec roundtrips, mirror implementations
and private-field-only tests are not sufficient closure evidence.

| Axis | Required coverage |
|---|---|
| Roles/topologies | SP; OpenNova host/OpenNova joiner; OpenNova host/retail joiner; retail host/OpenNova joiner; retail/retail control. Use the existing LAN runbook and required onHook tooling. |
| Population/lifecycle | At least three peers; duplicate names; denied/admitted join; late join; player and entity slot reuse; voluntary leave/rejoin; respawn; mission restart and rotation. |
| Network delivery | Normal traffic, duplicate, missing, reordered and fragmented packets; mixed reliable/one-send records; reply/state ordering. Keep live probes serial. |
| Gameplay | Infantry/NPC, all represented weapon and vehicle families, every competitive rules branch, mounted roles, damage/destruction, indoor/outdoor, slope/water/foliage. |
| Missions/data | Retain accepted 00TRa; exercise 00TRg, CP11, minefield and suitable multiplayer/vehicle missions. Record base-game versus expansion assets and asset hashes. Missing assets are reported, never replaced by invented substitutes. |
| Presentation | Fixed resolution, settings, pose, simulation phase, weather/time and effect seed where controllable; inspect state/admission before pixel comparisons. |
| Persistence | Mission transitions within a process and save/load into a fresh process; distinguish profile files from mission snapshots. |

Read [asset-gated-tests.md](asset-gated-tests.md) before interpreting a green suite.
`OPENNOVA_JO_DIR` and `OPENNOVA_JO_ASSETS` are required for the applicable corpus legs.
Use `scripts/build.sh`, the owning `ctest` targets, and isolated GUT files as documented
in the repository; run full required CI checks for implementation slices. No new network
path, renderer backend, raw-byte writer bypass, or duplicated gameplay owner is proposed.

Audit validation actually performed:

- Existing `guided_missile_flight_test.cpp` was compiled directly with its production
  integrator using `g++ -std=c++20 -O0 -static -Iengine`; it reported
  `guided_missile_flight_test: all passed`.
- Existing `material_texture_test.cpp` was compiled the same way with production
  `material_texture.cpp`; it exited 0. This covers its existing filter/fallback cases,
  not the five missing specialized loaders.
- The ledger scoreboard, complete ID-to-package mapping, cited local paths and
  whitespace/diff checks are verified for the documentation change.

Those two native tests passing is consistent with the findings: neither exercises the
missing runtime lifecycle or loader branches. Full CTest, GUT, rendered comparisons,
retail LAN and jo-c oracle executions are future implementation/acceptance gates, not
results of this audit.

## Inventory coverage

The accompanying JSON is a dated snapshot at the master baseline, not a second status
ledger. It includes the 126 IDs active there (D-NET-169 included), their ledger descriptions
as of that baseline, proposed package/priority and evidence classification. Newly tabled rows identify their owning records. Existing
closed or permanently accepted differences are excluded from the repair count.

<!-- audit-coverage:begin -->
| Package | Active IDs | Count |
|---|---|---|
| W00 | Cross-cutting evidence / acceptance gate | 0 |
| W01 | `D-NET-136`, `D-NET-137`, `D-NET-167`, `D-NET-171`, `D-NET-179`, `D-NET-218` | 6 |
| W02 | `D-NET-97`, `D-NET-116`, `D-NET-127`, `D-NET-133`, `D-NET-139`, `D-NET-164`, `D-NET-189` | 7 |
| W03 | `D-NET-174`, `D-WPN-2`, `D-WPN-5`, `D-WPN-6`, `D-WPN-7`, `D-WPN-8`, `D-WPN-20`, `D-WPN-21`, `D-WPN-23`, `D-HUD-5` | 10 |
| W04 | `D-NET-64`, `D-COL-11`, `D-ITEM-1`, `D-ITEM-4`, `D-ITEM-6`, `D-ITEM-9`, `D-ITEM-10`, `D-ITEM-11`, `D-ITEM-13`, `D-ITEM-21`, `D-WPN-25`, `D-WPN-28`, `D-AI-8`, `D-AI-12` | 14 |
| W05 | `D-DOOR-1`, `D-DOOR-3`, `D-ITEM-3`, `D-ITEM-8`, `D-COL-2`, `D-OCC-9`, `D-NET-147` | 7 |
| W06 | `D-INF-2`, `D-INF-3`, `D-INF-11`, `D-INF-13`, `D-INF-17`, `D-INF-18`, `D-INF-20`, `D-AI-1`, `D-AI-2`, `D-AI-4`, `D-AI-6`, `D-AI-7`, `D-AI-9`, `D-COL-4`, `D-COL-8`, `D-COL-10`, `D-WPN-32`, `D-WPN-35` | 18 |
| W07 | `D-NET-161`, `D-NET-196`, `D-SND-17` | 3 |
| W08 | `D-WAC-4`, `D-WAC-5`, `D-WAC-6`, `D-EVT-1`, `D-EVT-3`, `D-AI-10`, `D-LOADSCR-8`, `D-PTL-26`, `D-SAVE-1` | 9 |
| W09 | `D-WPN-9`, `D-MNU-9`, `D-MNU-19`, `D-MNU-20`, `D-MNU-21`, `D-CTRL-1`, `D-CTRL-3`, `D-PLAYERINFO-9`, `D-PLAYERINFO-12`, `D-HUD-6`, `D-HUD-23`, `D-HUD-24`, `D-HUD-8`, `D-HUD-14`, `D-HUD-16`, `D-HUD-17`, `D-HUD-18`, `D-HUD-19`, `D-HUD-20`, `D-HUD-21`, `D-HUD-22`, `D-HUD-26` | 22 |
| W10 | `D-3DI-2`, `D-RMAT-12`, `D-PTL-27`, `D-PTL-28`, `D-PTL-29`, `D-PTL-30`, `D-TERRAIN-7`, `D-FOLIAGE-7`, `D-FOLIAGE-9`, `D-FOLIAGE-10`, `D-RORD-7`, `D-OCC-12`, `D-OCC-13`, `D-OCC-14`, `D-OCC-15` | 15 |
| W11 | `D-SND-5`, `D-SND-8`, `D-SND-18` | 3 |
| W12 | `D-MNU-5`, `D-MNU-6`, `D-MNU-12`, `D-MNU-13`, `D-LOADSCR-2`, `D-LOADSCR-5`, `D-MIS-2`, `D-MIS-3`, `D-CBIN-1`, `D-WPN-1`, `D-ITEM-2` | 11 |
| W13 | Cross-cutting evidence / acceptance gate | 0 |
| **Total** | **Every baseline-active ID assigned once; D-NET-169 closed by the carrying PR and removed** | **125** |

Statuses: **109 OPEN**, **7 NEEDS-RE**, **9 WITNESSED-READY-DEFERRED** (the ledger scoreboard, not this line, is current).
OPEN rows with an additional NEEDS-RE facet retain that facet in the ledger and inventory.

The audit restores D-ITEM-22's missing domain record and retires its stale open row.
The retry and production-presenter regression already landed in `9b24601587`; this
is a historical closure, not a new gameplay fix or a claim of a fresh GUT run.
<!-- audit-coverage:end -->
