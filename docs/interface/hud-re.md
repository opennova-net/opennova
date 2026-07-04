# HUD overlay — reverse-engineering record

Witness record for the Joint Operations in-game **HUD overlay** render pipeline.
Binary: retail **Jointops.exe** (IDB `Jointops.exe.kong.i64`, imagebase
`0x400000`). All addresses below are that binary's.

This is an **engine-research** record: the originals are witnessed but the
OpenNova HUD is not built yet — it is being implemented across these phases:
`libs/def` (`hudpos.def` parse, already present), a `NovaHudPos` GDExtension
binding, host-neutral view helpers under `godot/engine/ui/`, a read-only ONED
preview workspace, and a runtime overlay under `godot/game/`. Each port site
will cite back here as `docs/interface/hud-re.md (D-HUD-…)`. No raw
decompilation is committed; behavior is summarized and cited. No IDB writes were
made this session (one clarifying comment + one rename are **proposals** held
for the maintainer — see end).

The weapon/ammo-coupled HUD elements (ammo count, weapon name, clip graphic,
fire mode, the *spreading* crosshair) are deliberately deferred to a later phase
behind a runtime weapon/ammo model; this record fully witnesses everything that
binds to already-available state, and records the weapon-coupled pieces as
follow-ups.

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| Render pipeline + two-struct model | confirm-only (read-only grill) — port pending | `[orig: HUD_RenderAllOverlays @0x5a8070]` → `[orig: HUD_RenderOverlays @0x5a7bb0]` → element draws; per-frame `[orig: HUD_BuildEntityInfo @0x4b8440]` |
| Virtual coordinate space (1024×768) | confirm-only — port pending | `[orig: Viewport_ScaleToVirtualCoords @0x5d2b20]` exact formula |
| Health bar | confirm-only — port pending (runtime HUD) | `[orig: HUD_DrawHealthBar @0x5a2e50]` rect/fill/threshold-color witnessed |
| Stance indicator (IDB-misnamed "compass") | confirm-only — port pending (runtime HUD) | `[orig: HUD_DrawStanceIndicator @0x599f10]` keyed by stance index; `HUDSTANCE` frames |
| HUD text + half-bright | confirm-only — port pending | `[orig: HUD_DrawTextRightAligned_HalfBright @0x580850]` → `[orig: CGameFont_DrawText @0x6752c0]` |
| Crosshair / reticle (static) | confirm-only — port pending (runtime HUD) | `[orig: HUD_DrawCrosshairCornerQuad @0x590f50]` 5-region UV atlas; spread math `[orig: HUD_DrawCrosshair @0x592640]` deferred |
| Parachute / armor status icons | confirm-only — port pending | `[orig: sub_5925C0 @0x5925c0]` entity-flag gated |
| `hudpos.def` parser token map | confirm-only (cross-checked vs `libs/def`) | `[orig: loc_59F370]` registered by `[orig: HUD_InitOverlaySystem @0x5a4620]` |
| Radar / minimap (top-down map) | **not yet witnessed** — follow-up | `[orig: draw_minimap_overlay @0x599700]` located only |
| Crosshair spread (weapon-coupled) | **deferred** (weapon/ammo phase) | formula recorded below; needs the weapon model first |

## Render pipeline — the two-struct model

The HUD keeps **layout** (parsed once) separate from **per-frame state** (rebuilt
each frame); the draw code reads both.

- **Layout globals** — parsed once at load from `hudpos.def` by the parser
  callback `[orig: loc_59F370]` (an undefined `loc_` blob, a `_stricmp`
  token-dispatch chain), registered via `File_ParseASCIIFile("hudpos.def", …)`
  inside `[orig: HUD_InitOverlaySystem @0x5a4620 @0x5a4931]`. The globals occupy
  `dword_27237xx … dword_2723Bxx` (positions/colors/texture-name strings).
- **Per-frame entity-info struct** — base `dword_2723388`, size `0x240` = **576
  bytes**, rebuilt every frame by `[orig: HUD_BuildEntityInfo @0x4b8440]` from
  the local player. The global `entityA @0x27234e8` is `dword_2723388 + 352` —
  i.e. the info struct's "current entity" slot.
