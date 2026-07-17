# HUD overlay — reverse-engineering record

Witness record for the Joint Operations in-game **HUD overlay** render pipeline.
Binary: retail **Jointops.exe** (IDB `Jointops.exe.kong.i64`, imagebase
`0x400000`). All addresses below are that binary's.

Implementing code: `libs/def` (`hudpos.def` / `weapon.def` parsing), the
`NovaHudPos` GDExtension binding (`godot/engine/hud/`), the host-neutral view
helpers under `godot/engine/ui/hud_*.gd`, the ONED preview workspace
(`godot/modtools/hud/`), and the runtime overlay `godot/engine/world/game_hud.gd` fed
per-frame by `godot/game/main_game.gd`. The 2026-06-22 session witnessed the
core pipeline read-only; the 2026-07-09 session witnessed the weapon-coupled
elements and ported them (the weapon FSM of net-re §5.62 supplies the live
clip/reserve/ADS state); the 2026-07-11 re-grill (the extraction-train slice
gate) fresh-decompiled every ported function, fixed seven port divergences
in-source (ALPHAFADE atof, positional 4-field parse, stance frame-0 shared
scale + gates, capacity-1 reserve fold, triggered-text white, mission-bin
exists-gate, divisor byte wrap) and minted D-HUD-9/-10. No raw decompilation
is committed; behavior is summarized and cited. The 2026-07-11 session applied
two auto-name renames + two comments to the IDB (logged at the end); the held
curated-name proposals (D-HUD-1 `HUD_DrawStanceIndicator` and the crosshair
globals comment) were applied 2026-07-16, along with the scope-circle rename
`Hud_DrawScopeCircleMask @0x5d17a0` (logged at the end).

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| Render pipeline + two-struct model | confirm-only (read-only grill) | `[orig: HUD_RenderAllOverlays @0x5a8070]` → `[orig: HUD_RenderOverlays @0x5a7bb0]` → element draws; per-frame `[orig: HUD_BuildEntityInfo @0x4b8440]` |
| Virtual coordinate space (1024×768) | ported (`hud_layout.gd`) | `[orig: Viewport_ScaleToVirtualCoords @0x5d2b20]` exact formula; `hud_helpers_test.gd` |
| Health bar | ported (`hud_health_bar.gd`) | `[orig: HUD_DrawHealthBar @0x5a2e50]` rect/fill/threshold-color; `hud_helpers_test.gd` thresholds |
| Stance indicator + cross-fade (IDB-misnamed "compass") | ported (`hud_stance.gd` + `hud_fade.gd` + `game_hud.gd`) | `[orig: HUD_DrawStanceIndicator @0x599f10]` full witness incl. fade pair + per-frame offsets; `hud_helpers_test.gd` fade curve |
| HUD text + half-bright | ported (`hud_text.gd`) | `[orig: HUD_DrawTextRightAligned_HalfBright @0x580850]` → `[orig: CGameFont_DrawText @0x6752c0]` |
| Ammo count + weapon name text | **ported** (`hud_weapon_text.gd`) | `[orig: hud_draw_weapon_ammo_and_name @0x5939d0]`; format/hide/alignment/nudge witnessed; `hud_helpers_test.gd` format_ammo |
| Clip + rounds indicator (HUDCLIPGFX/HUDRNDGFX) | **ported** (`hud_clip_indicator.gd`, D-HUD-5) | `[orig: draw_hud_ammo_indicator @0x599a30]`; parse `[orig: @0x5442fc]`; `hud_helpers_test.gd` round_icon_count + flash |
| Crosshair / reticle + spread | **ported** (`hud_crosshair.gd`, D-HUD-7/8/9/10; target cursor / aim-point quad / lock brackets unported) | `[orig: HUD_DrawCrosshair @0x592640]` + `[orig: HUD_DrawCrosshairCornerQuad @0x590f50]`; spread math `hud_helpers_test.gd` |
| ALPHAFADE semantics | **ported** (`hud_fade.gd`) | `[orig: parse @0x5a086c]` ×2.55/×2.55/×62; flash curve `[orig: @0x599af9]`; `hud_helpers_test.gd` |
| Mission triggered text (WAC/BMS `text`) | **ported** (`hud_messages.gd` + `main_game.gd`, D-HUD-6) | `[orig: HUD_DisplayTriggeredText @0x51f190]` → `[orig: Chat_AddDebugMessage @0x4987f0]`; `hud_helpers_test.gd` expiry |
| `hudpos.def` parser token map + 4-field positions | ported (`libs/def`) | `[orig: loc_59F370; AMMOCOUNTPOS @0x59fc3d]`; ctest `def_parse_hudpos` |
| Parachute / armor status icons | confirm-only — port pending (needs entity flags) | `[orig: sub_5925C0 @0x5925c0]` entity-flag gated |
| MP objective status text + team tile | confirm-only — MP HUD phase | `[orig: draw_objective_status_text @0x59aa30]` client/strcli* strings witnessed |
| Radar / minimap (top-down map) | **not yet witnessed** — follow-up | `[orig: draw_minimap_overlay @0x599700]` located only |

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
  | `dword_2723CB4` | crosshair cluster (read in `HUD_DrawCrosshair @0x592757`) |
  | `dword_2723C90` | altitude / power bar |
  | `dword_2723CD0` | weapon reload bar |
  | `dword_2723CD8` | weapon slot bar (`[orig: HUD_DrawWeaponSlotBar @0x599cd0]`, unwitnessed) |

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
before drawing. The inverse (`[orig: Viewport_ScreenToVirtual @0x5d2c70]`,
×1024/width) maps screen points (e.g. the crosshair's screen center) back into
the design space. (Matches the 1024×768 design space the oscarmike reference used.)

## Per-frame info struct — `HUD_BuildEntityInfo @0x4b8440`

Field offsets into `dword_2723388` that the ported elements read.
This is the model the OpenNova runtime "HUD info" gatherer mirrors
(`main_game.gd _update_game_hud`).

| Offset | Meaning | Source |
|---|---|---|
| +352 | current entity ptr (= `entityA`) | the local/spectated player |
| +552 | **weapon def ptr** (`dword_27235B0`) | `MountSlot+32`; every weapon element gates on it |
| +556 | weapon MountSlot ptr | `entity+280` |
| +92 | **health fraction** 16.16, capped `0x10000` | `(entity+286 currentHealth << 16) / maxHealth` |
| +374 | **team** byte | `entity+354` |
| +568 | **stance index** | `entity+300 &0x200→1 (crouch)`, `&0x100→2 (prone)`, else `0`; vehicle/mounted→`3`; parachute `entity+36 &0x20→5` |
| +52 | **reserve/pool count** | `-1` for MountSlot slot-type 6/7/8 `[orig: @0x4b8586]`; else the per-player pool (`Entity_GetScoreValueBySlotType(def+216)`) ÷ `def+224` round cost (the divide is skipped when `def+224` is 0 `[orig: @0x4b858b]`); **capacity 1 adds the clip in** `[orig: @0x4b85ef]` — ported 2026-07-11 (`main_game.gd` folds `reserve += clip` for `clipsize == 1`; the ammo text and the round icons both read the folded count) |
| +56 | **clip rounds** | `-1` when capacity (`def+88`) is `-1` `[orig: @0x4b85fa]`; else the pool clip (`def+220` set) or `MountSlot+16` u16 |
| +64 | clip capacity | `def+88` |
| +572 | **rounds-per-icon divisor** byte | `def+727` (the HUDRNDGFX 5th field) `[orig: @0x4b85fd]` |
| +60 | weapon heat (capped `0xFFFF`) | `WeaponSlot_CalcAccumulatedHeat @0x53f780` — heat elements unwitnessed |
| +72 | horizontal **speed** | `300 * sqrt((entity+156>>8)² + (entity+152>>8)²) / 585` |
| +364 | distance to target/cam | sqrt of delta, `→int` |
| +368 | entity name string | `GameText_GetString("item", weapondef+1318)` else `"text_default"` |

`maxHealth` = `[orig: Entity_GetMaxHealthWithDifficulty @0x43b8a0]`. The entity
layout offsets (`+286` health, `+354` team, `+300` posture flags, `+36` flags)
agree with the GamePlayerEntity map in `docs/net/novaworld-net-re.md`. The
posture bits are now pinned by the crosshair's row select: **`0x100` = prone,
`0x200` = crouch** `[orig: HUD_DrawCrosshair @0x592b37]` (the stance *icon*
orders them crouch=1/prone=2; the ERROR table orders prone=0/crouch=1/stand=2).

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
  `dword_27239E4[idx*4]`, dims `dword_27239E8[]/EC[]`. Retail JO defines six
  (0..5: stand/crouch/prone/sitting/emplaced/parachute).
- **Gates**: the whole element skips unless the ALPHAFADE ramp is nonzero AND
  all six stance-frame handles (0..5) are loaded `[orig: @0x599f18..0x599f50]`
  — ported 2026-07-11 (`game_hud._draw_stance` early-outs).
- Anchor = `HUDSTANCEPOS → dword_2723AEC (x) / dword_2723AF0 (y)`
  `[orig: loc_59F370 @0x5a0be7]`; each frame draws at **anchor + its own
  HUDSTANCE offset + the shared centering offset** `[orig: @0x59a173]`.
  Vehicle stance anchor `HUDVEHSTANCEPOS → dword_2723AF4/AF8`.
- **Scale and centering are SHARED from frame 0**: one factor
  `scale = 0x800000 / max(w0, h0)` computed from frame 0's dims scales every
  drawn frame's dims (`scaled = (scale*dim + 0x8000) >> 16`), and the
  centering offset comes from frame 0's scaled dims — `(128-scaled)/2`, or 0
  at 127+ `[orig: @0x599fed..0x59a07e]`. (Uniform retail frame sizes hide the
  sharing; the 2026-07-09 port scaled per-frame — fixed 2026-07-11 to the
  exact integer form, `hud_stance.gd scale_q16/scaled_dim/center_offset`,
  pinned in `hud_helpers_test.gd` test_stance_shared_scale.)
- **Cross-fade** (fully witnessed 2026-07-09): on a stance change the previous/
  current bytes (`byte_2723D3C/D3D`) shift and the change tick restamps
  (`dword_2723D38`) `[orig: @0x599f8a]`. With `elapsed` clamped to the ALPHAFADE
  ramp (`dword_272361C` ticks): `fade = 255 − u8(u16((elapsed<<16)/ramp − 1) >> 8)`
  (255 right after the change, 0 at ramp end) `[orig: @0x599fc0]`. The **current**
  frame draws at `min(base + fade, 255)` (base = `dword_2723614`); the
  **previous** frame then ghosts on top at alpha `fade >> 2` (the witnessed
  `(fade<<22)` alpha-byte splice, max 63) `[orig: @0x59a226..0x59a2e3]`. Both
  are tinted the RGB of **STANCEICON_COLOR** (`dword_2723AE8`, parsed ARGB at
  `[orig: @0x5a0ec0]`).
- In an MP session a team tile (frame index 6/7 from `entityA+354` team 1/2) is
  drawn underneath `[orig: @0x59a0cb..0x59a173]` — MP HUD phase, not ported.
- Stance indices: `0`=stand, `1`=crouch (`&0x200`), `2`=prone (`&0x100`),
  `3`=vehicle/mounted, `5`=parachute.

Port: `hud_stance.gd` (frame draw + offsets) + `hud_fade.gd` (the fade pair) +
`game_hud.gd _draw_stance` (prev/current state).

### HUD text + half-bright — `HUD_DrawTextRightAligned_HalfBright @0x580850`

- All three wrappers share one body: halve the color, then call
  `CGameFont_DrawText` with a **drawFlags** word whose initial value selects
  the alignment — `HUD_DrawTextLeft_HalfBright @0x5804c0` starts 0,
  `HUD_DrawTextCentered_HalfBright @0x580680` starts 1, the right wrapper
  starts 2 `[orig: @0x580875]` (both siblings renamed from `sub_` 2026-07-11,
  anchored on the flag init + the consumption below). The wrapper's own flags
  arg adds `0x100` (format-tag suppression) and `2` (scaled mode, drawFlags
  `|= 4`); the HUD element calls pass flags `1` — plain, unscaled.
- **The alignment happens inside** `[orig: CGameFont_DrawText @0x6752c0]`:
  flag 1 measures the line and starts at `x − width·scale·0.5`
  `[orig: @0x675518..0x675539]`; flag 2 starts at `x − width·scale`
  `[orig: @0x675566..0x675587]`; neither → `x` (left). (The 2026-07-09
  "the caller measures and subtracts" wording was wrong about the mechanism;
  the drawn result — right anchor = right edge, center anchor = midpoint —
  is what `hud_text.gd` implements, unchanged.)
- **Half-bright** = `(color >> 1) & 0x7F7F7F | 0xFF000000` — halve each RGB
  channel, force opaque alpha. All three alignment paths half-bright.
- Fonts are `font_hi` / `font_lo` named in `hudpos.def` (`.fnt` bitmap fonts).
  Glyph layout/spacing inside `CGameFont_DrawText` (D3D vertex build) is a
  follow-up; the OpenNova port uses `NovaFntResource` (already parses the `.fnt`
  glyph atlas) for the glyphs.

### Ammo count + weapon name — `hud_draw_weapon_ammo_and_name @0x5939d0` (ported 2026-07-09)

Both elements gate on the info struct's weapon-def pointer (`dword_27235B0`,
info+552) and their own token's **hidden** dword, and both draw **half-bright**
in `WEAPON_TEXTCOLOR` (`dword_2723AC4`) with the token's alignment
(0=left→`sub_5804C0`, 1=right→`@0x580850`, 2=center→`sub_580680`).

