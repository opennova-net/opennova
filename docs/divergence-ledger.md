# Divergence burn-down ledger

Every tracked divergence between OpenNova and the original engine, in one place,
under one vocabulary, with a target of **zero OPEN entries**. This is the dashboard
for the maturity program's PAR (parity burn-down) track; the policy that ratifies it
is [ADR 0022](adr/0022-divergence-burn-down.md), and the program that schedules the
slices is [maturity-program.md](maturity-program.md).

## Purpose

The documentation index already states the rule: *a divergence is a tracked decision,
never an accident* ([docs/README.md](README.md)). The burn-down adds the second half:
**and every tracked divergence is either on a path to closure or ratified permanent.**

Three maintainer decisions (2026-07-05) stand behind this ledger:

1. **Target = zero OPEN.** Every tracked divergence is driven to one of two terminal
   states — *ported-and-closed* (`FIXED`), or *ratified deliberate* (`PERMANENT`,
   in [ADR 0022](adr/0022-divergence-burn-down.md)'s register or a domain ADR). Nothing
   is allowed to sit "known-broken" untracked.
2. **Starts NOW, in parallel with Wave 1.** The freeze (see
   [maturity-program.md](maturity-program.md)) otherwise blocks new reimplementation
   work; the maintainer granted a **per-slice freeze exemption for PAR slices**, because
   the burn-down pays down existing debt rather than adding surface. Env-domain closures
   are implemented **libs/env-first** so the ENG-2 port (env GDScript → `libs/env`) does
   not pay for the same math twice.
3. **The seven systems with no RE record get research audits.** Terrain, foliage, tiles,
   fonts, credits, the importer pipeline, and the VFS/PFF mount stack are `UNAUDITED`:
   their divergences, if any, are untracked. Audit slices (PAR-R1..R7) turn unknown
   unknowns into tracked rows.

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
porting a witnessed behavior; **B** = needs more RE first; **C** = platform/host-structural
(candidate `PERMANENT`); **D** = original-bug/garbage class (candidate `PERMANENT`). Where a
record splits a divergence into facets (e.g. D-NET-133), the facets get separate rows.

### Net — [net/novaworld-net-re.md](net/novaworld-net-re.md) (D-NET catalog)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-NET-9 | Gate literal parse is decimal-only; the 4-radix (char/hex/octal/binary) port is witnessed-and-ready | A | WITNESSED-READY-DEFERRED | PAR-NET |
| D-NET-17 | ClientHello `DE`/`PV3`/`PM`/`ET` parser fields unmodeled (server side witnessed) | A | OPEN | PAR-NET |
| D-NET-20 | Verify request must emit the `ClientVarList(Cookie)` parent unconditionally (`includeAll=1`) | A | OPEN | PAR-NET |
| D-NET-21 | `ClientConnected` emitted synchronously; retail waits one periodic tick (state 5/2) | A | OPEN | PAR-NET |
| D-NET-22 | Verify Cookie var-list is data-driven from client env; the registry/Win32 glue belongs in the Godot binding | A | OPEN | PAR-NET |
| D-NET-29 | Envelope variable-header (`first-dword==0`) decode mode unsupported — documented scope | A | WITNESSED-READY-DEFERRED | PAR-NET |
| D-NET-30 | Emit one `Cookie:` header per cookie; jar keyed by subnet-truncated host | A | OPEN | PAR-NET |
| D-NET-49 | `jointoperations_pg()` is a placeholder; the in-match PG (16 B @ proto+284) is unwitnessed | B | NEEDS-RE | PAR-NET / research starter |
| D-NET-64 | Guided-weapon record: structural port done, 0x0C-dispatch wiring deferred, wire-unvalidated (no capture carries guided traffic) | B | WITNESSED-READY-DEFERRED + NEEDS-RE (capture) | PAR-NET |
| D-NET-97 | Pool routing by BMS `EntityKind`, not item-def capability flags (crash fixed; the static-vs-destructible simplification is residual) | A | OPEN | PAR-NET |
| D-NET-116 | Pending-spawn load-complete gate (`dword_24D1DE0`) not modeled; latent for a driver that wires `ctx.world` during load | A | WITNESSED-READY-DEFERRED (latent) | PAR-NET |
| D-NET-117 | World-path pose look-pitch not sourced (0 until the AiEntity look-pitch is threaded into the pose builder) | A | WITNESSED-READY-DEFERRED | PAR-NET |
| D-NET-121 | The dvxi5 `fallback_anchor` still bites the no-owned-entity edge (a mid-match despawn); mostly resolved at P5 | A | OPEN (narrow residual) | PAR-NET |
| D-NET-123 | `Server_TickUpdate` owns the logic tick; the double-tick guardrail is comment-only | A | WITNESSED-READY-DEFERRED (latent) | PAR-NET |
| D-NET-124 | Drain/emit fan assumes type-1 (remote-joiner) nodes stay resident across a mid-match `configure_session_runtime()` | A | WITNESSED-READY-DEFERRED (latent) | PAR-NET |
| D-NET-125 | The single-drain / single-tick invariant is comment-only (nothing blocks a `NetSystem` + `Server_TickUpdate` double-owner) | A | WITNESSED-READY-DEFERRED (latent) | PAR-NET |
| D-NET-127 | Reactive-reply residual: the 0x46 per-field slot-state VALUES + the 0x51 NetId/anim binding plumbing (shape faithful) | A | OPEN (LOW residual) | PAR-NET |
| D-NET-133 | S2C 0x18 spawn approximations: `item_type` from pool, name-gate on `e.name`, attach-parent/ground-entity unmodeled → 0xFFFF | A | OPEN (approximation) | PAR-NET |
| D-NET-134 | Per-frame 0x0A sub-block phase cycles the safe subset `{1,0,3}`, omitting env(2) + passenger pending `world.env` authoring + mount modeling | A | OPEN (approximation) | PAR-NET |
| D-NET-135 | World-stream paging uses a flat 640-B pre-check vs the original's 650-B budget with a per-pool post-write margin (interop-equivalent, not byte-identical batching) | A | OPEN | PAR-NET |
| D-NET-136 | 0x0C `entity+36` bit 0x01 computed per-recipient; diverges for ≥3 players; the faithful per-entity stamp needs the `NapiNPPlayer+0x37` gate witnessed | B | OPEN + NEEDS-RE (the +0x37 writer) | PAR-NET |
| D-NET-137 | Player wire net_id is an invented encoding shim, not the minimap-slot packing — tolerable because the client self-heals unmatched ids | A | WITNESSED-READY-DEFERRED (tolerable) | PAR-NET |
| D-NET-139 | 0x0A priority score ports distance/age/own-boost; the view-interest / LOS / enemy-team-bonus terms contribute 0 | A | OPEN (approximation) | PAR-NET |
| D-NET-147 | Residual 0x10 tail: sectioned-destructible `sectionMask` rebuild + armory `weaponByte`/`attachRef` + `scoreFlag` gate deferred (the four base fields fixed + streamed) | A | WITNESSED-READY-DEFERRED | PAR-NET |

Closed net entries with a permanent facet are listed in the permanent register below
(D-NET-131, D-NET-133 empty-slot facet, D-NET-140).

### Environment — [env/env-tod-re.md](env/env-tod-re.md) (#-catalog) + [env/env-honored-matrix.md](env/env-honored-matrix.md)

Implemented **libs/env-first** so the ENG-2 port inherits the closures. The
honored-matrix PARTIAL rows (terrain_tint, iris, ceiling/floor, lightning, glare_3di,
skyfog) map onto these `#` entries.

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| env #14 | Sun-glare terrain-raycast occlusion held at full brightness (8-jittered-ray + ±16/frame hysteresis unmodeled) | A | OPEN (PARTIAL) | PAR-ENV |
| env #15 | Thunder SoundBank triggers (0 / 0x80) + `SETFLASH1` start — fully specced, wiring deferred to WAC weather | A | WITNESSED-READY-DEFERRED | PAR-ENV |
| env #16 | `.trn`/`overcast.def` first-pass TOD table + overcast cross-fade — precedence corrected, runtime carries the `.env` table only until WAC weather lands | A | WITNESSED-READY-DEFERRED | PAR-ENV |
| env #17 | Iris auto-exposure modulator gain — curve + consumer chain recovered, no modulator chain built (`get_terrain_lighting_attenuation` returns identity for the iris path) | A | WITNESSED-READY-DEFERRED | PAR-ENV |
| env #18 | Earthquake / rain / wind oscillator rings — constants documented, wiring deferred to WAC weather | A | WITNESSED-READY-DEFERRED | PAR-ENV |
| env #19 | `terrain_rgb` terrain-stack consumers (texture bake ×v≫12, water-quad half-tint, foliage lightmap ×/128) render none — attenuation hard-returns identity | A | OPEN (PARTIAL) | PAR-ENV |
| env #21 | `skyfog` frame-clear color (cross-faded skyfog↔fog at low fog distance) not wired; host scenes choose their own background | A | OPEN | PAR-ENV |

### World / AI + mission events — [world/world-wac-ai-re.md](world/world-wac-ai-re.md), [mission/bms-event-runtime-re.md](mission/bms-event-runtime-re.md), [world/itemdef-re.md](world/itemdef-re.md)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-INF-1 | No blend windows on clip switches (the original blends 10/15 ticks, root motion included) | A | WITNESSED-READY-DEFERRED (rides the skeletal/blend pass) | PAR-WORLD |
| D-INF-2 | Command channels 123–127 (mount/waypoint) partially driven; walk-to-seat staging, 126/127, child-seat traversal, seat-bone follow, driver-lean pending | A | OPEN (partial) | PAR-WORLD |
| D-INF-3 | Ground/water resolver: horizontal capsule + platforms/water + airborne anim overlay pending (the vertical capsule-bottom settle landed as D-INF-6) | A | OPEN (partial) | PAR-WORLD |
| D-INF-4 | Computed sin/cos tables vs the runtime-built originals (`trunc(f(idx)·2^22)`) | A | OPEN | PAR-WORLD |
| D-INF-5 | Idle look-at system + its spotting side effects — rides the combat pass | A | WITNESSED-READY-DEFERRED | PAR-WORLD |
| D-EVT-1 | Spawn-point activation on fire is unported (linked spawn points not marked) | A | OPEN | PAR-WORLD |
| D-EVT-2 | Quarter-pass piggyback (`@0x454d50`) skipped | A | OPEN | PAR-WORLD |
| D-EVT-3 | Condition categories 1 (team/zone matrix), 5 (load-toggle), 6 (net) unmodeled (return false) | A | OPEN | PAR-WORLD |
| D-EVT-4 | Pre/post-pass call frequency unwitnessed | B | NEEDS-RE | PAR-WORLD |

Closed 2026-07-05: **D-ITEMDEF-1** → `FIXED` (faec4b3e — `item_type_from_string`
witnessed mapping `[orig: ItemDef_ParseProperty @ 0x49eb00]`;
[world/itemdef-re.md](world/itemdef-re.md) verdict flipped to MATCHING). The first
ledger row driven to zero.

Unnumbered latent divergence (needs a `D-EVT-5` mint the next time
[bms-event-runtime-re.md](mission/bms-event-runtime-re.md) is touched): the BMS second
chunk (header +0x246 bytes) is always `fseek`'d past and never consumed — round-trips
only while that value is 0 (`OPEN`, class A).

### UI — menus/controls, sound, player-info, HUD

Sources: [mnu/menu-re.md](mnu/menu-re.md), [audio/lwf-dbf-sound-re.md](audio/lwf-dbf-sound-re.md),
[playerinfo/avatars-re.md](playerinfo/avatars-re.md), [interface/hud-re.md](interface/hud-re.md).

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-MNU-5 | Text-item rendering scope: combo/list image/color items not backed (shipped menus are text-only there) | A | OPEN | PAR-UI |
| D-MNU-6 | CBIN credits custom `~F` fonts / `~I` images not resolved from the resource root (default font only) | A | OPEN | PAR-UI (see credits audit PAR-R5) |
| D-CTRL-1 | Mouse/joystick binding arrays (profile-built at runtime) not ported; those rows show a blank Control column | A | OPEN | PAR-UI |
| D-CTRL-2 | Control-list visibility filter approximated (hide admin classes) vs the per-entry show-flag `(*entry & 0x20)==0 && (*entry & 0x800)!=0` | A | OPEN (approximation) | PAR-UI |
| D-CTRL-3 | Live double-click rebinding / DEFAULTS / CLEAR_KEY / profile persistence deferred — gated on a real input-action layer | A | WITNESSED-READY-DEFERRED | PAR-UI |
| D-SND-2 | Expansion banks (`<exp>L.lwf` / `<exp>.lwf`) not loaded (no expansion slot yet) | A | OPEN | PAR-UI |
| D-PLAYERINFO-1 | In-world (spawned-player) combo→3D-model binding untraced (the preview is witnessed + fully ported) | B | NEEDS-RE | PAR-UI / research starter |
| D-PLAYERINFO-2 | The 512-part pool cap should be enforced as an error (not silent truncation) | A | OPEN | PAR-UI |
| D-PLAYERINFO-7 | `PLAYER_INFO` screen orchestration (init + 28-control registration + nat→div→combo cascade) host wiring is the next phase | A | WITNESSED-READY-DEFERRED | PAR-UI |
| D-PLAYERINFO-9 | ACCEPT/commit + profile persistence host wiring is the next phase | A | WITNESSED-READY-DEFERRED | PAR-UI |
| D-PLAYERINFO-10 | Voice preview (`VOICE_%d` via `menu.lwf`) host wiring is the next phase | A | WITNESSED-READY-DEFERRED | PAR-UI |
| D-PLAYERINFO-11 | Loadout ammo combos + weight readout remaining (weapon lists implemented) | A | OPEN (partial) | PAR-UI |
| D-PLAYERINFO-12 | Per-(slot, team) selection-state globals host wiring is the next phase | A | WITNESSED-READY-DEFERRED | PAR-UI |
| D-HUD-1 | Stance indicator = discrete cross-faded `HUDSTANCE` frames (IDB `draw_minimap_compass_overlay` + oscarmike model it as a compass) | A | OPEN (HUD port in flight) | PAR-UI |
| D-HUD-2 | Stance widget = frame-swap + fade; heading/north is a *separate* top-down radar (do not port a rotating ring) | A | OPEN (port in flight) + NEEDS-RE (radar) | PAR-UI / research starter |
| D-HUD-3 | HUD design space is fixed 1024×768, scaled round-to-nearest (`Viewport_ScaleToVirtualCoords`) | A | OPEN (port in flight) | PAR-UI |
| D-HUD-4 | Health-bar fill WIDTH uses the capped `+92` ratio; fill COLOR uses an uncapped recomputed ratio — the port matches both reads | A | OPEN (port in flight) | PAR-UI |

### Format ports — mission `.mis`, LW `.3di`, particles `.ptl`

Sources: [mission/mis-format-re.md](mission/mis-format-re.md),
[threedi/3di-lw-format-re.md](threedi/3di-lw-format-re.md),
[particles/ptl-format-re.md](particles/ptl-format-re.md). IDs minted this train (see
"Normalized prose-only catalogs" below).

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-MIS-1 | All `begin item` records land in the generic pool; the pool-kind classifier needs a `dfx2med.exe` grill against `items.def`/type flags | B | NEEDS-RE | PAR-WORLD |
| D-MIS-2 | `weapon_availability` emitted empty + skipped on read; loadout semantics deferred | A | WITNESSED-READY-DEFERRED | PAR-WORLD |
| D-MIS-3 | Full `dfx2med.exe` `.mis` grammar unmapped (hand-authored / legacy variants beyond the writer subset) | B | NEEDS-RE | PAR-WORLD |
| D-3DILW-1 | v8 branch deferred (v10-only parser; the NovalogicTools v8 layout is unvalidated against the 3 local v8 files) | B | NEEDS-RE | rides an LW-import revival |
| D-3DILW-2 | Textures deferred (geometry + one-weight skinning parsed; material textures not ported) | A | WITNESSED-READY-DEFERRED | rides an LW-import revival |
| D-3DILW-3 | SAF/KSA playback intentionally not applied (`parsed_not_applied_pending_re`; the pose recipe is pinned, end-to-end validation pending) | B | NEEDS-RE | rides an LW-import revival |
| D-PTL-3 | `mod2x` approximates `DESTCOLOR`/`SRCCOLOR` over Godot `blend_mul`; the 0.5 midpoint is preserved, the gamma curve is not byte-exact | A | OPEN (approximation) | PAR-UI/render |
| D-PTL-4 | `bump`/`bumpadd` lit-color rotation about view-Z vs the engine's composite-matrix X (combiner topology matches; pending a 4×4 port + reference capture) | A | OPEN (approximation) | PAR-UI/render |
| D-PTL-5 | `distort` fixed-strength screen-tex UV offset; the engine stage-1 combiner bytes are undecoded | B | NEEDS-RE | PAR-UI/render |
| D-PTL-6 | Atlas pack strategy undecoded (shelf vs the engine's layout); `inset` bleed padding defaults to 0 | B | NEEDS-RE | PAR-UI/render |

The LW `.3di` record is unlanded overall (PR #45 closed); its rows ride whenever an LW
import is revived. Note D-3DILW-1's v8 branch overlaps the 3DI/GP audit surface only at
the container-detection seam.

### Boot-required resources — [required-resources.md](required-resources.md) (D-BOOT catalog; R8/ENG-6)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-BOOT-1 | Menu/game music bank resolution: retail hardcodes `MENUMUS.SBF/.BIN` + `GAMEMUS.SBF/.BIN` (`M<exp>`/`G<exp>` under an expansion); `menu_shell.gd` scans by name heuristic instead | A | OPEN | rides the ENG-6 manifest (Wave 2) |

---

## Count-to-zero scoreboard

Open counts by domain (the target is zero in every cell):

| Domain | OPEN | NEEDS-RE | WITNESSED-READY-DEFERRED | Domain open total |
|---|---|---|---|---|
| Net | 10 | 0 | 12 (2 also NEEDS-RE) | 22 |
| Environment | 2 | 0 | 5 | 7 |
| World / AI + events | 6 | 1 | 2 | 9 |
| Item def | 0 (D-ITEMDEF-1 `FIXED` 2026-07-05) | 0 | 0 | 0 |
| UI (menu/ctrl/sound/playerinfo/HUD) | 11 | 1 (+1 dual) | 5 | 17 |
| Mission `.mis` | 0 | 2 | 1 | 3 |
| LW `.3di` | 0 | 2 | 1 | 3 |
| Particles `.ptl` | 2 | 2 | 0 | 4 |
| Boot resources (new domain, R8 audit) | 1 | 0 | 0 | 1 |
| **Total OPEN** | | | | **66** |

The Boot-resources row is the R8 audit doing its job: an audit that converts
unknown unknowns into tracked rows RAISES the count before the burn-down
lowers it (the same will happen at PAR-R1..R7).

Plus one unnumbered latent divergence (BMS second chunk) awaiting a `D-EVT-5` mint.
Permanent register size: **13** (below). `UNAUDITED` systems: **7** (below).

---

## Permanent-candidate register (feeds [ADR 0022](adr/0022-divergence-burn-down.md))

Ratified deliberate divergences — each verified against its record. Two classes:
**platform/host-structural** (the host cannot or should not reproduce the original's
substrate) and **original-bug/garbage** (reproducing it would manufacture garbage against
[ADR 0003](adr/0003-no-raw-passthrough-create-from-scratch.md)). Each carries the
one-line rationale for why porting it would be *wrong*.

### Platform / host-structural (class C)

| ID | Divergence | Why porting it would be wrong |
|---|---|---|
| env #20 (residual) | Sky-dome clip-space proximity dot runs in Godot's reverse-Z clip conventions, not D3D's | The dome combine is a structural port; the z-scale difference is the host graphics API's clip space, not a math error to "fix". |
| D-3DI-1 | MTRX byte-exact output needs OED's x87 `_PC_24` precision; a 64-bit SSE2 build diverges in low FP bits | Byte-exactness is a property of the original's 24-bit x87 mantissa; a modern SSE2 host cannot match the low bits without the documented `_controlfp(_PC_24)` parity sub-build. |
| D-MNU-4 | The original truncates each scaled quad rect to int per element; the reimpl applies one float `CanvasItem` scale | A sub-pixel cosmetic difference; reproducing per-element int truncation would fight Godot's scene-graph scale model for no visible gain. |
| D-PTL-2 | One mesh batch per graphic layer vs the engine's shared vertex/index buffer pooling | A host renderer architecture choice; visually equivalent, and pooling is a performance strategy, not observable behavior. |
| D-NET-131 | A dedicated ("serve only") host runs as a mode-3 in-process listen server (`serve_and_play=false`), not the original's mode-1 host-only | Wire-equivalent from a joiner's view ([ADR 0011](adr/0011-single-player-in-process-listen-server.md)); the difference is host-internal bookkeeping that never reaches a connected client. |
| D-NET-140 | The listen host's own loopback connection receives the full 0x0A record set; retail sends its local player header-only frames | The full-record loopback is how serve-and-play renders its local view ([ADR 0011](adr/0011-single-player-in-process-listen-server.md)); that frame never leaves the process, so retail interop is unaffected. |

### Original-bug / garbage class (class D; basis: [ADR 0003](adr/0003-no-raw-passthrough-create-from-scratch.md))

| ID | Divergence | Why porting it would be wrong |
|---|---|---|
| env #11 | Original packs negative color components as garbage (no lower clamp); the reimpl clamps to 0 | Reproducing unclamped negative-color UB would carry garbage bytes through the parser for no defined behavior. |
| D-NET-133 (empty-slot facet) | An in-capacity EMPTY 0x18 slot replies a zeroed type-0 record; retail serializes the slot's raw (possibly stale) memory | The observable effect is identical (the client stops at the type gate either way); reproducing retail's stale-memory bytes would be manufacturing garbage. |
| D-MUS-7 | `op_callvl` (`0x0A` call form) resolves against an uninitialised-BSS name table in Jointops, so the opcode is dead; the reimpl mirrors the dead stub (push 0) | The original behavior *is* "do nothing" (the table is never populated); porting a "working" call would invent behavior the engine never had. |
| D-MUS-5 | `inc_g`/`dec_g` (`0x11`/`0x12`) operate on 1 byte and raise no globals-dirty notify | An intentional mirror of the original's silence; adding the notify would diverge from the witnessed behavior. |
| D-PTL-1 | The engine's outer dispatcher remaps `g2_color1`/`g3_color1`/… into higher color slots (a parse bug); the reimpl maps `g{N}_color{M}` correctly | A recorded intentional divergence: the correct mapping is what an author means; reproducing the dispatch remap would carry the engine's parse bug forward. |
| D-SCR-1 / D-SCR-2 | The SCR container codec accepts version bytes 0–2 and selects the key from the version byte + policy, where each original call site fixes the key | A deliberate multi-title superset so one codec serves JO-demo-era and shader containers; load-bearing equivalence holds for everything retail JO ships. |

`PERMANENT` is not a resting place for hard work: each entry above is a decision that the
*faithful* behavior is to diverge. If a future need arises (e.g. exact host-internal-state
match for D-NET-131, or a byte-exact MTRX parity sub-build for D-3DI-1), the record names
the follow-up path.

---

## UNAUDITED systems (no RE record yet)

Seven systems are documented mainly by code and tests
([docs/README.md](README.md)). Each gets a research audit (engine-research / grill-ida)
that lands an RE record **with a D-catalog**, converting untracked divergences into
tracked rows.

| System | Audit slice | Partial coverage today |
|---|---|---|
| Terrain | PAR-R1 | [oned/editor-runtime-parity.md](oned/editor-runtime-parity.md) records the intentional terrain-shader edit/runtime split (shared surface-shading include). |
| Foliage | PAR-R2 | none (placement port witnessed in code; no RE record). |
| Tiles | PAR-R3 | none. |
| Fonts | PAR-R4 | the FNT shelf packer + format facts live in `fnt_rasterizer.gd`; ENG-4 plans the `libs/fnt` extraction. |
| Credits | PAR-R5 | D-MNU-6 (CBIN credits custom fonts/images) is the one tracked credits divergence. |
| Importer pipeline | PAR-R6 | none (behavior in `apps/importer/` + tests). |
| VFS / PFF mount stack | PAR-R7 | the PFF write side is [ADR 0008](adr/0008-pff-writer-policy.md); the mount stack itself is unaudited. |

---

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

---

## Normalized prose-only catalogs (this train)

Four records tracked divergences in prose only; PAR-0 minted stable IDs from their own
existing text (no new findings, no reworded witnesses):

- [threedi/3di-gp-format-re.md](threedi/3di-gp-format-re.md) → **D-3DI-1** (the MTRX
  SSE2 low-FP-bit divergence; `PERMANENT`).
- [threedi/3di-lw-format-re.md](threedi/3di-lw-format-re.md) → **D-3DILW-1..3** (v8
  branch, textures, SAF/KSA playback — the record's own deferrals).
- [particles/ptl-format-re.md](particles/ptl-format-re.md) → **D-PTL-1..6** (the
  intentional `g{N}_color{M}` map + the §6 bounded deviations; pure "not yet researched"
  §8 items stay in §8).
- [mission/mis-format-re.md](mission/mis-format-re.md) → **D-MIS-1..3** (the
  writer-subset gaps + the full `dfx2med.exe` grill as a `NEEDS-RE` row).
