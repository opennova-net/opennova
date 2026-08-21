# ADR 0036 - Hard-cut rendering facelift

Status: accepted (maintainer, 2026-08-20). Supersedes ADR 0023 decision 1 for
OpenNova's current Godot presentation after the final fixed-function parity
baseline at `b2ff3369fcef74ca6c77ee2f4c2dd53eb0d2bdd8`.

## Context

The REN track established and verified the canonical rendering-data contract:
geometry, material tags, texture slots, animation generators, coverage, blend,
cull and order semantics, environment inputs, and authored light records. That
work made the decoded data trustworthy enough to change its presentation without
converting, rewriting, or requiring replacements for retail source assets.

The fixed-function emulation is deliberately not retained as a selectable mode.
Maintaining two complete shader, light and capture paths would add permanent
surface area without advancing the intended renderer.

## Decision

1. The Godot device leg adopts a restrained modern relight as a hard cut. Ordinary
   surfaces use Godot's native lit spatial pipeline and native light nodes. The
   sun uses a shadow-casting `DirectionalLight3D`, authored and transient lights
   use pooled native clustered `OmniLight3D`/`SpotLight3D` nodes, and exposure is
   fixed Filmic presentation policy. Water, glass, sky, particles and
   ordering-sensitive effects may retain custom shaders.
2. Canonical data semantics do not change. Parsers, geometry, texture-slot roles,
   material classification, UV/RGB/alpha animation, coverage, blend, cull, draw
   order, LOD, visibility and ENV/TOD inputs remain the renderer interface.
3. Existing color, detail and normal textures are consumed only in their authored
   roles. Missing metallic, roughness and AO maps are not inferred. Metallic is
   zero and roughness is presentation policy selected from the existing material
   technique. Optional presentation sidecars or caches may be added later, but
   they must remain derived overrides: source assets and their canonical decoded
   semantics stay sufficient and unchanged.
4. Authored and transient light records retain their decoded identity, transform,
   color animation, range and lifetime, but the fixed-function nearest-four
   selection and lighting equation are not compatibility requirements.
   Retail render-slot planning and decoded shadow-admission semantics may remain
   as a portable oracle, but the Godot `SlotShadow`/terrain-drape device does not
   ship. Native directional shadows replace it. The retail fullscreen sun veil
   and exposure stop-down likewise remain research history, not a second live
   presentation path.
5. The existing 18-fixture registered retail catalog,
   `docs/render/render-fixtures-retail-v4.json`, is recaptured unchanged as the
   comparison instrument. Retail pixels are a stable reference, not a facelift
   pixel target; numeric deltas remain descriptive and acceptance is
   scene-by-scene review.
6. The presentation target is a restrained high-end desktop relight at 2560x1440
   and 60 frames per second. The registered comparison catalog remains 2000x1200
   so its existing poses and evidence contract do not change.

## Consequences

- ADR 0023's material/state vectors remain permanent semantic regression tests.
  Shader hashes and pixel baselines change intentionally.
- There is one shipping renderer and no runtime render-profile interface.
- The final parity publication remains available through Git history. The current
  tracked publication represents retail-versus-facelift comparisons.
- Presentation tuning belongs in the Godot device leg. Asset formats and portable
  simulation do not grow PBR fields or Godot dependencies.
