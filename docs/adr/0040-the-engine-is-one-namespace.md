# ADR 0040: the engine is one namespace — no Nova prefix, files follow classes, group-qualified includes

- **Status**: accepted (2026-08-26; maintainer directive)
- **Updated**: [ADR 0042](0042-godot-permanent-shell-one-mission-kernel.md)
  (2026-08-28) fixed decision 6's consumer list (the "any future non-Godot
  front-end" clause is gone) and reframed ladder rows A3 and B (remainder) as
  the mission-kernel / listen-host slices.
- **Owners**: engine layout, build topology, the Godot binding layer
- **Supersedes/updates**: ADR 0034 decision 5's deferral ("file names keep
  their `nova_` prefix for now"); ADR 0029's and ADR 0024's include-root
  clause ("each GROUP directory is the one public include dir, so
  `#include <domain/file.h>` resolves"); ADR 0034 §2's list of the engine's
  non-Godot consumers. ADR 0020's terrain seam, ADR 0029's five group
  targets and PUBLIC chain, and ADR 0033/0034's boundary rule stand.

## Context

This is a port, but everything we build is a feature of *our* engine. There is
no separate "Nova layer" wrapping something else, so nothing in the tree earns
a `Nova`/`nova_` prefix: not the registered classes (retired in ADR 0034 d5),
not the files (deferred there), not the GDScript aliases and autoloads, not the
`NOVA_*` macros and guards, not the shader helper functions, and not the
C-linkage `gameprofile` records. The survivors are the words that are proper
nouns in their own right — `NovaWorld*` (the matchmaking service we speak the
wire protocol of), `NovaLogic` (the vendor), `opennova`/`opennova_*` (the
project).

Finishing the file rename exposed a real design flaw rather than a naming
nit. The engine's public include namespace was FLAT: each group target
exported its own directory as an include root, so `<mission/x.h>` was
resolved by CMake link order across four roots. The engine already aliased
itself — `formats/mission` vs `runtime/mission`, `formats/particle` vs
`runtime/particle`, `formats/wac` vs `runtime/wac` — surviving only because
the file names inside the twins happened not to collide. `godot/src`, whose
subdirectories mirror engine lib names and whose root is searched before the
engine roots, would have gained five exact path twins the moment `nova_` was
stripped (`audio/ambient_mixer.h`, `audio/sound_selector.h`,
`devtools/oned_ui.h`, `mission/mission_catalog.h`, `particle/effect_scene.h`
— each a binding whose class name equals the engine type it wraps, which is
the thin-seam ideal, not a mistake). The `nova_` prefix had been silently
carrying the uniqueness load; nothing enforced it.

## Decision

1. **No Nova prefix anywhere.** Files, identifiers, macros, header guards,
   GDScript `class_name`s, preload aliases, autoloads (`MusicService`),
   shader helper functions (`scene_output`, `terrain_fog_factor`, ...), and the
   C-linkage `gameprofile` set (`GameProfile`, `GameId`, `BootPhase`,
   `RequiredResource`, `ResourceSeverity`; enumerators `GAME_*`,
   `BOOT_PHASE_*`, `RES_*`). Survivors: `NovaWorld*` classes and the
   `novaworld_*` files that carry them (one proper noun, spelled like
   `engine/net/novaworld/` and `apps/novaworld_server/`), `NovaLogic`,
   `opennova`. "`nova_` never starts a file name" is now a lintable
   property.
2. **A file is named after the primary type it declares**, in snake_case;
   translation-unit splits keep `<class>_<facet>` (`simulation_present.cpp`).
   The class name, not the old file name, wins where they differed
   (`PffDocument` -> `pff_document.*`, `FrameFx` -> `frame_fx.*`,
   `WindowState` -> `window_state.gd`).
3. **`engine/` is the one public include root, and includes are
   group-qualified.** Every engine header is included as
   `#include <group/lib/file.h>` with group in {`base`, `formats`, `runtime`,
   `net`}: `<runtime/world/player_view.h>`, `<formats/mission/mission.h>`,
   `<base/vfs/vfs.h>`, `<net/npwire/peer_addr.h>`. Same-directory siblings
   may keep a bare `"file.h"`; every cross-lib include is the angle form.
   Each group target exports `engine/` PUBLIC. The group in the path is what
   makes the layer visible at the include line.
4. **The layering is a lint, read off the include path.**
   `scripts/lint/include_graph_check.py` (a PR gate) enforces: the ADR 0029
   d3 group order (`base/io` and `base/crt` include only each other;
   `formats` includes `base/io`, `base/crt`, `formats`; `base` includes
   `base`, `formats`; `runtime` adds `runtime`; `net` includes all four); no
   unqualified engine include anywhere under `engine/`, `apps/`, `tests/`,
   `godot/src`; the ADR 0020 terrain seam under its qualified prefixes; no
   include naming `godot` under `engine/`, `apps/` or `tests/`; and no
   `godot/src` subdirectory named like an engine group.
5. **Bindings keep `godot/src` as their include root** with quoted
   root-relative includes (`"audio/ambient_mixer.h"` is the binding,
   `<runtime/audio/ambient_mixer.h>` the engine type it wraps). Because no
   `godot/src` subdirectory may be named `base`, `formats`, `runtime` or
   `net` (decision 4), a binding path can never alias an engine path, and a
   binding may share its class name and directory name with the engine
   concept it exposes — the thin typed seam ADR 0034 asks for.
6. **The engine's portability has an honest consumer list.** ADR 0034 §2
   named an importer FFI and DCC plugins; those left with ADR 0037/0038 (no
   `oned_edit`, no shared-library export, no ctypes/pybind11). What consumes
   `engine/` without Godot today: the six `apps/` (the NovaWorld service,
   the in-match dev host, the LAN probe, the packet pretty-printer, the
   mounted-install entry extractor `opennova-extract` added by ADR 0041, the
   shared socket helpers), the ctest suite, and the C-linkage headers.
   `engine/` stays Godot-free for them — now as
   a ratcheted property (decision 4), not a re-verified one.

## Consequences

- One rule for a reader: an angle include with a group prefix is the engine,
  a quoted root-relative include is the binding tree, a bare name is a
  sibling. The twin lib names (`mission`, `particle`, `wac`) stop aliasing.
- The `adapter_cpp_orig_cites_*` [since ADR 0042 d7 the one `adapter_cpp_orig_cites`],
  `gd_orig_cites`, and size ratchets key on
  paths and survive the rename by rewriting their baselines in the same
  change; the maturity instruments are otherwise untouched.
- Historical records keep their historical names where they describe a past
  state; paths that name a current file are updated.
- No compatibility aliases: every consumer is rewritten in the same change
  (pre-1.0 policy).

## Ladder: the push-down campaign

With the names and the include roots settled, the witnessed behavior still
living Godot-side moves to an engine home. The gauges are
`adapter_cpp_orig_cites` (one `[orig:` count over all of `godot/src`, ADR 0042
d7) and `gd_orig_cites` (the same markers under `godot/game` + `godot/modtools`
+ `godot/probes`), both in
`scripts/lint/maturity_baseline.json`: a slice moves code, banks the counter,
and keeps fidelity, because the cites travel with the code they cite.

The campaign runs one commit per slice, and **this table is its queue.** The
ranked list originally written into `TODO.md` was lifted back out at
`77c025a2d` (the maintainer's call for this campaign: run it in the PR, not as
a TODO queue), so the remainder is tracked here and nowhere else. The ladder is
finished when both counters hold only documented seam contracts, not when they
reach any particular number.

| Slice | What moves | Engine home | Status |
|---|---|---|---|
| A1 | the local player's view cluster | `engine/runtime/world/local_player_view.{h,cpp}` | LANDED `4a7195299` |
| A2 | the minimap marker rows and their feed layout (duplicated at `simulation.h` and `hud/hud_overlay.cpp`) | `engine/runtime/inmatch/minimap_markers.{h,cpp}` + `engine/runtime/hud/hud_minimap_feed.{h,cpp}` | LANDED `61bfa5efa` |
| A3 | `simulation.cpp`'s `advance_world_tick` / `boot_mission` / `restore_world_baseline` | `engine/runtime/mission/mission_kernel` + `engine/runtime/inmatch/listen_host` (ADR 0042) | LANDED `972c774de` + `6510891bb` |
| A4 | the LAN browse/announce cadence and its constants | `engine/runtime/inmatch/lan_discovery.{h,cpp}` | LANDED `a5ca3f3c9` |
| A5a | the boot, pack and options policies leave the GDScript | `engine/base/resource_index/boot_policy.{h,cpp}`, `engine/base/vfs/pack_policy.h`, `engine/runtime/menu/options_policy.h` | LANDED `c33b15f46` |
| A5b | the seat mirror, the volume law and the weapon-category rows | `engine/runtime/world/vehicle_attach.{h,cpp}`, `engine/runtime/audio/volume_law.h`, `engine/runtime/controls/controls.{h,cpp}` | LANDED `6e34d2e2a` |
| B3a | the occlusion frame camera build | `engine/runtime/world/occlusion_camera.h` | LANDED `c2e41b97c` |
| B3b | the iris exposure march | `engine/runtime/world/iris_march.{h,cpp}` | LANDED `97fb7a83b` |
| B (remainder) | `simulation_present.cpp`'s data model, `simulation_net.cpp`'s GameConfig policy + pumps, `simulation_player_loadout.cpp` / `simulation_player_weapon.cpp` / `simulation_player.cpp`, `simulation_assets.cpp`, the feed marshallers, then the `simulation.h` state-model split, after which `Simulation` is a `TickTarget` adapter — reframed by ADR 0042: the rig-twinned half moves into the kernel; the Godot-only witnessed blocks stay binding-side until grilled | the rig-twinned half: `mission_kernel` + `listen_host` | PART-LANDED `6510891bb` (the boot/tick/player/table bodies); the Godot-only witnessed blocks (present rows, sun feed, scar gate, the joiner pump) stay binding-side ON-TOUCH after a grill |
| B1 | `Simulation::bringup_host_runtime` re-implements `listen_host::bringup` (the same `SinglePlayer_StartMission @0x561af0` body twice): parameterize `listen_host::bringup(kernel, state, ListenBringupOptions{host_cfg, socket_mode, serve_and_play, local_character_vars, mission text/til installs})`, adopt `state.client_runtime` as the HostClient runtime, and make the binding a converter | `engine/runtime/inmatch/listen_host` | QUEUED (2026-08-29 hygiene census C-01; proven by the retail-interop recipe + the nw_pp pcap diff, not GUT) |
| B2 | the deploy-screen zone rows and status composed in `simulation_net.cpp` (`get_deploy_spawn_zones`, `UI_UpdateDeathScreenContent @0x5536a0`) and re-parsed from their own Dictionaries | `engine/runtime/world/deploy_screen_feed` (`build_deploy_zone_rows` + a typed `DeployZoneRow` record) | QUEUED (census C-04) |
| B3 | the kill-feed policy composed in `drain_feed_events` (camp detection, the own/verbose gate, the STRCND48 bonus recompose) | `engine/runtime/hud/feed_format` (`feed_event_rows` + a typed `FeedRow` record) | LANDED `fbd04a8e6` |
| B4 | the local-player view/aim frames and the character profile still crossing as Dictionaries (`get_local_player_view`, `get_local_player_aim_overlay`, `MissionSetupOptions.local_character_profile`) | typed `LocalPlayerViewFrame` / `AimOverlay` / `CharacterProfile` records assigned from the engine structs | QUEUED (census C-15, C-22) |
| C1 | the mission load plan (`Game_StartMission`'s sequence + progress schedule) | `engine/runtime/mission/mission_load_plan.h` | LANDED `d081d2908` |
| C2 | the HUD presenter's config tokens and feed units | `engine/runtime/hud/hud_config_tokens.h` | LANDED `d8fa149b2` |
| C3+C4 | the presenter's swizzles/rangefinder and the viewmodel rig's frame math | `engine/runtime/world/presentation_frame.h` + `engine/runtime/simassets/fp_viewmodel_spec.h` | LANDED `0b07c5f9f` |
| C5 | the light director's spawner constants | `engine/runtime/renderer/light_scene.h` | LANDED `89440caf7` |
| C6 | the loading screen's names, sidecar and due rules | `engine/runtime/hud/loading_screen.h` | LANDED `4c8a38c6a` |
| C7 | the character registry and the joiner profile | `engine/runtime/inmatch/character_registry.{h,cpp}` + `engine/runtime/inmatch/join_character_profile.h` | LANDED `eb9fa8c69` |
| C8 | `godot/src/audio/mission_audio.cpp`'s reverb, which is an invented approximation and so cannot stand as written: port `Audio_LoadReverbDefs @0x766d80` or ledger the divergence | `engine/runtime/audio` | QUEUED |
| C9 | `godot/src/world/session_drive.cpp` (ex net_session_drive.gd, ADR 0043 slice G10), plus the `destruction_presenter.cpp` / `fire_presenter.cpp` passes (the item-effect director's law landed in `engine/runtime/world/item_effects.{h,cpp}`, ADR 0043 slice G6) | to be chosen per slice | QUEUED |
| C10 | the player-info and armory companions (`godot/game/player_info_menu_companion.gd`, `godot/game/world/armory_menu_companion.gd`), mostly wired-doc cites rather than behavior | to be chosen per slice | QUEUED |
| D | `godot/src/object/object_model_anim.cpp` + the `skeletal_anim` slot machine to `engine/runtime/anim`; the `item_database` / `weapon_database` doc blocks to their engine homes; the CTRL store + PANM cache; `godot/src/env/weather.cpp`'s mission-start boundary; `godot/src/particle/particle_renderer.cpp`'s `lit_primary_color`; `godot/src/env/water_core.cpp`'s view builder; `godot/src/terrain/terrain.cpp`'s normal + quadrant policy | to be chosen per slice | QUEUED |
