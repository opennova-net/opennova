# Weapon and vehicle HUD validation — 2026-09-19

PR #655 implements the weapon/vehicle display paths identified in the first
HUD audit: launcher targeting, mounted aim cues, vehicle status, aircraft
altitude, received designation radius, hit feedback, the Inset scene, and the mortar's separate impact HUD.
It also preserves the earlier seat-specific ammo/stance/panel corrections.

## Reference

Read-only comparison against IDA MCP `Jointops.exe.kong.i64`, image base
`0x400000`, and `~/Development/jo-c/Jointops.exe.kong.c` at
`1dfaae1a2aaf9e7879cfae60d7aa8d1461867486`. No reference files or IDB records
were changed. The starting PR revision was `c56ee9b8a`.

The reference checks corrected these earlier interpretations:

- `Render_RadarCompassOverlay @0x5C9740` belongs to **Scoped + FLAGS2 Inset**,
  not mortar/base FLAGS `0x200`. The load at `@0x5CA2B1` reads definition
  `+0x0C`. The installed mortars author Sighted, OnlyScoped, UseDesignator,
  2DImpact and ShowImpactDist; they do not author Scoped/Inset.
- Friendly brackets use an `mpattrib` bit selected by the is_client flag,
  which connection modes 2 AND 3 set: single player, the listen host and a
  joiner all test bit 3, and bit 8 is the dedicated host's leg, which draws no
  HUD (`@0x5926D4..0x5926E1`; `CGameSession_SetConnectionMode @0x4C49F0`). It is not a
  clock-driven blink. They surround the main aim anchor, not the target's
  projected position (`@0x592680..0x592705`, `@0x592CE2..0x592DD7`).
- The crosshair's color is **diffuse**, not specular. Forced disassembly of
  `@0x590F50` and the FVF `0x2C4` stores at `@0x678962/@0x678A3E` establish
  XYZRHW, diffuse at +16, zero specular at +20, and two UV sets. The
  decompiler's local variable names were offset by one vertex field.
- `UseDesignator` (`FLAGS 0x200000`) adjusts the 2D impact radius using the
  nearest active same-team type-1 designation received through S2C `0x6B`.
  `LollyPop` (`0x8000`) draws the red world marker. The installed mortar
  authors UseDesignator and does not author LollyPop; the two are independent.

## Display and transition matrix

