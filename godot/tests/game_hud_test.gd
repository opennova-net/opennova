extends GutTest

# Smoke-tests the runtime GameHud overlay's draw path with a real hudpos.def layout
# and a per-frame info dict, with and without art (no VFS root -> placeholders).

const HUDPOS_PATH := "res://../fixtures/def/hudpos.def"
const WEAPON_PATH := "res://../fixtures/def/weapon.def"
const PlayerViewEffectsScript := preload("res://engine/world/player_view_effects.gd")

var _temp_dirs: Array[String] = []


class WeaponClusterOnlyHud:
	extends GameHud

	func _draw() -> void:
		_draw_weapon_cluster(Vector2(1024, 768), 0)


class StanceOnlyHud:
	extends GameHud

	func _draw() -> void:
		_draw_stance(Vector2(1024, 768), 100)


func after_each() -> void:
	for dir_path in _temp_dirs:
		for file_name in DirAccess.get_files_at(dir_path):
			DirAccess.remove_absolute(dir_path.path_join(file_name))
		DirAccess.remove_absolute(dir_path)
	_temp_dirs.clear()


func _load_temp_layout(lines: PackedStringArray, textures: PackedStringArray,
		texture_sizes: Dictionary = {}) -> Dictionary:
	var dir_path := OS.get_temp_dir().path_join("game_hud_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(dir_path), OK)
	_temp_dirs.append(dir_path)
	for texture_name in textures:
		# Minimal uncompressed BGRA TGA, loaded through GameHud's real VFS path.
		var texture_size: Vector2i = texture_sizes.get(texture_name, Vector2i(2, 2))
		var bytes := PackedByteArray()
		bytes.resize(18 + texture_size.x * texture_size.y * 4)
		bytes[2] = 2
		bytes[12] = texture_size.x & 0xff
		bytes[13] = (texture_size.x >> 8) & 0xff
		bytes[14] = texture_size.y & 0xff
		bytes[15] = (texture_size.y >> 8) & 0xff
		bytes[16] = 32
		bytes[17] = 0x28
		for i in range(18, bytes.size()):
			bytes[i] = 0xff
		var texture_file := FileAccess.open(dir_path.path_join(texture_name), FileAccess.WRITE)
		assert_not_null(texture_file)
		texture_file.store_buffer(bytes)
		texture_file.close()
	var def_file := FileAccess.open(dir_path.path_join("hudpos.def"), FileAccess.WRITE)
	assert_not_null(def_file)
	def_file.store_string("\n".join(lines))
	def_file.close()
	var layout := NovaHudPos.new()
	assert_eq(layout.load(dir_path.path_join("hudpos.def")), OK)
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(dir_path), OK)
	return {"layout": layout, "root": root, "dir": dir_path}


func _load_weapon(name: String) -> Dictionary:
	var weapons := NovaWeaponDatabase.new()
	assert_eq(weapons.load(ProjectSettings.globalize_path(WEAPON_PATH)), OK,
		"weapon.def fixture loads")
	var index := weapons.find_weapon(name)
	assert_gte(index, 0, "%s exists in the weapon.def fixture" % name)
	return weapons.get_weapon(index) if index >= 0 else {}


