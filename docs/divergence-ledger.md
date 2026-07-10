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
[env/env-tod-re.md](env/env-tod-re.md) #21), and **env #19** -> `FIXED`
(corrected 2026-07-09): the FULL/HALF split remains ported and the confirmed
LIVE terrain renderer consumer is the tile-overlay HALF×MODULATE2X path. FAR's
four CPU tinted-colormap samples execute, but their intermediate colors are
unconditionally overwritten by the source-Y red wind weight; they are not an
instance-tint consumer. FAR fragment lighting separately combines terrain
light/colormap T1 with c0/c1/c6, whose exact host feed rides D-FOLIAGE-7. The
texture-bake consumer remains DEAD CODE, readers zero-xref, so the untinted
terrain surface is ratified faithful; witness in
[env/env-tod-re.md](env/env-tod-re.md) #19 and the honored matrix).

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
| env #29 | Water surface tessellation — **FIXED 2026-07-07 (the REN-6 tail)**: the DETAILED tier live end to end (`env::water_*` structural translation with 40 ctest pins → `NovaWaterCore.strip_*` packed arrays → the per-frame strip ArrayMesh → the witnessed ps.1.1 chain in water.gdshader, found at the port's debug: alpha = noiseA×diffuseA×2, reflection ×2 diffuse ×4 noise + specular `[orig: Water_InitSurfaceShaders @ 0x5c19b0]`); goldens re-pinned. 2026-07-07 fidelity facets: the far-fade discard misport deleted (#34 re-grade), the witnessed spec-alpha fog factor + z-write/depth-replica model hosted (`depth_draw_always` + the tracked 2⁻¹⁵ near-ward nudge), the underwater opaque `0x20000` swap HOSTED (`u_underwater_view` premul branch). Residuals in-row (env-tod-re.md #29): the LOW tier unported (host runs detail > 1), the nightvision redraw unhosted, the below-horizon dome band pointer rides #30/D-TERRAIN-3 | A | FIXED | REN-6 tail |
| env #30 | Water reflection — **FIXED 2026-07-07 (the REN-6 tail)**: host planar reflection (SubViewport mirror camera about y = wh, up-column-negated proper mirror — the witnessed strip rows pin u = screenU / v = vbase − screenV so the ps.1.1 texm3x2 lookup runs verbatim `[orig: Water_InitSurfaceShaders @ 0x5c19b0; render_main_scene @ 0x5c1240]`) feeding the t2 sampler; water self-excluded via a visual layer. Residuals in-row (env-tod-re.md #30): the wh − 0.1 clip plane approximated (no host oblique near plane), half-res host choice, the below-horizon skyfog band rides the dome follow-up | A | FIXED | REN-6 tail |
| env #35 | Water sine LUT provenance: the runtime `std::sin` build forked per libm at trunc boundaries (the GitHub `macos-26-arm64` image flipped non-landmark bytes and every downstream noise pixel — `env_render_unit` red on macOS only, 2026-07-10); the original builds ONE deterministic instance via x87 fsin `[orig: Water_InitNoiseFieldAndSineLut @ 0x5c0308..0x5c0334]` | C | FIXED (2026-07-10): the LUT is a committed 256-byte constant — the deterministic instance every existing pin was generated from; landmarks + symmetry sum unchanged; the retail-instance byte check rides the pinned-current caveat (env-tod-re.md #35) | fidelity 2026-07-10 |

