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
   unknowns into tracked rows — **VFS/PFF (R7), Fonts (R4), Foliage (R2), and Tiles
   (R3) landed; three remain** (terrain, credits, importer — see the UNAUDITED table).

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

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| env #14 | Sun-glare terrain-raycast occlusion held at full brightness (8-jittered-ray + ±16/frame hysteresis unmodeled) | A | OPEN (PARTIAL) | PAR-ENV |
| env #15 | Thunder SoundBank triggers (0 / 0x80) + `SETFLASH1` start — fully specced, wiring deferred to WAC weather | A | WITNESSED-READY-DEFERRED | PAR-ENV |
| env #16 | `.trn`/`overcast.def` first-pass TOD table + overcast cross-fade — precedence corrected, runtime carries the `.env` table only until WAC weather lands | A | WITNESSED-READY-DEFERRED | PAR-ENV |
| env #17 | Iris auto-exposure modulator gain — the CURVE is ported to `libs/env` (`iris_gain`, unit-tested, `[orig: @ 0x5c7550]`); the residual is the modulator CHAIN that applies the gain to the color blocks (runtime consumer, `get_terrain_lighting_attenuation` still identity for the iris path) | A | WITNESSED-READY-DEFERRED | PAR-ENV |
| env #18 | Earthquake / rain / wind oscillator rings — constants documented, wiring deferred to WAC weather | A | WITNESSED-READY-DEFERRED | PAR-ENV |

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
| D-CTRL-1 | Mouse/joystick binding arrays (profile-built at runtime) not ported; those rows show a blank Control column | A | OPEN | PAR-UI |
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

### Tiles — [tiles/til-re.md](tiles/til-re.md) (D-TIL catalog; PAR-R3)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-TIL-1 | `TIL_FLAG_OUTLINE` (0x08): `libs/til` models a jodemo LINELIST outline pass; retail's overlay render (`render_water_quad @ 0x604700`) handles only flip/rotate (bits 0/1/2) — outline may be jodemo-only (→ PERMANENT) or a separate retail path | B | NEEDS-RE | PAR (tiles) |

Overlay entry (12 B), atlas UV, flip/rotate flags, half-texel shift, Z negation,
and the 128-LRU cache are **MATCHING** vs retail `PolyTrn_RenderTile @ 0x60df0d`.

### Foliage — [foliage/foliage-re.md](foliage/foliage-re.md) (D-FOLIAGE catalog; PAR-R2)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-FOLIAGE-1 | Instance color: one color/MultiMesh-instance (2×2 lightmap avg) vs the engine's per-vertex quad color `0xFF000000 | (0x404040 + avg>>1)` under a 2× draw + alpha-premultiplied colormap — visually close, the per-vertex gradient is the residual | A | OPEN (approximation) | PAR (foliage) |

Placement (seed 0xA55B1EED, ROL-hash PRNG, 36 candidates/cell, surface gate,
0x20000 proximity) is **MATCHING** — byte-exact vs retail `generate_foliage_instances_0 @ 0x600197`.

### Fonts — [fonts/fnt-re.md](fonts/fnt-re.md) (D-FNT catalog; PAR-R4)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-FNT-1 | Offset +4 is the design-width scale reference (`800/it`), not a version; our reader rejects `!= 800` (strictness) — identical for shipped JO fonts (all 800), stricter than retail otherwise | A | OPEN | PAR (fonts) |
| D-FNT-2 | The per-font design scale `800/designWidth` is not retained by our reader (moot at 800; render scaling is host-side, ENG-4) | A | OPEN (minor) | PAR (fonts) |
| D-FNT-3 | Offset +12 (`hdr3`) named `shadow_offset` but only STORED by the loader — the shadow semantics are unconfirmed | B | NEEDS-RE | PAR (fonts) |

### Boot-required resources — [required-resources.md](required-resources.md) (D-BOOT catalog; R8/ENG-6)

| ID | One-liner | Class | Disposition | Slice |
|---|---|---|---|---|
| D-BOOT-1 | Menu/game music bank resolution: retail hardcodes `MENUMUS.SBF/.BIN` + `GAMEMUS.SBF/.BIN` (`M<exp>`/`G<exp>` under an expansion); `menu_shell.gd` scans by name heuristic instead | A | OPEN | rides the ENG-6 manifest (Wave 2) |