func test_sighted_m4_draws_all_authored_sight_card_rows() -> void:
	var weapon := _load_weapon("WPN_M4AUTO")
	assert_false(weapon.is_empty())
	if weapon.is_empty():
		return
	assert_eq(int(weapon.get("flags", 0)) & 0x02000003, 0x2,
		"M4AUTO is Sighted and has neither Scoped nor NoCardSwitch")
	var sights: Array = weapon.get("sights", [])
	assert_eq(sights.size(), 3, "M4AUTO retains all three authored SIGHTS rows")
	var textures := PackedStringArray()
	for entry in sights:
		var sight: Dictionary = entry
		textures.append(String(sight.get("texture", "")))
	assert_eq(textures, PackedStringArray([
		"car15aim.tga", "car15gls.tga", "reddot1.tga",
	]), "The final authored row is the committed M4 red-dot analogue")
	var reticle: Dictionary = sights[2]
	assert_eq(int(reticle.get("blend", -1)), 1, "The red-dot row keeps additive blend")
	assert_true(bool(reticle.get("scale", false)), "The red-dot row keeps its scale flag")

	var fixture := _load_temp_layout(PackedStringArray([
		"ALPHAFADE 30 50 3",
	]), textures)
	var hud := GameHud.new()
	hud.size = Vector2(1024, 768)
	add_child_autofree(hud)
	hud.set_layout(fixture["layout"], fixture["root"])
	hud.set_weapon(PlayerHudWeaponDef.from_weapon_dict(weapon), "M4")
	hud.update_info({"scope_card": true})
	await get_tree().process_frame

	assert_eq(hud.get_child_count(), 3,
		"Settled ADS constructs every authored M4AUTO sight-card row")
	if hud.get_child_count() != 3:
		return
	var scale_v := hud.get_viewport_rect().size / Vector2(1024, 768)
	for i in range(sights.size()):
		var row := hud.get_child(i) as Control
		assert_not_null(row, "SIGHTS row %d is a drawable Control" % i)
		assert_true(row.visible, "SIGHTS row %d is visible while the card is up" % i)
		RenderingServer.canvas_item_set_custom_rect(row.get_canvas_item(), false)
		var sight: Dictionary = sights[i]
		var x1 := float(sight.get("x1", 0))
		var y1 := float(sight.get("y1", 0))
		var expected := Rect2(
			Vector2(x1, y1) * scale_v,
			Vector2(float(sight.get("x2", 0)) - x1,
				float(sight.get("y2", 0)) - y1) * scale_v)
		assert_eq(RenderingServer.debug_canvas_item_get_rect(row.get_canvas_item()), expected,
			"SIGHTS row %d emits its authored draw rectangle" % i)
	var reticle_row := hud.get_child(2) as Control
	var reticle_material := reticle_row.material as CanvasItemMaterial
	assert_not_null(reticle_material, "The red-dot row gets a canvas material")
	if reticle_material != null:
		assert_eq(reticle_material.blend_mode, CanvasItemMaterial.BLEND_MODE_ADD,
			"The M4 red-dot layer renders additively")
	hud.update_info({"scope_card": true, "binoculars_view_active": true})
	await get_tree().process_frame
	for i in range(sights.size()):
		assert_false((hud.get_child(i) as Control).visible,
				"Binocular view suppresses authored SIGHTS row %d" % i)
	hud.free()


func test_draws_with_layout() -> void:
	var hud := GameHud.new()
	hud.size = Vector2(1024, 768)
	add_child_autofree(hud)
	var hp := NovaHudPos.new()
	assert_eq(hp.load(ProjectSettings.globalize_path(HUDPOS_PATH)), OK, "Fixture loads.")
	hud.set_layout(hp, null) # null root -> no textures, placeholder draw
	hud.update_info({"health_fraction": 0.5, "stance": 1, "team": 0, "objective": "Defend the FARP"})
	await get_tree().process_frame
	assert_true(is_instance_valid(hud), "HUD survives a draw with the fixture layout.")


func test_draws_without_layout() -> void:
	var hud := GameHud.new()
	hud.size = Vector2(800, 600)
	add_child_autofree(hud)
	# No set_layout: _draw must no-op cleanly.
	hud.update_info({"health_fraction": 1.0})
	await get_tree().process_frame
	assert_true(is_instance_valid(hud), "HUD with no layout draws nothing without error.")


func test_stance_index_bounds_safe() -> void:
	var hud := GameHud.new()
	hud.size = Vector2(1024, 768)
	add_child_autofree(hud)
	var hp := NovaHudPos.new()
	assert_eq(hp.load(ProjectSettings.globalize_path(HUDPOS_PATH)), OK)
	hud.set_layout(hp, null)
	# Out-of-range stance must not crash the draw (including the cross-fade ghost).
	hud.update_info({"health_fraction": 0.9, "stance": 99, "team": 1, "objective": "", "ticks": 10})
	await get_tree().process_frame
	hud.update_info({"health_fraction": 0.9, "stance": 1, "team": 1, "objective": "", "ticks": 20})
	await get_tree().process_frame
	assert_true(is_instance_valid(hud), "Out-of-range stance index is draw-safe.")