- **Ammo count** at `AMMOCOUNTPOS` (`dword_27235FC/2600/2604/2608` =
  x/y/hidden/align): reads reserve (info+52, `dword_27233BC`) and clip
  (info+56, `dword_27233C0`). Hidden entirely when `reserve == -1` or capacity
  (`weapondef+88`) `== -1`; formats **`"%d/%d"` clip/reserve** when
  `clip != -1 && capacity >= 2`, else **`"%d"` reserve** `[orig: @0x593a33..0x593ab0]`.
  (The `capacity < 2` compare is **unsigned** — capacity `-1` takes the
  `"%d/%d"` branch but the `-1` hide covers it; the port's signed
  `capacity >= 2` after the `-1` early-out yields the identical display.)
- **Weapon name** at `HUDWEAPONNAME` (`dword_27235EC/F0/F4/F8`): the string is
  `GameText_GetString("WepDes", weapondef+20)` — the **raw weapon id** (e.g.
  `WPN_AK47`) as the key into gametext's `WepDes` section; a miss returns the
  empty string (nothing draws) `[orig: @0x593b7f; GameText_GetString @0x51ebd0
  miss @0x51ec00]`. On surfaces ≤ 640 wide the x nudges −4 (left-aligned) /
  +4 (right-aligned) `[orig: @0x593b36..0x593b4d]`.

