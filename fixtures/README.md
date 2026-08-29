# fixtures/ — test inputs

Every file under `fixtures/` belongs to one of three classes:

- **Minted** — produced by a `tests/fixtures/minimal_*_gen.cpp` generator through
  one of our own writers from small integer data, and byte-compared by that
  generator's ctest on every run. To change one, change the generator, run its
  binary with `--write`, and commit the files it wrote. Never edit a minted file
  by hand and never carve one out of retail bytes (ADR 0003).
- **Authored** — text we wrote (test `.def` rows, menus, the one-bone `.bad`
  clips).
- **Keep** — the small retail-interop set: original files kept as-is so the
  parsers prove they read what the shipped game wrote.

Retail corpora (whole installs, extracted asset trees, captures) never live here;
tests that need them run behind the `OPENNOVA_*` gates
(`docs/asset-gated-tests.md`); the retail files those gated tests need in CI live
in the private `opennova-net/opennova-reference-assets` repository.

## Rules (enforced by `scripts/lint/fixture_lint.py`)

- Every file is one of the three classes: minted files are named (by path or
  directory) in the `tests/fixtures/minimal_*_gen.cpp` that writes them; authored
  and keep files carry a row in `scripts/lint/fixture_allowlist.json`. A retail
  blob outside the keep rows fails the lint, and a keep row is a deliberate
  decision recorded there and in the table below.
- Every file is referenced by a test, the ctest registration, a workflow, a script,
  or a doc (this README counts): a fixture nothing reads is deleted, not kept.
- Every file is LFS-tracked (`.gitattributes` `fixtures/**`), except the text
  carve-out (`*.md`, `.gitignore`), and no tracked file under `fixtures/` or
  `assets/` exceeds 2 MiB; the three oversize `assets/mnml*` files carry a reason
  in the allowlist's `size_exceptions`.
- CI runs the lint with `--require-pulled`: every LFS fixture must be materialized
  in the checkout the tests read (the scoped `git lfs pull` in `ci.yml`).

Run `python scripts/lint/fixture_lint.py --report` locally; `--enforce` is the CI
gate.

## The keep set

Original files kept byte-for-byte because a test proves the parser reads what the
shipped game wrote (or because a wire test replays what a retail peer sent).
Everything else that ever came from a retail install was replaced by a minted or
authored file.

| Files | Why they stay |
|---|---|
| `mnu/jo_*.mnu` (15), `mns/menu_style.mns` | the shipped JO menu screens and style: the menu compiler, the widget tests and the GUT shells pin retail widget ids and layouts |
| `def/weapon.def`, `def/ammo.def`, `def/hudpos.def` | the wire-visible weapon/ammo order (the weapon table bakes indices from it) and the HUD layout table |
| `bms/ash_i5b.reference.bms` | the mission reference the BMS writer round-trips against |
| `mus/jo_gamemus.bin`, `mus/jo_menumus.bin`, `mus/golden_jo_gamemus.mus.txt` | the music director scripts and their decoded golden |
| `adm/mp5_1st.adm`, `bad/BINOC.bad`, `bad/BINOC_twist.bad` | the retail animation-definition and clip formats (skinned-rig twist parity) |
| `particle/*.ptl` | the particle catalogue parser's retail corpus |
| `cbin/nlist*.reference.kda` | the credits-list container in its JO, JOX01 and BHD encodings |
| `avatars/Avatars.def` | the avatar definition table |
| `novaworld/**` | NovaWorld/in-game wire captures and manifests the codec tests replay (retail bytes are the product here) |

## Authored files

`def/items.def` (test rows 106100..), `terrain/tmap/items.def`, `mnu/widgets.mnu`,
`mnu/all_widgets.mnu`, `mns/test_style.mns`, `score/score_sample.ini`,
`anim/*` (the one-bone `.bad` clips, `soldier.adm`, `US01.adm`),
`particle/gorehit.ptu` (the gore-set half of the effect catalog, written in the
retail `.ptu` grammar with our own effect), the `novaworld/*_manifest.txt`
records, and this README.

## threedi/synth — the synthetic 3DI model set (minted)

`tests/fixtures/minimal_3di_gen.cpp` authors every model in memory
(`tests/fixtures/minimal_3di_builder.h`, mission axes: x forward, y left, z up,
origin on the ground unless noted) and writes it through `threedi_3di3_write`.
Eleven base models stand in for the retail models the tests once loaded, shaped
after what those tests key on:

