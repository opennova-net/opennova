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
porting a witnessed behavior; **B** = needs more RE first; **C** = platform/host-structural
(candidate `PERMANENT`); **D** = original-bug/garbage class (candidate `PERMANENT`). Where a
record splits a divergence into facets (e.g. D-NET-133), the facets get separate rows.

### Net — [net/novaworld-net-re.md](net/novaworld-net-re.md) (D-NET catalog)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-NET-9 | Gate literal parse is decimal-only; the 4-radix (char/hex/octal/binary) port is witnessed-and-ready | A | WITNESSED-READY-DEFERRED | PAR-NET |
| D-NET-17 | ClientHello `DE`/`PV3`/`PM`/`ET` parser fields unmodeled (server side witnessed) | A | OPEN | PAR-NET |
| D-NET-21 | `ClientConnected` emitted synchronously; retail waits one periodic tick (state 5/2) | A | OPEN | PAR-NET |
| D-NET-22 | Verify Cookie var-list is data-driven from client env; the registry/Win32 glue belongs in the Godot binding | A | OPEN | PAR-NET |
| D-NET-29 | Envelope variable-header (`first-dword==0`) decode mode unsupported — documented scope | A | WITNESSED-READY-DEFERRED | PAR-NET |
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
| D-NET-163 | The dev golden-harness host (`nw_server`, ADR 0013) does not emit retail's full S2C tag set — 21 tags across the anti-cheat/CRC challenge, gameplay-event, and low-frequency session/roster families; the `nw_golden_diff` baseline now names each deferral by ID | A | WITNESSED-READY-DEFERRED | PAR-NET |

Closed 2026-07-05: **D-NET-30** -> `FIXED` — one `Cookie: name=value;` header
per cookie (`CookieJar::cookie_header_lines()`; our own server already merged
multiple `Cookie:` headers, so the merged-line client was the sole
inconsistency) + the subnet key ported (`subnet_key()`, IPv4 /16)
`[orig: CUIBrowser_SendHTTPRequest @ 0x658840; Network_TruncateIPToSubnet
@ 0x62dfe0]`; C2 summary row flips to matching.

Closed net entries with a permanent facet are listed in the permanent register below
(D-NET-131, D-NET-133 empty-slot facet, D-NET-140). Closed 2026-07-05: **D-NET-20**
-> `FIXED` (the Cookie var-list parent emitted unconditionally; empty-cfg flow pinned
in `client_session_loopback_test`).

### Environment — [env/env-tod-re.md](env/env-tod-re.md) (#-catalog) + [env/env-honored-matrix.md](env/env-honored-matrix.md)

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
U sign; NovaWeatherCore now owns the witnessed CloudScrollState) and minted
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
(`NovaStarField` + `nova_celestial.gd`; per-star twinkle, 0.98 near-light
cull, regenerate-per-load); the single-body stand-in deleted. Details:
[env/env-tod-re.md](env/env-tod-re.md).

**env #30** minted NEEDS-RE (the reflection passes exist; spec deferred; internals closed at the REN-6 witness leg). The
celestial leg (2026-07-06) CLOSED **env #14** (the glare occlusion — the witnessed
model is 2 jittered rays/frame into an 8-sample sliding window + dead-band
hysteresis, ported libs/env-first with the NovaCelestial terrain ray march) and
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
| env #15 | Thunder SoundBank triggers (0 / 0x80) + `SETFLASH1` start — fully specced, wiring deferred to WAC weather | A | WITNESSED-READY-DEFERRED | PAR-ENV |
| env #16 | `.trn`/`overcast.def` first-pass TOD table + overcast cross-fade — precedence corrected, runtime carries the `.env` table only until WAC weather lands | A | WITNESSED-READY-DEFERRED | PAR-ENV |
| env #17 | Iris auto-exposure modulator gain — **FIXED 2026-07-06 (REN-5)**: the modulator CHAIN is live (`env::ModulatorChain` ticks modulator2 → modulator → the hosted blocks in the witnessed order `[orig: @ 0x57ef97..0x57f03c]`; 62-tick exposure chase `[orig: @ 0x57e512; @ 0x57d940]`; ÷64 gain to `ColorSrcGlobalGain`/ambient scale `[orig: @ 0x58db30; @ 0x5aaef0]`); env vectors re-dumped surgically (8 weather rows). Sampling-geometry + unhosted-block residuals tracked as D-RLIT-1/-2 ([render/render-lighting-re.md](render/render-lighting-re.md)) | A | FIXED | REN-5 (from PAR-ENV) |
| env #18 | Earthquake / rain / wind oscillator rings — constants documented, wiring deferred to WAC weather | A | WITNESSED-READY-DEFERRED | PAR-ENV |
| env #29 | Water surface tessellation — **FIXED 2026-07-07 (the REN-6 tail)**: the DETAILED tier live end to end (`env::water_*` structural translation with 40 ctest pins → `NovaWaterCore.strip_*` packed arrays → the per-frame strip ArrayMesh → the witnessed ps.1.1 chain in water.gdshader, found at the port's debug: alpha = noiseA×diffuseA×2, reflection ×2 diffuse ×4 noise + specular `[orig: Water_InitSurfaceShaders @ 0x5c19b0]`); goldens re-pinned. 2026-07-07 fidelity facets: the far-fade discard misport deleted (#34 re-grade), the witnessed spec-alpha fog factor + z-write/depth-replica model hosted (`depth_draw_always` + the tracked relative `3×10⁻⁴` view-depth pull; the rejected 2⁻¹⁵ NDC form overpainted far-altitude terrain), the underwater opaque `0x20000` swap HOSTED (`u_underwater_view` premul branch). Residuals in-row (env-tod-re.md #29): the LOW tier is unported (host runs detail > 1) and the nightvision redraw is unhosted | A | FIXED | REN-6 tail |
| env #30 | Water reflection — **FIXED 2026-07-07 (the REN-6 tail)**: host planar reflection (SubViewport mirror camera about y = wh, up-column-negated proper mirror — the witnessed strip rows pin u = screenU / v = vbase − screenV so the ps.1.1 texm3x2 lookup runs verbatim `[orig: Water_InitSurfaceShaders @ 0x5c19b0; render_main_scene @ 0x5c1240]`) feeding the t2 sampler; water self-excluded via a visual layer. **2026-07-14 fidelity correction:** the RTT is fixed retail detail-2 `256×256` (not half-display), its square projection preserves horizontal FOV `[orig: allocator @ 0x5c08d1..0x5c0937; viewport @ 0x5c1464..0x5c1614]`, and the sky dome anchors from the mirror pass camera. **2026-07-15 view-registration correction:** the completed texm3x2 lookup is scaled around UV center by (1, source height/source width), converting the main-normalized rows into the square RTT projection and eliminating view-pitch swimming. Residual in-row (env-tod-re.md #30): the wh − 0.1 clip plane remains approximated (no host oblique near plane); the retail 512 branch awaits a detail-3/capture selector | A | FIXED | REN-6 tail |
| env #35 | Water sine LUT provenance: the runtime `std::sin` build forked per libm at trunc boundaries (the GitHub `macos-26-arm64` image flipped non-landmark bytes and every downstream noise pixel — `env_render_unit` red on macOS only, 2026-07-10); the original builds ONE deterministic instance via x87 fsin `[orig: Water_InitNoiseFieldAndSineLut @ 0x5c0308..0x5c0334]` | C | FIXED (2026-07-10): the LUT is a committed 256-byte constant — the deterministic instance every existing pin was generated from; landmarks + symmetry sum unchanged; the retail-instance byte check rides the pinned-current caveat (env-tod-re.md #35) | fidelity 2026-07-10 |