---

## Count-to-zero scoreboard

Open counts by domain (the target is zero in every cell):

| Domain | OPEN | NEEDS-RE | WITNESSED-READY-DEFERRED | Domain open total |
|---|---|---|---|---|
| Net | 8 | 0 | 13 (2 also NEEDS-RE) | 21 |
| Environment | 1 | 0 | 4 | 5 |
| World / AI + events | 2 | 0 | 4 | 6 |
| Item def | 0 (D-ITEMDEF-1 `FIXED` 2026-07-05) | 0 | 0 | 0 |
| UI (menu/ctrl/sound/playerinfo/HUD) | 8 | 1 (+1 dual) | 5 | 14 |
| Mission `.mis` | 0 | 2 | 1 | 3 |
| LW `.3di` | 0 | 2 | 1 | 3 |
| Particles `.ptl` | 2 | 2 | 0 | 4 |
| Tiles (new domain, PAR-R3 audit) | 0 | 1 | 0 | 1 |
| Foliage (new domain, PAR-R2 audit) | 1 | 0 | 0 | 1 |
| Fonts (new domain, PAR-R4 audit) | 2 | 1 | 0 | 3 |
| Boot resources (new domain, R8 audit) | 1 | 0 | 0 | 1 |
| VFS/PFF (new domain, PAR-R7 audit) | 3 | 1 | 0 | 4 |
| **Total OPEN** | | | | **66** |

The Boot-resources row is the R8 audit doing its job: an audit that converts
unknown unknowns into tracked rows RAISES the count before the burn-down
lowers it (the same will happen at PAR-R1..R7).

Permanent register size: **17** (below). `UNAUDITED` systems: **3** (below).
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

`PERMANENT` is not a resting place for hard work: each entry above is a decision that the
*faithful* behavior is to diverge. If a future need arises (e.g. exact host-internal-state
match for D-NET-131, or a byte-exact MTRX parity sub-build for D-3DI-1), the record names
the follow-up path.

---

## UNAUDITED systems (no RE record yet)

Three systems remain documented mainly by code and tests (VFS/PFF via PAR-R7;
**Fonts** via PAR-R4; **Foliage** via PAR-R2; **Tiles** via PAR-R3 — foliage and
tiles both audited cleanly against RETAIL even though the ports were made from
jodemo)
([docs/README.md](README.md)). Each gets a research audit (engine-research / grill-ida)
that lands an RE record **with a D-catalog**, converting untracked divergences into
tracked rows.

**Which binary each audit needs (finding, 2026-07-05, revised):** the code was
originally RE'd from **`jodemo.exe`** (the demo's more-accessible renderer, cited
addresses like `sub_5C0240`/`Terrain_DrawTileOverlays2D @ 0x5C79C0`), but the
same systems ARE in retail — **Foliage (R2) audited cleanly against retail**
(`generate_foliage_instances_0 @ 0x600197`, placement MATCHING) and **Tiles (R3)
is retail-auditable** too (`PolyTrn_RenderTile @ 0x60df0d`, `serialize_terrain_tiles
@ 0x6080F0` witnessed), just multi-part (overlay + atlas + tilestrip). **Terrain
(R1)** is the large renderer/mesh pipeline; the jodemo IDB is the accessible
route but retail equivalents exist. Fonts (R4) audited cleanly (retail
`sub_580400`/`sub_674740`). The **CBIN credits** format (`.kda`, R5) is NOT in
retail JO (no `CBIN`/`.kda` string), so it needs the source binary. The importer
(R6) is a Python + native-FFI pipeline, not a binary-format audit.

| System | Audit slice | Partial coverage today | IDB needed |
|---|---|---|---|
| Terrain | PAR-R1 | [oned/editor-runtime-parity.md](oned/editor-runtime-parity.md) records the intentional terrain-shader edit/runtime split (shared surface-shading include). | jodemo |
| Credits | PAR-R5 | D-MNU-6 (CBIN credits custom fonts/images) is the one tracked credits divergence. | source binary (CBIN not in retail JO) |
| Importer pipeline | PAR-R6 | none (behavior in `apps/importer/` + tests). | n/a (Python + FFI) |

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