Port: `hud_weapon_text.gd`; name resolution + capacity/-1 mapping in
`main_game.gd` (`_resolve_weapon_display_name`, `_update_game_hud`).

### Clip + rounds indicator — `draw_hud_ammo_indicator @0x599a30` (ported 2026-07-09)

The weapon's magazine graphic at the `HUDCLIP` anchor (`dword_27237B8/BC`;
either component nonzero enables), gated on the ALPHAFADE ramp and the ammo
`-1` sentinels.

- **Weapon-def fields** (parsed from `weapon.def`): `HUDCLIPGFX <x> <y> <tex>`
  → offset `weapon+620/624` (via `atol`), texture record `+608..616`
  `[orig: parse @0x54427f]`; `HUDRNDGFX <x> <y> <stepx> <stepy> <divisor> <tex>`
  → round start `+644/648`, step `+652/656`, **rounds-per-icon divisor byte
  `+727`** (byte store — out-of-range file values wrap mod 256; ported
  `PlayerHudWeaponDef` masks `& 0xFF`), texture record `+628..640`
  `[orig: parse @0x5442fc]`. Both parses gate on
  `FileSystem_FileExists(texture)` FIRST and skip the whole token (offsets
  included) with a parse warning when the art is missing
  `[orig: @0x544295 / @0x544316]` — behaviorally equal to the port's
  null-texture draw gates (`libs/def` has no VFS; the load-time miss lands in
  `game_hud._load_texture`). Retail JO sample: `hudrndgfx 9 0 18 0 1 H_round.tga`.
