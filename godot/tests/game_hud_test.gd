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
	# Out-of-range stance must not crash the draw.
	hud.update_info({"health_fraction": 0.9, "stance": 99, "team": 1, "objective": ""})
	await get_tree().process_frame
	assert_true(is_instance_valid(hud), "Out-of-range stance index is draw-safe.")