| Model | Shape |
|---|---|
| `crate` | one part, one box; COBJ 0 with a 12-face box and one CB volume; user point `ground` |
| `gun` | receiver + barrel parts, no collision block (a held weapon); `MFlash01` on the barrel (row 1 sits on a drawing part off the root), `Bullet01` and `bcasing` on the root (a one-bone rig can carry the muzzle), `ground` |
| `shed` | one part: an opaque hut plus one alpha bulb strip; exactly ONE light (style 24, atten 0..3 at mission (0, 0, 1.25), subobject 0); CTRL `FLICKER`; two CB volumes |
| `house` | one inert part, three opaque `FF_ST_OP` strips, no lights; 3 CB + one octagonal prism (a multi-plane hull); no OOBJ |
| `bird` | nine skinned parts (the Bird1 hierarchy), two collision faces per COBJ (18), no BVOL |
| `person` | nineteen skinned parts in the retail person rig order (14 = head, 16 = left hand), origin at the pelvis; COBJ 0 a face box, every other COBJ a bone sphere (14: parent 13, center (1/16, 0, 13/16), radius 5/32); `bullet`/`MFlash01` on bone 16, `LOOK` on 14 |
| `pump` | two LODs x five parts (base, beam, head, rod, weight); LOD0's beam row is a free-running sine on rotation z (control 50), every other row inert; each part owns a collision face box, COBJ 0 two CB volumes |
| `armory` | four parts (exterior shell, east, west, middle rooms); material 3 = the `FLICKER`-driven `FF_ST_OP_LUM` bulb; two spatially separated alpha strips (`FFP_GLASS`, `FF_ST_AB`); two lights; COBJ faces [12, 8, 2, 2]; volumes 5 CB, 1 CA, 1 VC, 1 CL, 3 BB (east 0x2E, west 0x28, middle 0x2E); the retail 00TRa floor plan's OCCL topology (open box, four wall occluders, the east room's south window, portals east<->middle and middle<->west); user points `Armory`, `Ground` |
| `mount` | base + cradle + gun; CTRL `HEAT_GLOW`, `EWEAP_GUNYAW`, `EWEAP_GUNPITCH`; the cradle yaws on register 1, the gun yaws on 1 and pitches on 2; user points `BCasing`, `Bullet`, `Camera`, `heat`, `MFlash01`, `Usegun` (row 6); one CB per part |
| `carrier` | hull + cabin; the dsuv1 user-point order (`ctrlx13` first, then `ewep01`, `sitex00d`, `sitex08c`, `FX01`, `ground`, `sitex06b`, `sitex12a`); CTRL `VEHICLE_STEERING`, `VEHICLE_WHEELS`, `VEHICLE_TIRE00..03`; the cabin COBJ owns its own CVRT run and the sole CXLT (2, 0, 1/2) |
| `tank` | hull + turret; `ctrlx25` (row 1), `ewep01` on the turret, `fx00`, `ground`; the turret yaws on `VEHICLE_GUNYAW`; two CP track volumes |

The variants are one authored edit each over a base, spelled the way the
retired `ObjectData` editing surface spelled them ("delete rows" clears the
LOD0 PANM block; "slide(part)" adds one row translating that part on x over
control register 0, values 0..4, no speed):