- **Flash restamp**: the stamp tick (`dword_2723D48`) resets when the ammo
  class (`weapondef+220`), the reserve count, or the pool id byte
  (`weapondef+216`) changes `[orig: @0x599ab2]` — i.e. weapon switch or reload,
  NOT per shot. Alpha = `base − u8(u16((elapsed<<16)/ramp − 1)>>8) + 255`,
  clamped to the ALPHAFADE **max** (`dword_2723618`) `[orig: @0x599af9]` — a
  flash toward `base+255` decaying to `base` over the ramp.
- **Background** (clip graphic) draws at anchor+offset with constant alpha
  `base`; **round icons** draw with the flash alpha, one per round, stepping by
  the step vector: count = clip (or **reserve when capacity == 1** — single-shot
  weapons), `(n+1)/divisor` when divisor > 1, capped at **40**
  `[orig: @0x599b9c..0x599c0a]`. Both tinted the STANCEICON_COLOR RGB.

Port: `hud_clip_indicator.gd` (restamp key proxy: D-HUD-5).

### Crosshair / reticle — `HUD_DrawCrosshair @0x592640` → `HUD_DrawCrosshairCornerQuad @0x590f50` (ported 2026-07-09)

- **Texture**: NOT per-weapon. `HUD_LoadAllTextures @0x59dda0` loads
  `sprintf("cross%02d.tga", style+1)` where `style` = the user's crosshair-style
  config (`dword_25510DC`) `[orig: @0x59e3d6]`, into the block `dword_2723980`
  ({…, handle, w, h}). The weapon-def `crosshair` field feeds a different
  (scope/lock) surface. The crosshair color is the user config
  `dword_25510E0`, overridden to `0xFFFF5050` in one mode (`dword_A8235C`)
  `[orig: @0x592bd5]`; defaults unwitnessed (D-HUD-8 / follow-up).
- **Visibility**: the cluster gates on `dword_2723CB4` and the weapon-def ptr;
  the spread crosshair draws when the player **cannot** take an aimed shot —
  `!Player_CanFireWeapon() || vehicle auto-aim || (dword_A8235C && gunner
  scoped)` `[orig: @0x592adc..0x592b01]`. `Player_CanFireWeapon @0x5cf780`
  requires the **settled** scope/sight view (`Player_IsEquippedWeaponScoped
  @0x4dcc80`, the SIGHTS-card gate) and returns 0 under `g_camera_mode`
  `[orig: @0x5cf807/@0x5cf828]` — so the crosshair draws from the hip, **all
  through the ADS ease**, and in every external-camera mode; it yields only
  once fully sighted (D-HUD-9 CLOSED 2026-07-11: the port hides only at
  `scope_fraction >= 1` — the settled sight view; the `@0x4de4f7` promoter is
  the only writer of `g_weaponScopeActive`). Its can't-fire
  path also resets `g_cameraFovDeg = 5242880` = **80.0 deg** 16.16
  `[orig: @0x5cf88e]` — the port's `fov_deg` default.
- **Anchor**: the offset applies to the **virtual-space** projection of the
  aim point (`Viewport_ScreenToVirtual`). For the on-foot local player with no
  camera mode that point is the literal screen center
  `[orig: @0x5928a0 — overlayCtx/2, dword_24C1424/2]`; a spectated entity or
  `g_camera_mode` (external/3P) projects `Entity_BuildCameraView` instead
  `[orig: @0x592910..0x59295e]` (D-HUD-10 CLOSED 2026-07-11: the host's
  `aim_screen_point()` returns no projection in first person — the HUD pins the
  exact design center — and projects the aim ray's 1000.0-unit far point
  `[orig: 65536000 q16 @0x592910]` in third person). Arms draw at top `(x, y−off)`, bottom
  `(x, y+off)`, left `(x−off, y)`, right `(x+off, y)`, center `(x, y)`
  `[orig: @0x592c50..0x592cd2]`.
