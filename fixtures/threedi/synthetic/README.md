# Synthetic 3DI fixtures

Runtime-only test inputs: real committed models with one authored edit each,
so GUT tests can pin runtime behavior (CTRL aliasing, PANM liveness, LGHT
attachment, material generators, collision ordinals) without an authoring
surface. They were minted ONCE on master `5820432c1` through the
`ObjectData` edit + export bindings that ADR 0038 retired; the runtime can no
longer re-author them, so this file is their provenance record. The probe
that produced them is reproduced verbatim at the end; every recipe is the
authoring code of the GUT test it replaced, and every file was re-opened and
checked with the same read-back guards the restored test asserts.

Bases: `B50Cal`, `Armry01`, `dm1a1`, `Pmpjk01` are the committed
`fixtures/threedi/objects/` models (byte-identical to master's
`fixtures/3dp/` copies); `Shed`, `House` are `fixtures/threedi/3di3/`.
"delete rows" = `delete_part_anim(L, i)` over every LOD-L row, highest first;
"slide(L, part)" = `add_part_anim(L, part)` (== 0), translation enabled,
`translation.x` mode `control_register` 0, values `0.0 .. 4.0`, speed `0.0`.

| File | Base | Recipe | Consumer |
|---|---|---|---|
| `b50cal_ctrl1_heat_glow` | B50Cal | `set_control_register_name(1, "HEAT_GLOW")` | `object_data_ctrl_bus_test.gd` duplicate CTRL alias |
| `b50cal_ctrl1_not_retail` | B50Cal | `set_control_register_name(1, "NOT_RETAIL")` | ctrl_bus unknown CTRL alias |
| `b50cal_yaw_style114` | B50Cal | the LOD0 track with `control == 113 && control_param == 1`: `set_part_anim_track_field(0, anim, track, "control", 114)` | ctrl_bus wave style, normal |
| `b50cal_ctrl1_lod_frac_yaw_style114` | B50Cal | `set_control_register_name(1, "LOD_FRAC")` + the same track edit | ctrl_bus wave style, patched |
| `b50cal_mtrl0_rgbgen113_reg1` | B50Cal | `set_material_field(0, "rgb_gen_style", 113)`, `("rgb_gen_reg", 1)`, `("rgb_gen_start_color", Color.BLACK)`, `("rgb_gen_end_color", Color.WHITE)` | ctrl_bus material alias |
| `armry01_lght0_colorgen113_flicker` | Armry01 | `set_light_field(0, "disable_lightobjects", false)`, `("colorgen_style", 113)`, `("colorgen_phase", 0)`, `("color_start", Color.BLACK)`, `("color_end", Color.WHITE)` | ctrl_bus light bus |
| `pmpjk01_anim0_noise_translation` | Pmpjk01 | `set_part_anim_channel_enabled(0, 0, "translation", true)`; track fields `control = 0x36`, `control_param = 0`, `rate = 0`, `start = 0`, `end = 32767` | `object_data_panm_apply_test.gd` same-time noise |
| `shed_lght0_sub2_origin_atten100` | Shed | `set_light_field(0, "subobject", 2)`, `("position", Vector3.ZERO)`, `("atten_end", 100.0)` | `effect_light_world_test.gd` exact ROBJ row; `per_model_light_isolation_test.gd` owner scope |
| `armry01_lght0_sub1_offset` | Armry01 | `set_light_field(0, "subobject", 1)`, `("position", Vector3(0.25, 0.5, -0.75))`, `("atten_end", 1000.0)`, `("disable_lightobjects", false)` | effect_light spawn-time matrix |
| `house_lod0_sine_rotx` | House | `p = add_part_anim(0, 0)`; rotation enabled; `rotation.x` `sine_wave` -1, values `0.0 .. 90.0` speed `1.0` (appended, no delete) | `terrain_static_shadow_runtime_test.gd` resident pages + restore phase |
| `house_lod0_sine_rotx_uv1` | House | the previous recipe + `set_material_field(0, "uv_u_style", 1)` | terrain dynamic-UV phase |
| `house_mtrl0_uvscroll16_alphatest` | House | `set_material_field(0, "alpha_test_enabled", true)`, `("uv_u_style", 16)`, `("uv_u_rate", 1.0)` | terrain worker snapshot |
| `b50cal_heat_glow_slide_part1` | B50Cal | `set_control_register_name(0, "HEAT_GLOW")`; delete rows; slide(0, 1) | `simulation_test.gd` heat glow |
| `armry01_special1_slide_part1` | Armry01 | `set_control_register_name(0, "VEHICLE_SPECIAL1")`; delete rows; slide(0, 1) | simulation animated collision + FastRope SPECIAL1 |
| `armry01_special2_slide_part1` | Armry01 | `set_control_register_name(0, "VEHICLE_SPECIAL2")`; delete rows; slide(0, 1) | simulation FastRope SPECIAL2 |
| `dm1a1_special1_slide_ewep01` | dm1a1 | `set_control_register_name(0, "VEHICLE_SPECIAL1")`; `anchor` = `subobject` of user point `ewep01`; delete rows; slide(0, anchor) | simulation listen-snapshot attachment |
| `pmpjk01_lod0_inert_lod1_sine_rotz` | Pmpjk01 | delete LOD0 rows; `add_part_anim(0, 0)` (inert); delete LOD1 rows; `add_part_anim(1, 0)`, rotation enabled, `rotation.z` `sine_wave` -1, values `0.0 .. 90.0` speed `1.0` | simulation effective LOD0 collision |
| `panm_live_01_spinner` .. `panm_inert_10_rotrev` | Shed | delete rows; `a = add_part_anim(0, 0)`; `set_part_animation_flags(0, a, FLAGS)`; every track `control = 0`; the live tracks `control = 0x10`. Cases (flags, live tracks): 01 `1<<8` []; 02 `3<<8` []; 03 `4<<8` []; 04 `2<<8` [rotation_z]; 05 `2<<8` [scale_x]; 06 `1` [scale_y]; 07 `1` [scale_x]; 08 `2` [scale_y]; 09 `1<<24` [translation]; 10 `1<<16` [] | simulation PANM liveness family (`live` files evaluate live, `inert` files do not) |

