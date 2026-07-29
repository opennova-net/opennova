extends GutTest

# DebugAnimationPage: the Animation & models debug page. Formats the local
# player's animation scalars and one row per registry animatable, and
# degrades to empty states when the world/registry are gone.

const PageScript := preload("res://engine/debug/pages/debug_animation_page.gd")


class StubModel:
	extends Node
	var clip := ""
	var lod := 0
	var skinned := false

	func get_active_body_clip() -> String:
		return clip

	func get_active_lod() -> int:
		return lod

	func has_skeleton() -> bool:
		return skinned


class StubRegistry:
	extends RefCounted
	var nodes: Array = []

	func get_animatable_nodes() -> Array:
		return nodes


# The value-only sim double the page reads through _ctx.sim(): the local
# player's animation scalars under NovaSimulation's native getter names.
class StubSim:
	extends RefCounted

	func get_local_player_anim_key() -> String:
		return "prone_crawl"

	func get_local_player_body_anim_slot() -> int:
		return 17

	func get_local_player_anim_phase_ticks() -> int:
		return 42


class StubRuntime:
	extends Node
	var registry := StubRegistry.new()
	var sim := StubSim.new()

	func get_sim() -> Object:
		return sim

	func get_registry() -> StubRegistry:
		return registry


class StubWorld:
	extends Node


func _make_page(world: Node = null, runtime: Node = null) -> DebugAnimationPage:
	var ctx := NovaDebugContext.new()
	ctx.options = NovaDebugOptionState.new()
	if world != null:
		ctx.world_source = func(): return world
	if runtime != null:
		ctx.runtime_source = func(): return runtime
	var page: DebugAnimationPage = PageScript.new()
	page.setup(ctx)
	add_child_autofree(page)
	return page


func test_renders_empty_states_without_sources() -> void:
	var page := _make_page()
	page.refresh()
	assert_string_contains((page.find_child("AnimPlayer", true, false) as Label).text,
			"No local player")
	assert_string_contains((page.find_child("AnimModelsHeader", true, false) as Label).text,
			"No live models")


func test_formats_player_scalars_and_model_rows() -> void:
	var world := StubWorld.new()
	add_child_autofree(world)
	var runtime := StubRuntime.new()
	add_child_autofree(runtime)
	var idle := StubModel.new()
	idle.name = "Crate01"
	var soldier := StubModel.new()
	soldier.name = "Rifleman"
	soldier.clip = "run_f"
	soldier.lod = 1
	soldier.skinned = true
	runtime.add_child(idle)
	runtime.add_child(soldier)
	runtime.registry.nodes = [idle, soldier]
	var page := _make_page(world, runtime)
	page.refresh()

	var player := (page.find_child("AnimPlayer", true, false) as Label).text
	assert_string_contains(player, "prone_crawl")
	assert_string_contains(player, "slot 17")
	assert_string_contains(player, "42 ticks")

	var header := (page.find_child("AnimModelsHeader", true, false) as Label).text
	assert_string_contains(header, "2 animatable")
	assert_string_contains(header, "1 skinned")
	assert_string_contains(header, "1 playing")

	var list := page.find_child("AnimModels", true, false) as ItemList
	assert_eq(list.item_count, 2)
	assert_string_contains(list.get_item_text(0), "Crate01")
	var soldier_row := list.get_item_text(1)
	assert_string_contains(soldier_row, "Rifleman")
	assert_string_contains(soldier_row, "clip run_f")
	assert_string_contains(soldier_row, "detail L1")
	assert_string_contains(soldier_row, "skinned")


func test_bone_toggles_ride_the_option_registry() -> void:
	var page := _make_page()
	assert_not_null(page.find_child("show_skeletons", true, false),
			"the skeleton view checkbox lives here now")
	assert_not_null(page.find_child("show_user_points", true, false),
			"the user-point view checkbox lives here now")
