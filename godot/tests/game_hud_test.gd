extends GutTest

# Smoke-tests the runtime GameHud overlay's draw path with a real hudpos.def layout
# and a per-frame info dict, with and without art (no VFS root -> placeholders).

const HUDPOS_PATH := "res://../fixtures/def/hudpos.def"

var _temp_dirs: Array[String] = []


class WeaponClusterOnlyHud:
	extends GameHud

	func _draw() -> void:
		_draw_weapon_cluster(Vector2(1024, 768), 0)


func after_each() -> void:
	for dir_path in _temp_dirs:
		for file_name in DirAccess.get_files_at(dir_path):
			DirAccess.remove_absolute(dir_path.path_join(file_name))
		DirAccess.remove_absolute(dir_path)
	_temp_dirs.clear()


func _load_temp_layout(lines: PackedStringArray, textures: PackedStringArray) -> Dictionary:
	var dir_path := OS.get_temp_dir().path_join("game_hud_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(dir_path), OK)
	_temp_dirs.append(dir_path)
	for texture_name in textures:
		# Minimal uncompressed 2x2 BGRA TGA, loaded through GameHud's real VFS path.
		var bytes := PackedByteArray()
		bytes.resize(18 + 2 * 2 * 4)
		bytes[2] = 2
		bytes[12] = 2
		bytes[14] = 2
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
	return {"layout": layout, "root": root}


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
	]), PackedStringArray([
		"stance_1.tga", "stance_2.tga", "stance_old.tga",
		"stance_3.tga", "stance_5.tga", "stance_6.tga",
	]))
	var hud := GameHud.new()
	add_child_autofree(hud)
	hud.set_layout(fixture["layout"], fixture["root"])

	assert_eq(hud._stance_textures.size(), 6, "The six retail stance slots stay index-addressable.")
	assert_eq(hud._stance_offsets[0], Vector2(10, 11), "Out-of-order ID 0 occupies slot 0.")
	assert_eq(hud._stance_offsets[2], Vector2(22, 23), "The later duplicate ID replaces slot 2.")
	assert_null(hud._stance_textures[3], "A missing ID leaves its slot unloaded for the six-frame gate.")
	assert_eq(hud._stance_offsets[5], Vector2(50, 51), "Out-of-order ID 5 occupies slot 5.")
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


func test_crosshair_style_clamps_and_reloads_live() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"ALPHAFADE 30 50 3",
	]), PackedStringArray(["cross01.tga", "cross25.tga"]))
	var hud := GameHud.new()
	add_child_autofree(hud)
	hud.set_crosshair_style(99)
	hud.set_layout(fixture["layout"], fixture["root"])
	var high_style_texture: Texture2D = hud._crosshair_tex
	assert_not_null(high_style_texture, "Styles above 24 clamp to cross25.tga.")

	hud.set_crosshair_style(-4)
	assert_not_null(hud._crosshair_tex, "Negative styles clamp and live-reload cross01.tga.")
	assert_ne(hud._crosshair_tex.get_instance_id(), high_style_texture.get_instance_id(),
		"Changing style reloads the selected texture on a live HUD.")
	high_style_texture = null
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
