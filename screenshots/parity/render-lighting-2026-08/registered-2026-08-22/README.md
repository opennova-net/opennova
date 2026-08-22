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
| Frozen OpenNova source | `269c1c8e727a16074f13e7b2dfdc4ba9ff3af42c` |
| Retail fixture catalog SHA-256 | `d5ea3d0548f9854e38f8d30ecf3c0119053edcf4496260b27b611e35a39c8f3e` |
| Godot executable SHA-256 | `1e5efe381f62ee1cea6bc18caac71c0c74bd6e68e6af5c6efb3dbe76628f61c7` |
| GDExtension SHA-256 | `8009f4fff847bc0092d4404fafd321fb7dedd777ceaa6eb55a4ddfd159e0e577` |
| Retail executable SHA-256 | `b9971c8273b7bbb1c8518a738596d669cd7794e9d307ae63a7a9a530eb802fac` |
| Retail video profile | `retail_reference_highest_retail_selectable_v2` |
| Retail capture producer | onHook 0.6.0; bridge 1.5; `opennova.render_capture_bundle.v4` |
| Retail registrar | 4.1.0; `opennova.registered-retail-capture.v5` |
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
Across all 18 fixtures, full-frame MAE spans `5.165582`-`25.923306`,
world-center MAE spans `2.439046`-`15.728697`, and viewmodel-arms MAE spans
`5.036941`-`32.155453`. These are exact channel deltas from each comparison
manifest, not thresholds or pixel-parity verdicts.