- **Spread**: with `mp_CrossHairSpread` enabled (`dword_25510E4`; disabled
  still draws the assembled reticle at offset 0 `[orig: fldz @0x592bcc]` — the
  port models the enabled state):
  `row = stance + 3*Player_CanFireWeapon()` where stance = 0 prone (`&0x100`)
  / 1 crouch (`&0x200`) / 2 stand, forced 2 when swimming/under water
  (`entity+36 & 0x108020` or below `Env_WaterHeightFixed`), forced 1 when
  mounted `[orig: @0x592b35..0x592b87]`. Because the draw gate and the row
  select share the CanFire predicate, **on foot every drawn crosshair reads
  the hip rows 0..2**; the scoped rows 3..5 are reachable only through the
  vehicle auto-aim / gunner-scoped cases (witness comment left at
  `@0x592b87`; a train-side fix that keyed +3 on the port's `scope_engaged`
  contradicts this — re-adjudicate when the ADS-ease plumbing lands). Then
  `spread = C·(ERROR[row] + (player+0x380 >> 7) + (player+0x384 >> 7))` with
  `C = flt_7D76D0 = 11930464.0 = 2^31/180`, and
  `pixel = int(spread · screen_w / fov_scale) >> 16` where
  `fov_scale = 11930464 · SHIWORD(fov 16.16)` `[orig: @0x592b07..0x592bf5]` —
  the two binary-angle factors cancel:
  **`pixel = (ERROR_16.16[row] + recoil terms) · screen_w / int(fov_deg) >> 16`**.
  `ERROR` is parsed as six sequential 16.16 **degree** values into
  `weapon+0xB0..0xC4` via `Math_ParseFixedPoint16 @0x6131f0` (a digit parser,
  not atof) `[orig: parse @0x543b21]` — rows hip prone/crouch/stand then
  scoped prone/crouch/stand (retail sample `error 0.05 0.2 0.25 0.05 0.1
  0.15`). `libs/def` stores the rows as float degrees and the port re-quantizes
  `int(deg × 65536)` at consumption — equal for retail data; values whose f32
  rounds down (e.g. `0.7`) can read 1 16.16-LSB (0.000015 deg) low, invisible
  at pixel altitude. The recoil accumulators `player+0x380/+0x384` (pitchBlend
  et al, see `docs/world/world-wac-ai-re.md`) are not yet surfaced by the
  runtime — D-HUD-7.
- **Unported sub-elements of the same function** (witnessed 2026-07-11,
  explicitly out of the SP weapon-cluster port):
  - the **target-tracking cursor** — with a tracked entity (`ptr @0x27234F0`)
    and the cursor art loaded, a quad draws at the projected
    `Entity_ComputeWeaponFireOrigin` of the target, color/texture switching on
    same-team (`dword_2723900` vs `dword_27238F0` records) with an MP team
    gate and a tick-blink `[orig: @0x592790..0x592875]`;
  - the **aim-point quad for flagged weapons** — `weapondef+12 & 0x80` swaps
    the spread reticle for the weapon's own crosshair record (`weapon+376`)
    drawn at a raycast-projected aim point
    (`Entity_ComputeUserpointTransform` → `physics_raycast_entity_pools…` →
    project) `[orig: @0x592973..0x592ac8]`;
  - the **lock brackets** — four clipped 2D lines blinking around the tracked
    target when its mount state reads 3, team- and blink-gated
    `[orig: @0x592ce2..0x592dd7]`.
  - a mode flag `dword_24C1930 & 0x10000` replaces triggered/gametext strings
    with the literal `"&"` `[orig: @0x51f1c8 / @0x51ebe3]` — writer
    unwitnessed; not modeled.
- **Region geometry** (`HUD_DrawCrosshairCornerQuad @0x590f50`): each region's
  quad rect is the texture's size centered on the (offset) point, corners
  scaled to screen; the arms are 5-vertex triangle strips and the center a
  4-vertex strip, with the **inner vertices at the quad midpoint pulled back by
  0.1 × half-extent**, and **every vertex's UV = its normalized position within
  the quad rect** — which lands the inner vertices exactly on the witnessed
  0.45 / 0.5 / 0.55 atlas bands (center band 0.45..0.55, arms the outer bands).
  Vertex format: `rhw = 0.9`, `diffuse = 1.0`, `specular = color`; emitted via
  `[orig: GDynamicVB_DrawPrimitive @0x6788e0]` (D-HUD-8 on the color stage).

Port: `hud_crosshair.gd` (spread_px / error_row / the 5 strips as UV'd
polygons); visibility + row select in `game_hud.gd _draw_crosshair` (hidden
while `scope_engaged`).

### Mission triggered text — `HUD_DisplayTriggeredText @0x51f190` (ported 2026-07-09)