func test_stance_assets_use_explicit_ids_for_slots() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"HUDSTANCEPOS 21 630",
		"ALPHAFADE 30 50 3",
		"HUDSTANCE 5 50 51 stance_6.tga PARACHUTE",
		"HUDSTANCE 0 10 11 stance_1.tga STAND",
		"HUDSTANCE 2 20 21 stance_old.tga PRONE_OLD",
		"HUDSTANCE 4 40 41 stance_5.tga EMPLACED",
		"HUDSTANCE 1 12 13 stance_2.tga CROUCH",
		"HUDSTANCE 2 22 23 stance_3.tga PRONE",
		"HUDSTANCE 3 30 31 stance_4.tga SITTING",
	]), PackedStringArray([
		"stance_1.tga", "stance_2.tga", "stance_old.tga",
		"stance_3.tga", "stance_4.tga", "stance_5.tga", "stance_6.tga",
	]))
	var hud := StanceOnlyHud.new()
	hud.size = Vector2(1024, 768)
	add_child_autofree(hud)
	hud.set_layout(fixture["layout"], fixture["root"])
	hud.update_info({"stance": 2, "ticks": 100})
	hud.queue_redraw()
	await get_tree().process_frame
	RenderingServer.canvas_item_set_custom_rect(hud.get_canvas_item(), false)

	assert_eq(RenderingServer.debug_canvas_item_get_rect(hud.get_canvas_item()),
		Rect2(43, 653, 128, 128),
		"Explicit IDs select slots independent of file order; the later ID 2 offset replaces the earlier one.")
	hud.free()


func test_weapon_cluster_artless_safe() -> void:
	# The weapon-coupled elements with a real weapon dict but no art/root: text uses the
	# (missing) font guard, and the clip indicator and crosshair skip cleanly.
	var hud := GameHud.new()
	hud.size = Vector2(1024, 768)
	add_child_autofree(hud)
	var hp := NovaHudPos.new()
	assert_eq(hp.load(ProjectSettings.globalize_path(HUDPOS_PATH)), OK)
	hud.set_layout(hp, null)
	hud.set_weapon(PlayerHudWeaponDef.from_weapon_dict({
		"name": "WPN_AK47",
		"clipsize": 30,
		"round_type": "AMMO_762",
		"error": PackedFloat32Array([0.05, 0.2, 0.25, 0.05, 0.1, 0.15]),
		"hudclipgfx_texture": "H_clip.tga",
		"hudclipgfx_offset": Vector2i(0, 0),
		"hudrndgfx_texture": "H_round.tga",
		"hudrndgfx_offset": Vector2i(9, 0),
		"hudrndgfx_layout": Vector3i(18, 0, 1),
	}), "AK-47")
	hud.update_info({
		"health_fraction": 0.8, "stance": 0, "team": 1, "objective": "",
		"weapon_active": true, "clip": 12, "reserve": 90,
		"scope_engaged": false, "fov_deg": 80.0, "ticks": 100,
	})
	await get_tree().process_frame
	# Scoped-in hides the crosshair; empty weapon clears the cluster.
	hud.update_info({
		"health_fraction": 0.8, "stance": 0, "team": 1, "objective": "",
		"weapon_active": true, "clip": 12, "reserve": 90,
		"scope_engaged": true, "fov_deg": 40.0, "ticks": 160,
	})
	await get_tree().process_frame
	hud.set_weapon(null, "")
	await get_tree().process_frame
	assert_true(is_instance_valid(hud), "Weapon cluster draw is art-less safe.")


func test_weapon_cluster_draws_nothing_without_weapon() -> void:
	var hud := WeaponClusterOnlyHud.new()
	hud.size = Vector2(1024, 768)
	add_child_autofree(hud)
	hud.queue_redraw()
	await get_tree().process_frame
	RenderingServer.canvas_item_set_custom_rect(hud.get_canvas_item(), false)

	assert_eq(RenderingServer.debug_canvas_item_get_rect(hud.get_canvas_item()), Rect2(),
		"No weapon produces no retail crosshair draw commands.")


func test_weapon_cluster_draws_nothing_without_crosshair_texture() -> void:
	var hud := WeaponClusterOnlyHud.new()
	hud.size = Vector2(1024, 768)
	add_child_autofree(hud)
	hud.set_weapon(PlayerHudWeaponDef.from_weapon_dict({
		"name": "WPN_TEST",
		"error": PackedFloat32Array([0.05, 0.2, 0.25, 0.05, 0.1, 0.15]),
	}), "Test weapon")
	hud.update_info({
		"weapon_active": true,
		"clip": -1,
		"reserve": -1,
		"scope_engaged": false,
		"stance": 0,
		"fov_deg": 80.0,
		"ticks": 0,
	})
	hud.queue_redraw()
	await get_tree().process_frame
	RenderingServer.canvas_item_set_custom_rect(hud.get_canvas_item(), false)

	assert_eq(RenderingServer.debug_canvas_item_get_rect(hud.get_canvas_item()), Rect2(),
		"A missing crosshair texture produces no invented replacement reticle.")