| Fixture | Mission / minute | Full MAE / RMS | World MAE / RMS | Arms MAE / RMS | Direct evidence |
|---|---:|---:|---:|---:|---|
| `00tra-armory-glass-retail` | `00TRa.bms` / 0900 | 8.046839 / 18.456542 | 6.988898 / 16.581326 | 13.807761 / 28.751207 | [side-by-side](00tra-armory-glass-retail/comparison/00tra-armory-glass-retail-side-by-side.png) · [overlay](00tra-armory-glass-retail/comparison/00tra-armory-glass-retail-overlay-50.png) · [diff](00tra-armory-glass-retail/comparison/00tra-armory-glass-retail-absolute-diff.png) · [comparison](00tra-armory-glass-retail/comparison/00tra-armory-glass-retail-comparison.json) · [OpenNova manifest](00tra-armory-glass-retail/opennova/00tra-armory-glass-retail-manifest.json) · [registration](00tra-armory-glass-retail/retail/registered.json) |
| `00tra-armory-lght-retail` | `00TRa.bms` / 0900 | 9.423458 / 12.480531 | 8.677817 / 10.121365 | 13.402264 / 17.862744 | [side-by-side](00tra-armory-lght-retail/comparison/00tra-armory-lght-retail-side-by-side.png) · [overlay](00tra-armory-lght-retail/comparison/00tra-armory-lght-retail-overlay-50.png) · [diff](00tra-armory-lght-retail/comparison/00tra-armory-lght-retail-absolute-diff.png) · [comparison](00tra-armory-lght-retail/comparison/00tra-armory-lght-retail-comparison.json) · [OpenNova manifest](00tra-armory-lght-retail/opennova/00tra-armory-lght-retail-manifest.json) · [registration](00tra-armory-lght-retail/retail/registered.json) |
| `00tra-courtyard-retail` | `00TRa.bms` / 0900 | 5.747050 / 10.955822 | 4.382448 / 10.195656 | 5.036941 / 7.533692 | [side-by-side](00tra-courtyard-retail/comparison/00tra-courtyard-retail-side-by-side.png) · [overlay](00tra-courtyard-retail/comparison/00tra-courtyard-retail-overlay-50.png) · [diff](00tra-courtyard-retail/comparison/00tra-courtyard-retail-absolute-diff.png) · [comparison](00tra-courtyard-retail/comparison/00tra-courtyard-retail-comparison.json) · [OpenNova manifest](00tra-courtyard-retail/opennova/00tra-courtyard-retail-manifest.json) · [registration](00tra-courtyard-retail/retail/registered.json) |
| `00tra-fire-barrel-close-retail` | `00TRa.bms` / 0900 | 14.728575 / 27.453201 | 8.233721 / 16.639302 | 18.671699 / 34.643456 | [side-by-side](00tra-fire-barrel-close-retail/comparison/00tra-fire-barrel-close-retail-side-by-side.png) · [overlay](00tra-fire-barrel-close-retail/comparison/00tra-fire-barrel-close-retail-overlay-50.png) · [diff](00tra-fire-barrel-close-retail/comparison/00tra-fire-barrel-close-retail-absolute-diff.png) · [comparison](00tra-fire-barrel-close-retail/comparison/00tra-fire-barrel-close-retail-comparison.json) · [OpenNova manifest](00tra-fire-barrel-close-retail/opennova/00tra-fire-barrel-close-retail-manifest.json) · [registration](00tra-fire-barrel-close-retail/retail/registered.json) |
| `00tra-fire-barrel-east-retail` | `00TRa.bms` / 0900 | 8.644017 / 16.987979 | 8.593755 / 16.036253 | 11.387717 / 19.272510 | [side-by-side](00tra-fire-barrel-east-retail/comparison/00tra-fire-barrel-east-retail-side-by-side.png) · [overlay](00tra-fire-barrel-east-retail/comparison/00tra-fire-barrel-east-retail-overlay-50.png) · [diff](00tra-fire-barrel-east-retail/comparison/00tra-fire-barrel-east-retail-absolute-diff.png) · [comparison](00tra-fire-barrel-east-retail/comparison/00tra-fire-barrel-east-retail-comparison.json) · [OpenNova manifest](00tra-fire-barrel-east-retail/opennova/00tra-fire-barrel-east-retail-manifest.json) · [registration](00tra-fire-barrel-east-retail/retail/registered.json) |
| `00tra-fire-barrel-full-composite-retail` | `00TRa.bms` / 0900 | 8.694505 / 17.079427 | 8.729054 / 16.058127 | 11.426744 / 19.329277 | [side-by-side](00tra-fire-barrel-full-composite-retail/comparison/00tra-fire-barrel-full-composite-retail-side-by-side.png) · [overlay](00tra-fire-barrel-full-composite-retail/comparison/00tra-fire-barrel-full-composite-retail-overlay-50.png) · [diff](00tra-fire-barrel-full-composite-retail/comparison/00tra-fire-barrel-full-composite-retail-absolute-diff.png) · [comparison](00tra-fire-barrel-full-composite-retail/comparison/00tra-fire-barrel-full-composite-retail-comparison.json) · [OpenNova manifest](00tra-fire-barrel-full-composite-retail/opennova/00tra-fire-barrel-full-composite-retail-manifest.json) · [registration](00tra-fire-barrel-full-composite-retail/retail/registered.json) |
| `00tra-tire-marks-retail` | `00TRa.bms` / 0900 | 10.191796 / 20.795704 | 4.283566 / 12.679590 | 21.066132 / 33.776296 | [side-by-side](00tra-tire-marks-retail/comparison/00tra-tire-marks-retail-side-by-side.png) · [overlay](00tra-tire-marks-retail/comparison/00tra-tire-marks-retail-overlay-50.png) · [diff](00tra-tire-marks-retail/comparison/00tra-tire-marks-retail-absolute-diff.png) · [comparison](00tra-tire-marks-retail/comparison/00tra-tire-marks-retail-comparison.json) · [OpenNova manifest](00tra-tire-marks-retail/opennova/00tra-tire-marks-retail-manifest.json) · [registration](00tra-tire-marks-retail/retail/registered.json) |
| `03tr-sun-sky-retail` | `03TR.bms` / 0390 | 25.923306 / 42.251571 | 8.913938 / 19.393931 | 32.155453 / 46.176745 | [side-by-side](03tr-sun-sky-retail/comparison/03tr-sun-sky-retail-side-by-side.png) · [overlay](03tr-sun-sky-retail/comparison/03tr-sun-sky-retail-overlay-50.png) · [diff](03tr-sun-sky-retail/comparison/03tr-sun-sky-retail-absolute-diff.png) · [comparison](03tr-sun-sky-retail/comparison/03tr-sun-sky-retail-comparison.json) · [OpenNova manifest](03tr-sun-sky-retail/opennova/03tr-sun-sky-retail-manifest.json) · [registration](03tr-sun-sky-retail/retail/registered.json) |
| `cp01-waterline-above-retail` | `CP01.bms` / 0930 | 13.520613 / 24.823012 | 13.188535 / 26.219583 | 21.823826 / 35.747959 | [side-by-side](cp01-waterline-above-retail/comparison/cp01-waterline-above-retail-side-by-side.png) · [overlay](cp01-waterline-above-retail/comparison/cp01-waterline-above-retail-overlay-50.png) · [diff](cp01-waterline-above-retail/comparison/cp01-waterline-above-retail-absolute-diff.png) · [comparison](cp01-waterline-above-retail/comparison/cp01-waterline-above-retail-comparison.json) · [OpenNova manifest](cp01-waterline-above-retail/opennova/cp01-waterline-above-retail-manifest.json) · [registration](cp01-waterline-above-retail/retail/registered.json) |
| `cp01-waterline-below-retail` | `CP01.bms` / 0930 | 6.102730 / 9.581443 | 8.616417 / 12.578984 | 5.755077 / 8.481017 | [side-by-side](cp01-waterline-below-retail/comparison/cp01-waterline-below-retail-side-by-side.png) · [overlay](cp01-waterline-below-retail/comparison/cp01-waterline-below-retail-overlay-50.png) · [diff](cp01-waterline-below-retail/comparison/cp01-waterline-below-retail-absolute-diff.png) · [comparison](cp01-waterline-below-retail/comparison/cp01-waterline-below-retail-comparison.json) · [OpenNova manifest](cp01-waterline-below-retail/opennova/cp01-waterline-below-retail-manifest.json) · [registration](cp01-waterline-below-retail/retail/registered.json) |
| `cp01-water-oblique-retail` | `CP01.bms` / 0930 | 13.755154 / 24.613044 | 10.025057 / 17.341998 | 25.629497 / 36.695648 | [side-by-side](cp01-water-oblique-retail/comparison/cp01-water-oblique-retail-side-by-side.png) · [overlay](cp01-water-oblique-retail/comparison/cp01-water-oblique-retail-overlay-50.png) · [diff](cp01-water-oblique-retail/comparison/cp01-water-oblique-retail-absolute-diff.png) · [comparison](cp01-water-oblique-retail/comparison/cp01-water-oblique-retail-comparison.json) · [OpenNova manifest](cp01-water-oblique-retail/opennova/cp01-water-oblique-retail-manifest.json) · [registration](cp01-water-oblique-retail/retail/registered.json) |
| `cp01-water-shallow-retail` | `CP01.bms` / 0930 | 16.931444 / 28.857594 | 15.728697 / 28.734156 | 26.514019 / 40.610694 | [side-by-side](cp01-water-shallow-retail/comparison/cp01-water-shallow-retail-side-by-side.png) · [overlay](cp01-water-shallow-retail/comparison/cp01-water-shallow-retail-overlay-50.png) · [diff](cp01-water-shallow-retail/comparison/cp01-water-shallow-retail-absolute-diff.png) · [comparison](cp01-water-shallow-retail/comparison/cp01-water-shallow-retail-comparison.json) · [OpenNova manifest](cp01-water-shallow-retail/opennova/cp01-water-shallow-retail-manifest.json) · [registration](cp01-water-shallow-retail/retail/registered.json) |
| `cp01-water-steep-retail` | `CP01.bms` / 0930 | 13.756026 / 23.697487 | 9.465911 / 14.840455 | 26.361627 / 39.862100 | [side-by-side](cp01-water-steep-retail/comparison/cp01-water-steep-retail-side-by-side.png) · [overlay](cp01-water-steep-retail/comparison/cp01-water-steep-retail-overlay-50.png) · [diff](cp01-water-steep-retail/comparison/cp01-water-steep-retail-absolute-diff.png) · [comparison](cp01-water-steep-retail/comparison/cp01-water-steep-retail-comparison.json) · [OpenNova manifest](cp01-water-steep-retail/opennova/cp01-water-steep-retail-manifest.json) · [registration](cp01-water-steep-retail/retail/registered.json) |
| `cp01-water-wide-retail` | `CP01.bms` / 0930 | 15.122586 / 27.277023 | 11.838328 / 20.986683 | 27.181356 / 40.798577 | [side-by-side](cp01-water-wide-retail/comparison/cp01-water-wide-retail-side-by-side.png) · [overlay](cp01-water-wide-retail/comparison/cp01-water-wide-retail-overlay-50.png) · [diff](cp01-water-wide-retail/comparison/cp01-water-wide-retail-absolute-diff.png) · [comparison](cp01-water-wide-retail/comparison/cp01-water-wide-retail-comparison.json) · [OpenNova manifest](cp01-water-wide-retail/opennova/cp01-water-wide-retail-manifest.json) · [registration](cp01-water-wide-retail/retail/registered.json) |
| `cp04-checkpoint-fires-retail` | `CP04.bms` / 0120 | 8.508073 / 13.148095 | 11.973596 / 18.936039 | 7.220615 / 10.667875 | [side-by-side](cp04-checkpoint-fires-retail/comparison/cp04-checkpoint-fires-retail-side-by-side.png) · [overlay](cp04-checkpoint-fires-retail/comparison/cp04-checkpoint-fires-retail-overlay-50.png) · [diff](cp04-checkpoint-fires-retail/comparison/cp04-checkpoint-fires-retail-absolute-diff.png) · [comparison](cp04-checkpoint-fires-retail/comparison/cp04-checkpoint-fires-retail-comparison.json) · [OpenNova manifest](cp04-checkpoint-fires-retail/opennova/cp04-checkpoint-fires-retail-manifest.json) · [registration](cp04-checkpoint-fires-retail/retail/registered.json) |
| `cp12-truck-material-retail` | `CP12.bms` / 1260 | 5.873995 / 9.490161 | 3.556387 / 6.745145 | 9.353570 / 14.006542 | [side-by-side](cp12-truck-material-retail/comparison/cp12-truck-material-retail-side-by-side.png) · [overlay](cp12-truck-material-retail/comparison/cp12-truck-material-retail-overlay-50.png) · [diff](cp12-truck-material-retail/comparison/cp12-truck-material-retail-absolute-diff.png) · [comparison](cp12-truck-material-retail/comparison/cp12-truck-material-retail-comparison.json) · [OpenNova manifest](cp12-truck-material-retail/opennova/cp12-truck-material-retail-manifest.json) · [registration](cp12-truck-material-retail/retail/registered.json) |
| `cp12-yard-road-retail` | `CP12.bms` / 1260 | 5.187522 / 8.260961 | 2.439046 / 4.567718 | 6.685775 / 9.992210 | [side-by-side](cp12-yard-road-retail/comparison/cp12-yard-road-retail-side-by-side.png) · [overlay](cp12-yard-road-retail/comparison/cp12-yard-road-retail-overlay-50.png) · [diff](cp12-yard-road-retail/comparison/cp12-yard-road-retail-absolute-diff.png) · [comparison](cp12-yard-road-retail/comparison/cp12-yard-road-retail-comparison.json) · [OpenNova manifest](cp12-yard-road-retail/opennova/cp12-yard-road-retail-manifest.json) · [registration](cp12-yard-road-retail/retail/registered.json) |
| `cp12-yard-tanks-retail` | `CP12.bms` / 1260 | 5.165582 / 8.107877 | 3.053372 / 4.255949 | 5.579677 / 8.238896 | [side-by-side](cp12-yard-tanks-retail/comparison/cp12-yard-tanks-retail-side-by-side.png) · [overlay](cp12-yard-tanks-retail/comparison/cp12-yard-tanks-retail-overlay-50.png) · [diff](cp12-yard-tanks-retail/comparison/cp12-yard-tanks-retail-absolute-diff.png) · [comparison](cp12-yard-tanks-retail/comparison/cp12-yard-tanks-retail-comparison.json) · [OpenNova manifest](cp12-yard-tanks-retail/opennova/cp12-yard-tanks-retail-manifest.json) · [registration](cp12-yard-tanks-retail/retail/registered.json) |

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
- Retail frames are SETTLED captures: >= 4 s after the fixture apply with the
  clock pinned by the hook's capture-frame fixture-binding witness, so the
  teleport transient the 2026-08-20 publication sampled is gone and the
  cameras are the ground-snapped settled poses (net-re section 5.40 ninth
  pass; runbook section 8). Deep-water rows keep the fast capture (float
  physics pins the pose; D-INF-3).
- The M16 and bare-arm identity and PLACEMENT are matched (bone-exact and
  counter-gated, D-INF-14 FIXED), but viewmodel lighting, material response,
  and animated phase remain different. The deep-water CP01 rows additionally
  show retail's swimming raised-rifle hold (Flags 0x8000) against our hip
  hold (D-INF-3's first-person consequence), so those pairs are not
  viewmodel-placement evidence.
- Ground tire marks and other ordered .til overlay contributions diverge in
  placement and blend (the open D-TERRAIN-7 tile-composition producer gap,
  measured by the tire-marks fixture).
- Model-authored `LGHT` lamp delivery diverges: corona billboards are not
  drawn, Target/spot cones are dropped, and batched static buildings lose the
  authored subobject owner scope (the open D-RLIT-4 residual tail, measured by
  the armory-lght fixture).
- Sun, sky-dome, and ambient response diverge at low sun: iris/ambient
  sampling (D-RLIT-2), the sun-glint/reflection stand-ins (D-RLIT-5), and the
  deferred overcast/TOD first-pass table (env #16), measured by the 03TR
  sun-sky fixture.

This package supports qualitative scene-by-scene review. It does not claim
pixel parity or complete water, sky, particles, vegetation, lighting,
post-processing, or renderer parity.