- **Dispatch** — `[orig: HUD_RenderAllOverlays @0x5a8070]` (top, per frame) →
  `[orig: HUD_RenderOverlays @0x5a7bb0]` (element sub-dispatcher) → the
  per-element draws. Each element is gated by an enable flag:

  | Flag | Element |
  |---|---|
  | `dword_2723C9C` | health bar |
  | `dword_2723CCC` | objective status text (`[orig: draw_objective_status_text @0x59aa30]`) |
  | `dword_2723CD4` | game timer + score |
  | `dword_2723CA0` | weapon/ammo + stance-indicator + ammo-indicator cluster |
  | `dword_2723C90` | altitude / power bar |
  | `dword_2723CD0` | weapon reload bar |
  | `dword_2723CD8` | weapon slot bar |

  A spectator path (`g_death_screen_active`) rebuilds the info for the *spectated* entity
  and restores: `qmemcpy(tmp, &dword_2723388, 0x240)` → `HUD_BuildEntityInfo` →
  `qmemcpy(&dword_2723388, tmp, 0x240)` `[orig: @0x5a7bf1]` — pinning the 576-byte
  size.

## Virtual coordinate space — 1024×768

All `hudpos.def` positions are authored in a **1024×768** virtual design space
and scaled to the actual screen by `[orig: Viewport_ScaleToVirtualCoords @0x5d2b20]`:

```
out_x = (design_x * screen_w + 512) / 1024
out_y = (design_y * screen_h + 384) / 768
```

`screen_w/screen_h` come from the overlay context `overlayCtx @0x24c1420`. The
`+512`/`+384` are round-to-nearest. Every element scales its rect this way
before drawing. (Matches the 1024×768 design space the oscarmike reference used.)

## Per-frame info struct — `HUD_BuildEntityInfo @0x4b8440`

Field offsets into `dword_2723388` that the bindable (non-weapon) elements read.
This is the model the OpenNova runtime "HUD info" gatherer mirrors.

| Offset | Meaning | Source |
|---|---|---|
| +352 | current entity ptr (= `entityA`) | the local/spectated player |
| +92 | **health fraction** 16.16, capped `0x10000` | `(entity+286 currentHealth << 16) / maxHealth` |
| +374 | **team** byte | `entity+354` |
| +568 | **stance index** | `entity+300 &0x200→1`, `&0x100→2`, else `0`; vehicle/mounted→`3`; parachute `entity+36 &0x20→5` |
| +72 | horizontal **speed** | `300 * sqrt((entity+156>>8)² + (entity+152>>8)²) / 585` |
| +364 | distance to target/cam | sqrt of delta, `→int` |
| +368 | entity name string | `GameText_GetString("item", weapondef+1318)` else `"text_default"` |
| +60 | weapon heat (capped `0xFFFF`) | weapon-coupled — Phase: weapon/ammo |
| +52 / +56 / +64 | ammo / clip / ammo-type | weapon-coupled — Phase: weapon/ammo |

`maxHealth` = `[orig: Entity_GetMaxHealthWithDifficulty @0x43b8a0]`. The entity
layout offsets (`+286` health, `+354` team, `+300` posture flags, `+36` flags)
agree with the GamePlayerEntity map in `docs/net/novaworld-net-re.md`.

## Element witness map

### Health bar — `HUD_DrawHealthBar @0x5a2e50`

- Layout rect = `dword_27237C8/CC/D0/D4` (x1,y1,x2,y2). All four zero ⇒ element
  disabled (early return). Scaled to screen via `Viewport_ScaleToVirtualCoords`.
- **Fill**: width = `(barWidth * dword_27233E4 + 0x8000) >> 16`, where
  `dword_27233E4` = `dword_2723388 + 92` = the per-frame health fraction (16.16).
  Drawn as a filled rect from `(x1+1, y1+1)` to `(x1+fillWidth, y2)`
  (`[orig: sub_5D48E0 @0x5d48e0]`) — fills **left → right**, 1px inset.
- **Border**: `[orig: Render_DrawWireframeRect @0x5d4760]` over the full rect,
  color `dword_27237C4` (the `HEALTHBORDER` color).
- **Fill color** by a freshly recomputed `(currentHealth << 16)/maxHealth`
  (currentHealth = `entity+286`): `> 0xC000` (0.75) → `dword_2723ADC` (good);
  `> 28671` (`0x6FFF` ≈ 0.437) → `dword_2723AE0` (mid); else `flt_2723AE4` (bad).
  (Note D-HUD-4: the *fill width* uses the capped `+92` ratio while the *color*
  uses an uncapped recompute — equivalent in range, recorded for fidelity.)

### Stance indicator — `HUD_DrawStanceIndicator @0x599f10` (IDB name `draw_minimap_compass_overlay` is a misnomer; D-HUD-1)

Renders the **stance** icon, keyed by `byte_27235C0` = `dword_2723388 + 568` =
the stance index from `HUD_BuildEntityInfo`. It is **not** a compass.

