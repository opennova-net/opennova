# Registered OpenNova / retail maximum-quality lighting review

This publication contains 18 registered comparisons across `00TRa.bms`,
`03TR.bms`, `CP01.bms`, `CP04.bms`, and `CP12.bms`. Every pair keeps terrain
and the first-person viewmodel active, uses the M16 Burst with plain bare
`IndoArms.3di` arms, and omits gameplay HUD content from the saved images.
OpenNova's runtime witness additionally records 30/270 ammunition.

Retail ran at the exhaustive highest-quality RevX02 profile. Its pre-launch
stage changed only `object_texdetail`, because the other video/effect values
already matched the profile. The capture hook copies the backbuffer at a
certified pre-HUD boundary, restores the D3D scene, and then lets retail
render its UI unchanged. Normal on-screen HUD/FPS therefore remain visible;
only the registered PNG is HUD-free.

## Immutable identities and inventory

| Identity | Value |
|---|---|
| Frozen OpenNova source | `04ac107197c0d068c2fc071bf4e3fcc247c6a8ba` |
| Retail fixture catalog SHA-256 | `e230836ea42fe563e24d16eec3a0d95137bb3c7138c711a9e04b8cc4a30fd1b1` |
| Godot executable SHA-256 | `1e5efe381f62ee1cea6bc18caac71c0c74bd6e68e6af5c6efb3dbe76628f61c7` |
| GDExtension SHA-256 | `b8590ed91f7c1b30e362e971f6341f1d20d153f079291d85671bdbf37d5276ce` |
| Retail executable SHA-256 | `b9971c8273b7bbb1c8518a738596d669cd7794e9d307ae63a7a9a530eb802fac` |
| Retail video profile | `retail_reference_highest_retail_selectable_v2` |
| Retail capture producer | onHook 0.5.0; bridge 1.4; `opennova.render_capture_bundle.v4` |
| Retail registrar | 4.0.0; `opennova.registered-retail-capture.v5` |
| Retail staging | 3.0.0; `opennova.retail-presentation-stage.v3` |
| Comparison builder | 4.0.0; `opennova.retail-comparison.v6` |

Including this index, the package contains 343 files: 180 PNGs and 162 JSON
records plus this README. The PNGs comprise 90 raw OpenNova diagnostic
variants, 18 retail frames, 18 normalized OpenNova frames, 18 side-by-sides,
18 overlays, and 18 absolute differences. No backup, restore token,
transcript, or absolute caller path is included.

Every sanitized stage record binds original `game.cfg` SHA-256
`556880e9ec85d60021f2ce17d584bde30702a6ae3ecfba6a1e6eb54402c8cad3`, effective
maximum-profile SHA-256
`e7bd7d27d6dcb22c8b58e3daa6ef543e2489f0600ffee28ffcb9a53d79806ed3`, and
approved `weapon.sav` SHA-256
`f4907820a58505a6988f140a27638dae7dbaaea2cc446642f0bc44c868905623`.
`hud_detail` remains 0 in both original and staged config. The original config
was restored byte-for-byte after registration.

## Review index and exact descriptive metrics

The full-frame policy is `qualitative_only_matched_hud_hidden_cross_engine`.
The two required ROI families are descriptive presentation regions:
`world_center=[240,180,1320,420]` and `viewmodel_arms=[850,700,900,500]`.
Across all 18 fixtures, full-frame MAE spans `8.953803`-`32.955741`,
world-center MAE spans `5.388775`-`35.747731`, and viewmodel-arms MAE spans
`5.780953`-`44.944125`. These are exact channel deltas from each comparison
manifest, not thresholds or pixel-parity verdicts.

