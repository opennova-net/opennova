# Registered OpenNova / retail maximum-quality lighting review

This publication contains 16 registered comparisons across `00TRa.bms`,
`CP01.bms`, `CP04.bms`, and `CP12.bms`. Every pair keeps terrain and the
first-person viewmodel active, uses the M16 Burst with plain bare
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
| Frozen OpenNova source | `3ada00e96b055075db4442ac7724ee08fee19325` |
| Retail fixture catalog SHA-256 | `8b21c2a0feed2e55ac11fd555f9ad96b9c533d49069c6ce0780b2984d62982ef` |
| Godot executable SHA-256 | `1e5efe381f62ee1cea6bc18caac71c0c74bd6e68e6af5c6efb3dbe76628f61c7` |
| GDExtension SHA-256 | `3a35e9065f23fa576e45e82819222d3b3d6b379c0e21d08454d2c75f4281af65` |
| Retail executable SHA-256 | `b9971c8273b7bbb1c8518a738596d669cd7794e9d307ae63a7a9a530eb802fac` |
| Retail video profile | `retail_reference_highest_retail_selectable_v2` |
| Retail capture producer | onHook 0.5.0; bridge 1.4; `opennova.render_capture_bundle.v4` |
| Retail registrar | 4.0.0; `opennova.registered-retail-capture.v5` |
| Retail staging | 3.0.0; `opennova.retail-presentation-stage.v3` |
| Comparison builder | 4.0.0; `opennova.retail-comparison.v6` |

Including this index, the package contains 305 files: 160 PNGs and 144 JSON
records plus this README. The PNGs comprise 80 raw OpenNova diagnostic
variants, 16 retail frames, 16 normalized OpenNova frames, 16 side-by-sides,
16 overlays, and 16 absolute differences. No backup, restore token,
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
Across all 16 fixtures, full-frame MAE spans `5.037678`-`21.001876`,
world-center MAE spans `2.493596`-`23.876309`, and viewmodel-arms MAE spans
`6.092216`-`29.890922`. These are exact channel deltas from each comparison
manifest, not thresholds or pixel-parity verdicts.