func test_binocular_view_hides_weapon_crosshair() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"ALPHAFADE 30 50 3",
	]), PackedStringArray(["cross01.tga"]), {
		"cross01.tga": Vector2i(8, 8),
	})
	var hud := WeaponClusterOnlyHud.new()
	hud.size = Vector2(1024, 768)
	add_child_autofree(hud)
	hud.set_layout(fixture["layout"], fixture["root"])
	hud.set_weapon(PlayerHudWeaponDef.from_weapon_dict({
		"name": "WPN_TEST",
		"error": PackedFloat32Array([0.0, 0.0, 0.0, 0.0, 0.0, 0.0]),
	}), "Test weapon")
	hud.update_info({
		"weapon_active": true,
		"scope_engaged": false,
		"binoculars_view_active": true,
		"stance": 0,
		"fov_deg": 20.0,
	})
	await get_tree().process_frame
	RenderingServer.canvas_item_set_custom_rect(hud.get_canvas_item(), false)

	assert_eq(RenderingServer.debug_canvas_item_get_rect(hud.get_canvas_item()), Rect2(),
			"Binocular view hides the normal weapon crosshair without hiding the HUD.")


func test_binocular_range_smoothing_matches_retail_steps() -> void:
	assert_eq(PlayerViewEffectsScript.smooth_range_value(-10, 1000), 1000,
			"Corrections larger than 1000 snap to the target.")
	assert_eq(PlayerViewEffectsScript.smooth_range_value(0, 1000), 111)
	assert_eq(PlayerViewEffectsScript.smooth_range_value(0, 111), 33)
	assert_eq(PlayerViewEffectsScript.smooth_range_value(0, 33), 11)
	assert_eq(PlayerViewEffectsScript.smooth_range_value(0, 11), 3)
	assert_eq(PlayerViewEffectsScript.smooth_range_value(0, 3), 1)
	assert_eq(PlayerViewEffectsScript.smooth_range_value(10, 1), 7,
			"Negative corrections use the same stepped easing.")
	assert_eq(PlayerViewEffectsScript.smooth_range_value(1, 0), 1,
			"The raw range target clamps to the retail 1..1000 interval.")


func test_player_view_effects_draw_retail_asset_stack() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"ALPHAFADE 30 50 3",
	]), PackedStringArray([
		"Binoculr.tga", "BinoCH.tga", "BNumbers.tga", "NVG.tga", "Nvgscale.tga",
	]), {
		"Binoculr.tga": Vector2i(512, 256),
		"BinoCH.tga": Vector2i(128, 128),
		"BNumbers.tga": Vector2i(16, 160),
		"NVG.tga": Vector2i(512, 512),
		"Nvgscale.tga": Vector2i(16, 80),
	})
	var effects := PlayerViewEffectsScript.new()
	effects.size = Vector2(1024, 768)
	add_child_autofree(effects)
	effects.set_resource_root(fixture["root"])
	effects.update_info({
		"binoculars_view_active": true,
		"binocular_range": 1000,
		"nvg_visible": true,
		"nvg_gain": 4,
	})
	await get_tree().process_frame
	RenderingServer.canvas_item_set_custom_rect(effects.get_canvas_item(), false)

	assert_eq(RenderingServer.debug_canvas_item_get_rect(effects.get_canvas_item()),
			Rect2(0, 0, 1024, 768),
			"Retail masks cover the viewport while inset art stays in design coordinates.")
	assert_eq(effects.get_child_count(true), 1, "The NVG post-process is an internal child.")
	assert_true((effects.get_child(0, true) as CanvasItem).visible,
			"First-person-visible NVG enables the post-process.")
	effects.update_info({"nvg_visible": false})
	assert_false((effects.get_child(0, true) as CanvasItem).visible,
			"Camera suppression hides the post-process without consuming simulation state.")


