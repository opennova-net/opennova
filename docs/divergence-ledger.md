# Divergence burn-down ledger

Every tracked divergence between OpenNova and the original engine, in one place,
under one vocabulary, with a target of **zero OPEN entries**. The policy that
ratifies it is [ADR 0022](adr/0022-divergence-burn-down.md); the maturity program
that stood it up ([maturity-program.md](maturity-program.md)) CLOSED 2026-07-12,
and since the close this ledger itself is the plan — work runs as retail-fidelity
slices off these rows ([current-state.md](current-state.md) routes each domain).

**2026-08-13 HUD adjudication:** the `hud_color_index` palette port stands. A
catalog walk of the 108-byte action records at `0x8159A8` found the real
producer — row 76 `hudcolor`, dispatch code 10 (the cycle `@0x49afc7`),
default F6 — and refuted BOTH earlier key stories: #491's KEY_H stand-in and
the interim "retail H shows/hides the HUD" claim (H is only `pause`'s
secondary; the sole visibility control is the boot `/NOHUD` switch). Retail's
stock F6 is shadowed by the `huddetail` row — the HUD declutter cycle, a real
live arm of the in-game handler and PORTED 2026-08-15 — so the `hudcolor` cycle
ships dormant on the stock keymap; OpenNova keeps the row reachable by
rebinding — D-CTRL-4.

## Purpose

The documentation index already states the rule: *a divergence is a tracked decision,
never an accident* ([docs/README.md](README.md)). The burn-down adds the second half:
**and every tracked divergence is either on a path to closure or ratified permanent.**

Three maintainer decisions (2026-07-05) stand behind this ledger:

