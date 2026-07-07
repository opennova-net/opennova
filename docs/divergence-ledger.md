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
[env/env-tod-re.md](env/env-tod-re.md) #21), and **env #19** -> `FIXED` (the terrain
tint grill re-shaped it: the FULL/HALF split + both LIVE consumers ported —
tile-overlay HALF×MODULATE2X, foliage `min((texel×FULL)>>7,255)` — while the
texture-bake consumer proved DEAD CODE, readers zero-xref, so the untinted terrain
surface is ratified faithful; witness in [env/env-tod-re.md](env/env-tod-re.md) #19,
honored-matrix terrain_tint -> HONORED; residual emitter facets ride PAR-R2).

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
surface with SrcBlend ONE + DestBlend SRCALPHA and alpha-test ref 32
`[orig: Water_InitSurfaceShaders @ 0x5c19b0; render_water_surface
@ 0x5c33f0..0x5c3419]`; the reimpl used standard alpha blending —
`water.gdshader` now expresses the witnessed blend exactly
(`blend_premul_alpha` + inverted alpha) with the ref-32 discard.

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| env #15 | Thunder SoundBank triggers (0 / 0x80) + `SETFLASH1` start — fully specced, wiring deferred to WAC weather | A | WITNESSED-READY-DEFERRED | PAR-ENV |
| env #16 | `.trn`/`overcast.def` first-pass TOD table + overcast cross-fade — precedence corrected, runtime carries the `.env` table only until WAC weather lands | A | WITNESSED-READY-DEFERRED | PAR-ENV |
| env #17 | Iris auto-exposure modulator gain — **FIXED 2026-07-06 (REN-5)**: the modulator CHAIN is live (`env::ModulatorChain` ticks modulator2 → modulator → the hosted blocks in the witnessed order `[orig: @ 0x57ef97..0x57f03c]`; 62-tick exposure chase `[orig: @ 0x57e512; @ 0x57d940]`; ÷64 gain to `ColorSrcGlobalGain`/ambient scale `[orig: @ 0x58db30; @ 0x5aaef0]`); env vectors re-dumped surgically (8 weather rows). Sampling-geometry + unhosted-block residuals tracked as D-RLIT-1/-2 ([render/render-lighting-re.md](render/render-lighting-re.md)) | A | FIXED | REN-5 (from PAR-ENV) |
| env #18 | Earthquake / rain / wind oscillator rings — constants documented, wiring deferred to WAC weather | A | WITNESSED-READY-DEFERRED | PAR-ENV |
| env #29 | Water surface tessellation — **FIXED 2026-07-07 (the REN-6 tail)**: the DETAILED tier live end to end (`env::water_*` structural translation with 40 ctest pins → `NovaWaterCore.strip_*` packed arrays → the per-frame strip ArrayMesh → the witnessed ps.1.1 chain in water.gdshader, found at the port's debug: alpha = noiseA×diffuseA×2, reflection ×2 diffuse ×4 noise + specular `[orig: Water_InitSurfaceShaders @ 0x5c19b0]`); goldens re-pinned. Residuals in-row (env-tod-re.md #29): the LOW tier unported (host runs detail > 1), the underwater opaque-material swap unhosted, the grazing fade exposes the below-horizon dome band (rides #30's skyfog work) | A | FIXED | REN-6 tail |
| env #30 | Water reflection — **FIXED 2026-07-07 (the REN-6 tail)**: host planar reflection (SubViewport mirror camera about y = wh, up-column-negated proper mirror — the witnessed strip rows pin u = screenU / v = vbase − screenV so the ps.1.1 texm3x2 lookup runs verbatim `[orig: Water_InitSurfaceShaders @ 0x5c19b0; render_main_scene @ 0x5c1240]`) feeding the t2 sampler; water self-excluded via a visual layer. Residuals in-row (env-tod-re.md #30): the wh − 0.1 clip plane approximated (no host oblique near plane), half-res host choice, the below-horizon skyfog band rides the dome follow-up | A | FIXED | REN-6 tail |