### World / AI + mission events — [world/world-wac-ai-re.md](world/world-wac-ai-re.md), [mission/bms-event-runtime-re.md](mission/bms-event-runtime-re.md), [world/itemdef-re.md](world/itemdef-re.md)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-INF-1 | No blend windows on clip switches (the original blends 10/15 ticks, root motion included) | A | WITNESSED-READY-DEFERRED (rides the skeletal/blend pass) | PAR-WORLD |
| D-INF-2 | Command channels 123–127 (mount/waypoint) partially driven; walk-to-seat staging, 126/127, child-seat traversal, seat-bone follow, driver-lean pending | A | OPEN (partial) | PAR-WORLD |
| D-INF-3 | Ground/water resolver: platforms/water residuals pending (the vertical capsule-bottom settle landed as D-INF-6; the horizontal capsule landed with D-COL; the airborne overlay's PLAYER 31 leg landed 2026-07-16 §22 — org1 plain falls keep the clip by design (parachute-gated ladder), the 47 leg rides D-INF-20) | A | OPEN (partial) | PAR-WORLD |
| D-INF-5 | Idle look-at system + its spotting side effects — rides the combat pass | A | WITNESSED-READY-DEFERRED | PAR-WORLD |
| D-INF-11 | Third-person body aim overlay (torso bend) — witnessed in full + **LOCAL PLAYER PORTED 2026-07-08** (world-wac-ai-re §14/§14.6: libs/anim aim_overlay + leg-chase sim + eval_pose_overlay path, probe-verified); **upper-body weapon channel producer WITNESSED + LOCAL PLAYER FULLY PORTED 2026-07-09** (§14.8: entity secondary AnimMap channel +0x18C; reload 65/66 via the +0x372 80-tick window; the weapon.def `special_hold` kind ladder → hold poses 50–61 + scoped variants; `attack_anim` → the 62/63 fire stamps; the +0x371 arms-dip feed into headLookDecay; mask-bone hard override before the overlay compose; probes body_reload_probe + body_holds_probe pistol/knife); §14.3 pitch kick RESOLVED = the audio mixer output power meter `g_audioOutLevel` (port needs a host mixer level tap); remaining: NPC/remote threading (closes D-NET-117; AI bodies lack the hold keys — RESET backfill vs our no-op), the binoculars input toggle (ladder side ported), blend windows on channel re-init (D-INF-1), mounted/seated branches, attachments | A | OPEN (partial — local player landed incl. the full weapon channel) | PAR-WORLD |
| D-INF-12 | Player (org2) chase sources — the org2 grill landed 2026-07-16 (world-wac-ai-re §22): the witnessed model has NO body chase — the legs chase the render yaw (¼-step, clamp ±0x3000000, twist ±0x30000000 vs the yaw, per-leg staggered 64-tick re-plant windows) and `bodyHeading` = the leg midpoint (`@0x4b4945-0x4b4ac1`); ported in `tick_infantry` (the org1 approximation deleted), and the org1 legs corrected the same session (midpoint re-plant value, staggered windows, walk half-snap `@0x4be969-0x4bea0b`) | A | FIXED 2026-07-16 (§22; mounted/parachute branches ride D-INF-2/-20) | PAR-WORLD |
| D-INF-13 | Body rigs still consume `.bad` channels as ABSOLUTE bone orientations with the bind-matrix skeleton rest; the original composes every clip's channels against the rig's ONE skeleton bind — the `.adm` slot-0 `.bad` pinned into `channel+44` at registration (`AnimMap_RegisterEntity @0x40bb60`; `AnimChannel_ComputeBoneMatrices @0x410da0` `Transpose(bind 3x3) × channel`; `build_world_bone_matrices @0x40c770` = same composed math as the FP `@0x40c400`). Identical output for healthy exports (channel-at-reset == bind); the faithful `model_bind` path (net-re §5.40 + the 2026-07-09 bind-source + model-table corrections) currently engages for FP viewmodel rigs only. The part↔bone "matcher" question is RESOLVED: the original never matches — rows pair BY INDEX bounded by the model-side table (the FK never reads the `.bad`'s bone count/parents/positions), so 20-parts-vs-19-bones bodies need no map, just the body table semantics: `@0x40c770` reads the 108-byte entity bone table (`skeletonData+104` count, parent `@+40`, 16.16 fixed pivots `@+56/60/64` in (z,x,y) order with x negated, bind-inverse `T(−parent pivot)`, NO bone-0 padding loop) — port that table + a body probe pass to close | A | OPEN (FP landed; bodies pending) | PAR-WORLD |
| D-INF-14 | FP viewmodel `model_bind` composition CORRECTED 2026-07-09: the bind operand is the SKELETON (`.adm` slot-0) `.bad`'s records via the `channel+44` override — per-clip self-bind (the 2026-07-08 reading) self-cancels at clip start and froze the rig at its T-pose (`AnimMap_RegisterEntity @0x40bb60 @0x40bbe3`; `AnimChannel_ComputeBoneMatrices @0x410da0 @0x410dd8`). Composition `q(stored skeleton bind) ⊗ channel`, operand order pinned visually on the ak47 rig (conjugate collapses the rig); `NOVA_VM_DELTA` knob DELETED. **Rig-source corrected same day (the model-table port)**: the rig's count/hierarchy/pivots come from the MODEL table (`modelDef+52/+56`), `.bad` rows pair by index, rows past the anim's bones take bone 0's composed matrix (`@0x40c5a1`) — the reimpl's silent fallback to `BadBone.position` on count mismatch is GONE (it made AKM_1st work only because AKM's pos happen to be healthy; 12/43 JO viewmodel rigs ship zeroed/stale pos and retail renders them all — §5.40 corpus sweep). Remaining tail: def `rot` bias signs + reload direction + finger/left-hand pose vs retail footage | A | OPEN (mechanism witnessed + ported; footage confirm of sense tail pending) | PAR-WORLD |
| D-INF-15 | Model-table rows past the `.bad`'s bone count: the original's flag-2 translation add reads UNINITIALIZED stack floats for those rows (`bone_translations` written only for anim rows, the FK sums it for every model row `@0x40c6e9..0x40c71d`); the reimpl adds zero | D | PERMANENT (register, this slice) | — |
| D-INF-16 | Run-promotion pitch tier ported as the constant 2: the original reads `entity+0x37C` into the `>0x430000/<0→0, ≥0x210000→1, else 2` band before adding `run_anim` (`@0x4b72aa-0x4b72cf`), but the field has NO writer in the retail image (full-image displacement sweep 2026-07-13; pool memory zero-init ⇒ constantly band 2). `player_body_select` bakes the 2 with the thresholds recorded (world-wac-ai-re D-INF-16); lift to a live field if a sibling title writes it | D | PERMANENT (dead-field register; thresholds documented) | — |
| D-INF-17 | Lean producer gate legs unmodeled: the on-foot ramp's `Flags & 0x100020` skip and the prone-roll selection's `0x10000/0x100000` legs (`@0x4b7da2/@0x4b7322`) gate on entity flags the port does not model (alive/prone/airborne carried instead); the seated (`+0x168==1`) ±0x1400000 ramp variant (`@0x4b66b5`) rides the mounting slice | A | OPEN (partial — on-foot ramp/decay + prone rolls landed 2026-07-13) | PAR-WORLD |
| D-INF-18 | FP eye tails: the local head-bone eye (the `@0x4b6bb3` bone-path translation, 0.125u floor, bone INDEX 14), the `−0x3000` view-forward pull-back (`@0x438001..0x438031`), and the full roll term `torsoRoll + lean/4` (producer `@0x4b5cff..6d` + the 41/42 barrel-roll ramp `@0x4b700c..25`) are ported; still open: the 4-sample terrain floor (`@0x4b6c1c`, unless `Flags & 0x800000`), the remote capsule-trig CameraOffset (`@0x4b6984`), and the `2·pitchBlend(+0x380)` recoil pitch term (impulse producer unported). The 3P anchor now chases the host-sampled head-bone eye (`@0x437b70`); its render-skeleton sampling is one frame stale vs the original's sim-side bones | A | OPEN (narrowed 2026-07-13 — eye + pull-back + torsoRoll landed) | PAR-WORLD |
| D-INF-19 | The slope pass's conform selector dropped by the port: every live body chased the terrain lean and the look pitch (`+0x14`) took the slope write, so a standing player's FP camera (`torsoRoll + lean/4 @0x437fe6`) leaned on hillsides with no lean input. Both updaters gate on `def+84 & 0x200 \| animStateFlags & 2 (prone family) \| grounded corpse` with a decay-to-level else (`@0x4ba10f/@0x4ba133`; `@0x4b6d95/@0x4b6dbd`); the org2 player leg runs atan2 slopes / quarter-step / 512 slide / 60°-48° thresholds / 2-tick cadence / 41-42 roll-skip / corpse-only `Pitch` tip (`@0x4b6de4-0x4b6ff4`), and the live lean lands in `bodyPitch(+0x90)` (added, feeds the §14 overlay). Residuals: corpse tumble (`@0x4ba0b2/@0x4b6ccb`) unported; dead-leg `Flags & 0x10A000` exclusion rides D-INF-17; org1 cadence entity-salted vs the global tick (world-wac-ai-re §3.5 item 4 / D-INF-19) | A | FIXED 2026-07-13 (selector + both legs + body_pitch; test_slope_* pins) | PAR-WORLD |
| D-INF-20 | The parachute system (Flags 0x20) unmodeled: auto-deploy (authority, alive, `vel_z ≤ −14336`, aux `+0x2C & 0x10` `@0x4b7aef`), the in-air 47 variants (org2 +0x10 on its straight stamp `@0x4b7e3f`; org1's whole 47→31 ladder is parachute-gated `@0x4bf8d8` — plain NPC falls stamp nothing), the org2 sixteenth-step body chase + leg snap while chuted (`@0x4b494d`), the `@0x4b7b18+` descent block (unread) — ours never sets the flag; the player's jump/fall stamps 31 (world-wac-ai-re §22) | A | OPEN (parachute slice; descent block = NEEDS-RE) | PAR-WORLD |
| D-INF-21 | The "!Poof!" ghost mode deliberately unported: `g_localPlayerPoofMode @0xA82298` (net-toggled `@0x42d450`, debug-chat `!Poof!`) doubles the local player's horizontal root-motion integrate while set (`@0x4b7c8d`); normal play takes the same 1× integrate as org1 (`@0x4b7cbf`), which is what the port implements (world-wac-ai-re §22.3) | D | PERMANENT (dev/admin feature, decision recorded) | — |
| D-ANIM-1 | The `.bad` pose bake stopped at the header `frame_count`, dropping every clip's FINAL channel key — the header counts INTERVALS and dense channels carry `frame_count + 1` keys (fence-post; the original walks every key and holds the last, `BoneAnim_FindKeyframeAtTime @0x410220`). Invisible on loops (the seam key ~= key 0) but a ONE-frame clip (2 keys = its whole motion) collapsed to a static pose: the REVVY/JOTAC M4 `m4_1f` fire kick froze the FP viewmodel through every volley (kick once, hold until release plays idle). Fixed: pose table bakes keys `0..frame_count` inclusive; one-shot eval holds the true final key (loop indexing unchanged); translated clips hold the LAST translation row for the final key (the block carries exactly `frame_count` rows; the original's final-window translation read is unwalked) (ADR 0007 §3; net-re §5.62 frozen-viewmodel grill) | A | FIXED 2026-07-12 | PAR-WORLD |
| D-WPN-1 | weapon.def FUNCTION rows resolve through the `g_actionFuncDefTable @0x829E58` name registry (18 entries incl. the `*_map` scope variants + `powerup_*`); the port fixes each state's behavior instead — safe because every shipped row (JOX + REVX sweeps) names `wpn_std_<its own suffix>` (net-re §5.62) | A | WITNESSED-READY-DEFERRED (registry table witnessed; port when a non-std consumer appears) | PAR-WORLD |
| D-WPN-2 | Single-pool ammo model: the original tracks per-ammo-class carried pools (`Entity_GetScoreValueBySlotType @0x5406e0` class byte def+0xD8, units/round def+0xE0, pool caps `ammoclass_max_carry`); the FSM folds them to one rounds counter and the recoil auto-reload gate approximates units=1 (net-re §5.62) | A | OPEN (rides the ammo-class/pool port, with §5.57/§5.58 pool semantics) | PAR-WORLD |
| D-WPN-3 | `WeaponSlot_CanFire @0x541ba0` legs beyond the clip: busy weapon-child entity, the underwater-fire ban vs `Env_WaterHeightFixed`, the adm+224 score-lock; plus the kick bump's fire-sound-id gate (Def+0x294) — all need slot/env/sound state the port does not model yet (net-re §5.62) | A | OPEN (partial — the clip + reserve-routing leg ported) | PAR-WORLD |
| D-WPN-4 | The heat model: the def+876/880 window stamp into slot+0x14, `WeaponSlot_CalcAccumulatedHeat @0x53f780` (internals unwitnessed), the pump's overheat deny (heat>0xFFFF → queued FIRE becomes EMPTY `@0x541046`), and the OVERHEATED(11) entry writer (unfound) (net-re §5.62) | A | NEEDS-RE (CalcAccumulatedHeat + the state-11 writer), then port | PAR-WORLD |
| D-WPN-5 | Weapon-switch machinery — NOW FULLY WITNESSED (net-re §5.62 switch-chain block): `Player_SwitchToWeaponByHandle @0x4e0170` category scan over `weaponSlotArrayBase @0xB75FD4` → `Player_MountWeaponSlot @0x4dfa40` writes `g_pendingWeaponSlot` + queues SWITCHRANK(8)/`ForceQueueSwitchFrom`(7); the switchfrom swap + `TryQueueSwitchTo`, the −901 instant paths (Flags&0x80), the recoil def+0x168 auto-switch, mount-scoped auto-engage — the FSM ports the timing shapes; the multi-slot pool + swap port rides the loadout slice | A | WITNESSED-READY-DEFERRED (rides the priority-3 loadout/equipped-weapon slice) | PAR-WORLD |
| D-WPN-6 | The pump covers the LOCAL player's equipped slot only; the original pumps every pool-0 equipped slot + pool-1 unmounted weapons with a live muzzle flash (`WeaponAction_ProcessAllEntities @0x542690`) (net-re §5.62) | A | OPEN (extend with NPC/remote weapon state) | PAR-WORLD |
| D-WPN-7 | Interim ammo seed: the FSM installs with clip=clipsize + reserve=startrounds from the def; the original resolves ammo through the PLAYER_INFO loadout + S2C 0x5A apply (§5.30/§5.57) (net-re §5.62) | A | OPEN (rides D-PLAYERINFO-1/-11) | PAR-WORLD |
| D-WPN-8 | FSM↔net residual: authority/listen-host and single-player fire now append the round ring with the witnessed pre-consume-magazine mode byte `((clip & 3) << 4) \| 2`, the exact ordinary on-foot hip/ADS-raise/third-person subtype 12, and spawn `world::RoundSim` synchronously; settled-FP and first-person-mounted integer zoom subtypes remain unmodeled, a joiner's fired event still does not emit C2S 0x06, joiner reload does not send C2S 0x25 / await S2C 0x49, and clients do not feed decoded S2C tag-2 round events into a presentation `RoundSim`, so co-op impact visuals remain host-only (net-re §5.62, §5.16, §5.58) | A | OPEN (zoom-level subtype + joiner uplink + client round presentation ride the npruntime in-match integration) | PAR-WORLD |
| D-WPN-9 | ADS residuals: zoom-level adjust keys (`Player_AdjustWeaponZoomLevel @0x4dbcc0`), the scope-state C2S 0x1D notify, stance (parentSlot 2/5) + NVG gates, the mid-ease movement reversal + auto-re-raise legs (`@0x4df548`/`@0x4df5ae`/`@0x4df607` — need the engaged/active/hipfire tri-state), the HandGunUp (0x4000000) auto-follow leg (`@0x4de444`, player-flag writer unwalked) (net-re §5.62, §5.41) | A | OPEN (landed: toggle/tpos-ease/FOV/rescope, the SIGHTS card, the 7-step Inset interp, unscope-on-move + the scope-up move refusal + the ForceScoped toggle pin — the weapon round) | PAR-WORLD |
| D-WPN-10 | Host clip-key lookup was case-SENSITIVE (`NovaSkeletalAnim::find_clip` exact ==) where the original resolves anim names with stricmp (`AnimMap_FindSlotByName @0x40cfa0`, name+5 `anim_` skip): JOTAC-era weapon.def rows author `ANIM_WPN_*` uppercase vs the .adm's lowercase clip keys, so every `auto` delaystart/delayend collapsed to 0 and no weapon-action clip played on JOTAC/RevX02 mounts (JOX's lowercase rows masked it) (net-re §5.62) | A | FIXED 2026-07-10 (`nocasecmp_to`) | PAR-WORLD |
| D-WPN-11 | The FSM's held-ready phase (0x40) reused the normal begin leg and emitted `action_started`, replaying begin sound/effects; retail's `ActionSlot_BeginActivePhase` plays sound/ctrlreg only on phase 1 (`@0x53f873`) while the held branch replays the animation only (`@0x53f88b`) (net-re §5.62) | A | FIXED 2026-07-11 (held replay no longer emits the begin event) | PAR-WORLD |
| D-WPN-12 | Weapon presentation events crossed the sim/host seam as one latest-value snapshot plus serials; a `MissionRuntime` catch-up could run several fixed ticks before one present pass, overwriting distinct clips/begin/end sounds, collapsing the serial jump to one host edge, and applying the final view state to every event. The single host consumer now drains an ordered per-tick event batch with `age_ticks`, production position, and production-tick scope/third-person/vehicle routing state; snapshot serials remain diagnostic/rebuild state (net-re §5.62) | A | FIXED 2026-07-11 (ordered destructive event drain; routing-state snapshot completed 2026-07-13) | PAR-WORLD |
| D-WPN-13 | Held auto fire ran as a per-tick `request_fire` re-request where the original sustains the volley through the recoil window's deferred re-queue of binding 149 (`Input_QueueDeferredEvent @0x542e9d`, gated `counter<=1 && local && ROUNDS && Flags&0x100 && burst<=0 && Input_IsBindingActive(149)` @0x542e7f; the mid-FIRE press re-queue @0x53effd): at an empty magazine the ungated re-request overwrote the recoil arbiter's queued RELOAD every tick — a held trigger never auto-reloaded and the FSM thrashed FIRE↔RECOIL at 31 Hz strobing the recoil row's soundset/particle (the REVVY M4 wedge) (net-re §5.62) | A | FIXED 2026-07-12 (the `refire_queued` slot latch — the deferred-event queue ported) | PAR-WORLD |
| D-WPN-14 | **FALSE READING RESOLVED 2026-07-14.** `Weapon_RaycastAndSpawnImpact @0x4e8460` is not the ballistic-impact path: `RoundData_SpawnRound` calls it only inside `AmmoDef.flags & 0x400` (instantkillzone) and only for `kztype == Knife`; its `AmmoDef+0x38` extent is `kz_maxradius`, not a weapon range. Ordinary bullets emit their ammo effect/sound row from the physical `Projectile_Handle*Impact -> Projectile_SpawnImpactEffect @0x4e9b80` path, so `world::RoundSim` resolving presentation at simulated collision time is correct. (net-re §5.60) | A | FIXED (disproven premise removed; arrival-time behavior retained) | fix-ptl round (2026-07-14) |
| D-WPN-15 | Impact-row tag selection legs unported: terrain hits always take the original's own no-surface-map default (type 1 → tag 5 dirt; the charmap sampler `Terrain_GetSurfaceTypeAtPosition @0x606510` + the `.TIL` override remap are unrouted), entity hits are pool-0 organics only (always tag 2 player; vehicle/static material+4 and the building material-1→23 leg ride the §5.60 hit-test port), and the water plane is absent from the round sim (tag 11 unreachable) (net-re §5.60) | A | OPEN (charmap sampler + water plane + entity material plumb) | fix-ptl round (2026-07-13) |
| D-WPN-16 | Knife/instant-kill-zone simulation is unported: `RoundData_SpawnRound` gates the immediate damage/raycast branch on `AmmoDef.flags & 0x400`, then `kztype == Knife` calls `Weapon_RaycastAndSpawnImpact @0x4e8460` with ray extent `AmmoDef.kz_maxradius`; `RoundSim` intentionally rejects Knife/Medic kztypes at its ballistic boundary. | A | OPEN (port the non-ballistic kill-zone family separately; do not fold it into bullet flight) | PAR-WORLD |
| D-WPN-17 | The local muzzle flash spawned `BINDING_WORLD` at its spawn-time userpoint where retail re-anchors the live action-effect emitter (`MountSlot+0x18 actionEffectHandle` + anchor action `+0x28`) to the action's bone EVERY pump tick (`WeaponAction_ProcessFrame` tracker leg `@0x540edf` → `CEffectEmitter_UpdatePositionAndParams @0x5f6810`), releasing it underwater — a moving/turning shooter's flash trailed the muzzle (net-re §5.62) | A | FIXED 2026-07-15 (owner-bound spawn + the GameWorld live-anchor resolver polled by the owner-pose sync; underwater release rides the §8 water plumb) | fix-ptl round (2026-07-15) |
| D-WPN-18 | The LOCAL fire leg fed `RoundSim.dir_yaw` the `(90 − heading)` mission-yaw flip where the round bearing frame IS the engine heading frame (the wire-validated 0x06 `(cos, sin)` mapping; the same flip D-NET-153 records for the wire leg) — every local shot flew mirrored across the NE diagonal and impact effects landed 90° off the aim ray (`fp_impact_probe`: due-north aim, impacts 14.75 m due east) (net-re §5.62) | A | FIXED 2026-07-15 (`dir_yaw = heading`; the sim fire ctest target moved onto the true bearing + drained-position pin) | fix-ptl round (2026-07-15) |
| D-WPN-19 | **FALSE READING RESOLVED 2026-07-16.** The slot+0x18 callback is installed on the GROUP (`CEffectGroup_SetDeathCallback @0x5e1940`, group+0x5C/+0x60) and invoked only by `CEffectGroup_Destroy @0x5e3460`; `CEffectGroup_AdvanceChildrenAndReap @0x5e59a0` destroys dead children individually without clearing the action slot. The former `CEffectEmitter_OnChildDied`/first-child inference followed the wrong object chain, and the port's early `SuppressWhileOwned` release was a real divergence (net-re §5.62) | A | FIXED 2026-07-16 (`child_reaping_and_group_suppression_lifetime_contract`: retain suppression through final-child/group death and recycle each finished child immediately) | PR #237 adversarial review |
| D-EVT-1 | Spawn-point activation on fire: fully witnessed (POI/deploy list `0xB76570`, marker @0x452ce0, +0x210/+0x217/+0x218 authoring) — rides the deploy/POI subsystem port | A | WITNESSED-READY-DEFERRED | PAR-WORLD |
| D-EVT-3 | Residual after the TriggerRelations port (state + evaluators + recounts + alert stamps + damage-site SHOT writes landed): the acquisition/fire-time write quads ride the combat pass, motor visited marks ride D-INF-2, sub 11 needs the held-object link; cat-2 alert/count + 42-45 subs unwitnessed | A | WITNESSED-READY-DEFERRED (write-sites) + NEEDS-RE (cat-2 subs) | PAR-WORLD |
| D-AI-1 | The candidate FEED is ported (2026-07-16): `acquire_target` scans registry pools 0/1 with the witnessed gates (team/0x200 see-all incl. the teamless head gate, flags 2/0x8000000, health, `+530` refcount saturation via `Entity::ai_target_refcount`, LOS-last) into the byte-exact scoring core (`acquire_target_from`). Residual deviations: the profile weapon-slot CLASS table (`+40+4i`, ai.def — pools 0/1 scan unconditionally instead), the priority target (`profile+148`), the building 0x100 sub-filter, and the per-candidate range-cap words (`+422/+420`, def fields) are unmodeled (`AI_FindBestTargetB @ 0x466f60`, world-wac-ai-re §16.2) | A | OPEN (feed live 2026-07-16; residuals = the class table / priority / range caps, riding the ai.def parse) | playability P1 |
| D-AI-2 | The state-17/18 SM rows are ported (2026-07-16): enters (`AI_EnterState_GroundCombat @ 0x467650` incl. the alert-red + ally wake, `AI_EnterState_GroundEvade @ 0x467400` incl. the organic/tracked/wheeled routing + the enter tail-call) and the state-17 tick structure (`AIEntity_ProcessWeaponFire @ 0x472e00` §17.6: packed cooldown pair, processed-tick accum + `brain[42]` retarget cadence, search sweep + engage, chase + give-up>620, burst window). The FIRE legs (stationary aligned flag, transform solve, scatter, continuation volleys) ride the unwitnessed turret solver `Entity_ComputeWeaponFireTransform_0 @ 0x455b30` — a visible `unported_calls` stub, so SM vehicles/emplacements still spawn no rounds (riflemen fire via the infantry pass) | A | OPEN (structure ported 2026-07-16; the turret solver = the open witness) | playability P1 |
| D-AI-3 | CLOSED 2026-07-16: the engagement relation ops APPLY — `apply_engage_relations` writes the sees+targeted quads into `world.relations` with the witnessed key orientation (group = `Entity::group_id` +0x11C, single = `net_id` +0x7C; GS transposed), at both the SM engage (`@ 0x4677b3..0x4678b2`) and the infantry scan-hit (`@ 0x4b0a6f..0x4b0ae2`) sites; `ai_set_target` ports `Entity_SetAITarget @ 0x45d760` incl. the target `+530` refcount. The `rel_ops` vector remains as a test-observability trace | A | FIXED 2026-07-16 (applied; matrix identity pinned by the ai ctest) | playability P1 |
| D-AI-4 | The infantry combat pass is ported (2026-07-16, `AiSystem::infantry_combat_think`/`infantry_fire_pass`): 32-tick staged perception (calm half-range, 4-phase schedule, lastAttacker fallback, slot[3]+aimPoint+damageTimer commit, priority-mark decay), the attack-anim reactions (155–158/165/166 by distance/health/hit + post_attack 151), approach/hold move modes, reload (anim 65 + magazine), lead + sawtooth aim error, the walking-fire latch, and the `.bad` anim-event fire (bits 0x4/0x8/0x10 off `RootMotionFrame.events`) into the ring + RoundSim. Exit test: the ai ctest's NPC-kills-player block. Residuals: nearest-first scan penalties (corpse/drowning/far ×2), fresh-corpse (≤16-tick) targets, forced-target words, cover-seeking (`ai_find_cover_position`), retreat/board modes, and the §4-item-13 idle look-at stay unported | A | OPEN (the threat loop is live 2026-07-16 — the exit ctest passes; behavior residuals tracked) | playability P1 |
| D-AI-5 | The anim-fire weapon bytes' load-time writer is unwitnessed: `entity+0x358..0x35B` / bones `+0x365..0x367` carry the items.def `ammo_closeattack/easyrocket/advancedrocket/marker3` + `launchups_*` ids (def source witnessed, `ItemDef_ParseProperty @ 0x4a1823 -> def+0x56B..+0x5FB`; JO riflemen author all four = the rifle round) but no per-field instruction writes the entity copies — a struct block-copy (world-wac-ai-re §17.7 item 1). The port seeds ONE ammo id + `clipsize` per NPC (`AiProfile::ammo_primary/clip_size`): the host seed is WIRED 2026-07-16 — `NovaSimulation::resolve_ai_weapons` (after `load_ammo_table`) resolves each AI entity's items.def `ammo_closeattack` name (`def+0x56B`) against the mission ammo table and stamps the profile + the spawn magazine (`clipsize` `@ 0x49fa1c -> def+0x894`; word `entity+0x35C` reseed `[orig: Entity_ResetToSpawnState @ 0x4b97a9]`). Residual = the single-ammo stand-in itself (four weapon bytes + `launchups_*` bones collapse to one id) until the block-copy is witnessed | A | OPEN (host seed wired 2026-07-16; residual = witness the copy site, model the four-slot family) | playability P1 |
| D-AI-6 | Fire origin + concealment stand-ins — the ROUND/EFFECT muzzle is LANDED 2026-07-16 (session 7, world-wac-ai-re §21): the host muzzle seam feeds the posed gun-flash userpoint (`Entity_GetAttachmentWorldPosition @ 0x4b2670`) back to `AiSystem::infantry_fire_pass` (NovaObjectModel resolve -> present-pass push keyed by SSN -> `set_entity_muzzle`; `ai` ctest + ai_muzzle_probe CP01 PASS, +0.51 u up / 0.93 u out). Residuals: the LOS endpoints + aim eye still `entity pos + 0.9 u` chest lift instead of the bone transform (`Entity_GetAttachmentWorldPosition` on bones `+0x365..0x367` / the def muzzle bone `+1350` via `Entity_ComputeWeaponFireOrigin @ 0x43b4b0` — libs/world has no skeletal pose; a host bone seam would close it), and the prone-in-foliage `+40` accuracy penalty is skipped (needs a foliage-mask seam; `Foliage_SampleFoliageMapMask @ 0x606620`). The aim-error difficulty global (`wac_var_accuracyspread @0xC6EAE8`) is modeled as `AiSystem::ai_difficulty` default 1 — its config source IS WITNESSED 2026-07-16 (session 6): it is the WAC named variable `accuracyspread` (the named-value table @0x82EEF0, world-wac-ai-re §20.3 — mission scripts can set it; the earlier `autogain` attribution was a phase-shifted table read); wiring `ai_difficulty` to the WAC var is the remaining tail | A | OPEN (bone + foliage seams; wire ai_difficulty to the WAC accuracyspread var) | playability P1 |
| D-AI-7 | The LOS raycast is ported (2026-07-16 session 4): `Physics_RaycastTerrainAndSectors @ 0x539910` witnessed in full (world-wac-ai-re §18.5) and `line_of_sight_clear` now rides `CollisionWorld::raycast_clear` — the terrain leg via the ported heightmap raycast (`terrain_raycast_refined`, the `@ 0x60e710` sibling of the witnessed `@ 0x60c760`; boolean-equivalent with a null out-hit) with the both-INDOORS skip, plus the sector leg (pool-2 statics then dynamics, exclusions incl. the +0x28 owner link, bound-sphere broad phase, TYPE-1 convex clip via `collision_raycast_model`). Endpoints keep the D-AI-6 0.9 u chest lift. NPCs no longer see through buildings (`collision` ctest `test_raycast_clear_los`). Residuals: the `Flags & 4` destroyed-HUSK collision-model swap (no husk instances yet), the itemDef type-3 person sphere-block w/ same-team 3.0 u exemption (person-kind residents of the walked pools don't exist in our world — organics are pool 0, unwalked, like retail), the ray-radius arg (LOS passes 0), and the `@ 0x60c760` sibling's internal delta (terrain-re open item) | A | OPEN (the collision leg is LIVE 2026-07-16 — NPCs no longer see through buildings; residuals = the husk swap, the type-3 person case, the 0x60c760 sibling delta) | playability P1 |
| D-AI-8 | Fire-presentation stand-ins (world-wac-ai-re §18, ported 2026-07-16 session 4 as `RoundSim::fired` + `NovaSimulation::drain_fire_presentation_events`/`get_tracer_rounds` + `fire_present_pass.gd`): (a) the tracer cadence counter rides the SHOOTER ENTITY (`Entity::tracer_shot_counter`) instead of the per-WEAPON-SLOT byte `weaponSlot+0x80` — identical for one-weapon NPCs, survives a player weapon swap; (b) the fire-sound max-range gate runs at PLAY time (the bank's cull) vs FIRE time (`soundDef+72` @ 0x528ec6) — differs only when the listener moves during the propagation delay; (c) tracer visuals are an additive vertex-colored streak (family colors red 1/6/9/11, green 2/7/10/12, ordnance 3/4/5) — the retail tracer-pool emitter styles (`CEffectEmitterPool_AllocSlot` over `dword_2BF5270`, init `sub_5DB130`), the +0x10/+0x14 friendly/enemy round GRAPHIC item models (`frndlyTrcrID`/`type_id` parse), and the per-round glow light (round+0x1B4) are unported; (d) the MF_Light muzzle glow (`Entity_UpdateMuzzleGlowEffect @ 0x56c960`, light pool) is parsed (+36/+40) but not presented — no light-pool port; the +40 value's consumer is unwitnessed; (e) the MP NoTracers rules bit (`dword_24D1E34 & 1`) is a net seam, not yet wired | A | OPEN (streak/emitter styles, round graphics, glow lights, NoTracers wire) | playability P1 |
| D-AI-9 | Death-presentation stand-ins (world-wac-ai-re §19, ported 2026-07-16 session 5 — the kill's `RoundSim` anim selection, the `tick_infantry` death edge + corpse block, rows 21/23, `deathtime`/`LeaveCorpse` def traits, `mission_present_pass` corpse visibility): (a) the bullet selection's BONE is the hardcoded torso (1) — the body-cylinder hit model has no bone zones (the explosive path hardcodes 1 for persons too @ 0x4e6ac7); the hit-record bone (`sub_4E7000()[14]`) rides the round_sim bone-collision deferral; (b) the death SCREAM (def sound slots 7/8 via `Entity_GetWeaponSlotTableValue @ 0x528300`, `Bms_AttribFlags & 0x100000` select) rides the unparsed `sound_profile` chain; (c) despawn maps `Entity_Destroy @ 0x43e810` to `Entity::hidden` (our registry keeps the slot) and the SP watch-check gate maps `!is_in_session` to "a local player exists", with the D-AI-6 chest-lift LOS endpoints standing in for the entity-origin ray; (d) unported edge/corpse legs: the +0x134-bit0 silent-cleanup variant, the incendiary ammo+72 → 173 override, `Entity_ApplyCollisionForce` knockback, the bodyRoll nudge, DISMEMBERMENT (`Entity_CloneFromTemplateByType`; JO NPCs author `nodismember`), the 186-tick `particledeath` decay effect, the +0x35E NPC-respawn path, the medic-drag follow (anim 139), drowning 175 (swim flags unmodeled); (e) rows 21/23 carry visible stubs for `Entity_ProcessFallingDeathPhysics @ 0x461d30`, the def+1352 child-kill loop, the attrib-0x40 wreck-respawn watcher, and the whole `Entity_UpdateDeathTransforms @ 0x494660` presentation (husk swap Flags\|=6 / `Entity_SpawnDeathPieces` / death sounds) — dead NON-organics still hide in the present pass as the husk-swap stand-in | A | OPEN (corpses live 2026-07-16 — kills leave directional-death bodies that persist ~31 s under the watch rule; residuals above) | playability P1 |
| D-AI-10 | The round-outcome loop is ported (world-wac-ai-re §20, 2026-07-16 session 6: `World::process_round_end` [orig: `Server_ProcessRoundEnd @ 0x5164f0`], the WAC win/lose handlers + outcome builtins (bluekills/greenkills/humans/GameOver/WinVar/LoseVar from the named-value table @0x82EEF0), the BMS Blue/Red/GreenWin call-through, the SP death auto-lose [orig: `Server_CheckWinConditions @ 0x51ad40` SP leg], the kill tallies [orig: `Score_TallyKillByLocalPlayer @ 0x4fd160`/`Score_TallyKillByOthers @ 0x4fd300`], the zone-ref + player-SSN script resolutions, and the SP end presentation — 04TR probe PASS end-to-end). Stand-ins: tallies are COUNTS only (no def+404 points/difficulty/per-type split/human bucket); the end screens are a shell overlay (no flyaway cine/.cne, no count-up lines, no saved-game list, no end-music switch [orig: `MusicCtx_SelectEndTrack @ 0x672fd0`], 3 s fade + ESC/300 s stand in for the cine fades + key/18600-tick exits); the MP legs are cited stubs (S2C 0x61/0x1D, slot 6->7, `SetGameState(11)`, the 2790 linger, round-win counters, scoreboard block, MP win conditions); the SP gate reads `world.mp_session` (our listen server always runs `ctx.is_in_session=1`) | A | OPEN (presentation + MP depth; each stub cited inline) | playability P2 |

Closed 2026-07-05: **D-INF-4** → `FIXED` (the direction-table generator witnessed —
`[orig: Math_BuildSinTable @ 0x613050]`, an accumulating 1281-entry sin table at 2^22
with the cos read aliasing +256 entries; ported structurally in `quantized_dir`,
integer-equivalence + landmarks pinned in the `infantry` ctest). Also closed:
**D-ITEMDEF-1** → `FIXED` (faec4b3e — `item_type_from_string`
witnessed mapping `[orig: ItemDef_ParseProperty @ 0x49eb00]`;
[world/itemdef-re.md](world/itemdef-re.md) verdict flipped to MATCHING). The first
ledger row driven to zero. Same day, the D-EVT grill closed three more:
**D-EVT-2** → `FIXED` (the quarter-pass piggyback IS the player-AWOL counter
`[orig: @0x454d50 → Entity_UpdateStuckCounter @0x439dc0]`, ported with the
PlayerAwol evaluator), **D-EVT-3 cats 5/6** → `FIXED` (load-parity toggle
`[orig: dword_815174]`; Teammate category `[orig: @0x453b3c..0x453b67]`), and
**D-EVT-4** → `FIXED` (pre/post passes are one-shot per transition, never
periodic `[orig: @0x525b86; @0x52266c/@0x5263a0]` — our per-phase-tick post
evaluation was itself the divergence, replaced by `run_post_mission_pass`).
**D-EVT-5** minted and closed at birth: the BMS second chunk (header +0x246)
is runtime-opaque — both retail paths `fseek` past it (@0x40f6da/@0x40f756,
its only xrefs); our reader's parse-and-round-trip is a faithful superset
whose grammar is editor-side surface gated on D-MIS-3.

### UI — menus/controls, sound, player-info, HUD

Closed 2026-07-05: **D-SND-2** -> `FIXED` (expansion bank slots 0/1 load ahead of
the static banks in slot order `[orig: Expansion_LoadAssets @ 0x4a4989/@ 0x4a495e]`,
fed by `NovaResourceRoot.get_expansion()` off the runtime mount; missing files skip
like `SoundBank_LoadIfExists`). **D-CTRL-2** -> `FIXED` — the witnessed per-entry show-flag
gate ported with every catalog row's flag word minted from the binary
(`[orig: UI_PopulateControlMappingList @ 0x55c0c0; catalog flags @ 0x8159AC
+ 108*id]`; the class-category approximation deleted; observable corrections
pinned in `controls_test`). Also closed: **D-PLAYERINFO-2** -> `FIXED` — verified already enforced:
the parser errors at the 512-part cap (`libs/avatars/src/avatars.cpp` guard,
`[orig: CAvatarDefs_ParseConfigLine @ 0x57a456]`), `NovaAvatarDatabase`
propagates the failure, and `tests/avatars/avatars_parse_test.cpp` pins the
512-part parse failure. The row predated the guard's landing (AVA train).

Sources: [mnu/menu-re.md](mnu/menu-re.md), [audio/lwf-dbf-sound-re.md](audio/lwf-dbf-sound-re.md),
[playerinfo/avatars-re.md](playerinfo/avatars-re.md), [interface/hud-re.md](interface/hud-re.md).

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-MNU-5 | Text-item rendering scope: combo/list image/color items not backed (shipped menus are text-only there) | A | OPEN | PAR-UI |
| D-MNU-6 | CBIN credits custom `~F` fonts / `~I` images not resolved from the resource root (default font only) | A | OPEN | PAR-UI (see credits audit PAR-R5) |
| D-CTRL-1 | Mouse/joystick binding arrays (profile-built at runtime) not ported; those rows show a blank Control column. **Scoping (2026-07-05):** NOT in `PlayerProfile_InitDefaults @ 0x54bb40` (that sets settings/macros/default weapon loadouts only) — the mouse/joystick default bindings are built by a separate input-binding init (an RE hunt), and the consumer is the Godot input-action layer (same gate as D-CTRL-3) | A | OPEN | PAR-UI |
| D-CTRL-3 | Live double-click rebinding / DEFAULTS / CLEAR_KEY / profile persistence deferred — gated on a real input-action layer | A | WITNESSED-READY-DEFERRED | PAR-UI |
| D-PLAYERINFO-1 | In-world (spawned-player) combo→3D-model binding untraced (the preview is witnessed + fully ported) | B | NEEDS-RE | PAR-UI / research starter |
| D-PLAYERINFO-7 | `PLAYER_INFO` screen orchestration (init + 28-control registration + nat→div→combo cascade) host wiring is the next phase | A | WITNESSED-READY-DEFERRED | PAR-UI |
| D-PLAYERINFO-9 | ACCEPT/commit + profile persistence host wiring is the next phase | A | WITNESSED-READY-DEFERRED | PAR-UI |
| D-PLAYERINFO-10 | Voice preview (`VOICE_%d` via `menu.lwf`) host wiring is the next phase | A | WITNESSED-READY-DEFERRED | PAR-UI |
| D-PLAYERINFO-11 | Loadout ammo combos + weight readout remaining (weapon lists implemented; the weight-calc CORE now ported to `libs/def` `def_loadout_weight`/`def_encumbrance_class` `[orig: @ 0x55f1f0; @ 0x55f480]`, unit-tested — the residual is the ammo combos + the UI host wiring, which need the Godot runtime) | A | OPEN (partial) | PAR-UI |
| D-PLAYERINFO-12 | Per-(slot, team) selection-state globals host wiring is the next phase | A | WITNESSED-READY-DEFERRED | PAR-UI |
| D-HUD-1 | Stance indicator = discrete cross-faded `HUDSTANCE` frames (IDB was `draw_minimap_compass_overlay` + oscarmike model it as a compass) — the cross-fade ported 2026-07-09 (`hud_stance.gd`/`hud_fade.gd`); the IDB rename to `HUD_DrawStanceIndicator` applied 2026-07-16 | A | FIXED (2026-07-09 port; IDB rename applied 2026-07-16) | PAR-UI |
| D-HUD-2 | Stance widget = frame-swap + fade; heading/north is a *separate* top-down radar (do not port a rotating ring) | A | NEEDS-RE (the radar; the stance leg is fixed by the 2026-07-09 port) | PAR-UI / research starter |
| D-HUD-3 | HUD design space is fixed 1024×768, scaled round-to-nearest (`Viewport_ScaleToVirtualCoords`) | A | FIXED (`hud_layout.gd`) | PAR-UI |
| D-HUD-4 | Health-bar fill WIDTH uses the capped `+92` ratio; fill COLOR uses an uncapped recomputed ratio — the port matches both reads | A | FIXED | PAR-UI |
| D-HUD-5 | Clip-indicator flash restamp keys on (`round_type`, reserve) — the original keys (ammo class `def+220`, reserve, pool id `def+216`); same transitions under the single-pool weapon model (D-WPN-2) | A | OPEN (revisit with per-class pools) | PAR-UI |
| D-HUD-6 | Mission triggered text ported as a timed message-line feed (930-tick life, ≥186 stagger) at the `HUDCHATTEXT` anchor — the original rides the full chat pipeline (channel ring buffers + a geometry table whose writer is unwitnessed) | A | OPEN (chat-pipeline follow-up) | PAR-UI / research starter |
| D-HUD-7 | Crosshair spread omits the recoil accumulators (`player+0x380/+0x384 >> 7`) — the runtime does not surface them yet; ERROR-row term ported exactly | A | OPEN (needs the recoil write-side witness) | PAR-UI / research starter |
| D-HUD-8 | Crosshair color modulates the texture — the original writes it to the strip's specular channel (blend stage in the unwitnessed HUD shader pass); identical for the default white | B | OPEN (witness the texture-stage state) | PAR-UI |
| D-HUD-9 | Crosshair hides the instant the scope engages — the original draws while an aimed shot is unavailable (`!Player_CanFireWeapon @ 0x5cf780`, which needs the SETTLED sight view), i.e. through the whole ADS ease; the row select stays hip (`+3` keys on CanFire, not the scope toggle, `@ 0x592b87`) | A | FIXED 2026-07-11 (weapon round: the hide is `scope_engaged && scope_fraction >= 1`; `NovaGameHudHost` plumbs `scope_fraction`) | PAR-UI |
| D-HUD-10 | Crosshair anchors at the fixed design center — the original anchors at the projected aim point (screen center only on-foot first-person `@ 0x5928a0`; spectate/`g_camera_mode` project `Entity_BuildCameraView` `@ 0x592910`) | A | FIXED 2026-07-11 (weapon round: `aim_screen_point()` = INF in 1P -> the HUD pins the exact center; the 3P projection uses the witnessed 1000.0 far point) | PAR-UI |

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
host-sampled `extra_bheight` (the mission workspace passes terrain heights
in write order). **D-MIS-5** -> `FIXED` — reader/writer asymmetries
corrupted round-trips (base-0 `strtol` parsed zero-padded numerics as
OCTAL vs the witnessed base-10 `atol`; `fog_level`/`water_level` u32-out
u16-truncate-in; `gen_def_val1..4` write-only; parse defaults rewrote
zero-valued fields on all 1365 retail-00TRg entities); fixed to base-10 +
symmetric fields + unconditional emission of defaulted keys —
`.bms`→`.mis`→`.mis` is byte-idempotent on the retail fixture. Full
witness: [mission/mis-format-re.md](mission/mis-format-re.md).

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-MIS-1 | All `begin item` records land in the generic pool; the pool-kind classifier needs original-editor confirmation | B | NEEDS-RE (narrowed 2026-07-07: the Nile importer classifies by the `items.def` TYPE STRING `[orig: MisLdr_WriteNileProjectXml @ 0x10004930, misldr.dll]`, matching the placer's empirically-verified kind mapping — porting it into the `.mis` reader closes this; `dfx2med.exe` confirmation rides D-MIS-3) | PAR-WORLD |
| D-MIS-2 | `weapon_availability` emitted empty + skipped on read; loadout semantics deferred | A | WITNESSED-READY-DEFERRED | PAR-WORLD |
| D-MIS-3 | Full `dfx2med.exe` `.mis` grammar unmapped (hand-authored / legacy variants beyond the writer subset) | B | NEEDS-RE | PAR-WORLD |
| D-3DILW-1 | v8 branch deferred (v10-only parser; the NovalogicTools v8 layout is unvalidated against the 3 local v8 files) | B | NEEDS-RE | rides an LW-import revival |
| D-3DILW-2 | Textures deferred (geometry + one-weight skinning parsed; material textures not ported) | A | WITNESSED-READY-DEFERRED | rides an LW-import revival |
| D-3DILW-3 | SAF/KSA playback intentionally not applied (`parsed_not_applied_pending_re`; the pose recipe is pinned, end-to-end validation pending) | B | NEEDS-RE | rides an LW-import revival |
| D-PTL-2 | Per-command ArrayMesh surfaces could exceed the 256-surface cap and Godot could reorder equal-priority transparent surfaces, violating the engine-wide packet order under casing/impact churn | A | FIXED 2026-07-14 (one immutable packet + persistent RD buffers + sequential command draws) | fix-ptl round |
| D-PTL-3 | `mod2x` approximated `DESTCOLOR`/`SRCCOLOR` over Godot `blend_mul` | A | FIXED 2026-07-14 (exact RD `SRC=DESTCOLOR, DST=SRCCOLOR` pipeline) | fix-ptl round |
| D-PTL-4 | `bump`/`bumpadd` used the wrong rotation axis and saturated encoded light bytes | A | FIXED 2026-07-14 (exact `transpose(Rx×view)` transform, truncating low-byte packing, DOT3 pipelines) | fix-ptl round |
| D-PTL-5 | `distort` used an arbitrary fixed-strength screen-texture offset | A | FIXED 2026-07-14 (decoded normal/wave/projective equation + one immutable scene-color copy; host UV mapping remains bounded) | fix-ptl round |
| D-PTL-6 | Atlas registrar, allocator, type preprocessing, and inset were approximated by per-emitter shelf packing | A | FIXED 2026-07-14 (portable exact builder and CI contracts) | fix-ptl round |
| D-PTL-7 | Scripted effect initial orientation starts at world-up instead of the retail terrain surface normal; attached fx2ssn groups subsequently follow the live entity basis | A | OPEN (terrain-normal lookup) | PAR-WORLD/runtime |
| D-PTL-13 | Parser hard-failed a whole .ptl on any unrecognized top-level or `=`-less line where retail ignores unclaimed lines — RevX02's `joAmmoHit.ptl` divider comment cost all 18 `Effect_AmHit*` impact effects (invisible stockeffect clones) | A | **FIXED** (skip-and-continue both sites; `particle_lenient_lines` ctest; `[orig: CEffectWorld_ParseSectionCallback @ 0x5ecb40]`) | fix-ptl round (2026-07-13) |
| D-PTL-14 | Flipbook frame naming was guessed, causing missing shipped frames and procedural-fallback strobing. Retail is exact: N=1 keeps the authored name; N>1 lowercases, truncates at the first `.tga`, and appends `_01.tga`…`_09.tga`, then `_10.tga`+; case-insensitive `(name,type)` identity; no literal or trailing-letter fallback (`CParticleDef_ReloadGraphicFrameTextures @0x5e4bb0`, ex kong `CParticleTableDef_ReloadAllTextures`) | A | FIXED 2026-07-14 (shared registrar + portable CI contracts) | fix-ptl round |
| D-PTL-15 | Static-batch buildings/decorations/no-anim vehicles formerly lost ITEMS.DEF `particlefx` because they had no per-entity Node/model handle | A | FIXED 2026-07-14 (placer retains value descriptors; GameWorld applies the original pool/userpoint rules and spawns World-bound persistent groups without creating vehicle Nodes; `fxs`/`fxw*` and death-state families remain separate §8 work) | fix-ptl round |
| D-PTL-16 | Casing and ballistic-impact particles were suppressed to avoid per-emitter Node/material/atlas/upload churn; recoil muzzle rows were temporarily classified by `mflash*`/userpoint and guarded even though retail's direct leg is generic and unsuppressed | A | FIXED 2026-07-14 (all direct/impact payloads are generic `Always` transients in one value scene; shared atlas + ordered persistent-buffer renderer removes the churn without changing FSM timing) | fix-ptl round |
| D-PTL-17 | The PlayerControl occupancy effect runs class-wide in the port (every `attrib & 0x40` item), while retail reaches the spawner only through the `CHel`/`cpln` class updater — shipped ctank/cbike/cveh exhaust authors are dead data in JO retail (`entity_update_damage_accumulator_and_shadow @ 0x48fa70`; class table `@ 0x82ac00`) | A | PERMANENT (intentional, small): presents authored-but-unreachable data on ground vehicles; the +368 claimant protocol itself is witnessed and ported exactly | fix-ptl round (2026-07-15) |
| D-PTL-18 | Retail's atlas skyline placer can lower taller columns and overlap earlier rects, and its uncapped 2.5-px inset inverts tiny-rect UV windows (`CParticleAtlas_TryPlaceEntry @ 0x5e2be0`) | A | PERMANENT (bounded, original-garbage class): the witnessed allocator is kept verbatim but guarded by an occupied-rect check + `max()` column restore + a half-extent inset cap (ADR 0022) | fix-ptl round (2026-07-15) |
| D-PTL-19 | Retail carries authored `flip_frames` into frame registration with no witnessed host-style normalization; the port forces non-positive values to 1 and caps counts at 256 | C | PERMANENT (bounded safety; shipped corpus below cap) | PR #237 adversarial review |
| D-PTL-20 | Retail consumes one trailing curve modifier (`reverse` OR `inverse`); the port consumes and composes both trailing modifiers in either order | C | PERMANENT (bounded mod-syntax superset; shipped corpus uses at most one) | PR #237 adversarial review |
| D-PTL-21 | Retail recursively partitions emitter AABBs, then globally particle-sorts each overlapping leaf; the host globally particle-sorts the entire selected domain | A | OPEN (overlapping-emitter interleaving fixed; exact recursive leaf/tie order remains) | PR #237 adversarial review |
| D-PTL-22 | PTL section tags and known keys were case-sensitive in the port while retail uses `_stricmp` throughout the parser family | A | FIXED 2026-07-16 (fold all known section/effect/particle/graphic/table/edit-handle tokens; preserve unknown spelling) | PR #237 adversarial review |
| D-PTL-23 | Curve-table duplicate selection treated TableDef+0x248 as owner and always selected the first match; retail uses +0x248 as transform mask, chooses first base unmodified and cached/last base modified | A | FIXED 2026-07-16 (native + legacy Godot first-unmodified/last-modified selection; duplicate-selection ctest) | PR #237 adversarial review |

The LW `.3di` record is unlanded overall (PR #45 closed); its rows ride whenever an LW
import is revived. Note D-3DILW-1's v8 branch overlaps the 3DI/GP audit surface only at
the container-detection seam.

### VFS / PFF mount stack — [vfs/vfs-pff-mount-re.md](vfs/vfs-pff-mount-re.md) (D-VFS catalog; PAR-R7)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-VFS-1 | Per-query `VfsLookupPolicy` now preserves the session default while implemented foliage/UI consumers force loose-first and local/network BMS probes + reads force archive-only; VFS + Godot contracts pin packed, `/d`, has/read, and texture-cache isolation | A | FIXED (2026-07-17) | fidelity 2026-07-17 |
| D-VFS-3 | Runtime retail lookups preserve the full relative query (case-insensitive loose subdir walk; no flat-archive basename alias); editor roots retain their deliberate flat authoring contract, pinned at both VFS and `NovaResourceRoot` seams | A | FIXED (2026-07-17) | fidelity 2026-07-17 |
| D-VFS-5 | Encrypted-entry streaming: retail decrypts whole-file reads only; ours always — corpus check needed | B | NEEDS-RE | PAR (vfs) |
| D-VFS-7 | Archive lookup now mirrors the 31-byte query cap + both-sides ASCII uppercase and exact trailing-space significance; the apparent retail trim starts on the terminating NUL and is dead, pinned by `test_archive_names_keep_trailing_spaces` | A | FIXED (2026-07-17) | fidelity 2026-07-17 |
| D-VFS-10 | Host rejects rooted/drive-qualified/ADS/`..` queries and symlink escapes from a mounted loose root, where retail constructs an unchecked path; pinned by `test_retail_query_stays_inside_mounted_root` [orig: FileSystem_OpenFile @ 0x75b1c0 / FileSystem_FileExists @ 0x75aa50] | C | PERMANENT (2026-07-17) | mounted-root safety |

D-VFS-4/6/8/9/10 are ratified permanent decisions (register below). Closed 2026-07-05:
**D-VFS-2** -> `FIXED` — `Vfs::mount_game` defaults to the witnessed fixed boot
table (`VfsArchiveDiscovery::RetailTable`: language/localres/resource.pff in
slot order, extra archives never mount, pinned by
`test_mount_game_retail_table` `[orig: PFF_OpenAllArchives @ 0x4a4310, table
@ 0x829f90]`); the editor's browse index deliberately keeps `ScanAll`
(recorded in the record's D-VFS-2 row — an authoring tool indexes arbitrary
modder archives), and `NovaResourceRoot::mount_runtime` passes `RetailTable`.
Closed 2026-07-17: **D-VFS-1**, **D-VFS-3**, and **D-VFS-7** — per-call source
policy, verbatim relative runtime queries, and exact archive-name comparison are
live and pinned at the VFS/Godot seams; **D-VFS-10** records the ratified safety
boundary around those newly live loose probes.

### Credits (CBIN) — [credits/cbin-re.md](credits/cbin-re.md) (D-CBIN catalog; PAR-R5, PARTIAL)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-CBIN-1 | Credits `~C`/`~F`/`~J`/`<CR>` markup consumers (the retail scroller) not yet witnessed; D-MNU-6 custom-font/image resolution rides here | B | NEEDS-RE | PAR (credits) |
| D-CBIN-2 | Read path CONFIRMED: 8 rol-7 cipher sites in the CBIN codec region (0x75e158-0x75e914) incl. decode loops (`@0x75e473` read+decipher) — retail READS CBIN, not just writes; our symmetric decode matches | B | **RESOLVED (MATCHING)** | PAR (credits) |

CBIN codec (magic 0x4E494243 + 20-B header + ROL32/XOR cipher `@0x75e348`) is
**MATCHING** vs `libs/cbin`, witnessed read-only via raw disasm (no IDB write).

### Terrain — [terrain/terrain-re.md](terrain/terrain-re.md) (D-TERRAIN catalog; PAR-R1, PARTIAL)

Fresh re-grill 2026-07-13 closes the remaining top-tier shader stand-ins;
a stage-3 correction landed 2026-07-15. The host integer-normalizes DBlend,
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
depth. This closes D-TERRAIN-5. The remaining rendering gap is the dynamic
producer beyond its closed bare pass: retail composes ordered tile
models/overlays/depth-alpha into a per-tile render target, whereas the host
applies a static CPU overlay. D-TERRAIN-6 closes base LOD/fog/order;
D-TERRAIN-7 tracks those remaining producer mechanics, D-TERRAIN-8 the
local-light/shadow pass, and D-TERRAIN-9 the editor-only raw input preview.
A 2026-07-14 coordinate-basis audit also minted and closed D-TERRAIN-10: the
host now preserves EnvFile's direct retail getter tuple `g=(g0,g1,g2)` and
reproduces PolyTrn's D3DCOLOR packing as GPU RGB `(g2,g0,g1)`, host `(z,x,y)`.
The former `(x,z,y)` mapping swapped the horizontal DOT3 axes; flat tests could
not expose it, so the correction is pinned by non-flat 08:00 slope vectors.

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-TERRAIN-1 | Terrain-shader edit/runtime split: editor live-sculpt shader vs runtime baked shader, sharing the surface-shading math via an include — a tracked deliberate divergence justified by the editing need | C | PERMANENT (candidate) | PAR (terrain) |
| D-TERRAIN-2 | Shared surface include stacked TWO ×2 detail-normal factors on the splat (gobj-era chimera); the witnessed top-tier ps.1.4 applies exactly ONE `[orig: PolyTrn_PS14SplatNormalMap @ 0x7dece0; compile_terrain_pixel_shaders @ 0x605260]` — post-gamma the squared factor clipped regions to white (full witness in the prose block above) | A | **FIXED (2026-07-06, the model-parity follow-up)** | REN (model-parity) |
| D-TERRAIN-3 | Below-horizon region: cameras see past the sky dome's 1024-unit rim to the raw viewport background — retail fills the below-rim region with the **frame clear alone** (the env #21 skyfog blend; no skirt/ring geometry exists in the frame walk — sky-pass leg witness `Terrain_RenderSkyboxPass @ 0x610ac0` → `Terrain_RenderSectorBatchLit @ 0x60c670`, the seam hidden by fog convergence at the 1024 fog reference). The host's ported clear consumer was silently swallowed by a `BG_SKY`(null-sky) Environment mode rendering BLACK — a 1-px black dome-rim seam behind covering geometry, and once env #29's witnessed strip march landed (the strips stop at their witnessed row/fog-clamp extent like retail's), a PURE-BLACK BAND filling the whole strip-edge-to-rim region in water-horizon views (near half the frame at dusk) | C | **FIXED (REN-7, 2026-07-07)**: two facets — `game_world.tscn` ClearColor → `BG_COLOR` + `AMBIENT_SOURCE_DISABLED`, GUT-pinned (`game_world_test`), so the env #21 consumer renders at all; and `get_frame_clear_color()` now serves the post-blend DOUBLED skyfog (the modulate2x-path Clear consumes it verbatim `[orig: @ 0x677100]`; the 07-05 "undoubled" reasoning was the non-modulate2x fallback, no host analog — the undoubled band measured exactly half the fogged dome rim). Env vectors re-dumped (frame-clear token only). Residual (not a retail-parity surface): the ONED editor preview keeps its own viewport environment — far-env adoption rides ONED polish/ENV-1 | REN-7 |
| D-TERRAIN-4 | The ported terrain raycast's editor-host guards (the `terrain_raycast.h` sampler seam): beyond-extent samples report no-terrain/no-hit where retail CLAMPS the cell to the grid edge — terrain continues forever `[orig: OOB masks @ 0x31a0010/0x319fc0c]`; no terrain data returns clear/NAN where retail returns HIT `[orig: @ 0x60ccf7]`; the bilinear substrate is our contiguous-atlas sampler where retail applies a per-quadrant seam-flag +1-neighbor policy `[orig: Terrain_SeamFlags_* @ 0x31a17f0..]`. Observable in principle only past the authored rim (e.g. the celestial glare ray) | C | PERMANENT (candidate) — the same editor-guard class already ratified for `coords_editor_options` (ADR 0020 seam); the witnessed retail forms are recorded in [terrain/terrain-re.md](terrain/terrain-re.md) §Runtime terrain queries for any future runtime-faithful host | ENG-3 B1b |
| D-TERRAIN-5 | Top ps.1.4 inputs and fog were stand-ins: heightmap normal was misused as t3, raw near/far textures were camera-crossfaded, DBlend was unnormalized, authored-detail coefficient/custom mips and the separate heightfield-DOT3 tile alpha were absent, a final env terrain tint was multiplied, and all fog modes used radial distance | A | **FIXED 2026-07-13** — byte-vector preprocessors, four `.trn` lock pairs, exact bare t0 producer, top-tier shader arithmetic, and type-0 eye depth [`Terrain_GenerateNormalMap @ 0x603210`; `PolyTrn_RenderTile @ 0x60da70`]. **Corrected 2026-07-15**: t3 = the authored second detail pair at density2, not the generated coefficient (prose above); every mipped stage samples anisotropically per the device-global texfilter [`@ 0x67e3b5..0x67e50e`] | terrain/foliage re-grill |
| D-TERRAIN-6 | LOD/fog/overlay base-pass semantics: exact clamped `lod_sub / 2` eight-family selection, type-0 eye-depth vs linear radial fog, and overlay RGB before lighting while retaining the cached heightfield/light DOT3 alpha | A | **FIXED 2026-07-13** | terrain/foliage re-grill |
| D-TERRAIN-7 | Retail's t0 is a dynamically composed per-tile render target. Its bare colormap RGB, TrnNMap quadrant CLAMP, coordinate-basis-correct light packing, DOT3 alpha, and hosted static `.til` composition are closed; allocation/format, draw/dirty cadence, general patch/page c7/c8 projection, ordered tile-model/depth-alpha contributions, and final RT mip behavior remain open | B | OPEN, bounded producer gap | terrain/foliage re-grill |
| D-TERRAIN-8 | Retail local-light/shadow terrain variants and `render_terrain_lightmaps @ 0x609de0` have no host pass | A | OPEN, bounded | terrain/render lighting |
| D-TERRAIN-9 | Runtime binds normalized DBlend and paired retail mip chains; the live editor preview still binds raw DBlend and raw C1/C2/C3 textures (its coefficient fallback is exact) | B | OPEN, editor-preview-only | terrain editor parity |
| D-TERRAIN-10 | EnvFile preserves the direct `Environment_GetLightDirectionFloat` tuple `g`, not Godot/world XYZ. PolyTrn writes D3DCOLOR `BYTE2←g2`, `BYTE1←g0`, `BYTE0←g1`, hence GPU diffuse RGB `(g2,g0,g1)`; normal A8R8G8B8 RGB is `(grid X slope, grid Y slope, up)` and the host `FORMAT_RGBA8` upload does not swap it. The old host `(x,z,y)` mapping swapped horizontal DOT3 axes. Terrain and analytic foliage now pack `(z,x,y)`; the non-flat 08:00 oracle pins light bytes `(231,83,187)` and slope alphas `0.8987774/0.0794002` [`orig: Environment_GetLightDirectionFloat @ 0x57d870; Terrain_GenerateNormalMap pack @ 0x603470..0x6034eb; PolyTrn light pack @ 0x60e201..0x60e331; PolyTrn_TileBakeDot3LightPass @ 0x60e385..0x60e39e`] | A | **FIXED 2026-07-14** | terrain/render lighting |

The build → mesh-simplify → CPT data path remains byte-identical across the
fixture corpus, and all 16 LOD sublevels now pin the recovered eight-family
selector. The record remains PARTIAL because D-TERRAIN-7/-8 are open runtime
rendering gaps and D-TERRAIN-9 is an editor-preview gap; D-TERRAIN-1 is
deliberate, D-TERRAIN-2/-3/-5/-6/-10 are fixed, and D-TERRAIN-4 is the ENG-3
editor-guard candidate.

### Tiles — [tiles/til-re.md](tiles/til-re.md) (D-TIL catalog; PAR-R3)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-TIL-1 | `TIL_FLAG_OUTLINE` (0x08): the LINELIST outline is jodemo-only; retail JO's render (`render_water_quad @ 0x604700`) omits it and so do we (flag preserved for round-trip, no outline drawn) — faithful to retail JO | B | **FIXED (faithful)** | PAR (tiles) |
| D-TIL-2 | `ROTATE_90` was the CW transpose `(v, 1−u)`; retail rotates CCW `(1−v, u)` (corner cycle @ `render_water_quad 0x6047d4..0x604806`) — rotated tiles drew 180° off, scrambling tire-track tile runs (00TRa) | A | **FIXED 2026-07-15** — one shared helper corrected; bake/ONED preview/GDScript binding inherit | terrain/foliage re-grill |

Overlay entry (12 B), atlas UV, flip/rotate flags, half-texel shift, Z negation,
and the 128-LRU cache are **MATCHING** vs retail `PolyTrn_RenderTile @ 0x60df0d`.

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
| D-FOLIAGE-1 | The prior per-corner colored-emitter premise came from the inverted port and is retracted. Detail output diffuse is the source-height bend carrier, while the distant MODEL pass uploads black `(0,0,0,1)` only as the zero-contribution source of an additive depth mask (clarified 2026-07-14) | A | **FIXED 2026-07-13** | fresh foliage re-grill |
| D-FOLIAGE-2 | Detail fragment arithmetic differs from the exact lightmap-blend chain `t0 × (t1 × (t1.a·c1 + c0)) × v0 × 8` | A | **FIXED** for arithmetic/state; composed t1 producer moved to D-FOLIAGE-7 | REN/fresh re-grill |
| D-FOLIAGE-3 | Tier wind was missing/wrong-axis: detail needs source-height-weighted render-Z sway; silhouettes need their separate grid-placement Z term | A | **FIXED 2026-07-13** — dedicated tier shaders | fresh foliage re-grill |
| D-FOLIAGE-4 | The 2026-07-08 runtime inverted the tiers: it treated detail as a ground-patch tier and the distant sector-entity path as a colored model tier. Retail detail expands full terrain-bent source geometry through distance 42; the overlapping ≥38 path is a ground-fitted, color-invisible MODEL depth mask | A | **FIXED 2026-07-13** — old runtime and tests superseded by the fresh portable runtime + adapter | fresh foliage re-grill |
| D-FOLIAGE-5 | Exact model-own `:fd` chain: wrapped `(4C + cardinals + 2×diagonals) >> 4` alpha; authored RGB at mip 0; recursively box-downsampled later mips blended toward `0x808080` by `min(256,floor(320×i/N))`; alpha preserved from the base mip | A | **FIXED 2026-07-14** — portable builder plus native/Godot literal full-chain vectors; no generic mip regeneration | fresh foliage re-grill |
| D-FOLIAGE-6 | The first fresh pass treated uploaded c6 black as final color and missed the downstream SRC=ONE/DEST=ONE combiner plus retained strict-alpha Z write | A | **FIXED 2026-07-14** — dedicated unlit, unfogged, additive MODEL depth-mask pass; GPU color/depth contract pinned | fresh foliage re-grill |
| D-FOLIAGE-7 | Detail t1 is retail's composed per-tile render target. Runtime reconstructs exact bare RGB/heightfield-DOT3 alpha and composes hosted static `.til` RGB/tint at the witnessed pre-wind coordinate; retail's general patch/page c7/c8 projection and ordered tile-model/depth-alpha RT contributions remain absent, while editor foliage preview additionally falls back to mesh normals because it has no parent NovaTerrain atlas | B | OPEN, bounded; shares D-TERRAIN-7 producer | terrain/foliage re-grill |
| D-FOLIAGE-8 | Retail candidate exclusion linearly scans the shared mission .til array with inclusive 16x16 entry AABBs and a radius-2 candidate square unless attrib bit 0 FORCE_ON; GameWorld now shares the parsed resource with terrain and foliage and reuses its bytes for network initial state | B | **FIXED 2026-07-14** - exact portable scan plus host lifecycle wiring | foliage runtime host mapping |
| D-FOLIAGE-9 | Silhouette driver membership: retail walks visible sector entities with `test_sector_entity_occlusion @ 0x5c4610`; the host stands in a camera-frustum test for the surviving stance-gated anchors (class selection itself is closed — D-FOLIAGE-11). Overlapping host anchors retain distinct submissions but coalesce same-frame refreshes of one `(slot, cell key)` | C | OPEN, narrowed to visibility membership (2026-07-16) | foliage runtime host mapping |
| D-FOLIAGE-11 | The host anchored the MODEL/depth-mask tier on EVERY placed mission object; retail's sector walk generates it only around crouched/prone infantry standing on terrain (`MoveOrder & 0x300`, empty `groundEntity` — the hide-in-grass masks) [`orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7dc2/0x5c7ded/0x5c7dd5`]. Non-retail grass masks around objects + model-cache thrash to 3 FPS on the 03TR airfield vista (212 ms foliage in a 354 ms frame; 0.5 ms after). Anchors = the sim's stance query; the ONED preview feeds none and its `anchor_provider` plumbing is removed | A | **FIXED 2026-07-16** | terrain/foliage re-grill |
| D-FOLIAGE-13 | Detail-cell collection was a standalone radial 42u-disc walk; retail hands subtrees to the collector only from frustum-surviving traversal nodes (level ≥ 3) [`orig: Terrain_TraverseQuadtreeNode @ 0x60905c..0x60907c`]. Over-collection exceeded the witnessed far-slot pool capacity and its strict-first-max LRU (all-ties per-update stamps) blinked one grass cell at 2-frame period while standing still (user-reported, 00TRa) — retail's frustum wedge never overflows the pool. Re-seated in the traversal handoff: 34→24 cells, steady-state misses 2/frame→0 at the reported pose | A | **FIXED 2026-07-16** | terrain/foliage re-grill |
| D-FOLIAGE-12 | Two retail gate samplers (detail = flat 1024-wrap `Terrain_GetSurfaceTypeAtFixedPoint @ 0x6066d0`; MODEL = sector-grid-routed `Foliage_SampleFoliageMapMask @ 0x606620`) vs the host's single routed sampler for both tiers — identical on identity-grid maps, divergent on repeat/remapped sector layouts | C | OPEN, host mapping | terrain/foliage re-grill |
| D-FOLIAGE-10 | Retail inserts each immediate MODEL depth-mask draw after the initial sector flush and before later entity/foliage consumers; the host's transparent-pass depth sorting cannot cull already-drawn farther detail under a nearer mask or reproduce every insertion point. The secondary LOW's strict `LESS` is now emulated exactly (high-pass cutoff discard on identical geometry), and both detail passes blend `SRCALPHA/INVSRCALPHA` at the shared fade. The reflection-scene LOW-only `fade × 0.1` pass is unhosted while water reflections carry no foliage | C | OPEN, narrowed to order/reflection host mapping (state half retired 2026-07-15) | foliage runtime host mapping |

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

### Fonts — [fonts/fnt-re.md](fonts/fnt-re.md) (D-FNT catalog; PAR-R4)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-FNT-1 | Offset +4 is the design-width scale reference, not a version; our reader rejected `!= 800` | A | **FIXED** (no equality gate; non-800 parses, `fnt_roundtrip`) | PAR (fonts) |
| D-FNT-2 | The per-font design scale `800/designWidth` was not retained | A | **FIXED** (`design_width` carried; `fnt_design_scale`) | PAR (fonts) |
| D-FNT-3 | Offset +12 (`hdr3`) named `shadow_offset` but only STORED by the loader — the shadow semantics are unconfirmed | B | NEEDS-RE | PAR (fonts) |
| D-FNT-4 | cp1252 specials (0x80–0x9F): `to_font_file` keys glyphs at raw bytes while the display decode maps to Unicode, so the FontFile misses and Godot's default `allow_system_fallback` silently draws a SYSTEM font glyph where retail draws the font's own slot (glyph = byte−32) | A | **OPEN** — pinned as-is (`strings_encoding_test`); fix = key glyphs at decoded codepoints or disable fallback (tracked decision) | STR-2 audit (2026-07-12) |

### Boot-required resources — [required-resources.md](required-resources.md) (D-BOOT catalog; R8/ENG-6)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-BOOT-1 | Menu/game music bank resolution: retail hardcodes `MENUMUS.SBF/.BIN` + `GAMEMUS.SBF/.BIN` (`M<exp>`/`G<exp>` under an expansion); `menu_shell.gd` scanned by name heuristic instead | A | **FIXED (2026-07-07, fidelity backlog)** — the shell resolves the witnessed hardcoded pairs (`[orig: Expansion_LoadAssets @ 0x4a4798/@ 0x4a4906; AudioVM_OpenContextFile @ 0x672160; AudioVM_LoadScriptFile @ 0x672d20; Sbf_OpenFile_Gamemus @ 0x4ed6c0]`): expansion `M<n>`/`G<n>` first then the base pair (a documented host graceful-degrade — retail selects names ONCE and goes silent on a musicless expansion), scripts via VFS bytes + the single-sourced SCR decode (they ship inside PFFs; the old loose-only `ResourceLoader` path could never load them on a PFF install — the actual "no menu music" defect), banks loose-streamed as witnessed; the GAMEMUS pair swaps onto the director at game entry; `menu_shell_test` 16/16 incl. three resolution pins | fidelity backlog (D-BOOT-1) |

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
([render/render-lighting-re.md](render/render-lighting-re.md); reflection/
phong stand-in residuals = D-RLIT-5).

Minted-and-closed 2026-07-06 (the model-parity slice, between REN-5 and
REN-6): **D-RMAT-7** -> `FIXED` — the retail color pipeline witnessed
**gamma-space end to end** (no `D3DSAMP_SRGBTEXTURE` at any device
sampler-state site, no `D3DRS_SRGBWRITEENABLE` at any render-state site, no
sRGB `.fx` pass states, identity display ramp at default gamma 1.0
`[orig: GLib_SetGammaRamp @ 0x677be0; default @ 0x84f354]`); the host was
decoding textures sRGB→linear and re-encoding at the blit around the
witnessed math. Fixed across the composer + the full shader set: raw
sampling + gamma-space math + the exact-inverse `nova_gamma_to_linear`
output (`godot/shaders/nova_color.gdshaderinc`), with a new swatch-probe
**calibrate mode** proving byte identity 256/256 on the live build; T1
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
modulate; the host masks `OSCAP_DETAIL` off the key when the secondary
fails to resolve (retail's NULL-texture stage drop, exactly; the white ×1
fallback deleted). UV evidence: the .3di v8 vertex carries TWO authored UV
sets — `v_uv2` was always right. T1: exactly the 224 detail-keyed hashes
moved, 0 classification rows. Full witness:
[render/render-material-re.md](render/render-material-re.md) §Divergence
catalog.

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-RMAT-6 | Single-pass host materials; the six technique classes (NORMAL/PROJSHAD/DEPTHMASK/CLIP/GLOW/MATCHTERRAIN, batch-selected `[orig: @ 0x5d9ff3]`) are un-modeled beyond NORMAL-class state — selection ported + T1-pinned at REN-3; the class CONTENT witnessed at REN-4 (FF technique tables, the pass-execution model, the GLOW capability landed as `is_glow_capable`) | A | WITNESSED-READY-DEFERRED (selection + GLOW flag ported) | remaining host mappings ride D-RORD-4/-5 residuals |
| D-RMAT-8 | Framebuffer blending runs on blit-encoded (linear) values; retail blends gamma bytes (`[orig: decode_blend_mode_to_d3d_states @ 0x680f00]`) — opaque + alpha-tested surfaces byte-exact under D-RMAT-7, translucent composites diverge boundedly (alpha midtone shift; additive accumulates dimmer) | C | PERMANENT (register, this slice) | revisit only on an objectionable T3 composite |

### Render — draw order — [render/render-order-re.md](render/render-order-re.md) (D-RORD catalog; REN-3)

Minted at the REN-3 engine-research session (2026-07-06). D-RORD-1 (the
transparent ordering ladder — sky → far-water-side alpha → water →
camera-side alpha → overlays, from the witnessed frame bracket
`[orig: Terrain_RenderSceneWithReflection @ 0x5c93a0]`) was ported and
FIXED in the same slice: the ladder lives in `libs/renderer/render_order`
and is applied as the generalized Godot priority ladder (celestial, water,
object-model rungs), with the sort-key/pass-class semantics T1-pinned.

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-RORD-2 | Opaque state-sort (per-frame CPU quicksort by alpha-test bit → 256-unit depth slabs → effect index → fine depth `[orig: RenderBatch_QuickSort @ 0x5d8b40]`) not reproduced — the host's internal opaque ordering serves the same intent; key semantics preserved as T1-pinned functions | C | PERMANENT (register, this slice) | — |
| D-RORD-3 | Water-side transparent binning is per OBJECT (model origin at rebuild / `refresh_render_order()`) vs retail's per STRIP per frame (`[orig: @ 0x5d932e..0x5d9354]`) — straddling or water-crossing models can mis-bin strips | A | OPEN (partial) | REN-6/T3 attestation decides |
| D-RORD-4 | FP render pass ported 2026-07-09: `LocalPlayerHost` composites the viewmodel through a dedicated shared-world SubViewport — camera cull-masked to the viewmodel layer, near 0.05, weapon `renderfov` HORIZONTAL degrees converted to vertical via aspect (JO defs omit the key; every weapon uses the record default 80.0 = `flt_7D1898`, `AdmDef_InitEntryDefaults @0x53ff31`), drawn over the finished frame (the depth-window's visible equivalent) (`[orig: @ 0x4ded60: near swap @0x4dee29/restore @0x4df0aa, fov @0x4dee71 -> h->v @0x58d900, depth remap @ 0x58a7b0; parser key 'renderfov' @0x54482a]`). Per-weapon `renderfov`/`pos`/`tpos` def plumbing landed same day (net-re §5.40 fifth pass) | A | RESOLVED (SubViewport composite realizes the depth window; def plumbing landed) | PAR-WORLD |
| D-RORD-5 | No glow/envmap duplicate pass: retail re-queues strips whose effect carries capability 0x10000000 back-to-front into Q3, flushed in the bloom pass (`[orig: @ 0x5d93b5; FrameFX_RenderBloomPass @ 0x582a54]`) | A | WITNESSED-READY-DEFERRED (capability semantics landed at REN-4; the specular-cube SOURCE witnessed at REN-5 — the static sun-glint cube `[orig: Render_FillStaticCubemaps @ 0x58f290 → generate_cubemap_lighting @ 0x685bb0]`, rotated by MatRotSpecular; [render/render-lighting-re.md](render/render-lighting-re.md)) | residual = host bloom wiring (the cube content is now specced, D-RLIT-5 carries the hosting); FrameFX out of REN scope |
| D-RORD-6 | The two original sort-key quirks (opaque key bits 15+ = residual stack garbage; transparent key lags one strip within a render object) not reproduced — reproducing them manufactures garbage | C | PERMANENT (register, this slice) | — |
| D-RORD-7 | EffectWorld particles render once in a main-camera POST_TRANSPARENT compositor after water/both transparent sides; retail invokes the same global particle manager twice, between far-side transparents and water and again after camera-side transparents | A | OPEN (bounded ordering/pass-placement residual; packet command preservation, blends, and depth semantics match, while exact recursive-sort equivalence remains unproven) | split only if a water-intersection T3 scene demonstrates a visible mismatch |

### Render — lighting — [render/render-lighting-re.md](render/render-lighting-re.md) (D-RLIT catalog; REN-5)

Minted at the REN-5 session (2026-07-06), which also closed env #17 (the
modulator chain went live) and D-RMAT-5 (the composed FF lighting model) in
the same slice, converted the last `UNAUDITED` render system, and answered
D-RORD-5's specular-cube question (the static sun-glint cube). The chain is
ported libs-first (`libs/renderer/light_runtime`, `libs/env::ModulatorChain`)
and T1-pinned (`renderer_state_vectors` section 5).

Minted-and-closed 2026-07-06 (the model-parity slice): **D-RLIT-7** ->
`FIXED` — the placer's static MultiMesh batches froze the env lighting
harvested at load (no live owner for the template-harvested materials;
even the pre-first-iris-tick modulator was baked in), while retail relights
every entity from the current lighting block each frame
`[orig: setup_entity_lighting_and_shader_constants @ 0x5d98a0]`. The placer
now registers every harvested batch material and re-stamps from the live env
per frame (generation-gated; single-sourced stamping shared with
NovaObjectModel). The same slice re-derived the un-enved preview defaults to
the retail noon register (full_00.env tod 1200 bytes — composer +
nova_object_model + shader-global + terrain-include defaults, one cited
register).

Minted-and-closed 2026-07-06 (the REN-6 session): **D-RLIT-8** -> `FIXED` —
the object per-material `hemi_sky` served the RAW TOD keyframe while
`dir_color`/`hemi_ground` served the smoothed+modulated writeback (mixed
color spaces; the sky-facing hemisphere half too dark off-noon), where
retail feeds all entity lighting from the post-modulator block colors
`[orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090 fills
[8..10] ← Env_SkyBlock[0]]`. The sky block now rides the per-tick
writeback seam (`set_sky_ambient_rt`, mirroring fill/sun/fog); details in
the catalog below.

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-RLIT-1 | Hosted weather runs 4 color blocks through the modulator; retail modulates 16 (skyfog, cloud set, statics) `[orig: @ 0x57ef97..0x57f03c]` | A | OPEN (partial — the hosted subset is the rendered set) | joins as consumers are hosted (skyfog rides frame-clear/horizon) |
| D-RLIT-2 | Iris exposure targets the OUTDOOR sample each tick; retail averages 3 samples marched back from the camera-ray hit with interior detection + sun-occlusion raycasts `[orig: compute_ambient_light_along_direction @ 0x5c7a00]` | A | OPEN (partial — curve/chase/chain exact) | rides the interior system + host raycast wiring |
| D-RLIT-3 | Object materials light at full sun visibility; retail dims DirLightColor per entity (3-ray occlusion, 1.0..0.25) and lerps interior-parented entities to floor/ceiling ambience by the interior's daylight openness `[orig: @ 0x5c6800; @ 0x5d98a0]` | A | WITNESSED-READY-DEFERRED (math ported + T1-pinned) | runtime/interior slices |
| D-RLIT-4 | Dynamic point lights (≤4 D3D lights, owner/interior group culling, modulator-scaled colors, {1,0,15/r²,1}) unhosted beyond the editor LGHT preview `[orig: @ 0x5a9180; @ 0x5abc50]` | A | WITNESSED-READY-DEFERRED (color/attenuation math ported) | EffectWorld/particle track |
| D-RLIT-5 | Glass/env reflection = hemisphere-along-reflection stand-in; phong specular = pow-16 stand-in; retail samples the LIVE scene cube / the static sun-glint cube (contents witnessed) / the PhongMap texture `[orig: @ 0x6106a0; @ 0x58f290; Glass.fx]` | A | OPEN (approximation) | the D-RORD-5 bloom-wiring substrate |
| D-RLIT-6 | No terrain shadow-map PS variants or baked lightmap TGA draping (`PSShadow*` light scale `4·t3²·t0.a`; the 64-px-tile mission lightmap) `[orig: @ 0x604420; @ 0x604a90]` | A | OPEN | terrain lightmap hosting (terrain-record scope) |

---

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
| Net | 11 | 1 | 11 | 23 | 0 |
| Environment | 0 | 0 | 3 | 3 | 4 |
| World / AI + events | 25 | 1 | 6 | 32 | 13 |
| UI (menu/ctrl/sound/playerinfo/HUD) | 8 | 2 | 5 | 15 | 5 |
| Mission `.mis` | 0 | 2 | 1 | 3 | 0 |
| LW `.3di` | 0 | 2 | 1 | 3 | 0 |
| Particles `.ptl` | 2 | 0 | 0 | 2 | 15 |
| VFS / PFF mount stack | 0 | 1 | 0 | 1 | 4 |
| Credits (CBIN) | 0 | 1 | 0 | 1 | 1 |
| Terrain | 3 | 0 | 0 | 3 | 7 |
| Tiles | 0 | 0 | 0 | 0 | 2 |
| Foliage | 4 | 0 | 0 | 4 | 9 |
| Fonts | 1 | 1 | 0 | 2 | 2 |
| Boot-required resources | 0 | 0 | 0 | 0 | 1 |
| Render — materials/state | 0 | 0 | 1 | 1 | 1 |
| Render — draw order | 2 | 0 | 1 | 3 | 3 |
| Render — lighting | 4 | 0 | 2 | 6 | 0 |
| **Total** | **60** | **11** | **31** | **102** | 67 |

Dual-flagged rows (also carry a NEEDS-RE facet): D-EVT-3, D-NET-136, D-NET-64.

<!-- scoreboard:generated:end -->

The Boot-resources row is the R8 audit doing its job: an audit that converts
unknown unknowns into tracked rows RAISES the count before the burn-down
lowers it (as PAR-R1..R7 did for their six new domains).

Permanent register size: **25 IDs across 24 rows** (below). `UNAUDITED` systems: **0** — the
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
**platform/host-structural** (the host cannot or should not reproduce the original's
substrate) and **original-bug/garbage** (reproducing it would manufacture garbage against
[ADR 0003](adr/0003-no-raw-passthrough-create-from-scratch.md)). Each carries the
one-line rationale for why porting it would be *wrong*.

### Platform / host-structural (class C)

| ID | Divergence | Why porting it would be wrong |
|---|---|---|
| D-3DI-1 | MTRX byte-exact output needs OED's x87 `_PC_24` precision; a 64-bit SSE2 build diverges in low FP bits | Byte-exactness is a property of the original's 24-bit x87 mantissa; a modern SSE2 host cannot match the low bits without the documented `_controlfp(_PC_24)` parity sub-build. |
| D-MNU-4 | The original truncates each scaled quad rect to int per element; the reimpl applies one float `CanvasItem` scale | A sub-pixel cosmetic difference; reproducing per-element int truncation would fight Godot's scene-graph scale model for no visible gain. |
| D-PTL-9 | Portable LCG and host-basis yaw/pitch construction vs retail `rand()`/DirectX/FPU direction helper | The authored component bounds, `spread_skip`, and two-draw cadence match; byte-identical platform RNG and FPU basis construction would make deterministic host simulation platform-dependent. |
| D-PTL-10 | GFXFLIPRAND derives its start frame from a stable particle serial instead of the retail slot pointer | The original value depends on process address layout; a serial preserves the distribution intent without making playback allocator-dependent. |
| D-PTL-12 | Per-emitter capacity is capped at 4096 (default 256; shipped maximum override 400) while the exact retail manager-wide ceiling is unwitnessed | The bounded superset preserves authored headroom and prevents hostile mods from causing unbounded allocation or burst work. |
| D-PTL-19 | Flipbook frame counts are normalized to 1..256 while retail has no witnessed equivalent bound | A finite shared cap prevents malformed/mod-authored counts from causing unbounded frame-name, atlas, and preview work; shipped content is unaffected. |
| D-PTL-20 | The parser accepts and composes both curve modifiers while retail consumes one trailing modifier | A deterministic syntax superset improves mod tolerance; no shipped file combines modifiers, so strict emulation would only reject an otherwise well-defined extension. |
| D-NET-131 | A dedicated ("serve only") host runs as a mode-3 in-process listen server (`serve_and_play=false`), not the original's mode-1 host-only | Wire-equivalent from a joiner's view ([ADR 0011](adr/0011-single-player-in-process-listen-server.md)); the difference is host-internal bookkeeping that never reaches a connected client. |
| D-VFS-4 | Raw runtime reads now probe live like retail, while editor listings and decoded caches remain epoch snapshots | The gameplay byte-read seam no longer carries this divergence. Rebuilding editor-facing indexes and decoded Godot resources on every open would fight the host cache model; explicit remount/epoch invalidation exposes authoring changes without stale raw runtime reads. |
| D-VFS-8 | Retail's 16-search-path x 16-byte / 16-slot / 6-name caps (incl. the >5-char expansion-name strcpy overflow) | Capacity supersets; reproducing the caps (and the overflow) would manufacture the original's buffer bugs. |
| D-VFS-9 | `<exp>L.pff` mounted as our persistent primary vs retail's secondary slot 0 | Effective lookup precedence is identical; the slot bookkeeping is host-internal. |
| D-VFS-10 | Mounted loose lookups reject rooted/drive-qualified/ADS/`..` queries and symlink escapes, unlike retail's unchecked path construction [orig: FileSystem_OpenFile @ 0x75b1c0 / FileSystem_FileExists @ 0x75aa50] | A resource name must stay inside the explicitly mounted root. Preserving legitimate relative, case-insensitive lookup while refusing arbitrary host-file access is a host safety boundary, not a gameplay fidelity loss. |
| D-NET-140 | The listen host's own loopback connection receives the full 0x0A record set; retail sends its local player header-only frames | The full-record loopback is how serve-and-play renders its local view ([ADR 0011](adr/0011-single-player-in-process-listen-server.md)); that frame never leaves the process, so retail interop is unaffected. |
| D-RORD-2 | Retail's per-frame CPU quicksort of opaque batch entries (alpha-test bit → 256-unit depth slabs → effect index → fine depth) vs the host renderer's internal opaque ordering | The sort is a device-era draw-call-batching strategy, not observable behavior for z-buffered opaques; reproducing it would fight the host pipeline for zero visual difference. The key semantics survive as T1-pinned functions (`renderer::opaque_sort_key`) so any future host that CAN consume them has the witnessed spec ([render/render-order-re.md](render/render-order-re.md)). |

### Original-bug / garbage class (class D; basis: [ADR 0003](adr/0003-no-raw-passthrough-create-from-scratch.md))

| ID | Divergence | Why porting it would be wrong |
|---|---|---|
| env #11 | Original packs negative color components as garbage (no lower clamp); the reimpl clamps to 0 | Reproducing unclamped negative-color UB would carry garbage bytes through the parser for no defined behavior. |
| D-NET-133 (empty-slot facet) | An in-capacity EMPTY 0x18 slot replies a zeroed type-0 record; retail serializes the slot's raw (possibly stale) memory | The observable effect is identical (the client stops at the type gate either way); reproducing retail's stale-memory bytes would be manufacturing garbage. |
| D-MUS-7 | `op_callvl` (`0x0A` call form) resolves against an uninitialised-BSS name table in Jointops, so the opcode is dead; the reimpl mirrors the dead stub (push 0) | The original behavior *is* "do nothing" (the table is never populated); porting a "working" call would invent behavior the engine never had. |
| D-MUS-5 | `inc_g`/`dec_g` (`0x11`/`0x12`) operate on 1 byte and raise no globals-dirty notify | An intentional mirror of the original's silence; adding the notify would diverge from the witnessed behavior. |
| D-PTL-1 | The engine's outer dispatcher remaps `g2_color1`/`g3_color1`/… into higher color slots (a parse bug); the reimpl maps `g{N}_color{M}` correctly | A recorded intentional divergence: the correct mapping is what an author means; reproducing the dispatch remap would carry the engine's parse bug forward. |
| D-PTL-11 | Retail reads `scale_lut[i+1]` one byte past the 256-byte table at the final sample; the host clamps to byte 255 | The overread is adjacent heap memory and therefore allocator-dependent garbage; clamping the last 1/256th avoids manufacturing undefined data. |
| D-VFS-6 | Retail's PFF open trusts the header blindly (no magic/entry_size/count checks; entry_size>36 overflows; two write-after-free bugs @ 0x768348/0x7685ba) — ours validates and is UAF-free | Reproducing unvalidated reads and UAFs would manufacture garbage against ADR 0003. |
| D-SCR-1 / D-SCR-2 | The SCR container codec accepts version bytes 0–2 and selects the key from the version byte + policy, where each original call site fixes the key | A deliberate multi-title superset so one codec serves JO-demo-era and shader containers; load-bearing equivalence holds for everything retail JO ships. |
| D-RORD-6 | The original's two sort-key defects: opaque key bits 15+ OR in an uninitialized stack slot (`@ 0x5d92b9`), and a transparent strip's key reads the depth slot BEFORE its own store, lagging one strip within a render object (`@ 0x5d9326`) | Both are stale/uninitialized-memory reads whose effect is accidental (constant-per-call garbage; a one-strip-stale depth); reproducing them would manufacture the bugs rather than the intent (back-to-front by depth), against ADR 0003. |
| D-INF-15 | FP bone builder: model rows past the anim's bone count sum an uninitialized `bone_translations` stack slot into their world position on flag-2 (translated) clips (`BoneAnim_BuildWorldMatrices @0x40c6e9..0x40c71d`; the buffer is only written for anim rows `@0x40c4bc..0x40c57c`); the reimpl adds zero | An uninitialized-stack read whose value is accidental per call; reproducing it would manufacture garbage against ADR 0003 — the witnessed intent (rows ride bone 0's matrix + their model pivot) is what the reimpl ports. |

`PERMANENT` is not a resting place for hard work: each entry above is a decision that the
*faithful* behavior is to diverge. If a future need arises (e.g. exact host-internal-state
match for D-NET-131, or a byte-exact MTRX parity sub-build for D-3DI-1), the record names
the follow-up path.

---

## Audit track

### The 2026-07-05 sweep — COMPLETE

All seven systems that started with no RE record now have one (full or partial) or
a tracked-by-composition audit; their divergences are tracked rows, not unknown
unknowns.

| System | Slice | Result |
|---|---|---|
| VFS / PFF | PAR-R7 | full record — [vfs/vfs-pff-mount-re.md](vfs/vfs-pff-mount-re.md) (D-VFS-1..10) |
| Fonts | PAR-R4 | full record — [fonts/fnt-re.md](fonts/fnt-re.md) (D-FNT-1..4) |
| Foliage | PAR-R2 | fresh full record 2026-07-13 plus MODEL and near-secondary LOW corrections 2026-07-14 — [foliage/foliage-re.md](foliage/foliage-re.md); both tier cores MATCHING, D-FOLIAGE-7/-9/-10 bounded host gaps |
| Tiles | PAR-R3 | full record — [tiles/til-re.md](tiles/til-re.md), overlay/atlas/flip-rotate MATCHING vs retail `@0x60df0d`/`@0x604700` |
| Credits (CBIN) | PAR-R5 | partial — [credits/cbin-re.md](credits/cbin-re.md); codec (magic + header + ROL32/XOR cipher `@0x75e348`) MATCHING vs `libs/cbin`, witnessed read-only via raw disasm; markup + read-path NEEDS-RE |
| Terrain | PAR-R1 | re-grilled partial 2026-07-13 — [terrain/terrain-re.md](terrain/terrain-re.md); preprocessing/top shader/LOD matching, D-TERRAIN-7/-8 runtime gaps and D-TERRAIN-9 editor-preview gap bounded |
| Importer | PAR-R6 | tracked-by-composition — [importer/importer-audit.md](importer/importer-audit.md); composes RE'd libs, no independent parity surface |

**Notes from the sweep (2026-07-05):** two "which binary" assumptions were
corrected by testing them — **the foliage generator cores and tile overlay
core audit cleanly against retail**, with their host gaps tracked separately
(the same functions ship in retail: foliage detail starts at
`generate_foliage_instances_0 @ 0x5ffdd0`, while `0x600197` is only an internal
sample; tiles use `PolyTrn_RenderTile @ 0x60df0d`).
**The CBIN codec IS in retail JO** — the magic is a binary constant a string search
misses; `find_bytes 43 42 49 4E` finds the writer at `~0x75e250` and the cipher
loop at `0x75e348` (`rol ebx,7` + `xor [blob],key&0xFF`, 4-byte groups), byte-exact
to `libs/cbin`. The two partials (Terrain, Credits) have their remaining grills
scoped in their records; the six code-cited-jodemo systems are retail-anchored
where they ship.

### Render (REN) — reopened 2026-07-05, burned back to `UNAUDITED` = 0 on 2026-07-06

The REN planning grill ([ADR 0023](adr/0023-render-visual-parity.md),
[maturity-program.md](maturity-program.md) REN track) found the runtime render
path silently uncovered — the one substantially **reimplemented but
unwitnessed** surface: the object-material chain (the `libs/oed` 45-entry
shader-tag table + the `libs/renderer` classifier/composer +
`NovaObjectShaderCache`) carries only ModSuperOed-side citations, and no
record covers batching/draw order, the runtime TSS stage tables, or lighting
application. Three systems enter `UNAUDITED`; the REN grill slices convert
them into records with catalogs (raising open counts before the burn-down
lowers them, as the R-audits did):

| System | Slice | Record |
|---|---|---|
| Object materials / render state (the runtime flag/tag→state path) | REN-2 | **landed 2026-07-06** — [render/render-material-re.md](render/render-material-re.md) (D-RMAT, tabled above) |
| Batching / draw order / pass structure | REN-3 | **landed 2026-07-06** — [render/render-order-re.md](render/render-order-re.md) (D-RORD, tabled above) |
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

---

## Normalized prose-only catalogs (this train)

Four records originally tracked divergences in prose only; PAR-0 minted stable IDs from their
then-existing text. Later evidence passes have extended the particle catalog through D-PTL-23:

- [threedi/3di-gp-format-re.md](threedi/3di-gp-format-re.md) → **D-3DI-1** (the MTRX
  SSE2 low-FP-bit divergence; `PERMANENT`).
- [threedi/3di-lw-format-re.md](threedi/3di-lw-format-re.md) → **D-3DILW-1..3** (v8
  branch, textures, SAF/KSA playback — the record's own deferrals).
- [particles/ptl-format-re.md](particles/ptl-format-re.md) → **D-PTL-1..23** (the
  intentional parse mapping, renderer/runtime approximations, platform-stable substitutions,
  and bounded-safety choices; pure "not yet researched" §8 items stay in §8; D-PTL-8 is closed).
- [mission/mis-format-re.md](mission/mis-format-re.md) → **D-MIS-1..5** (the
  writer-subset gaps + the full `dfx2med.exe` grill as a `NEEDS-RE` row;
  D-MIS-4/-5 minted-and-FIXED at the 2026-07-07 Nile parity pass).