1. **Target = zero OPEN.** Every tracked divergence is driven to one of two terminal
   states — *ported-and-closed* (`FIXED`), or *ratified deliberate* (`PERMANENT`,
   in [ADR 0022](adr/0022-divergence-burn-down.md)'s register or a domain ADR). Nothing
   is allowed to sit "known-broken" untracked.
2. **Started 2026-07-05, in parallel with Wave 1** (historical: the maturity-program
   freeze would otherwise have blocked new reimplementation work, so the maintainer
   granted a **per-slice freeze exemption for PAR slices**; the freeze itself LIFTED
   2026-07-12 at program close — the burn-down pays down existing debt rather than
   adding surface, and now runs unexempted). Env-domain closures
   are implemented **engine-env-first** so the ENG-2 port (env GDScript →
   `engine/formats/env` + `engine/runtime/environment`) does
   not pay for the same math twice.
3. **The seven systems with no RE record get research audits.** Terrain, foliage, tiles,
   fonts, credits, the importer pipeline, and the VFS/PFF mount stack started `UNAUDITED`:
   their divergences, if any, were untracked. Audit slices (PAR-R1..R7) turn unknown
   unknowns into tracked rows — **all seven landed this cycle**: VFS/PFF (R7),
   Fonts (R4), Foliage (R2), Tiles (R3) full; Terrain (R1) + Credits (R5) partial;
   Importer (R6) tracked-by-composition. `UNAUDITED` reached **0** on 2026-07-05;
   the REN planning grill reopened the set the same day with the **three
   runtime-render systems** (materials/state, draw order, lighting — the audit
   track below), audited by REN-2/3/5
   ([ADR 0023](adr/0023-render-visual-parity.md)).

## Canonical disposition vocabulary (normative)

One vocabulary, merging the three dialects the records grew independently — env's
Disposition column, the D-NET `[SEVERITY, STATUS]` tags, and the prose
"accepted/intentional" notes. Every ledger row and every RE-record catalog entry uses
these terms:

| Disposition | Meaning | Counts as open? |
|---|---|---|
| `OPEN` | Confirmed divergence; the fix is understood but not yet applied. | **yes** |
| `NEEDS-RE` | Not fully witnessed — research the original before porting. | **yes** |
| `WITNESSED-READY-DEFERRED` | The port is specified from a witness, deferred for value/risk (or waiting on an upstream system). Still a divergence today. | **yes** |
| `FIXED` | Behavior now matches; the closing commit/PR carries the witness citation. | no |
| `PERMANENT` | A ratified, deliberate divergence — must cite [ADR 0022](adr/0022-divergence-burn-down.md)'s register or a domain ADR. | no |
| `UNAUDITED` | The system has no RE record; divergences (if any) are untracked. | counted separately |

**The faithful-vs-open axis** (from [env/env-honored-matrix.md](env/env-honored-matrix.md)).
A field or behavior that is *unconsumed in retail too* is legitimately closed — the
original does not use it either, so reproducing nothing is faithful (mark it faithful,
not open). A behavior that is *awaiting a ported consumer* — the original DOES use it and
we do not yet — is `OPEN` and **must not be faked**: inventing an effect the port has not
earned would mis-train authors and violate the parity rule ([GOALS.md](../GOALS.md),
[ADR 0003](adr/0003-no-raw-passthrough-create-from-scratch.md)).

---

## Per-domain OPEN tables

Rows are drawn from each source record's own catalog; the disposition is re-expressed in
the canonical vocabulary above. `Class` is the burn-down triage: **A** = closeable by
porting a witnessed behavior; **B** = needs more RE first; **C** = platform/reimpl-structural
(candidate `PERMANENT`); **D** = original-bug/garbage class (candidate `PERMANENT`). Where a
record splits a divergence into facets (e.g. D-NET-133), the facets get separate rows.

### Net — [net/novaworld-net-re.md](net/novaworld-net-re.md) (D-NET catalog)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-NET-64 | Guided-weapon record ADVANCED 2026-08-18: the S2C 0x44 dispatch is wired (`ClientReplicaPipeline::apply_entity_routed` folds §5.36 sub-header + §5.15 groups 1/2/3/4/5 into typed `ClientGuidedMissile` state — 5 clears the lock like 2, the witnessed read semantic; 6 dropped, no presented surface) and the flight integrator is hosted (`world::GuidedFlight` `[orig: Entity_UpdateGuidedMissile_0 @0x446060]`: 31-tick ignition hold, integer-truncated boost ramp, per-axis BAM turn clamp; the overshoot/steer-guard detonation + proximity AI-notify leg is AUTHORITY-only `[orig: gate @0x4463cb]` — the non-authority client flies until the wire's group 1 ends it), ticked per client pump. Wire validation was capture-proven against the local Karo reference capture before the capture root retired (ca1cef465, 2026-08-29; the pinned slice: 649 records, 0 undecodable, 12 missiles, 3 shooters — the fork measured a different slice of the same match: 709 records, 100% stng, 4 shooters); the live CI gates are `nw_ingame_guided` (the §5.15 per-(mode, field-group) codec round-trip), `guided_missile_flight` (the integrator), and `npruntime_client_runtime`'s guided lane (the 0x44 fold + the joiner APPLIED-stream order). JOINER ROUTING landed 2026-08-19 (the post-merge review): `joiner_connection` forwards 0x44 into the reducer stream — before that a joiner on a retail server dropped the whole lane before the fold (the loopback-masked gap's fourth strike, after 0x59/0x12, 0x16/0x46 and 0x1E), pinned in the APPLIED-stream order test. Residuals: flight velocity/turn clamps use the integrator defaults until the missile's ammo identity resolves through its entity class (`turnrate_maxpit/maxyaw` parsed + carried already); missile presentation (model + trail) unhosted; the authority seeker branch (target acquisition, flare preference, the 0x44 write side) unported; C2S command-map 0x44 overload shape-guard only | B | OPEN (partial — dispatch + flight + capture validation ported; presentation/ammo-resolve/authority-seeker residuals remain) | PAR-NET |
| D-NET-97 | Host pool routing still follows BMS `EntityKind`; retail's `Pool_Alloc` caller and exact item-definition allocation predicate remain unwitnessed. `ItemReplicationCatalog` now keeps raw type/attrib/attrib2/capability inputs on an independent `StoragePool::Unresolved` axis, so wire codec, motion family, and BMS kind can no longer silently masquerade as allocation evidence. The pool-1 trailer crash remains fixed; allocation parity remains open. | A | OPEN + NEEDS-RE | PAR-NET |
| D-NET-116 | Pending-spawn load-complete gate (`dword_24D1DE0`) not modeled; latent for a driver that wires `ctx.world` during load | A | WITNESSED-READY-DEFERRED (latent) | PAR-NET |
| D-NET-123 | `Server_TickUpdate` owns the logic tick; the double-tick guardrail is comment-only | A | WITNESSED-READY-DEFERRED (latent) | PAR-NET |
| D-NET-124 | Drain/emit fan assumes type-1 (remote-joiner) nodes stay resident across a mid-match `configure_session_runtime()` | A | WITNESSED-READY-DEFERRED (latent) | PAR-NET |
| D-NET-125 | The single-drain / single-tick invariant is comment-only (nothing blocks a `NetSystem` + `Server_TickUpdate` double-owner) | A | WITNESSED-READY-DEFERRED (latent) | PAR-NET |
| D-NET-127 | Reactive-reply residual: the 0x46 per-field slot-state VALUES + the 0x51 NetId/anim binding plumbing (shape faithful) | A | OPEN (LOW residual) | PAR-NET |
| D-NET-133 | S2C 0x18 residual: fixed-slot seat mapping/empty sentinels and several modeled fields are repaired; entity+340, live AiBrain→Entity state mirroring, non-player entity+36 flags, and the pre-admission 0x0F over-answer remain (player flag/net-id gaps also remain D-NET-136/137) | A | OPEN (partial repair) | PAR-NET |
| D-NET-136 | 0x0C `entity+36` bit 0x01 computed per-recipient; diverges for ≥3 players; the faithful per-entity stamp needs the `NapiNPPlayer+0x37` gate witnessed | B | OPEN + NEEDS-RE (the +0x37 writer) | PAR-NET |
| D-NET-137 | Player wire net_id is an invented encoding shim, not the minimap-slot packing — tolerable because the client self-heals unmatched ids | A | WITNESSED-READY-DEFERRED (tolerable) | PAR-NET |
| D-NET-139 | 0x0A priority score: FULL terms ported 2026-08-06 (view-angle, LOS gate, enemy/isPlayer/standing bonuses, occupied +200, recipient-carrier +1000, inside-view +200, last-sent heading/speed delta boosts + per-recipient caches) — the 00TRg OR starvation fix (a watched patrol boat had ONE record per ~10 s) — plus the dead-recipient flat social score (600·mounted+200·sameTeam / 1000·carrier+300·occupied+100·sameTeam, replacing the positional terms; the organic death edge now latches the witnessed Flags\|=2 it reads). 2026-08-20: the tracked-handle priority floor landed on BOTH wire legs — the host stores the C2S 0x0C's 4 interest pairs per connection and floors those rows' scores past the 1124-tile gate (@0x50e93a / @0x50eb8c), and build_player_uplink fills the pairs from the witnessed client-side pairlist build (Server_BuildEntityPriorityListForPlayer @0x50df20; aim-target terms 0 — aim targets unmodeled joiner-side). Record-density parity measured the same day: retail 17.6 vs godot 17.4 records/frame — far-row "starvation" for a stationary recipient is the witnessed gate behavior on both engines, not a divergence. 2026-08-30 (the tidy round): the flat branch reads the witnessed deploy-hold storage itself (slot+89912&0x10 @0x50e67c = Connection::respawn_pending, held by join-time spectators AND pre-deploy joiners — the former canonical-spectator-bit inference and its unported pre-deploy leg are both resolved), and the owner-hidden admission landed (a spectator slot's entity leaves every other recipient's list, the self-exception kept @0x50e6fd). Residual: the invalid-tracked-slot 0x12 despawns (including for tracked owner-hidden rows), projectile chain, budget halving, pool-0 tick-displacement metric, recipient EYE anchor offset, uplink pairlist aim-target/boundRadius terms | A | OPEN (residual tail; core score parity live) | PAR-NET |
| D-NET-147 | Residual 0x10 tail: sectioned-destructible `sectionMask` rebuild + armory `weaponByte`/`attachRef` + `scoreFlag` gate deferred (the four base fields fixed + streamed) | A | WITNESSED-READY-DEFERRED | PAR-NET |
| D-NET-161 | Host vehicle simulation: ground core PORTED 2026-07-04, AI-driver/parked legs 2026-07-16, watercraft authority mover + shared avoid brake 2026-08-06 (the 00TRg defense slice). Open tail: the air family (chel/cpln authority — Super Pumas stay parked), skid/tire-slip, vehicle-vs-vehicle collision contact, water drag/drowning drain, wait-for-boarders + minAI crew clamp + stuck check, handbrake/aim-lock, boat MoveOrder merge + submerged-driver cut, `EntityAI_ProcessVehicleStateMachine @0x4583c0` non-drive states, engine sound SM, ctan/cbik dedicated contact solves; plus the authority legs routed here at the D-NET-196 close (2026-08-06): the infantry runover/crush kill leg (`@0x4b37c2..0x4b39f7`, witnessed — world-wac-ai-re.md §29.3), ground/boat contact damage + drown drain, FX/sounds, crashed-Yaw adoption, spring/oscillator softened transients, the park/wreck/crash latch machine, and the contact-direction slope-velocity store | A/B | OPEN (partial — ground + watercraft authority cores ported; air family + deferral tail remain). Catalog: net-re §8 D-NET-161 | PAR-NET |
| D-NET-164 | Game-session ordered gate + `0x44`/`0x84` NACK/resend ported (contiguous frontier, retained records, LAN 0x4B0 cap). Per-producer delivery models retail's `userParam=1` one-send records in both directions: host S2C `0x0A/0x16/0x30/0x31/0x40/0x42/0x49/0x57/0x68/0x6E/0x6F`, and joiner C2S `0x0C/0x2C/0x3D`. A mixed packet is sent intact once, then retains and reconstructs only its reliable siblings. C2S `0x4C`'s distinct finite `userParam=310` lifetime is also modeled exactly: all packets in one open logical send boundary use counter C, individual sent nodes prune at C, then the counter advances once; a node stamped at C survives through C+308 and expires at C+309, while held frames do not age it. The 0x4B0 bound now admits the exact first-N queued-node prefix before MTU splits in both directions and counts earlier transient nodes through the whole OPEN boundary. Ordinary one-record nodes in the cap-rejected tail are dropped; a semantic record that expands into FIRST/MID/FINAL is admitted only when its complete physical-node group fits, otherwise that whole semantic record waits for capacity so the receiver can never retain a stranded FIRST. A real encode failure still preserves the admitted-but-unframed owner suffix. Residual: the independent wall-time message deadline and retail's overflow-disconnect side effect remain unmodeled. | A | OPEN (PARTIAL port 2026-08-03; delivery + exact count-bound parity and fragment-group safety fixed) | PAR-NET |
| D-NET-167 | Join password `FID` + team-choice `JSP` CUs never sent — side/squad-password hosts reject (the `@0x512100` DC=18/19/20/21 legs); needs a join-password prompt + the two CUs | A | OPEN | PAR-NET |
| D-NET-169 | Self-ID is name-match (D.0): duplicate-callsign ambiguity is now a fail-fast pre-release error + the default callsign is per-machine unique; the faithful numeric self-ID (ConnectionId/dcb → player table, `Player_FindLocalPlayerEntity @0x4e0090` via 0x4D + 0x46 §5.21) stays unported | B | OPEN (guarded) + NEEDS-RE (0x4D semantics) | PAR-NET |
| D-NET-171 | Remaining host-side join-reject legs (version/environment/expansion CRC and the ordinary server/side password families) silently DROP where retail answers a reject the client renders as a reason — a rejected retail client sees why; a rejected OpenNova joiner times out to the 60 s watchdog. Narrowed 2026-08-30 (D-NET-217): session-full now answers the witnessed CR=0 0x82 `JFC=14/JFP=4\|5` (`NapiNPProtocol_SendJoinRejection @0x620cd0`), and the spectator disabled/full/password legs answer the game-layer DPC 14/15/16 description punts; the OpenNova joiner consumes both vehicles. The remaining `Server_ValidatePlayerJoinRequest @0x512100` families (codes 2..48 beyond the spectator trio) still drop silently at our game-layer gate. | B | OPEN (port the remaining game-layer DPC reject families) | PAR-NET |
| D-NET-174 | Our HOST does not implement the fire-freshness gate the S2C `0x61` tick seed anchors: retail stamps the per-player seed into `playerSlot+0x178D8`, rejects a C2S `0x06` whose tick is zero or not past it, and on acceptance re-stamps `floor = tick + adm[276]` (the per-weapon refire window — this also closes the unparsed `adm[276]` deferral at `server_message_dispatch.cpp`). We seed and re-roll per connection now (client + host), but accept every `0x06` regardless of its tick, so an OpenNova host cannot rate-limit or reject stale fire the way a stock host does. The SECOND leg of the same predicate is also unported: retail runs it locally on the AUTHORITY side too — `Entity_FireWeaponAndSendPacket @0x42bd80` splits on `is_authority` (`@0x42bdfd`) and the authority arm gates its own local player's shot on `PlayerSlot_IsActive @0x4fc760` (`@0x42be3a`, bail `@0x42be44`) before clearing `+0x178E2`, so a listen host rate-limits itself with the same floor it enforces on joiners. The joiner arm (`@0x42bf46`) has NO gate — a client fires locally and transmits regardless, and 2026-07-25 removed an invented client-side refusal that had been cited to the authority arm's addresses | B | OPEN (port `PlayerSlot_IsActive @0x4fc760` + the `@0x513740` stamp against the per-connection `tick_seed`, and the authority-side local-fire leg with it) | PAR-NET |
| D-NET-179 | `APPID` is conditional, not an always-missing parity field. The older retail-ashi5a f=199140 and retail_join_v18 f=47676 LAN captures carry an 18th CU with value **9360**, while both fresh `p403f16` retail-client legs (retail→retail and retail→OpenNova) carry the same 17-CU core as OpenNova and omit `APPID`. The static predicate is witnessed: `CNapiServerInfo_SerializeToSession @0x4c3650` writes it only when `NapiServerInfo+64` is nonzero. The LAN-path source of that slot remains unresolved: the known NovaWorld populate site is `UI_JoinSelectedSession @0x569b8e`, while its LAN branch clears the same block at `@0x569bbb`, suggesting another boot-persisted source in the older runs. No unconditional `9360` was added; parity comparison must follow the matching retail↔retail oracle's field presence. The receiver merely stores it (`NapiNetConfig_LoadFromConnTags @0x4c7260`, store `@0x4c7400`) and admission never reads it | A | OPEN (conditional, low severity) + NEEDS-RE (nonzero LAN source of `NapiServerInfo+64`) | PAR-NET |
| D-NET-182 | Retail's HOST-originated numeric punt family is now ported except for the type-6 violation producer. The shared `Server_LogCRCMismatchPunt @0x517ed0` chain formats `tN` and sends the exact reliable connection-description record (full `H:0x03`, flags `0xA0`, seven `DS/DC/DP1/DP2/DSTR/DPC/DDSTR` TLVs); `Server_StageHostDisconnect` gives each connection a first-event-wins latch and closes its gameplay gate before the one allowed flush. Landed producers match the witnessed gates and strict boundaries: type 16/24 checks the eighth unanswered character-attribute/time-sync round before decrementing the 744-tick request countdown, with type 16 precedence; type 35 measures the state-6 join/deploy entry timestamp and fires strictly after 360000 ms; type 7 counts consecutive dead state-6 ticks, resets on a live tick, fires strictly over 360, and is suppressed by permanent-death mode. On receipt, the joiner now runs retail's teardown leg automatically: four identical keyed `CLIENT_GOODBYE` datagrams are queued before the terminal transition, the first removes the host peer/entity, and the remaining three are idempotent. `npruntime_host_punt` pins the exact bytes, first-event latch, t7/t16/t24/t35 boundaries, receiver, and end-to-end goodbye teardown. **RESIDUAL:** type 6 reads two `CRenderState` violation fields (3/5) for which OpenNova has no modeled source; emitting it from a guessed proxy would be invention. The generic description seam also serves the exact DPC-46 integrity punts in D-NET-181. | A | OPEN — PARTIAL: exact serializer/latch, t7/t16/t24/t35, and automatic goodbye landed; only the type-6 render-state source remains unmodeled | PAR-NET |
| D-NET-189 | A remote player's RELOAD was invisible to our client: the joiner present leg passed a literal `false` for the hold-state selector's `reloading` argument, so no wire-decoded peer could reach hold state 65/66. Investigating it corrected a premise — **a retail PURE CLIENT never plays a peer's reload CLIP either**. `NapiNPClientMsg_WeaponReload_0x049 @ 0x42c0a0` branches on the addressed entity's item type and, for a remote PERSON, stamps `entity+0x371 = 80` `@ 0x42c10b` and RETURNS `@ 0x42c113` — it never reaches `WeaponSlot_ReloadAmmo @ 0x541720`, whose `@ 0x54173c` is the sole writer of the `+0x372` window the pose selector reads `@ 0x4b5e5e..0x4b5e6f`. A HOST does play it, running the refill on its own copy of the requester `@ 0x514f03` (gated `g_local_player_entity != *player`). The observer's entire feedback is an ARMS DIP: `+0x371` drains TWICE per tick into the pitch-kick accumulator `+0x36C` `@ 0x4b5cab..0x4b5ce7`, so an 80 stamp dips for 40 ticks (~0.64 s at 62 Hz). PORTED 2026-07-27: `ClientEntityState` carries the window and the term; `ClientReplicaPipeline::tick_arms_dip()` rides the same body tick as the lean integrator; and `aim_overlay_inputs_for_client` feeds `pitch_kick_accum` through. Our LISTEN HOST had the mirror gap (it relayed 0x49 and refilled the clip but never stamped the pose window) and now stamps it, above the two bails retail does not have. Flipping the `reloading` literal is explicitly NOT the fix: it is retail-incorrect for a pure client, and states 65/66 carry anim flag `0x84` whose `0x80` bit selects the held weapon's HAND attach frame (D-WPN-32). **Ordering FIXED 2026-08-02:** IDA confirms `Game_ProcessMainFrame` calls `Client_ProcessNetworkFrame @0x526692` before `Entity_UpdateAllEntities @0x52674b`. `ClientRuntime` now drains each decoded S2C `0x49` at the receive/body boundary, stamps only a non-self `Player`/`Infantry` row to 80, then runs that same frame's `tick_arms_dip()`; the first body pass therefore ends at 78 with `pitch_kick_accum = -0x02300000`, exactly pinned through a framed public-seam regression. The notification remains queued for `Simulation`'s diagnostics and self-only refill, and the old post-body duplicate stamp is gone. RESIDUALS: retail's NON-person remote 0x49 branch (the real refill on a vehicle/emplaced weapon `@0x42c116`) is unported rather than invented; our decoded rows carry an `EntityClass`, used as an ANALOGUE for retail's `ItemType_Person` (3) because they hold no ItemDef type; and showing a peer the reload CLIP on a pure client would be a deliberate DIVERGENCE needing its own entry, not a port. | A | OPEN — PARTIAL: exact remote-Person onset/shape/presentation and listen-host leg ported; non-person/type-source residuals remain | PAR-NET |
| D-NET-196 | Client replica movers/prediction (the #403 train): per-class interpolation targets staged, class movers chase and predict — the client subset | A | **OPEN — LOW** (2026-08-06 state: the vehicle B-facet, the resolver wiring, the flag channels incl. the indoors probe-skip, the per-motor water/float blocks, the deck-ride with rotate-about-carrier, and the first-tick/first-compact timing dispositions are ALL landed — net-re §8 + world-wac-ai-re.md §29. The 2026-08-06 parity round then landed (1) the row planar-velocity channel (maintenance/integrate/ledge-3/4-carry/water-drags — world-wac-ai-re.md §29.2a), (2) the ledge-edge anim stamps (org2 31/47 straight; org1 keeps-clip witnessed), (3) carrier-side `savedLivePose` (`Entity::saved_live_*` + `stamp_saved_live_pose` in both world vehicle passes; the provider serves the witnessed live+saved pair), and (4) the capsule-derived eye vertical `min(top−bottom, 0xD000)` in the water lines `@0x4b6984..`. Remaining OPEN items: (a) the org2 deck bodyPitch/torso-aim adoption — needs the replica body-conform presentation channel (the same +0x90 consumer family as the unported replica slope pass); (b) the deck-carried VEHICLE follow legs `@0x48D6DA..0x48DACD`/`@0x4905BC..0x49095B` (witnessed — same math as the org rides, no capsule bias, no radius drop), sequenced behind D-NET-161's vehicle-vs-vehicle contact, without which no vehicle ground link can form on our side. Non-divergences: the one-frame carrier-delta lag (inside retail's own mover-order envelope) and replay/spectate (stack deleted #419). The splash/landing FX-sound edges ride the SOUND slice (open for local rows too — world-wac-ai-re.md §22.5). The ewep dead-carrier retire-vs-hide equivalent stays as ratified in the #424 review (the literal hide breaks `netsim_loopback_identity`); revisiting it needs its own decision. The vehicle-family live A/B remains the standing validation pass. Authority legs ride D-NET-161; local-motor water rides D-INF-3 (the ladder tail landed with D-COL-5, world-wac-ai-re §30); 2026-08-21: the org2 goal-resolve block is `@0x4B42A7..0x4B464A` (netsim's `0x4B4470` start is late) with the client z-soften `@0x4B4626/@0x4B4635`; the AUTHORITY smoothing arm `@0x4B45E5..0x4B4618` (bucket 10 `@0x4B4466`, z step zeroed, x/y zeroed when `|+0xA0| > 0x686`) is unported; 2026-08-21: the ground-family spring sinks/oscillators are WIRED (`vehicle_suspension.cpp` — vehicle-client-movers-re §7.3: the crash latch `+0x2ED != 0 && +0x2EC == 0`, the role pick as a −Z chassis-tilt impulse, dt 0.75/3.0 normal/crashed, the +250/+187/+100 sink growth); still open in the crash machine: the crash-request producers whose inputs are unported (the bike's `+0x3DE` has-been-driven byte, the client 10-tick airborne window `+0x2F1/+0x2F0/+0x2F8`) and the contact-direction slope feed (D-NET-161) | PAR-NET |

Closed 2026-08-26: **D-NET-140** -> `FIXED` — the listen host's own player now receives retail's
header-only 0x0A (14 B, or 25 B with the phase-0 block: no priority build `@0x517c1b`, no
records/rounds/terminator `@0x50f07e`, parser return `@0x430174`) and the host presents from its
own pools (`Simulation::present_snapshot_from_world`; the effect-pose and sun-visibility walks read
the registry), while a joiner keeps presenting its decoded `ClientState`. `Server_TickUpdate` skips
the replication raycast prep and world snapshot when no in-match recipient takes entity records.
[ADR 0011](adr/0011-single-player-in-process-listen-server.md) Decision 1 is amended; the
permanent-register row is retired.

Closed 2026-08-29: **D-NET-166** -> `FIXED` — the row had gone stale: the JOIN `VERSIONCRCSTRING` is no longer pinned `"0"`. The joiner CRC-32/MPEG-2s the loose `expansion/<name>/version.txt` under the binding-supplied install root at JOIN-build time (`vfs_expansion_version_checksum`, `JoinerConnection::expansion_version_root_`, `Simulation::set_join_expansion_version_root` fed by `mission_presentation.gd`), the host computes its `g_expansion_checksum` analog from `game_root` and runs retail's compare only while an expansion is active `[orig: Expansion_SwitchTo @0x5688c0 -> Expansion_LoadAssets @0x4a4885; the JOIN write @0x42a2b4; Server_ValidatePlayerJoinRequest @0x512100, reject DPC=48]`; `"0"` is emitted only when no root/expansion/file exists, which is retail's own value there (`vfs_test` D-NET-166 leg, `handshake_server_test` expansion + base-game gates; full entry: net-re §8).

Closed 2026-08-24: **D-NET-115** -> `PERMANENT` (the seed and the sim/effects stream split; the placement itself stays MATCHING) — the far-marker random arm and the 0x100 death roll now draw from one owned MSVC-CRT recurrence (`World::crt_rand`, `io::CrtRand`) in retail's draw order, seeded from the session seed at `create_session` with the slot-table draw spent, where retail seeds its process `rand()` from the wall clock once at host start; the ported render/effects `rand()` consumers (scorch texture rolls, material/PANM noise, the `@0x5CC1F0` glass-reseed target) share the separate thread-local `engine/base/crt/crt_rng.h` stream, so retail's frame-rate-dependent cross-family interleaving never perturbs the sim stream — unobservable on the wire since every order-coupled value is authority-drawn-and-shipped `[orig: Server_AllocatePlayerSlotTable srand @0x51C1AA / rand @0x51C1AF; Entity_FindBestSpawnPoint @0x50CEA2; GameEvent_PlayerDeath @0x51718A]` (register below; full entry: net-re §5.42 D-NET-115).

Closed 2026-08-22: **D-NET-172** -> `FIXED` — `build_spawn_zone_list` reproduces the both-zero-key raw-address tie without allocator dependence: retail's one pool allocation places pool 1 at +232420 (stride 1360) and pool 2 at +1865416 (stride 812), so unnumbered co-op deploy letters put every pool-1 row before pool 2 exactly as the original bubble sort does `[orig: EntityPool_Allocate @0x442130; Entity_BuildSpawnZoneList @0x43EAE0, compare @0x43ECC6]` (`zone_chain_test`; full entry: net-re §8).

Closed 2026-08-10: **D-NET-210** -> `FIXED` — LAN host bind scan ported: the authority arm feeds `{mplanserverportmin/max/delta, random=0}` into the socket open, `(max-min+1)/step` tries first at min, stepping by delta `[orig: CNapiNetwork_OpenTransportSocket @ 0x4c6a40 -> NapiUdpSocket_CreateAndBind @ 0x62d2a0; clamp NapiSocket_ClampBufferParams @ 0x62e180]`; live-proven with two hosts on one machine. Residue: our joiner binds an OS-assigned port where retail's client arm scans its own authored quad — behavior-neutral against stock peers (full entry: net-re §8).

Closed 2026-07-05: **D-NET-30** -> `FIXED` — one `Cookie: name=value;` header
per cookie (`CookieJar::cookie_header_lines()`; our own server already merged
multiple `Cookie:` headers, so the merged-line client was the sole
inconsistency) + the subnet key ported (`subnet_key()`, IPv4 /16)
`[orig: CUIBrowser_SendHTTPRequest @ 0x658840; Network_TruncateIPToSubnet
@ 0x62dfe0]`; C2 summary row flips to matching.

Closed net entries with a permanent facet are listed in the permanent register below
(D-NET-133 empty-slot facet). Closed 2026-07-05: **D-NET-20**
-> `FIXED` (the Cookie var-list parent emitted unconditionally; empty-cfg flow pinned
in `client_session_loopback_test`). Closed 2026-07-19: **D-NET-21** -> `FIXED`
(`ClientSession::process_periodic_update` now emits the one-shot `ClientConnected`
after `ServerSessionInit`; both Godot session pumps call the boundary after their
receive drain, and `client_session_loopback_test` pins no synchronous 0x82 reply).
Closed 2026-07-24: **D-NET-131** -> `FIXED`
(`start_host_session` selects mode 1 / `HostOnly` when `serve_and_play=false`, creates
no type-2 loopback and therefore no phantom local player; serve-and-play retains mode 3).

Closed 2026-08-12: **D-NET-121** -> `FIXED` — the S2C 0x0A writer no longer accepts a
dvxi5 fallback anchor. It validates the connection's live owner allocation before any phase,
visibility-cache, or round-watermark mutation; an unbound, despawned, or packed-slot-reused owner
emits nothing until explicitly rebound. `[orig: Server_SendEntityStateToPlayer @0x517BA0 state==6
gate; recipient entity eye reads @0x517BF5..0x517C13; phase increment @0x517BE8]` (full detail:
net/novaworld-net-re.md P4/§5.44).

Closed 2026-07-20: **D-NET-135** -> `FIXED` (the four world pools now use retail's
650-B post-write guards with margins 0x10=40, 0x0D=110, 0x0C=100, and 0x20=30;
the crossing record remains in the page, while 0x45 tiles retain their separate
650-B pre-write cap; boundaries are pinned by `npruntime_batch_chunker`, with the
production path covered by `npruntime_initial_state_burst`).

De-tabled 2026-08-06 (the closed-row compaction — full entries live in the
record's §8 catalog; the table above holds OPEN work only):

Closed 2026-08-01: **D-NET-9** -> `FIXED` — Gate numeric fields now use the witnessed NAPI literal dispatcher in retail order: character, hexadecimal, octal, binary, then decimal (full entry: net-re §8).
Closed 2026-08-01: **D-NET-17** -> `FIXED` — ClientHello models and round-trips the four previously omitted conditional fields in retail order: `DE`, `PV3`, `PM`, and `ET` (full entry: net-re §8).
Closed 2026-08-01: **D-NET-22** -> `FIXED` — The join client and listen host now build the witnessed ten-field verify Cookie from one Godot-owned environment snapshot: Win32 English country/language, base timezone bias with the correct sign ... (full entry: net-re §8).
Closed 2026-08-01: **D-NET-29** -> `FIXED` — NAPI envelope decode supports retail's extended-header mode when the first dword is zero: carrier/CRC at +4 and header size at +9, with bounds checks (full entry: net-re §8).
Closed 2026-08-01: **D-NET-49** -> `FIXED` — The Joint Operations protocol GUID is the retail static-initializer value `46 D6 74 B0 F9 81 5F 47 92 DA DE A7 24 7F 14 68` (full entry: net-re §8).
Closed 2026-07-22: **D-NET-117** -> `FIXED` — World-path `HostJoinerPose.pitch` now resolves the bound `AiEntity` and carries the signed high 16 bits of its BAM32 look pitch (full entry: net-re §8).
Closed 2026-08-03: **D-NET-134** -> `FIXED` — S2C 0x0A now uses retail's free-running phase byte (full entry: net-re §8).
Closed 2026-08-02: **D-NET-163** -> `FIXED` — Full Godot+npruntime host steady-state coverage now matches the cfg-only retail oracle: all integrity pairs plus S2C 0x58/0x5D/0x79/0x7E are live (full entry: net-re §8).
Closed 2026-08-03: **D-NET-165** -> `FIXED` — LAN `0x81` now carries the live `CNapiServerConfig_BuildFlags` value in `P2`, snapshotted once at session creation and shared with the S2C `0x08` tail (full entry: net-re §8).
Closed: **D-NET-168** -> `FIXED` — The joiner's `0x2F` loadout pair was a fixed default kit with no wire seam to the shell's applied selection; closed via the `NetPacket_SendLoadoutSubmit @0x42cdc0` witness (full entry: net-re §8).
Closed 2026-07-24: **D-NET-170** -> `FIXED` — The joiner now folds every valid S2C `0x5A` grant into its authoritative loadout revision and rebuilds the local inventory at the recv-before-actions boundary without echoing the grant as a new C2S ... (full entry: net-re §8).
Closed 2026-08-04: **D-NET-173** -> `FIXED` — The HOST side of retail's EMPTY send interval is ported: the per-connection flush arms a "last framed anything" clock (any batch, pre-framed settings/resend, or minted packet stamps it), and with ... (full entry: net-re §8).
Closed 2026-08-02: **D-NET-175** -> `FIXED` — The joiner answers all three periodic requests through the shared holdoff-gated, MTU-batched send boundary: `0x43` → C2S `0x08` with echoed server stamp + monotonic-ms client stamp (full entry: net-re §8).
Closed 2026-07-25: **D-NET-176** -> `FIXED` — The ENTITY-REMOVAL fold was entirely missing: a peer that left (or any entity the host freed) kept its decoded `ClientState` row forever, so its wire-present node AND its wire person collision proxy ... (full entry: net-re §8).
Closed 2026-07-26: **D-NET-177** -> `FIXED` — A session loss was never surfaced: a host that closed or went silent left the joiner parked in a dead world with no feedback (the 60 s admission watchdog only covers the pre-match legs) (full entry: net-re §8).
Closed 2026-07-25: **D-NET-178** -> `FIXED` — A joiner mounted its resource root from the LOCAL persisted expansion setting and never from the host's, while still echoing the host's `ServerHello.SUS2` in its C2S JOIN `EXP` TLV (full entry: net-re §8).
Closed 2026-08-12: **D-NET-180** -> `FIXED` — C2S `0x2F` now re-resolves the live equipped combo against the populated local slot table and assigned-side mask, scanning only the same 65-slot category while preserving the raw first/team-change 195 fallthrough (full entry: net-re §5.56).
Closed: **D-NET-181** -> `FIXED` — Anti-cheat S2C `0x30`/`0x31` remains SILENT BY DEFAULT because the retail host recomputes each source and punts any mismatch (`PUNT WCRC` / `PUNT ACRC` (full entry: net-re §8).
Closed 2026-07-26: **D-NET-183** -> `FIXED` — Our multiplayer C2S `0x2F` sourced its kit from the JOINED MISSION's `.bms` loadout chunk and paired it with a HARDCODED `player_class = 8`, two values produced by code paths that never consulted ... (full entry: net-re §8).
Closed 2026-07-26: **D-NET-184** -> `PERMANENT` — A retail co-op HOST's OWN first-person weapon visibly reacts when another player fires, whenever both hold the SAME weapon (primary or secondary), with no matching motion on the shooter's ... (register below; full entry: net-re §8).
Closed 2026-07-26: **D-NET-185** -> `FIXED` — Our joiner's per-frame C2S `0x0C` extended uplink never carried the entity Flags byte: `netsim::build_player_uplink` filled the carrier handle, pose, movement-input byte and equipped ADM index and ... (full entry: net-re §8).
Closed 2026-08-02: **D-NET-186** -> `FIXED` — The DEATH deploy screen could send at most ONE `0x0E` deployment pick per session, so a player killed mid-match could never respawn (full entry: net-re §8).
Closed 2026-07-26: **D-NET-187** -> `FIXED` — The listen host's OWN player respawned at full health but never moved off its corpse (full entry: net-re §8).
Closed 2026-07-26: **D-NET-188** -> `FIXED` — The upper-body WEAPON CHANNEL was derived and presented for the LOCAL player only, so every other player (full entry: net-re §8).
Closed 2026-07-27: **D-NET-190** -> `FIXED` — GSB SVRS row dword1 is the host IPv4 (in_addr bytes (full entry: net-re §8).
Closed 2026-07-27: **D-NET-191** -> `FIXED` — GSB SVRS records ACCUMULATE (retail appends with no per-record clear, `@0x63dbec..0x63dc0d`); our parser cleared per record so only the final SVRS chunk survived (full entry: net-re §8).
Closed 2026-07-27: **D-NET-192** -> `FIXED` — The "GSB " record is a GATED reset (frees fields + rows only when payload dword0 == 0x00010000 `@0x63d8f2`, else skipped) and undersized FLDS/SVRS records (payload < 2, `@0x63d7c2`/`@0x63da43`) skip ... (full entry: net-re §8).
Closed: **D-NET-193** -> `PERMANENT` — Retail's GSB parser is an incremental HTTP callback with NO failure return (offset persists at ctx+128 (register below; full entry: net-re §8).
Closed 2026-08-03: **D-NET-194** -> `FIXED` — Retail's joiner NEVER opens the mission `.bms`: `Game_StartMission @0x524360` authority-gates every disk leg, while the non-authority arm builds terrain/environment identity from the exact 616-byte ... (full entry: net-re §8).
Closed 2026-07-27: **D-NET-195** -> `FIXED` — On non-COOP retail hosts exactly ONE world vehicle per map rigid-followed the local player around, orbiting as they turned (reported live; varies per map) (full entry: net-re §8).
Closed 2026-08-01: **D-NET-197** -> `FIXED` — Send-rate defaults: retail uses a session-selected period stored/counting down per connection: NovaWorld 12; authority LAN `g_LanMode` 1..4 -> 12/6/4/3 (stock mode 1); SP and loopback 1 (full entry: net-re §8).
Closed 2026-07-31: **D-NET-198** -> `FIXED` — Our joiner parsed a retail host's dictated send-holdoff (H:0x00 mask 8 / CS field 3) but forgot it after one skip, then uplinked per-tick forever (full entry: net-re §8).
Closed 2026-07-31: **D-NET-199** -> `FIXED` — The joiner's C2S uplink omitted held-jump MoveOrder bit 5, and our host did not derive remote jump anims (full entry: net-re §8).
Closed 2026-08-02: **D-NET-200** -> `FIXED` — The host's live 21-byte vehicle compact hard-coded its three prediction registers to zero, so a retail/OpenNova client could not receive the authority motor's forward command, lateral command, or ... (full entry: net-re §8).
Closed 2026-08-02: **D-NET-201** -> `FIXED` — S2C 0x76 was mislabeled `SERVER_TICK16` and serialized `now_tick & 0xffff`, so an OpenNova host sent a changing clock word where a retail client expects the host's class-availability policy (full entry: net-re §8).
Closed 2026-08-02: **D-NET-202** -> `FIXED` — Retail's `CNapiNPConnection_DispatchMessage` owns one fragment buffer per connection and applies the same FIRST/MID/FINAL fold in either receive direction (full entry: net-re §8).
Closed 2026-08-02: **D-NET-203** -> `FIXED` — OpenNova serialized the mission-file name into both S2C `0x7B` mission and map fields (full entry: net-re §8).
Closed 2026-08-02: **D-NET-204** -> `FIXED` — OpenNova rebuilt S2C `0x0B` from its parsed BMS model, but retail copies the exact 616 loaded header bytes (full entry: net-re §8).
Closed 2026-08-02: **D-NET-205** -> `FIXED` — OpenNova always serialized `mission_file` as the S2C `0x60` VarList's `MISSIONNAME` (full entry: net-re §8).
Closed 2026-08-05: **D-NET-206** -> `FIXED` — A live OpenNova listen host never emitted the §5.34 maintenance-quartet S2C `0x68` loaded-model page request, so a joined retail client's C2S `0x3D` reply never fired either (full entry: net-re §8).
Closed 2026-08-05: **D-NET-207** -> `FIXED` — The joiner's world-stream folds clamped pool slots at two INVENTED capacities (full entry: net-re §8).
Closed 2026-08-05: **D-NET-208** -> `FIXED` — A joiner NEVER learned that a destructible died: no explosion FX, no husk swap, items stood intact on the joiner while the host showed the full destruction (user-reported live on the 01TR retail ... (full entry: net-re §8).

Closed 2026-08-20: **D-NET-215** -> `FIXED` - the S2C 0x14 header is `[channel][sender_slot][cstr text]`, not the reverse: the handler tail-jumps `Chat_DispatchToChannel(body[1], (char)body[0], &body[2])` `[orig: NapiNPClientMsg_ChatMessage @0x42F240; Chat_DispatchToChannel @0x42B910]`; `decode_chat_broadcast` and its coverage fixture were corrected together and the channel stays signed; residual (net-re §5.52): the dispatcher's muted/spectator sender gate, `ClientRosterSlot` lacking both slot bytes (full entry: net-re §8).
Closed 2026-08-30: **D-NET-217** -> `FIXED` — spectator discovery, Player/Spectator choice, ClientAuth JSR/JSPP, the two witnessed reject vehicles (0x42 CR=0 `JFC=14/JFP=4|5` shared-capacity; game-layer DPC 14/15/16 description punts with a case-insensitive JSPP compare), team-0 hidden spawn, 0x75/0x0A/0x16 state, joiner free-flight, and real F3/MCP authority mutation now share one portable player-slot bit across both wire directions (full entry: net-re §5.0e/§8).

### Environment — [env/env-tod-re.md](env/env-tod-re.md) (#-catalog) + [env/env-honored-matrix.md](env/env-honored-matrix.md)

Ratified 2026-08-30 (the post-merge tidy round; ratification = this PR's
merge): **env #8** -> `PERMANENT` (envscale at interpolation vs retail's
positional parse-time bake — register, class C) and **env #9** -> `PERMANENT`
(the fog→skyfog per-keyframe flag vs the `0xC0C0FF` leftover-slot sentinel —
register, class C). The same sweep normalized the catalog's residual
"Tracked decision"/"Documented" dialect onto the canonical vocabulary:
**env #12** and **env #13** read `FIXED` (faithful — the default mirrors the
raw-200 quirk; `vertex_rgb` is unconsumed in retail too), and **env #35**'s
record cell now opens `FIXED` to match its 2026-07-10 committed-LUT closure.

Implemented **libs/env-first** so the ENG-2 port inherits the closures. The
honored-matrix PARTIAL rows (iris, ceiling/floor, lightning, glare_3di)
map onto these `#` entries. Closed 2026-07-05: **env #21** -> `FIXED` (the frame-clear
horizon blend ported libs/env-first + consumed by the GameWorld clear; witness in
[env/env-tod-re.md](env/env-tod-re.md) #21), and **env #19** -> `FIXED` (the
observable terrain_rgb consumers are the tile-overlay HALF×MODULATE2X path and
the effects reciprocal. Fresh foliage re-grill 2026-07-13 proved its sampled
FULL-tint color is overwritten by the detail bend carrier before emission; the
texture-bake consumer is also dead, so untinted terrain remains faithful).

Minted-and-closed 2026-07-05 at the ENG-2 weather-core port (grill of
`Environment_UpdateWeatherTick @ 0x57e9b0` + cluster): **env #22** (the weather-PRNG
signed-carry transcription bug), **env #23** (lightning SET-per-epoch vs the GDScript
maxf plateau + integer additives), **env #24** (the wind model: the 0..8192 strength
scale drove the oscillator past its stability envelope; retail runs the constant
`Env_WindScale = 256`, now the default; smoothers chase keyframe targets). All three
were discovered, witnessed, and fixed in the same slice — catalog rows in
[env/env-tod-re.md](env/env-tod-re.md) #22–#24. Minted-and-closed 2026-07-06 at the
sky-leg re-grill: **env #25** — the weather-PRNG seed is `0x12333333`
(`[orig: mov imm32 @ 0x57d2ff]`; `0x12345633` was a transcription error shared
with the WAC RNG seed `[orig: @ 0x4f966b]`). The libs/wac VM carried BOTH bugs
(wrong seed + env #22's unsigned bit-31 carry `[orig: signed rol9+sar+add
@ 0x4f5a83..0x4f5a91]`) — fixed in the same commit; no committed test pinned
the wrong WAC stream. The sky binding slice minted-and-closed **env #26**
(the cloud-scroll consumption model: rate ramp skipped + the accumulator term's
U sign; WeatherCore now owns the witnessed CloudScrollState) and minted
**env #27** (the smoothed scalar spring channels — fog distance, sky height,
FOV, one unidentified pair — remain unwired; consumers read parsed values;
witnessed-ready-deferred). The water leg (2026-07-06) grilled the previously
unwitnessed surface pipeline (render_water_surface @ 0x5c32c0 — a split
function two misnomers deep): **env #28** minted-and-closed (water-height
precedence — witnessed BMS > TRN(bit-31-flagged) > ENV; the reimpl ladder ran
env-over-terrain, now reordered), **env #31** minted-and-closed (the invented
water look — sin/cos waves + fresnel — replaced by the witnessed per-frame
noise color + DuDv textures over the lit-color pipeline, libs/env-first,
ctest + vector pinned), **env #29** minted OPEN (the screen-marched adaptive
strip tessellation, spec complete — the plane is the tracked stand-in), and
Closed 2026-07-06 (the REN-6 port leg): **env #27** -> `FIXED` — the scalar
springs live in `env::EnvScalarChannels` (witnessed steps + in-tick order,
ctest-pinned), ticked by the weather core with parsed-value targets
(targets-only snap `[orig: @ 0x57d1e0]`) and written back through the env
seam so every consumer (dome, water UV, object/terrain fog ends, the frame
clear) serves the ramp; SunDim is live end-to-end (celestial sun + glare);
rain%/overcast channels are state-live awaiting their systems, FOV rides the
camera. **env #33** -> `FIXED` — the witnessed generator + twinkle ported
(`env::generate_star_instances`/`star_twinkle_tick`/`star_visible_fixed`,
ctest-pinned) and hosted as the 256-instance camera-anchored billboard field
(`StarField` + `godot/src/env/celestial.{h,cpp}`; per-star twinkle, 0.98 near-light
cull, regenerate-per-load); the single-body stand-in deleted. Details:
[env/env-tod-re.md](env/env-tod-re.md).

**env #30** minted NEEDS-RE (the reflection passes exist; spec deferred; internals closed at the REN-6 witness leg). The
celestial leg (2026-07-06) CLOSED **env #14** (the glare occlusion — the witnessed
model is 2 jittered rays/frame into an 8-sample sliding window + dead-band
hysteresis, ported libs/env-first with the Celestial terrain ray march) and
minted-and-closed **env #32** (placement inventions: dir×2000×height_scale, zeroed
camera height, the dir.y gate — the live renderer places at camera + dir × 64 with
witnessed alpha folds), plus **env #33** (the 256-instance star field with per-star
twinkle — specced, table generator unfound; WRD).

The render-consumer rows transferred to the REN track on 2026-07-05
([ADR 0023](adr/0023-render-visual-parity.md), Slice column updated): #17 →
REN-5 (the modulator chain is a render-lighting consumer); #27/#29/#30/#33 →
REN-6. Dispositions unchanged — the transfer moves ownership, not status.

The REN-4 shader/TSS decode (2026-07-06) minted-and-closed **env #34** — the
water surface framebuffer blend + far cutoff: retail draws the above-water
surface with SrcBlend ONE + DestBlend SRCALPHA
`[orig: Water_InitSurfaceShaders @ 0x5c19b0; render_water_surface
@ 0x5c33f0..0x5c3419]`; the reimpl used standard alpha blending —
`water.gdshader` now expresses the witnessed blend exactly
(`blend_premul_alpha` + inverted alpha). **The alpha-test half was RE-GRADED
at the 2026-07-07 fidelity grill** (the user-reported short water draw
distance): ALPHATESTENABLE rides pass-flag bit 0x40000
`[orig: CGfxShader_ApplyPass @ 0x68326b]`, which the water passes never set
— `SetAlphaTestRef(0x20)` is an inert device latch `[orig: @ 0x6770a0]` and
the ref-32 discard was a misport, now deleted (env-tod-re.md #34).

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|

Closed 2026-08-30 (the weather port, ONE home / ONE clock): **env #15** ->
`FIXED` — thunder epochs surface as `WeatherTickEvents`, the kernel logs
`world::WeatherSoundEvent`s and `MissionAudio.play_weather_sounds` plays the
THUNDER set; `flash`/`farflash` ARE `Env_TriggerLightningFlashA/B`. **env #16**
-> `FIXED` — `overcast.def` is the live overcast table, cross-faded per tick by
the weather's pre-spring overcast blend (`env::blend_tod_states`). **env #18**
-> `FIXED` — the quake jitter + camera shake, the rain/snow drop pool, the rain
ambient, and every WAC weather handler land through `world::WeatherState`
(env-tod-re.md §The WAC weather handlers, §Precipitation).

De-tabled 2026-08-06 (the closed-row compaction — the table above holds
OPEN work only; full detail in the named record + git history):

Closed: **env #17** -> `FIXED` — Iris auto-exposure modulator gain — FIXED 2026-07-06 (REN-5): the modulator CHAIN is live (`env::ModulatorChain` ticks modulator2 → modulator → the hosted blocks in the witnessed order `[orig: @ ... (full detail: env-tod-re.md + git history) Witness: [orig: @ 0x57e512; @ 0x57d940].
Closed: **env #29** -> `FIXED` — Water surface tessellation — FIXED 2026-07-07 (the REN-6 tail): the DETAILED tier live end to end (`env::water_*` structural translation with 40 ctest pins → `WaterCore.strip_*` packed arrays → ... (full detail: env-tod-re.md + git history).
Closed: **env #30** -> `FIXED` — Water reflection — FIXED 2026-07-07 (the REN-6 tail): reimpl planar reflection (SubViewport mirror camera about y = wh, up-column-negated proper mirror — the witnessed strip rows pin u = screenU / v ... (full detail: env-tod-re.md + git history) Witness: [orig: Water_InitSurfaceShaders @ 0x5c19b0; render_main_scene @ 0x5c1240] Witness: [orig: allocator @ 0x5c08d1..0x5c0937; viewport @ 0x5c1464..0x5c1614].
Closed 2026-07-10: **env #35** -> `FIXED` — Water sine LUT provenance: the runtime `std::sin` build forked per libm at trunc boundaries (the GitHub `macos-26-arm64` image flipped non-landmark bytes and every downstream noise pixel (full detail: env-tod-re.md + git history).
Minted-and-closed 2026-08-16: **env #37** -> `FIXED` — Water reflection-sample brightness (~2.7× at the retail-matched CP01 pose): the WITNESSED mirror dim was unported — retail multiplies the finished reflection RTT by 0x404040 (SRCBLEND=DESTCOLOR/DESTBLEND=ZERO fullscreen quad `[orig: render_main_scene @ 0x5c186c..0x5c189e]`) before the water shader samples it; ported as `env::kReflectionDimFactor` + a multiply quad over the mirror SubViewport. Post-fix matched ratios 0.94–1.03 at three pitches (full detail: env-tod-re.md #37 — the former near-row D-RMAT-8 attribution is retired by the 2026-08-22 gamma-framebuffer cutover; celestial-after-dim ordering, detail-3 512² RTT, and the unwalked second additive quad remain to be remeasured after renderer validation closes).

### World / AI + mission events — [world/world-wac-ai-re.md](world/world-wac-ai-re.md), [mission/bms-event-runtime-re.md](mission/bms-event-runtime-re.md), [world/itemdef-re.md](world/itemdef-re.md)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-INF-2 | Command channels 123–127 are ported (#571, 2026-08-25: the board walk to the filtered seat point, the 126 hold and the 127 follow — world-wac-ai-re §23.4; child-seat traversal landed with `find_best_seat` 2026-08-24). Still pending: the E1..E8 entry-bone claim/stagger, the 64-tick seated re-upgrade, the 11000/12000/12001 escort offsets and the can't-enter fallbacks, `Entity_CanEnterVehicle @0x435480`'s internal gates, non-UseGun seat-bone follow, the full UseGun matrix basis, and driver-lean. The local/authority UseGun root position now follows its live control-posed parent userpoint, matching `Entity_AttachToBoneAndUpdateTransform @ 0x5463d0` and its player/AI callers at `0x4b63c7` / `0x4bec23` (world-wac-ai-re §23.5). | A | OPEN (partial; UseGun root position fixed, generic/full-basis residuals remain) | PAR-WORLD |
| D-INF-3 | Ground/water resolver: the WATER residual narrowed 2026-08-24 (the §29.1 float channel is ported in BOTH local motors -- org1 `infantry_water_block`, org2 `player_water_block` + the swim selection 36-40, engine/runtime/world/infantry_water.cpp; still on this row: the wash overlay 27/28, the submerged scope auto-untoggle @0x4b8304, the resolver swim upward-force damping @0x4b304e, and the swim forward-speed question -- authored 2.88 u/s vs ~2.30 measured retail, no witnessed scale at the root fold) (the vertical capsule-bottom settle landed as D-INF-6; the horizontal capsule landed with D-COL; the LADDER tail landed 2026-08-15 with the D-COL-5 climb-motor port — world-wac-ai-re §30: entry/chase/exits, states 32–35, gravity/root gates, the view clamp + arms lock; residuals there: the AI climb-order writer, the parachute/carried halves of shared gates, the remote-climber authority display; the airborne overlay's PLAYER 31 leg landed 2026-07-16 §22 — org1 plain falls keep the clip by design (parachute-gated ladder), the 47 leg rides D-INF-20). 2026-08-22: the first-person consequence is visible in the registered CP01 water pairs — retail's swimming state (`Flags 0x8000`, entered in `Entity_UpdateInfantryPlayerBody @0x4b8130..0x4b8261`) raises the rifle above the waterline while the port keeps the hip hold; those pairs are not viewmodel-placement evidence (net-re §5.40 eighth pass) | A | OPEN (partial — water only) | PAR-WORLD |
| D-INF-5 | Idle look-at system + its spotting side effects — rides the combat pass | A | WITNESSED-READY-DEFERRED | PAR-WORLD |
| D-INF-11 | Third-person body aim overlay (torso bend) — witnessed in full + **LOCAL PLAYER PORTED 2026-07-08** (world-wac-ai-re §14/§14.6: libs/anim aim_overlay + leg-chase sim + eval_pose_overlay path, probe-verified); **upper-body weapon channel producer WITNESSED + LOCAL PLAYER FULLY PORTED 2026-07-09** (§14.8: entity secondary AnimMap channel +0x18C; reload 65/66 via the +0x372 80-tick window; the weapon.def `special_hold` kind ladder → hold poses 50–61 + scoped variants; `attack_anim` → the 62/63 fire stamps; the +0x371 arms-dip feed into pitchKickAccum; mask-bone hard override before the overlay compose; the `infantry` ctest's retail weapon-channel leg, rifle/pistol/knife); §14.3 pitch kick RESOLVED = the audio mixer output power meter `g_audioOutLevel` (port needs a reimpl mixer level tap); **2026-08-17: the secondary channel's RESET backfill, blend window, and per-entity variant rings are ported (D-INF-1 closed), and AI bodies now run the SHARED dual-channel advance** (`AiSystem::infantry_weapon_channel_advance` — promotion, playhead, blend — witnessed for both bodies via `AnimMap_UpdateDualChannels @0x40b8c0`; the org1 SELECTION writer stays unread, ledgered D-INF-24); remaining: the binoculars input toggle (ladder side ported), mounted/seated branches, attachments, and D-INF-24 | A | OPEN (partial — local + wire players landed incl. the full weapon channel; AI bodies advance but do not select) | PAR-WORLD |
| D-INF-13 | Body rigs still consume `.bad` channels as ABSOLUTE bone orientations with the bind-matrix skeleton rest; the original composes every clip's channels against the rig's ONE skeleton bind — the `.adm` slot-0 `.bad` pinned into `channel+44` at registration (`AnimMap_RegisterEntity @0x40bb60`; `AnimChannel_ComputeBoneMatrices @0x410da0` `Transpose(bind 3x3) × channel`; `build_world_bone_matrices @0x40c770` = same composed math as the FP `@0x40c400`). Identical output for healthy exports (channel-at-reset == bind). **CORRECTED 2026-08-17:** the earlier reading that "the faithful `model_bind` path engages for FP viewmodel rigs only" is wrong — `sample_clip(..., model_bind=true)` has NO production call site (every rig loader passes `model_bind=false`: `skeletal_anim.cpp`, `adm_skeletal_clips.cpp`); FP and body rigs run the SAME rest-carrying factorization, and `model_bind` survives only as the ctest reference implementation (`anim_sample`). So this row is not "port FP's path to bodies" — it is "prove the equivalence the loaders assert against `@0x40c770`, and settle the world builder's table + padding semantics", which needs an IDA grill of `build_world_bone_matrices @0x40c770` (the four questions: who writes the 108-byte entity bone table; whether the world builder has the FP's bone-0 padding loop `@0x40c5a1`; whether it consumes `@0x410da0`-composed channels; and its frame map vs our `(−x,y,z)` import flip). The part↔bone "matcher" question is RESOLVED: the original never matches — rows pair BY INDEX bounded by the model-side table (the FK never reads the `.bad`'s bone count/parents/positions), so 20-parts-vs-19-bones bodies need no map, just the body table semantics: `@0x40c770` reads the 108-byte entity bone table (`skeletonData+104` count, parent `@+40`, 16.16 fixed pivots `@+56/60/64` in (z,x,y) order with x negated, bind-inverse `T(−parent pivot)`, NO bone-0 padding loop) — port that table + a body probe pass to close. The pinning oracle must use body-shaped data where the two factorizations CAN disagree (a rig with more model rows than channel rows, and a clip whose frame-0 channel is not its own bind) — a healthy-export-only test passes for the wrong reason. **2026-08-22 (net-re §5.40 ninth pass) FP-side facts:** the FP matrix source is the WEAPON's model table only — `Player_RenderFirstPersonViewModel @0x4ded60` submits the gun AND the arms with ONE array built from `fpModel`'s table (`@0x4def59` → `@0x4df028`, arms reuse `@0x4df088`), so an arms model's own bone table is dead data in the FP path (IndoArms: 40 rows vs the rig's 42 anim bones, stale MESHLESS helper rows BN38–BN40 up to 1.7 u off the live rig — the predicted disagree-capable specimen, harmless by construction); the M16 full-clip sweep proves loader-vs-table equivalence exactly for that rig (0.0000 u over idle+reload, all variants, both parts), leaving the per-rig `gfx1`-table-vs-`.adm`-bind corpus check as this row's FP half | A | OPEN (grill of `@0x40c770` pending — IDA was unreachable 2026-08-17; the FP half narrowed 2026-08-22 to the per-rig table-vs-bind corpus check) | PAR-WORLD |
| D-INF-17 | Lean producer gate legs unmodeled: the on-foot ramp's `Flags & 0x100020` skip and the prone-roll selection's `0x10000/0x100000` legs (`@0x4b7da2/@0x4b7322`) gate on entity flags the port does not model (alive/prone/airborne carried instead); the seated (`+0x168==1`) ±0x1400000 ramp variant (`@0x4b66b5`) rides the mounting slice. The row covers BOTH integrators: the local producer (`AiSystem::infantry_lean_tick`) and the REMOTE/netsim one (`netsim::ClientReplicaPipeline::tick_lean`), which folds the same two `move_input` bits for decoded peers. The decoded row carries no honest stance/seat state, so the ramp gates and the seated variant are unmodeled there too; 2026-07-25 fixed only the ORDER on the remote leg — decay `lean -= (lean+8)>>4` `@0x4b5c97` BEFORE the ramp `@0x4b7dbf`/`@0x4b7dd6`, as in the single `Entity_UpdateInfantryPlayerBody @0x4b40e0` pass (equilibrium ±0x30000000, not the ramp-first ±0x2D000000) | A | OPEN (partial — on-foot ramp/decay + prone rolls landed 2026-07-13; remote integrator ordering 2026-07-25) | PAR-WORLD |
| D-INF-18 | FP eye tails: the local head-bone eye (the `@0x4b6bb3` bone-path translation, 0.125u floor, bone INDEX 14), the `−0x3000` view-forward pull-back (`@0x438001..0x438031`), full roll `torsoRoll + lean/4`, and exact `2·pitchBlend(+0x380)` camera recoil are ported; the recoil impulse/decay producer closed with D-WPN-34 on 2026-07-31; the FIVE-sample terrain floor (`@0x4b6c08..0x4b6ca4`, unless `Flags & 0x800000`) FIXED 2026-08-12 (#484 — `player_view_floor_eye_to_terrain`); the world-side below-water classifier's `CameraOffset.Z` projection FIXED 2026-08-12 — recoil/spread/stance and the HUD crosshair rows now compare `Position.Z + eye_offset_z` (the ported `entity+0x74`) with the water plane `[orig: RoundData_SpawnRound @0x4ec2d5..0x4ec2ea / @0x4ec342..0x4ec35a / @0x4ec86d..0x4ec885]` (`npruntime_round_sim` pins the eyes-dry/eyes-wet split; decoded wire rows keep the documented bounded projection — no CameraOffset carrier on the wire). Still open: remote capsule-trig CameraOffset (`@0x4b6984`). The 3P anchor chases the binding-sampled head-bone eye (`@0x437b70`); its render-skeleton sampling is one frame stale vs the original's sim-side bones | A | OPEN (narrowed 2026-08-12 — terrain floor + water projection landed; remote-eye residual remains) | PAR-WORLD |
| D-INF-24 | AI (org1) bodies never SELECT a secondary weapon-channel state. Retail's org2 selection ladder (`Entity_UpdateInfantryPlayerBody @0x4b5dad..0x4b5ea9`) is witnessed and ported for the local player and every wire peer, and the dual-channel ADVANCE is witnessed for both bodies (`AnimMap_UpdateDualChannels @0x40b8c0` from `@0x4b40e0` and `@0x4b9910`) and ported for AI bodies 2026-08-17 (`AiSystem::infantry_weapon_channel_advance`: promotion, playhead, blend). But the org1 body has its OWN secondary-state writer `@0x4b9a28` (world-wac-ai-re §14.8.4) which has never been decompiled, and a placed `.bms` soldier never receives an equipped ADM index (its source in the original is UNWITNESSED — every witnessed writer of `entity+0x2B0` is a player path, §13.5). Rather than run the player ladder on AI entities (a different function's behavior), an AI body's secondary channel holds whatever state its reset left it (idle → RESET-backfilled, i.e. the arms follow the primary — the observable retail appearance for AI bodies, whose `.adm`s carry `anim_reload` at most). Closing it needs an IDA read of `@0x4b9a28` + the AI equipped-ADM source; ctest `infantry::test_ai_weapon_channel_advances_without_selection` pins the current split | A | OPEN (blocked on research: `@0x4b9a28` + the AI `entity+0x2B0` writer) | PAR-WORLD |
| D-INF-20 | The parachute system (Flags 0x20) unmodeled: auto-deploy (authority, alive, `vel_z ≤ −14336`, aux `+0x2C & 0x10` `@0x4b7aef`), the in-air 47 variants (org2 +0x10 on its straight stamp `@0x4b7e3f`; org1's whole 47→31 ladder is parachute-gated `@0x4bf8d8` — plain NPC falls stamp nothing), the org2 sixteenth-step body chase + leg snap while chuted (`@0x4b494d`), the `@0x4b7b18+` descent block (unread) — ours never sets the flag; the player's jump/fall stamps 31 (world-wac-ai-re §22) | A | OPEN (parachute slice; descent block = NEEDS-RE) | PAR-WORLD |
| D-WPN-1 | weapon.def FUNCTION rows resolve through the `g_actionFuncDefTable @0x829E58` name registry (18 entries incl. the `*_map` scope variants + `powerup_*`); the port fixes each state's behavior instead — safe because every shipped row (JOX + REVX sweeps) names `wpn_std_<its own suffix>` (net-re §5.62) | A | WITNESSED-READY-DEFERRED (registry table witnessed; port when a non-std consumer appears) | PAR-WORLD |
| D-WPN-2 | Single-pool ammo model: the original tracks per-ammo-class carried pools (`Entity_GetScoreValueBySlotType @0x5406e0` class byte def+0xD8, units/round def+0xE0, pool caps `ammoclass_max_carry`); the FSM folds them to one rounds counter and the recoil auto-reload gate approximates units=1 (net-re §5.62) | A | OPEN (rides the ammo-class/pool port, with §5.57/§5.58 pool semantics) | PAR-WORLD |
| D-WPN-3 | `WeaponSlot_CanFire @0x541ba0` legs beyond the clip: busy weapon-child entity, the underwater-fire ban vs `Env_WaterHeightFixed`, the adm+224 score-lock; plus the kick bump's fire-sound-id gate (Def+0x294) — all need slot/env/sound state the port does not model yet (net-re §5.62) | A | OPEN (partial — the clip + reserve-routing leg ported) | PAR-WORLD |
| D-WPN-5 | Weapon-switch machinery — FULLY WITNESSED (net-re §5.62 switch-chain block): `Player_SwitchToWeaponByHandle @0x4e0170` category scan over `weaponSlotArrayBase @0xB75FD4` → `Player_MountWeaponSlot @0x4dfa40` writes `g_pendingWeaponSlot` + queues SWITCHRANK(8)/`ForceQueueSwitchFrom`(7); the switchfrom swap + `TryQueueSwitchTo`, the −901 instant paths when either Def has Flags 0x80, the recoil def+0x168 auto-switch, and mount-scoped auto-engage. The inventory switch path is ported. Local UseGun now follows retail's distinct branch: `Entity_AttachToUseGunSlot @0x546b80` saves the personal slot and passes parent `+0x2B4` through `Player_MountWeaponSlot`; detach passes the saved slot through the same path (`@0x43565f`); a gun-to-gun swap overwrites only the latest pending parent. SWITCHFROM uses the exact `TryQueueSwitchTo` predicate, while SWITCHRANK does not modify the committed target. The committed slot drives both the action FSM and FP model, and the borrowed parent's ammo/FSM state survives detach/remount. The retail render side is now witnessed too: `Entity_RenderVehicleModel @0x4407d0` skips its sole parent-model submit `@0x440918` only for the local first-person slot-3 parent when the embedded MountSlot `+0x2B4` is the live `EquippedSlot` and its Def has a resolved `fpModel` pointer at `+0x16C`; `Def.flags2(+0x0C) & 0x800` (Invisible) is the alternate force-cull leg without that FP-model/equipped-slot comparison. OpenNova re-derives the transient `PF_LOCAL_VIEW_SUPPRESSED` presentation verdict from the exact parent/camera/slot state and the host's actual FP gun-resolution result plus owning Def identity, so same-Def parent swaps reuse the model while different Defs cannot inherit it; zero-fixed-tick render frames still present the verdict. Authoritative hidden state, collision/simulation, and separately rendered attached actors remain untouched. Authored-but-unresolved or absent `gfx1` therefore keeps the world parent visible and never substitutes the bring-up AK; pre-commit, third person, and detach restore the world model immediately. 2026-08-05: the verdict was computed only inside the host-only registry-enrichment branch of the present builder, so a JOINER never suppressed and drew a mounted 50cal TWICE (its wire world model plus the FP model); the cull is now computed role-blind from the local mount state against the exact-handle materialized mount row — retail's render walk is role-blind [orig: @0x4407f6..0x44084c] (`wire_header_world_materialization_test`) | A | OPEN (inventory switching + local UseGun borrow/swap/restore and resolved parent-model cull landed 2026-07-20; joiner-side cull fixed 2026-08-05; broader loadout/network ownership residuals remain) | PAR-WORLD |
| D-WPN-6 | The local player's active slot and occupied mounted-parent slots are pumped; while locally attached, Simulation owns the borrowed parent slot and the global mounted pump skips it, preventing a double tick. NPC/remote mounted slots stay on the global pump. Its placement after entity updates and before rounds matches retail [orig: `Entity_UpdateAllEntities @0x52674b`; `WeaponAction_ProcessAllEntities @0x526786`]. Residual: non-local pool-0 equipped slots and eligible unmounted pool-1 weapons still lack the general pump (world-wac-ai-re §26.6/§26.8) | A | OPEN (mounted path and local borrowed-slot ownership landed 2026-07-20; general slot coverage remains) | PAR-WORLD |
| D-WPN-7 | Interim ammo seed: the FSM installs with clip=clipsize + reserve=startrounds from the def; the original resolves ammo through the PLAYER_INFO loadout + S2C 0x5A apply (§5.30/§5.57) (net-re §5.62) | A | OPEN (rides D-PLAYERINFO-1/-11) | PAR-WORLD |
| D-WPN-8 | FIRE-context residual: authority/listen-host and single-player fire append the round ring with the witnessed pre-consume-magazine mode byte `((clip & 3) << 4) \| 2`, the exact ordinary on-foot hip/ADS-raise/third-person subtype 12, and spawn `world::RoundSim` synchronously. The remote-joiner path predicts its own visual round, emits C2S 0x06 with the client-runtime `currentTick`, retail-rounded Yaw/Pitch high words, and the five modulo-u16 fire-pose deltas, completes payload-addressed reload through C2S 0x25 → S2C 0x49, and feeds decoded S2C tag-2 events into a visual-only client `RoundSim`. The listen host now also sends its reload through transport-mode-1 C2S 0x25 so the shared dispatcher broadcasts S2C 0x49 without refilling authority twice; parentSlot 3 names the authoritative mounted parent handle and derives its combo from the mounted Def. Remaining mounted-reload parity: a remote joiner has no authoritative local-parent → host-wire-H mapping, and host/client vehicle-slot 0x49 refill/application is not modeled, so the joiner-mounted producer stays deferred rather than guessing a handle. Remaining C2S-producer parity: OpenNova's resolved posed-eye/fallback origin does not yet reproduce retail's exact `Position+CameraOffset`, `Pitch+pitchBlend`, or mounted/scoped `Entity_CalcWeaponFirePosition` branches, the C2S `0x06` `hit_part` word (off30) shipped a BARE shot sequence where retail packs `(roster slot << 9) \| (seq & 0x1FF)` — **FIXED 2026-07-26**: zero slot bits named roster slot 0, which on a listen host is the HOST ITSELF, and a live retail co-op host attributed every round our joiner fired to its own player, so its first-person weapon reacted on our shots (same-weapon only; a retail↔retail pair never reproduced it). The host copies our raw word into the global `word_B7C670` verbatim on the network arm [orig: `Server_ClientFiredRound @0x50c2ba`/`@0x50c774`] and composes its own the packed way [orig: `@0x50bda5`]. Now built by `opennova::pack_fired_round_hit_part` from the S2C `0x04` body-byte-17 roster id [orig: `NetPacket_WriteSlotAssignment @0x502b30`], pinned by `nw_ingame_c2s_uplink_test::test_fired_round_hit_part_packing`; net-re §5.16. Separately `extra_byte1` (`entity+352`, C2S `0x06` off32) is STILL unmodeled/zero — **now witnessed live**: all six retail fired-rounds in `.scratch/golden/retail-coop-playerinfo-join.pcapng` carry `extras = (0x03, 0x0c, 0x00)`, so retail ships `0x03` where we ship `0x00` (net-re §5.66). The same capture CLOSES the adjacent suspicion about off34: retail sends `0x00` there too, so emitting zero on that byte is not a divergence. Remaining host validation: the `AdmDef[276]` cooldown/freshness stamp, savedLivePose compensation, and moving-carrier re-anchor. Presentation residuals: settled-FP/mounted zoom subtypes; remote/vehicle 0x49 presentation; per-weapon tracer metadata (shooter TEAM is no longer a residual — `ClientEntityState::team` is retained and consumed as of 2026-07-25); posed-bone person collision (player AND decoded-infantry proxies use the torso fallback); PANM/turret section posing and husk substitution for wire dynamic proxies, plus movement-contact/blink/LOS projection (those queries still lack complete replica contact/indoors state). Clean-disconnect proxy retirement and the 0x46/0x5D lifecycle fold are NO LONGER residual here — ported under D-NET-176 (FIXED 2026-07-25; net-re §5.62, §5.16, §5.58) | A | OPEN (joiner fire/reload + listen-host loopback relay + client tag-2 visual round + witnessed 0x06 tick/pose-delta producer landed 2026-07-22; decoded non-player Infantry/vehicle projectile-collision projection — wire-keyed person + authored-geometry dynamic proxies at the decoded pose with visual-client native/proxy de-duplication — landed 2026-07-23; bounded producer/validation/presentation tails remain) | PAR-WORLD |
| D-WPN-9 | ADS residuals: zoom-level adjust keys (`Player_AdjustWeaponZoomLevel @0x4dbcc0`), the scope-state C2S 0x1D notify, stance (parentSlot 2/5) + NVG gates, the mid-ease movement reversal + auto-re-raise legs (`@0x4df548`/`@0x4df5ae`/`@0x4df607` — need the engaged/active/hipfire tri-state), the HandGunUp (0x4000000) auto-follow leg (`@0x4de444`, player-flag writer unwalked), plus the adjacent Sighted-only crosshair distinction (D-HUD-9) (net-re §5.62, §5.41) | A | OPEN (landed: toggle/tpos-ease/FOV/rescope; exact standard SIGHTS-card selection for Scoped/Sighted, SWITCHFROM, NoCardSwitch and ForceScoped; authored-row materialization; 7-step Inset interp; unscope-on-move + scope-up refusal; ForceScoped toggle pin — the weapon round) | PAR-WORLD |
| D-WPN-20 | The def+0xDC ammo pass-type path (shared-pool "clip" weapons routed through `sub_5405F0`/`sub_540670` in the recalc, eligibility, and reload-input legs) is unmodeled — the port always uses the slot's loaded clip; ordinary infantry weapons never author the pass (net-re §5.63) | A | OPEN (witness the pass family before porting) | loadout grill (2026-07-18) |
| D-WPN-21 | The spawn rebuild commits the SELECTED slot and skips `Player_SwitchToWeaponByHandle`'s mount walk: the walk rides the entity+0x68 AI-slot binding gate (`@0x4e023a`, writer `Entity_AllocateAISlot @0x40d2f4`) whose value DURING `Player_InitPlayer` is unwitnessed — modeled as not-yet-bound, which matches the retail-observed spawn weapon (the walk would advance-first rank-cycle onto a sub-variant when one is loaded) (net-re §5.63) | A | OPEN (witness the gate's init-time value; flip the spawn to the full walk if it reads bound) | loadout grill (2026-07-18) |
| D-WPN-23 | **FALSE NAMING RESOLVED 2026-07-21.** Input case 220 is ToSpecial/QuickSwitch (catalog id 37, default F), not Binoculars: globals `0xB75FE0/4` hold the weapon.def `QuickSwitch 0x08000000` target/stash (retail JO uses it only on `WPN_MAG58_PointAim`). That hold-swap and the msg-0x38 server-confirmed pickup swap remain unported. True Binoculars is action 26 (default B) and is now ported: raw/effective state, movement/death/round/3P suppression, body/replication flags, 20-degree FOV, fixed-radius random aim displacement, input/fire/switch gates, VFS masks/crosshair, and exact four-digit range smoothing. Residuals are the charge-fire-start refusal (no charge-fire mechanic yet) and binocular-specific capture-point short-name/progress presentation. | A | OPEN (QuickSwitch/pickup plus the two bounded Binocular residuals; core Binocular view landed 2026-07-21) | retail Binocular/NVG parity |
| D-ITEM-1 | Ordinary item bullets now use CFAC face geometry with material and husk-model selection; the +533 refNum self-site gates are ported 2026-07-20 (item leg vs the ray[18] mount carrier `@0x4e4d40`, person leg vs the ray[17] shooter `@0x4e4688`; `collision` ctest). Remaining structural differences are retail proximity-slot residency, the pool-2 blast query's separate AABB/face refinement (world-wac-ai-re §24.7), The bullet person-leg impact tag is GRILLED and FIXED 2026-08-22 — split out as D-ITEM-21. The invented "building material 1 → 23 flesh" bullet remap is DELETED 2026-08-15 (IDA-refuted: `Projectile_HandleEntityImpact` passes `ray[22] + 4` unconditionally `@0x4e982b`; the 1 → 23 remap is the Knife PERSON leg's `@0x4e8880..0x4e8888`, re-witnessed under D-WPN-16) | A | OPEN (bounded broad-phase/blast residuals) | PAR-WORLD |
| D-ITEM-2 | `husk_swap_at`/`_sec` are parsed, but their runtime reader was not found; no progressive swap behavior is implemented (world-wac-ai-re §24.7) | B | NEEDS-RE (find the +0x19C/+0x1A0 reader) | PAR-WORLD |
| D-ITEM-3 | Blast-time breakable collision-section marking is unported; the collision model carries no per-section flag byte (world-wac-ai-re §24.7) | A | OPEN | PAR-WORLD |
| D-ITEM-4 | Death-piece trajectories are simulated, but presentation has trail effects only: no isolated husk-section mesh/spin or glow light, and one PRNG stream replaces retail's three (world-wac-ai-re §24.7) | A | OPEN (presentation fidelity) | PAR-WORLD |
| D-ITEM-6 | Blast/damage tails remain: organic knockback and hit emitter/sound, medic/ram queue legs, the MP destroy-buildings gate, and destruction wire emits. Ordinary bullet vehicle-occupant scaling is now ported separately (world-wac-ai-re §24.7; net-re §5.60); 2026-08-21: impact scars PORTED (world-wac-ai-re §24.9 — `world::ScarCache` with the BY-POOL ring selection, the ammo `scar_type` router + the def gate, `scar_add_entry` at the impact site and the knife leaf, the death + attach-time clears, `renderer::compile_scar_draws`, the `ScarPresenter` device); the one scar residual is the GLASS userpoint leg (`Terrain_SpawnSurfaceEffectsAtUserPoints @0x5cea90` over the 24-row table `@0x841980` — rows not witnessed in full, so such a hit takes the ring scar) | A | OPEN (the blast/damage tails + the glass userpoint leg) | PAR-WORLD |
| D-ITEM-7 | Destruction eligibility is inferred from kind/capabilities plus unitType; retail's def-class callback-column mapping remains unwitnessed (world-wac-ai-re §24.7) | B | NEEDS-RE (witness the class callback table) | PAR-WORLD |
| D-ITEM-8 | Crane/water-tower special destruction, 992-tick re-notify, and ambient phase-0 regional shot behavior are unported (world-wac-ai-re §24.7) | A | OPEN | PAR-WORLD |
| D-ITEM-9 | Falling/Generic wrecks and unitType-3's four short slope rays resolve terrain only; retail also returns object ground. Falling/Generic synthesize the upright sec0 rest extent from LOD-0 bounds and still lack the inverted extent leg; PiecePhysics uses the resolved `box_z_lo` correction. Static's separate terrain/water thresholds are ported (world-wac-ai-re §24.5/§24.7) | A | OPEN (object-ground query + generic inverted extent) | PAR-WORLD |
| D-ITEM-10 | Routed and specialized wreck water crossings now emit the resolved `Effect_MedSplash`, and all fallback sounds are ported. The authored per-item water (+156) and landing (+140) sound slots remain unavailable (world-wac-ai-re §24.7) | A | OPEN (two authored sound slots only) | PAR-WORLD |
| D-ITEM-11 | Shooter/mount exclusions landed, but the fourth projectile exclusion slot's server-side provenance is unwitnessed (world-wac-ai-re §24.7) | B | NEEDS-RE (walk `Server_ClientFiredRound @ 0x50baa0`) | PAR-WORLD |
| D-ITEM-13 | Exact static/dynamic CFAC hits, first-person-table semantics, current-pose COBJ bone sections, split reaction/death versus damage-zone routing, person effect backoff, FatBullets gates, attrib-0x200 seat x6, and the former inside-sphere miss are fixed. The 2026-07-20 phantom-bone fix stopped `finalize_sections` from minting a vertex-derived sphere on the authored radius-0 CFAC mesh row (every JO person model's trailing COBJ — a big shootable off-body sphere in F3). The radius-zero follow-up is **FIXED 2026-07-20**: person rays and F3 retain retail's extra+0xCCC floor at the raw mid, including the mesh-row primary ordinal on a dead-center shot. Residuals are terrain refinement/`.TIL` material input, remaining non-person post-hit parking details, and production animated organic pose publication (world-wac-ai-re §15.8b/§24.7) | A | OPEN (bounded residuals; radius-zero fixed 2026-07-20) | PAR-WORLD |
| D-ITEM-15 | Wreck effects collapse retail's four-slot bone banks to one origin group per family and one crackle roll per wreck; bone follow and underwater `Boat01Steam` are absent, and a particle kill plane is not equivalent (world-wac-ai-re §24.5/§24.7) | A | OPEN (presentation fidelity) | PAR-WORLD |
| D-ITEM-21 | GRILLED and FIXED 2026-08-22: the bullet person impact-effect leg. Correspondence established first — bullets reach a person only through the bone-section pass (`Physics_RaycastAgainstProximityList` → `Physics_RaycastAgainstBoneSections @0x4e4670`, sole caller `@0x4e4c59`) → dispatch case 3 → `Projectile_HandleTerrainImpact_0 @0x4e98f0`, an auto-namer misnomer that is the person handler; the two proximity-slot cases walk pool 2 and pool 1 only (`Entity_BuildProximityLists_Pool01 @0x4b9340`) and cannot produce a person hit. Ported from it: a dead victim presents nothing (`@0x4e9920`); the LOCAL player takes tag 2 (`@0x4e9aa1`); every other person takes tag 23 (`@0x4e9ad7`) but only when its group differs from the local player's (`@0x4e9aac`) or it is below half its items.def hp (`sar dx,1 @0x4e9ac6`, `jge` skip `@0x4e9ad0`) — a healthy same-group victim shows no impact effect. Ours previously emitted tag 2 for every person collision. RESIDUAL, the only open part: the ADDITIVE body-armor leg `@0x4e99f8`..`@0x4e9a38` (tag 24 when the hit zone `hitContext+0x80` is 0..4 unsigned and `victim+0x2C & 8`, the carry bit from weapon def `flags & 0x1000` — `WeaponSlotTable_LoadAllFromDefs @0x5415ac`) is unported because `WeaponInventory::carry_flags` has no per-entity mirror; it needs an `Entity` field plus a host/wire feed (world-wac-ai-re §24.7) | C | OPEN (body-armor tag-24 leg only; the tag 2/23 split and its suppression are ported and pinned) | PAR-WORLD |
| D-WPN-25 | The ordinary stock-ballistic path now matches the recovered pre-force sweep, 167-Q16 gravity, aerodynamic drag table/rounding/water/stability gates, live pre-arm dud substitution, MP authority/OneShotKill, exact signed-wrap kinetic arithmetic, shooter class, one carrier hop, ItemDef/impact-armor/dead/NoDie gates, person/seat zones plus their critical flag, and vehicle occupant reduction. Dud substitution is no longer an impact-row-only approximation: the active logical child preserves owner/kinematics/elapsed age from the witnessed 692-B prefix copy, starts at contact under the resolved dud ammo/max-age, and does not inherit the +692 trail slot. Its distinct retail pool-3 identity, same-frame allocator visitation, and copied fields absent from `LiveRound` remain bounded structural gaps; the in-slot projection first advances next tick. Other residuals: randomized threshold-crossing tumble needs the retail PRNG/local frame; the remaining non-throwable `useownmove` classes need their callbacks/guidance (the witnessed `nade`/`schl`/`clym` motors are ported and tracked by D-THROW); impact-energy `armor_density` deceleration, explosive/AoE, bounce, and shell physics are separate; production animated COBJ poses are not published; peer callback globals are not modeled separately from impact presentation; float `LiveRound` carriers can lose Q16 low bits at large magnitudes; the one-hop damage rollup and the Gunner `ray[19]` exclusion read our ground/carrier reference where retail reads the item's +40 attach parent (items carry no attach field in our model — the occupant COUNT was moved onto the rider `mount_target` channel 2026-07-20); and the `water_z != 0` guards on the ordinary stall/underwater-drag legs are reimpl-model gates retail lacks (retail compares `Env_WaterHeightFixed` raw, semantics of its no-water value unwitnessed; the separate throwable no-water sentinel is fixed under §27) (net-re §5.60). | A | OPEN, bounded residuals around a ported ordinary bullet core | projectile retail alignment (2026-07-19) |
| D-WPN-28 | The overheat level and both dedicated `HEAT_GLOW` writers are ported in their witnessed local/authority scopes; two presentation residuals remain. The **particle emitter**: above `def+0x374` (`heat_effect`'s threshold) the pump normalizes `(heat − threshold)/(0xFFFF − threshold)` to a 0..0xFFFF fraction, resolves the OVERHEATED(11) action row's muzzle bone through `Entity_ComputeWeaponFireTransform`, and spawns (handle -> `MountSlot+0x1C`) or per-tick re-aims/re-intensifies an emitter carrying that fraction in descriptor +40/+44, releasing it when heat drops under the threshold, the window lapses, or the owner submerges `[orig: @ 0x54109E..0x54122C]`. Shipped data authors `FX_OVERHEAT1` on the emplaced .50s/miniguns/DShK/turrets; the shell effect seam is unported. The world writer is not a blanket render callback: `HUD_CacheWeaponSlotInfo @0x440930` is called only at `Entity_AttachToBoneAndUpdateTransform @0x546518`, caches the parent carrier's inline MountSlot for a valid UseGun child relation, owns cold zero, and caps hot values at `0xFFFF`. OpenNova reproduces that scoped parent PANM/collision/presentation state for authority/SP/listen and wire-direct rows. Compact joiner rows carry neither the attachment heat window nor enough state to reconstruct it, so remote carrier heat remains unavailable (D-3DI-2). The sibling first-person writer at `0x4DEEC2..0x4DEEF5` `[orig: Player_RenderFirstPersonViewModel @ 0x4DED60]` preserves exact `0x10000`. Zero shipped `weapon.def` occurrences of `ctrlreg` rule out only the generic ACTION ramp, not these writers. | A | OPEN (particle emitter + compact-joiner heat reconstruction; authority world and first-person CTRL writers FIXED 2026-07-29) | PAR-WORLD |
| D-3DI-2 | The complete 96-slot catalog, loader remap/unknown→0 alias, signed slot layout, PANM/material/light consumer math, and raw `>0x70` OED export preservation are ported. Full bus fidelity is not complete: retail owns one process-global persistent 96-slot array, so an unwritten slot retains the last value from an earlier model draw; OpenNova's retained models currently construct zero-based per-model evaluation arrays and snapshot only values written for that model. Cross-model draw-order persistence and exact frustum-submission timing are therefore unavailable, and the small owner tag used by retained presentation is teardown bookkeeping rather than a second value stack. Random waveform lifetime is also partial: retail builds the waveform table with 256 calls to the same process-wide CRT `rand()` used later by PANM/material/light and unrelated engine systems, then submits every model's PANM before the later sorted material flush. OpenNova uses the same MSVC formula only for ported waveform consumers and precomputes the table; it now preserves PANM-before-material order inside a model and re-samples noise PANM per instance, but cross-system seed/call order and retained-frame interleaving remain different. Producer coverage is also incomplete: the generic ACTION `ctrlreg`/`ctrlreginc` parser and 30-slot animator can address nonzero ordinals 1–95 but remain unported because the dormant retail updater seeds its numeric accumulator from a live `MountSlot *` address and no shipped audited weapon row authors the key; substituting clip/action phase would be invented behavior. The complete bus-xref census finds dedicated writers at ordinals 3–10, 14–36, 41, 46–47, and 52–95. Exact value/state projections for **8, 54–56, 61–62, 71–72, and 91–95** are hosted in bounded semantic scopes, leaving exactly 65 dedicated ordinals unported: **3–7, 9–10, 14–36, 41, 46–47, 52–53, 57–60, 63–70, and 73–90**. The complementary 18 ordinals **0–2, 11–13, 37–40, 42–45, and 48–51** have no dedicated writer in that audit; together the 13/65/18 partition accounts for all 96 names enumerated in 3di-gp-format-re. Compact joiners still lack SPECIAL1/2 phase, attachment heat, and cveh steer/speed source fields; those values are not reconstructed from transforms. The format writer preserves raw PANM 114–117, but ONED no longer offers model authoring under ADR 0037; retail OED UI parity is outside the product contract. | B | NEEDS-RE / OPEN (recover each dedicated writer's arithmetic and lifecycle; reproduce the session bus and global CRT/draw-flush order at the render seam; implement joiner transport only from witnessed fields; never synthesize generic ACTION phase) | PAR-WORLD / 3DI CTRL producer audit |
| D-WPN-32 | Third-person held-weapon rendering (increments 1-3 landed: the local player and every remote player render their held weapon, both attach frames). Remaining scope in the disposition; full implementation history: world-wac-ai-re.md (the D-WPN-32 transplant section) + git history | A | OPEN (increments 1-3 landed: local + every remote player, both attach frames; AI and the sibling NVG/binocular overlays remain) | PAR-WORLD |
| D-WPN-35 | Every recoil body path runs the exact `PRNG_Next16` recurrence and consumes one unconditional draw per person in its body-pass order before applying the parity-selected `±half` yaw drift; decoded remote people also retain the required full sub-byte heading. Whole-process stream identity is nevertheless not closed. Retail's `dword_31BFBB0` is shared with unrelated systems, while authority/local/AI recoil draws live in `AiSystem::prng16`, throwable consumers have another state, and decoded rows use a `ClientReplicaPipeline`-local BSS-zero mirror. Those separated owners can diverge from retail's cross-system call history, so a given session's recoil yaw signs need not match. This is bounded orientation drift only: recoil `R`, pitch drift, projectile/HUD spread, impulse selection, and the deterministic shot hash do not depend on that sign. `[orig: PRNG_Next16 @0x6130a0; Entity_UpdateInfantryPlayerBody @0x4b40e0]` | B | OPEN (route every witnessed consumer through one engine-wide PRNG owner; recurrence and subsystem draw order pinned by `infantry` and `netsim_client_replica_pipeline_recoil`) | retail recoil/spread parity |
| D-EVT-1 | Spawn-point activation on fire: fully witnessed (POI/deploy list `0xB76570`, marker @0x452ce0, +0x210/+0x217/+0x218 authoring) — rides the deploy/POI subsystem port | A | WITNESSED-READY-DEFERRED | PAR-WORLD |
| D-EVT-3 | Every trigger category evaluator is ported: alert 3/14, health 6/9/12, holding sub-11 over `Entity::mounted_child`, and raw-positive chain/distance/LOS 42–45. The flag-family carried-link writers are live end to end through the resolver's exact first-pass MoveCB/non-Powerup contact stream and `world::Match` (pickup, death/drop, save, capture/reset/removal), host S2C 0x2F/0x12 routing, and the joiner folds `[orig: Entity_MovementCollisionResolver @0x4B2F90..0x4B2FF5; Entity_ProcessWaypointInteraction @0x4AD820; NapiNPClientMsg_0x02F @0x430E10]`. Remaining holding producers are generic non-flag carryables and savegame restore. The 44/45 endpoints use the host-stamped bbox center, with the effective entity/def scale applied via the exact signed-Q16 `+0x8000` fold landed with D-COL-3; residuals there are the ≤20u entity-aware/terrain-only walker split and sub-45 yaw exactness. ctests `collision`, `event_runtime_bms`, `match`, `npruntime_round_end`, `netsim_client_world_materializer` | A | OPEN (exact flag producers CLOSED 2026-08-23; bounded generic-carry/save and LOS residuals remain) | PAR-WORLD |
| D-AI-1 | The class-driven pool walk is PORTED 2026-08-13 (§16.2/§16.2a): `acquire_target` runs the witnessed four-slot class walk (`+40+4*slot` class ids gated by the `+80+4*class` priority words — the `.aip` `priority_air/ground/organics/decorations`; class 0 = pool 1 helo-brained then pool 0 Player-flagged, 1 = pool 1 non-helo, 2 = pool 0 non-player, 3 = pool 2, incl. the case-3 `target_vehicles` inherit quirk), the priority-descending `+40..+52` sort (`AIProfile_LoadOrFind @0x45fd80` qsort, seeded at promote), the brain[37] priority-target feed (the row's old "profile+148" wording was a slip — the priority target is brain+148; profile+148 is `primary_weap`), the per-candidate engage caps = items.def `radarsig`/`heatsig` (`def+376/+378 -> entity+422/+420` `[orig: Entity_InitFromModel @0x40e136]`, stamped at the item-traits sweep), the `g_spawn_success_gate` round-end entry gate, and LOS-last as a lazy probe (the eager per-candidate rays were themselves a divergence). The zero-priority no-scan outcome retired promote's `flags100 \|= 2` transport stand-in — an unresolved `.aip` is retail's memset-0 record. ctests `ai` + `promote`. Residuals: the brain[37] live gameplay PRODUCER (only the savegame restore `@0x45dbb3` + release clears witnessed; an indexed-store sweep is the follow-up), the `dword_24C1930 & 0x800` MP-rules wire (`World::ai_rules_skip_local_player` seam defaults clear), the death/spectator half of the retail entry gate (net-side), and the adjacent variant A `AI_FindBestTarget @ 0x465a50` (§16.5 item 7, unwitnessed) | A | OPEN (class walk/priority/caps PORTED 2026-08-13; bounded residuals above) | playability P1 |
| D-AI-2 | The state-17/18 SM rows AND the turret fire solver are ported: enters (2026-07-16), the state-17 tick structure (§17.6), and — 2026-08-12 — `Entity_ComputeWeaponFireTransform_0 @ 0x456980` (the record's old 0x455b30 cite was a slip; full digest §17.9) as `AiSystem::solve_weapon_fire_transform` plus the three fire legs (`sm_weapon_fire`: stationary RC_FIRE behind the commanded guard byte +785, continuation, mobile solve+scatter+fire), the `.aip` GROUND weapon blocks (§17.9c, `aip::parse_profile`), and the WAC AI commands 0x15/0x16 — uncrewed SM vehicles/emplacements now spawn rounds through the authoritative path (`ai` ctest `test_sm_turret_fire`). Residuals, cited at the port sites: SM muzzle bone lists (the fill loop's name selection is unwitnessed — `Entity_InitVehicleAI @ 0x4603ba..0x460414`, capped 16 — so the empty-list leg always runs; the current-pose refine primitive now exists as `IMuzzlePoseProvider::resolve_userpoint_transform`, 2026-08-26), the yaw-only solve frame (no entity pitch/roll), the HELO SM + HELO `.aip` key set, the remaining ChangeAI command family (the shared queue producer plus events 6..11/21/22 were ported 2026-08-30; internal event cases 12..20 and the FIND_AND_USE/HUD/teammate/node-path/TARGETSSN owners remain), the CTRL diagnostic globals (D-3DI-2), `AI_GetSuspensionFirePoint @ 0x456860` consumers, and the §17.7 items 8/9 (saved-delta consumer, `Weapon_FireProcess` mount-frame compose). The attached organic emplacement path was already FIXED 2026-07-20 (§26) | A | OPEN (solver + fire legs PORTED 2026-08-12; bounded residuals above) | playability P1 |
| D-AI-4 | The infantry combat pass is ported (2026-07-16, `AiSystem::infantry_combat_think`/`infantry_fire_pass`): 32-tick staged perception (calm half-range, 4-phase schedule, lastAttacker fallback, slot[3]+aimPoint+damageTimer commit, priority-mark decay), the attack-anim reactions (155–158/165/166 by distance/health/hit + post_attack 151), approach/hold move modes, reload (anim 65 + magazine), lead + sawtooth aim error, the walking-fire latch, and the `.bad` anim-event fire into the ring + RoundSim. The 2026-07-20 parity pass added direct `AiSlot[1]&0x200` Berserk friendly filtering [orig: `Entity_FindTargets @ 0x53a7ea-0x53a824`] and the actual-hit move/group + wasHit/timer/attacker chain [orig: `Entity_HandleDamageTrigger @ 0x407310`; `Entity_OnDamageReceived @ 0x4af800`]; retail near misses remain listener-only [orig: `Projectile_UpdatePhysics @ 0x4ea99a-0x4ea9f2`]. Residuals: nearest-first scan penalties (corpse/drowning/far ×2), fresh-corpse (≤16-tick) targets, forced-target words, cover-seeking (`ai_find_cover_position`), retreat/board modes, and the §4-item-13 idle look-at stay unported. The 2026-08-28 pass ported the pre_attack 152 reaction, the cover_idle/cover_run 163/164 hit flinch with the think-cadence wasHit consume, the unconditional moveTimer stamp and the no-reaction fall-through into the approach arm, the command-126 and target-ground approach gates (`Entity_CheckGroundHeightAtPosition @ 0x4aff70`), the alerted-idle predicate (49 with a slot[3] target, wasHit not a term), the availability-gated wounded gaits, the same-think waypoint step on a 0 cooldown, and the flt_7C19E0 sqrt clamp on the waypoint distance | A | OPEN (friendly/hit reactions fixed; remaining behavior residuals tracked) | playability P1 |
| D-AI-5 | The anim-fire weapon bytes' load-time writer is unwitnessed: `entity+0x358..0x35B` / bones `+0x365..0x367` carry the items.def `ammo_closeattack/easyrocket/advancedrocket/marker3` + `launchups_*` ids (def source witnessed, `ItemDef_ParseProperty @ 0x4a1823 -> def+0x56B..+0x5FB`; JO riflemen author all four = the rifle round) but no per-field instruction writes the entity copies — a struct block-copy (world-wac-ai-re §17.7 item 1). The port seeds ONE ammo id + `clipsize` per NPC (`AiProfile::ammo_primary/clip_size`): the host seed is WIRED 2026-07-16 — `Simulation::resolve_ai_weapons` (after `load_ammo_table`) resolves each AI entity's items.def `ammo_closeattack` name (`def+0x56B`) against the mission ammo table and stamps the profile + the spawn magazine (`clipsize` `@ 0x49fa1c -> def+0x894`; word `entity+0x35C` reseed `[orig: Entity_ResetToSpawnState @ 0x4b97a9]`). Residual = the single-ammo stand-in itself (four weapon bytes + `launchups_*` bones collapse to one id) until the block-copy is witnessed | A | OPEN (host seed wired 2026-07-16; residual = witness the copy site, model the four-slot family) | playability P1 |
| D-AI-6 | Fire origin + concealment stand-ins. **The fire origin is pulled at fire time from the sim's own pose (2026-08-26, retail's synchronous call-site computation):** the present-pass muzzle push, its per-entity stamps (`Entity::posed_muzzle_*`, `AiEntity::muzzle_*`, `set_ai_muzzle_world`, `kMuzzleFreshTicks`) and the 0.9 u chest lift are DELETED. Persons: `AiSystem::weapon_fire_origin(World&, ...)` asks `World::muzzle_pose_provider` (`SimCollisionPoseProvider::resolve_muzzle_pose` — the composed clip/blend/aim-overlay pose of the current tick, the launch bone's FK prefix, the def's `launchups_closeattack` userpoint: the in-sim `Entity_GetAttachmentWorldPosition @ 0x4b2670`), else the RAW entity origin (`@ 0x4b2767..0x4b278e` when index 0 / no model / no table). Mounted UseGun shots: `pump_mounted_weapon_slots` fires from the EMPLACEMENT's posed fire userpoint along that bone's euler (`Entity_CalcWeaponFirePosition @ 0x4dc750` parentSlot 3 -> `Entity_ComputeUserpointWorldTransform @ 0x545c60` -> `Userpoint_ComputeWorldTransform @ 0x56c420`: `resolve_userpoint_transform` = entity placement matrix (+Q16 scale) x the sim-clock PANM part pose x the record position, euler via `collision_matrix_to_euler` = `Math_FixedPointMatrixToEulerAngles @ 0x613310`), the slot's FIRE field b (the fire tick's `@ 0x545d17..0x545d3f` select; `Server_ClientFiredRound @ 0x50c1f4` names field 0), byte 0 / no model = the parent's raw position/euler (`@ 0x545e1f`). The twelve `weap[lr][bmc]up[2]` items.def keys are parsed (def+0x61B..0x6CB) and resolved on the entity model into `Entity::weapon_userpoint_bytes[4][3]` (`Entity_InitBoneReferences @ 0x441470` slot map + `Entity_ResolveBoneUserpoints @ 0x545940` zero-fill; `weapon_userpoint_byte` = `Entity_GetWeaponSlotByte @ 0x5459c0`). The aim-error global stays ported (2026-07-20). Residuals: the aim/LOS origin's PERSON leg (`Entity_ComputeWeaponFireOrigin @ 0x43b4b0`: `pos + entity+0x6C` — the +0x6C writer unwalked) still substitutes the posed muzzle; its non-person leg is PORTED 2026-08-26 (`AiSystem::weapon_aim_origin`: the def TARGET userpoint def+1350, resolved from the hardcoded name at `Entity_InitFromModel @ 0x40dd04`, through the placement matrix `@ 0x43b5d4..0x43b5f6`, else the collision-bbox center entity+0x1FC `@ 0x43b619`, else the raw position `@ 0x43b54f`; both mutual-LOS ends and the aim TARGET point ride it); the weapon-def userpoint (+0x333 <- weapon.def+856) and the person +0x4D8 cluster are not carried; the AI fire pitch adds `inf.recoil_pitch` (unwitnessed on the AI path); the `ItemDef+0x144` pre-eval hook and the SM bone-list filler (`Entity_InitVehicleAI @ 0x4603ba..0x460414`) are unwalked; the prone-in-foliage `+40` accuracy penalty is skipped (needs a foliage-mask seam; `Foliage_SampleFoliageMapMask @ 0x606620`). | A | OPEN (fire origin exact in-sim at fire time 2026-08-26; residuals = the +0x6C person-leg walk, the +0x333/+0x4D8 clusters, the recoil term, foliage concealment) | playability P1 |
| D-AI-7 | The LOS raycast is ported (2026-07-16 session 4): `Physics_RaycastTerrainAndSectors @ 0x539910` witnessed in full (world-wac-ai-re §18.5) and `line_of_sight_clear` now rides `CollisionWorld::raycast_clear` — the terrain leg via the ported heightmap raycast (`terrain_raycast_refined`, the `@ 0x60e710` sibling of the witnessed `@ 0x60c760`; boolean-equivalent with a null out-hit) with the both-INDOORS skip, plus the sector leg (pool-2 statics then dynamics, exclusions incl. the +0x28 owner link, bound-sphere broad phase, TYPE-1 convex clip via `collision_raycast_model`). Endpoints are `AiSystem::weapon_aim_origin` at both ends (`Entity_ComputeWeaponFireOrigin @ 0x43b4b0` x 2, 2026-08-26: persons the posed launch userpoint else the raw origin — the `+0x6C` leg stays the D-AI-6 residual — modeled non-persons the def TARGET userpoint else the bbox center). NPCs no longer see through buildings (`collision` ctest `test_raycast_clear_los`). Residuals: the itemDef type-3 person sphere-block w/ same-team 3.0 u exemption (person-kind residents of the walked pools don't exist in our world — organics are pool 0, unwalked, like retail), the ray-radius arg (LOS passes 0), and the `@ 0x60c760` sibling's internal delta (terrain-re open item). The `Flags & 4` destroyed-HUSK collision-model swap CLOSED 2026-07-17: husk instances attach beside the graphic (CollisionWorld::assign_entity_husk, binding-built from the def `husk` model) and every query resolves through the one swapped target_view seam (world-wac-ai-re §24.6) | A | OPEN (the collision leg is LIVE 2026-07-16; husk swap CLOSED 2026-07-17; residuals = the type-3 person case, the 0x60c760 sibling delta) | playability P1 |
| D-AI-8 | Fire-presentation stand-ins (world-wac-ai-re §18, ported 2026-07-16 session 4 as `RoundSim::fired` + `Simulation::drain_fire_presentation_events` + `fire_present_pass.gd`): (a) CLOSED 2026-08-12: the tracer cadence byte is per-WEAPON-SLOT (`WeaponSlotState::tracer_shot_counter`, retail `weaponSlot+0x80` `@0x4ec199`) — the local player's fire passes its active slot's byte via `RoundSpawnParams::tracer_counter`, so each weapon keeps its own phase across switches; slot-less NPC fire keeps the entity stand-in byte (one modeled weapon per NPC, D-AI-5); (b) the fire-sound max-range gate runs at PLAY time (the bank's cull) vs FIRE time (`soundDef+72` @ 0x528ec6) — differs only when the listener moves during the propagation delay; (c) CLOSED 2026-07-18: the tracer-pool emitter system is witnessed + ported in full (world-wac-ai-re §25 — `g_TracerEmitterPool @ 0x2BF5270`, the 12 style blocks, `CEffectChannel_AppendPoint @ 0x5db290` per-tick pre-move appends, `CEffectEmitterPool_Tick @ 0x5db830` drain, `CEffectChannel_RenderRibbon @ 0x5db8a0` camera-facing ribbons = `engine/runtime/world/tracer_trails.{h,cpp}` + `RoundSim` + `fire_present_pass.gd` `get_tracer_trails()`); the render residuals moved to D-AI-12 (smoke wave anim, distortion pass, round graphic models, light_move glow, NVG laser, fog-to-black approx); (d) ROUTED 2026-08-16, owner-isolated 2026-08-18: the MF_Light muzzle glow (`Entity_UpdateMuzzleGlowEffect @ 0x56c960`) presents through the D-RLIT-4 light pool (`fire_present_pass.gd` → `EffectLightDirector.on_muzzle_fire`: spawn-once per shooter, re-armed mode 4/5 per shot, owner = shooter wire handle) and, with per-draw owner selection live, lights only its shooter's draws as retail does. The +40 value's consumer stays unwitnessed; (e) CLOSED 2026-08-29: the MP NoTracers rules bit (`dword_24D1E34 & 1`) has its sim seam (`RoundSim::no_tracers_rule`, forcetracer bypass ported) and the host session now wires the advertised `mp_attributes` bit 0 into it beside the claymore rule (`host_session.cpp`; host-authority side only — the joiner's replica presenter carries no tracer gate of its own) `[orig: g_rules_flags @0x24D1E34 & 1 at RoundData_SpawnRound @0x4ec41f; admin set @0x405c80; published as the lobby "Tracers" key @0x4fee4f]` | A | OPEN (b only; a closed 2026-08-12, c closed into D-AI-12, d closed with D-RLIT-4, e closed 2026-08-29) | playability P1 |
| D-AI-9 | Death-presentation residuals (world-wac-ai-re §19, ported 2026-07-16 session 5 — the kill's `RoundSim` anim selection, the `tick_infantry` death edge + corpse block, rows 21/23, `deathtime`/`LeaveCorpse` def traits, `mission_present_pass` corpse visibility): (a) **bullet bone routing FIXED 2026-07-18**: `Physics_RaycastAgainstBoneSections @ 0x4e4670` supplies the current-pose reverse-scan primary COBJ ordinal, `RoundHit` preserves it, and kill-time `compute_death_anim_state` consumes that bone plus the incoming-round quadrant; the explosive-person path's bone 1 remains because retail itself hardcodes 1 `@ 0x4e6ac7`; (b) the death SCREAM CLOSED 2026-07-17: the sound-profile chain is parsed + ported (slots 7/8 via `Entity_GetProfileSlotSound @ 0x528300`, night = EnableNVG; audio doc §sound-profile, D-SND-10..15); (c) despawn maps `Entity_Destroy @ 0x43e810` to `Entity::hidden` (our registry keeps the slot) and the SP watch-check gate maps `!is_in_session` to "a local player exists", with `AiSystem::weapon_aim_origin` as the LOS endpoints (the `+0x6C` person leg is the D-AI-6 residual); (d) **bodyRoll + DISMEMBERMENT PORTED 2026-08-15 (corrected to the 2026-08-19 re-witness)**: authoritative bullet kills write the exact quadrant-0/2 `bodyRoll = ±0x5B05B00` into the AI body roll (the `+0x94` store the slope pass chases for grounded corpses `[orig: @0x407564/@0x407575]`) and apply the `(1 << bone) \| addend` mask table `[orig: @0x407601 + @0x407608]` behind the retail gates; the victim and the host-only pool-0 clone take complementary section masks, and the clone starts at health zero with the witnessed `roundVel X/Y >> 8` horizontal kick `[orig: @0x4076b7/@0x4076c9]`. Two deliberate crash-guard divergences from the original's unchecked clone: the victim mask commits only after a successful allocation (retail commits first and faults through a null clone on pool exhaustion), and the clone ticks an independent AiEntity copy seeded to the victim's pose/anim state instead of sharing the victim's anim-slot pointer. The wire story is WITNESSED CLOSED 2026-08-19: the clone rides every NPC-corpse channel — the 0x0A priority walk admits any live pool-0 slot with a def class tag `[orig: Server_BuildEntityPriorityList @0x50e6cb-0x50e6da]`, the join 0x0C batch serializes every pool-0 slot (sole caller @0x51bd24), and connected joiners self-heal it via C2S 0x0F → S2C 0x18 (net-re §5.46) — while NO organic record carries sectionMask +0x134, so remote views render whole-body corpse pairs and the mask split is host-local presentation. Our host now streams the clone through `snapshot_world`/`build_pool0_organic_batch` identically; the one deliberate wire divergence is the clone's cleared net_id/name (retail memcpy-duplicates the victim's; our ascending find_by_net_id/find_by_name would misroute scripted refs to a lower-slot clone under our forward-scanning allocator). Ammo `secondary_anim`/`kz_physics` now parse/bake at +224/+225, but the exact `Entity_ApplyCollisionForce` interior remains open. Other unported edge/corpse legs are the +0x134-bit0 silent-cleanup variant, incendiary ammo+72 → 173 override/emitter, the 186-tick `particledeath` decay effect, +0x35E NPC respawn, medic-drag follow (anim 139), and drowning 175 (swim flags unmodeled); (e) rows 21/23 settle through the shared production-mode destruction pass, including AI-capable entities; the row-handler counter remains diagnostic, while the def+1352 child-kill loop and attrib-0x40 wreck-respawn watcher remain visible stubs; the `Entity_UpdateDeathTransforms @ 0x494660` presentation (husk swap Flags\|=6 / `Entity_SpawnDeathPieces` / death sounds) is PORTED 2026-07-17 — rows 21/23 now run `entity_update_death_transforms` (engine/runtime/world/destruction.cpp) and dead non-organics render their husk instead of hiding; the destruction residuals moved to world-wac-ai-re **§24.7 D-ITEM-1..20** | A | OPEN (gore/roll closed; corpse, exact force, incendiary, and D-ITEM-1..20 residuals remain) | playability P1 |
| D-AI-10 | One `world::Match` owns every retail competitive win branch and primary score: DM/TDM, KOTH/TKOTH, S&D/A&D, CTF, FlagBall, Flag Me, A&S/C&C, plus the universal all-zones-owned arm; WAC/BMS owns the co-op result edge. It also owns objective proximity (the literal player-slot mask, numbered-over-6006 precedence, three presence counters, TKOTH hold and six kill bonuses), exact collision-fed flag transitions, death-fed demolition target scoring, zone scoring, live S2C 0x16 projection, the frozen scoreboard, S2C 0x61/0x1D + C2S 0x2B/S2C 0x56 transaction, and phase-exact 2790-tick linger. Shared defaults/score.ini drive gameplay and S2C 0x58; Flag Me deliberately preserves retail's invalid row 12. `[orig: Entity_ApplyWeaponDamage @0x4E6820; Entity_MovementCollisionResolver @0x4B2F90..0x4B2FF5; Server_UpdateCaptureZoneProximity @0x5086A0; Server_CheckWinConditions @0x51AD40; GameEvent_ProcessScoring @0x52F550; GameType_CreateDefaultSettings @0x52DD00; Server_ProcessRoundEnd @0x5164F0]` Residuals are the proximity pass's hidden-selector generic score callbacks, SP-only tallies/presentation/music, score events whose gameplay producers remain outside the enumerated mode paths, and the SP gate's `world.mp_session` approximation. | A | OPEN (all MP mode decision branches plus exact flag-contact and demolition-death producers CLOSED 2026-08-23; shared/event-producer residuals remain) | playability P2 |
| D-AI-11 | The USE-ITEM mount/ride/drive chain remains ported from §23. The 2026-07-20 pass closes the attached-organic combat half: UseGun attachment owns the parent's embedded weapon slot [orig: `Entity_AttachToUseGunSlot @ 0x546b80`], mounted aim/request gates and the post-entity action pump now fire from the mount muzzle with the organic owner [orig: `Entity_UpdateInfantryAI @ 0x4bef57-0x4bf59e`; `WeaponAction_ProcessAllEntities @ 0x526786`], the 8-tick collision resolver retains callbacks but suppresses mounted model push [orig: `Entity_UpdateInfantryAI @ 0x4bf5a5-0x4bf5c6`], and death detaches before consuming the directional animation [orig: `Entity_UpdateInfantryAI @ 0x4b9c57-0x4b9d52`] (world-wac-ai-re §26; `ai`/`collision` ctests). The local/authority UseGun root position now also follows the live parent pose, matching `Entity_AttachToBoneAndUpdateTransform @ 0x5463d0` and the player/AI callers at `0x4b63c7` / `0x4bec23`; the asset-gated 00TRc E50triB regression proves the articulated case. Joiner C2S 0x26/0x27 attach/detach, world-aware mounted C2S 0x0C carrier-local pose, and requester-local S2C 0x0A relationship confirmation are live. Residuals: registry/chest-eye USE scan; emplaced-carrier LOS/reject leg; WAC no-dismount; seat-position/displace-AI keys; the 64-tick seated seat-upgrade `@0x4ba9d8..0x4baa41` (the walk-to-entry board itself is ported, #571 2026-08-25, world-wac-ai-re §23.4); child-vehicle traversal; exact groundEntity persistence; `.aip`-gated item brains (the patrol/combat speed pair is ported 2026-08-06 — `PromoteOptions::ai_profile_speeds`, world-wac-ai-re §23.3 addendum — the rest of the profile parse remains; the def-level/helo1 profile-NAME fallbacks are ported 2026-08-25, `mission::ai_profile_name_for`); own-hull scan occlusion; and generic-seat/full-basis follow (D-INF-2). | A | OPEN (mounted fire/collision/death, UseGun root, and joiner relationship wire fixed; boarding, generic/full-basis, and scan residuals remain) | playability P4a |

| D-AI-12 | Tracer ribbon residuals (world-wac-ai-re §25, the 2026-07-18 grill that closed D-AI-8c — pool/styles/ribbons witnessed + ported): (a) jitter/anim styles (smoke 3/4/5, sniper 9/10, NVG 8) draw the single camera-facing ribbon instead of the 4-verts-per-point 3-quad cross-section with the GetTickCount wave (+0x81C/+0x820/+0x824 x 0.3/0.2/4e-4) and animated UVs (params recorded §25.3); (b) the distortion pass (+0x828 styles — backbuffer shimmer, `CEffectEmitterPool_RenderDistortionPass @ 0x5dcb40`) unported; (c) additive fog-to-black (`CD3DDevice_SetFogAndBlendMode(dev, 2) @ 0x677740`) approximated by `disable_fog`; (d) the visible round item model selected by `frndlyTrcrID`/`foeTrcrID` is ported through `Simulation::get_throwable_visuals` and `throwable_present_pass.gd`, including non-tracer suppression, but its TRACER_SCALE/TRACER_WIDTH procedural node channels (table `@ 0x83e428`, evaluator unwalked) remain unported; (e) the `light_move` glow (round+0x1B4) presented 2026-08-16 through the D-RLIT-4 light pool (`Simulation::get_round_glow_rows` → `EffectLightDirector.sync_round_glows` — mode 1, radius/2 spawn lift, per-tick follow, despawn on drop); (f) the NVG laser (`Entity_RenderNVGLaserBeam @ 0x5c6090`, style 8) waits on NVG; (g) the min-width projection divisor unresolved — 0.0012 x distance approximation; (h) jitter PRNG is a local LCG (presentation-only); (i) style +8/+0xC words consumer-less so far; (j) the pool drain runs per logic tick vs retail per frame — identical at 62 Hz | A | OPEN (procedural/dressing legs cited; the visible model and core in-flight look are ported) | playability P1 |

| D-THROW-6 | `lndm` minefield items remain unported: `Entity_LandmineThink @0x441A40` is witnessed, but the def ammo-slot writers for +692/+696 (`SMALLLANDMINE`/`LARGELANDMINE`) are not (world-wac-ai-re §27.6/§27.8) | B | NEEDS-RE (resolve the def wiring, then port the witnessed think) | PAR-WORLD / research starter |
| D-COL-2 | Building destroyed/animated section skip not modeled — the itemDef+2192/2193 bone map + the `dword_A8A418` state table skip sections (gated !player); destroyed-wall pass-through rides the destruction system (full entry: world-wac-ai-re.md §15.5) | A | OPEN | PAR-WORLD |
| D-COL-4 | NARROWED 2026-08-23: the eye test point is built from the org1 at-rest CameraOffset stand-in (h = max(top - bottom, 0x9000), lean at rest so X = Y = 0; `engine/runtime/world/collision_resolve.cpp`, the 00TRg wave-3 convoy pin) - the residual is the entity-carried +0x74 CameraOffset with live lean, which retail's think writes before it calls the resolver (full entry: world-wac-ai-re.md §15.5) | A | OPEN | PAR-WORLD |
| D-COL-8 | Run-over kill / crush + walk-over-body sounds / non-flag attrib-1 waypoint branches + the attrib-2 collision callback / the CD 0x20 door-section vtbl callback / the blocked-push AI latch remain unported. The shared ItemDef branch order and exact attrib-1 flag-family contact producer are now ported; CD containment is detected but doors/lifts remain operationally inert; the resolver's player predicate is the class bit `Flags & 0x100` at every physics leg (`@0x4b2cd9/@0x4b2f7c/@0x4b3271/@0x4b33aa/@0x4b3c78`), the local-entity compare reserved for the `g_LocalPlayerLookYaw`/blink/pitch-restore side-writes, so a listen host's remote bodies now run the ladder legs (2026-08-24) (full entry: world-wac-ai-re.md §15.5) | A | OPEN (flag-family MoveCB leg closed 2026-08-23, see the note below; listed residuals remain) | PAR-WORLD |
| D-COL-10 | PANM rotation types 3/4 route through per-section matrices but `ObjectData::evaluate_panm` passes an identity `view_inverse` where retail derives the matrix from the current global inverse-view matrix — camera-facing/upright billboard parts can pose-mismatch (full entry: world-wac-ai-re.md §15.5) | A | OPEN | PAR-WORLD |
| D-COL-11 | `LiveRound` has no BB/indoors state: retail refreshes each projectile's blink state per tick and skips the terrain clamp while the round is indoors (`Projectile_UpdatePhysics @ 0x4e9d70`) — a shot inside an interior BB can falsely hit the heightfield; port after the probe radius/state lifetime is pinned (full entry: world-wac-ai-re.md §15.5) | B | OPEN | PAR-WORLD |

Closed 2026-08-15: **D-COL-5** -> `FIXED` — the ladder climb state machine is ported end to end: entry gate + anchor snap/bump, the recontact mask 0x1 + 2-point capsule, the per-tick alignment chase, states 32–35 selection, gravity suppression + horizontal-root zeroing, the ±120° view clamp + arms lock, the side/back/bottom dismounts + on-ladder jump push + exit push/pitch restore, and the org1 `Flags 0x80` Z-chase variant `[orig: @ 0x4b3245..0x4b3495 / @ 0x4b7484..0x4b76d8 / @ 0x4bf917..0x4bfad8]`; residuals: the AI climb-order writer, the parachute/carried gate halves, the remote-climber authority display (full entry: world-wac-ai-re.md §15.5/§30). 2026-08-20 — a suspected live symptom for the missing climb-order writer was investigated and RULED OUT: 05TRcoop's east-bunker garrison (SSNs 233, 1254, 2393) does stand beside Cbunker2's interior ladder (type-4 CL volumes 6/7), but the climb order is an AUTHORED BMS attribute (bmsi_attributes bit 0x1000 -> aiRuntime[1] bit 0x400 [orig: the promote-time attribute fold, kong line 12268]) and NO person in that mission carries it — retail's soldiers there never climb either. The writer stays unported with no known symptom; that garrison's stillness is a separate open question (they walk into a wall that happens to sit beside the ladder). `tests/world/bunker_walkin_test.cpp` replays the leg offline (`--from x,y[,z]` / `--to x,y` / `--column x,y`).

Closed 2026-08-23: **D-COL-6** -> `FIXED` — capture requests now consume the movement resolver's exact authored type-10 Change Team Box contacts. `CollisionWorld::resolve_entity` publishes deduplicated authority source/trigger pairs from pass 0, snapshot-owned remote org2 bodies run that shared collision tail, and `zone_capture_contact_tick` drains the stream with the retail secured-zone/request gates; the former MoveOrder test and trigger-radius fallback are removed `[orig: Entity_ComputeBoneCollisionForce @0x4AE150 flag @0x4AEB7B; callback callsite @0x4B31DD..0x4B3238; Server_OnPlayerTouchCaptureZone @0x500BA0]` (full entry: world-wac-ai-re.md §15.5).

Closed 2026-08-23: **D-COL-8** flag-family MoveCB leg -> `FIXED` (the row stays OPEN for its listed residuals) — flag pickup/save/capture no longer scans a gameplay sphere. Successful authority pass-0 contact with an ItemDef whose `MoveCB` bit is set and `Powerup` bit is clear publishes one exact source/target event; both callback classes bypass solid force, and `world::Match` consumes the event in resolver order while repeating retail's source MoveOrder/player/not-dead gates. CTF, FlagBall, Flag Me, and C&C reach the same handler and ordered 0x1E/0x2F/0x12 host transaction `[orig: Entity_MovementCollisionResolver @0x4B2F90..0x4B2FF5; sole handler xref @0x4B2FF5; Entity_ProcessWaypointInteraction @0x4AD820]` (`collision`, `match`, `npruntime_round_end`).

**D-AI-10 KOTH producer correction (2026-08-23):** the competitive-branch
closure includes the authored hill data path, not only its decision logic.
Mission promotion preserves type 6006's BMS `wp_distance` as the entity+0 Q16
radius, the host carries that dword under pool-3 S2C 0x20 flag 0x02, and the
joining client materializes it back into the shared entity model. Numbered
capture-zone radius remains the distinct entity+350 u16 field. `[orig:
Entity_SpawnFromBMSRecord @0x40F157..0x40F173;
Server_UpdateCaptureZoneProximity @0x5089E8..0x508A68;
serialize_entity_pool_to_packet @0x503593..0x5035A9;
NapiNPClientMsg_0x020 @0x425D07..0x425D1B]`

**D-AI-10 co-op producer correction (2026-08-23):** both waypoint-family
variants now have composed authority-to-wire proof. A real WAC `Win` execution
ends stock Co-op and a real BMS `RedWin` event ends Objective Co-op; both enter
the common end transaction from inside `Server_TickUpdate`, emit 0x61/0x1D,
and consume the script-action tick from the linger. The former direct-latch
host fixture is removed. `[orig: WacAction_Win @0x4ED4A0;
EventAction_Dispatch EventAction_Dispatch @0x4542E0, the RedWin case @0x454495; Server_TickUpdate @0x51DA04;
Server_ProcessRoundEnd @0x5164F0]`

Closed 2026-08-29: **D-WPN-29** -> `FIXED` — the heat window's submerged release has its live source: the local pump feeds `WeaponFsmInputs::submerged` from the owner's BODY Z against `env.water_z` (not the drowning bit, not the eye height), so a body at or below the water plane drops a live window unless the def carries Underwater, and no authored water never submerges `[orig: WeaponAction_ProcessFrame @0x540e60 — 'Position.Z > Env_WaterHeightFixed || (Def->Flags & 4)' @0x54101c keeps the window, the clear @0x54125f]` (`local_player_view_test` pump leg; net-re §5.62).

Closed 2026-08-29: **D-THROW-2** -> `PERMANENT` — the bounce-kick and claymore-fan draws keep world-local streams of retail's generator shape rather than the shared process globals at `0x31BFBB0/B8`: the distribution is retail's, no coupled draw order is observable (every affected value is authority-drawn-and-shipped), and a shared clock-seeded stream is irreproducible by construction — the D-NET-115 ratification applied to the throwable family (register below; full entry: world-wac-ai-re §27.8).

Closed 2026-08-29: **D-COL-7** -> `PERMANENT` — the vertical ground probe reads the bilinear column height where retail marches + bisects (`Terrain_RaycastHeightmapHiRes_0 @ 0x60e710`); the two are equal for vertical rays on a heightfield and oblique rays already take `terrain_raycast_refined`, so reproducing the march would add cost for an identical answer (register below; full entry: world-wac-ai-re §15.5).

Closed 2026-08-15: **D-WPN-16** -> `FIXED` — every `instantkillzone` ammo keeps the immediate authority explosion path and Knife adds the effects-only ray with retail's strict terrain → water → PERSON-prox → buildings/items leg order and no bullet-sphere fallback; the PERSON leg's material 1 is the flesh row 23 `[orig: Weapon_RaycastAndSpawnImpact @0x4e8460; legs @0x4e86ad/@0x4e873f/@0x4e87cb; flesh remap @0x4e8880..0x4e8888]` (full entry: net-re §5.60).

Closed 2026-08-15: **D-WPN-26** -> `FIXED` — the production weapon-table builder resolves each weapon's ADM and bakes every authored `auto` action start/end from exactly one consuming clip variant at the retail 62.5 Hz duration; a definition with no `animadm` collapses `auto` to zero `[orig: Anim_InitActions @ 0x541fa0, reads @ 0x5421c5 / @ 0x5421d8; no-anim collapse @0x542180]` (full entry: net-re §5.62 + world-wac-ai-re.md §26.8).

Closed 2026-08-15: **D-THROW-7** -> `FIXED` — authoritative placed-device conversion/removal encode exact S2C 0x59/0x12 with reliable no-loopback fanout, joiner validation/fold, and exact-handle pool-1 materialization; the record's three angle words are yaw/pitch/roll from entity+16/+20/+24 `[orig: Entity_UpdateSatchelPhysics @0x448aeb..0x448b09 / Entity_UpdateClaymorePhysics @0x447a6c..0x447a8a; handler @0x5468cb..0x5468df]`. Residual: the client rest gap — retail keeps a non-authority round 248 ticks after rest and the 0x59 handler copies the client's own resting round into the entity, where ours shows nothing until 0x59 arrives (full entry: net-re §5.36 + world-wac-ai-re.md §27.8).

Closed 2026-08-15: **D-THROW-10** -> `FIXED` — after the 0x59 conversion the host enforces retail's live per-owner pool-1 device cap `[orig: Server_EnforcePlacedDeviceCapByOwner @0x5119E0; callers @0x448bd5 / @0x447b56]`: keyed on the conversion MOTOR (AT mines author `move_function schl` and ride the satchel motor's max-3 call; claymores max 4), a surplus retires the armed entry with the most negative age; the placed-device think is gated on the PRE-decrement age with the unconditional wrap decrement after `[orig: @0x4b8e1b / @0x4b8ea0]` (full entry: world-wac-ai-re.md §27.6/§27.8).

Closed 2026-07-05: **D-INF-4** → `FIXED` (the direction-table generator witnessed —
`[orig: Math_BuildSinTable @ 0x613050]`, an accumulating 1281-entry sin table at 2^22
with the cos read aliasing +256 entries; ported structurally in `quantized_dir`,
integer-equivalence + landmarks pinned in the `infantry` ctest). Also closed:
**D-ITEMDEF-1** → `FIXED` (faec4b3e — `item_type_from_string`
witnessed mapping `[orig: ItemDef_ParseProperty @ 0x49eb00]`;
[world/itemdef-re.md](world/itemdef-re.md) verdict flipped to MATCHING). The first
ledger row driven to zero. Same day, the D-EVT grill closed three more:
**D-EVT-2** → `FIXED` (the quarter-pass piggyback IS the player-AWOL counter
`[orig: @0x454d50 → Entity_UpdatePlayerAwolCounter @0x439dc0]`, ported with the
PlayerAwol evaluator), **D-EVT-3 cats 5/6** → `FIXED` (load-parity toggle
`[orig: dword_815174]`; Teammate category `[orig: @0x453b3c..0x453b67]`), and
**D-EVT-4** → `FIXED` (pre/post passes are one-shot per transition, never
periodic `[orig: @0x525b86; @0x52266c/@0x5263a0]` — our per-phase-tick post
evaluation was itself the divergence, replaced by `run_post_mission_pass`).
**D-EVT-5** minted and closed at birth: the BMS second chunk (header +0x246)
is runtime-opaque — both retail paths `fseek` past it (@0x40f6da/@0x40f756,
its only xrefs); our reader's parse-and-round-trip is a faithful superset
whose grammar is an interchange-only surface gated on D-MIS-3.

De-tabled 2026-08-06 (the closed-row compaction — the table above holds
OPEN work only; full detail in the named record + git history):

Closed 2026-08-12: **D-ITEM-14** -> `FIXED` — the ground-death transition's three ported sites now play the item's authored `particlefinale` once at the grounded pose and stamp savedLivePose (`Entity_TransitionToGroundDeath @0x493080` read `@0x493088`; the old "+0x4E0 impact pair" gloss corrected to the single +0x4E2 interned handle), and the periodic-sound clear closed as FAITHFUL-NOTHING: the 256x20-B pool at `0x26B8050` has no producer in retail JO (allocator/reset unreferenced), so the entity-matched clear is a vestigial no-op (full entry: world-wac-ai-re §24.7).

Closed 2026-08-12: **D-THROW-4** -> `FIXED` — the stick pose is the exact `Entity_OrientToSurfaceNormal @0x445fa0` port (yaw KEPT on both devices; satchel pitch bias 0xC0000040, claymore 0; the two atan2 legs off the yaw-local normal with the Q22 transform's +0x200000 rounding), and both the round motors and the placed-device ride share the exact full-Euler `Entity_InterpolateFromParentDelta @0x4a8d60` port reading the parent's `saved_live_*` channel (full entry: world-wac-ai-re §27.8).

Minted-and-closed 2026-08-12: **D-VEH-1** -> `FIXED` — the vehicle platform-solve probe boxes were a per-COBJ AABB-union stand-in; the witnessed load-time derivation (box Z = the CMDL header bbox Z pair, box X/Y = the lower-half type-1 BVOL fold, footprint = the bottom-eighth fold with the q+0x2000 clamps `[orig: Threedi_BuildCollisionModelFromChunks @0x5b3bf0 tail @0x5b4455..0x5b45db]`) is ported as `threedi_3di3_collision_probe_boxes` and closes the user-reported SP parked-truck wheel float on 00TRa (the hull floated by its below-origin wheel depth, ~0.33 u for DTruck1/2); pinned by `threedi_collision_3di` (full entry: world/vehicle-client-movers-re.md §3).
Closed 2026-08-12: **D-WPN-22** -> `FIXED` — the switch/equip deny click is the `DRY_CLAYSATCH` trigger set: `dword_24E08C4` is that row's slot in the 36-B `{name[32], slot*}` resolver table `@0x82F590`, resolved at mission load by `DialogSystem_Init @0x527687` across every loaded bank; wired as `MissionAudio.ui_soundset` + the `switch_denied` presentation consumer (full entry: net-re §5.63).
Closed 2026-07-16: **D-AI-3** -> `FIXED` — CLOSED 2026-07-16: the engagement relation ops APPLY (full detail: world-wac-ai-re.md + git history).
Closed 2026-07-12: **D-ANIM-1** -> `FIXED` — The `.bad` pose bake stopped at the header `frame_count`, dropping every clip's FINAL channel key (full detail: world-wac-ai-re.md + git history).
Closed 2026-08-17: **D-INF-1** -> `FIXED` — Clip-transition blending is ported on BOTH AnimMap channels: the primary landed 2026-07-29; the secondary weapon channel now re-inits through the same shared body (`InfantryState::begin_weapon_transition` — blend 10 / 15 on `0x400`, both playheads advancing, stable outgoing on retarget, served ring variant latched) and presentation + authoritative collision compose the outgoing/target weapon clips through one `eval_pose_blended` seam `[orig: AnimMap_UpdateDualChannels @0x40b8c0 -> AnimMap_UpdateEntity @0x40b5f0; AnimChannel_BlendTwoChannels @0x410740]` (full detail: world-wac-ai-re.md §14.8.7 + git history).
Closed 2026-07-16: **D-INF-12** -> `FIXED` — Player (org2) chase sources — the org2 grill landed 2026-07-16 (world-wac-ai-re §22): the witnessed model has NO body chase — the legs chase the render yaw (¼-step, clamp ±0x3000000, twist ... (full detail: world-wac-ai-re.md + git history).
Closed: **D-INF-15** -> `PERMANENT` — Model-table rows past the `.bad`'s bone count: the original's flag-2 translation add reads UNINITIALIZED stack floats for those rows (`bone_translations` written only for anim rows, the FK sums it ... (register below; full detail: world-wac-ai-re.md + git history).
Closed: **D-INF-16** -> `PERMANENT` — Run-promotion pitch tier ported as the constant 2: the original reads `entity+0x37C` into the `>0x430000/<0→0, ≥0x210000→1, else 2` band before adding `run_anim` (`@0x4b72aa-0x4b72cf`), but the field ... (register below; full detail: world-wac-ai-re.md + git history).
Closed 2026-07-13: **D-INF-19** -> `FIXED` — The slope pass's conform selector dropped by the port: every live body chased the terrain lean and the look pitch (`+0x14`) took the slope write, so a standing player's FP camera (`torsoRoll + lean/4 ... (full detail: world-wac-ai-re.md + git history).
Closed: **D-INF-21** -> `PERMANENT` — The "!Poof!" ghost mode deliberately unported: `g_localPlayerPoofMode @0xA82298` (net-toggled `@0x42d450`, debug-chat `!Poof!`) doubles the local player's horizontal root-motion integrate while set ... (register below; full detail: world-wac-ai-re.md + git history).
Closed 2026-07-21: **D-INF-22** -> `FIXED` — Per-entity body-ADM assignment was a one-shot mission-load sweep (full detail: world-wac-ai-re.md + git history).
Closed 2026-07-20: **D-ITEM-5** -> `FIXED` — Death kz now reads every exact case-insensitive `KZ` user point from the active first-stage husk (not `huskFinal`), converts the model IR back to mission-local axes, and queues the witnessed radius-5 ... (full detail: world-wac-ai-re.md + git history).
Closed: **D-ITEM-12** -> `RESOLVED/SUPERSEDED` — The former ordinary-round ballistic omission is fixed: old-velocity sweep order, 167-Q16 gravity, deterministic drag-table math, underwater multiplier, water-plane hit, and low-speed termination are ... (full detail: world-wac-ai-re.md + git history).
Closed 2026-07-22: **D-ITEM-20** -> `FIXED` — Building Static/collapse now gates its callback body on `ItemDeathTraits::husk_model_loaded`, fed by successful live huskFinal/husk `ObjectData` resolution; authored `has_husk` remains separate (full detail: world-wac-ai-re.md + git history).
Closed 2026-08-12: **D-THROW-1** -> `FIXED` — Throwable item sweeps now exclude terrain, water, and persons at the query boundary, leaving the witnessed pool-2/pool-1 arbitration so a nearer excluded domain cannot mask a farther entity (full detail: world-wac-ai-re.md §27.8).
Closed 2026-07-22: **D-THROW-3** -> `FIXED` — Placed-device LOS now routes through the full terrain-plus-sector collision ray, excluding the device and candidate (full detail: world-wac-ai-re.md + git history).
Closed 2026-07-21: **D-THROW-5** -> `FIXED` — The PowerThrow HUDPOWERBAR outline/fill/percent presentation and windup-state feed now match `HUD_DrawPowerThrowChargeBar @0x599830` (world-wac-ai-re §27.3/§27.8) (full detail: world-wac-ai-re.md + git history).
Closed 2026-08-12: **D-THROW-8** -> `FIXED` — the pool-1 projectile walk demand-resolves
late placed-item collision assets before narrow phase, so devices use their authored CFAC. A clone
that still has no live collision model raises a fatal invariant instead of acquiring substitute
sphere geometry. Packed-slot reuse is guarded by the registry allocation serial.
`[orig: Entity_CloneFromTemplateByType @0x4398A0 ->
Entity_InitFromModel @0x40DC30; Projectile_RaycastProximitySlots @0x4E53D4 ->
Physics_RaycastAgainstBoneCollision @0x4E4CB0]` (full detail: world-wac-ai-re.md §27.8).
Closed: **D-THROW-9** -> `FIXED` — Ported devices are pool-1 entities and already decrement arm delay once per tick, matching retail (full detail: world-wac-ai-re.md + git history).
Closed 2026-07-22: **D-WPN-4** -> `FIXED` — The heat model, witnessed and ported 2026-07-22: heat is not a stored accumulator but a DEADLINE (full detail: world-wac-ai-re.md + git history).
Closed 2026-07-10: **D-WPN-10** -> `FIXED` — Reimpl clip-key lookup was case-SENSITIVE (`SkeletalAnim::find_clip` exact ==) where the original resolves anim names with stricmp (`AnimMap_FindSlotByName @0x40cfa0`, name+5 `anim_` skip) ... (full detail: world-wac-ai-re.md + git history).
Closed 2026-07-11: **D-WPN-11** -> `FIXED` — The FSM's held-ready phase (0x40) reused the normal begin leg and emitted `action_started`, replaying begin sound/effects (full detail: world-wac-ai-re.md + git history).
Closed 2026-07-11: **D-WPN-12** -> `FIXED` — Weapon presentation events crossed the sim/present seam as one latest-value snapshot plus serials (full detail: world-wac-ai-re.md + git history).
Closed 2026-07-12: **D-WPN-13** -> `FIXED` — Held auto fire ran as a per-tick `request_fire` re-request where the original sustains the volley through the recoil window's deferred re-queue of binding 149 (`Input_QueueDeferredEvent @0x542e9d` ... (full detail: world-wac-ai-re.md + git history).
Closed: **D-WPN-14** -> `FIXED` — FALSE READING RESOLVED 2026-07-14. `Weapon_RaycastAndSpawnImpact @0x4e8460` is not the ballistic-impact path: `RoundData_SpawnRound` calls it only inside `AmmoDef.flags & 0x400` (instantkillzone) and ... (full detail: world-wac-ai-re.md + git history).
Closed 2026-07-15: **D-WPN-17** -> `FIXED` — The local muzzle flash spawned `BINDING_WORLD` at its spawn-time userpoint where retail re-anchors the live action-effect emitter (`MountSlot+0x18 actionEffectHandle` + anchor action `+0x28`) to the ... (full detail: world-wac-ai-re.md + git history).
Closed 2026-07-15: **D-WPN-18** -> `FIXED` — The LOCAL fire leg fed `RoundSim.dir_yaw` the `(90 − heading)` mission-yaw flip where the round bearing frame IS the engine heading frame (the wire-validated 0x06 `(cos, sin)` mapping (full detail: world-wac-ai-re.md + git history).
Closed 2026-07-16: **D-WPN-19** -> `FIXED` — FALSE READING RESOLVED 2026-07-16. The slot+0x18 callback is installed on the GROUP (`CEffectGroup_SetDeathCallback @0x5e1940`, group+0x5C/+0x60) and invoked only by `CEffectGroup_Destroy @0x5e3460` (full detail: world-wac-ai-re.md + git history).
Closed: **D-WPN-24** -> `RESOLVED` — Ammo-class pool ids are assigned by first-appearance registry order at table build and both storages (entity+288 class 1 / the pool array) fold into one per-entity array (full detail: world-wac-ai-re.md + git history).
Closed 2026-07-21: **D-WPN-27** -> `FIXED` — Emplaced models received only PLAYPARTANIM control-register ordinals 0/1 (full detail: world-wac-ai-re.md + git history).
Closed 2026-07-29: **D-WPN-31** -> `FIXED` — The PANM bridge placed PLAYPARTANIM phases on the model's first two CTRL entries, so B50Cal's local `[HEAT_GLOW, EWEAP_GUNYAW, EWEAP_GUNPITCH]` order aliased a generic part phase onto heat/yaw (full detail: world-wac-ai-re.md + git history) Witness: [orig: Entity_ApplyCommand case 0x22 @ 0x43B192; integrator @ 0x456710] Witness: [orig: HUD_CacheEntityDisplayInfo @ 0x4A3E18..0x4A3E38].
Closed 2026-07-27: **D-WPN-33** -> `FIXED` — Fire particles spawned at the shooter's EYE instead of the muzzle, on two independent paths (full detail: world-wac-ai-re.md + git history).
Closed 2026-07-31: **D-WPN-34** -> `FIXED` — Spawn-time weapon spread and physical recoil were absent/approximate: ERROR/theta and ammo recoil values lost their exact integer carriers (full detail: world-wac-ai-re.md + git history).

Closed 2026-08-22: **D-INF-14** -> `FIXED` - FP viewmodel `model_bind` composition corrected to the skeleton-`.bad` bind with the model-table rig source (`AnimMap_RegisterEntity @0x40bb60`; `AnimChannel_ComputeBoneMatrices @0x410da0`); placement bone-exact and rendered-identity, motion counter-gated as retail's pump, the reload/left-hand full-clip sweep exact under the one-rig law (`Player_RenderFirstPersonViewModel @0x4ded60`); the live proof is the `vm_bone_dump` probe (`godot/probes/runtime/vm_bone_dump_probe.gd`) plus the FP viewmodel GUT/ctests, the retired `fp_bone_oracle.py` transliteration having been the 2026-08-22 oracle; the per-weapon `gfx1`-table-vs-`.adm`-bind equivalence rides D-INF-13 (full entry: anim/adm-bad-format-re.md + net-re §5.40).

Closed 2026-08-11: **D-INF-23** -> `FIXED` - the authority body channel runs the gait->stance transition-clip insert (`run2crouch`/`runl2crouch`/`runr2crouch`/`run2prone`, 169-172) on the commit path and both clip-end promotion sites, sharing `world::gait_stance_transition_clip` with the netsim replica channel `[orig: AnimMap_UpdateEntity @0x40b662..0x40b737]` (`infantry_test::test_gait_stance_transition_insert`; full entry: world-wac-ai-re.md).

Closed 2026-08-11: **D-WPN-15** -> `FIXED` - terrain impact rows sample the charmap (`Terrain_GetSurfaceTypeAtPosition @0x606510` + 4 at the impact point), joining the person / CFAC / building (`material + 4` `@0x4e982b`, the Knife PERSON leg's 1 -> 23 remap `@0x4e8880`) / object / water legs; the placed-tile `.TSD` override remap closed with D-SND-15 (ctests `projectile_combat`/`throwables`; full entry: net-re §5.60).

Closed 2026-08-16: **D-ITEM-16** -> `FIXED` - destructible death samples the intact collision model at retail's total-face 8.8 stride with per-section reset, signed centroid truncation, first callback matrix, the witnessed launch direction and material-17 foliage/otherwise-wood selection; one resolved transient effect per sampled triangle, the presenter's six invented radial wood spawns deleted (`destruction`, `destruction_present_pass_test`; full entry: world-wac-ai-re.md §24.3/§24.7).

Closed 2026-08-16: **D-ITEM-17** -> `FIXED` - the exact stock graphic -> `GLASS`/`GLASS02`/`GLASS1` userpoint table resolves case-insensitively on the intact model; full-Euler points range-test against authored `kz_maxradius`, shatter once, consume the retail two-draw-per-slot PRNG sequence and emit the ordered Glass/Paper/Fire/Dust families through the transient effect drain (`destruction`, `simulation_test`, `destruction_present_pass_test`; full entry: world-wac-ai-re.md §24.1/§24.7).

Closed 2026-08-23: **D-ITEM-18** -> `FIXED` - unitType 3's explicit `PiecePhysics` state ports the raw-Q16 air/water callback, the one-draw angle branch, the Q22 four-probe slope solve/model-bottom correction, strict landing pose, resolved effects/fallbacks, scorch 7, authority-only Organic/MItem dual blasts and the separate `PiecePitchSettle` state for `DeathPiece_SettlePitch`; object participation in the short probes stays centralized under D-ITEM-9 (`destruction`; full entry: world-wac-ai-re.md §24.4/§24.7).

Closed 2026-08-15: **D-ITEM-19** -> `FIXED` - the first resolved husk retains every exact case-insensitive `DEAD` user point independently of its `KZ` bank; unitType 11 transforms each through the entity's complete Euler pose and submits one unowned `Effect_ShockWaterBrdg` at raw `Env_WaterHeightFixed` (zero included), first husk transition only, no origin fallback (the installed retail base/RevX02 catalogs author only its invisible `stockeffect` clone) (`destruction`, `simulation_test`; full entry: world-wac-ai-re.md §24.4/§24.7).

Closed 2026-08-12: **D-WPN-30** -> `FIXED` - every ammo.def fixed-point key parses through the witnessed digit walker `parse_fixed16_digits_n` (all seven `AmmoDef_ParseProperty @0x40a2d0` callers pinned to `Math_ParseFixedPoint16 @0x6131f0`; `max_age`/`arm_age` via `AmmoDef_ParseSecondsToTicks @0x40a0f0`), the round-half-up helper deleted; corpus diff 1038 values, 8 one-LSB shifts, zero signed forms (ctest `def_parse_ammo`; full entry: net-re §8).

### UI — menus/controls, sound, player-info, HUD

Closed 2026-07-05: **D-SND-2** -> `FIXED` (expansion bank slots 0/1 load ahead of
the static banks in slot order `[orig: Expansion_LoadAssets @ 0x4a4989/@ 0x4a495e]`,
fed by `ResourceRoot.get_expansion()` off the runtime mount; missing files skip
like `SoundBank_LoadIfExists`). **D-CTRL-2** -> `FIXED` — the witnessed per-entry show-flag
gate ported with every catalog row's flag word minted from the binary
(`[orig: UI_PopulateControlMappingList @ 0x55c0c0; catalog flags @ 0x8159AC
+ 108*id]`; the class-category approximation deleted; observable corrections
pinned in `controls_test`). Also closed: **D-PLAYERINFO-2** -> `FIXED` — verified already enforced:
the parser errors at the 512-part cap (`engine/formats/avatars/avatars.cpp` guard,
`[orig: CAvatarDefs_ParseConfigLine @ 0x57a456]`), `AvatarDatabase`
propagates the failure, and `tests/avatars/avatars_parse_test.cpp` pins the
512-part parse failure. The row predated the guard's landing (AVA train).

Sources: [mnu/menu-re.md](mnu/menu-re.md), [audio/lwf-dbf-sound-re.md](audio/lwf-dbf-sound-re.md),
[playerinfo/avatars-re.md](playerinfo/avatars-re.md), [interface/hud-re.md](interface/hud-re.md),
[interface/loading-screen-re.md](interface/loading-screen-re.md).

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-MNU-5 | Text-item rendering scope: combo/list image/color items not backed (shipped menus are text-only there) | A | OPEN | PAR-UI |
| D-MNU-6 | CBIN credits custom `~F` fonts / `~I` images not resolved from the resource root (default font only) | A | OPEN | PAR-UI (see credits audit PAR-R5) |
| D-MNU-9 | Armory per-class loadout memory: the visible row model + weight path match, but retail's separate remembered per-class buffers (save-on-class-flip counts) remain deferred | A | OPEN (kept-deferred) | PAR-UI |
| D-MNU-12 | Combo popup draw order: retail draws the child-of-combo popup INLINE at tree position yet renders it visually topmost via an unwalked mechanism (the 2026-08-11 sweep ruled out an overlay pass, a draw-path visibility gate, toggle-time reordering, and the +0x290 dirty flag — menu-re.md "Combo dropdown"); options.mnu's Advanced panel makes the order OBSERVABLE (WATERQUALITY's 80px popup over later 30px rows), so the reimpl defers open popups to a post-walk overlay pass (the compiled successor of the Control-tree menu-top overlay). Single-open registry + outside-press consume carry the input half | C | OPEN (kept divergence + the unwalked retail topmost witness) | PAR-UI |
| D-MNU-13 | Draw-walk residue — deferred interiors (menu-re.md D-MNU-13): the RADIOEDIT render (`@ 0x65d310`) is unwalked (its event interaction IS witnessed; no shipped JO menu authors one); compiled-path follow-ups: table image/SUBST/custom cells + per-row appearance overrides + row-overflow wrap; marquee image nodes + the 50px edge fade band. Scrollbar art AND interaction (arrows/track-paging/shuttle drag-capture) are ported into the compiler's pump (2026-08-11, `CScrollWnd_HandleEvent @ 0x64d050`); residue = the child BUTTONs' independent hover/pressed states + named scroll events, and the multiline edit's scrollbar/wheel interaction (menu wheel policy itself is D-MNU-18) | B | OPEN (deferred interiors — menu-re.md draw-walk residue) | PAR-UI |
| D-CTRL-1 | Mouse/joystick binding arrays (profile-built at runtime) not ported; those rows show a blank Control column. **Scoping (2026-07-05):** NOT in `PlayerProfile_InitDefaults @ 0x54bb40` (that sets settings/macros/default weapon loadouts only) — the mouse/joystick default bindings are built by a separate input-binding init (an RE hunt), and the consumer is the Godot input-action layer (same gate as D-CTRL-3) | A | OPEN | PAR-UI |
| D-CTRL-3 | **Remap flow ported 2026-08-11** (double-click arm -> capture -> witnessed assignment/dedupe incl. the per-slot MODIFIER word — Ctrl-combo capture, "Ctrl-"/"Shift-" display, every-Ctrl-press drop — Esc cancel + screen-change cancel, mouse-mask capture, DEFAULTS/CLEAR_KEY; the gameplay sampler reads the live records: keyboard slots gated on their modifier plus held L/R/M mouse-mask buttons — wheel-mask bindings display/persist but are impulse-only and do not sample) [orig: UI_ControlsRemapArmHandler @0x55d560; KeyBinding_HandleKeyAssignment @0x55bb20; Input_QueueKeyEvent @0x760c10; KeyBinding_FormatBindingString @0x559a10; @0x55bd90/@0x55bfd0; @0x55c780]. Residue: persistence rides `user://controls.cfg` — retail stores the records in player.sav (profile+1804/+1808, 72-byte stride; PlayerProfile_SaveToFiles @0x54be00) and the profile record format beyond its geometry is unwalked; joystick capture unwired (with D-CTRL-1); the single-click-of-selected-row arm and the refresh pass's yellow active-binding highlight (@0x55b320) are not ported | A | OPEN (player.sav profile format slice; joystick page) | PAR-UI |
| D-PLAYERINFO-9 | ACCEPT/commit + profile persistence: callsign and the active `weapon.sav` slot's two side-specific avatars + shared class now restore and persist atomically `[orig: save_player_info_from_dialog @ 0x55EE10; class loop @0x55EE3F..0x55EE6D; selected-side avatar stores @0x55EE93..0x55EF38]`; all five records and existing kit pages are preserved. Residual = serializing newly edited kit tuples plus the `player.sav`-level option fields | A | OPEN (narrowed 2026-08-15) | PAR-UI |
| D-PLAYERINFO-12 | Per-(slot, team) selection globals: the per-TEAM memory is ported 2026-08-15 (`PlayerCharacterSelectionState` keeps both sides of slot 0 through UI entry, team switch, ClientAuth/host spawn, 0x0C decode, and presentation; the writer preserves the other four `weapon.sav` records) `[orig: g_charSelClass/Nationality/Division/Combo @0x2551130.. strides 67596/32774; save_player_info_from_dialog @0x55EE10]`. Residual = the profile-SLOT dimension: retail's five-slot `PLAYER` selector (`PlayerInfo_InitProfileSelector @0x5611b0`, `g_curProfileSlot @0x25506B8`) is unported, OpenNova always edits slot 0 | A | OPEN (narrowed 2026-08-15: slot selector) | PAR-UI |
| D-SND-5 | WAC `wave`/`pwave` ride one interrupting reimpl voice channel; positional `SSNwave`/`SSNradio` + the rest of the sound family stay parsed-but-unconsumed | A | OPEN (record tracks the follow-ups) | PAR-UI |
| D-SND-8 | Master fade / underwater duck / SFX volume / bearing pan map to bus routing + the Godot spatial panner; doppler unported | C | OPEN (reimpl playback territory; underwater/doppler on demand) | PAR-UI |
| D-SND-17 | Ground-vehicle engine-sound lanes PORTED 2026-07-29. CORRECTED 2026-08-19: the "remote rows do not restore `veh.speed`" clause was STALE — the wire prediction registers (D-NET-200, 2026-08-02) and the client-side motor prediction whose tail runs the sound fold (D-NET-196 B-facet, #424, 2026-08-06) already restore it, so moving remote gain/pitch works today and is now pinned by `vehicle_motor_test::test_remote_speed_drives_the_same_movement_sound`. Also fixed here: a wire-frozen row kept its motor speed registers (the freeze branch cleared bob/accumulators but not `speed`/`speed_accel`/`cmd_speed`), so a row frozen mid-motion held a moving lane on state nothing advances. Genuinely open: the zero-speed displacement substitute inside the fold (fork-cited as a `\|savedLivePose − Position\|` leg inside `@0x5294A0` but never quoted at instruction level — needs a grill before porting); the ai_system fallback branch's `collided=false` (retail derives contact from client physics, so this rides D-NET-161's contact-severity stand-in rather than a wire bit); and the aircraft slot-30 start + air movement-loop siblings `@0x46F883`/`@0x471621`, which are a different witnessed surface neither tree has grilled | A | OPEN (narrowed — displacement substitute + fallback contact + the air family) | PAR-UI |
| D-LOADSCR-2 | Loading-screen text renders via the Godot FontFile view + line metrics, not CGameFont glyph compositing (spacing params unwitnessed) | A | OPEN (shared CGameFont follow-up with hud-re.md) | PAR-UI |
| D-LOADSCR-8 | The epilog-stage splash re-show (the start key at the SP post-spawn stage re-runs the splash then queues the deploy event) not ported — rides the unported SP epilog/respawn flow + the configurable start-key binding (D-CTRL-1 territory) `[orig: Input_HandleSpecialKeys @ 0x49c5c0, branch @0x49c871, splash re-run @0x49c88f, release @0x49c899]` | A | WITNESSED-READY-DEFERRED (with the epilog/respawn flow) | PAR-UI |
| D-LOADSCR-5 | Seven-segment numeric load percentage (drawn only under `g_ShowLoadBarCommandLineArg`) not ported | B | OPEN (debug-only; revisit with launch-flag work) | PAR-UI |
| D-HUD-5 | Clip-indicator flash restamp keys on (`round_type`, reserve) — the original keys (ammo class `def+220`, reserve, pool id `def+216`); same transitions under the single-pool weapon model (D-WPN-2) | A | OPEN (revisit with per-class pools) | PAR-UI |
| D-HUD-6 | Mission triggered text — NARROWED 2026-08-19: witnessed to post into the ONE system ring `[orig: HUD_DisplayTriggeredText @0x51f190 -> Chat_AddDebugMessage(text, -1, 930) @0x51f216]`, the same ring as the 0x1E feed lines, and now ported that way (`push_message` forwards into the feed ring at `HUDSYSTEXT`; the earlier `HUDCHATTEXT` placement was the pre-witness approximation). The drawer paints both channels behind one gate `[orig: HUD_DrawConsoleMessages @0x59ad30 — mask @0x59AD33, level cull @0x59AD43]`: 3 ring rows per channel, oldest at the anchor, 18 design-px step DOWNWARD `[orig: @0x59ad97]`; the `timer*255/186` alpha fold belongs to the CHAT loop alone `[orig: @0x59adef vs the stored-color read @0x59ae97]`. Residuals: the player-chat channel (S2C 0x14) and its ring, the word-wrap of long lines into 2-space-indented ring slots `[orig: Chat_AddDebugMessage @0x4987F0 wraps via font metrics]`, the `dword_28E4DF8` geometry table, the J-key log window, and the centre announce banner fed by the involved-line copy `[orig: strncpy @0x427B8B -> HUD_DrawKillAnnounceBanner @0x59dc90]`; 2026-08-21: the player-chat channel PORTED — the S2C 0x14 fold (`client_replica_feed.cpp` → `ClientChatLine`), the display-slot ring with the witnessed wrap (`push_chat_line`/`chat_wrap_text` = `Chat_AddMessageChannel1 @0x4985d0` + the `HUD_WordWrapText` wrapper), the `Chat_DispatchToChannel @0x42b910` channel → sink/colour table (no local-team term), the HUDCHATTEXT first loop of `element_feed`, and the J-key Recent Messages window (`hud_frame_message_log.cpp` over the display slots 16..1, the `g_showMessageLog`-only gate) with its `OldMessages` toggle lane; the geometry table is the authored HUDCHATTEXT/HUDSYSTEXT rows (`HUD_GetChatBoxCoord @0x5bbe90`) | A | OPEN (narrowed — the centre announce banner `HUD_DrawKillAnnounceBanner @0x59dc90`; channel 13's tracked-target leg; `HUD_DrawOverlayPanels`'s third toggle reader) | PAR-UI |
| D-HUD-23 | The kill/objective/medic message feed: S2C 0x1E folds to typed client events (`netsim::ClientGameEvent`) and each line is the game's own "Canned Msg" template with the witnessed substitution (sequential case-insensitive `$A` then `$B` `[orig: Chat_FormatMessage @0x422C60 -> String_ReplaceAllCaseInsensitive @0x422970]`, the `STRCND48` bonus re-compose when aux is the local player `[orig: @0x422CA2]`, `STRCLI01` "Unknown" for a null actor `[orig: @0x422DDA]`), posted to the SYSTEM ring with the per-case color table of the `@0x426270` switch (own white `-1` / other grey `0xFFAFAFAF`; friendly-fire 7/8/9 and 16-18/27-31/35-37 white; bonus 32/33/34 yellow `-256`; medic trio 38/39/45 + SSKB 46/47 `0xFF008CEE`; PSP/LFP blue `0xFF00AFFF` / red `0xFFFF0000`; 40 red, 48 orange `-32768`; camp 59/60 by team byte with the `WPNames[level+1]` `%s` compose `[orig: @0x4272EC/@0x427327]`). The suppression set is witnessed (50-53 format-and-return `@0x42702E-@0x42716D`; 58 tip-only `@0x427202`), as is the verbose gate on uninvolved kill lines `[orig: g_MpVerbose2 @0x24D2154, 13 tests @0x426472..@0x4267C6]` — held at the verbose-on session default. Residuals: the runtime team/gametype-keyed types (19/20/21/46/47) resolve no key yet, join/leave lines + the host-exclusion filter now have their roster (D-HUD-24 folds S2C 0x46 into `ClientState`) and only need wiring, player-slot names with `<ch>clan<co>` tags `[orig: @0x422E1D]` (roster names serve today), the verbose keybind toggle `[orig: @0x49B78F]`, and the PSP/LFP/camp team sounds, tips and effect spawns beside the lines | A | OPEN (partial — the 0x1E fold, the SYSTEM ring and the witnessed line/color policy are ported; runtime-keyed types, join/leave, slot names and the event side-effects remain) | PAR-UI |
| D-HUD-24 | The Tab scoreboard's DATA lane: S2C 0x16 folds to `ClientScoreboard` (flags, rows in wire order, the team table, the in-game/spectator trailer) and S2C 0x46 to a connection-slot roster, both in `ClientState` `[orig: NapiNPClientMsg_PlayerList @0x42FAE0; NapiNPClientMsg_PlayerSync @0x431370; Server_BuildAndBroadcastScoreboard @0x50D960 every 311 ticks]`, with the witnessed parser shape: every well-formed 0x16 applies unconditionally (an empty update EMPTIES the board `[orig: @0x42fb46]`), rows for roster-unknown slots drop `[orig: @0x42fc05]`, name/clan join into the row at apply time `[orig: @0x42fd4c..0x42fd8f]`, and a 0x46 removal deactivates + wipes the slot `[orig: @0x434730/@0x4346c0]`. Decode-era field names corrected against their witnesses: the second row u16 is a STATUS BITFIELD not a ping `[orig: @0x42fdb4, readers @0x423f1f-0x4240e0, the zeroing @0x42fbfb]`, the fourth is accumulated points/EXP not deaths `[orig: @0x52C8E0; sort @0x500A80]`, and the team-row bytes are kothHold/ctfFlag not player/alive counts `[orig: @0x50dc62/@0x50dd30]`. Joiners receive all three lanes on the reducer stream; the host's own view binds via the loopback self-0x46 (D-NET-114 form). PORTED 2026-08-29: the C2S 0x22 unknown-row retry — the reducer queues each dropped row's slot and the joiner runtime frames one reliable `0x22 {slot, 0x1CF7}` per row at the next send boundary `[orig: @0x42fc05..0x42fc3a, the queue @0x42fc35]` (`npruntime_client_runtime` pins one request for the unknown slot and none for the bound one); and the 4-team page — with more than two sides configured the team board alternates between the 1/2 page and a 3/4 page (colors 0xFFFFFF00/0xFFFF027F) on bit 7 of the per-main-frame HUD counter `[orig: @0x423cd0-0x423cf1, g_num_teams_config > 2 && dword_A87060 & 0x80; the counter Game_TickHudFrameCounters @0x434c14]`, the joiner reading the side count off the 0x16 team table the host serializes from that config `[orig: @0x50db3a -> g_scoreboard_team_count @0x42fdda]` (`scoreboard_format` pins the page). Residuals: the per-recipient status-word gate `[orig: server @0x504bd6 slot+96481 / client g_scoreboardStatusSuffixEnabled, the SU text command @0x429f71]`. PANEL DRAWN 2026-08-19 (re-witnessed in full on review): `HudFrameCompiler::element_scoreboard` + `hud::hud_scoreboard` carry the witnessed layout in raw design-space constants through the shared scaler, every string on `g_hudLabelFontBold` `[orig: each draw site in HUD_DrawKillList @0x423A30 / the header block @0x423060]`. The stdbox panel (20,78)-(1004,550) with the title left-aligned white just inside its corner `[orig: HUD_DrawLabelBox @0x423a90; (x+15, y+2) @0x51f002/@0x51f006]`; the centred header ladder on FIXED rungs 105/125/145/165/185 stepping 0x14 whether or not a line's string is empty (only the spectator rung is gated on a nonzero count) `[orig: the unconditional adds @0x42315c/@0x423184/@0x4231da/@0x423225; the gate @0x42322a]` — server name and mission title render white via retail's embedded `<cFFFFFF>` runs `[orig: HUD_GetLoadingScreenTextByGameType @0x51f300 — the sessionvar pair + the tag sprintf @0x51f42d/@0x51f497]`, the game-type label from the witnessed Overlays row map (STROVER28/29/64/30/48/31/56/57/58/92/93) `[orig: HUD_GetGameTypeOverlayLabel @0x5b8680]`, and the player count = accepted rows MINUS the trailer's spectator count `[orig: the subtraction @0x4231dd]`. The list base sits one step below the header's return and every column cursor PRE-increments, so the first row lands at base + 18 `[orig: return + 0x14 @0x423aad; row_y = cursor + 18 @0x423d30]`; rows draw while y < 490 `[orig: @0x424168]`; non-team modes (types 0, 1, 8 `[orig: @0x423acb]`) alternate columns x190/x690 with a leading SIGNED score (the parser movsx's the wire u16 `[orig: @0x42fb9d]`; fmt `@0x7c4bc4`) while team modes column by team with no score (fmt `@0x7c4c00`) and draw ONLY rows whose slot still binds a live entity — a leaver's row vanishes from a team board `[orig: the entity-null fallthrough @0x423d1b]`. Spectators keep their own x440 column SEEDED below the longer player column plus two spacer rows `[orig: base + 18*(max+2) @0x423c3a; the 2 @0x423ba1-0x423baa]`, never show a score in either mode `[orig: the spectator arm @0x423e04]` and never a rank, but DO draw the connection icon. One GLOBAL rank counter runs in wire order across both columns, advancing for clipped rows too `[orig: "%2ld." @0x424186 at column-40, yellow -256 @0x4241b0; the ++ outside the clip test @0x42424f]`. The per-row 16x16 icon samples band `quality-1` of neticon2.tga (a 4-row strip, band = tgaH/4 `[orig: the load @0x4c2cf0]`) and draws NOTHING outside 1..3 — retail's own gate, quality clamps at 4 on store yet 4 still selects no band `[orig: NetIcon_DrawConnectionQualityBand @0x4c2ee0; the clamp @0x43170d]`. The status-glyph suffix follows the witnessed APPEND order (not bit order) with `S` outside the bracket `[orig: @0x423f29-0x4240e5]`. The board is a press-TOGGLE, not a held level: the playerlist action (the shipped `playerlist_alt` vk 0x09 row) edge-toggles the visible flag, respawn init clears it, and PgUp/PgDn page while it is up `[orig: Scoreboard_TogglePlayerList @0x4244c0 from the dispatch case @0x49bb68; the clear @0x4993ae; the page keys @0x49c912/@0x49c93e; the drawer gate HUD_DrawKillListIfVisible @0x424300]`. The stdbox GEOMETRY is the witnessed one, pinned by `hud_frame_compiler`: pieces and the fill inset scale by `s = surface_w / 1600` `[orig: the 0.000625 double @0x51f02e]`, one piece = one quarter-cell of the border atlas `[orig: quarterW/H @0x56adb6/@0x56adbd]`, the fill insets 16*s / 24*s `[orig: the ctor literals @0x56b342/@0x56b351, read @0x56b7bd-0x56b80d]`, the bottom trio crops to its top 90% in BOTH dest and source `[orig: the crop arm @0x56b454-0x56b470, flt 0.9 @0x7c459c; the flags @0x56bbc0/@0x56bc22/@0x56bc80]`, and a TITLE notches the top border: the row-3 stub / title-bar / end-cap cells around a gap of the measured bold title + 2 (less 12*s once it exceeds that) `[orig: the outTechnique arm @0x56b93d-0x56ba52; the gap rule @0x51f0ea-0x51f114]`. The FILL is NOT part of any camo combine: the style ctor EXTRACTS atlas cell (3,0) into its own texture (zeroing it out of the atlas) and, because the stdbox registration passes a ZERO fourth arg `[orig: @0x525aa2, ebx=0 @0x524381]`, the drawer takes the plain path — one wrap-addressed quad of that cell, UV = (screen_px + 0.5)/cell, i.e. screen-anchored tiling at the cell's own UNSCALED period `[orig: the extraction @0x56adbd-0x56ae44; the rec+0x3C==0 arm @0x56b739 -> stdbox_draw_fill_wrap_tiled @0x56b5d0]` — the port's fill (the same cell out of the atlas slot, tile loop on the same screen grid) matches it. One recorded divergence remains, a missing capability rather than a missing witness: the EIGHT BORDER PIECES bind border x boxtile — ONE combined material `[orig: CGfxTexture_Create (ex sub_676EA0)(BoxTexA, BoxTexB, 0x651, 2) -> style+0x30 @0x56af3c, applied @0x56b902]`, stage 1 = MODULATE(CURRENT, TEXTURE1) with a SCREEN-ANCHORED UV1 = (screen_px + 0.5)/boxtile_dim `[orig: draw_textured_quad_0 @0x56b3e0 — the dest-derived UV pair @0x56b560-0x56b592; the divisors stored @0x56b357/@0x56b361]` — so retail's PIECES read camo where ours read plain stencil; it needs a second texture stage the HUD quad stream does not carry yet. Every retail quad's diffuse is `alpha<<24 \| 0x7F7F7F`, half-bright under MODULATE2X `[orig: @0x56b70e-0x56b713]` — a host without that stage reproduces it with a neutral white diffuse. The monogram watermark is deliberately NOT drawn: its 0x622 flag word's LOW NIBBLE selects ONE/ONE, pure additive `[orig: decode_blend_mode_to_d3d_states @0x680f00 -> D3DRS 0x13/0x14/0x1B via GfxBlend_ApplyToDevice @0x6817d0]`, its diffuse is the darker `alpha<<24 \| 0x282828` `[orig: @0x56b8db]`, and the shipped sheet is measured 100% pure black — the pass provably adds nothing. Still unported and recorded rather than invented: the header's per-mode TEAM-SCORE block (TDM/KOTH/CTF/SD/FB lines from the team table + the flag-carrier line) `[orig: @0x4232bf-0x423a12]`; the per-recipient SU status gate — with it, a gate-ON zero word draws the empty " []" the port's zero-word-no-suffix approximation drops `[orig: the byte gate @0x423ef8; " [" @0x423f1c, "]" @0x4240c8]`; host-side sessionvar strings (a hosting client's server/mission rungs stay blank until plumbed); the host-side 0x16 SERIALIZER's stat sources — `encode_player_list` emits zero status/score1/score2, never sets the spectator bit, and zeroes the team table, because the `CPlayerStats` record system the retail serializer reads `[orig: CPlayerStats_GetFieldPlusOne (ex CRenderState_GetFieldByIndex) @0x52d7d6; Player_ComputeScore @0x500A80; the serializer fills @0x504c2e..0x504d20]` is unported — an opennova-HOSTED board draws rank/name/icon rows with zero scores until that system lands (retail-server joins are unaffected); the row's third highlighted string — the slot+0x20 label set by the 0x46 bit-0x10 arm, drawn `<ch>..<co>` after the name `[orig: the setter @0x434870; the fmt @0x423ddf]`; the same-team class suffix `" (%s)"` `[orig: the switch @0x423d8a; the append @0x424104]`; the KOTH countdown row format `[orig: @0x423e7d-0x423ec9]` (needs the session time limit `dword_A821C0`); and PgUp/PgDn paging `[orig: the page fold @0x423c1c]` | A | OPEN (partial — data lane folded, panel drawn with layout, toggle and geometry re-witnessed on review; the camo piece combine, the team-score header block and the listed tails remain) | PAR-UI |
| D-HUD-8 | Crosshair color modulates the texture — the original writes it to the strip's specular channel (blend stage in the unwitnessed HUD shader pass); identical for the default white | B | OPEN (witness the texture-stage state) | PAR-UI |
| D-HUD-14 | Bottom prompts (preround armory / vehicle-bay / FARP wait+reload, `HUD_DrawGameplayOverlays @ 0x5bde60`) unported — each rides an unported system (MP preround / vehicle.mnu / FARP rearm); the ARMORY_WAIT leg is dead code in retail | A | WITNESSED-READY-DEFERRED | PAR-UI |
| D-HUD-16 | SP waypoint track is built sim-side at mission load from the BMS nav channel (`flags & 2`) + pool-3 markers — retail routes the same data through the S2C 0x0F apply even in SP mode 3 (net-re §5.29); same selection rule, no wire round-trip. 2026-08-13 spawn probe (JOTAC 00TRa, two captures seconds apart + a 55 s hold): retail DOES arm the first waypoint at spawn (tether + tip dot + the "032m" at-tip label, identical at t0 and t+55) — the earlier same-day "retail spawns unarmed" frame was a seconds-wide pre-arm sliver during the loopback world-state apply, NOT a polarity divergence; the load-time latch model stands (the latch itself is retail's own `@0x4de6de..0x4de6fa`, family-gated `(g & 0xFFFDFFFF) == 0x10020` with the count-1 and non-family fallthrough latches `@0x4de6f3/@0x4de701`) | A | OPEN (the MP-join 0x0F waypoint leg is the npwire follow-up) | PAR-UI |
| D-HUD-17 | Waypoint advance ports the proximity/last-entry/skip-done/event-link legs; `SpawnPoint_CheckWeaponRestrictions @ 0x4dbe80` modeled always-pass, and the MP POI list (`Entity_BuildMapPoiLists @ 0x42de40`) + spectate-cycle reuse are unported | A | WITNESSED-READY-DEFERRED (AAS/MP HUD phase) | PAR-UI |
| D-HUD-18 | Objectives panel (`HUD_DrawWinConditions @ 0x5ba940`) — the subgoal state machine, row walk, and announcements were already exact; GEOMETRY CLOSED 2026-08-12 (full disasm): the 16x16 four-line checkbox (0xFFE0E0E0), the done-mark RED X (six 0xFFFF0000 lines — the old "checkmark" gloss was wrong), the `HUD_DrawLabelBox` rect (y-0x18/+0x48/+0x30), the measured-height row advance, and the exact alpha/gray color folds are ported into `element_objectives` with the panel alpha byte (`HudFrameState::objectives_alpha` — retail `dword_24C18CC`) folded into every draw color. Residuals, each riding an unported system: the win-score add `@ 0x454526` (score), the "New Objective" toast `@ 0x5ba2e0` (internals unwalked), the header unknown5[2]/[3] team-banner masks (banners), the KEY_O reimpl binding (input layer), `HUD_DrawLabelBox`'s box-shader styling (fill+wire stand-in at the witnessed rect), and the fontLarge/fontBold slot plumb (single HUD font) | A | OPEN (geometry/color/alpha FIXED 2026-08-12; residuals = the unported score/toast/banner/binding systems + two cited drawer stand-ins) | PAR-UI |
| D-HUD-19 | The DEATH deploy screen (`DeployScreenPresenter`, death.mnu) ships the authored chrome, the zone-row half of the SPAWNPOINTS_LIST populate, the live D-NET-170 zone-security fold, stable selection by the row's wire parameter, and the full pick flow, with three witnessed legs still unported: (a) its MAP window renders no map image — the windowed map view `MapOverlay_DrawView @ 0x5a58e0` (pan/zoom handler `command_map_overlay_input_handler @ 0x554310`; fullscreen sibling `HUD_DrawMapOverlay @ 0x5a5f40`) is the tracked next map-phase witness — the 2026-08-13 gameplay-spinmap port (D-HUD-21) supplies the reusable compiler, banks, and the shared `MapOverlay_RenderAllLayers`/`MinimapSlot_*` story to host here; (b) PORTED 2026-08-24 — `UI_UpdateDeathScreenContent @0x5536a0`'s second loop (the per-zone wave occupant rows + blank separator at node -1 from the 0x6E fold, the `insert_pos = 0` unlisted-zone quirk kept), the closing `ListWidget_SortRows(list, 0, 1)` case-insensitive text sort over the whole list (`cmp @0x6448a0`), the STATIC_RESPAWN_MSG1 penalty/wave line, STATIC_PSPRESPAWN_MSG1, and the STATIC_MEDIC_MSG1/STATIC_CALLMEDIC_MSG revive-window pair (`KeyBinding_FormatDisplayString @0x496bd0` → `controls::format_display_string`), all in `engine/runtime/world/deploy_screen_feed.h` + `Simulation::get_deploy_list_rows/get_deploy_status`; the client medic call (C2S 0x2E, `Input_HandleActionBinding` case 217, the 310-tick `dword_B76804` cooldown) rides `Simulation::request_local_player_medic`; still open in (b): the STATIC_INSTRUCTIONS_MSG/INSTRUCTIONS2 arms (`@0x553f6a..0x554110` — the full-spawn scan, `sub_43B910`, the welcome/team text) and the permanent-death arm (`byte_A821EF`, `@0x553710..0x5538e0`); (c) the populate's 0x40 minimap-flag zone exclusion | A | OPEN (map-draw witness + the instruction statics + the 0x40 exclusion) | PAR-UI |
| D-HUD-20 | Overhead friendly name labels (retail FRIENDLYTAGS): CORE PORTED 2026-08-10 — the drawer is `HUD_DrawEntityLabel @ 0x5a39b0` off `HUD_DrawFriendlyTagsPass @ 0x5a4480`; the SP/AI path lands end to end (gather → projection → `element_friendly_tags`: health-tier tagcolors, 50–300 m alpha, centered alpha-preserving half-bright text, `'^'`+36-name fallback, BMS→`[PeopleNames]` authored names, FULL/FARBRIEF/BRIEF modes + KEY_F cycle/toast, the red-cross medic plate). The `hud_color_index` scheme swap LANDED 2026-08-13 (the good tier reads `tagcolor_good` only at index 2, else `g_hudColorTable[index]`; the table/cycle/config witness + the two overlay-color twins = hud-re.md "The hud_color_index scheme"; the cycle rides the byte-witnessed `hudcolor` row — code 10, default F6, retail-shadowed by `huddetail`; making it reachable = D-CTRL-4) | A | OPEN (narrowed to the residues: the MP slot-walk's remaining legs — squad colors, the slot+32 channel wrap, the joiner rows' class/eye offset — the callsign/dead-recolor/count/pulse legs and the client 1 Hz revive countdown PORTED 2026-08-24 (`collect_roster_tags`, `tick_roster_revive_countdown`, `friendly_tag_revive_pulse`); the server-granted enemy leg `@0x24D1DF4`, the `entity+885` wounded icon (writer unwitnessed), the speaking-level device feed, the sample-less remote-player 3-angle lateral eye tilt (`@0x4b66fc..0x4b68e5`; the local head-bone exact triple + the NPC lateral pair ported 2026-08-19, the z restamp + Arial label fonts 2026-08-11), the difficulty max-health term — hud-re.md row); 2026-08-21: the charattr medic FEED landed (`world::class_has_attribute` Medic 0x8 → the plate AND the map marker) | PAR-UI |
| D-HUD-21 | Gameplay spinmap: the normal gameplay pass is ported and synchronized against retail JOTAC 00TRa. Closed through 2026-08-15: (1) mission spawn zoom is `65536/524288 × clamp(1 - Bms_MapZoom, 0.0625, 1)` (`0.61 -> 25559`), and world-per-pixel is `zoom/(scaled rect HEIGHT × 200)`; the live completed pass gives `25559/(281×200)=0.45478648`. (2) The true-pixel-circle backing/terrain/marker disc is `scaled half-height - scaleX((flags >> 8) & 2)` — two design px through the width-axis scaler, 4 physical at the probed 1920 width (corrected 2026-08-19); the compass quad uses uninset half-height ×1.25 and samples COMPRING's centered `0.05..0.95` UV range (`0.5 ± 0.45`), making the authored visible ring `1/0.9` larger so the same landmarks touch it. (3) Building footprints come from the model OOBJ occlusion arrays (`model+0xDC/+0xE0`): type 0/1 records, OPLN Y normal >0.5, OVRT X/Z triangles, exact per-record OFAC low-15-bit parity, opaque team/neutral fills; completed-pass captures show no observable `0x80000000` boundary stroke, so OpenNova submits no black outline. (4) The base pass samples the original four 512² colormap quadrants directly (packed as one half-texel-clamped 1024² atlas in OpenNova), not the fuzzy 4096² per-cell/.til composite. Water is the independent 256² `depthspin` pass built from exact four-tap raw16 height reduction, quadrant UVs `127/256` + `130/256`, integer-plane alpha test, and final synchronized tone `0xFF16476B`; `.til` art does not participate and `GameWorld.minimap_water_changed` refreshes only this mask. (5) Ordinary TSDicon sprites apply the missing saturating `MODULATE2X` RGB stage; synchronized green peaks are retail `(88,255,65)` vs OpenNova `(86,255,64)`, while apparent capture shades can differ with filtering, blending, and live state. The center blue glyph is definitively the local deployed Person: regular source, cell 3, raw team-blue `0xFF304080`, 6px half extent; the snapshot restores that client-local row only when absent. The reference frame has `showWaypoints=0`; the separately configured level waypoint line remains the exact raw `0xFF007000` and is gameplay-state dependent. Also ported: unmasked sector walk and forced-opaque ×4-equivalent tint, exact depthspin shoreline, marker-bank order/pulse/size/rotation policies, compass gate, waypoint pointer/altitude/distance state, MAPCOORDS/grid leg and type-2043 origin, M-cycle modes/lifecycle/frame stacking, 0x6B range-valid liveness, and snapshot v3. Still unported/residual: weapon-direction indicators + timer/radar-contact rings; objective tether lines; entity/location labels; tracked-target legs; persistent-bank split and special layer-1/2 redraw quirk; objectives-family-above-big-map ordering; pointer/grid-label anchors pending an M-map capture; big-map pan/drag and masks 11/13/14/15/modes 1/4; sibling out-of-map radar/damage/directional consumers. The backing-disc color remains capture-calibrated pending a pass-state witness. The HUDDECLUT dependency is RESOLVED by the port: the declutter system (persisted `hud_detail` 0..3 over the 24 authored `HUDDECLUT_*` mask bytes, slot 17 = SPINMAP, the F6 `huddetail` cycle, death forcing 3, level 3 blanking the whole gameplay overlay pass) landed 2026-08-15 in this branch; 2026-08-21: the #545 `hud_map_bracket` "target bracket" port was a misattribution of `HUD_DrawMedicCrossQuad @0x59BCB0` (the map medic marker via `draw_entity_labels_and_markers @0x5a4d40`) — unified into `hud_medic_cross.h` with the friendly-tag plate; its map-scale half duplicated `hud_minimap` and was dropped; the map medic marker itself PORTED 2026-08-21 (`HudMinimapMarker::medic` from the charattr Medic bit replaces the teammate blip in `hud_minimap.cpp`, snapshot v4) — the remaining in-map label tails of `draw_entity_labels_and_markers @0x5a49e0` are the pickup pulse icons 8/14, the parachute icon 23, the selection icon 28, the name/clan labels and the second `unit_type == 3` loop | A | OPEN | PAR-UI |
| D-HUD-22 | Waypoint info element (HUDWPDINFO): the 2026-08-13 retail side-by-side (JOTAC 00TRa, identical pose) shows retail rendering "760 m to Alley Corner" where OpenNova renders "761 Marketplace" — same selected waypoint (the distances agree within truncation), so (a) our text omits the localized "m to" infix (an INDEXED string-table entry witnessed in the RevX02 strings blob next to "m to FARP"; the composing drawer's table/index is unwitnessed), and (b) the resolved NAME differs — the `get_waypoint_name @0x594630` raw-id vs +1-remap branch pick for gametype 0x30020 (or the mission-table source) needs a witness pass against the live JOTAC session. 2026-08-13 spawn probe additions: at the 32 m spawn waypoint retail shows the map's at-tip "032m" label (%03dm — the leg is live because `g_spinmapWpDistLabelOff` is BSS-zero and this hudpos authors no suppressor token — the 2026-08-14 pff extraction shows RevX02 carries no `SPINMAPWPDISTOFF` line; the on-screen anchor rides the modded layout) while the HUDWPDINFO text row is absent at spawn — a range or state gate on the info row to witness alongside the name/infix pass | A | OPEN | PAR-UI |
| D-HUD-26 | The flag-0x200 scoped terrain-ring overlay (`Render_RadarCompassOverlay @0x5c9740` — the mortar-class scope view) is unported: retail draws it when the equipped weapon can fire, is scoped, its def flags carry 0x200 and binoculars are down (gate `@0x5ca949`) — two ring primitives clipping a full 3D terrain re-render from a THIRD `Camera_ComputeThirdPersonView` call that frame (`@0x5c9841`, advancing the mode-0 shake IIRs once more) with the weapon-elevation offsets applied (`pitch -= slot counter[1]`, `yaw += counter[2]` `@0x5c98fc..0x5c9903`). OpenNova serves affected weapons the standard scope treatment and samples the shake twice per frame, not three times | A | OPEN | PAR-UI |

**D-HUD-24 host-serializer correction (2026-08-22):** the stale
“host-side 0x16 serializer emits zero scores/team table” residual in the row
above is superseded. `world::Match` now supplies every retail primary score and
points field, the exact 0/2/4-team shape, TKOTH alive-player bytes, and the
CTF/S&D/A&D authored-objective bytes; row zero remains zero. The remaining host
residuals are spectator/status-word production, not gameplay score authority.
`[orig: Server_BuildAndBroadcastScoreboard @0x50D960;
Game_CountAlivePlayersPerTeam @0x5001C0; ScoreRules_GetPrimaryScoreField (ex sub_52C850) @0x52C850]`

Closed 2026-08-29: **D-MNU-1, D-MNU-2, D-MNU-3, D-MNU-10, D-SND-1, D-SND-3, D-SND-4, D-SND-10, D-SND-13, D-LOADSCR-1, D-LOADSCR-6, D-LOADSCR-7** -> `PERMANENT` — the twelve rows that had carried "register candidate — ADR 0022" in their own disposition since the 2026-08-04 catalog tabling, ratified by the maintainer as one batch: each is a reimpl-structural (class C) choice whose rendered/audible/observable result equals retail's — the per-field `%VAR%` pass, the un-modelled per-play menu sound jitter, the authoring-side format strictness (menu and sound), the offline PLAYER_CLASS spin (deliberate 2026-07-11), the merged bank chain, the dialog FIFO, the ChuteFlap/FreeFall coalescing, the per-load SndProf.def parse, the coarser load-progress granularity, the unmodulated (MODULATE2X-neutral) loading background, and the uninterruptible synchronous SP/host map load — and porting each would reproduce mechanism, not behavior (register below; full entries: mnu/menu-re.md, audio/lwf-dbf-sound-re.md, interface/loading-screen-re.md).

De-tabled 2026-08-06 (the closed-row compaction — the table above holds
OPEN work only; full detail in the named record + git history):

Closed 2026-08-15: **D-LOADSCR-4** -> `FIXED` — the SP start-mission splash is a `LoadingScreen` mode: the held background, the blinking centered Impac22b `LT_Continue` line (512 ms white/`0xFF8080` pulse @ tick bit 0x200), the cursor-arrow quad at the live mouse position, key-queue-flush entry, any-key/any-mouse-button dismissal (input-only — the "sound completes" gloss refuted), the final background-only frame, and the fire-and-forget START_MISSION one-shot `[orig: show_start_mission_splash @ 0x520820, gate @ 0x525d38, call @ 0x525d48, release @ 0x525d52]` (full entry: interface/loading-screen-re.md; GUT `loading_screen_splash_test.gd`).

Closed 2026-07-24: **D-LOADSCR-3** -> `FIXED` — joiner spawn-gate hold: `world_loaded` no longer releases a joiner's loading presentation; the authoritative `join_admission_ready` / `join_deploy_pick_required` edges do, matching retail's two blocking waits to the S2C 0x1D spawn gate (full entry: interface/loading-screen-re.md; this row was omitted from the ledger when fixed — repaired 2026-08-15).
Closed 2026-07-09: **D-HUD-1** -> `FIXED` — Stance indicator = discrete cross-faded `HUDSTANCE` frames (IDB was `draw_minimap_compass_overlay` + oscarmike model it as a compass) (full detail: hud-re.md + git history).
Closed 2026-08-13: **D-HUD-2** -> `FIXED` — the IDB-misnamed stance widget remains frame-swap + fade, while the separately witnessed normal `HUDSPINMAP*` call is a real heading-up terrain/blip map drawn under the HUDDECLUT declutter gate at its authored level mask (`HUDDECLUT_SPINMAP` — the interim "compiled-in-true master switch" gloss corrected 2026-08-15; the whole-overlay master gate is the separate boot `/NOHUD` switch; the July "no in-HUD radar" gloss corrected too); the two are no longer conflated (full detail: hud-re.md + git history).
Closed: **D-HUD-3** -> `FIXED` — HUD design space is fixed 1024×768, scaled round-to-nearest (`Viewport_ScaleToVirtualCoords`) (full detail: hud-re.md + git history).
Closed: **D-HUD-4** -> `FIXED` — Health-bar fill WIDTH uses the capped `+92` ratio; fill COLOR uses an uncapped recomputed ratio (full detail: hud-re.md + git history).
Closed 2026-07-31: **D-HUD-7** -> `FIXED` — Crosshair spread now consumes the exact ERROR integer plus both signed live terms, `pitchBlend(+0x380)>>7` and movement/weapon-weight spread `(+0x384)>>7` (full detail: hud-re.md + git history) Witness: [orig: HUD_DrawCrosshair @ 0x592640; RoundData_SpawnRound @ 0x4ec0d0; Entity_UpdateInfantryPlayerBody @ 0x4b40e0].
Closed 2026-07-31: **D-HUD-9** -> `FIXED` — Crosshair visibility and the aimed ERROR triplet now share a bounded `Player_CanFireWeapon @0x5cf780` projection: promoted Scoped or Sighted (except SWITCHFROM), card-switch reload, camera ... (full detail: hud-re.md + git history).
Closed 2026-07-11: **D-HUD-10** -> `FIXED` — Crosshair anchors at the fixed design center (full detail: hud-re.md + git history).
Closed 2026-07-22: **D-HUD-15** -> `FIXED` — Weapon heat bar (`HUD_DrawWeaponHeatBar @ 0x599700`, ex "minimap" misnomer) drawer ported; the info feed now carries a real level (full detail: hud-re.md + git history).
Closed 2026-08-24: **D-HUD-25** -> `FIXED` — the MP end-of-round presentation is complete: both S2C 0x1D header forms (the non-team top-three names/scores form included), the overlay ladder + stat.mnu STAT screen, the toggled Show Score statistics panel (catalog row 99 `ShowScore`, F5, action 422), the joiner's `g_round_time_remaining` fold (0x0A sub-block 1, host projection `@0x4ffa81..0x4ffaca`), and the stat.mnu exit's round-cycle handoff (the post-STAT once-only latch `@0x5b864a`; the host's linger-expiry mission exit, reason 3 `@0x51db63`) (full entry: net-re §5.68).
Closed 2026-06-23: **D-PLAYERINFO-7** -> `FIXED` — `PLAYER_INFO` screen orchestration host wiring: `PlayerInfoMenuCompanion` (godot/game/player_info_menu_companion.gd) drives the live `player.mnu` screen (full detail: avatars-re.md + git history) Witness: [orig: PlayerInfo_PopulateNationalityList @ 0x55d8c0; PlayerInfo_HandleNationalitySelect @ 0x560600; PlayerInfo_HandleDivisionSelect @ 0x560690; populate_avatar_combo_list @ 0x560210].
Closed 2026-07-22: **D-PLAYERINFO-10** -> `FIXED` — Voice preview host wiring now binds `TESTPLAYERVOICE` and requests the selected avatar's `VOICE_%d` through `menu.lwf` `[orig: PlayerInfo_PreviewVoice @ 0x55ff70]` (full detail: avatars-re.md + git history).
Closed 2026-07-30: **D-PLAYERINFO-11** -> `FIXED` — Loadout ammo combos + weight readout + icons: witnessed 2026-07-30 (`@ 0x55e8b0`/`@ 0x55def0`/`@ 0x55f480`/`@ 0x55f1f0` decompiled (full detail: avatars-re.md + git history).
Closed 2026-08-15: **D-PLAYERINFO-1** -> `FIXED` — Packed character id now survives host/join networking and keys selected head+body world composition plus the first-person arms (the character's combo arms are retail's ONLY arms source: weapon.def `gfx1a`/`gfx1b` are discarded tokens `@0x5448d0`; no character arms → no arms). Each part receives its own raw `TEX_CAMO1/2/3` controls immediately before its retained draw, matching retail `[orig: lookup_entity_slot_and_pack_entry @0x57AD40; world head/body @0x5C7FEC/@0x5C800F; FP arms @0x4df05f/@0x4DF008/@0x4DF070; camo writers Avatar_Set{Head,Body,Arms}CamoCtrl @0x57A370/@0x57A390/@0x57A3B0]`. Follow-up 2026-08-17: D-RMAT-11 now consumes those raw values through retail's statically state-one modulo branch, fixing RevX02 `IndoArms.3di` selector 1 (full detail: avatars-re.md; the unresolved-id item-model residual rides D-NET-137).
Closed 2026-07-28: **D-SND-16** -> `FIXED` — Ambient marker eval RAN per render frame in `MissionAudio.tick` (full detail: lwf-dbf-sound-re.md + git history).
Closed 2026-08-12: **D-MNU-18** -> `PERMANENT` — Menu wheel scrolling, a deliberate reimpl addition ratified by the maintainer: retail's wheel plumbing is witnessed dead code (the shell bridge's direction-less event 0x100000B has NO consumer; the in-game bridge drops ticks entirely), so the reimpl scrolls one row per tick via `pump_mouse_wheel` — open popup exclusively, else the front-most row owner (register below; full entry: mnu/menu-re.md "Mouse wheel").

Closed 2026-08-10: **D-MNU-14** -> `FIXED` - SP mission-select population witnessed and ported: the mission table build `[orig: MissionList_ScanAndBuildFromFiles @ 0x563170 + Mission_BuildMapListFromPFF @ 0x562910, qsorted by Mission_CompareMapNames @ 0x5628e0]` lives in `engine/runtime/mission/mission_catalog` (titles from the sibling `.bin`'s [Info] TITLE, briefing text, the loose `*` flag, single-select game mode) and the shell's SP lists ride it with the Co-op-family filter, briefing cleared on populate / filled on selection and ACCEPT disabled until a pick `[orig: SinglePlayer_PopulateMissionList @ 0x561840 + SinglePlayer_MissionListEventHandler @ 0x561ed0 + SinglePlayer_RefreshAcceptOnActivate @ 0x561a20]`; the HOST screen's populate split off as D-MNU-17 (full entry: mnu/menu-re.md).

Closed 2026-08-10: **D-MNU-17** -> `FIXED` (core; the residues are listed in the record) - the host-screen populate chain witnessed (`init_host_settings_dialog @0x558960`; filter `@0x556fe0`; the ADD/REMOVE handler `@0x557c10`, an unowned tail chunk repaired; the table events `@0x557fb0`; start `@0x556d00`) and ported: title-else-filename rows, the stock-co-op (pure-SP) exclusion, the 13-way code -> category GAME_TYPE filter with ALL=255, the rotation table, ADD/REMOVE hide/restore and the non-empty-rotation START_GAME gate (`game_type_policy` ctest + `mp_lan_menu_seam` GUT; residues: per-item spin enablement, the rotation-cell toggle/double-click surfaces, the restriction lists, the config-word gate, rotation beyond its head) (full entry: mnu/menu-re.md).

Closed 2026-08-10: **D-MNU-15** -> `FIXED` - the combo closed face's authored-only gate left every RUNTIME-seeded combo blank; the face now draws `items[selected]` whenever rows exist, authored or runtime, matching retail's selection-showing CButtonWnd face `[orig: CComboWnd ctor @ 0x65be40, the +764 closed button]` (`menu_frame_compiler` pins the runtime-rows face; full entry: mnu/menu-re.md).

Closed 2026-08-10: **D-MNU-16** -> `FIXED` - the pump's claim walk claims the spin widget from its SPINUP/SPINDOWN rects too (shared `spin_arrow_hit_` with `spin_arrow_at`), the child-window claim retail gets for free `[orig: CSpinListWnd_CreateUpDownChildren @ 0x64b8b0]`; outside-authored arrows (mp.mnu GAME_TYPE) cycle by mouse again (`menu_frame_compiler` pins the outside-rect claim; full entry: mnu/menu-re.md).

Closed 2026-08-15: **D-CTRL-4** -> `PERMANENT` - retail F6 genuinely cycles the HUD declutter: the `huddetail` action (catalog row 50, dispatch code 19) first-match-shadows `hudcolor` (row 76, dispatch code 10, hidden from the rebind UI by the D-CTRL-2 flag gate) on the shared default F6, and code 19 is a live arm of `Input_HandleActionBinding_0 @0x4e0420` (the earlier no-op reading refuted 2026-08-15); the declutter cycle is ported so F6 drives `huddetail` here too, and the reimpl keeps the `hudcolor` row reachable by rebinding (register below; full entry: mnu/menu-re.md D-CTRL family + interface/hud-re.md).

Closed 2026-08-20: **D-SND-6** -> `FIXED` (stale row corrected: the divergence it described no longer exists) - `MissionAudio` runs retail's TRANSIENT model: at most `MIX_CHANNELS` = 8 physical voices, a drop-out `stop()`s its player, nulls the bound stream and releases the slot, a re-entrant is rebound and `play()`ed from the wave start `[orig: AudioChannel_ResetByHandle @0x767160 / AudioChannel_OpenSlotChecked @0x767060]` (GUT `mission_audio_test`); residual, still unwitnessed: whether a retail channel loops natively off an AUD1 flag or is re-registered per wrap (the mix kernel around `sub_7BD671`) (full entry: audio/lwf-dbf-sound-re.md).

Closed 2026-08-12: **D-SND-9** -> `FIXED` - the BPLN flags word rides the collision feed (`CollisionPlane::flags`) and the sound-occlusion entity clip selects per plane: a nonzero flags BYTE clamps the clip radius at 0 where flag-0 planes keep the raw (ray-2 - 0x8000) radius `[orig: byte test @0x538d00; clamped arm @0x538d4b, raw arm @0x538dd6]` (ctest `collision`; full entry: audio/lwf-dbf-sound-re.md).

Closed 2026-08-13: **D-SND-11** -> `FIXED` - the footstep pick reads the live `Entity::ground_target`, the `entity+0x28 groundEntity` link the resolve's ground probe stores unconditionally each tick `[orig: Entity_RaycastGroundHeightAndObject @0x525fd0; the +0x28 store @0x414370]`, null-on-miss like retail, the never-set `standing_on_entity` mirror deleted; walking on placed objects plays the single generic `SS*FootOBJ` pair exactly like retail (ctest `slot_sound`; full entry: audio/lwf-dbf-sound-re.md).

Closed 2026-08-15: **D-SND-12** -> `FIXED` - `AvatarDatabase` projects each character's packed id and `combo.head.sex` into the simulation's reset-stable character-traits table; player entities select `AiProfile::sound_profile_female` when the packed character id is female [orig: `Entity_GetProfileSlotSound @0x52831c`], unknown ids and NPCs keep the primary profile, both items.def profile names keeping the retail default fallback (`slot_sound`, `simassets_item_traits`, `avatars_data_test`; full entry: audio/lwf-dbf-sound-re.md).

Closed 2026-08-11: **D-SND-14** -> `FIXED` - the local player's death edge emits org2's body-model composite `sprintf("%s_%s", Entity_GetBodyModelPrefix(entity), "DEATH"/"DEATH_K")` as a named `SoundSlotEvent` (the by-name drain reproduces retail's miss = silence) `[orig: @0x4b4c4a-0x4b4c6a; SoundProfile_FindByEntityAndType @0x528180 over the g_entity_sound_type_table @0x82F548; prefix switch @0x5280F0 (anim-slot +0x374, 0->1->BM1)]`; NPCs keep slots 7/8; the record's old `"<DefName>_<Type>"` gloss corrected (ctest `slot_sound`; full entry: audio/lwf-dbf-sound-re.md).

Closed 2026-08-12: **D-SND-15** -> `FIXED` (premise corrected) - the surface sampler walks the mission `.til` array and a covered position returns the tileset `.TSD` table entry for the tile index `[orig: walk @0x6065ca-0x606601, read @0x60660c; table fill Terrain_ParseTsdRow @0x604c00 over the 20-name TSD table @0x8493f0, probe @0x60c5d3]`; no shipped JO install carries a `.TSD`, so retail tiles read TSD_NULL and the old "reads the underlying charmap" premise was ours; residue: the BMS tile-set-name override of the tilestrip pair (ctests `til_tsd` + `slot_sound`; full entry: audio/lwf-dbf-sound-re.md).

Closed 2026-08-16: **D-HUD-11** -> `FIXED` - attach-label nearest-entity selection consumes the same complete `Player_CanFireWeapon @0x5cf780` verdict as the body/HUD aimed row (alive/equipped, passenger-or-borrowed-UseGun seat, camera/binocular, reload-card, promoted Scoped/Sighted/SWITCHFROM, movement/air/water, ForceScoped); the tick-committed promoted-scope bit keeps the query frame-stable while camera toggles apply immediately (`simulation_test`; full entry: interface/hud-re.md).

Closed 2026-08-11: **D-HUD-12** -> `FIXED` - attach labels lay out through the ported CGameFont engine with the witnessed bold Arial label font at the slot scale (`HUD_InitAllFonts @0x51ee20`; `g_hudLabelFontBold @0xb4c394`, ex "fontObj"), box arithmetic ported verbatim (full entry: interface/hud-re.md).

Closed 2026-08-10: **D-HUD-13** -> `FIXED` - the master overlay-color writer is witnessed: `g_hudActiveColor @0x24c1868` = `g_hudColorTable[cfg_hud_color_index]` OR-ed with `0xFF000000` (`HUD_InitTeamColorTable @0x51f240`; the input case 10 cycle `@0x49afc7`; config token `hud_color_index`, default 2 `@0x54d28b`), table slot 2 refreshed per frame from hudpos `hud_textcolor` (`@0x5a8100`); the reimpl base was already exact under the default scheme, the dim transform stays ported, and the non-default schemes landed 2026-08-13 (full entry: interface/hud-re.md).

### Format ports — mission `.mis`, LW `.3di`, particles `.ptl`

Sources: [mission/mis-format-re.md](mission/mis-format-re.md),
[threedi/3di-lw-format-re.md](threedi/3di-lw-format-re.md),
[particles/ptl-format-re.md](particles/ptl-format-re.md). IDs minted this train (see
"Normalized prose-only catalogs" below).

Minted-and-closed 2026-07-07 (the `.mis` parity pass — the original Nile
editor's importer `misldr.dll` grilled after a user repro: heights all wrong
opening our export in the original editor): **D-MIS-4** -> `FIXED` — `.mis`
item heights are terrain-RELATIVE unless `height_lock 1` declares the z
ABSOLUTE with `extra_bheight` carrying the baked base height
(`[orig: MisLdr_ParseMisLine @ 0x100017b0 — height_lock→rec+356,
extra_bheight→rec+292; MisLdr_WriteNileProjectXml @ 0x10004930 — scene
Y = z/65536 − (lock ? bheight/65536 : 0); both misldr.dll]`); our exporter
wrote absolute BMS z with neither, floating every object by the local
terrain height. Fixed: `height_lock 1` per BMS-sourced item +
caller-sampled `extra_bheight` (the former mission exporter supplied terrain
heights in write order). **D-MIS-5** -> `FIXED` — reader/writer asymmetries
corrupted round-trips (base-0 `strtol` parsed zero-padded numerics as
OCTAL vs the witnessed base-10 `atol`; `fog_level`/`water_level` u32-out
u16-truncate-in; `gen_def_val1..4` write-only; parse defaults rewrote
zero-valued fields on all 1365 retail-00TRg entities); fixed to base-10 +
symmetric fields + unconditional emission of defaulted keys —
`.bms`→`.mis`→`.mis` is byte-idempotent on the retail fixture. Full
witness: [mission/mis-format-re.md](mission/mis-format-re.md).

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-MIS-2 | `weapon_availability` emitted empty + skipped on read; the semantics are now grilled (SEMANTICS CLOSED 2026-07-18 in mis-format-re.md — the per-map `{name, statusByte}` weapon-rules list, `[orig: build_item_restriction_table @ 0x54ddb0]`, net-re §5.63); the `.mis` text section stays empty-emitted pending the dfx2med grammar grill (D-MIS-3); the tuple names `{name, ammoPri, ammoSec, flags}` are applied to `bms.h` + the loadout panel (2026-07-30) | A | WITNESSED-READY-DEFERRED | PAR-WORLD |
| D-MIS-3 | Full `dfx2med.exe` `.mis` grammar unmapped (hand-authored / legacy variants beyond the writer subset) | B | NEEDS-RE | PAR-WORLD |

Retired from the tables 2026-08-28: **D-3DILW-1**, **D-3DILW-2**, **D-3DILW-3** - the
`threedi_lw` parser they describe never landed (PR #45 closed) and ADR 0027 removed the
model IR it converted into, so there is no OpenNova behavior for the rows to diverge from;
the three deferrals (v8 branch, textures, SAF/KSA playback) stay cataloged in
[threedi/3di-lw-format-re.md](threedi/3di-lw-format-re.md) §4 and ride whenever an LW
import is revived. D-3DILW-1's v8 branch overlaps the 3DI/GP audit surface only at the
container-detection seam.

De-tabled 2026-08-06 (the closed-row compaction — the table above holds
OPEN work only; full detail in the named record + git history):

Closed 2026-08-12: **D-MIS-1** -> `FIXED` - `.mis` `begin item` records classify into the four pools through the witnessed items.def TYPE mapping `[orig: MisLdr_WriteNileProjectXml @ 0x10004930, misldr.dll]`: `parse_mis_text_to_bms` takes an embedder-built type resolver (the mission format lib stays def-free), routes each record through `entity_kind_for_item_type` (the 185k-entity/114-mission empirical 1:1 pin), and the idempotency ctest pins classification + byte round-trip; callers without an items.def keep the generic pool explicitly; the `dfx2med.exe` grammar confirmation stays with D-MIS-3 (full entry: mission/mis-format-re.md).

Closed 2026-08-22: **D-PTL-21** -> `FIXED` — `ParticleFrameCompiler` now reproduces retail's projected emitter-sphere intervals, Z→X→Y inclusive recursive partition, rendered-entry recursion, per-leaf raw particle depths, and literal non-stable `Utility_QuickSortWithAux` tie behavior. The portable adversarial contract pins both spatial leaf separation and equal-depth swaps `[orig: CParticleManager_TransformToViewSpace @ 0x5ecc50; CParticleManager_RecursiveSortAndRender @ 0x5ec980; CParticleManager_RenderBatch @ 0x5e9890; Utility_QuickSortWithAux @ 0x53d470]` (full detail: particles/ptl-format-re.md).

Closed 2026-07-14: **D-PTL-2** -> `FIXED` — Per-command ArrayMesh surfaces could exceed the 256-surface cap and Godot could reorder equal-priority transparent surfaces, violating the engine-wide packet order under casing/impact churn (full detail: ptl-format-re.md + git history).
Closed 2026-07-14: **D-PTL-3** -> `FIXED` — `mod2x` approximated `DESTCOLOR`/`SRCCOLOR` over Godot `blend_mul` (full detail: maturity-program.md + git history).
Closed 2026-07-14: **D-PTL-4** -> `FIXED` — `bump`/`bumpadd` used the wrong rotation axis and saturated encoded light bytes (full detail: ptl-format-re.md + git history).
Closed 2026-07-14: **D-PTL-5** -> `FIXED` — `distort` used an arbitrary fixed-strength screen-texture offset (full detail: ptl-format-re.md + git history).
Closed 2026-07-14: **D-PTL-6** -> `FIXED` — Atlas registrar, allocator, type preprocessing, and inset were approximated by per-emitter shelf packing (full detail: ptl-format-re.md + git history).
Closed 2026-08-12: **D-PTL-7** -> `FIXED` — fx2ssn initial orientation now comes from a
portable world-to-heightfield normal query sharing the generated terrain normal map's centered
raw16 differences, 1/256 height scale, unit-up normalization, and quadrant-lock tap policy; the
Godot boundary maps `{x,z,up}` once to `{x,y,z}`. `[orig: WacScript_SpawnEffectAtSsnEntity
@0x4F23A0; Terrain_GenerateNormalMap @0x603210; scale @0x7C6950]` (full detail:
particles/ptl-format-re.md §4/§8).
Closed: **D-PTL-13** -> `FIXED` — Parser hard-failed a whole .ptl on any unrecognized top-level or `=`-less line where retail ignores unclaimed lines (full detail: ptl-format-re.md + git history).
Closed 2026-07-14: **D-PTL-14** -> `FIXED` — Flipbook frame naming was guessed, causing missing shipped frames and procedural-fallback strobing (full detail: ptl-format-re.md + git history).
Closed 2026-07-14: **D-PTL-15** -> `FIXED` — Static-batch buildings/decorations/no-anim vehicles formerly lost ITEMS.DEF `particlefx` because they had no per-entity Node/model handle (full detail: correspondence.md + git history).
Closed 2026-07-14: **D-PTL-16** -> `FIXED` — Casing and ballistic-impact particles were suppressed to avoid per-emitter Node/material/atlas/upload churn (full detail: correspondence.md + git history).
Closed: **D-PTL-17** -> `PERMANENT` — The PlayerControl occupancy effect runs class-wide in the port (every `attrib & 0x40` item), while retail reaches the spawner only through the `CHel`/`cpln` class updater (register below; full detail: correspondence.md + git history).
Closed: **D-PTL-18** -> `PERMANENT` — Retail's atlas skyline placer can lower taller columns and overlap earlier rects, and its uncapped 2.5-px inset inverts tiny-rect UV windows (`CParticleAtlas_TryPlaceEntry @ 0x5e2be0`) (register below; full detail: ptl-format-re.md + git history).
Closed: **D-PTL-19** -> `PERMANENT` — Retail carries authored `flip_frames` into frame registration with no witnessed reimpl-style normalization (register below; full detail: ptl-format-re.md + git history).
Closed: **D-PTL-20** -> `PERMANENT` — Retail consumes one trailing curve modifier (`reverse` OR `inverse`) (register below; full detail: ptl-format-re.md + git history).
Closed 2026-07-16: **D-PTL-22** -> `FIXED` — PTL section tags and known keys were case-sensitive in the port while retail uses `_stricmp` throughout the parser family (full detail: ptl-format-re.md + git history).
Closed 2026-07-16: **D-PTL-23** -> `FIXED` — Curve-table duplicate selection treated TableDef+0x248 as owner and always selected the first match (full detail: ptl-format-re.md + git history).

### VFS / PFF mount stack — [vfs/vfs-pff-mount-re.md](vfs/vfs-pff-mount-re.md) (D-VFS catalog; PAR-R7)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|

The mount stack, resolution order and read disciplines are **MATCHING** vs retail
(`PFF_Open @ 0x7682e0`, `PFF_FindEntry @ 0x7685d0`, `PFF_LoadFileToMemory @ 0x768920`);
D-VFS-1/2/3/5/7 are closed and D-VFS-4/6/8/9/10/11 ratified permanent.

Closed 2026-08-29: **D-VFS-5** -> `FIXED` — corpus-checked, faithful-nothing: every entry `flags` word in the five retail JO archives is 0 (`language.pff` 3913, `localres.pff` 2812, `resource.pff` 2565, `expansion/jox01/jox01.pff` 1481, `jox01L.pff` 805 = 11,576 entries, none bit0-encrypted), so retail's whole-file-only decrypt (`PFF_LoadFileToMemory @ 0x768920` vs the never-decrypting `PFF_ReadFilePartial @ 0x768ab0` / `FileSystem_Read @ 0x75abe0`) has no reachable surface in JO — and our VFS exposes no partial or streaming read at all (`vfs.cpp` routes every archive read through `pff_extract`). Full detail: vfs-pff-mount-re.md.

D-VFS-4/6/8/9/10/11 are ratified permanent decisions (register below). Closed 2026-07-05:
**D-VFS-2** -> `FIXED` — `Vfs::mount_game` defaults to the witnessed fixed boot
table (`VfsArchiveDiscovery::RetailTable`: language/localres/resource.pff in
slot order, extra archives never mount, pinned by
`test_mount_game_retail_table` `[orig: PFF_OpenAllArchives @ 0x4a4310, table
@ 0x829f90]`). Explicit resource catalogs may request `ScanAll` for arbitrary
mod archives; the retired extraction C ABI/importer no longer owns a separate
policy (ADR 0038).
(D-VFS-1/-3/-7/-10/-11 are closed — see the dated closure lines below; not repeated here.)

De-tabled 2026-08-06 (the closed-row compaction — the table above holds
OPEN work only; full detail in the named record + git history):

Closed 2026-07-17: **D-VFS-1** -> `FIXED` — Per-query `VfsLookupPolicy` now preserves the session default while implemented foliage/UI consumers force loose-first and local/network BMS probes + reads force archive-only (full detail: vfs-pff-mount-re.md + git history).
Closed 2026-07-17: **D-VFS-3** -> `FIXED` — Runtime retail lookups preserve the full relative query (case-insensitive loose subdir walk (full detail: vfs-pff-mount-re.md + git history).
Closed 2026-07-17: **D-VFS-7** -> `FIXED` — Archive lookup now mirrors the 31-byte query cap + both-sides ASCII uppercase and exact trailing-space significance (full detail: vfs-pff-mount-re.md + git history).
Closed 2026-07-17: **D-VFS-10** -> `PERMANENT` — Reimpl rejects rooted/drive-qualified/ADS/`..` queries and symlink escapes from a mounted loose root, where retail constructs an unchecked path (register below; full detail: vfs-pff-mount-re.md + git history).
Closed 2026-07-30: **D-VFS-11** -> `PERMANENT` — ONED-managed loose runs (`--loose-root`, passed by ONED's Run OpenNova loose action) fall back to the selected loose directory when the fixed boot table opens zero archives, where retail aborts subsystem init (register below; full detail: vfs-pff-mount-re.md + git history).

### Credits (CBIN) — [credits/cbin-re.md](credits/cbin-re.md) (D-CBIN catalog; PAR-R5, PARTIAL)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-CBIN-1 | Credits `~C`/`~F`/`~J`/`<CR>` markup consumers (the retail scroller) not yet witnessed; D-MNU-6 custom-font/image resolution rides here | B | NEEDS-RE | PAR (credits) |

CBIN codec (magic 0x4E494243 + 20-B header + ROL32/XOR cipher `@0x75e348`) is
**MATCHING** vs `engine/formats/cbin`, witnessed read-only via raw disasm (no IDB write).

De-tabled 2026-08-06 (the closed-row compaction — the table above holds
OPEN work only; full detail in the named record + git history):

Closed: **D-CBIN-2** -> `RESOLVED` — Read path CONFIRMED: 8 rol-7 cipher sites in the CBIN codec region (0x75e158-0x75e914) incl (full detail: cbin-re.md + git history).

### Terrain — [terrain/terrain-re.md](terrain/terrain-re.md) (D-TERRAIN catalog; PAR-R1, PARTIAL)

Fresh re-grill 2026-07-13 closes the remaining top-tier shader stand-ins;
a stage-3 correction landed 2026-07-15. The reimpl integer-normalizes DBlend,
builds the independent base/far custom mip chains, and executes the exact
t0..t5 ps.1.4 splat arithmetic. The splat's t3 is the authored second detail
pair (`polytrn_detailmap2` ⊕ `polytrn_detailmapdist2`) at its own
`detail texture density2` — the 07-13 reading that bound the generated
detailmap-B coefficient there produced non-retail dark spots; that generated
map belongs to the unported ps.1.1 tiers at stage 7
[`orig: stage bind @ 0x6043ff; texcoord density2/density @ 0x609810`]. A separate, lock-aware
heightfield-normal atlas now reconstructs bare cached-tile alpha as the
byte-quantized heightfield/light DOT3; the base tile draw discards authored
colormap A. The invented camera-distance normal crossfade, colormap-alpha sun
mask, and final terrain-tint multiply were removed; type-0 fog now uses eye
depth. This closes D-TERRAIN-5. Runtime now hosts the dynamic producer as a
128-layer, 256×256 current-frame page cache shared by terrain and detail
foliage. Its ordered page result is base RGB/A0, mission `.til` source-over
RGBA, additive heightfield/DOT3 A, then supported selected-LOD static-model
silhouettes that modify A only. D-TERRAIN-6 closes the base LOD/fog/order and
D-TIL-3 closes the previously dropped overlay target-alpha recurrence;
D-TERRAIN-7 tracks the remaining scorch/ordered contributions, exact
refresh cadence, and final RT edge/address/mip
behavior. The full PROJSHAD audit closed per-technique pass admission/blending,
skinned rigid collapse, one-sided culling, and overlap z ordering. The former terrain-only
directional static-shadow surrogate and its foliage omission are retired.
D-TERRAIN-9 was retired with the ONED terrain preview (ADR 0037).
Closed 2026-08-13: **D-TERRAIN-8** → FIXED — the below-water terrain
modulation is hosted: the terrain frame stamps `below_water` from the render
eye vs the live water height and the shared surface include swaps the ps.1.4
stage-3 dp3 input to the water module's per-frame noise texture at
`source × 8/512` (`colormap_uv × 16` for the normalized 1024 atlas), per
the full selector decode (terrain-re.md underwater
section — detail2-less splat maps faithfully get no modulation; the
`saturate(4·t3²)·t0.a` PSShadow pair serves only the unported ps.1.1 tiers).
ctest `terrain_frame_compiler` + GUT `terrain_shader_contract_test` /
`terrain_underwater_modulation_test`.
A 2026-07-14 coordinate-basis audit also minted and closed D-TERRAIN-10: the
reimpl now preserves EnvFile's direct retail getter tuple `g=(g0,g1,g2)` and
reproduces PolyTrn's D3DCOLOR packing as GPU RGB `(g2,g0,g1)`, reimpl `(z,x,y)`.
The former `(x,z,y)` mapping swapped the horizontal DOT3 axes; flat tests could
not expose it, so the correction is pinned by non-flat 08:00 slope vectors.

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-TERRAIN-7 | Retail's t0 is a dynamically composed per-tile render target. Runtime hosts a current-frame 128-layer 256×256 cache shared by terrain and foliage: base RGB/A0, ordered `.til` source-over RGBA, additive TrnNMap/DOT3 A, and static selected-LOD/all-ROBJ A-only projections. Static source lifecycle follows destruction, husk replacement, and runtime transform revisions; all diffuse-alpha frames, content stamps, LRU/generation safety, required-overlay source readiness, and page-local unsupported attribution are typed and tested. The former terrain-only directional surrogate is retired. The max-quality c7/c8 mapping is exact: the packed cache record and D3D foliage axis permutation reduce to the shared `TerrainTilePageProjection`, now consumed by terrain, foliage, MATCHTERRAIN, and the static-shadow raster with no failed-VS compatibility API (`@ 0x60A220..0x60A34F`, uploads `@ 0x6006AB..0x600704`). The 2026-08-23 source-complete PROJSHAD audit admits `_FFP` material blending only, forces the 15 shader declarations opaque, suppresses tracer/flag/glass no-pass surfaces, reduces skinned casters through the collector's identical matrix slots, and applies retail CCW/two-sided culling plus LESSEQUAL z writes. The material-animation leg now shares ordinary rendering's AlphaGen/full row-vector UV evaluator and time/control diffuse-frame selector at the frame tick, with the full captured CTRL array in caster identity and worker-state refresh that does not manufacture a resident spatial-page miss `[orig: submit tick @ 0x5DAD9D; batch CTRL snapshot/restore @ 0x5D91AB..0x5D91DE / 0x5DA1B8..0x5DA1FD; consumer @ 0x58DB80]`. Whole-process CTRL/RNG ordering remains D-3DI-2. The final temporary-blue state is closed by a live D3D9 state/readback probe: RGBA writes, ONE/ONE, no separate alpha, and `(0,0,0,tempBlue)` preserved all observed RGB bytes while installing temp blue in zero destination A; the portable pixel equation is tested. This rejects the old speculative low-sun density controls: the shipped pass sources contain no sun-angle opacity, `PolyTrn_SunToBlendRatioColor` is lower-path-only, and `dword_319FBB8` has no live writer. Scorch updates, remaining ordered contributions, retail cadence matching, and final RT edge/address/mip behavior remain open. Retail cadence itself is witnessed: `terrain_cache_evict_lru @ 0x604600` invalidates the oldest stale visible page after each ~5 s TOD epoch; OpenNova still uses coarser DOT3-byte content epochs. The composite order is DOT3 then silhouettes (`@ 0x60D794..0x60D7C0`, skip gate `@ 0x60E1CE`) `[orig: Terrain_CollectAndRenderTileModels @ 0x60D250; all-matrix submit @ 0x60D926..0x60D971; tile composite @ 0x60E0C6..0x60E19D; hit compare @ 0x60DAD1; TOD stamp @ 0x60DBC0]` | B | OPEN, narrowed producer gap | terrain/foliage re-grill |

The build → mesh-simplify → CPT data path remains byte-identical across the
fixture corpus, and all 16 LOD sublevels now pin the recovered eight-family
selector. The record remains PARTIAL because D-TERRAIN-7 is an open runtime
rendering gap. D-TERRAIN-1/-9 retired with the ONED preview,
D-TERRAIN-2/-3/-5/-6/-8/-10/-11 are fixed, and D-TERRAIN-4 is the runtime
safe-query boundary.

De-tabled 2026-08-06 (the closed-row compaction — the table above holds
OPEN work only; full detail in the named record + git history):

Retired 2026-08-24: **D-TERRAIN-1** — the deleted ONED terrain preview was the sole live-sculpt shader consumer, so its edit/runtime split no longer exists (full detail: terrain-re.md + git history).
Closed 2026-07-06: **D-TERRAIN-2** -> `FIXED` — Shared surface include stacked TWO ×2 detail-normal factors on the splat (gobj-era chimera) (full detail: terrain-re.md + git history) Witness: [orig: PolyTrn_PS14SplatNormalMap source aPs14TexldR0T0T_0 @ 0x7dece0; compile_terrain_pixel_shaders @ 0x605260].
Closed 2026-07-07: **D-TERRAIN-3** -> `FIXED` — Below-horizon region: cameras see past the sky dome's 1024-unit rim to the raw viewport background (full detail: terrain-re.md + git history) Witness: [orig: @ 0x677100].
Closed: **D-TERRAIN-4** -> `PERMANENT` — The ported terrain raycast's safe-query bounds (`terrain_raycast.h`): game consumers report no-terrain/no-hit outside valid data where retail clamps the cell to the grid edge (full detail: terrain-re.md + git history) Witness: [orig: OOB masks @ 0x31a0010/0x319fc0c] Witness: [orig: Terrain_SeamFlags_* @ 0x31a17f0..].
Closed 2026-07-13: **D-TERRAIN-5** -> `FIXED` — Top ps.1.4 inputs and fog were stand-ins: heightmap normal was misused as t3, raw near/far textures were camera-crossfaded, DBlend was unnormalized, authored-detail coefficient/custom mips and the ... (full detail: terrain-re.md + git history).
Closed 2026-07-13: **D-TERRAIN-6** -> `FIXED` — LOD/fog/overlay base-pass semantics: exact clamped `lod_sub / 2` eight-family selection, type-0 eye-depth vs linear radial fog, and ordered overlay color before lighting; the later target-alpha recurrence correction is D-TIL-3 (full detail: terrain-re.md + git history).
Closed 2026-07-14: **D-TERRAIN-10** -> `FIXED` — EnvFile preserves the direct `Environment_GetLightDirectionFloat` tuple `g`, not Godot/world XYZ (full detail: terrain-re.md + git history).
Closed 2026-08-17: **D-TERRAIN-11** -> `FIXED` — Retail terrain detail UV1 is `source × polytrn_detaildensity / 512`; runtime mesh UVs, authored detail2, and the underwater stage-3 swap share that source-grid conversion (full detail: terrain-re.md + git history).
Retired 2026-08-24: **D-TERRAIN-9** — the deleted ONED terrain preview was the only raw derived-input consumer; no runtime divergence remained.

### Tiles — [tiles/til-re.md](tiles/til-re.md) (D-TIL catalog; PAR-R3)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|

Overlay entry (12 B), atlas UV, flip/rotate flags, half-texel shift, Z negation,
and the 128-LRU cache are **MATCHING** vs retail `PolyTrn_RenderTile @ 0x60df0d`.

De-tabled 2026-08-06 (the closed-row compaction — the table above holds
OPEN work only; full detail in the named record + git history):

Closed: **D-TIL-1** -> `FIXED` — `TIL_FLAG_OUTLINE` (0x08): the LINELIST outline is jodemo-only; retail JO's render (`render_water_quad @ 0x604700`) omits it and so do we (flag preserved for round-trip, no outline drawn) (full detail: til-re.md + git history).
Closed 2026-07-15: **D-TIL-2** -> `FIXED` — `ROTATE_90` was the CW transpose `(v, 1−u)`; retail rotates CCW `(1−v, u)` (corner cycle @ `render_water_quad 0x6047d4..0x604806`) (full detail: til-re.md + git history).
Closed 2026-08-17: **D-TIL-3** -> `FIXED` — The page composer now reproduces overlay render-target alpha recurrence before the additive DOT3 alpha pass, instead of blending `.til` RGB while retaining bare-ground alpha (full detail: til-re.md + git history).
Closed 2026-08-20: **D-TIL-4** -> `FIXED` — Flip/rotate composition order: retail mirrors the corner UVs first and applies `ROTATE_90` as a corner-assignment cycle over the mirrored values (= rotate-then-flip in sampling-function form); flip-then-rotate drew rotate+single-flip tiles (`0x05`/`0x06`) 180° off — the 00TRa driving-course fork the `00tra-tire-marks-retail` fixture exposed (MAE 16.35 → 12.17) (full detail: til-re.md + git history).

### Foliage — [foliage/foliage-re.md](foliage/foliage-re.md) (D-FOLIAGE catalog; PAR-R2)

The 2026-07-14 LOW-pass audit found the secondary submission; the 2026-07-15
grill corrected its fade and blend: below distance 33, retail re-submits the
same geometry after HIGH at the SAME unscaled c6 fade under strict
`D3DCMP_LESS` (not wireframe/fill mode), and every detail draw alpha-blends
`SRCALPHA/INVSRCALPHA` with tested alpha `t0.a × v0.a`. The 0.1 fade scale
rides the whole-call reflection flag (`arg_8 = reflectionEnabled`, pushed at
`Terrain_RenderSceneWithReflection @ 0x5c95c1/0x5c9661`), which also forces
LOW for all patches — it is the water-reflection scene's dimmed foliage, not
a main-scene state. [orig: `Foliage_RenderFarPatches @ 0x60a171..0x60a19c,
0x60a497..0x60a4ae, 0x60a659..0x60a694`; `Foliage_SetupFarSlotDraw @
0x6008fc..0x600912`; `Foliage_LoadDefAssets @ 0x60141f..0x601427`;
`SetRenderState` wrapper `0x67cac0..0x67caea`]

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-FOLIAGE-7 | Detail t1 is retail's composed per-tile render target. Runtime detail borrows the terrain frame's exact ready page from the hosted 128-layer 256×256 cache: base RGB/A0, ordered `.til` source-over RGBA, additive heightfield-DOT3 A, and static A-only projections. Retained prior-frame pages cannot win lookup, and the former terrain-only directional surrogate is retired, so alpha-tested foliage and terrain consume the same page result. The required-VS c7/c8 branch, pre-wind coordinate, per-technique PROJSHAD admission/blending, skinned rigid collapse, one-sided culling, overlap z ordering, shared AlphaGen/full-UV/time-or-control diffuse-frame evaluation, and live-probed zero-RGB ONE/ONE temporary-blue composite are exact. Remaining ordered contributions, refresh cadence, and final RT edge/address/mip behavior remain open (the retail cadence is witnessed at D-TERRAIN-7; whole-process CTRL/RNG ordering remains D-3DI-2). | B | OPEN, narrowed; shares D-TERRAIN-7 producer | terrain/foliage re-grill |
| D-FOLIAGE-9 | Silhouette driver membership: retail walks visible sector entities with `test_sector_entity_occlusion @ 0x5c4610`; the reimpl stands in a camera-frustum test for the surviving stance-gated anchors (class selection itself is closed — D-FOLIAGE-11). Overlapping reimpl anchors retain distinct submissions but coalesce same-frame refreshes of one `(slot, cell key)` | C | OPEN, narrowed to visibility membership (2026-07-16) | foliage runtime reimpl mapping |
| D-FOLIAGE-10 | Retail inserts each immediate MODEL depth-mask draw after the initial sector flush and before later entity/foliage consumers; the reimpl's transparent-pass depth sorting cannot cull already-drawn farther detail under a nearer mask or reproduce every insertion point. The secondary LOW's strict `LESS` is now emulated exactly (high-pass cutoff discard on identical geometry), and both detail passes blend `SRCALPHA/INVSRCALPHA` at the shared fade. The reflection-scene LOW-only `fade × 0.1` pass is unhosted — and since the #30 mirror carries foliage (one shared world), the reimpl's reflected foliage draws at FULL main-scene fade where retail dims it to a tenth at forced LOW, so our water reflects visibly brighter vegetation. | C | OPEN, narrowed to order/reflection reimpl mapping (state half retired 2026-07-15) | foliage runtime reimpl mapping |

The fresh core is literal-vector matching for both generators: shared
0xA55B1EED ROL-hash stream, 36 candidates, high15=X/low15=Z-top keys, the
match-remapped authored foliage-map gate, 42-unit detail fade/pass split, four silhouette cells,
21-per-cell cap, eight-sample ground fit, distance alpha refs, and `:fd`.
The persistent detail cache is strict signed-age LRU with draw-before-update
miss visibility; the distant cache is 1000 entries per definition with the
exact `((sceneCounter + 2×slot) & 7) == 0` refresh phase. Host
slot/key/revision identities, same-frame submission ordering, and post-submit
eviction now preserve duplicate draws and in-place terrain mutations reset both
caches. Every surface of every LOD0 submesh survives aggregation. The parsed
`shadow` attribute's bit 1 has no generator/draw consumer, so explicit host
shadow-off is matching rather than a divergence.

De-tabled 2026-08-06 (the closed-row compaction — the table above holds
OPEN work only; full detail in the named record + git history):

Closed 2026-07-13: **D-FOLIAGE-1** -> `FIXED` — The prior per-corner colored-emitter premise came from the inverted port and is retracted (full detail: foliage-re.md + git history).
Closed: **D-FOLIAGE-2** -> `FIXED` — Detail fragment arithmetic differs from the exact lightmap-blend chain `t0 × (t1 × (t1.a·c1 + c0)) × v0 × 8` (full detail: foliage-re.md + git history).
Closed 2026-07-13: **D-FOLIAGE-3** -> `FIXED` — Tier wind was missing/wrong-axis: detail needs source-height-weighted render-Z sway (full detail: foliage-re.md + git history).
Closed 2026-07-13: **D-FOLIAGE-4** -> `FIXED` — The 2026-07-08 runtime inverted the tiers: it treated detail as a ground-patch tier and the distant sector-entity path as a colored model tier (full detail: foliage-re.md + git history).
Closed 2026-07-14: **D-FOLIAGE-5** -> `FIXED` — Exact model-own `:fd` chain: wrapped `(4C + cardinals + 2×diagonals) >> 4` alpha (full detail: foliage-re.md + git history).
Closed 2026-07-14: **D-FOLIAGE-6** -> `FIXED` — The first fresh pass treated uploaded c6 black as final color and missed the downstream SRC=ONE/DEST=ONE combiner plus retained strict-alpha Z write (full detail: foliage-re.md + git history).
Closed 2026-07-14: **D-FOLIAGE-8** -> `FIXED` — Retail candidate exclusion linearly scans the shared mission .til array with inclusive 16x16 entry AABBs and a radius-2 candidate square unless attrib bit 0 FORCE_ON (full detail: foliage-re.md + git history).
Closed 2026-07-16: **D-FOLIAGE-11** -> `FIXED` — The reimpl anchored the MODEL/depth-mask tier on EVERY placed mission object (full detail: foliage-re.md + git history).
Closed 2026-07-17: **D-FOLIAGE-12** -> `FIXED` — Two retail gate samplers: detail = flat 1024-wrap (`Terrain_GetSurfaceTypeAtFixedPoint @ 0x6066d0`), MODEL = sector-grid-routed (`Foliage_SampleFoliageMapMask @ 0x606620`) (full detail: foliage-re.md + git history).
Closed 2026-07-16: **D-FOLIAGE-13** -> `FIXED` — Detail-cell collection was a standalone radial 42u-disc walk (full detail: foliage-re.md + git history).

### Fonts — [fonts/fnt-re.md](fonts/fnt-re.md) (D-FNT catalog; PAR-R4)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|

The `.fnt` header, glyph-table and text-engine contract are **MATCHING** vs retail
`CGameFont_MeasureText @ 0x674e70` / `CGameFont_DrawText @ 0x6752c0`; D-FNT-1..4 are all
closed.

De-tabled 2026-08-06 (the closed-row compaction — the table above holds
OPEN work only; full detail in the named record + git history):

Closed 2026-08-23: **D-FNT-3** -> `FIXED` — Offset +12 (`hdr3`) is the inter-glyph SPACING term, not a shadow offset: both the measurer and the drawer advance by `glyph_width + (glyph_spacing - 1) * scale` and the measured width strips the trailing pad `[orig: CGameFont_MeasureText @ 0x674e70 this+356; CGameFont_DrawText @ 0x6752c0]`; the drawer's shadow is a format flag plus fixed sub-pixel offsets. `fnt_font_t.shadow_offset` -> `glyph_spacing`, `FntResource.get/set_glyph_spacing`, pinned by `fnt_roundtrip` + `strings_encoding_test.gd` (the ledger row lagged the record by six days; full detail: fnt-re.md + git history).
Closed: **D-FNT-1** -> `FIXED` — Offset +4 is the design-width scale reference, not a version (full detail: fnt-re.md + git history).
Closed: **D-FNT-2** -> `FIXED` — The per-font design scale `800/designWidth` was not retained (full detail: fnt-re.md + git history).
Closed 2026-07-19: **D-FNT-4** -> `FIXED` — cp1252 specials: `to_font_file` keyed glyphs at raw bytes while display text was Unicode, so Godot substituted a SYSTEM font where retail selected the `.fnt` slot by byte (full detail: maturity-program.md + git history).

### Boot-required resources — [required-resources.md](required-resources.md) (D-BOOT catalog; R8/ENG-6)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|

De-tabled 2026-08-06 (the closed-row compaction — the table above holds
OPEN work only; full detail in the named record + git history):

Closed 2026-07-07: **D-BOOT-1** -> `FIXED` — Menu/game music bank resolution: retail hardcodes `MENUMUS.SBF/.BIN` + `GAMEMUS.SBF/.BIN` (`M<exp>`/`G<exp>` under an expansion) (owning doc: required-resources.md — BOOT has no `*-re.md` record; the fix itself is in git history) Witness: [orig: Expansion_LoadAssets @ 0x4a4798/@ 0x4a4906; AudioVM_OpenContextFile @ 0x672160; AudioVM_LoadScriptFile @ 0x672d20; Sbf_OpenFile_Gamemus @ 0x4ed6c0].

### Render — materials/state — [render/render-material-re.md](render/render-material-re.md) (D-RMAT catalog; REN-2)

Minted at the REN-2 grill (2026-07-06). D-RMAT-1 (alpha-test compare shape —
the invert flag flips the COMPARE `a <= ref`, never the value; normal is
strict `a > ref`) and D-RMAT-3 (tag lookup is case-insensitive `stricmp`)
were discovered, witnessed, and FIXED in the same slice, with the T1
render-state golden re-dumped under citation.

Closed 2026-07-06 (REN-4): **D-RMAT-2** -> `FIXED` — the "soft edge" is the
`vsTracer` facing falloff `Diff = |dot(eye, normal)|^2` (not a displacement),
ported as `MATERIAL_DESCRIPTOR_VIEW_FADE`/`OSCAP_VIEW_FADE` `[orig: Tracer.fx
vsTracer]`; **D-RMAT-4** -> `FIXED` — the capability probe replicated over the
shipped localres text (unions over ALL techniques `[orig: @ 0x5ae690]`): 14/19
tags match the OED dump, 5 drift rows corrected on the renderer descriptor
table (FFP_GLASS, VS_SKBUMPDIFFT/PHONGT/DIFFT2, VS_SKGLASS), the 0x10000000
dialect resolved as the glow-copy capability (`MATERIAL_FLAG_GLOW`).

Closed 2026-07-06 (REN-5): **D-RMAT-5** -> `FIXED` — the composer emits the
witnessed FF model (`tex × min(hemi + dir·ndotl, 1) × 2`, SELFLUM ×
`ColorSrcGlobalGain`) on the witnessed uniform surface with engine-fed env
block values; the ×1.5/×1.6/spec-0.8 prototype constants deleted; T1
re-dumped (key set identical, 630 hashes re-hashed under citation), T2
swatch 116/120 cells moved with the 4 unlit VS_TRACER cells byte-identical
([render/render-lighting-re.md](render/render-lighting-re.md); the then-open
reflection/Phong rider D-RLIT-5 was closed on 2026-08-22 below).

Minted-and-closed 2026-07-06 (the model-parity slice, between REN-5 and
REN-6): **D-RMAT-7** -> `FIXED` — the retail color pipeline witnessed
**gamma-space end to end** (no `D3DSAMP_SRGBTEXTURE` at any device
sampler-state site, no `D3DRS_SRGBWRITEENABLE` at any render-state site, no
sRGB `.fx` pass states, identity display ramp at default gamma 1.0
`[orig: GLib_SetGammaRamp @ 0x677be0; default @ 0x84f354]`); the reimpl was
decoding textures sRGB→linear and re-encoding at the blit around the
witnessed math. Fixed across the composer + the full shader set: raw
sampling + gamma-space math. The original per-shader inverse output was
superseded by the 2026-08-22 hard cutover: shaders now write raw gamma-domain
scene values and `FrameFxCompositorEffect` performs one terminal display
decode after all 3D/particle blending. The swatch-probe **calibrate mode**
proves byte identity 256/256 plus exact source-over and additive results; T1
re-dumped (key set identical, 630 hashes), T2 swatch 120/120 cells moved
(the expected global response change), composite IDENTICAL, world set
re-captured. **D-RMAT-9** -> `FIXED` — the object composer's fog was an
invented linear ramp + `smoothstep`; now the witnessed device fog table
(`[orig: @ 0x58a950 → @ 0x677960]`, one text with the terrain/water
shaders). Full witness: [render/render-material-re.md](render/render-material-re.md)
§Color pipeline.

Minted-and-closed 2026-07-07 (REN-7, the T3 "W_RCK1_O watch item"):
**D-RMAT-10** -> `FIXED` — the `_MT` secondary (detail) stage ran HALF the
witnessed combine (composer `×1`, no alpha touch) vs the witnessed stage 1
`TSSColor(1, Modulate2x, Texture, Current)` + `TSSAlpha(1, Modulate,
Texture, Current)` — resolved MT surfaces (RckS05's `W_Rck1_o`, gray avg
93/255) modulated ×0.365 where retail runs ×0.73 (MT objects too dark in
detail regions). Fixed: the composer emits the witnessed ×2 + alpha
modulate; the reimpl masks `OSCAP_DETAIL` off the key when the secondary
fails to resolve (retail's NULL-texture stage drop, exactly; the white ×1
fallback deleted). UV evidence: the .3di v8 vertex carries TWO authored UV
sets — `v_uv2` was always right. T1: exactly the 224 detail-keyed hashes
moved, 0 classification rows. Full witness:
[render/render-material-re.md](render/render-material-re.md) §Divergence
catalog.

Minted-and-closed 2026-08-17 (the PR-503 adversarial T3 arm comparison):
**D-RMAT-11** -> `FIXED` — controlled flipbooks no longer assume every CTRL's
adjacent state is zero. The retail image statically seeds `TEX_TEAM` and
`TEX_CAMO1/2/3` (ordinals 92–95) to state one, selecting signed
`value % frame_count` `[orig: apply_shader_parameters @ 0x58DC36..0x58DC42]`.
The port now takes that branch only for those four discrete selectors while
preserving generic signed 16.16 animation. Literal tests pin RevX02
`IndoArms.3di`'s two-frame camo selector and `APLFP1.3DI`'s three-frame
Jflag1/Jflag2/Jflag3 team material, including x86 IDIV's negative remainder.

No D-RMAT work remains in the open-work table.

Closed 2026-08-22: **D-RMAT-6** -> `FIXED` — all 44 retail effect
sources, 59 technique declarations, and 138 pass declarations are now in an
exact decoded inventory. The selected shader-usage-level-2 path maps NORMAL
to 24 audited runtime techniques, CLIP to the reflection plane/fallback
matrix, PROJSHAD to all twelve slot captures, MATCHTERRAIN to the skinned
stance/page fold, and GLOW to LUM-copy plus Glass's rotated specular in the
typed focused Q3 draw list; all live classes have D3D12 raster probes.
DEPTHMASK's two source declarations
are correctly unhosted because the sole producer
`LightPool_SpawnSpotProjectorEffect @ 0x5a9fd0` is caller-less. The native
Q3/FrameFX backend closed D-RORD-5 in the same cutover.

De-tabled 2026-08-06 (the closed-row compaction — the table above holds
OPEN work only; full detail in the named record + git history):

Closed 2026-08-22: **D-RMAT-8** -> `FIXED` — all retail 3D/particle passes blend gamma-domain scene values, followed by one terminal display decode. The live D3D12 calibration pins 256/256 transfer bytes and exact SRCALPHA/INVSRCALPHA plus ONE/ONE results (`[orig: decode_blend_mode_to_d3d_states @ 0x680f00]`; full detail: render-material-re.md).

### Render — draw order — [render/render-order-re.md](render/render-order-re.md) (D-RORD catalog; REN-3)

Minted at the REN-3 engine-research session (2026-07-06). D-RORD-1 (the
transparent ordering ladder — sky → far-water-side alpha → water →
camera-side alpha → overlays, from the witnessed frame bracket
`[orig: Terrain_RenderSceneWithReflection @ 0x5c93a0]`) was ported and
FIXED in the same slice: the ladder lives in `engine/runtime/renderer/render_order.{h,cpp}`
and is applied as the generalized Godot priority ladder (celestial, water,
object-model rungs), with the sort-key/pass-class semantics T1-pinned.

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-RORD-7 | Particle pass A runs at PRE_TRANSPARENT (before every transparent, water included) and pass B at POST_TRANSPARENT; retail draws far-side (below-water) object ALPHA strips before pass A, here they draw after it — a submerged transparent strip overlapping a far-side particle composites strip-over-particle instead of particle-over-strip. The emitter-scope water predicate, subset reversal, recursive packet order, and the mirror's consecutive pair are ported (render-order-re.md) | A | OPEN (bounded; reopened 2026-08-23 — the 2026-08-22 auxiliary far-alpha view closed it at the price of a second full-resolution scene render every frame and was withdrawn) | reopen only with a scene that shows the strip/particle overlap |

De-tabled 2026-08-06 (the closed-row compaction — the table above holds
OPEN work only; full detail in the named record + git history):

Reopened 2026-08-23: **D-RORD-7** — the 2026-08-22 closure's same-world far-alpha SubViewport rendered the entire opaque scene a second time per frame and replaced the beauty color at PRE_TRANSPARENT (additive object variants drew twice through it); withdrawn for the PRE/POST particle split alone, tabled above with the narrowed residual. `framefx_test.gd` keeps the water-attenuation differential.

Closed: **D-RORD-2** -> `PERMANENT` — Opaque state-sort (per-frame CPU quicksort by alpha-test bit → 256-unit depth slabs → effect index → fine depth `[orig: RenderBatch_QuickSort @ 0x5d8b40]`) not reproduced (register below; full detail: render-order-re.md + git history).
Closed: **D-RORD-4** -> `RESOLVED` — FP render pass ported 2026-07-09: `PlayerViewmodelRig` composites the viewmodel through a dedicated shared-world SubViewport (full detail: render-order-re.md + git history) Witness: [orig: @ 0x4ded60: near swap @0x4dee29/restore @0x4df0aa, fov @0x4dee71 -> h->v @0x58d900, depth remap @ 0x58a7b0; parser key 'renderfov' @0x54482a].
Closed: **D-RORD-6** -> `PERMANENT` — The two original sort-key quirks (opaque key bits 15+ = residual stack garbage; transparent key lags one strip within a render object) not reproduced (register below; full detail: render-order-re.md + git history).
Closed 2026-08-12: **D-RORD-8** -> `FIXED` — The frame pipeline now places the current-tick local view before terrain and foliage; terrain samples the live viewport camera and foliage consumes the same render transform, eliminating the hard-cut one-frame lag (full detail: render-order-re.md).
Closed 2026-08-22: **D-RORD-5** -> `FIXED` — `FrameFx` owns the exact capture, 256² weighted blur, 45-degree half-strength SRCALPHA/ONE composite, and terminal ordering; water contributes its NV bright pass, not its full color. On 2026-08-29 the source became a typed `Q3FrameCompiler` draw list in a compositor-owned full-resolution target attached to resolved beauty depth; the auxiliary camera/view and proxy/fallback paths were deleted (full detail: render-order-re.md).
Closed 2026-08-29: **D-RORD-10** -> `FIXED` — typed object, static-instance, skinned-pose, water, celestial, and sun producers feed the focused RenderingDevice target against resolved beauty depth, through a retained per-source geometry cache (packed once, device buffer per entry, no per-frame server readback; the water strip publishes its CPU arrays, a static RLOD switch or carve invalidates only its instance rows). No second shared-world submission or depth-occluder rerasterization remains.
Closed 2026-08-29: **D-RORD-11** -> `FIXED` (MATCHING: the divergence claim was refuted, no drawn behaviour changed) — retail does NOT cross-fade or dual-submit across an RLOD threshold: `Model_SelectRlodLevel @ 0x5c3b20` returns one level in EAX, its overlap fraction goes only to `0x29ACD9C` whose sole reader is the callerless stub `@ 0x5c38c0`, and the `0x10000000` pair at `Terrain_RenderSectorEntitiesBySide @ 0x5c7ffc/@ 0x5c8020` is the head+body player avatar (each part walks its own table with one projected radius). The hard switch is the port; the dead `blend_fraction` output was deleted and the attachment rule (the parent's level clamped to the overlay's own count, `BoneCallback_org0_World @ 0x4e39c4..0x4e3e51`) ported as `renderer::attachment_lod_index` (full detail: render-order-re.md).
Closed 2026-08-22: **D-RORD-3** -> `FIXED` — every retained rigid alpha strip owns its priority material and is classified from its live transformed authored center whenever its model transform or the water plane changes (2026-08-23: change-driven — the same result as retail's per-frame recompute, without keeping every alpha-strip model awake); bone-path alpha follows retail's entity-side submit selector, and the far/camera-side ladder reverses from the adjusted render eye when underwater (full detail: render-order-re.md; GUT `object_model_runtime_gate_test` / `render_shader_cache_handoff_test`).

### Render — lighting — [render/render-lighting-re.md](render/render-lighting-re.md) (D-RLIT catalog; REN-5)

Minted at the REN-5 session (2026-07-06), which also closed env #17 (the
modulator chain went live) and D-RMAT-5 (the composed FF lighting model) in
the same slice, converted the last `UNAUDITED` render system, and answered
the former D-RORD-5 specular-cube question (the static sun-glint cube). The chain is
ported engine-lib-first (`engine/runtime/renderer/light_runtime.{h,cpp}`,
`env::ModulatorChain` in `engine/formats/env`)
and T1-pinned (`renderer_state_vectors` section 5).

Minted-and-closed 2026-07-06 (the model-parity slice): **D-RLIT-7** ->
`FIXED` — the placer's static MultiMesh batches froze the env lighting
harvested at load (no live owner for the template-harvested materials;
even the pre-first-iris-tick modulator was baked in), while retail relights
every entity from the current lighting block each frame
`[orig: setup_entity_lighting_and_shader_constants @ 0x5d98a0]`. The placer
first registered every harvested batch material and re-stamped it from the
live env per frame (generation-gated; single-sourced stamping shared with
ObjectModel). The same slice re-derived the no-environment fallback defaults to
the retail noon register (full_00.env tod 1200 bytes — composer +
object_model + shader-global + terrain-include defaults, one cited
register). Mechanism re-ported 2026-08-26 (the World-tick perf pass): the
per-material restamp was the reimpl's own; retail keeps one lighting block
per scene pass plus two per-entry factors `[orig:
CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090 ->
RenderBatchCtx_StoreLightingConstants @ 0x5d89e0]`, so the block is now the
`opennova_light_block_*` global shader parameters `MissionEnvironment`
writes once per env change and the factors one `u_entity_light` instance
uniform; no material carries lighting state (render-lighting-re.md D-RLIT-7).

Minted-and-closed 2026-07-06 (the REN-6 session): **D-RLIT-8** -> `FIXED` —
the object per-material `hemi_sky` served the RAW TOD keyframe while
`dir_color`/`hemi_ground` served the smoothed+modulated writeback (mixed
color spaces; the sky-facing hemisphere half too dark off-noon), where
retail feeds all entity lighting from the post-modulator block colors
`[orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090 fills
[8..10] ← Env_SkyBlock[0]]`. The sky block now rides the per-tick
writeback seam (`set_sky_ambient_rt`, mirroring fill/sun/fog); details in
the catalog below.

Minted-and-closed 2026-08-20 (the 03tr-sun-sky fixture slice; id minted
2026-08-21 by the post-merge tidy, which found the closure recorded without
one): **D-RLIT-9** -> `FIXED` — several Godot device seams consumed the
render-float (D3D-world) celestial/light tuple as if it were Godot world;
the two bases differ by the x/z swap `godot = (z, y, x)_render`
`[orig: Math_FixedPointToFloat3_YNegated @ 0x611210; the glint submit matrix
update_sun_glare @ 0x5ad1ba..0x5ad213]`, so every low-sun frame front-lit
where retail backlights. Fixed via one mapping seam
(`godot/src/env/env_axes.h`) at every consumer — the environment direction
getters, the object directional term, sun/moon/glare/glint placement, the
glare jitter plane, the sun-veil dot, the dome uniforms and the star
placement — with the raw tuple deliberately left on the terrain/foliage
globals, which re-swizzle it themselves (byte-parity-verified in that basis).
Measured by the registered `03tr-sun-sky` fixture; details in the record.

Updated 2026-08-19 (the viewmodel-parity slice): D-RLIT-3 closed — the
outdoor per-entity 3-radius sun-visibility feed is live beside the 2026-07-29
interior transfer. The earlier model-light correction stands: retail's
mission-start `Entity_SpawnGlowEffects` consumes model `LGHT` records into
the EffectWorld pool; OpenNova routes those records and the transient
families through `LightScene`. Per-draw live/static object delivery, lifecycle,
terrain/corona legs, and the refuted max-quality foliage premise close
D-RLIT-4 as of 2026-08-23.

Minted 2026-08-30 (the PR #595 slot RenderingDevice pass review):
**D-RLIT-10** -> `PERMANENT` (class C, register below) — the slot silhouette
capture's eye and depth band. Retail renders the entity at the origin of
its rotation-only look-at view under an ortho band 0.2..5000.2
`[orig: setup_shadow_cascade_matrices @ 0x58d300; Entity_RenderWithLODCallback
@ 0x5d6ef0]`, a band that as read starts in front of the entity's own
origin; the shell's RD pass backs its eye off along −forward so the whole
model sphere lies inside its clip band. The orthographic silhouette is the
same image either way (render-lighting-re.md D-RLIT-10). The capture view
basis itself is witnessed and shared: `renderer::direction_look_at`
(`build_direction_look_at_matrix @ 0x612c90`) serves the slot view and the
addeweap attachment frame from one engine home.

Corrected 2026-08-22 by the exhaustive highest-quality technique audit: the
earlier D-RLIT-4 statement that all object point lighting is vertex-rate was
too broad. Fixed-function NORMAL families consume D3D vertex diffuse with its
range cutoff. Pixel-shader DOT3/Phong/mirror point passes instead calculate
mapped-normal N.L/N.H in the pixel shader. Ordinary bump/Phong effects
interpolate the retail vertex shader's attenuation × geometric self-shadow;
BDiffT2 and the fixed-function skinned DOT3 effects interpolate attenuation
only, and the latter also use geometric/Gouraud hemisphere with no
directional self-shadow. Their authored attenuation has no explicit D3D
range cutoff `[orig: _BaseInc.fx; _vsDiffT.fx; _vsDiffO.fx; _vsPhongT.fx;
_vsPhongO.fx; BDiffT2.fx; SkBDiffT2.fx; SkBDiffO2.fx; the corresponding
_vsSk* sources]`. Flag and Glass have no authored point pass. The 24-technique evidence ledger at
`godot/shaders/object/technique_validation.json` closes D-RLIT-4's former
"VS-technique D3D-light consumption question"; this correction supersedes
that stale wording in the historical row below.

Closed 2026-08-22: **D-RLIT-5** -> `FIXED` — the former
hemisphere-reflection and Phong approximations are gone. The selected path
hosts retail's 256² CubeEnvironment synchronously before the visible draw:
six mapped 90° faces at player+1/clamped terrain+10, the callback's sky plus
sun/moon only, exact 0x60 gamma-byte multiply, and the additive
CubeRotSpecular/Env_LightBlock lobe. Godot's X/Z basis swap, the cube
layer order, all six face orientations, same-frame publication, and
full/midtone bytes pass the checked-in Forward+ D3D12 probe (2026-08-30: the
faces are copied into the published RD cubemap on the RenderingDevice, no
CPU readback, the same bytes). The exact 256²
PhongMap and every source-specific pow-8/pow-16/pow-4/64 alpha-controlled
lobe are separately generated and raster-pinned `[orig:
init_render_textures @ 0x58f6e0; update_environment_cubemap @ 0x6106a0;
GTexture_RenderCubeMapFace @ 0x6864d0; callback @ 0x5c3700;
Render_CreateSystemTextures @ 0x58aca0; _psPhong.fx; _psPhong2.fx]`.
The 2026-08-22 `FrameFx` cutover also closes the former
D-RORD-5 Q3/FrameFX post-backend rider.

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
---

De-tabled 2026-08-06 (the closed-row compaction — the table above holds
OPEN work only; full detail in the named record + git history):

Closed 2026-08-23: **D-RLIT-6** -> `FIXED / REFUTED` — the alleged baked
mission-lightmap TGA was an IDB naming error, not an unhosted render input.
`Terrain_LoadTileSetAtlas @0x604a90` loads the configured tile-set strip,
derives 64-pixel cell counts/UV reciprocals, and publishes the view consumed by
`PolyTrn_RenderTile @0x60ddd4..0x60df1b` for ordered `.til` overlay quads.
Its only other view xref is an uncalled helper at `0x607b30`; there is no live
second foliage-lightmap consumer. Detail foliage receives the already-composed
tile RT through `Terrain_FindSectorPatchRT @0x6042a0`. OpenNova's shared
128-layer terrain/foliage cache loads the same `tilestrip`, composes ordered
overlay RGBA before DOT3 alpha, and publishes it to both shaders. Native
`terrain_tile_composer` real-asset oracles plus the shader provenance test pin
the path. Static silhouettes/final RT mechanics remain D-TERRAIN-7.

Closed 2026-08-23: **D-RLIT-4** -> `FIXED` — EffectWorld's portable pool,
model/powerup and transient spawn routes, spawn-fixed LGHT/shared muzzle lease,
fade lifecycle, owner/interior/per-ROBJ selection, live/static shader delivery,
coronas, and terrain projection are all hosted and tested. The final foliage
residual was refuted rather than implemented: retail calls
`Light_SelectAndEnableForDraw @0x60a5dc`, but the locked max-quality draw binds
`Foliage_WindSwayVS`, whose complete source declares no normal/light input and
writes `oD0=c6`; the blend PS consumes only cached-tile c0/c1 plus that c6.
Enabled D3D lights can affect only the failed-VS FVF fallback outside our
selected profile `[orig: Terrain_CreateFoliageVertexShaders @0x5ff630;
Foliage_SetupFarSlotDraw @0x60087a; Foliage_RenderFarPatches @0x609de0]`.
Spot/Target delivery is likewise caller-less dead code. The generation lease's
stale-handle rejection remains an intentional safety divergence (full detail:
render-lighting-re.md + foliage-re.md + git history).

Closed 2026-07-21: **D-RLIT-1** -> `FIXED` — Hosted weather ran only 4 color blocks through the modulator; retail modulates 16 in witnessed order (skyfog, cloud set, statics) `[orig: @ 0x57ef97..0x57f03c]` (full detail: render-lighting-re.md + git history).

Closed 2026-08-22: **D-RLIT-2** -> `FIXED` — The iris now marches all three samples from the nearest terrain/entity-clipped camera ray, reuses the local player's exact candidate slice for blink and sun tests, preserves the interior group set/retain/clear sequence, and tests eligible pool-1 plus pool-2 solids through the shared radiused walker `[orig: Environment_ApplyFogAndAmbient @0x57e440; compute_ambient_light_along_direction @0x5c7a00; terrain_sector_compute_lighting @0x5c7550]` (full detail: render-lighting-re.md + git history).

Reopened 2026-08-23: **D-RLIT-3** — The 2026-08-19 close covered the local/authority draw path, but a later role audit proved decoded joiner replicas still bypass the per-entity sun query and present at factor 1.0. D-COL-3's exact typed wire bound/scale/midpoint prerequisite is now live; the open table tracks the remaining wire candidate-slice and presentation-light feed rather than hiding it as a bounded residual.

Closed again 2026-08-23: **D-RLIT-3** -> `FIXED` — decoded pool-0 and eligible pool-1 draw sources now receive their own separately wire-keyed candidate slices on retail's 17-tick cadence and cast the same three rays from the exact scaled bbox midpoint. The identity-aware `get_draw_lighting_changes` cutover emits `[wire,bms,quality]`; `WirePresentPass` caches and applies the factor to both the body and any late-built held weapon. Explicit registry-twin identity is the only self-exclusion, so equal packed H/L values cannot alias `[orig: Entity_BuildProximityListsFromPools @0x4b8eb0; Entity_ComputeSunVisibility @0x5c6800; setup_terrain_effect_for_entity @0x5c74a0]` (full detail: render-lighting-re.md + git history).

### Render — occlusion — [render/render-occlusion-re.md](render/render-occlusion-re.md) (D-OCC catalog)

The blink-box/section-mask/portal engine landed 2026-07-17
(engine/runtime/world/occlusion.cpp). Its port pass minted D-OCC-9..15 as
record-only PORT DIVERGENCE rows; the 2026-08-30 post-merge tidy tabled and
registered them (standing rule 2 — the record-only carve-out below covers only
the D-OCC-1..8 witness details, which are open research notes, not port
divergences, and stay out of these tables deliberately).

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-OCC-9 | Forced-visible def bytes (`itemDef+2193/+2194`, the destruction bone-map bases) are wired through `OcclusionWorld::EntityDefBits` but default 0 — latent until the destruction system (D-COL-2); their load-time source is unwitnessed. Identical behavior for buildings without destruction bones | B | WITNESSED-READY-DEFERRED (latent — awaiting D-COL-2) + NEEDS-RE (the load-time source) | PAR-REN |
| D-OCC-12 | View culling stands in for `Viewport_TransformAndClipPoint @0x4115e0`: 5 frustum planes (near + 4 sides) + a Q22 forward-row depth cull vs the fog distance — same culling intent; the retail projector's screen-space epsilon behavior is not replicated | C | OPEN (bounded stand-in; PERMANENT candidate on ratification) | PAR-REN |
| D-OCC-13 | The window-glow facet: the slot glow value is computed and banked but no renderer consumes it — retail draws window glows (the renderer-replaced legs of this id are registered class C below) | A | OPEN (awaiting-consumer facet) | PAR-REN |
| D-OCC-14 | Entity-gate coverage: pooled opaque/alpha-tested MultiMesh rows have no per-instance retail section gate (terrain-aligned 512-unit bins give exact population AABBs; blended rows stay global to preserve ordering); wire avatars ride their own present path ungated; organics without collision instances use a position-centered 1 u bound-sphere stand-in for the graphic bounds | A | OPEN (bounded coverage) | PAR-REN |
| D-OCC-15 | The staggered-refresh facet: retail refreshes pool-2 static `blink_hits` in staggered batches; the port has no placement or staggered static refresh, so static objects inside rooms can remain unstamped (the re-arm PRNG stream facet is registered class C below) | A | OPEN (coverage facet) | PAR-REN |

Closed 2026-08-30 (the post-merge tidy round; ratification = this PR's merge,
each entry falls back to an OPEN row if declined): **D-OCC-10** -> `PERMANENT`
— `Terrain_SortSectorCacheByDistance @0x5c4410` orders retail draw calls/slot
iteration only; results are order-independent per candidate and Godot owns
draw order (register below; precedent D-RORD-2). **D-OCC-11** -> `PERMANENT`
(class D) — the port clamps/guards where retail's wedge builder and viewthru
bank write past their arrays into adjacent memory (register below).
**D-OCC-13 (renderer-replaced legs)** -> `PERMANENT` — two-pass open-building
draw order, per-light interior section scoping, the water-mirror clip matrix
leg, and the reflection-pass collector variant (register below; the
window-glow facet stays OPEN above). **D-OCC-15 (re-arm PRNG stream facet)**
-> `PERMANENT` — an owned re-arm stream with identical distribution (register
below; precedent D-NET-115; the staggered-refresh facet stays OPEN above).

## Count-to-zero scoreboard

Open counts by domain (the target is zero in every cell). The table below is
**generated from the per-domain tables** by `scripts/lint/ledger_check.py`
(`--write` regenerates; the CI lint step runs `--check`) — the scoreboard can
no longer drift from the rows the way hand arithmetic did twice in the
program's first two days. A domain burned to zero with no tabled rows left
drops off the scoreboard (first to do it: Item def, D-ITEMDEF-1, 2026-07-05).

<!-- scoreboard:generated:begin -->
<!-- Generated by scripts/lint/ledger_check.py --write. Do not hand-edit this block; the CI lint step checks it against the tables. -->

| Domain | OPEN | NEEDS-RE | WITNESSED-READY-DEFERRED | Domain open total | Closed rows still tabled |
|---|---|---|---|---|---|
| Net | 16 | 0 | 6 | 22 | 0 |
| World / AI + events | 49 | 4 | 3 | 56 | 0 |
| UI (menu/ctrl/sound/playerinfo/HUD) | 26 | 0 | 3 | 29 | 0 |
| Mission `.mis` | 0 | 1 | 1 | 2 | 0 |
| 3DI `.3di` (GP) | 0 | 1 | 0 | 1 | 0 |
| Credits (CBIN) | 0 | 1 | 0 | 1 | 0 |
| Terrain | 1 | 0 | 0 | 1 | 0 |
| Foliage | 3 | 0 | 0 | 3 | 0 |
| Render — draw order | 1 | 0 | 0 | 1 | 0 |
| Render — occlusion | 4 | 0 | 1 | 5 | 0 |
| **Total** | **100** | **7** | **14** | **121** | 0 |

Dual-flagged rows (also carry a NEEDS-RE facet): D-INF-20, D-NET-136, D-NET-169, D-NET-179, D-NET-97, D-OCC-9.

<!-- scoreboard:generated:end -->

The Boot-resources row is the R8 audit doing its job: an audit that converts
unknown unknowns into tracked rows RAISES the count before the burn-down
lowers it (as PAR-R1..R7 did for their six new domains).

Permanent register (below; counting rule: one row per register entry —
`D-SCR-1`/`D-SCR-2` share a row, `env #11` counts as one ID, and a facet entry
registers the facet while its parent id may stay tabled). `UNAUDITED` systems: **0** — the
runtime-render systems reopened the set on 2026-07-05 (the REN audit track
below, [ADR 0023](adr/0023-render-visual-parity.md)); REN-2 converted the
materials/state system, REN-3 the draw-order system, and REN-5 the lighting
system (all 2026-07-06) — **the audit track is burned back to zero**; every
system has an RE record (full or partial) or a tracked-by-composition audit.
(The former unnumbered BMS-second-chunk note is now D-EVT-5, minted and closed
in the World table above.)

---

## Permanent register (feeds [ADR 0022](adr/0022-divergence-burn-down.md))

Ratified deliberate divergences — each verified against its record. Two classes:
**platform/reimpl-structural** (the reimpl cannot or should not reproduce the original's
substrate) and **original-bug/garbage** (reproducing it would manufacture garbage against
[ADR 0003](adr/0003-no-raw-passthrough-create-from-scratch.md)). Each carries the
one-line rationale for why porting it would be *wrong*.

### Platform / reimpl-structural (class C)

| ID | Divergence | Why porting it would be wrong |
|---|---|---|
| D-NET-115 | The far-marker spawn scores and the 0x100 death roll draw from an owned MSVC-CRT recurrence seeded from the session seed (`World::crt_rand`), while the render/effects consumers (scorch rolls, material/PANM noise, the glass reseed) draw from a separate thread-local stream (`engine/base/crt`); retail's ONE process `rand()` stream is seeded from the wall clock once at host start and interleaves both families | The recurrence, the per-row draw order and the slot-table first draw are retail's; the seed and the sim/effects stream split are the two halves that differ (net-re §5.42): a clock seed is irreproducible by construction and the interleave is frame-rate dependent, so a session seed keeps a host's draw sequence testable and no cross-family draw order is observable on the wire — every coupled value is authority-drawn-and-shipped |
| D-3DI-1 | MTRX byte-exact output needs OED's x87 `_PC_24` precision; a 64-bit SSE2 build diverges in low FP bits | Byte-exactness is a property of the original's 24-bit x87 mantissa; a modern SSE2 build cannot match the low bits without the documented `_controlfp(_PC_24)` parity sub-build. |
| D-MNU-4 | The original truncates each scaled quad rect to int per element; the reimpl applies one float `CanvasItem` scale | A sub-pixel cosmetic difference; reproducing per-element int truncation would fight Godot's scene-graph scale model for no visible gain. |
| D-PTL-9 | Portable LCG and reimpl-basis yaw/pitch construction vs retail `rand()`/DirectX/FPU direction helper | The authored component bounds, `spread_skip`, and two-draw cadence match; byte-identical platform RNG and FPU basis construction would make deterministic simulation platform-dependent. |
| D-PTL-10 | GFXFLIPRAND derives its start frame from a stable particle serial instead of the retail slot pointer | The original value depends on process address layout; a serial preserves the distribution intent without making playback allocator-dependent. |
| D-PTL-12 | Per-emitter capacity is capped at 4096 (default 256; shipped maximum override 400) while the exact retail manager-wide ceiling is unwitnessed | The bounded superset preserves authored headroom and prevents hostile mods from causing unbounded allocation or burst work. |
| D-PTL-19 | Flipbook frame counts are normalized to 1..256 while retail has no witnessed equivalent bound | A finite shared cap prevents malformed/mod-authored counts from causing unbounded frame-name and atlas work; shipped content is unaffected. |
| D-PTL-20 | The parser accepts and composes both curve modifiers while retail consumes one trailing modifier | A deterministic syntax superset improves mod tolerance; no shipped file combines modifiers, so strict emulation would only reject an otherwise well-defined extension. |
| D-VFS-4 | Raw runtime reads now probe live like retail, while indexed listings and decoded caches remain epoch snapshots | The gameplay byte-read seam no longer carries this divergence. Rebuilding indexes and decoded Godot resources on every open would fight the Godot cache model; explicit remount/epoch invalidation exposes source changes without stale raw runtime reads. |
| D-VFS-8 | Retail's 16-search-path x 16-byte / 16-slot / 6-name caps (incl. the >5-char expansion-name strcpy overflow) | Capacity supersets; reproducing the caps (and the overflow) would manufacture the original's buffer bugs. |
| D-VFS-9 | `<exp>L.pff` mounted as our persistent primary vs retail's secondary slot 0 | Effective lookup precedence is identical; the slot bookkeeping is reimpl-internal. |
| D-VFS-10 | Mounted loose lookups reject rooted/drive-qualified/ADS/`..` queries and symlink escapes, unlike retail's unchecked path construction [orig: FileSystem_OpenFile @ 0x75b1c0 / FileSystem_FileExists @ 0x75aa50] | A resource name must stay inside the explicitly mounted root. Preserving legitimate relative, case-insensitive lookup while refusing arbitrary local-file access is a reimpl safety boundary, not a gameplay fidelity loss. |
| D-VFS-11 | Under `--loose-root` (ONED's Run OpenNova loose action) the game shell falls back to the selected loose directory when the fixed boot table opens zero archives, where retail aborts subsystem initialization [orig: PFF_OpenAllArchives @ 0x4a4310; fatal check @ 0x4a6f44] | Running the exact selected loose file set is the managed launch's purpose ([ADR 0037](adr/0037-oned-runs-game-data.md)); every unflagged standalone run keeps the retail fatal, so shipped-game behavior is unchanged. |
| D-RLIT-10 | The render-slot silhouette capture's RenderingDevice eye backs off the caster center along −forward (two model-sphere diameters plus 2 u) with a 0.05..(2·eye + r) ortho depth band, where retail renders the entity at the origin of its rotation-only look-at view under the 0.2..5000.2 band `[orig: setup_shadow_cascade_matrices @ 0x58d300; Entity_RenderWithLODCallback @ 0x5d6ef0]` | An orthographic silhouette is invariant under a translation along the view axis, so the shell's eye reproduces the witnessed image exactly; the literal band's near plane sits 0.2 u in front of the entity's origin and the retail render state that admits the entity's near half is unwitnessed, so porting it as read would clip half of every caster ([render/render-lighting-re.md](render/render-lighting-re.md) D-RLIT-10). |
| D-RORD-2 | Retail's per-frame CPU quicksort of opaque batch entries (alpha-test bit → 256-unit depth slabs → effect index → fine depth) vs the reimpl renderer's internal opaque ordering | The sort is a device-era draw-call-batching strategy, not observable behavior for z-buffered opaques; reproducing it would fight the Godot pipeline for zero visual difference. The key semantics survive as T1-pinned functions (`renderer::opaque_sort_key`) so any future implementation that CAN consume them has the witnessed spec ([render/render-order-re.md](render/render-order-re.md)). |
| D-CTRL-4 | The `hudcolor` action (catalog row 76, dispatch code 10) stays reachable by rebinding, while on the stock keymap the default F6 cycles `huddetail` exactly as retail's first-match shadowing does (`Input_HandleActionBinding_0 @0x4e0420`) | Retail leaves `hudcolor` dormant only as a keymap accident of two rows sharing F6; both rows are live dispatcher arms, so hiding the row would reproduce the accident rather than the mechanism, and the default-key behavior already matches |
| D-MNU-18 | Menu wheel scrolling — a reimpl addition; retail menus never wheel-scroll (the witnessed pipeline dead-ends: `Menu_ShellMouseCallback @ 0x54b8c6` collapses both tick masks into event 0x100000B that no handler consumes, `Menu_InGameMouseCallback @ 0x568760` drops ticks) | Parity here means discarding wheel input a modern player expects at a witnessed dead end. The addition stays inside the witnessed dispatch shape (one notch = one CScrollWnd arrow step; open popup exclusive, else the front-most row owner under the point) and the CONTROLS remap capture keeps first claim, so the wheel stays bindable. Ratified 2026-08-12 (maintainer). |
| D-THROW-2 | Bounce kicks and claymore-fan angles draw from world-local streams of retail's generator shape; retail draws them from the shared process globals at `0x31BFBB0/B8` | The distribution is retail's and every coupled value is authority-drawn-and-shipped, so no cross-system draw order is observable; a shared clock-seeded stream is irreproducible by construction (the D-NET-115 ratification, applied to the throwable family; world-wac-ai-re §27.8) |
| D-COL-7 | The vertical ground probe reads the bilinear column height; retail marches + bisects (`Terrain_RaycastHeightmapHiRes_0 @ 0x60e710`) | Equal for vertical rays on a heightfield, and oblique rays already take `terrain_raycast_refined` — the march would add cost for the identical answer (world-wac-ai-re §15.5) |
| D-MNU-1 | `%VAR%` expansion runs per field at build time; retail runs one whole-buffer pre-parse pass (`NapiXML_ExpandVariablesInText @ 0x63a000`) | The rendered output is identical for every shipped stylesheet variable; the whole-buffer pass is a parser mechanism, not a behavior (ADR 0005; mnu/menu-re.md) |
| D-MNU-2 | Menu sound member selection is reproduced (`SoundSelector`); the interleaved per-play volume/pitch jitter draws are not modelled | The jitter is a per-play random draw with no authored intent to reproduce; modelling it would fake a stream nobody can observe (mnu/menu-re.md) |
| D-MNU-3 | The menu format layer preserves attributes the runtime ignores and services file-less `<SOUND>` nodes retail would fail on | An authoring superset over retail's tolerance (ADR 0002): every retail menu parses identically, and reproducing the failure modes would only reject files (mnu/menu-re.md) |
| D-MNU-10 | The WEAPON screen's PLAYER_CLASS spin is enabled offline; retail enables it only in a network session (`UI_InitTeamClassSelection @ 0x567370`) | Deliberate maintainer decision 2026-07-11: the runtime hosts a listen session even for SP (ADR 0009) and the offline loadout flow wants the choice; retail's SP pin is the absence of a session, not a rule (mnu/menu-re.md) |
| D-SND-1 | The co-named sound bank rides the single search chain; retail scopes it to dialog playback | A superset — dialog set names never collide with ambient sets in JO assets, so every lookup resolves to the same member (audio/lwf-dbf-sound-re.md) |
| D-SND-3 | The sound parser rejects out-of-range / oversize rows retail silently tolerates | Deliberate authoring-side strictness: every shipped bank parses, and tolerating garbage rows would only hide authoring errors (audio/lwf-dbf-sound-re.md) |
| D-SND-4 | Dialog playback is serialized by a FIFO advanced on the voice's `finished`; retail gates one channel on `dword_A895FC` with a per-line countdown | No dialog overlap either way — the observable rule (one line at a time, in order) is identical; the countdown is the original's channel bookkeeping (audio/lwf-dbf-sound-re.md) |
| D-SND-10 | ChuteFlap/FreeFall per-tick refires coalesce into one exclusive voice; retail's refires pile into a finite channel pool and steal their own channels | Audibly one continuous rush either way; reproducing the 62-voice pileup would spend voices to sound the same (audio/lwf-dbf-sound-re.md) |
| D-SND-13 | SndProf.def parses per mission load; retail parses once at boot plus expansion reloads | Same file, same table, no observable difference; the load cadence is the original's memory model (audio/lwf-dbf-sound-re.md) |
| D-LOADSCR-1 | Load progress pumps 8 stage-boundary values + per-model pulses; retail has ~30 call sites with per-subsystem slot++ ticks | The value set and the constant-plus-creep pump mechanism match; the granularity follows how each load pipeline decomposes and is cosmetic-only (interface/loading-screen-re.md) |
| D-LOADSCR-6 | The loading background draws unmodulated; retail draws it through the MODULATE2X-neutral `0xFF7F7F7F` effect modulate | Net-identical color — recorded so nobody "fixes" a half-bright that is not there (interface/loading-screen-re.md) |
| D-LOADSCR-7 | ESC/disconnect cannot abort the synchronous SP/host map load; retail polls `Client_CheckDisconnectOrEscDuringLoad @ 0x520270` at four asset points | The load has no reachable interruption window (one synchronous call the SceneTree cannot pre-empt) while both joiner waits are coroutines and honour ESC; scope-corrected 2026-07-25 (interface/loading-screen-re.md) |
| env #8 | envscale applies at interpolation/byte quantization to the engine view's targets; retail bakes it at parse into every `*_rgb` positionally, leaking a stale scale across files in one load | Equivalence holds whenever envscale precedes the colors (corpus-validated); reproducing the positional parse-time bake would re-manufacture the cross-file stale-scale bleed and break the round-trip-preserving raw getters (env/env-tod-re.md #8) |
| env #9 | The fog→skyfog mirror rides a per-keyframe flag; retail overwrites leftover keyframe slots with the `0xC0C0FF` sentinel value | Equivalent for well-formed files; the sentinel's only extra behavior is accidental leftover-slot bleed — stale-state bytes the flag model cannot manufacture (env/env-tod-re.md #9) |
| D-OCC-10 | `Terrain_SortSectorCacheByDistance @0x5c4410` is not ported | It orders retail draw calls/slot iteration only; every mask/TOC result is order-independent per candidate and Godot owns draw order (precedent D-RORD-2; render/render-occlusion-re.md D-OCC-10) |
| D-OCC-13 (renderer-replaced legs) | The two-pass open-building draw order, per-light interior section scoping (`Lighting_SetInteriorLightGroup @0x5a90e0`), the water-mirror clip matrix leg (`@0x5c5e75`), and the reflection-pass collector variant (def-flag 0x2000000) are not ported | Each replaces a D3D-pipeline mechanism Godot's depth buffer, light model, and water reflections already provide; the visibility UNION and admission semantics are ported (render/render-occlusion-re.md D-OCC-13) |
| D-OCC-15 (re-arm PRNG stream facet) | The three-ray latch re-arm jitter draws from an owned `PRNG_Next16_C`-form stream seeded from the BSS-zero boot state; retail shares one process stream with unrelated consumers | Per-frame re-arm values are irreproducible against any given retail run by construction; the distribution is identical and no coupled value is observable (precedent D-NET-115; render/render-occlusion-re.md D-OCC-15) |

### Modern presentation (ADR 0043)

Deliberate technique replacements under [ADR 0043](adr/0043-modern-presentation-witnessed-intent.md)'s
intent contract: the engine keeps the witnessed decode of the authored data; the
listed rendering technique is replaced by canonical Godot machinery, accepting the
visual change as a modern improvement. One row per replaced subsystem — historical
defects of a deleted system close as moot with it. Rows land with the commit that
deletes their system, never ahead of it.

| ID | Replaced technique | Modern equivalent / intent preserved |
|---|---|---|
| MP-2 | The hand-written fixed-function object surface lighting: the per-vertex `clamp01(ambient + dir·N.L + hemisphere deltas)` MODULATE2X fold (`Lighting_SetHemisphereD3DLights @ 0x5d8cb0`), the DOT3/Phong pixel families with the `gsys_phong` lookup and `SelfShadowMult=10` horizon term, the environment-cube approximation, the per-shader device fog, and the vertex-diffuse coverage source | The 25 technique families are LIT materials: the authored facts (textures, normal maps, Phong lobes, ReflectColor, SELFLUM gain, alpha tests, UV transforms) fill an `ObjSurface` (albedo/normal/roughness/specular/emission) and Godot's pipeline lights it — sun + hemisphere-sky ambient + clustered omnis + Environment fog; self-lit is EMISSION, glass/mirrors are metals on the scene reflection source; additive wrappers and postmultiply stay unshaded emissive islands with the retail fade-to-black fog; the interior daylight lerp survives as an ambient factor |
| MP-1 | The EffectWorld per-draw light delivery: the 64-cap collect, the owner/interior group gate, the 3-per-strip select (`collect_render_objects_for_batch @ 0x5d9226`), instance-uniform + static RGBAF-atlas shader delivery, and the terrain projected-texture pass (`Light_SetupTerrainProjectedPass @ 0x5AA830`) | Real `OmniLight3D` nodes under Forward+ clustering, synced per frame from the witnessed pool (spawn lifecycle, LGHT owner policy, flicker ring, color/blend/gain folds all intact in `renderer::LightScene`); light range bounds the owned/interior bleed the group gate used to scope |

### Original-bug / garbage class (class D; basis: [ADR 0003](adr/0003-no-raw-passthrough-create-from-scratch.md))

| ID | Divergence | Why porting it would be wrong |
|---|---|---|
| env #11 | Original packs negative color components as garbage (no lower clamp); the reimpl clamps to 0 | Reproducing unclamped negative-color UB would carry garbage bytes through the parser for no defined behavior. |
| D-NET-133 (empty-slot facet) | An in-capacity EMPTY 0x18 slot replies a zeroed type-0 record; retail serializes the slot's raw (possibly stale) memory | The observable effect is identical (the client stops at the type gate either way); reproducing retail's stale-memory bytes would be manufacturing garbage. |
| D-MUS-7 | `op_callvl` (`0x0A` call form) resolves against an uninitialised-BSS name table in Jointops, so the opcode is dead; the reimpl mirrors the dead stub (push 0) | The original behavior *is* "do nothing" (the table is never populated); porting a "working" call would invent behavior the engine never had. |
| D-MUS-5 | `inc_g`/`dec_g` (`0x11`/`0x12`) operate on 1 byte and raise no globals-dirty notify | An intentional mirror of the original's silence; adding the notify would diverge from the witnessed behavior. |
| D-MUS-12 | The MUS VM's soft tick budget lets a never-draining malformed script wedge the frame forever (the original hangs); the reimpl caps the per-tick drain extension and returns — bounded work per tick, the embedder never hangs [orig: `AudioVM_DispatchLoop @ 0x672720`] | Reproducing an unbounded in-frame hang on malformed data would manufacture the original's bug against ADR 0003; well-formed scripts never reach the ceiling (mus-sbf-re.md D-MUS-12). |
| D-PTL-1 | The engine's outer dispatcher remaps `g2_color1`/`g3_color1`/… into higher color slots (a parse bug); the reimpl maps `g{N}_color{M}` correctly | A recorded intentional divergence: the correct mapping is what an author means; reproducing the dispatch remap would carry the engine's parse bug forward. |
| D-PTL-11 | Retail reads `scale_lut[i+1]` one byte past the 256-byte table at the final sample; the reimpl clamps to byte 255 | The overread is adjacent heap memory and therefore allocator-dependent garbage; clamping the last 1/256th avoids manufacturing undefined data. |
| D-VFS-6 | Retail's PFF open trusts the header blindly (no magic/entry_size/count checks; entry_size>36 overflows; two write-after-free bugs @ 0x768348/0x7685ba) — ours validates and is UAF-free | Reproducing unvalidated reads and UAFs would manufacture garbage against ADR 0003. |
| D-SCR-1 / D-SCR-2 | The SCR container codec accepts version bytes 0–2 and selects the key from the version byte + policy, where each original call site fixes the key | A deliberate multi-title superset so one codec serves JO-demo-era and shader containers; load-bearing equivalence holds for everything retail JO ships. |
| D-RORD-6 | The original's two sort-key defects: opaque key bits 15+ OR in an uninitialized stack slot (`@ 0x5d92b9`), and a transparent strip's key reads the depth slot BEFORE its own store, lagging one strip within a render object (`@ 0x5d9326`) | Both are stale/uninitialized-memory reads whose effect is accidental (constant-per-call garbage; a one-strip-stale depth); reproducing them would manufacture the bugs rather than the intent (back-to-front by depth), against ADR 0003. |
| D-INF-15 | FP bone builder: model rows past the anim's bone count sum an uninitialized `bone_translations` stack slot into their world position on flag-2 (translated) clips (`BoneAnim_BuildWorldMatrices @0x40c6e9..0x40c71d`; the buffer is only written for anim rows `@0x40c4bc..0x40c57c`); the reimpl adds zero | An uninitialized-stack read whose value is accidental per call; reproducing it would manufacture garbage against ADR 0003 — the witnessed intent (rows ride bone 0's matrix + their model pivot) is what the reimpl ports. |
| D-OCC-11 | The port clamps the wedge builder at 64 planes and guards the viewthru bank like the window bank; retail writes past both (printing "too many planes" after already corrupting adjacent stack, and erroring on the viewthru bank only after 512/2048 unguarded writes) | Divergence exists only in the overflow regime where retail corrupts its own memory — reproducing it would manufacture memory corruption against ADR 0003; scratch caps are sized above any witnessed record (render/render-occlusion-re.md D-OCC-11). |

`PERMANENT` is not a resting place for hard work: each entry above is a decision that the
*faithful* behavior is to diverge. If a future need arises (e.g. exact reimpl-internal-state
match for a reimpl-internal subsystem, or a byte-exact MTRX parity sub-build for D-3DI-1), the record names
the follow-up path.

---

## Audit track

### The 2026-07-05 sweep — COMPLETE

All seven systems that started with no RE record now have one (full or partial) or
a tracked-by-composition audit; their divergences are tracked rows, not unknown
unknowns.

| System | Slice | Result |
|---|---|---|
| VFS / PFF | PAR-R7 | full record — [vfs/vfs-pff-mount-re.md](vfs/vfs-pff-mount-re.md) (D-VFS-1..11) |
| Fonts | PAR-R4 | full record — [fonts/fnt-re.md](fonts/fnt-re.md) (D-FNT-1..4) |
| Foliage | PAR-R2 | fresh full record 2026-07-13 plus MODEL and near-secondary LOW corrections 2026-07-14 — [foliage/foliage-re.md](foliage/foliage-re.md); both tier cores MATCHING, D-FOLIAGE-7/-9/-10 bounded reimpl gaps |
| Tiles | PAR-R3 | full record — [tiles/til-re.md](tiles/til-re.md), overlay/atlas/flip-rotate MATCHING vs retail `@0x60df0d`/`@0x604700` |
| Credits (CBIN) | PAR-R5 | partial — [credits/cbin-re.md](credits/cbin-re.md); codec (magic + header + ROL32/XOR cipher `@0x75e348`) MATCHING vs `engine/formats/cbin`, witnessed read-only via raw disasm; markup + read-path NEEDS-RE |
| Terrain | PAR-R1 | re-grilled partial through 2026-08-17 — [terrain/terrain-re.md](terrain/terrain-re.md); preprocessing/top shader/LOD/detail coordinates matching, D-TERRAIN-7 runtime gap bounded; D-TERRAIN-1/-9 retired with the ONED preview; D-TERRAIN-8/-11 fixed |
| Importer | PAR-R6 | track retired with ADR 0038 — it was tracked-by-composition (composed RE'd libs, no independent parity surface) and its `docs/importer/importer-audit.md` record went with the asset pipeline, so nothing outlives the composition |

**Notes from the sweep (2026-07-05):** two "which binary" assumptions were
corrected by testing them — **the foliage generator cores and tile overlay
core audit cleanly against retail**, with their reimpl gaps tracked separately
(the same functions ship in retail: foliage detail starts at
`generate_foliage_instances_0 @ 0x5ffdd0`, while `0x600197` is only an internal
sample; tiles use `PolyTrn_RenderTile @ 0x60df0d`).
**The CBIN codec IS in retail JO** — the magic is a binary constant a string search
misses; `find_bytes 43 42 49 4E` finds the writer at `~0x75e250` and the cipher
loop at `0x75e348` (`rol ebx,7` + `xor [blob],key&0xFF`, 4-byte groups), byte-exact
to `engine/formats/cbin`. The two partials (Terrain, Credits) have their remaining grills
scoped in their records; the six code-cited-jodemo systems are retail-anchored
where they ship.

### Render (REN) — reopened 2026-07-05, burned back to `UNAUDITED` = 0 on 2026-07-06

The REN planning grill ([ADR 0023](adr/0023-render-visual-parity.md),
[maturity-program.md](maturity-program.md) REN track) found the runtime render
path silently uncovered — the one substantially **reimplemented but
unwitnessed** surface: the object-material chain (the 45-entry
shader-tag table, then in `libs/oed` and retired with the OED authoring
layer in ADR 0038, + the `engine/runtime/renderer` classifier/composer +
`ObjectShaderCache`) carries only ModSuperOed-side citations, and no
record covers batching/draw order, the runtime TSS stage tables, or lighting
application. Three systems enter `UNAUDITED`; the REN grill slices convert
them into records with catalogs (raising open counts before the burn-down
lowers them, as the R-audits did):

| System | Slice | Record |
|---|---|---|
| Object materials / render state (the runtime flag/tag→state path) | REN-2 | **landed 2026-07-06** — [render/render-material-re.md](render/render-material-re.md) (D-RMAT — open rows tabled above, closures below) |
| Batching / draw order / pass structure | REN-3 | **landed 2026-07-06** — [render/render-order-re.md](render/render-order-re.md) (D-RORD — open rows tabled above, closures below) |
| Lighting (modulator chain, entity lights, terrain lightmaps) | REN-5 | **landed 2026-07-06** — [render/render-lighting-re.md](render/render-lighting-re.md) (D-RLIT, tabled above) |

Terrain-TSS and sky/water shader findings grow the existing
[terrain/terrain-re.md](terrain/terrain-re.md) and
[env/env-tod-re.md](env/env-tod-re.md) records in place rather than forking
new ones.

## Standing rules

1. **Every RE record carries a D-catalog** of stable, never-renumbered `D-<DOMAIN>-n`
   IDs. A record without one is normalized on its next touch.
2. **A new divergence gets its ledger row at birth** — discovering it and tracking it are
   the same act.
3. **Closing a row requires the witness citation in the closing commit** — the
   `[orig: Name @ 0xADDR]` (or capture/test) that proves the behavior now matches.
4. **`PERMANENT` requires the register** — an entry moves to `PERMANENT` only by landing
   in [ADR 0022](adr/0022-divergence-burn-down.md)'s register (or a domain ADR) with its
   rationale.
5. **The ledger is updated in the same PR that changes a disposition** — the dashboard
   never lags the tree.
6. **Closed rows are condensed, not hoarded** — once a row's full detail lives in its record's catalog, the table row is replaced by a dated prose closure line ("Closed <date>: **ID** -> `FIXED` — <one line> (full entry: <record>)"). The scoreboard's "Closed rows still tabled" column counts down to zero, and a domain with zero open and zero tabled-closed rows drops off the scoreboard (the pruned state). Ids stay stable and greppable in the closure lines forever; `host_lint.py --frozen-audit` verifies nothing stops resolving.

---

## Normalized prose-only catalogs (this train)

Five records originally tracked divergences in prose only; PAR-0 minted stable IDs from their
then-existing text. Later evidence passes have extended the particle catalog through D-PTL-23:

- [threedi/3di-gp-format-re.md](threedi/3di-gp-format-re.md) →
  **D-3DI-1..2** (the MTRX SSE2 low-FP-bit divergence, `PERMANENT`; and the
  bounded runtime CTRL producer/joiner gap, `OPEN`).
- [threedi/3di-lw-format-re.md](threedi/3di-lw-format-re.md) → **D-3DILW-1..3** (retired from the
  tables 2026-08-28: the parser is not in the tree; the record's §4 keeps them) (v8
  branch, textures, SAF/KSA playback — the record's own deferrals).
- [particles/ptl-format-re.md](particles/ptl-format-re.md) → **D-PTL-1..23** (the
  intentional parse mapping, renderer/runtime approximations, platform-stable substitutions,
  and bounded-safety choices; pure "not yet researched" §8 items stay in §8; D-PTL-8 is closed).
- [mission/mis-format-re.md](mission/mis-format-re.md) → **D-MIS-1..5** (the
  writer-subset gaps + the full `dfx2med.exe` grill as a `NEEDS-RE` row;
  D-MIS-4/-5 minted-and-FIXED at the 2026-07-07 Nile parity pass).
- [render/render-occlusion-re.md](render/render-occlusion-re.md) → **D-OCC-1..15**
  (D-OCC-1..8: the blink-box visibility consumer witness, 2026-07-16 — open
  WITNESS details, not port divergences; they stay deliberately record-only.
  The port pass's divergences D-OCC-9..15 were record-only until 2026-08-30,
  when the post-merge tidy tabled them in the "Render — occlusion" section
  above and registered the four deliberate ones; the section-mask/portal
  engine port LANDED 2026-07-17 (engine/runtime/world/occlusion.cpp). The
  slice's ported halves: sound occlusion closed **D-SND-7** and minted
  **D-SND-9** in the audio record's catalog; the indoor frame gates ride
  `GameWorld` with their deferred override legs pointed at the record).