### World / AI + mission events — [world/world-wac-ai-re.md](world/world-wac-ai-re.md), [mission/bms-event-runtime-re.md](mission/bms-event-runtime-re.md), [world/itemdef-re.md](world/itemdef-re.md)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-INF-1 | No blend windows on clip switches (the original blends 10/15 ticks, root motion included) | A | WITNESSED-READY-DEFERRED (rides the skeletal/blend pass) | PAR-WORLD |
| D-INF-2 | Command channels 123–127 (mount/waypoint) partially driven; walk-to-seat staging, 126/127, child-seat traversal, seat-bone follow, driver-lean pending | A | OPEN (partial) | PAR-WORLD |
| D-INF-3 | Ground/water resolver: horizontal capsule + platforms/water + airborne anim overlay pending (the vertical capsule-bottom settle landed as D-INF-6) | A | OPEN (partial) | PAR-WORLD |
| D-INF-5 | Idle look-at system + its spotting side effects — rides the combat pass | A | WITNESSED-READY-DEFERRED | PAR-WORLD |
| D-INF-11 | Third-person body aim overlay (torso bend) — witnessed in full + **LOCAL PLAYER PORTED 2026-07-08** (world-wac-ai-re §14/§14.6: libs/anim aim_overlay + leg-chase sim + eval_pose_overlay path, probe-verified); **upper-body weapon channel producer WITNESSED + LOCAL PLAYER FULLY PORTED 2026-07-09** (§14.8: entity secondary AnimMap channel +0x18C; reload 65/66 via the +0x372 80-tick window; the weapon.def `special_hold` kind ladder → hold poses 50–61 + scoped variants; `attack_anim` → the 62/63 fire stamps; the +0x371 arms-dip feed into headLookDecay; mask-bone hard override before the overlay compose; probes body_reload_probe + body_holds_probe pistol/knife); §14.3 pitch kick RESOLVED = the audio mixer output power meter `g_audioOutLevel` (port needs a host mixer level tap); remaining: NPC/remote threading (closes D-NET-117; AI bodies lack the hold keys — RESET backfill vs our no-op), the binoculars input toggle (ladder side ported), blend windows on channel re-init (D-INF-1), mounted/seated branches, attachments | A | OPEN (partial — local player landed incl. the full weapon channel) | PAR-WORLD |
| D-INF-12 | Player (org2) chase sources approximated by org1 §3.3 math: body-heading quarter-step + one shared leg re-plant target/window (the original's per-leg +0x2e4/+0x2e8 divergence + org2 chase constants unwitnessed — `Entity_UpdateInfantryPlayerBody @ 0x4b40e0`) | A | OPEN (needs org2 grill) | PAR-WORLD |
| D-INF-13 | Body rigs still consume `.bad` channels as ABSOLUTE bone orientations with the bind-matrix skeleton rest; the original composes every clip's channels against the rig's ONE skeleton bind — the `.adm` slot-0 `.bad` pinned into `channel+44` at registration (`AnimMap_RegisterEntity @0x40bb60`; `AnimChannel_ComputeBoneMatrices @0x410da0` `Transpose(bind 3x3) × channel`; `build_world_bone_matrices @0x40c770` = same composed math as the FP `@0x40c400`). Identical output for healthy exports (channel-at-reset == bind); the faithful `model_bind` path (net-re §5.40 + the 2026-07-09 bind-source + model-table corrections) currently engages for FP viewmodel rigs only. The part↔bone "matcher" question is RESOLVED: the original never matches — rows pair BY INDEX bounded by the model-side table (the FK never reads the `.bad`'s bone count/parents/positions), so 20-parts-vs-19-bones bodies need no map, just the body table semantics: `@0x40c770` reads the 108-byte entity bone table (`skeletonData+104` count, parent `@+40`, 16.16 fixed pivots `@+56/60/64` in (z,x,y) order with x negated, bind-inverse `T(−parent pivot)`, NO bone-0 padding loop) — port that table + a body probe pass to close | A | OPEN (FP landed; bodies pending) | PAR-WORLD |
| D-INF-14 | FP viewmodel `model_bind` composition CORRECTED 2026-07-09: the bind operand is the SKELETON (`.adm` slot-0) `.bad`'s records via the `channel+44` override — per-clip self-bind (the 2026-07-08 reading) self-cancels at clip start and froze the rig at its T-pose (`AnimMap_RegisterEntity @0x40bb60 @0x40bbe3`; `AnimChannel_ComputeBoneMatrices @0x410da0 @0x410dd8`). Composition `q(stored skeleton bind) ⊗ channel`, operand order pinned visually on the ak47 rig (conjugate collapses the rig); `NOVA_VM_DELTA` knob DELETED. **Rig-source corrected same day (the model-table port)**: the rig's count/hierarchy/pivots come from the MODEL table (`modelDef+52/+56`), `.bad` rows pair by index, rows past the anim's bones take bone 0's composed matrix (`@0x40c5a1`) — the reimpl's silent fallback to `BadBone.position` on count mismatch is GONE (it made AKM_1st work only because AKM's pos happen to be healthy; 12/43 JO viewmodel rigs ship zeroed/stale pos and retail renders them all — §5.40 corpus sweep). Remaining tail: def `rot` bias signs + reload direction + finger/left-hand pose vs retail footage | A | OPEN (mechanism witnessed + ported; footage confirm of sense tail pending) | PAR-WORLD |
| D-INF-15 | Model-table rows past the `.bad`'s bone count: the original's flag-2 translation add reads UNINITIALIZED stack floats for those rows (`bone_translations` written only for anim rows, the FK sums it for every model row `@0x40c6e9..0x40c71d`); the reimpl adds zero | D | PERMANENT (register, this slice) | — |
| D-WPN-1 | weapon.def FUNCTION rows resolve through the `g_actionFuncDefTable @0x829E58` name registry (18 entries incl. the `*_map` scope variants + `powerup_*`); the port fixes each state's behavior instead — safe because every shipped row (JOX + REVX sweeps) names `wpn_std_<its own suffix>` (net-re §5.62) | A | WITNESSED-READY-DEFERRED (registry table witnessed; port when a non-std consumer appears) | PAR-WORLD |
| D-WPN-2 | Single-pool ammo model: the original tracks per-ammo-class carried pools (`Entity_GetScoreValueBySlotType @0x5406e0` class byte def+0xD8, units/round def+0xE0, pool caps `ammoclass_max_carry`); the FSM folds them to one rounds counter and the recoil auto-reload gate approximates units=1 (net-re §5.62) | A | OPEN (rides the ammo-class/pool port, with §5.57/§5.58 pool semantics) | PAR-WORLD |
| D-WPN-3 | `WeaponSlot_CanFire @0x541ba0` legs beyond the clip: busy weapon-child entity, the underwater-fire ban vs `Env_WaterHeightFixed`, the adm+224 score-lock; plus the kick bump's fire-sound-id gate (Def+0x294) — all need slot/env/sound state the port does not model yet (net-re §5.62) | A | OPEN (partial — the clip + reserve-routing leg ported) | PAR-WORLD |
| D-WPN-4 | The heat model: the def+876/880 window stamp into slot+0x14, `WeaponSlot_CalcAccumulatedHeat @0x53f780` (internals unwitnessed), the pump's overheat deny (heat>0xFFFF → queued FIRE becomes EMPTY `@0x541046`), and the OVERHEATED(11) entry writer (unfound) (net-re §5.62) | A | NEEDS-RE (CalcAccumulatedHeat + the state-11 writer), then port | PAR-WORLD |
| D-WPN-5 | Weapon-switch machinery — NOW FULLY WITNESSED (net-re §5.62 switch-chain block): `Player_SwitchToWeaponByHandle @0x4e0170` category scan over `weaponSlotArrayBase @0xB75FD4` → `Player_MountWeaponSlot @0x4dfa40` writes `g_pendingWeaponSlot` + queues SWITCHRANK(8)/`ForceQueueSwitchFrom`(7); the switchfrom swap + `TryQueueSwitchTo`, the −901 instant paths (Flags&0x80), the recoil def+0x168 auto-switch, mount-scoped auto-engage — the FSM ports the timing shapes; the multi-slot pool + swap port rides the loadout slice | A | WITNESSED-READY-DEFERRED (rides the priority-3 loadout/equipped-weapon slice) | PAR-WORLD |
| D-WPN-6 | The pump covers the LOCAL player's equipped slot only; the original pumps every pool-0 equipped slot + pool-1 unmounted weapons with a live muzzle flash (`WeaponAction_ProcessAllEntities @0x542690`) (net-re §5.62) | A | OPEN (extend with NPC/remote weapon state) | PAR-WORLD |
| D-WPN-7 | Interim ammo seed: the FSM installs with clip=clipsize + reserve=startrounds from the def; the original resolves ammo through the PLAYER_INFO loadout + S2C 0x5A apply (§5.30/§5.57) (net-re §5.62) | A | OPEN (rides D-PLAYERINFO-1/-11) | PAR-WORLD |
| D-WPN-8 | FSM↔net uplink unwired: the fired event does not emit C2S 0x06 nor spawn rounds through world::RoundSim, and a joiner's reload_requested does not send C2S 0x25 / await the S2C 0x49 refill (the authority path is complete and zero-latency) (net-re §5.62, §5.16, §5.58) | A | OPEN (rides the npruntime in-match integration) | PAR-WORLD |
| D-WPN-9 | ADS residuals: the SIGHTS overlay texture draw (DefSightEntry parsed, undrawn), the unscope-on-move consumer site, zoom-level adjust keys (`Player_AdjustWeaponZoomLevel @0x4dbcc0`), the scope-state C2S 0x1D notify, the 7-step interp variant (Field0C&0x200), stance (parentSlot 2/5) + NVG gates (net-re §5.62, §5.41) | A | OPEN (partial — toggle/tpos-ease/FOV/rescope landed) | PAR-WORLD |
| D-EVT-1 | Spawn-point activation on fire: fully witnessed (POI/deploy list `0xB76570`, marker @0x452ce0, +0x210/+0x217/+0x218 authoring) — rides the deploy/POI subsystem port | A | WITNESSED-READY-DEFERRED | PAR-WORLD |
| D-EVT-3 | Residual after the TriggerRelations port (state + evaluators + recounts + alert stamps + damage-site SHOT writes landed): the acquisition/fire-time write quads ride the combat pass, motor visited marks ride D-INF-2, sub 11 needs the held-object link; cat-2 alert/count + 42-45 subs unwitnessed | A | WITNESSED-READY-DEFERRED (write-sites) + NEEDS-RE (cat-2 subs) | PAR-WORLD |

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
| D-HUD-1 | Stance indicator = discrete cross-faded `HUDSTANCE` frames (IDB `draw_minimap_compass_overlay` + oscarmike model it as a compass) — the cross-fade ported 2026-07-09 (`hud_stance.gd`/`hud_fade.gd`); the IDB rename is still a held proposal | A | FIXED (2026-07-09 port; the IDB rename proposal stays held) | PAR-UI |
| D-HUD-2 | Stance widget = frame-swap + fade; heading/north is a *separate* top-down radar (do not port a rotating ring) | A | NEEDS-RE (the radar; the stance leg is fixed by the 2026-07-09 port) | PAR-UI / research starter |
| D-HUD-3 | HUD design space is fixed 1024×768, scaled round-to-nearest (`Viewport_ScaleToVirtualCoords`) | A | FIXED (`hud_layout.gd`) | PAR-UI |
| D-HUD-4 | Health-bar fill WIDTH uses the capped `+92` ratio; fill COLOR uses an uncapped recomputed ratio — the port matches both reads | A | FIXED | PAR-UI |
| D-HUD-5 | Clip-indicator flash restamp keys on (`round_type`, reserve) — the original keys (ammo class `def+220`, reserve, pool id `def+216`); same transitions under the single-pool weapon model (D-WPN-2) | A | OPEN (revisit with per-class pools) | PAR-UI |
| D-HUD-6 | Mission triggered text ported as a timed message-line feed (930-tick life, ≥186 stagger) at the `HUDCHATTEXT` anchor — the original rides the full chat pipeline (channel ring buffers + a geometry table whose writer is unwitnessed) | A | OPEN (chat-pipeline follow-up) | PAR-UI / research starter |
| D-HUD-7 | Crosshair spread omits the recoil accumulators (`player+0x380/+0x384 >> 7`) — the runtime does not surface them yet; ERROR-row term ported exactly | A | OPEN (needs the recoil write-side witness) | PAR-UI / research starter |
| D-HUD-8 | Crosshair color modulates the texture — the original writes it to the strip's specular channel (blend stage in the unwitnessed HUD shader pass); identical for the default white | B | OPEN (witness the texture-stage state) | PAR-UI |

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
| D-PTL-3 | `mod2x` approximates `DESTCOLOR`/`SRCCOLOR` over Godot `blend_mul`; the 0.5 midpoint is preserved, the gamma curve is not byte-exact | A | OPEN (approximation) | PAR-UI/render |
| D-PTL-4 | `bump`/`bumpadd` lit-color rotation about view-Z vs the engine's composite-matrix X (combiner topology matches; pending a 4×4 port + reference capture) | A | OPEN (approximation) | PAR-UI/render |
| D-PTL-5 | `distort` fixed-strength screen-tex UV offset; the engine stage-1 combiner bytes are undecoded | B | NEEDS-RE | PAR-UI/render |
| D-PTL-6 | Atlas pack strategy undecoded (shelf vs the engine's layout); `inset` bleed padding defaults to 0 | B | NEEDS-RE | PAR-UI/render |

The LW `.3di` record is unlanded overall (PR #45 closed); its rows ride whenever an LW
import is revived. Note D-3DILW-1's v8 branch overlaps the 3DI/GP audit surface only at
the container-detection seam.

### VFS / PFF mount stack — [vfs/vfs-pff-mount-re.md](vfs/vfs-pff-mount-re.md) (D-VFS catalog; PAR-R7)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-VFS-1 | Loose gating session-global vs retail's per-call forces (saves/foliage/gt.ssc/UI force loose-first sans /d; BMS-from-PFF forces archive-only under /d) | A | OPEN | PAR (vfs) |
| D-VFS-3 | Path-qualified names passed verbatim in retail (loose subdir probes; flat archive names); our flat_key strips everywhere | A | OPEN | PAR (vfs) |
| D-VFS-5 | Encrypted-entry streaming: retail decrypts whole-file reads only; ours always — corpus check needed | B | NEEDS-RE | PAR (vfs) |
| D-VFS-7 | Query-only normalization (31-char truncation, trailing-space trim) vs our both-sides — pathological names only | A | OPEN (minor) | PAR (vfs) |

D-VFS-4/6/8/9 are permanent candidates (register below). Closed 2026-07-05:
**D-VFS-2** -> `FIXED` — `Vfs::mount_game` defaults to the witnessed fixed boot
table (`VfsArchiveDiscovery::RetailTable`: language/localres/resource.pff in
slot order, extra archives never mount, pinned by
`test_mount_game_retail_table` `[orig: PFF_OpenAllArchives @ 0x4a4310, table
@ 0x829f90]`); the editor's browse index deliberately keeps `ScanAll`
(recorded in the record's D-VFS-2 row — an authoring tool indexes arbitrary
modder archives), and `NovaResourceRoot::mount_runtime` passes `RetailTable`.

### Credits (CBIN) — [credits/cbin-re.md](credits/cbin-re.md) (D-CBIN catalog; PAR-R5, PARTIAL)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-CBIN-1 | Credits `~C`/`~F`/`~J`/`<CR>` markup consumers (the retail scroller) not yet witnessed; D-MNU-6 custom-font/image resolution rides here | B | NEEDS-RE | PAR (credits) |
| D-CBIN-2 | Read path CONFIRMED: 8 rol-7 cipher sites in the CBIN codec region (0x75e158-0x75e914) incl. decode loops (`@0x75e473` read+decipher) — retail READS CBIN, not just writes; our symmetric decode matches | B | **RESOLVED (MATCHING)** | PAR (credits) |

CBIN codec (magic 0x4E494243 + 20-B header + ROL32/XOR cipher `@0x75e348`) is
**MATCHING** vs `libs/cbin`, witnessed read-only via raw disasm (no IDB write).

### Terrain — [terrain/terrain-re.md](terrain/terrain-re.md) (D-TERRAIN catalog; PAR-R1, PARTIAL)

Minted-and-closed 2026-07-06 (the model-parity follow-up): **D-TERRAIN-2** ->
`FIXED` — the shared surface include stacked TWO ×2 detail-normal factors on
top of the 3-way splat (the gobj-era "v23 dual-normal" chimera); the
witnessed top-tier ps.1.4 shader applies EXACTLY ONE normal term —
`out = (cm.a·c1 + c0)/2 ×2 cm ×2 dp3(normalmap, blendmap) ×4 splat`
`[orig: PolyTrn_PS14SplatNormalMap source @ 0x7dece0; PolyTrn_PS14Splat
@ 0x7dee18; assembled by compile_terrain_pixel_shaders @ 0x605260]` (the
dual-normal product belongs to the separate non-splat ps.1.1 tier,
PolyTrn_PSDualNormalMap, never combined with the splat). Under the
gamma-faithful pipeline (D-RMAT-7) the squared bump factor clipped whole
terrain regions to white (retail-texture measurements: colormap ≤128
nominal, normal pairs 127.5-128 avg — the extra factor was the only >1
multiplier). The include now matches the witnessed instruction stream;
`terrain_surface_light` T1 pins are unaffected (the c0/c1 lighting shape is
unchanged). Residual note-only: the witnessed t3 is the heightmap-derived
generated normal map (`Texture_GenerateNormalMap`, scale 1/32) — the host
samples the .trn near/far detail-normal pair as the texture-source stand-in
until that generator ports; the detailmap2/dist2 pair stays authored (the
.trn owns the slots; the ps.1.1 tier consumes them).

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-TERRAIN-1 | Terrain-shader edit/runtime split: editor live-sculpt shader vs runtime baked shader, sharing the surface-shading math via an include — a tracked deliberate divergence justified by the editing need | C | PERMANENT (candidate) | PAR (terrain) |
| D-TERRAIN-2 | Shared surface include stacked TWO ×2 detail-normal factors on the splat (gobj-era chimera); the witnessed top-tier ps.1.4 applies exactly ONE `[orig: PolyTrn_PS14SplatNormalMap @ 0x7dece0; compile_terrain_pixel_shaders @ 0x605260]` — post-gamma the squared factor clipped regions to white (full witness in the prose block above) | A | **FIXED (2026-07-06, the model-parity follow-up)** | REN (model-parity) |
| D-TERRAIN-3 | Below-horizon region: cameras see past the sky dome's 1024-unit rim to the raw viewport background — retail fills the below-rim region with the **frame clear alone** (the env #21 skyfog blend; no skirt/ring geometry exists in the frame walk — sky-pass leg witness `Terrain_RenderSkyboxPass @ 0x610ac0` → `Terrain_RenderSectorBatchLit @ 0x60c670`, the seam hidden by fog convergence at the 1024 fog reference). The host's ported clear consumer was silently swallowed by a `BG_SKY`(null-sky) Environment mode rendering BLACK — a 1-px black dome-rim seam behind covering geometry, and once env #29's witnessed strip march landed (the strips stop at their witnessed row/fog-clamp extent like retail's), a PURE-BLACK BAND filling the whole strip-edge-to-rim region in water-horizon views (near half the frame at dusk) | C | **FIXED (REN-7, 2026-07-07)**: two facets — `game_world.tscn` ClearColor → `BG_COLOR` + `AMBIENT_SOURCE_DISABLED`, GUT-pinned (`game_world_test`), so the env #21 consumer renders at all; and `get_frame_clear_color()` now serves the post-blend DOUBLED skyfog (the modulate2x-path Clear consumes it verbatim `[orig: @ 0x677100]`; the 07-05 "undoubled" reasoning was the non-modulate2x fallback, no host analog — the undoubled band measured exactly half the fogged dome rim). Env vectors re-dumped (frame-clear token only). Residual (not a retail-parity surface): the ONED editor preview keeps its own viewport environment — far-env adoption rides ONED polish/ENV-1 | REN-7 |
| D-TERRAIN-4 | The ported terrain raycast's editor-host guards (the `terrain_raycast.h` sampler seam): beyond-extent samples report no-terrain/no-hit where retail CLAMPS the cell to the grid edge — terrain continues forever `[orig: OOB masks @ 0x31a0010/0x319fc0c]`; no terrain data returns clear/NAN where retail returns HIT `[orig: @ 0x60ccf7]`; the bilinear substrate is our contiguous-atlas sampler where retail applies a per-quadrant seam-flag +1-neighbor policy `[orig: Terrain_SeamFlags_* @ 0x31a17f0..]`. Observable in principle only past the authored rim (e.g. the celestial glare ray) | C | PERMANENT (candidate) — the same editor-guard class already ratified for `coords_editor_options` (ADR 0020 seam); the witnessed retail forms are recorded in [terrain/terrain-re.md](terrain/terrain-re.md) §Runtime terrain queries for any future runtime-faithful host | ENG-3 B1b |

Record is PARTIAL by documentation depth, NOT by open divergences: the data path
(build → mesh-simplify → CPT) is proven **byte-identical** across 5 fixtures
(`dvd4_parity` + `parametric_parity` Sample/Gradient/Checker64/Perlin). The tracked
terrain divergences are settled — D-TERRAIN-1 deliberate (shader split), D-TERRAIN-2
and D-TERRAIN-3 FIXED, D-TERRAIN-4 the ENG-3 editor-guard candidate; the remaining
work is documenting the CDEP/LOD bitstream + retail-anchoring the render pass, not
closing a parity gap.

### Tiles — [tiles/til-re.md](tiles/til-re.md) (D-TIL catalog; PAR-R3)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-TIL-1 | `TIL_FLAG_OUTLINE` (0x08): the LINELIST outline is jodemo-only; retail JO's render (`render_water_quad @ 0x604700`) omits it and so do we (flag preserved for round-trip, no outline drawn) — faithful to retail JO | B | **FIXED (faithful)** | PAR (tiles) |

Overlay entry (12 B), atlas UV, flip/rotate flags, half-texel shift, Z negation,
and the 128-LRU cache are **MATCHING** vs retail `PolyTrn_RenderTile @ 0x60df0d`.

### Foliage — [foliage/foliage-re.md](foliage/foliage-re.md) (D-FOLIAGE catalog; PAR-R2)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-FOLIAGE-1 | FAR's four terrain-color samples are dead with respect to the emitted vertex: the unconditional final write is red-only `clamp(trunc(srcY*128),0,255)<<16`, consumed as wind weight `[orig: @ 0x6002DB..0x60030A]`; the former terrain-color/half-plus-bias account is retracted | A | **FIXED / false premise retracted (2026-07-09)** — `far_mesh_emitter.cpp` emits the witnessed source-height weight | foliage IDA parity |
| D-FOLIAGE-2 | FAR runs `rgb=t0*(t1*(t1.a*c1+c0))*c6.rgb*8`, `a=t0.a*c6.a`; MODEL does not share this blend `[orig: Foliage_LightmapBlendPS @ 0x5ff7a0]` | A | **FIXED (scope corrected 2026-07-09)** — split `foliage_far.gdshader`; exact sector T1/c6 inputs ride D-FOLIAGE-7 | foliage IDA parity |
| D-FOLIAGE-3 | FAR wind uses source-height RED, c27 phase wrap, the witnessed tenth-order polynomial at amplitude 0.03, render-Z-only displacement, and `c24.x = GetTickCount()*0.003 + Env_WaveOscRing[0]*1.5258789e-6` `[orig: Terrain_CreateFoliageVertexShaders @ 0x5ff630; constants @ 0x600450]` | A | **FIXED (2026-07-09)** — the shader mechanics and exact host phase feed are ported | foliage IDA parity |
| D-FOLIAGE-4 | MODEL stamps full source geometry with cap 21, XZ 0.75/Y 0.5, yaw-only eight-sample ground fit, anchor-derived alpha, duplicate tile draws per qualifying anchor, and per-tile-draw wind counter `[orig: @ 0x600980, 0x601f50, 0x601d90, 0x600f00]`; former shared-shader/shared-cap and FAR ground-patch addenda are retracted | B | **FIXED (corrected 2026-07-09)** — dedicated MODEL host/shader; FAR uses the full-mesh emitter; exact upstream entity visibility rides D-FOLIAGE-7 | foliage IDA parity |
| D-FOLIAGE-5 | Both tiers bind the model submesh[0] `:fd` bake: wrapped 3×3 alpha kernel and flattened `0x808080` RGB `[orig: Foliage_LoadDefAssets @ 0x601260; Foliage_DrawModelTileSlot @ 0x601d90]` | B | **FIXED (2026-07-08)** — `libs/foliage/fd_bake` + `VegAssets.resolve_slot_fd_textures` | foliage model-tier slice |
| D-FOLIAGE-6 | MODEL table[16] is a witnessed one-stage pass: T0=`:fd`, flags `0x00440000`, RGB selects c6 diffuse `(0,0,0,1)`, alpha=`T0.a*diffuse.a`; unfogged RGB is black and the texture supplies the silhouette `[orig: @ 0x601260, 0x601d90, 0x600f00]` | C | **FIXED (2026-07-09)** — separate black-alpha `foliage_model.gdshader`, not FAR's lightmap combine | foliage IDA parity |
| D-FOLIAGE-7 | FAR feed/pool/draw state witnessed AND hosted 2026-07-10 (42.0 traversal collect <=128 `[orig: Terrain_CollectNearFoliagePatches @ 0x603e60]`, bake-once slot pool `[orig: Foliage_UpdateFarCellSlots @ 0x601b30]`, fade knee 20/slope 1-22, high pass under 33 refs 180/8; the jodemo per-entity dispatcher and place_cell view cull deleted). Remaining approximate: exact per-tile T1 render-target content (host recomposes colormap x detail splat at LOD 0), exact c6.rgb floats (host: 0.5 FF-parity neutral), MODEL sector-entity visibility, blocker registry, the second low wireframe resubmit, retail pool residency count | A | WITNESSED-READY-DEFERRED (narrowed 2026-07-10) — local placement/emission/shader mechanics are ported; these five upstream feeds remain | terrain/foliage integration |

Candidate placement is **MATCHING**: FAR's seed/ROL-hash PRNG, gates and
36-candidate/36-accepted ceiling come from `generate_foliage_instances_0
@ 0x5ffdd0`; MODEL has its separate cap 21 at
`Foliage_GenerateModelTileInstances @ 0x600980`. Exact upstream traversal and
blocker/entity/draw inputs are D-FOLIAGE-7.

### Fonts — [fonts/fnt-re.md](fonts/fnt-re.md) (D-FNT catalog; PAR-R4)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-FNT-1 | Offset +4 is the design-width scale reference, not a version; our reader rejected `!= 800` | A | **FIXED** (no equality gate; non-800 parses, `fnt_roundtrip`) | PAR (fonts) |
| D-FNT-2 | The per-font design scale `800/designWidth` was not retained | A | **FIXED** (`design_width` carried; `fnt_design_scale`) | PAR (fonts) |
| D-FNT-3 | Offset +12 (`hdr3`) named `shadow_offset` but only STORED by the loader — the shadow semantics are unconfirmed | B | NEEDS-RE | PAR (fonts) |

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
| World / AI + events | 12 | 1 | 6 | 19 | 1 |
| UI (menu/ctrl/sound/playerinfo/HUD) | 8 | 2 | 5 | 15 | 3 |
| Mission `.mis` | 0 | 2 | 1 | 3 | 0 |
| LW `.3di` | 0 | 2 | 1 | 3 | 0 |
| Particles `.ptl` | 2 | 2 | 0 | 4 | 0 |
| VFS / PFF mount stack | 3 | 1 | 0 | 4 | 0 |
| Credits (CBIN) | 0 | 1 | 0 | 1 | 1 |
| Terrain | 0 | 0 | 0 | 0 | 4 |
| Tiles | 0 | 0 | 0 | 0 | 1 |
| Foliage | 0 | 0 | 1 | 1 | 6 |
| Fonts | 0 | 1 | 0 | 1 | 2 |
| Boot-required resources | 0 | 0 | 0 | 0 | 1 |
| Render — materials/state | 0 | 0 | 1 | 1 | 1 |
| Render — draw order | 1 | 0 | 1 | 2 | 3 |
| Render — lighting | 4 | 0 | 2 | 6 | 0 |
| **Total** | **41** | **13** | **32** | **86** | 27 |

Dual-flagged rows (also carry a NEEDS-RE facet): D-EVT-3, D-NET-136, D-NET-64.

<!-- scoreboard:generated:end -->

The Boot-resources row is the R8 audit doing its job: an audit that converts
unknown unknowns into tracked rows RAISES the count before the burn-down
lowers it (as PAR-R1..R7 did for their six new domains).

Permanent register size: **19** (below). `UNAUDITED` systems: **0** — the
runtime-render systems reopened the set on 2026-07-05 (the REN audit track
below, [ADR 0023](adr/0023-render-visual-parity.md)); REN-2 converted the
materials/state system, REN-3 the draw-order system, and REN-5 the lighting
system (all 2026-07-06) — **the audit track is burned back to zero**; every
system has an RE record (full or partial) or a tracked-by-composition audit.
(The former unnumbered BMS-second-chunk note is now D-EVT-5, minted and closed
in the World table above.)

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
| D-VFS-4 | Snapshot resource index vs retail's live per-call resolution | A host cache; our hosts remount on change — re-resolving every open would fight the indexed host model for no observable gain (mid-session loose drops are a dev workflow, not gameplay). |
| D-VFS-8 | Retail's 16-search-path x 16-byte / 16-slot / 6-name caps (incl. the >5-char expansion-name strcpy overflow) | Capacity supersets; reproducing the caps (and the overflow) would manufacture the original's buffer bugs. |
| D-VFS-9 | `<exp>L.pff` mounted as our persistent primary vs retail's secondary slot 0 | Effective lookup precedence is identical; the slot bookkeeping is host-internal. |
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
| VFS / PFF | PAR-R7 | full record — [vfs/vfs-pff-mount-re.md](vfs/vfs-pff-mount-re.md) (D-VFS-1..9) |
| Fonts | PAR-R4 | full record — [fonts/fnt-re.md](fonts/fnt-re.md) (D-FNT-1..3) |
| Foliage | PAR-R2 + 2026-07-09 correction | full record — [foliage/foliage-re.md](foliage/foliage-re.md), FAR full-mesh and MODEL tier mechanics witnessed; terrain feeds tracked by D-FOLIAGE-7 |
| Tiles | PAR-R3 | full record — [tiles/til-re.md](tiles/til-re.md), overlay/atlas/flip-rotate MATCHING vs retail `@0x60df0d`/`@0x604700` |
| Credits (CBIN) | PAR-R5 | partial — [credits/cbin-re.md](credits/cbin-re.md); codec (magic + header + ROL32/XOR cipher `@0x75e348`) MATCHING vs `libs/cbin`, witnessed read-only via raw disasm; markup + read-path NEEDS-RE |
| Terrain | PAR-R1 | partial — [terrain/terrain-re.md](terrain/terrain-re.md); surface + witness basis mapped, D-TERRAIN-1 shader split; mesh_simp/CDEP deep grill pending |
| Importer | PAR-R6 | tracked-by-composition — [importer/importer-audit.md](importer/importer-audit.md); composes RE'd libs, no independent parity surface |

**Notes from the sweep (2026-07-05):** two "which binary" assumptions were
corrected by testing them — **Foliage and Tiles audit against retail**
(the code cites jodemo but the same functions ship in retail: foliage
`generate_foliage_instances_0 @ 0x5ffdd0` — candidate loop interior
`@ 0x600197` — and tiles `PolyTrn_RenderTile @ 0x60df0d`).
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

Four records tracked divergences in prose only; PAR-0 minted stable IDs from their own
existing text (no new findings, no reworded witnesses):

- [threedi/3di-gp-format-re.md](threedi/3di-gp-format-re.md) → **D-3DI-1** (the MTRX
  SSE2 low-FP-bit divergence; `PERMANENT`).
- [threedi/3di-lw-format-re.md](threedi/3di-lw-format-re.md) → **D-3DILW-1..3** (v8
  branch, textures, SAF/KSA playback — the record's own deferrals).
- [particles/ptl-format-re.md](particles/ptl-format-re.md) → **D-PTL-1..6** (the
  intentional `g{N}_color{M}` map + the §6 bounded deviations; pure "not yet researched"
  §8 items stay in §8).
- [mission/mis-format-re.md](mission/mis-format-re.md) → **D-MIS-1..5** (the
  writer-subset gaps + the full `dfx2med.exe` grill as a `NEEDS-RE` row;
  D-MIS-4/-5 minted-and-FIXED at the 2026-07-07 Nile parity pass).