func test_crosshair_style_clamps_and_reloads_live() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"ALPHAFADE 30 50 3",
	]), PackedStringArray(["cross01.tga", "cross25.tga"]), {
		"cross01.tga": Vector2i(2, 2),
		"cross25.tga": Vector2i(6, 6),
	})
	var hud := WeaponClusterOnlyHud.new()
	hud.size = Vector2(1024, 768)
	add_child_autofree(hud)
	hud.set_crosshair_style(99)
	hud.set_layout(fixture["layout"], fixture["root"])
	hud.set_weapon(PlayerHudWeaponDef.from_weapon_dict({
		"name": "WPN_TEST",
		"error": PackedFloat32Array([0.0, 0.0, 0.0, 0.0, 0.0, 0.0]),
	}), "Test weapon")
	hud.update_info({
		"weapon_active": true, "stance": 0, "scope_engaged": false,
		"fov_deg": 80.0, "ticks": 0,
	})
	await get_tree().process_frame
	RenderingServer.canvas_item_set_custom_rect(hud.get_canvas_item(), false)
	var high_style_rect := RenderingServer.debug_canvas_item_get_rect(hud.get_canvas_item())
	assert_ne(high_style_rect, Rect2(), "Styles above 24 clamp to drawable cross25.tga.")

	hud.set_crosshair_style(-4)
	await get_tree().process_frame
	RenderingServer.canvas_item_set_custom_rect(hud.get_canvas_item(), false)
	var low_style_rect := RenderingServer.debug_canvas_item_get_rect(hud.get_canvas_item())
	assert_ne(low_style_rect, Rect2(), "Negative styles clamp to drawable cross01.tga.")
	assert_gt(high_style_rect.size.x, low_style_rect.size.x,
		"Changing style live-reloads the differently sized selected texture.")
	hud.free()


func test_message_feed_smoke() -> void:
	var hud := GameHud.new()
	hud.size = Vector2(1024, 768)
	add_child_autofree(hud)
	var hp := NovaHudPos.new()
	assert_eq(hp.load(ProjectSettings.globalize_path(HUDPOS_PATH)), OK)
	hud.set_layout(hp, null)
	hud.update_info({"health_fraction": 1.0, "ticks": 50})
	hud.push_message("Move to the extraction point")
	await get_tree().process_frame
	assert_true(is_instance_valid(hud), "A pushed triggered-text line draws safely.")


# The weapon heat bar draws at nonzero heat inside the HUDHEAT rect and stays
# hidden (draw-safe) at zero — the original's hudInfo+60 gate.
# [orig: HUD_DrawWeaponHeatBar @0x599700 gate @0x59970a]
func test_heat_bar_draws_at_nonzero_heat() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"HUDHEAT 10 579 133 598",
		"HUDHEATBORDER 90,200,200,200",
		"stancecolor_bad 200,175,009,009",
	]), PackedStringArray())
	var hud := GameHud.new()
	hud.size = Vector2(1024, 768)
	add_child_autofree(hud)
	hud.set_layout(fixture["layout"], fixture["root"])
	hud.update_info({"health_fraction": 1.0, "heat": 0})
	await get_tree().process_frame
	hud.update_info({"health_fraction": 1.0, "heat": 0x8000})
	await get_tree().process_frame
	hud.update_info({"health_fraction": 1.0, "heat": 0x20000}) # above the clamp
	await get_tree().process_frame
	assert_true(is_instance_valid(hud), "The heat bar draws at any heat value.")


# The waypoint label draws name + distance at the HUDWPDINFO anchor for every
# alignment form (with a REAL .fnt so the draw math runs), and hides cleanly
# when the presenter omits the entry.
# [orig: HUD_DrawWaypointNameAndDistance @0x5947a0; retail authors align "right"]
func test_waypoint_label_draws_each_alignment() -> void:
	for align in ["right", "center", "left"]:
		var fixture := _load_temp_layout(PackedStringArray([
			"fonthud1_hi Gunpl22b.fnt",
			"HUDWPDINFO 1013,448,0,%s" % align,
		]), PackedStringArray())
		# A real font in the temp root so _font resolves and the label really draws.
		var fnt_bytes := FileAccess.get_file_as_bytes("res://../fixtures/fnt/Gunpl22b.fnt")
		assert_true(fnt_bytes.size() > 0, "font fixture present")
		var fnt := FileAccess.open(String(fixture["dir"]).path_join("Gunpl22b.fnt"), FileAccess.WRITE)
		fnt.store_buffer(fnt_bytes)
		fnt.close()
		var root := NovaResourceRoot.new()
		assert_eq(root.set_root_dir(fixture["dir"]), OK)
		var hud := GameHud.new()
		hud.size = Vector2(1024, 768)
		add_child_autofree(hud)
		hud.set_layout(fixture["layout"], root)
		# No waypoint entry: the label hides.
		hud.update_info({"health_fraction": 1.0})
		await get_tree().process_frame
		hud.update_info({"health_fraction": 1.0,
			"waypoint": {"name": "North Sea Village", "distance_m": 143}})
		await get_tree().process_frame
		# The box-hidden form (field 3) still draws the texts.
		hud.update_info({"health_fraction": 1.0,
			"waypoint": {"name": "", "distance_m": 9}})
		await get_tree().process_frame
		assert_true(is_instance_valid(hud), "waypoint label draws for align %s" % align)
