# fixtures/ — test inputs

Every file under `fixtures/` belongs to one of three classes:

- **Minted** — produced by a `tests/fixtures/*_gen.cpp` generator through
  one of our own writers from small integer data, and byte-compared by that
  generator's ctest on every run. To change one, change the generator, run its
  binary with `--write`, and commit the files it wrote. Never edit a minted file
  by hand and never carve one out of retail bytes (ADR 0003). The one minted file
  no generator writes is `novaworld/self-capture-session.pcap`, an opennova
  self-capture that `nw_self_capture_test --write-fixture` regenerates.
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
  directory) in the `tests/fixtures/*_gen.cpp` that writes them (a test's
  self-capture carries a `minted_by` row instead); authored and
  keep files carry a row in `scripts/lint/fixture_allowlist.json`. A retail blob
  outside the keep rows fails the lint, and a keep row may live only under
  `novaworld/` (the lint refuses any other): every other retail file the tests
  read comes from the reference fixture set (`docs/asset-gated-tests.md`), never
  from this tree.
- Every file is referenced by a test, a probe, the ctest registration, a workflow,
  a script, or this README (a `docs/` mention does not count): a fixture nothing
  reads is deleted, not kept.
- Every binary file is LFS-tracked (`.gitattributes` `fixtures/**`) and every
  plain-text file (`.def`, `.mnu`, `.ptl`, the manifests, ...) is a plain git blob
  that diffs and reviews normally (the per-extension carve-outs there; the lint
  judges by content, so a new text format gets its extension carved out in the
  same change). No tracked file under `fixtures/` exceeds 2 MiB (the allowlist's
  `size_exceptions` is empty).
- CI runs the lint with `--require-pulled`: every LFS fixture must be materialized
  in the checkout the tests read (the scoped `git lfs pull` in `ci.yml`).

Run `python scripts/lint/fixture_lint.py --report` locally; `--enforce` is the CI
gate.

## The keep set

The NovaWorld wire captures, kept byte-for-byte because the codec tests replay
what a retail peer sent (the bytes are the product). Everything else that ever
came from a retail install was replaced by a minted or authored file, or moved to
the reference fixture set the gated tests read (`retail::reference_fixture`,
`RetailData.fixture`; `docs/asset-gated-tests.md`).

| Files | Why they stay |
|---|---|
| `novaworld/*.gsb`, `novaworld/*.hexcap`, `novaworld/run_*/*` | NovaWorld/in-game wire captures and manifests the codec tests replay (retail bytes are the product here) |

## Authored files