- Frames defined by `hudpos.def` token **`HUDSTANCE <idx> <xoff> <yoff> <texname>`**
  `[orig: loc_59F370 @0x5a0b4d]`: `xoff→dword_2723B24[idx]`, `yoff→dword_2723B44[idx]`,
  `texname→byte_2723B8C + idx*0x13` (19-byte stride). Texture handles loaded into
  `dword_27239E4[idx*4]`, dims `dword_27239E8[]/EC[]`.
- Anchor = `HUDSTANCEPOS → dword_2723AEC (x) / dword_2723AF0 (y)`
  `[orig: loc_59F370 @0x5a0be7]`; vehicle stance anchor
  `HUDVEHSTANCEPOS → dword_2723AF4/AF8`.
- Each frame scaled into a 128px box: `scale = 0x800000 / max(w,h)`,
  `scaled = (scale*dim + 0x8000) >> 16`, centered via `(128-scaled)/2`.
- **Cross-fade**: current stance (`byte_27235C0`) drawn over the previous
  (`byte_2723D3C`) with alpha ramped over `dword_272361C` ticks; base alpha
  `dword_2723614`; tint `dword_2723AE8`. In an MP session a team tile (index 6/7
  from `entityA+354`) is drawn underneath.
- Stance indices: `0`=stand, `1`/`2`=crouch/prone (from `entity+300` `&0x200`/`&0x100`;
  exact bit↔posture to confirm vs the infantry stance flags in
  `docs/world/world-wac-ai-re.md`), `3`=vehicle/mounted, `5`=parachute.

### HUD text + half-bright — `HUD_DrawTextRightAligned_HalfBright @0x580850`

- Body: `if (*font_info) CGameFont_DrawText(*font_info, (float)x, (float)y, color, text)`
  `[orig: CGameFont_DrawText @0x6752c0]`. `font_info` points at a font handle.
- **Half-bright** = `(color >> 1) & 0x7F7F7F | 0xFF000000` — halve each RGB
  channel, force opaque alpha.
- **Right alignment** is the caller's job: it measures with
  `GameFont_MeasureTextWidth` and subtracts before calling.
- Fonts are `font_hi` / `font_lo` named in `hudpos.def` (`.fnt` bitmap fonts).
  Glyph layout/spacing inside `CGameFont_DrawText` (D3D vertex build) is a
  follow-up; the OpenNova port uses `NovaFntResource` (already parses the `.fnt`
  glyph atlas) for the glyphs.

### Crosshair / reticle — `HUD_DrawCrosshairCornerQuad @0x590f50`

The reticle texture is split into **5 regions** drawn by 5 calls (cornerIndex
`0`=top, `1`=bottom, `2`=left, `3`=right, `4`=center):

- UV atlas boundaries **0.0 / 0.45 / 0.5 / 0.55 / 1.0** (center occupies the
  0.45–0.55 band; arms the outer bands).
- Arms taper by factor **0.1** (mid-edge pulled in by `0.1 × half-extent`).
- Each vertex: `rhw = 0.9`, `diffuse = 1.0`, `specular = color`; emitted via
  `[orig: GDynamicVB_DrawPrimitive @0x6788e0]` as a triangle-strip — **5 verts**
  for the arms, **4 verts** for the center.
- Caller `[orig: HUD_DrawCrosshair @0x592640]` offsets each region's center by
  the spread amount: top `(x, y−off)`, bottom `(x, y+off)`, left `(x−off, y)`,
  right `(x+off, y)`, center `(x, y)`. With **spread = 0** the regions assemble
  the full crosshair at center — directly implementable now. The spread offset
  itself is weapon-coupled (see follow-ups).

### Parachute / armor icons — `sub_5925C0 @0x5925c0`

- Parachute: `entityA flags &0x10` → draw `ParachuteIcon` at `(x@0x272383C, y@0x2723840)`,
  handle `dword_27239A4`.
- Armor: `entityA flags &8` → draw `ArmorIcon` at `(dword_2723844, dword_2723848)`,
  handle `dword_27239B4`.
- The static HUD frame background is `STATICFRAME` → pos `dword_2723B1C/B20`,
  name `byte_2723C24`, drawn at the top of `HUD_RenderOverlays`
  (`[orig: draw_textured_quad_with_border @0x590c40]`).

## `hudpos.def` parser token → global map — `loc_59F370`

A `_stricmp` token-dispatch; each token reads decimal fields via `atof → ftol`
(1024×768 ints) or copies a texture-name string. Witnessed tokens (cross-check
the exact set + the spinmap/radar tokens against `DefHudPosDef` in
`libs/def/include/def/def.h`, which already parses this file):

