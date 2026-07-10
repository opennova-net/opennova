extends GutTest

# Smoke-tests the runtime GameHud overlay's draw path with a real hudpos.def layout
# and a per-frame info dict, with and without art (no VFS root -> placeholders).

const HUDPOS_PATH := "res://../fixtures/def/hudpos.def"


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


func test_weapon_cluster_artless_safe() -> void:
	# The weapon-coupled elements with a real weapon dict but no art/root: text uses the
	# (missing) font guard, the clip indicator and crosshair fall back cleanly.
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