| Fixture | Mission / minute | Full MAE / RMS | World MAE / RMS | Arms MAE / RMS | Direct evidence |
|---|---:|---:|---:|---:|---|
| `00tra-armory-glass-retail` | `00TRa.bms` / 0900 | 20.874477 / 33.167525 | 15.786947 / 25.101811 | 20.507329 / 33.804822 | [side-by-side](00tra-armory-glass-retail/comparison/00tra-armory-glass-retail-side-by-side.png) · [overlay](00tra-armory-glass-retail/comparison/00tra-armory-glass-retail-overlay-50.png) · [diff](00tra-armory-glass-retail/comparison/00tra-armory-glass-retail-absolute-diff.png) · [comparison](00tra-armory-glass-retail/comparison/00tra-armory-glass-retail-comparison.json) · [OpenNova manifest](00tra-armory-glass-retail/opennova/00tra-armory-glass-retail-manifest.json) · [registration](00tra-armory-glass-retail/retail/registered.json) |
| `00tra-armory-lght-retail` | `00TRa.bms` / 0900 | 12.826923 / 18.117111 | 5.388775 / 9.146552 | 18.363650 / 21.610828 | [side-by-side](00tra-armory-lght-retail/comparison/00tra-armory-lght-retail-side-by-side.png) · [overlay](00tra-armory-lght-retail/comparison/00tra-armory-lght-retail-overlay-50.png) · [diff](00tra-armory-lght-retail/comparison/00tra-armory-lght-retail-absolute-diff.png) · [comparison](00tra-armory-lght-retail/comparison/00tra-armory-lght-retail-comparison.json) · [OpenNova manifest](00tra-armory-lght-retail/opennova/00tra-armory-lght-retail-manifest.json) · [registration](00tra-armory-lght-retail/retail/registered.json) |
| `00tra-courtyard-retail` | `00TRa.bms` / 0900 | 14.712840 / 20.695589 | 9.430514 / 14.055622 | 13.383284 / 16.363418 | [side-by-side](00tra-courtyard-retail/comparison/00tra-courtyard-retail-side-by-side.png) · [overlay](00tra-courtyard-retail/comparison/00tra-courtyard-retail-overlay-50.png) · [diff](00tra-courtyard-retail/comparison/00tra-courtyard-retail-absolute-diff.png) · [comparison](00tra-courtyard-retail/comparison/00tra-courtyard-retail-comparison.json) · [OpenNova manifest](00tra-courtyard-retail/opennova/00tra-courtyard-retail-manifest.json) · [registration](00tra-courtyard-retail/retail/registered.json) |
| `00tra-fire-barrel-close-retail` | `00TRa.bms` / 0900 | 32.906409 / 44.583397 | 28.194157 / 36.567608 | 30.849287 / 43.461047 | [side-by-side](00tra-fire-barrel-close-retail/comparison/00tra-fire-barrel-close-retail-side-by-side.png) · [overlay](00tra-fire-barrel-close-retail/comparison/00tra-fire-barrel-close-retail-overlay-50.png) · [diff](00tra-fire-barrel-close-retail/comparison/00tra-fire-barrel-close-retail-absolute-diff.png) · [comparison](00tra-fire-barrel-close-retail/comparison/00tra-fire-barrel-close-retail-comparison.json) · [OpenNova manifest](00tra-fire-barrel-close-retail/opennova/00tra-fire-barrel-close-retail-manifest.json) · [registration](00tra-fire-barrel-close-retail/retail/registered.json) |
| `00tra-fire-barrel-east-retail` | `00TRa.bms` / 0900 | 30.370269 / 38.959830 | 35.032579 / 41.673278 | 29.464929 / 42.039977 | [side-by-side](00tra-fire-barrel-east-retail/comparison/00tra-fire-barrel-east-retail-side-by-side.png) · [overlay](00tra-fire-barrel-east-retail/comparison/00tra-fire-barrel-east-retail-overlay-50.png) · [diff](00tra-fire-barrel-east-retail/comparison/00tra-fire-barrel-east-retail-absolute-diff.png) · [comparison](00tra-fire-barrel-east-retail/comparison/00tra-fire-barrel-east-retail-comparison.json) · [OpenNova manifest](00tra-fire-barrel-east-retail/opennova/00tra-fire-barrel-east-retail-manifest.json) · [registration](00tra-fire-barrel-east-retail/retail/registered.json) |
| `00tra-fire-barrel-full-composite-retail` | `00TRa.bms` / 0900 | 30.929611 / 39.636910 | 35.747731 / 42.513227 | 29.921205 / 42.573368 | [side-by-side](00tra-fire-barrel-full-composite-retail/comparison/00tra-fire-barrel-full-composite-retail-side-by-side.png) · [overlay](00tra-fire-barrel-full-composite-retail/comparison/00tra-fire-barrel-full-composite-retail-overlay-50.png) · [diff](00tra-fire-barrel-full-composite-retail/comparison/00tra-fire-barrel-full-composite-retail-absolute-diff.png) · [comparison](00tra-fire-barrel-full-composite-retail/comparison/00tra-fire-barrel-full-composite-retail-comparison.json) · [OpenNova manifest](00tra-fire-barrel-full-composite-retail/opennova/00tra-fire-barrel-full-composite-retail-manifest.json) · [registration](00tra-fire-barrel-full-composite-retail/retail/registered.json) |
| `00tra-tire-marks-retail` | `00TRa.bms` / 0900 | 32.955741 / 48.005632 | 17.516581 / 28.361560 | 44.944125 / 60.214920 | [side-by-side](00tra-tire-marks-retail/comparison/00tra-tire-marks-retail-side-by-side.png) · [overlay](00tra-tire-marks-retail/comparison/00tra-tire-marks-retail-overlay-50.png) · [diff](00tra-tire-marks-retail/comparison/00tra-tire-marks-retail-absolute-diff.png) · [comparison](00tra-tire-marks-retail/comparison/00tra-tire-marks-retail-comparison.json) · [OpenNova manifest](00tra-tire-marks-retail/opennova/00tra-tire-marks-retail-manifest.json) · [registration](00tra-tire-marks-retail/retail/registered.json) |
| `03tr-sun-sky-retail` | `03TR.bms` / 0390 | 23.253260 / 28.356819 | 22.350760 / 28.837041 | 25.247999 / 28.494326 | [side-by-side](03tr-sun-sky-retail/comparison/03tr-sun-sky-retail-side-by-side.png) · [overlay](03tr-sun-sky-retail/comparison/03tr-sun-sky-retail-overlay-50.png) · [diff](03tr-sun-sky-retail/comparison/03tr-sun-sky-retail-absolute-diff.png) · [comparison](03tr-sun-sky-retail/comparison/03tr-sun-sky-retail-comparison.json) · [OpenNova manifest](03tr-sun-sky-retail/opennova/03tr-sun-sky-retail-manifest.json) · [registration](03tr-sun-sky-retail/retail/registered.json) |
| `cp01-waterline-above-retail` | `CP01.bms` / 0930 | 18.883609 / 28.242705 | 26.607665 / 38.647288 | 18.292295 / 25.310272 | [side-by-side](cp01-waterline-above-retail/comparison/cp01-waterline-above-retail-side-by-side.png) · [overlay](cp01-waterline-above-retail/comparison/cp01-waterline-above-retail-overlay-50.png) · [diff](cp01-waterline-above-retail/comparison/cp01-waterline-above-retail-absolute-diff.png) · [comparison](cp01-waterline-above-retail/comparison/cp01-waterline-above-retail-comparison.json) · [OpenNova manifest](cp01-waterline-above-retail/opennova/cp01-waterline-above-retail-manifest.json) · [registration](cp01-waterline-above-retail/retail/registered.json) |
| `cp01-waterline-below-retail` | `CP01.bms` / 0930 | 8.953803 / 13.841502 | 7.786971 / 12.168433 | 5.780953 / 8.384286 | [side-by-side](cp01-waterline-below-retail/comparison/cp01-waterline-below-retail-side-by-side.png) · [overlay](cp01-waterline-below-retail/comparison/cp01-waterline-below-retail-overlay-50.png) · [diff](cp01-waterline-below-retail/comparison/cp01-waterline-below-retail-absolute-diff.png) · [comparison](cp01-waterline-below-retail/comparison/cp01-waterline-below-retail-comparison.json) · [OpenNova manifest](cp01-waterline-below-retail/opennova/cp01-waterline-below-retail-manifest.json) · [registration](cp01-waterline-below-retail/retail/registered.json) |
| `cp01-water-oblique-retail` | `CP01.bms` / 0930 | 21.726363 / 31.231794 | 27.918399 / 36.241286 | 23.091658 / 33.272501 | [side-by-side](cp01-water-oblique-retail/comparison/cp01-water-oblique-retail-side-by-side.png) · [overlay](cp01-water-oblique-retail/comparison/cp01-water-oblique-retail-overlay-50.png) · [diff](cp01-water-oblique-retail/comparison/cp01-water-oblique-retail-absolute-diff.png) · [comparison](cp01-water-oblique-retail/comparison/cp01-water-oblique-retail-comparison.json) · [OpenNova manifest](cp01-water-oblique-retail/opennova/cp01-water-oblique-retail-manifest.json) · [registration](cp01-water-oblique-retail/retail/registered.json) |
| `cp01-water-shallow-retail` | `CP01.bms` / 0930 | 21.098932 / 32.007058 | 29.406110 / 40.701426 | 21.133723 / 33.697806 | [side-by-side](cp01-water-shallow-retail/comparison/cp01-water-shallow-retail-side-by-side.png) · [overlay](cp01-water-shallow-retail/comparison/cp01-water-shallow-retail-overlay-50.png) · [diff](cp01-water-shallow-retail/comparison/cp01-water-shallow-retail-absolute-diff.png) · [comparison](cp01-water-shallow-retail/comparison/cp01-water-shallow-retail-comparison.json) · [OpenNova manifest](cp01-water-shallow-retail/opennova/cp01-water-shallow-retail-manifest.json) · [registration](cp01-water-shallow-retail/retail/registered.json) |
| `cp01-water-steep-retail` | `CP01.bms` / 0930 | 19.405687 / 27.801169 | 13.180575 / 18.907566 | 23.867796 / 34.836117 | [side-by-side](cp01-water-steep-retail/comparison/cp01-water-steep-retail-side-by-side.png) · [overlay](cp01-water-steep-retail/comparison/cp01-water-steep-retail-overlay-50.png) · [diff](cp01-water-steep-retail/comparison/cp01-water-steep-retail-absolute-diff.png) · [comparison](cp01-water-steep-retail/comparison/cp01-water-steep-retail-comparison.json) · [OpenNova manifest](cp01-water-steep-retail/opennova/cp01-water-steep-retail-manifest.json) · [registration](cp01-water-steep-retail/retail/registered.json) |
| `cp01-water-wide-retail` | `CP01.bms` / 0930 | 21.141877 / 31.706793 | 25.844022 / 35.689733 | 21.900063 / 34.099329 | [side-by-side](cp01-water-wide-retail/comparison/cp01-water-wide-retail-side-by-side.png) · [overlay](cp01-water-wide-retail/comparison/cp01-water-wide-retail-overlay-50.png) · [diff](cp01-water-wide-retail/comparison/cp01-water-wide-retail-absolute-diff.png) · [comparison](cp01-water-wide-retail/comparison/cp01-water-wide-retail-comparison.json) · [OpenNova manifest](cp01-water-wide-retail/opennova/cp01-water-wide-retail-manifest.json) · [registration](cp01-water-wide-retail/retail/registered.json) |
| `cp04-checkpoint-fires-retail` | `CP04.bms` / 0120 | 19.023571 / 23.399585 | 24.732843 / 29.754500 | 17.405990 / 21.904662 | [side-by-side](cp04-checkpoint-fires-retail/comparison/cp04-checkpoint-fires-retail-side-by-side.png) · [overlay](cp04-checkpoint-fires-retail/comparison/cp04-checkpoint-fires-retail-overlay-50.png) · [diff](cp04-checkpoint-fires-retail/comparison/cp04-checkpoint-fires-retail-absolute-diff.png) · [comparison](cp04-checkpoint-fires-retail/comparison/cp04-checkpoint-fires-retail-comparison.json) · [OpenNova manifest](cp04-checkpoint-fires-retail/opennova/cp04-checkpoint-fires-retail-manifest.json) · [registration](cp04-checkpoint-fires-retail/retail/registered.json) |
| `cp12-truck-material-retail` | `CP12.bms` / 1260 | 19.758317 / 23.767711 | 14.586029 / 17.024130 | 18.285745 / 22.397730 | [side-by-side](cp12-truck-material-retail/comparison/cp12-truck-material-retail-side-by-side.png) · [overlay](cp12-truck-material-retail/comparison/cp12-truck-material-retail-overlay-50.png) · [diff](cp12-truck-material-retail/comparison/cp12-truck-material-retail-absolute-diff.png) · [comparison](cp12-truck-material-retail/comparison/cp12-truck-material-retail-comparison.json) · [OpenNova manifest](cp12-truck-material-retail/opennova/cp12-truck-material-retail-manifest.json) · [registration](cp12-truck-material-retail/retail/registered.json) |
| `cp12-yard-road-retail` | `CP12.bms` / 1260 | 19.164501 / 23.142895 | 18.025941 / 19.767878 | 15.631935 / 18.942167 | [side-by-side](cp12-yard-road-retail/comparison/cp12-yard-road-retail-side-by-side.png) · [overlay](cp12-yard-road-retail/comparison/cp12-yard-road-retail-overlay-50.png) · [diff](cp12-yard-road-retail/comparison/cp12-yard-road-retail-absolute-diff.png) · [comparison](cp12-yard-road-retail/comparison/cp12-yard-road-retail-comparison.json) · [OpenNova manifest](cp12-yard-road-retail/opennova/cp12-yard-road-retail-manifest.json) · [registration](cp12-yard-road-retail/retail/registered.json) |
| `cp12-yard-tanks-retail` | `CP12.bms` / 1260 | 18.855067 / 21.631795 | 16.635454 / 17.570797 | 18.702820 / 21.290102 | [side-by-side](cp12-yard-tanks-retail/comparison/cp12-yard-tanks-retail-side-by-side.png) · [overlay](cp12-yard-tanks-retail/comparison/cp12-yard-tanks-retail-overlay-50.png) · [diff](cp12-yard-tanks-retail/comparison/cp12-yard-tanks-retail-absolute-diff.png) · [comparison](cp12-yard-tanks-retail/comparison/cp12-yard-tanks-retail-comparison.json) · [OpenNova manifest](cp12-yard-tanks-retail/opennova/cp12-yard-tanks-retail-manifest.json) · [registration](cp12-yard-tanks-retail/retail/registered.json) |