### World / AI + mission events — [world/world-wac-ai-re.md](world/world-wac-ai-re.md), [mission/bms-event-runtime-re.md](mission/bms-event-runtime-re.md), [world/itemdef-re.md](world/itemdef-re.md)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-INF-1 | No blend windows on clip switches (the original blends 10/15 ticks, root motion included) | A | WITNESSED-READY-DEFERRED (rides the skeletal/blend pass) | PAR-WORLD |
| D-INF-2 | Command channels 123–127 (mount/waypoint) partially driven; walk-to-seat staging, 126/127, child-seat traversal, seat-bone follow, driver-lean pending | A | OPEN (partial) | PAR-WORLD |
| D-INF-3 | Ground/water resolver: horizontal capsule + platforms/water + airborne anim overlay pending (the vertical capsule-bottom settle landed as D-INF-6) | A | OPEN (partial) | PAR-WORLD |
| D-INF-5 | Idle look-at system + its spotting side effects — rides the combat pass | A | WITNESSED-READY-DEFERRED | PAR-WORLD |
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

Record is PARTIAL by documentation depth, NOT by open divergences: the data path
(build → mesh-simplify → CPT) is proven **byte-identical** across 5 fixtures
(`dvd4_parity` + `parametric_parity` Sample/Gradient/Checker64/Perlin). The tracked
terrain divergences are settled — D-TERRAIN-1 deliberate (shader split), D-TERRAIN-2
and D-TERRAIN-3 FIXED; the remaining work is documenting the CDEP/LOD bitstream +
retail-anchoring the render pass, not closing a parity gap.

### Tiles — [tiles/til-re.md](tiles/til-re.md) (D-TIL catalog; PAR-R3)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-TIL-1 | `TIL_FLAG_OUTLINE` (0x08): the LINELIST outline is jodemo-only; retail JO's render (`render_water_quad @ 0x604700`) omits it and so do we (flag preserved for round-trip, no outline drawn) — faithful to retail JO | B | **FIXED (faithful)** | PAR (tiles) |

Overlay entry (12 B), atlas UV, flip/rotate flags, half-texel shift, Z negation,
and the 128-LRU cache are **MATCHING** vs retail `PolyTrn_RenderTile @ 0x60df0d`.