## The minting probe (master `5820432c1`, `godot/tests/mint_synthetic_fixtures_probe.gd`)

```gdscript
extends SceneTree

## One-off minting of fixtures/threedi/synthetic/*.3di on master 5820432c1,
## through the ObjectData edit + export surface that ADR 0038 retired.
## Run from a master checkout with a master GDExtension:
##   NW_MINT_OUT=<dir> godot --headless --path godot -s res://tests/mint_synthetic_fixtures_probe.gd
## Every recipe is the verbatim authoring code of the GUT test it replaces;
## every minted file is re-opened and checked with the same read-back guards
## the restored test asserts. Never collected by GUT (the _probe suffix).

const B50CAL := "res://../fixtures/3dp/B50Cal/B50Cal.3di"
const ARMRY := "res://../fixtures/3dp/armry01/Armry01.3di"
const DM1A1 := "res://../fixtures/3dp/dm1a1/dm1a1.3di"
const PMP := "res://../fixtures/3dp/Pmpjk01/Pmpjk01.3di"
const SHED := "res://../fixtures/threedi/3di3/Shed.3di"
const HOUSE := "res://../fixtures/threedi/3di3/House.3di"
const TRACKS := [
	"rotation_x", "rotation_y", "rotation_z",
	"scale_x", "scale_y", "scale_z", "translation",
]

var _failures := 0
var _out_dir := ""


func _fail(what: String) -> void:
	_failures += 1
	printerr("FAIL: " + what)


func _check(ok: bool, what: String) -> void:
	if not ok:
		_fail(what)


func _open(res_path: String) -> ObjectData:
	var data := ObjectData.new()
	_check(data.open_file(ProjectSettings.globalize_path(res_path)) == OK, "open " + res_path)
	return data


func _delete_rows(data: ObjectData, lod: int) -> void:
	for i in range(data.get_part_anim_count(lod) - 1, -1, -1):
		_check(data.delete_part_anim(lod, i), "delete_part_anim(%d, %d)" % [lod, i])


func _slide(data: ObjectData, lod: int, part: int) -> int:
	var anim := data.add_part_anim(lod, part)
	_check(anim == 0, "add_part_anim(%d, %d) returned %d" % [lod, part, anim])
	_check(data.set_part_anim_channel_enabled(lod, anim, "translation", true), "enable translation")
	_check(data.set_part_anim_channel_mode(lod, anim, "translation", "x", "control_register", 0),
			"translation.x control_register 0")
	_check(data.set_part_anim_channel_values(lod, anim, "translation", "x", 0.0, 4.0, 0.0),
			"translation.x 0..4")
	return anim


func _controlled_track(data: ObjectData, local_ordinal: int) -> Dictionary:
	for anim_index in range(data.get_part_anim_count(0)):
		var anim: Dictionary = data.get_part_anim_info(0, anim_index)
		for track_name in TRACKS:
			var track: Dictionary = anim.get(track_name, {})
			if int(track.get("control", 0)) == 113 and \
					int(track.get("control_param", -1)) == local_ordinal:
				return {
					"anim_index": anim_index,
					"track_name": track_name,
					"part_index": int(anim.get("transform_as", -1)),
				}
	return {}


func _register_name(data: ObjectData, index: int) -> String:
	var registers: Array = data.get_control_registers()
	if index >= registers.size():
		return ""
	return String((registers[index] as Dictionary).get("name", ""))


func _track(data: ObjectData, lod: int, anim: int, track: String) -> Dictionary:
	return data.get_part_anim_info(lod, anim).get(track, {})


func _ewep01_part(data: ObjectData) -> int:
	for index in range(data.get_user_point_count()):
		var candidate: Dictionary = data.get_user_point_info(index)
		if String(candidate.get("name", "")).to_lower() == "ewep01":
			return int(candidate.get("subobject", -1))
	return -1


func _export(data: ObjectData, name: String) -> String:
	var tmp := _out_dir.path_join("tmp_" + name)
	DirAccess.make_dir_recursive_absolute(tmp)
	var err := data.export_3di_to_dir(tmp)
	if err != OK:
		_fail("%s: export_3di_to_dir -> %d" % [name, err])
		return ""
	var produced := ""
	for file_name in DirAccess.get_files_at(tmp):
		if String(file_name).to_lower().ends_with(".3di"):
			produced = tmp.path_join(file_name)
	if produced.is_empty():
		_fail("%s: export produced no .3di" % name)
		return ""
	var out_path := _out_dir.path_join(name + ".3di")
	var bytes := FileAccess.get_file_as_bytes(produced)
	var out := FileAccess.open(out_path, FileAccess.WRITE)
	out.store_buffer(bytes)
	out.close()
	DirAccess.remove_absolute(produced)
	DirAccess.remove_absolute(tmp)
	return out_path


func _mint(name: String, base: String, author: Callable, verify: Callable) -> void:
	var before := _failures
	var data := _open(base)
	author.call(data)
	var out_path := _export(data, name)
	if out_path.is_empty():
		return
	var reread := ObjectData.new()
	_check(reread.open_file(out_path) == OK, "%s: re-open" % name)
	verify.call(reread)
	var size := FileAccess.get_file_as_bytes(out_path).size()
	if _failures == before:
		print("PASS %-42s %8d bytes" % [name + ".3di", size])
	else:
		printerr("FAIL %s (%d checks failed)" % [name, _failures - before])


func _liveness_case(index: int, live: bool, desc: String, flags: int, live_tracks: Array,
		expected: bool) -> void:
	var name := "panm_%s_%02d_%s" % ["live" if live else "inert", index, desc]
	_mint(name, SHED,
		func(d: ObjectData) -> void:
			_delete_rows(d, 0)
			var anim := d.add_part_anim(0, 0)
			_check(anim == 0, name + ": add_part_anim")
			_check(d.set_part_animation_flags(0, anim, flags) == OK, name + ": flags")
			for track in TRACKS:
				_check(d.set_part_anim_track_field(0, anim, track, "control", 0), name + ": control 0")
			for track in live_tracks:
				_check(d.set_part_anim_track_field(0, anim, track, "control", 0x10), name + ": control 0x10")
			_check(d.has_live_panm_for_lod(0) == expected, name + ": authored liveness"),
		func(r: ObjectData) -> void:
			_check(r.get_part_anim_count(0) == 1, name + ": one LOD0 row")
			_check(r.has_live_panm_for_lod(0) == expected, name + ": re-read liveness"))


func _init() -> void:
	_out_dir = OS.get_environment("NW_MINT_OUT")
	if _out_dir.is_empty():
		printerr("set NW_MINT_OUT")
		quit(2)
		return
	if not ClassDB.class_has_method("ObjectData", "export_3di_to_dir"):
		printerr("this GDExtension has no ObjectData.export_3di_to_dir: run from a master build")
		quit(2)
		return
	DirAccess.make_dir_recursive_absolute(_out_dir)

	# --- CTRL bus (object_data_ctrl_bus_test.gd) ---
	_mint("b50cal_ctrl1_heat_glow", B50CAL,
		func(d: ObjectData) -> void:
			_check(d.set_control_register_name(1, "HEAT_GLOW"), "ctrl1 HEAT_GLOW"),
		func(r: ObjectData) -> void:
			_check(_register_name(r, 0) == "HEAT_GLOW", "ctrl0 HEAT_GLOW")
			_check(_register_name(r, 1) == "HEAT_GLOW", "ctrl1 HEAT_GLOW re-read")
			_check(not _controlled_track(r, 1).is_empty(), "controlled track 1 survives"))
	_mint("b50cal_ctrl1_not_retail", B50CAL,
		func(d: ObjectData) -> void:
			_check(d.set_control_register_name(1, "NOT_RETAIL"), "ctrl1 NOT_RETAIL"),
		func(r: ObjectData) -> void:
			_check(_register_name(r, 1) == "NOT_RETAIL", "ctrl1 NOT_RETAIL re-read")
			_check(not _controlled_track(r, 1).is_empty(), "controlled track 1 survives"))
	_mint("b50cal_yaw_style114", B50CAL,
		func(d: ObjectData) -> void:
			var track := _controlled_track(d, 1)
			_check(not track.is_empty(), "yaw track found")
			_check(d.set_part_anim_track_field(0, int(track.get("anim_index", -1)),
					String(track.get("track_name", "")), "control", 114), "control 114"),
		func(r: ObjectData) -> void:
			var pristine := _open(B50CAL)
			var track := _controlled_track(pristine, 1)
			var t := _track(r, 0, int(track.get("anim_index", -1)), String(track.get("track_name", "")))
			_check(int(t.get("control", 0)) == 114, "control 114 re-read"))
	_mint("b50cal_ctrl1_lod_frac_yaw_style114", B50CAL,
		func(d: ObjectData) -> void:
			_check(d.set_control_register_name(1, "LOD_FRAC"), "ctrl1 LOD_FRAC")
			var track := _controlled_track(d, 1)
			_check(not track.is_empty(), "yaw track found")
			_check(d.set_part_anim_track_field(0, int(track.get("anim_index", -1)),
					String(track.get("track_name", "")), "control", 114), "control 114"),
		func(r: ObjectData) -> void:
			_check(_register_name(r, 1) == "LOD_FRAC", "ctrl1 LOD_FRAC re-read")
			var pristine := _open(B50CAL)
			var track := _controlled_track(pristine, 1)
			var t := _track(r, 0, int(track.get("anim_index", -1)), String(track.get("track_name", "")))
			_check(int(t.get("control", 0)) == 114, "control 114 re-read"))
	_mint("b50cal_mtrl0_rgbgen113_reg1", B50CAL,
		func(d: ObjectData) -> void:
			_check(d.set_material_field(0, "rgb_gen_style", 113), "rgb_gen_style")
			_check(d.set_material_field(0, "rgb_gen_reg", 1), "rgb_gen_reg")
			_check(d.set_material_field(0, "rgb_gen_start_color", Color.BLACK), "start color")
			_check(d.set_material_field(0, "rgb_gen_end_color", Color.WHITE), "end color"),
		func(r: ObjectData) -> void:
			var m: Dictionary = r.get_material_info(0)
			_check(int(m.get("rgb_gen_style", -1)) == 113, "rgb_gen_style re-read")
			_check(int(m.get("rgb_gen_reg", -1)) == 1, "rgb_gen_reg re-read"))
	_mint("armry01_lght0_colorgen113_flicker", ARMRY,
		func(d: ObjectData) -> void:
			_check(d.set_light_field(0, "disable_lightobjects", false), "disable_lightobjects")
			_check(d.set_light_field(0, "colorgen_style", 113), "colorgen_style")
			_check(d.set_light_field(0, "colorgen_phase", 0), "colorgen_phase")
			_check(d.set_light_field(0, "color_start", Color.BLACK), "color_start")
			_check(d.set_light_field(0, "color_end", Color.WHITE), "color_end"),
		func(r: ObjectData) -> void:
			var l: Dictionary = r.get_light_info(0)
			_check(int(l.get("colorgen_style", -1)) == 113, "colorgen_style re-read")
			_check(int(l.get("colorgen_phase", -1)) == 0, "colorgen_phase re-read")
			_check(not bool(l.get("disable_lightobjects", true)), "disable_lightobjects re-read"))

	# --- PANM apply (object_data_panm_apply_test.gd) ---
	_mint("pmpjk01_anim0_noise_translation", PMP,
		func(d: ObjectData) -> void:
			_check(d.get_part_anim_count(0) > 0, "pump jack has a LOD0 row")
			_check(d.set_part_anim_channel_enabled(0, 0, "translation", true), "enable translation")
			_check(d.set_part_anim_track_field(0, 0, "translation", "control", 0x36), "control 0x36")
			_check(d.set_part_anim_track_field(0, 0, "translation", "control_param", 0), "control_param")
			_check(d.set_part_anim_track_field(0, 0, "translation", "rate", 0), "rate")
			_check(d.set_part_anim_track_field(0, 0, "translation", "start", 0), "start")
			_check(d.set_part_anim_track_field(0, 0, "translation", "end", 32767), "end"),
		func(r: ObjectData) -> void:
			var t := _track(r, 0, 0, "translation")
			_check(int(t.get("control", 0)) == 0x36, "noise control re-read")
			_check(int(t.get("end", 0)) == 32767, "noise end re-read"))

	# --- effect lights / per-model isolation ---
	_mint("shed_lght0_sub2_origin_atten100", SHED,
		func(d: ObjectData) -> void:
			_check(d.get_light_count() == 1, "Shed has one LGHT")
			_check(d.set_light_field(0, "subobject", 2), "subobject 2")
			_check(d.set_light_field(0, "position", Vector3.ZERO), "position zero")
			_check(d.set_light_field(0, "atten_end", 100.0), "atten_end 100"),
		func(r: ObjectData) -> void:
			var l: Dictionary = r.get_light_info(0)
			_check(r.get_light_count() == 1, "one LGHT re-read")
			_check(int(l.get("subobject", -1)) == 2, "subobject 2 re-read")
			_check((l.get("position", Vector3.ONE) as Vector3).is_equal_approx(Vector3.ZERO), "position re-read")
			_check(is_equal_approx(float(l.get("atten_end", 0.0)), 100.0), "atten_end re-read"))
	_mint("armry01_lght0_sub1_offset", ARMRY,
		func(d: ObjectData) -> void:
			_check(d.get_light_count() > 0, "Armry01 has a LGHT")
			_check(d.set_light_field(0, "subobject", 1), "subobject 1")
			_check(d.set_light_field(0, "position", Vector3(0.25, 0.5, -0.75)), "position")
			_check(d.set_light_field(0, "atten_end", 1000.0), "atten_end 1000")
			_check(d.set_light_field(0, "disable_lightobjects", false), "disable_lightobjects"),
		func(r: ObjectData) -> void:
			var l: Dictionary = r.get_light_info(0)
			_check(int(l.get("subobject", -1)) == 1, "subobject 1 re-read")
			_check((l.get("position", Vector3.ZERO) as Vector3).is_equal_approx(Vector3(0.25, 0.5, -0.75)),
					"position re-read")
			_check(is_equal_approx(float(l.get("atten_end", 0.0)), 1000.0), "atten_end re-read")
			_check(not bool(l.get("disable_lightobjects", true)), "disable_lightobjects re-read"))

	# --- terrain static shadow ---
	var house_sine := func(d: ObjectData) -> void:
		var panm_index := d.add_part_anim(0, 0)
		_check(panm_index >= 0, "House add_part_anim")
		_check(d.set_part_anim_channel_enabled(0, panm_index, "rotation", true), "enable rotation")
		_check(d.set_part_anim_channel_mode(0, panm_index, "rotation", "x", "sine_wave", -1), "sine_wave x")
		_check(d.set_part_anim_channel_values(0, panm_index, "rotation", "x", 0.0, 90.0, 1.0), "0..90 @1")
		_check(d.has_live_panm_for_lod(0), "House live PANM")
	_mint("house_lod0_sine_rotx", HOUSE, house_sine,
		func(r: ObjectData) -> void:
			_check(r.has_live_panm_for_lod(0), "live PANM re-read")
			_check(int(r.get_material_info(0).get("uv_u_style", -1)) == 0, "uv_u_style 0"))
	_mint("house_lod0_sine_rotx_uv1", HOUSE,
		func(d: ObjectData) -> void:
			house_sine.call(d)
			_check(d.set_material_field(0, "uv_u_style", 1), "uv_u_style 1"),
		func(r: ObjectData) -> void:
			_check(r.has_live_panm_for_lod(0), "live PANM re-read")
			_check(int(r.get_material_info(0).get("uv_u_style", -1)) == 1, "uv_u_style 1 re-read"))
	_mint("house_mtrl0_uvscroll16_alphatest", HOUSE,
		func(d: ObjectData) -> void:
			_check(d.set_material_field(0, "alpha_test_enabled", true), "alpha_test_enabled")
			_check(d.set_material_field(0, "uv_u_style", 16), "uv_u_style 16")
			_check(d.set_material_field(0, "uv_u_rate", 1.0), "uv_u_rate 1"),
		func(r: ObjectData) -> void:
			var m: Dictionary = r.get_material_info(0)
			_check(bool(m.get("alpha_test_enabled", false)), "alpha_test_enabled re-read")
			_check(int(m.get("uv_u_style", -1)) == 16, "uv_u_style 16 re-read")
			_check(is_equal_approx(float(m.get("uv_u_rate", 0.0)), 1.0), "uv_u_rate re-read"))

	# --- simulation_test.gd ---
	_mint("b50cal_heat_glow_slide_part1", B50CAL,
		func(d: ObjectData) -> void:
			_check(d.set_control_register_name(0, "HEAT_GLOW"), "ctrl0 HEAT_GLOW")
			_delete_rows(d, 0)
			_slide(d, 0, 1),
		func(r: ObjectData) -> void:
			_check(_register_name(r, 0) == "HEAT_GLOW", "ctrl0 HEAT_GLOW re-read")
			_check(r.get_part_anim_count(0) == 1, "one LOD0 row")
			_check(int(r.get_part_anim_info(0, 0).get("transform_as", -1)) == 1, "transform_as 1")
			_check(int(_track(r, 0, 0, "translation").get("control", 0)) == 113, "translation control 113"))
	_mint("armry01_special1_slide_part1", ARMRY,
		func(d: ObjectData) -> void:
			_check(d.set_control_register_name(0, "VEHICLE_SPECIAL1"), "ctrl0 SPECIAL1")
			_check(d.has_collision(), "Armry01 has collision")
			_delete_rows(d, 0)
			_slide(d, 0, 1),
		func(r: ObjectData) -> void:
			_check(_register_name(r, 0) == "VEHICLE_SPECIAL1", "ctrl0 SPECIAL1 re-read")
			_check(r.has_collision(), "collision re-read")
			_check(r.get_part_anim_count(0) == 1, "one LOD0 row")
			_check(int(r.get_part_anim_info(0, 0).get("transform_as", -1)) == 1, "transform_as 1"))
	_mint("armry01_special2_slide_part1", ARMRY,
		func(d: ObjectData) -> void:
			_check(d.set_control_register_name(0, "VEHICLE_SPECIAL2"), "ctrl0 SPECIAL2")
			_delete_rows(d, 0)
			_slide(d, 0, 1),
		func(r: ObjectData) -> void:
			_check(_register_name(r, 0) == "VEHICLE_SPECIAL2", "ctrl0 SPECIAL2 re-read")
			_check(r.get_part_anim_count(0) == 1, "one LOD0 row")
			_check(int(r.get_part_anim_info(0, 0).get("transform_as", -1)) == 1, "transform_as 1"))
	_mint("dm1a1_special1_slide_ewep01", DM1A1,
		func(d: ObjectData) -> void:
			_check(d.set_control_register_name(0, "VEHICLE_SPECIAL1"), "ctrl0 SPECIAL1")
			var anchor_part := _ewep01_part(d)
			_check(anchor_part >= 0, "ewep01 bound to a part")
			_delete_rows(d, 0)
			_slide(d, 0, anchor_part),
		func(r: ObjectData) -> void:
			_check(_register_name(r, 0) == "VEHICLE_SPECIAL1", "ctrl0 SPECIAL1 re-read")
			var anchor_part := _ewep01_part(r)
			_check(anchor_part >= 0, "ewep01 re-read")
			_check(r.get_part_anim_count(0) == 1, "one LOD0 row")
			_check(int(r.get_part_anim_info(0, 0).get("transform_as", -1)) == anchor_part,
					"transform_as == ewep01 part"))
	_mint("pmpjk01_lod0_inert_lod1_sine_rotz", PMP,
		func(d: ObjectData) -> void:
			_check(int(d.get_summary().get("lod_count", 0)) > 1, "pump jack has LOD1")
			_delete_rows(d, 0)
			var inert := d.add_part_anim(0, 0)
			_check(inert == 0, "inert row is 0")
			_check(not d.has_live_panm_for_lod(0), "LOD0 inert")
			_check(Array(d.get_effective_panm_targets(0)) == [0], "LOD0 targets [0]")
			_delete_rows(d, 1)
			var live := d.add_part_anim(1, 0)
			_check(live == 0, "live row is 0")
			_check(d.set_part_anim_channel_enabled(1, live, "rotation", true), "enable rotation")
			_check(d.set_part_anim_channel_mode(1, live, "rotation", "z", "sine_wave", -1), "sine z")
			_check(d.set_part_anim_channel_values(1, live, "rotation", "z", 0.0, 90.0, 1.0), "0..90")
			_check(d.has_live_panm_for_lod(1), "LOD1 live")
			_check(d.get_live_panm_lod() == 1, "live lod 1"),
		func(r: ObjectData) -> void:
			_check(int(r.get_summary().get("lod_count", 0)) > 1, "LOD1 re-read")
			_check(not r.has_live_panm_for_lod(0), "LOD0 inert re-read")
			_check(Array(r.get_effective_panm_targets(0)) == [0], "LOD0 targets [0] re-read")
			_check(r.has_live_panm_for_lod(1), "LOD1 live re-read")
			_check(r.get_live_panm_lod() == 1, "live lod 1 re-read"))

	# --- PANM liveness family (simulation_test.gd, ten files) ---
	_liveness_case(1, true, "spinner", 1 << 8, [], true)
	_liveness_case(2, true, "view3", 3 << 8, [], true)
	_liveness_case(3, true, "view4", 4 << 8, [], true)
	_liveness_case(4, true, "rotz", 2 << 8, ["rotation_z"], true)
	_liveness_case(5, false, "rot_scalex", 2 << 8, ["scale_x"], false)
	_liveness_case(6, false, "uniform_scaley", 1, ["scale_y"], false)
	_liveness_case(7, true, "uniform_scalex", 1, ["scale_x"], true)
	_liveness_case(8, true, "axis_scaley", 2, ["scale_y"], true)
	_liveness_case(9, true, "translation", 1 << 24, ["translation"], true)
	_liveness_case(10, false, "rotrev", 1 << 16, [], false)

	if _failures > 0:
		printerr("%d check(s) failed" % _failures)
		quit(1)
	else:
		print("all fixtures minted into " + _out_dir)
		quit(0)
```