The WAC/BMS `text` action (our `event_runtime` `OutputText` effect, id in
`param1`) resolves `sprintf("ID%03i", id)` against the **per-mission string
table** `g_TextMission @0xB4C2B4`, section **`"Triggered Text"`** — read
directly, no override-table consult; a miss shows nothing. The table loads at
mission start from the map file name with its extension replaced by `.bin`,
**falling back to `medmssn.bin` only when that file does not exist**
(`FileSystem_FileExists` picks the name; a present-but-unloadable file stores
null with no fallback) `[orig: TextResource_LoadMissionTextBin @0x51ed90,
exists-gate @0x51ede3]` — ported exactly (`main_game._load_hud_text_tables`
via `has_file`). The resolved line goes to the system/debug chat channel:
`Chat_AddDebugMessage(text, -1, 930)` `[orig: @0x51f216]` — the `-1` color is
stored raw in the 128-byte slot, i.e. packed ARGB `0xFFFFFFFF` opaque white
(the port pushes white; whether the unwitnessed drawer treats `-1` as a
channel-default sentinel is part of the D-HUD-6 follow-up). Slots carry 119
text chars, word-wrapped via font metrics, per-line life **930 ticks** (~15 s)
with consecutive expiries floored to **prev + 186 ticks** `[orig: @0x49894e]`;
display buffers rebuilt by `[orig: Chat_RebuildDisplayBuffers @0x498bd0]`
(40 wrapped slots per channel, continuation lines indented two spaces). The
channel's on-screen geometry table (`dword_28E4DF8`) writer is unwitnessed —
follow-up.

Port: `hud_messages.gd` feed + `main_game.gd _show_triggered_text` /
`_load_hud_text_tables` (D-HUD-6 on the reduced altitude).

### Parachute / armor icons — `sub_5925C0 @0x5925c0`

- Parachute: `entityA flags &0x10` → draw `ParachuteIcon` at `(x@0x272383C, y@0x2723840)`,
  handle `dword_27239A4`.
- Armor: `entityA flags &8` → draw `ArmorIcon` at `(dword_2723844, dword_2723848)`,
  handle `dword_27239B4`.
- The static HUD frame background is `STATICFRAME` → pos `dword_2723B1C/B20`,
  name `byte_2723C24`, drawn at the top of `HUD_RenderOverlays`
  (`[orig: draw_textured_quad_with_border @0x590c40]`).
- Port pending: the runtime does not yet surface the entity flag bits.

### MP objective status — `draw_objective_status_text @0x59aa30` (witnessed, MP HUD phase)

The `GAMEINFO`-anchored status line for MP game types (`g_GameType & 0x10000`),
switching on the objective state byte (`byte_27234FE`): gametext `client`
section keys `strcli19/05/06/17/18/01` with per-state colors; KOTH appends
`strcli20/21` by flag-carrier state; the death screen shifts the draw up by the
measured text height. Not an SP element; ported later with the MP HUD.

## `hudpos.def` parser token → global map — `loc_59F370`

A `_stricmp` token-dispatch; each token reads decimal fields via `atof → ftol`
(1024×768 ints) or copies a texture-name string. Cross-checked against
`DefHudPosDef` in `libs/def/include/def/def.h`.

| Token | Writes |
|---|---|
| `HEALTHPOS` | `dword_27237C8/CC/D0/D4` (x1,y1,x2,y2) |
| `HUDSTANCE <idx> <x> <y> <tex>` | `dword_2723B24[idx]`, `dword_2723B44[idx]`, `byte_2723B8C+idx*0x13` |
| `HUDSTANCEPOS` | `dword_2723AEC` (x), `dword_2723AF0` (y) |
| `HUDVEHSTANCEPOS` | `dword_2723AF4` (x), `dword_2723AF8` (y) |
| `STATICFRAME` | name `byte_2723C24`, pos `dword_2723B1C/B20` |
| `ParachuteIcon` | name `byte_2723C34`, pos `0x272383C/0x2723840` |
| `ArmorIcon` | name `byte_2723C44`, pos `dword_2723844/0x2723848` |
| `AMMOCOUNTPOS <x> <y> <hidden> <align>` | `dword_27235FC/2600/2604/2608` — the **4-field positioned-text layout**, read strictly positionally: fields 1-3 `atof→ftol` (a word in field 3 reads 0), field 4 via `HUD_ParseTextAlignment @0x59d6b0` (full-string stricmp: "right"=1, "center"=2, anything else — including a missing field — 0=left) `[orig: @0x59fc3d]`. Retail 2-field lines (`GAMEINFO`, `HUDCHATTEXT`) render visible/left in retail JO, pinning missing fields to 0. `HUDWEAPONNAME` → `dword_27235EC/F0/F4/F8`, `GAMEINFO` → `dword_272382C/30/34/38`; the other positioned tokens follow the same shape |
| `ALPHAFADE <base%> <max%> <seconds>` | `dword_2723614` = base×**2.55**, `dword_2723618` = max×**2.55**, `dword_272361C` = seconds×**62** (ticks) `[orig: @0x5a0882..0x5a08c2; dbl_7D9A20 = 2.55, dbl_7C88C0 = 62.0]`. Each field goes through **atof**, so fractional file values (`1.5` s → 93 ticks) survive into the converts — `libs/def` stores the raw fields as floats and the consumers apply ×2.55/×62 with the same truncation (fixed 2026-07-11; `def_parse_hudpos` pins the fractional case) |
| `STANCEICON_COLOR <a> <r> <g> <b>` | `dword_2723AE8` packed ARGB `[orig: @0x5a0ec0]` — the stance/clip-indicator tint |
| `HUDCLIP` | `dword_27237B8/BC` — the clip-indicator anchor |
| `HUDTIMECLOCK`, `mapcoords`, `HUDPOWERBAR` | recon-confirmed token set (timer/map — witness when those elements land) |