### Foliage — [foliage/foliage-re.md](foliage/foliage-re.md) (D-FOLIAGE catalog; PAR-R2)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-FOLIAGE-1 | Instance color: one color/MultiMesh-instance vs the engine's per-VERTEX quad color + alpha-premultiplied colormap read — the half-plus-bias emitter form `0xFF000000 \| (0x404040 + avg>>1)` IS applied since 2026-07-07 (the foliage-combine slice); the per-vertex gradient is the residual | A | OPEN (approximation — narrowed 2026-07-07) | PAR (foliage) |
| D-FOLIAGE-2 | The fragment combine ran an unwitnessed ratio stand-in (`(sun/(ground·0.707+sun))×255/128`, no terrain-colormap sample, no SKY term — the shader's own header carried the correct witness) where retail runs `rgb = t0 × (t1 × (t1.a·c1 + c0)) × v0 × 8` with t1 = the planar-projected terrain colormap and c0/c1 = the SKY/LIGHT blocks `[orig: Foliage_CreateLightmapBlendPS @ 0x5ff7a0; constants @ 0x604420]` | A | **FIXED (2026-07-07)** — the witnessed chain ported (planar uv = (x,−z)/texsize wrap ≡ the CPU sampler; the dispatcher binds the colormap from the CPU-color source chain) | REN-6 rider |
| D-FOLIAGE-3 | Wind sway: the host displaces X weighted by height (`VERTEX.y/8`, sin of `phase + 0.11x + 0.07z`) where the witnessed VS displaces Z weighted by vertex RED via a polynomial sine of `world.x·c24.y + time` `[orig: Terrain_CreateFoliageVertexShaders @ 0x5ff630; Foliage_WindSwayVS @ 0x2c25e5c]`; the sway amount/phase globals are live (NovaWeather) | A | OPEN (stand-in — minted 2026-07-07) | rides the foliage render-emitter parity |

Placement (seed 0xA55B1EED, ROL-hash PRNG, 36 candidates/cell, surface gate,
0x20000 proximity) is **MATCHING** — byte-exact vs retail `generate_foliage_instances_0 @ 0x600197`.

### Fonts — [fonts/fnt-re.md](fonts/fnt-re.md) (D-FNT catalog; PAR-R4)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-FNT-1 | Offset +4 is the design-width scale reference, not a version; our reader rejected `!= 800` | A | **FIXED** (no equality gate; non-800 parses, `fnt_roundtrip`) | PAR (fonts) |
| D-FNT-2 | The per-font design scale `800/designWidth` was not retained | A | **FIXED** (`design_width` carried; `fnt_design_scale`) | PAR (fonts) |
| D-FNT-3 | Offset +12 (`hdr3`) named `shadow_offset` but only STORED by the loader — the shadow semantics are unconfirmed | B | NEEDS-RE | PAR (fonts) |

### Boot-required resources — [required-resources.md](required-resources.md) (D-BOOT catalog; R8/ENG-6)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-BOOT-1 | Menu/game music bank resolution: retail hardcodes `MENUMUS.SBF/.BIN` + `GAMEMUS.SBF/.BIN` (`M<exp>`/`G<exp>` under an expansion); `menu_shell.gd` scans by name heuristic instead | A | OPEN | rides the ENG-6 manifest (Wave 2) |

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
| D-RORD-4 | Viewmodel has no depth treatment (clips into near walls); retail draws it FIRST under near-Z 0.05 + viewport depth range [0, 0.1] with its own flush (`[orig: @ 0x4ded60; @ 0x58a7b0]`) | A | WITNESSED-READY-DEFERRED | runtime slice; T3 scene 6 |
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
| Environment | 0 | 0 | 3 | 3 | 3 |
| World / AI + events | 2 | 0 | 4 | 6 | 0 |
| UI (menu/ctrl/sound/playerinfo/HUD) | 8 | 1 | 5 | 14 | 0 |
| Mission `.mis` | 0 | 2 | 1 | 3 | 0 |
| LW `.3di` | 0 | 2 | 1 | 3 | 0 |
| Particles `.ptl` | 2 | 2 | 0 | 4 | 0 |
| VFS / PFF mount stack | 3 | 1 | 0 | 4 | 0 |
| Credits (CBIN) | 0 | 1 | 0 | 1 | 1 |
| Terrain | 0 | 0 | 0 | 0 | 3 |
| Tiles | 0 | 0 | 0 | 0 | 1 |
| Foliage | 2 | 0 | 0 | 2 | 1 |
| Fonts | 0 | 1 | 0 | 1 | 2 |
| Boot-required resources | 1 | 0 | 0 | 1 | 0 |
| Render — materials/state | 0 | 0 | 1 | 1 | 1 |
| Render — draw order | 1 | 0 | 2 | 3 | 2 |
| Render — lighting | 4 | 0 | 2 | 6 | 0 |
| **Total** | **34** | **11** | **30** | **75** | 14 |

Dual-flagged rows (also carry a NEEDS-RE facet): D-EVT-3, D-HUD-2, D-NET-136, D-NET-64.

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
| Foliage | PAR-R2 | full record — [foliage/foliage-re.md](foliage/foliage-re.md), placement MATCHING vs retail `@0x600197` |
| Tiles | PAR-R3 | full record — [tiles/til-re.md](tiles/til-re.md), overlay/atlas/flip-rotate MATCHING vs retail `@0x60df0d`/`@0x604700` |
| Credits (CBIN) | PAR-R5 | partial — [credits/cbin-re.md](credits/cbin-re.md); codec (magic + header + ROL32/XOR cipher `@0x75e348`) MATCHING vs `libs/cbin`, witnessed read-only via raw disasm; markup + read-path NEEDS-RE |
| Terrain | PAR-R1 | partial — [terrain/terrain-re.md](terrain/terrain-re.md); surface + witness basis mapped, D-TERRAIN-1 shader split; mesh_simp/CDEP deep grill pending |
| Importer | PAR-R6 | tracked-by-composition — [importer/importer-audit.md](importer/importer-audit.md); composes RE'd libs, no independent parity surface |

**Notes from the sweep (2026-07-05):** two "which binary" assumptions were
corrected by testing them — **Foliage and Tiles audit cleanly against retail**
(the code cites jodemo but the same functions ship in retail: foliage
`generate_foliage_instances_0 @ 0x600197`, tiles `PolyTrn_RenderTile @ 0x60df0d`).
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
- [mission/mis-format-re.md](mission/mis-format-re.md) → **D-MIS-1..3** (the
  writer-subset gaps + the full `dfx2med.exe` grill as a `NEEDS-RE` row).