| Context | Implemented behavior | Original evidence |
| --- | --- | --- |
| Lowered launcher / infantry | Authored ammo/name/round icons, stance, silhouette fade and user-colored spread reticle; capacity-one reload does not restart the flash without a displayed ammo change. | `@0x599A30`, `@0x59A710`, `@0x5A7CBE` |
| Raised Javelin / Stinger | Authored SIGHTS cards and scope text; tracked-target fire-origin cursor with lock/team colors; friendly brackets; all clear on lowering, weapon change, binoculars or death as their original gates require. | `@0x592640`, `@0x59E420` |
| CustomAim mounted weapon | The weapon's own reticle at the mounted muzzle ray's collision point; replaces the ordinary spread reticle. | `@0x592973..0x592AB8` |
| Tank / emplaced optic | Controller forward cue, articulated turret lag cue, third-person `dirguide.tga`, and the flag-8 commander reticle with its clipped circle and connecting line. | `@0x59EA20`, `@0x59ECA0`, `@0x59E3F6`, `@0x59E5B9..0x59E87F` |
| Controller / driver | Authored vehicle silhouette and localized medium/low/high control label, drawn left-aligned in the large overlay font (Impac22b). Separate weapon category/seat rules continue to govern ammo, stance, hull and occupant health. | `@0x59A5D0` (font slot `@0x59A6F9`), `@0x4B8661..0x4B8786` |
| Aircraft unit type 3 | ALTGRP-controlled nonlinear AGL ladder, feet readout, authored AGL color and bold label font. Absolute altitude and vertical speed participate in admission; this routine does not draw separate power/velocity gauges. | `@0x4B86CF..0x4B8734`, `@0x59F050` |
| Scoped + Inset | Separate scene camera with slot offsets, the additional shake sample, inclusive viewport bounds, 32-part aperture, green antialiased ring/cross and the friendly label in the bold overlay font. The aperture renders the whole world pass, particles included: the world's particle renderer compiles a second view group for the Inset camera while the pass renders (the first-person particle domain stays out, on the viewmodel layer the camera masks). The optical pass survives HUD declutter. | `@0x5C9740..0x5CA0E1`, gate `@0x5CA290..0x5CA2B4`, label font `@0x5CA0C0`, scene call `@0x5C9DE9` (particle passes `@0x5C95AC..0x5C95B5`, `@0x5C9687..0x5C9690`) |
| Mortar deployed | Side-effect-free falling-object impact prediction, green map radius (color `0xFF208020`) and localized impact distance. The distance line draws left-aligned in the hudpos font from a design anchor pulled back by half its width as the bold overlay slot measures it. Live map slots retain their 1984-tick lifetime. A missed trajectory removes the map marker and retains the last valid distance. OnlyScoped gates the preview. | `@0x4DE350`, `@0x445420`, `@0x540D00`, `@0x5BEA19`, distance line `@0x5A897E..0x5A89D5` |
| UseDesignator / LollyPop | UseDesignator reads the received link table in order, excludes expired/type-3/wrong-team records, and adjusts the impact radius. LollyPop independently draws the LOS-dependent red world marker: a stem and a head that is a 2:1 ellipse (the ring record's width scale is 2.0 beside its 2.0 stroke). | `@0x4DEBE1..0x4DEC62`, `@0x5BBF10`, `@0x5BEC10`, `@0x5A87F9..0x5A89DA`, head `@0x59322D..0x593283` (width scale `@0x59327B`, applied `@0x5D4513`) |
| Mortar raise / lower / holster | Authored `scopeup_map` opens mode 2 only from mode 0; `scopedown_map` and `switchfrom_map` close only mode 2. Mode 3 is preserved. Both local and joiner pumps consume the callbacks. | `@0x5432D0`, `@0x543360`, `@0x5434E0` |
| Physical hit feedback | The PERSON impact handler arms the shooter's latch at its head, for a victim not already dead, even if armor then absorbs the damage. Entity contacts and squib rays never set it. The owner's next outgoing frame carries the bit once. Each received frame sets the countdown to ten or decrements it; rendering does not consume it. Sighted optics retain the red reticle while this feedback is active. | person handler `0x4E98F0` (dead gate `@0x4E9920`, `or [eax+2Ch], 1000h` `@0x4E9962`), writer `@0x4FF7C5..0x4FF7D9`, reader `@0x42FF5C..0x42FF74`, reticle `@0x592BCE` |
| Carry / vehicle service | Parachute, armor and cargo icons; received preround and FARP timer/zone state feeds localized armory/bay/rearm prompts, centred on design (512, 280) in the large overlay font. The FARP unlock word is the host's owned-zone walk for the recipient's team, recomputed on every phase-0 frame (0 for an empty chain). | `@0x5925C0`, `@0x599C20`, `@0x5BDE60` (font slot `@0x5BDFD7` / `@0x5BE0AA` / `@0x5BE0F7`), writer `@0x4FF996..0x4FF9BB`, client store `@0x430136` |
| Dismount / death | Vehicle instruments and control labels clear; infantry groups return on dismount. The death-screen latch alone blanks the crosshair, instrument and scope passes: the local dead bit and the death lerp camera do not, so they keep drawing between the death and the latch. The LollyPop marker, the impact distance and the impact preview carry no death test at all. | `@0x5A7BB0` (gate `@0x5A7BBC`), `@0x59264D` (gate `@0x592646`), `@0x5A850D`, ungated tail `@0x5A87EF..0x5A89DA`, preview `@0x4DE760..0x4DE79D` |

The preview follows the original falling-object callback rather than firing
an invisible live round: drag, velocity integration, gravity, water crossing,
and terrain snap cannot produce damage, effects, or consume round slots.
The zero-spread radius reads weapon `splash` (`AdmDef+0x454`), not the ammo
blast radius. Crosshair geometry snaps the outer rectangle first, then uses
integer midpoints/half-extents and literal atlas UVs.

Native modules own eligibility, source points, state and draw commands.
Godot loads the art/fonts, projects through the current camera and renders
the extra SubViewport; it does not infer weapon families from names.

## Validation

The initial HUD implementation passed native Release and Godot RelWithDebInfo
builds before integration with the runtime-only base branch:

- **21/21 native suites pass**, including the new combat HUD and received
  designation tests, the existing weapon/vehicle integration suites, and
  308 unchanged original-instruction guided-missile vectors.
- **40/40 Godot HUD tests pass with D3D12 Forward+**, with 586 assertions
  and no pending cases. The headless run passes 39 tests / 558 assertions;
  only the GPU pixel-readback test is pending there.
- Windows **debug export and boot smoke checks pass** for both Mod Tools
  and Runtime; ONED packs the game and both distribution ZIPs are produced.
- Ratchet, maturity, include/link graph, orphan-header, environment,
  citation, conventions, fixture, retail-gate and ledger checks pass.
  No baselines change.

The focused native command is:

```powershell
ctest --test-dir build -C Release --output-on-failure -R '^(hud_combat|client_minimap_overlay|hud_frame_compiler|hud_math|hud_vehicle_panel|vehicle_panel_feed|sight_overlay|special_weapon_parity|inmatch_joiner_role|local_player_targeting|local_player_view|player_look|projectile_combat|vehicle_motor|guided_missile_flight|weapon_fsm|fire_sound|emplaced_gun_channel|throwables|weapon_inventory|npruntime_client_fire)$'
```

The GPU test command below also runs headless when the rendering options
are replaced with `--headless`. Runs use isolated application settings,
`OPENNOVA_JO_ASSETS` for the reference definitions, and `OPENNOVA_JO_DIR`
for the installed art and vehicle data.

```powershell
& ./.godot-bin/Godot_v4.6.1-stable_win64_console.exe --path godot --rendering-method forward_plus --rendering-driver d3d12 -s addons/gut/gut_cmdln.gd -gtest=res://tests/hud_overlay_test.gd,res://tests/hud_sights_card_test.gd,res://tests/hud_presenter_lanes_test.gd,res://tests/game_hud_presenter_declutter_test.gd,res://tests/hud_installed_assets_test.gd -gexit
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ./scripts/package_godot_windows.ps1 -SkipBuild -ExportMode debug
```

The stock reference cases use `hudpos.def`, `weapon.def` and `ammo.def`
from the private reference-assets corpus at
`200a18e83e585a678c5aaecf7fd32229814074fd`. These are separate from the
installed JOTAC definitions exercised by the installed-data suite.

The optional installed-data suite checks the base mount and installed expansions
in order, selecting the first weapon table that carries Javelin. Stock JO needs
Escalation for Javelin and the tank control seats; JOTAC supplies those in its
base mount. It mounts `OPENNOVA_JO_DIR` and stages only
selected definition rows into the existing minimal mission fixture. It
exercises the actual native simulation, HUD presenter, resource loader,
installed fonts and sight textures. It also boards the installed M1A1, T80
and Blackhawk control seats through the public simulation API and checks
the HUD on exit. No game asset files are committed with the tests.

Local installed-data validation covers **JOTAC** and CI's **JO:CA + Escalation**
packed reference at `36571cbaa09501b4740830c5c8bcd91dcabf09c4`, selected through
`OPENNOVA_JO_DIR`. The isolated stock-data rerun passes both installed HUD tests
with 105 assertions. This reproduces and fixes the CI failure caused by mounting
only the base game; no weapon or vehicle assertions were removed. JOTAC's
modified `hudpos.def` is not described as stock JO. Captures use a controlled
background/minimal world, not a synchronized running-original screenshot.
The independent GPU multiply/alpha-test case checks scene preservation
behind scope art. The live Inset test checks its camera, World3D, viewport
bounds, raise/lower/switch lifecycle and declutter independence. Launcher
captures run at 1024×768 and 1920×1080; GPU runs save their readbacks locally
to `user://hud-installed-captures/`. These images are not test goldens or
tracked game assets.

The [tank training follow-up](../world/special-weapons-parity.md#tank-training-right-click-follow-up)
also covers the authored alternate-gun selection. The real presenter updates the
HUD weapon definition and sight card through repeated switches, while each slot
retains its own ammunition (`mounted_weapon_switch_test.gd`, data-driven over
the designated-G carriers the install authors: the stock Escalation Apache and
Ka-52 run on the CI data (76 assertions, headless and D3D12; D3D12 exercises the
actual default RMB binding), JOTAC adds its M1A1 and T80 (151 assertions)). The
native host-loopback regression runs without any data.

## Boundaries

This closes the missing weapon/vehicle display paths above, not every open
HUD item in the project's divergence ledger. The general map-label/overlay
residue in D-HUD-21 and the vehicle-bay/FARP gameplay systems remain separate.
D-HUD-14 is narrowed to menu/system integration; rendering received service
state does not implement the host's rearm service.

The impact preview completes the deterministic trajectory synchronously;
the original allocates a machine-speed-dependent CPU budget after two
benchmark steps. Shared projectile drag retains its existing tumble-model
boundary. Received designation HUD behavior is implemented; creating and
broadcasting designations on an OpenNova host remains the separate gameplay
tracker work beside RoundSim. The optional slot-selection strip is outside
the installed mortar/launcher HUD layout exercised here.

These are source-backed behavior and rendering checks, not a claim of
bit-identical D3D9/Godot rasterization or mixed original/OpenNova live-play
validation. The older synthetic before/after reticle images illustrate the
previous Inset admission fix only; they do not show the new scene pass.

## Review follow-up (2026-09-19)

The #655 review reported seven items the pass had not changed. Each was
confirmed against the original before the fix (the witnesses are in the
[HUD record](hud-re.md#weapon--vehicle-combat-cues-font-slots-the-lollypop-head-the-death-gate-the-inset-scene-witnessed-2026-09-19)
and [net-re 5.61](../net/novaworld-net-re.md)):

- The gear label and the service prompts draw in the large overlay font and
  the Inset friendly name in the bold one; the impact distance is anchored by
  the bold slot's measure of its width and drawn left-aligned in the hudpos
  font. `hud_combat` pins each text's font page, anchor and scale.
- The LollyPop head is a 2:1 ellipse (`hud_combat` pins its 84 x 42 extents).
- The HUD's death gate is the death-screen latch alone; the LollyPop marker,
  the impact distance and the impact preview carry none (`hud_combat`,
  `local_player_view`).
- The 0x0A phase-0 owned-zone mask is walked off the live chain per recipient
  and per frame; the host no longer sends a constant `0x8`
  (`netsim_two_peer_fanout`).
- `mounted_weapon_switch_test.gd` runs the alternate-gun switch on the stock
  Escalation helicopters, so CI's stock data exercises the host-loopback path
  the native `host_role` test also covers (above).
- The Inset aperture draws the world's particles through a second particle
  view group (`particle_renderer_backend_test.gd`: the pair sits ahead of the
  FrameFx terminal, compiles for the Inset eye, leaves the main and mirror
  lists byte-identical, and retires with the scope;
  `game_hud_presenter_declutter_test.gd`: the camera is handed over for
  exactly as long as the pass renders).
- Present rows publish a motor-driven hull's BAM-precise pitch and roll, so
  the drawn cockpit and the carrier-owned eye share one attitude
  (`netsim_present_rows`; `virtual_display_present_test.gd` now allows half a
  millimetre, the float32 rounding of two world points, where it allowed 3 cm).