## Divergence catalog (D-HUD)

| ID | Ours / reference | Original (Jointops.exe) | Why / consequence |
|---|---|---|---|
| D-HUD-1 | IDB curated name `draw_minimap_compass_overlay`; the oscarmike reference models a "spinmap" compass | `HUD_DrawStanceIndicator @0x599f10` renders the **stance** indicator, keyed by `byte_27235C0` = `hudInfo+568` stance index | The function is mis-named in the IDB and mis-modeled in oscarmike. The OpenNova stance widget must be the discrete cross-faded `HUDSTANCE` frames, not a compass. Rename proposed (held). |
| D-HUD-2 | oscarmike `spinmap.gd` = a single rotating compass-ring texture | the stance widget is discrete pre-rendered frames cross-faded on stance change; the **heading/north** display is a *separate* top-down radar (`draw_minimap_overlay @0x599700`) | Do not port a rotating ring. Stance = frame swap with fade; heading = radar (follow-up). |
| D-HUD-3 | — | Design space is fixed **1024×768**, scaled with round-to-nearest (`Viewport_ScaleToVirtualCoords @0x5d2b20`) | OpenNova authors HUD positions in 1024×768 and scales to the actual surface with the `(p*s+½s)/dim` rounding. |
| D-HUD-4 | — | Health bar *fill width* uses the capped `+92` ratio; *fill color* uses an uncapped recomputed ratio (`HUD_DrawHealthBar @0x5a2e50`) | Equivalent over `[0,1]`; recorded so the port matches both reads rather than collapsing to one. |
| D-HUD-5 | `hud_clip_indicator.gd` restamps its flash on (`round_type`, reserve) change | restamp keys are (`weapondef+220` ammo class, reserve, `weapondef+216` pool id) `[orig: @0x599ab2]` | Our weapon model runs a single ammo pool (net-re D-WPN-2), so the ammo-class/pool ids aren't distinct state yet; the proxy fires on the same reload/switch transitions. Revisit with per-class pools. |
| D-HUD-6 | `hud_messages.gd` is a timed line feed (930-tick life, ≥186 stagger, wrap, two-space continuation indent) drawn at the `HUDCHATTEXT` anchor | triggered text rides the full chat system: channel-2 ring buffers `[orig: Chat_AddDebugMessage @0x4987f0]`, display rebuild `[orig: @0x498bd0]`, and a channel geometry table (`dword_28E4DF8`, writer unwitnessed) | Message-line altitude port. The channel's exact screen geometry, per-line fade curve, and the player-chat channel are the chat-pipeline follow-up. |
| D-HUD-7 | crosshair spread = the ERROR term only | spread adds `(player+0x380 >> 7) + (player+0x384 >> 7)` — the recoil/aim accumulators `[orig: @0x592b95..0x592bc8]` | The runtime does not yet surface those accumulators (they live in the entity angle state; see `docs/world/world-wac-ai-re.md` pitchBlend). Wire them when the recoil write-side is witnessed. |
| D-HUD-8 | crosshair color multiplies the texture (canvas modulate); default white | the strip writes the color to the **specular** channel with `diffuse = 1.0` `[orig: @0x5914d7]`; the blend-stage setup lives in the HUD shader pass (`GfxShader_ApplyPassChecked @0x677020`, unwitnessed); color source = user config `dword_25510E0` | Identical for the default white; witness the texture-stage state (and the config default) before modeling the user crosshair color. |
| D-HUD-9 | the crosshair hides the instant the scope engages (`scope_engaged`) | it draws while an aimed shot is NOT available — `!Player_CanFireWeapon() @0x5cf780`, which requires the **settled** sight view (`Player_IsEquippedWeaponScoped @0x4dcc80` = `g_weaponScopeActive`, promoted only at ease completion `@0x4de4f7`) — so it stays up through the whole ADS ease and yields only once fully sighted `[orig: gate @0x592afa]` | FIXED 2026-07-11 (weapon round): the hide is `scope_engaged && scope_fraction >= 1`; the row select stays hip — `+3` keys on `Player_CanFireWeapon()` itself (`@0x592b87`), unreachable on foot while the crosshair draws. |
| D-HUD-10 | the crosshair anchors at the fixed design center (512, 384) | the anchor is the projected aim point through `Viewport_ScreenToVirtual`: the literal screen center only for the on-foot local player with no camera mode `[orig: @0x5928a0]`; spectate / `g_camera_mode` (external/3P) project `Entity_BuildCameraView` (far point 65536000 q16 = 1000.0) `[orig: @0x592910..0x59295e]` | FIXED 2026-07-11 (weapon round): `LocalPlayerHost.aim_screen_point()` — `Vector2.INF` in first person (the HUD pins the exact center, matching `@0x5928a0`), the projected aim in third person; `NovaGameHudHost` feeds it to both shells. |