The five raw OpenNova variants in every row are `beauty`, `shadows_off`,
`lighting_only`, `unshaded`, and `directional_shadow_atlas`.

## Honest visual residuals

- Surface-water reflection shape, wave/noise phase, clouds, brightness, and
  shoreline shading still diverge. Waterline/glare ordering and underwater
  murk remain non-identical.
- Fire rows retain unsynchronized flame/smoke phase, blur, warm spill, and
  particle differences. CP04 also remains substantially underexposed in its
  night ambient response and contains foliage and live-actor phase.
- CP12 retains night exposure, residual road-marking contrast, vegetation and
  ground sampling, vehicle/material response, and live NPC or flag phase.
- The M16 and bare-arm identity is matched, but viewmodel placement, lighting,
  material response, and animation phase remain different.
- Ground tire marks and other ordered .til overlay contributions diverge in
  placement and blend (the open D-TERRAIN-7 tile-composition producer gap,
  measured by the tire-marks fixture).
- Model-authored `LGHT` records and transient glows now drive pooled native
  `OmniLight3D`/`SpotLight3D` nodes; corona billboards remain a separate port
  of the witnessed three-segment EffectWorld walk. The facelift intentionally
  does not reproduce retail's per-draw nearest-four owner/interior-group
  selection: Godot admits all spatially overlapping live lights, uses native
  attenuation/specular with light shadows disabled, and maps authored Target
  records to spotlights even though JO's runtime spot delivery is dead.
  Terrain/object participation is layer-coarse, while bone following and
  batched-static owner/corona visibility remain incomplete (D-RLIT-4,
  measured by the armory-lght fixture).
- Sun, sky-dome, and ambient response diverge at low sun: iris/ambient
  sampling (D-RLIT-2), the sun-glint/reflection stand-ins (D-RLIT-5), and the
  deferred overcast/TOD first-pass table (env #16), measured by the 03TR
  sun-sky fixture.

This package supports qualitative scene-by-scene review. It does not claim
pixel parity or complete water, sky, particles, vegetation, lighting,
post-processing, or renderer parity.