| File | Base | Edit | Consumer |
|---|---|---|---|
| `mount_ctrl1_heat_glow` | mount | CTRL 1 renamed `HEAT_GLOW` | `object_data_ctrl_bus_test.gd` duplicate alias |
| `mount_ctrl1_not_retail` | mount | CTRL 1 renamed `NOT_RETAIL` | ctrl_bus unknown alias |
| `mount_yaw_style114` | mount | the first LOD0 track with control 113 on register 1 (the cradle's yaw) becomes style 114 | ctrl_bus wave style, normal |
| `mount_ctrl1_lod_frac_yaw_style114` | mount | CTRL 1 renamed `LOD_FRAC` + the same track edit | ctrl_bus wave style, patched |
| `mount_mtrl0_rgbgen113_reg1` | mount | material 0 RGB generator style 113 on register 1, black to white | ctrl_bus material alias |
| `armory_lght0_colorgen113_flicker` | armory | light 0: style 113, phase 0, black to white, objects enabled | ctrl_bus light bus |
| `pump_anim0_noise_translation` | pump | LOD0 row 0: translation z enabled, control 0x36, end 32767 | `object_data_panm_apply_test.gd` same-time noise |
| `shed_lght0_sub2_origin_atten100` | shed | light 0: subobject 2, position zero, atten_end 100 | `effect_light_world_test.gd` exact ROBJ row; `per_model_light_isolation_test.gd` owner scope |
| `armory_lght0_sub1_offset` | armory | light 0: subobject 1, Godot position (0.25, 0.5, -0.75), atten_end 1000 | effect_light spawn-time matrix |
| `house_lod0_sine_rotx` | house | one appended LOD0 row: rotation x sine 0..90 deg at speed 1 | `terrain_static_shadow_runtime_test.gd` resident pages |
| `house_lod0_sine_rotx_uv1` | house | the previous edit + material 0 `uv_u_style` 1 | terrain dynamic-UV phase |
| `house_mtrl0_uvscroll16_alphatest` | house | material 0 alpha test on, `uv_u_style` 16, `uv_u_rate` 1.0 | terrain worker snapshot |
| `mount_heat_glow_slide_part1` | mount | delete rows; slide(1) on `HEAT_GLOW` | `simulation_test.gd` heat glow |
| `armory_special1_slide_part1` | armory | CTRL 0 renamed `VEHICLE_SPECIAL1`; delete rows; slide(1) | simulation animated collision + FastRope SPECIAL1 |
| `armory_special2_slide_part1` | armory | CTRL 0 renamed `VEHICLE_SPECIAL2`; delete rows; slide(1) | simulation FastRope SPECIAL2 |
| `tank_special1_slide_ewep01` | tank | CTRL 0 renamed `VEHICLE_SPECIAL1`; delete rows; slide(the part that owns `ewep01`) | simulation listen-snapshot attachment |
| `pump_lod0_inert_lod1_sine_rotz` | pump | LOD0: one inert row on part 0; LOD1: one rotation z sine row | simulation effective LOD0 collision |
| `panm_live_01_spinner` .. `panm_inert_10_rotrev` | shed | delete rows; one row on part 0 with flags F and every track control 0 except the live tracks (control 0x10). (F, live): 01 `1<<8` none; 02 `3<<8` none; 03 `4<<8` none; 04 `2<<8` rotation z; 05 `2<<8` scale x; 06 `1` scale y; 07 `1` scale x; 08 `2` scale y; 09 `1<<24` translation; 10 `1<<16` none | simulation PANM liveness family |

Every consumer that pins a number pins the authored one: the bird's 18 faces,
the armory's 24 collision faces (COBJ 1's 8 faces = 24 vertices move under a
slide, the other 48 stay), the person's head sphere. Retail-only assertions
(the six retail controlled models' PANM/CTRL bytes, the 00TRa armory's own
occlusion walk) run as `OPENNOVA_JO_ASSETS` legs of the same ctests.

## Other minted sets

- `terrain/tmap` — `tests/fixtures/minimal_terrain_gen.cpp` (the second map beside
  `assets/mnml`).
- `fnt/synth_*.fnt`, `cbin/credits_image.png`, `cbin/particle_dot.tga` —
  `tests/fixtures/minimal_fnt_gen.cpp`, `minimal_cbin_gen.cpp` (the .tga is the
  sprite the effect-world test's synthetic particle file names).
- `env/synth_full.env`, `env/cloud01.pcx`, `env/cloud01b.pcx` — `tests/fixtures/minimal_env_gen.cpp`:
  the synthetic environment (every keyword authored, ten time-of-day keyframes,
  written by `save_env`) and the two sky maps it names (the bright square-rooted
  cloud field and the modulation layer centered at 128), 64x64 indexed PCX by our
  writer. The shipped `.env` set is the gated `env_jo_install` sweep.
- `rtxt/synth_*.bin` — `tests/fixtures/minimal_rtxt_gen.cpp` (beside the `assets/`
  boot tables it also mints): the parity set `rtxt_synth_parity` grills — a
  multi-section game table with position hints, a menu table, a mission sidecar
  with cp1252 text and odd-length padding, and the one-entry case. The shipped
  string tables are read from the reference tree by the gated
  `rtxt_jo_install_sweep` and the menu-driven GUT tests.
- `sbf/synth_gamemus.sbf` — `tests/fixtures/minimal_sbf_gen.cpp`.
- `lwf/menu.lwf`, `lwf/tone.wav` — `tests/fixtures/minimal_lwf_gen.cpp`.
- `dbf/synth_bank.dbf` — `tests/fixtures/minimal_dbf_gen.cpp`: eleven dialog groups
  through `encode_dbf`; the shipped 00TRg bank is `dbf_roundtrip`'s reference-tree leg.