## Follow-ups (not yet witnessed / deferred)

- **Radar / minimap** `[orig: draw_minimap_overlay @0x599700]` — the top-down map
  + blips + heading; the `HUDSPINMAP X1/Y1/X2/Y2` token → its bounds. Located,
  not yet witnessed. Needed for the "compass/heading" HUD element.
- **Heading source** — the `byte_2723D3*`/heading state feeding the radar
  (computed in `HUD_RenderAllOverlays` via `Math_FixedPointTransformPoint22` +
  `sub_590970`/`sub_59A9E0`).
- **Chat channel geometry** — the `dword_28E4DF8` table (rows 1/2 chat, 3/4
  system/debug) that `Chat_RebuildDisplayBuffers @0x498bd0` wraps against and
  the drawer anchors with; its writer is unwitnessed (D-HUD-6).
- **Crosshair recoil accumulators** — the write side of `player+0x380/+0x384`
  (fire recoil / decay), needed to close D-HUD-7.
- **Crosshair color config** — `dword_25510E0` default + the HUD shader pass
  texture-stage state (D-HUD-8); the crosshair styles' count (`cross%02d.tga`).
- **Crosshair sub-elements** — the target-tracking cursor
  (`@0x592790..0x592875`), the `weapondef+12 & 0x80` aim-point quad
  (`@0x592973..0x592ac8`), and the lock brackets (`@0x592ce2..0x592dd7`);
  witnessed 2026-07-11, unported (MP/vehicle/lock phases).
- **`dword_24C1930` flag 0x10000** — replaces triggered/gametext strings with
  `"&"` (`@0x51f1c8`/`@0x51ebe3`); the writer is unwitnessed.
- **`mp_CrossHairSpread` default** — `dword_25510E4` (spread disabled still
  draws the assembled reticle at offset 0); the config default is unwitnessed
  (the port models enabled).
- **Scope overlay** — the scope view's reticle/mask (`scopexh.tga @0x59e133`,
  weapon sights), which replaces the HUD crosshair when scoped.
- **Team color table** — the team→color mapping used for text/labels (the stance
  team tile uses `entityA+354`; the text color table is unwitnessed).
- **CGameFont glyph layout** `[orig: CGameFont_DrawText @0x6752c0]` — per-glyph
  D3D vertex build / spacing, to confirm `NovaFntResource` layout parity.
- **Timer/score, altitude/power bar, reload bar, weapon slot bar** — enable
  flags witnessed; draws (`HUD_DrawWeaponSlotBar @0x599cd0` located) unwitnessed.
- **Weapon heat elements** — info+60 heat is computed
  (`WeaponSlot_CalcAccumulatedHeat @0x53f780`) but its HUD consumers (HUDHEAT
  rect) are unwitnessed.
- **MP objective status + stance team tile** — witnessed
  (`draw_objective_status_text @0x59aa30`, `@0x59a0cb`); port with the MP HUD.
- **Parachute/armor icons** — witnessed (`sub_5925C0`); needs the runtime to
  surface the entity flag bits (`+36 & 0x10/0x8`).

## IDB changes

Applied 2026-07-11 (auto-name renames at anchored confidence + appended
comments; IDB saved):

- **Rename** `sub_580680` → `HUD_DrawTextCentered_HalfBright` (anchored:
  drawFlags init 1 `@0x58069d`; `CGameFont_DrawText` flag-1 center
  `@0x675518`).
- **Rename** `sub_5804C0` → `HUD_DrawTextLeft_HalfBright` (anchored: drawFlags
  init 0 `@0x5804da`).
- **Comment** at `0x592b87`: the ERROR row select = `stance +
  3*Player_CanFireWeapon()`; +3 reachable only via the vehicle
  auto-aim/gunner-scoped cases.
- **Comment** at `0x51f1c8`: the `dword_24C1930 & 0x10000` → `"&"` replacement
  quirk (writer unwitnessed).

Applied 2026-07-16 (repo hygiene pass — the maintainer's apply-the-held-IDB-updates
call; IDB saved):

- **Rename** `draw_minimap_compass_overlay @0x599f10` → `HUD_DrawStanceIndicator`
  (anchored: indexes `HUDSTANCE` frames by `byte_27235C0 = hudInfo+568` stance
  index; D-HUD-1), with the misnomer + stance-keying comment at `0x599f10`.
- **Rename** `draw_minimap_compass_ring @0x5d17a0` → `Hud_DrawScopeCircleMask` —
  the 64-segment circular scope mask drawn for Scoped weapons that author no
  SIGHTS rows (net-re §5.62). Re-witnessed at rename time: both callers are the
  SIGHTS-card gates (`Render_ProcessMainSceneFrame @0x5cab15`,
  `render_hud_overlay @0x5d82f2`). The sibling reticle drawer
  `draw_minimap_crosshair_and_grid @0x5d1160` keeps its name (second caller
  unwitnessed) and carries a candidate-rename comment for the next HUD grill.
- **Comment** at `0x59e3d6` noting `dword_25510DC` = the user crosshair-style
  index (`cross%02d.tga`), `dword_25510E0` = the user crosshair color, and
  `dword_25510E4` = the `mp_CrossHairSpread` enable.