| Token | Writes |
|---|---|
| `HEALTHPOS` | `dword_27237C8/CC/D0/D4` (x1,y1,x2,y2) |
| `HUDSTANCE <idx> <x> <y> <tex>` | `dword_2723B24[idx]`, `dword_2723B44[idx]`, `byte_2723B8C+idx*0x13` |
| `HUDSTANCEPOS` | `dword_2723AEC` (x), `dword_2723AF0` (y) |
| `HUDVEHSTANCEPOS` | `dword_2723AF4` (x), `dword_2723AF8` (y) |
| `STATICFRAME` | name `byte_2723C24`, pos `dword_2723B1C/B20` |
| `ParachuteIcon` | name `byte_2723C34`, pos `0x272383C/0x2723840` |
| `ArmorIcon` | name `byte_2723C44`, pos `dword_2723844/0x2723848` |
| `AMMOCOUNTPOS`, `HUDWEAPONNAME`, `HUDTIMECLOCK`, `mapcoords`, `HUDPOWERBAR` | recon-confirmed token set (weapon/timer/map — witness when those elements land) |

## Divergence catalog (D-HUD)

| ID | Ours / reference | Original (Jointops.exe) | Why / consequence |
|---|---|---|---|
| D-HUD-1 | IDB curated name `draw_minimap_compass_overlay`; the oscarmike reference models a "spinmap" compass | `HUD_DrawStanceIndicator @0x599f10` renders the **stance** indicator, keyed by `byte_27235C0` = `hudInfo+568` stance index | The function is mis-named in the IDB and mis-modeled in oscarmike. The OpenNova stance widget must be the discrete cross-faded `HUDSTANCE` frames, not a compass. Rename proposed (held). |
| D-HUD-2 | oscarmike `spinmap.gd` = a single rotating compass-ring texture | the stance widget is discrete pre-rendered frames cross-faded on stance change; the **heading/north** display is a *separate* top-down radar (`draw_minimap_overlay @0x599700`) | Do not port a rotating ring. Stance = frame swap with fade; heading = radar (follow-up). |
| D-HUD-3 | — | Design space is fixed **1024×768**, scaled with round-to-nearest (`Viewport_ScaleToVirtualCoords @0x5d2b20`) | OpenNova authors HUD positions in 1024×768 and scales to the actual surface with the `(p*s+½s)/dim` rounding. |
| D-HUD-4 | — | Health bar *fill width* uses the capped `+92` ratio; *fill color* uses an uncapped recomputed ratio (`HUD_DrawHealthBar @0x5a2e50`) | Equivalent over `[0,1]`; recorded so the port matches both reads rather than collapsing to one. |

## Follow-ups (not yet witnessed / deferred)

- **Radar / minimap** `[orig: draw_minimap_overlay @0x599700]` — the top-down map
  + blips + heading; the `HUDSPINMAP X1/Y1/X2/Y2` token → its bounds. Located,
  not yet witnessed. Needed for the "compass/heading" HUD element.
- **Heading source** — the `byte_2723D3*`/heading state feeding the radar
  (computed in `HUD_RenderAllOverlays` via `Math_FixedPointTransformPoint22` +
  `sub_590970`/`sub_59A9E0`).
- **Team color table** — the team→color mapping used for text/labels (the stance
  team tile uses `entityA+354`; the text color table is unwitnessed).
- **CGameFont glyph layout** `[orig: CGameFont_DrawText @0x6752c0]` — per-glyph
  D3D vertex build / spacing, to confirm `NovaFntResource` layout parity.
- **Weapon-coupled crosshair spread** `[orig: HUD_DrawCrosshair @0x592640]`:
  `spread = (ERROR[weapon+0xB0 indexed by stance/scope] + (player+0x380 >>7) +
  (player+0x384 >>7)) × fov_scale`, pixel offset `= spread*screen_w/fov_scale >>16`.
  Blocked on the runtime weapon/ammo model; witness fully when that phase starts.
- **Ammo / weapon-name / clip / heat** elements (`hud_draw_weapon_ammo_and_name
  @0x5939d0`, `draw_hud_ammo_indicator @0x599a30`) — same weapon/ammo dependency.

## IDB changes proposed this session (held — not applied)

No IDB writes were made (the shared-state policy gated them as propose-first).
For the maintainer to apply if desired:

- **Rename** `draw_minimap_compass_overlay @0x599f10` → `HUD_DrawStanceIndicator`
  (anchored: indexes `HUDSTANCE` frames by `byte_27235C0 = hudInfo+568` stance
  index; D-HUD-1).
- **Comment** at `0x599f10` recording the misnomer + the stance-index keying and
  the real radar at `0x599700` (the text is in D-HUD-1 above).