| Fixture | Mission / minute | Full MAE / RMS | World MAE / RMS | Arms MAE / RMS | Direct evidence |
|---|---:|---:|---:|---:|---|
| `00tra-armory-glass-retail` | `00TRa.bms` / 0900 | 8.690916 / 19.565257 | 7.275807 / 16.935571 | 15.908614 / 31.506963 | [side-by-side](00tra-armory-glass-retail/comparison/00tra-armory-glass-retail-side-by-side.png) · [overlay](00tra-armory-glass-retail/comparison/00tra-armory-glass-retail-overlay-50.png) · [diff](00tra-armory-glass-retail/comparison/00tra-armory-glass-retail-absolute-diff.png) · [comparison](00tra-armory-glass-retail/comparison/00tra-armory-glass-retail-comparison.json) · [OpenNova manifest](00tra-armory-glass-retail/opennova/00tra-armory-glass-retail-manifest.json) · [registration](00tra-armory-glass-retail/retail/registered.json) |
| `00tra-courtyard-retail` | `00TRa.bms` / 0900 | 6.120141 / 11.654871 | 3.430199 / 8.407174 | 8.289013 / 13.097392 | [side-by-side](00tra-courtyard-retail/comparison/00tra-courtyard-retail-side-by-side.png) · [overlay](00tra-courtyard-retail/comparison/00tra-courtyard-retail-overlay-50.png) · [diff](00tra-courtyard-retail/comparison/00tra-courtyard-retail-absolute-diff.png) · [comparison](00tra-courtyard-retail/comparison/00tra-courtyard-retail-comparison.json) · [OpenNova manifest](00tra-courtyard-retail/opennova/00tra-courtyard-retail-manifest.json) · [registration](00tra-courtyard-retail/retail/registered.json) |
| `00tra-fire-barrel-close-retail` | `00TRa.bms` / 0900 | 19.344633 / 31.356205 | 19.795218 / 31.015380 | 26.321509 / 42.273008 | [side-by-side](00tra-fire-barrel-close-retail/comparison/00tra-fire-barrel-close-retail-side-by-side.png) · [overlay](00tra-fire-barrel-close-retail/comparison/00tra-fire-barrel-close-retail-overlay-50.png) · [diff](00tra-fire-barrel-close-retail/comparison/00tra-fire-barrel-close-retail-absolute-diff.png) · [comparison](00tra-fire-barrel-close-retail/comparison/00tra-fire-barrel-close-retail-comparison.json) · [OpenNova manifest](00tra-fire-barrel-close-retail/opennova/00tra-fire-barrel-close-retail-manifest.json) · [registration](00tra-fire-barrel-close-retail/retail/registered.json) |
| `00tra-fire-barrel-east-retail` | `00TRa.bms` / 0900 | 20.951091 / 31.339651 | 23.876309 / 32.218743 | 20.370430 / 33.068380 | [side-by-side](00tra-fire-barrel-east-retail/comparison/00tra-fire-barrel-east-retail-side-by-side.png) · [overlay](00tra-fire-barrel-east-retail/comparison/00tra-fire-barrel-east-retail-overlay-50.png) · [diff](00tra-fire-barrel-east-retail/comparison/00tra-fire-barrel-east-retail-absolute-diff.png) · [comparison](00tra-fire-barrel-east-retail/comparison/00tra-fire-barrel-east-retail-comparison.json) · [OpenNova manifest](00tra-fire-barrel-east-retail/opennova/00tra-fire-barrel-east-retail-manifest.json) · [registration](00tra-fire-barrel-east-retail/retail/registered.json) |
| `00tra-fire-barrel-full-composite-retail` | `00TRa.bms` / 0900 | 21.001876 / 31.378049 | 23.838137 / 32.130388 | 20.651428 / 33.424834 | [side-by-side](00tra-fire-barrel-full-composite-retail/comparison/00tra-fire-barrel-full-composite-retail-side-by-side.png) · [overlay](00tra-fire-barrel-full-composite-retail/comparison/00tra-fire-barrel-full-composite-retail-overlay-50.png) · [diff](00tra-fire-barrel-full-composite-retail/comparison/00tra-fire-barrel-full-composite-retail-absolute-diff.png) · [comparison](00tra-fire-barrel-full-composite-retail/comparison/00tra-fire-barrel-full-composite-retail-comparison.json) · [OpenNova manifest](00tra-fire-barrel-full-composite-retail/opennova/00tra-fire-barrel-full-composite-retail-manifest.json) · [registration](00tra-fire-barrel-full-composite-retail/retail/registered.json) |
| `00tra-tire-marks-retail` | `00TRa.bms` / 0900 | 12.135816 / 23.732107 | 5.962469 / 14.352992 | 25.561383 / 40.209794 | [side-by-side](00tra-tire-marks-retail/comparison/00tra-tire-marks-retail-side-by-side.png) · [overlay](00tra-tire-marks-retail/comparison/00tra-tire-marks-retail-overlay-50.png) · [diff](00tra-tire-marks-retail/comparison/00tra-tire-marks-retail-absolute-diff.png) · [comparison](00tra-tire-marks-retail/comparison/00tra-tire-marks-retail-comparison.json) · [OpenNova manifest](00tra-tire-marks-retail/opennova/00tra-tire-marks-retail-manifest.json) · [registration](00tra-tire-marks-retail/retail/registered.json) |
| `cp01-waterline-above-retail` | `CP01.bms` / 0930 | 14.292488 / 27.358425 | 15.712744 / 29.245664 | 24.992553 / 41.295356 | [side-by-side](cp01-waterline-above-retail/comparison/cp01-waterline-above-retail-side-by-side.png) · [overlay](cp01-waterline-above-retail/comparison/cp01-waterline-above-retail-overlay-50.png) · [diff](cp01-waterline-above-retail/comparison/cp01-waterline-above-retail-absolute-diff.png) · [comparison](cp01-waterline-above-retail/comparison/cp01-waterline-above-retail-comparison.json) · [OpenNova manifest](cp01-waterline-above-retail/opennova/cp01-waterline-above-retail-manifest.json) · [registration](cp01-waterline-above-retail/retail/registered.json) |
| `cp01-waterline-below-retail` | `CP01.bms` / 0930 | 5.037678 / 9.069786 | 7.538534 / 11.694446 | 6.092216 / 9.236795 | [side-by-side](cp01-waterline-below-retail/comparison/cp01-waterline-below-retail-side-by-side.png) · [overlay](cp01-waterline-below-retail/comparison/cp01-waterline-below-retail-overlay-50.png) · [diff](cp01-waterline-below-retail/comparison/cp01-waterline-below-retail-absolute-diff.png) · [comparison](cp01-waterline-below-retail/comparison/cp01-waterline-below-retail-comparison.json) · [OpenNova manifest](cp01-waterline-below-retail/opennova/cp01-waterline-below-retail-manifest.json) · [registration](cp01-waterline-below-retail/retail/registered.json) |
| `cp01-water-oblique-retail` | `CP01.bms` / 0930 | 15.187402 / 26.556088 | 15.129637 / 22.993806 | 27.614030 / 39.750538 | [side-by-side](cp01-water-oblique-retail/comparison/cp01-water-oblique-retail-side-by-side.png) · [overlay](cp01-water-oblique-retail/comparison/cp01-water-oblique-retail-overlay-50.png) · [diff](cp01-water-oblique-retail/comparison/cp01-water-oblique-retail-absolute-diff.png) · [comparison](cp01-water-oblique-retail/comparison/cp01-water-oblique-retail-comparison.json) · [OpenNova manifest](cp01-water-oblique-retail/opennova/cp01-water-oblique-retail-manifest.json) · [registration](cp01-water-oblique-retail/retail/registered.json) |
| `cp01-water-shallow-retail` | `CP01.bms` / 0930 | 15.944383 / 29.879899 | 17.678827 / 30.672314 | 28.731880 / 45.071777 | [side-by-side](cp01-water-shallow-retail/comparison/cp01-water-shallow-retail-side-by-side.png) · [overlay](cp01-water-shallow-retail/comparison/cp01-water-shallow-retail-overlay-50.png) · [diff](cp01-water-shallow-retail/comparison/cp01-water-shallow-retail-absolute-diff.png) · [comparison](cp01-water-shallow-retail/comparison/cp01-water-shallow-retail-comparison.json) · [OpenNova manifest](cp01-water-shallow-retail/opennova/cp01-water-shallow-retail-manifest.json) · [registration](cp01-water-shallow-retail/retail/registered.json) |
| `cp01-water-steep-retail` | `CP01.bms` / 0930 | 14.288099 / 26.157856 | 9.266619 / 15.103892 | 29.890922 / 45.618124 | [side-by-side](cp01-water-steep-retail/comparison/cp01-water-steep-retail-side-by-side.png) · [overlay](cp01-water-steep-retail/comparison/cp01-water-steep-retail-overlay-50.png) · [diff](cp01-water-steep-retail/comparison/cp01-water-steep-retail-absolute-diff.png) · [comparison](cp01-water-steep-retail/comparison/cp01-water-steep-retail-comparison.json) · [OpenNova manifest](cp01-water-steep-retail/opennova/cp01-water-steep-retail-manifest.json) · [registration](cp01-water-steep-retail/retail/registered.json) |
| `cp01-water-wide-retail` | `CP01.bms` / 0930 | 15.963505 / 29.369047 | 15.098767 / 24.902874 | 29.843827 / 45.305907 | [side-by-side](cp01-water-wide-retail/comparison/cp01-water-wide-retail-side-by-side.png) · [overlay](cp01-water-wide-retail/comparison/cp01-water-wide-retail-overlay-50.png) · [diff](cp01-water-wide-retail/comparison/cp01-water-wide-retail-absolute-diff.png) · [comparison](cp01-water-wide-retail/comparison/cp01-water-wide-retail-comparison.json) · [OpenNova manifest](cp01-water-wide-retail/opennova/cp01-water-wide-retail-manifest.json) · [registration](cp01-water-wide-retail/retail/registered.json) |
| `cp04-checkpoint-fires-retail` | `CP04.bms` / 0120 | 9.964115 / 15.776689 | 13.434408 / 21.411659 | 12.814006 / 19.686008 | [side-by-side](cp04-checkpoint-fires-retail/comparison/cp04-checkpoint-fires-retail-side-by-side.png) · [overlay](cp04-checkpoint-fires-retail/comparison/cp04-checkpoint-fires-retail-overlay-50.png) · [diff](cp04-checkpoint-fires-retail/comparison/cp04-checkpoint-fires-retail-absolute-diff.png) · [comparison](cp04-checkpoint-fires-retail/comparison/cp04-checkpoint-fires-retail-comparison.json) · [OpenNova manifest](cp04-checkpoint-fires-retail/opennova/cp04-checkpoint-fires-retail-manifest.json) · [registration](cp04-checkpoint-fires-retail/retail/registered.json) |
| `cp12-truck-material-retail` | `CP12.bms` / 1260 | 6.846845 / 10.645036 | 5.546595 / 8.624585 | 12.394496 / 17.280956 | [side-by-side](cp12-truck-material-retail/comparison/cp12-truck-material-retail-side-by-side.png) · [overlay](cp12-truck-material-retail/comparison/cp12-truck-material-retail-overlay-50.png) · [diff](cp12-truck-material-retail/comparison/cp12-truck-material-retail-absolute-diff.png) · [comparison](cp12-truck-material-retail/comparison/cp12-truck-material-retail-comparison.json) · [OpenNova manifest](cp12-truck-material-retail/opennova/cp12-truck-material-retail-manifest.json) · [registration](cp12-truck-material-retail/retail/registered.json) |
| `cp12-yard-road-retail` | `CP12.bms` / 1260 | 5.762818 / 9.101723 | 2.493596 / 4.830429 | 9.502017 / 13.169526 | [side-by-side](cp12-yard-road-retail/comparison/cp12-yard-road-retail-side-by-side.png) · [overlay](cp12-yard-road-retail/comparison/cp12-yard-road-retail-overlay-50.png) · [diff](cp12-yard-road-retail/comparison/cp12-yard-road-retail-absolute-diff.png) · [comparison](cp12-yard-road-retail/comparison/cp12-yard-road-retail-comparison.json) · [OpenNova manifest](cp12-yard-road-retail/opennova/cp12-yard-road-retail-manifest.json) · [registration](cp12-yard-road-retail/retail/registered.json) |
| `cp12-yard-tanks-retail` | `CP12.bms` / 1260 | 6.614629 / 9.665825 | 4.387105 / 6.679397 | 8.429986 / 12.479768 | [side-by-side](cp12-yard-tanks-retail/comparison/cp12-yard-tanks-retail-side-by-side.png) · [overlay](cp12-yard-tanks-retail/comparison/cp12-yard-tanks-retail-overlay-50.png) · [diff](cp12-yard-tanks-retail/comparison/cp12-yard-tanks-retail-absolute-diff.png) · [comparison](cp12-yard-tanks-retail/comparison/cp12-yard-tanks-retail-comparison.json) · [OpenNova manifest](cp12-yard-tanks-retail/opennova/cp12-yard-tanks-retail-manifest.json) · [registration](cp12-yard-tanks-retail/retail/registered.json) |

The five raw OpenNova variants in every row are `beauty`, `shadows_off`,
`lighting_only`, `unshaded`, and `directional_shadow_atlas`.

## Honest visual residuals

- Surface-water reflection shape, wave/noise phase, clouds, brightness, and
  shoreline shading still diverge. Waterline/glare ordering and underwater
  murk remain non-identical.
- Fire rows retain unsynchronized flame/smoke phase, blur, warm spill, and
  particle differences. CP04 also contains foliage and live-actor phase.
- CP12 retains night exposure, residual road-marking contrast, vegetation and
  ground sampling, vehicle/material response, and live NPC or flag phase.
- The M16 and bare-arm identity is matched, but viewmodel placement, lighting,
  material response, and animation phase remain different.
- Ground tire marks and other ordered .til overlay contributions diverge in
  placement and blend (the open D-TERRAIN-7 tile-composition producer gap,
  measured by the tire-marks fixture).

This package supports qualitative scene-by-scene review. It does not claim
pixel parity or complete water, sky, particles, vegetation, lighting,
post-processing, or renderer parity.