`def/items.def` (test rows 106100..), `terrain/tmap/items.def`, `mnu/widgets.mnu`,
`mnu/all_widgets.mnu`, `mns/test_style.mns`, `score/score_sample.ini`,
`anim/*` (the one-bone `.bad` clips, `soldier.adm`, `US01.adm`, and
`weapon_timing.txt`, the `opennova-3di weapon timing` input the Blender packaging
workflow runs),
`particle/gorehit.ptu` (the gore-set half of the effect catalog, written in the
retail `.ptu` grammar with our own effect), `grm/person.grm` (an authored facial
rig; `grm_roundtrip` pins it byte-for-byte through the writer),
`threedi/o3d/*.o3d` (the authored `.o3d` scenes the `opennova-3di` ctests and
the Blender add-on's package smoke test build), `wac/text_document.wac` (a script
naming an effect, a sound set, two ammo and a text key, the editor's text document
test's), the
`novaworld/run_*/manifest.txt` records, and this README.

## threedi/synth — the synthetic 3DI model set (minted)

`tests/fixtures/minimal_3di_gen.cpp` authors every model in memory
(`tests/fixtures/minimal_3di_builder.h`, mission axes: x forward, y left, z up,
origin on the ground unless noted) and writes it through `threedi_3di3_write`.
Eleven base models stand in for the retail models the tests once loaded, shaped
after what those tests key on:

| Model | Shape |
|---|---|
| `crate` | one part, one box; COBJ 0 with a 12-face box and one CB volume; user point `ground` |
| `gun` | receiver + barrel parts, a CMDL block with no usable collision geometry (a held weapon); `MFlash01` on the barrel (row 1 sits on a drawing part off the root), `Bullet01` and `bcasing` on the root (a one-bone rig can carry the muzzle), `ground` |
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
| `mount_mtrl0_rgbgen113_reg1` | mount | material 0 as FF_ST_OP_LUM (emissive 2), RGB generator style 113 on register 1, black to white | ctrl_bus material alias |
| `mount_mtrl2_ab_lum` | mount | material 2 (the heat slab) as `FF_ST_AB_LUM` | `framefx_test.gd` alpha-blend LUM adds nothing to Q3; `slot_shadow_test.gd` |
| `person_mtrl0_ad_lum` | person | material 0 `FF_ST_AD_LUM` (emissive full) | `framefx_test.gd` skinned LUM never reaches Q3 |
| `person_part9_trigger_scale` | person | CTRL 0 `WPN_TRIGGER`; LOD0 row 9 (the left upper arm) uniform scale, style 113 on register 0, 1.0 to 0 | `object_model_skeletal_panm_test.gd` part tracks over the clip pose |
| `pump_mtrl1_mt_alphatest` | pump | material 1 (the `FF_MT_OP` post) alpha test on, threshold byte 32 | `render_swatch_pass_modes.gd` probe, the `_MT` alpha-test projshadow caster |
| `crate_mtrl0_ad_lum_upl113` | crate | material 0 `FF_ST_AD_LUM` (emissive full), CTRL 0 `UPL_INTENSITY`, RGB generator style 113 on register 0, black to white | `celestial_test.gd` sky bodies |
| `crate_mtrl0_ab_lum_upl113` | crate | the same edits with material 0 `FF_ST_AB_LUM` | `celestial_test.gd` Q3 glow blend |
| `armory_lght0_colorgen113_flicker` | armory | light 0: style 113, phase 0, black to white, objects enabled | ctrl_bus light bus |
| `pump_minefield` | pump | ignored USRP followed by sixteen mixed-case mine names across the existing five parts; only the first fourteen bind | native `minefield`; GUT `minefield_present_test` |
| `pump_anim0_noise_translation` | pump | LOD0 row 0: translation z enabled, control 0x36, end 32767 | `object_data_panm_apply_test.gd` same-time noise |
| `shed_lght0_sub2_origin_atten100` | shed | light 0: subobject 2, position zero, atten_end 100 | `effect_light_world_test.gd` exact ROBJ row; `per_model_light_isolation_test.gd` owner scope |
| `armory_lght0_sub1_offset` | armory | light 0: subobject 1, Godot position (0.25, 0.5, -0.75), atten_end 1000 | effect_light spawn-time matrix |
| `house_lod0_sine_rotx` | house | one appended LOD0 row: rotation x sine 0..90 deg at speed 1 | `terrain_static_shadow_runtime_test.gd` resident pages |
| `house_lod0_sine_rotx_uv1` | house | the previous edit + material 0 `uv_u_style` 1 | terrain dynamic-UV phase |
| `house_mtrl0_uvscroll16_alphatest` | house | material 0 as FF_ST_OP#UV, alpha test on, `uv_u_style` 16, `uv_u_rate` 1.0 | terrain worker snapshot |
| `mount_heat_glow_slide_part1` | mount | delete rows; slide(1) on `HEAT_GLOW` | `simulation_test.gd` heat glow |
| `armory_special1_slide_part1` | armory | CTRL 0 renamed `VEHICLE_SPECIAL1`; delete rows; slide(1) | simulation animated collision + FastRope SPECIAL1 |
| `armory_special2_slide_part1` | armory | CTRL 0 renamed `VEHICLE_SPECIAL2`; delete rows; slide(1) | simulation FastRope SPECIAL2 |
| `tank_special1_slide_ewep01` | tank | CTRL 0 renamed `VEHICLE_SPECIAL1`; delete rows; slide(the part that owns `ewep01`) | simulation listen-snapshot attachment |
| `pump_lod0_inert_lod1_sine_rotz` | pump | LOD0: one inert row on part 0; LOD1: one rotation z sine row | simulation effective LOD0 collision |
| `pump_lod20`, `pump_lod80` | pump | LOD0 RMDL threshold 20 or 80 (integer pixels, retail's authored form: LOD0 draws above it, LOD1 below); original geometry, GHDR and CMDL retained | entity projection sphere, fallback/composed persons and primary-sphere husk LOD selection |
| `panm_live_01_spinner` .. `panm_inert_10_rotrev` | shed | delete rows; one row on part 0 with flags F and every track control 0 except the live tracks (control 0x10). (F, live): 01 `1<<8` none; 02 `3<<8` none; 03 `4<<8` none; 04 `2<<8` rotation z; 05 `2<<8` scale x; 06 `1` scale y; 07 `1` scale x; 08 `2` scale y; 09 `1<<24` translation; 10 `1<<16` none | simulation PANM liveness family |

Every consumer that pins a number pins the authored one: the bird's 18 faces,
the armory's 24 collision faces (COBJ 1's 8 faces = 24 vertices move under a
slide, the other 48 stay), the person's head sphere. Retail-only assertions
(the six retail controlled models' PANM/CTRL bytes, the 00TRa armory's own
occlusion walk) run as `OPENNOVA_JO_ASSETS` legs of the same ctests.

## Other minted sets

- `terrain/tmap` — `tests/fixtures/minimal_terrain_gen.cpp`.
- `fnt/synth_*.fnt`, `cbin/synth_nlist*.kda`, `cbin/credits_image.png`,
  `cbin/particle_dot.tga` — `tests/fixtures/minimal_fnt_gen.cpp`,
  `minimal_cbin_gen.cpp`: the credits lists in their JO, JOX01 and BHD shapes
  through `cbin::encode` (the shipped trio is `cbin_roundtrip`'s reference-tree
  leg), the image and font stand-ins the tests stage beside them, and the .tga
  sprite the effect-world test's synthetic particle file names.
- `env/synth_full.env`, `env/cloud01.pcx`, `env/cloud01b.pcx` — `tests/fixtures/minimal_env_gen.cpp`:
  the synthetic environment (every keyword authored, ten time-of-day keyframes,
  written by `save_env`) and the two sky maps it names (the bright square-rooted
  cloud field and the modulation layer centered at 128), 64x64 indexed PCX by our
  writer. The shipped `.env` set is the gated `env_jo_install` sweep.
- `rtxt/synth_*.bin` — `tests/fixtures/minimal_rtxt_gen.cpp`: the parity set
  `rtxt_synth_parity` grills — a multi-section game table with position hints, a
  menu table, a mission sidecar with cp1252 text and odd-length padding, and the
  one-entry case. The shipped
  string tables are read from the reference tree by the gated
  `rtxt_jo_install_sweep` and the menu-driven GUT tests.
- `gamecfg/synth_game.cfg` — `tests/fixtures/minimal_gamecfg_gen.cpp`: a `game.cfg`
  through `gamecfg::write` (CR LF, Game_SaveConfig's layout) from
  `tests/gamecfg/gamecfg_synth.h`, every written key off its default and a four-row weapon
  roster. No retail file stands behind it: the game writes `game.cfg` on the player's
  machine, with the player's hardware names in it.
- `banlist/synth_banlist.txt`, `banlist/synth_banned.txt` — `tests/fixtures/minimal_banlist_gen.cpp`:
  the two ban files through `banlist::write_pcid_list` (BanList_SaveToFileWithHeader's header,
  CR LF) and `banlist::write_address_list` (`%20s   "%s"`, CR LF) from
  `tests/banlist/banlist_synth.h`: invented PCIDs (one repeated, one nameless), addresses and
  names. No retail file stands behind them: a server writes both on its operator's machine.
- `sbf/synth_gamemus.sbf` — `tests/fixtures/minimal_sbf_gen.cpp`.
- `mus/synth_gamemus.bin`, `mus/synth_menumus.bin`, `mus/golden_synth_gamemus.mus.txt` —
  `tests/fixtures/synth_mus_gen.cpp`: two music-director programs authored in the
  MDEdit source dialect through `mus_compile` + `mus_encode_file` (the gamescript
  in the shipped eight-section routing shape, the menuscript a Var02-dispatched
  state machine) and the decompiler's own golden over the gamescript. The shipped
  programs and their decoded golden are the reference-tree legs of the mus ctests.
- `lwf/menu.lwf`, `lwf/tone.wav` — `tests/fixtures/minimal_lwf_gen.cpp`.
- `avatars/synth_avatars.def` — `tests/fixtures/minimal_avatars_gen.cpp`: the avatar
  table through `avatars_write` (26 parts, eight nationalities of divisions and
  combos with both `skipdemo` flag levels, four good and four evil); the shipped
  table is the reference-tree leg of `avatars_parse` and `avatars_roundtrip`.
- `bms/synth_dense.bms` — `tests/fixtures/minimal_bms_gen.cpp`: a dense mission
  authored through the `bms_edit` free functions and written by `bms::write` (four populated
  pools, one item id each, zero-valued optional fields, the minted terrain and
  environment in its header); the shipped ash_i5b is `mission_mis_idempotency`'s
  reference-tree leg and the whole shipped corpus is `mission_corpus`'s retail leg
  (the install's archives, through the VFS).
- `bms/synth_logic.bms`, `bms/synth_logic.bin` — `tests/fixtures/mission_set_gen.cpp`: the
  small mission the mission document and its picture read (every row kind on the minted
  terrain inside mission x, y of -512..512, items whose graphics are `threedi/synth` models,
  a path, two area triggers (zones 20 and 30), two events naming one another, a four-entry loadout, the first
  win and lose directives) through `bms::write`, and its string table (the keys its records
  form: `LOCATION001`, `STRNAME001`, the win and lose directives) through `rtxt::write`.
- `particle/synth_*.ptl` — `tests/fixtures/minimal_particle_gen.cpp`: five particle
  files through `save_particles` (a lone table, a table with its edit handles,
  seven effects over a blank particle, a three-layer particle with curves and
  per-layer colours, and the `Buildup` effect the authored `gorehit.ptu` also
  references); the shipped `.ptl` corpus is `particle_smoke_all_fixtures`'
  reference-tree leg.
- `dbf/synth_bank.dbf` — `tests/fixtures/minimal_dbf_gen.cpp`: eleven dialog groups
  through `encode_dbf`; the shipped 00TRg bank is `dbf_roundtrip`'s reference-tree leg.
