extends GutTest

# DebugAnimationPage: the Animation & models debug page. Formats the local
# player's animation scalars and one row per registry animatable, and
# degrades to empty states when the world/registry are gone.

const PageScript := preload("res://adapter/debug/pages/debug_animation_page.gd")


# Real native models: state is DRIVEN through the public surface (committed
# rig + object fixtures back the clip/skeleton legs), never faked by
# overriding. The object data makes rebuild() construct the Skeleton3D.
const RIGGED_3DI := "res://../fixtures/threedi/3di3/Shed.3di"


static func _rigged_model(parent: Node, clip: String) -> NovaObjectModel:
	# Enter the tree FIRST (global_transform/bounds math must be valid before
	# rebuild), the production placer order.
	var m := NovaObjectModel.new()
	parent.add_child(m)
	var data := NovaObjectData.new()
	assert(data.open_file(ProjectSettings.globalize_path(RIGGED_3DI)) == OK)
	var root := NovaResourceRoot.new()
	assert(root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/anim")) == OK)
	var sk := NovaSkeletalAnim.new()
	assert(sk.load_from_bad_files(root, "idle.bad",
			{ "anim_idle": "idle.bad", "anim_walk": "walk.bad" }))
	m.set_object_data(data)
	m.set_skeletal_anim(sk)
	if not clip.is_empty():
		m.play_body_clip(clip)
	return m


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

	func get_local_player_anim_source_key() -> String:
		return "stand_idle"

	func get_local_player_anim_source_phase_ticks() -> int:
		return 12

	func get_local_player_anim_blend_weight() -> float:
		return 0.25


class StubRuntime:
	extends Node
	var registry := NovaEntityIndex.new()
	var sim := StubSim.new()

	func get_sim() -> Object:
		return sim

	func get_registry() -> NovaEntityIndex:
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
	var idle := NovaObjectModel.new()
	idle.name = "Crate01"
	runtime.add_child(idle)
	var soldier := _rigged_model(runtime, "anim_walk")
	soldier.name = "Rifleman"
	runtime.registry.build([
		{ "model": idle, "ref": { "bms_id": 1 } },
		{ "model": soldier, "ref": { "bms_id": 2 } },
	], [])
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
	var crate_row := ""
	var soldier_row := ""
	for i in range(list.item_count):
		var row := list.get_item_text(i)
		if row.contains("Crate01"):
			crate_row = row
		if row.contains("Rifleman"):
			soldier_row = row
	assert_string_contains(crate_row, "Crate01")
	assert_string_contains(soldier_row, "Rifleman")
	assert_string_contains(soldier_row, "clip anim_walk")
	assert_string_contains(soldier_row, "skinned")


func test_discovers_dynamic_world_models_outside_the_placed_registry() -> void:
	var world := StubWorld.new()
	add_child_autofree(world)
	var runtime := StubRuntime.new()
	add_child_autofree(runtime)
	var remote := _rigged_model(world, "anim_walk")
	remote.name = "RemotePlayerAvatar"

	var page := _make_page(world, runtime)
	page.refresh()

	var list := page.find_child("AnimModels", true, false) as ItemList
	assert_eq(list.item_count, 1,
			"wire/player models outside the placed registry are still inspectable")
	if list.item_count != 1:
		return
	assert_string_contains(list.get_item_text(0), "RemotePlayerAvatar")
	assert_string_contains(
			(page.find_child("AnimModelDetail", true, false) as Label).text,
			"RemotePlayerAvatar")


func test_steady_refresh_does_not_reset_model_list_browsing_position() -> void:
	var world := StubWorld.new()
	add_child_autofree(world)
	var runtime := StubRuntime.new()
	add_child_autofree(runtime)
	var entries: Array = []
	for index in range(36):
		var model := NovaObjectModel.new()
		model.name = "Model_%02d" % index
		runtime.add_child(model)
		entries.append({ "model": model, "ref": { "bms_id": 1000 + index } })
	runtime.registry.build(entries, [])
	var page := _make_page(world, runtime)
	page.size = Vector2(300, 190)
	page.refresh()
	await wait_process_frames(2)

	var list := page.find_child("AnimModels", true, false) as ItemList
	var scroll := list.get_v_scroll_bar()
	assert_gt(scroll.max_value, scroll.page,
			"the fixture has enough models to browse")
	scroll.value = scroll.max_value
	await wait_process_frames(1)
	var before := scroll.value

	page.refresh()
	await wait_process_frames(1)
	assert_almost_eq(scroll.value, before, 0.01,
			"unchanged live data updates rows without yanking the list to the top")


func test_explains_the_local_player_transition_and_blend() -> void:
	var runtime := StubRuntime.new()
	add_child_autofree(runtime)
	var page := _make_page(null, runtime)
	page.refresh()

	var player := (page.find_child("AnimPlayer", true, false) as Label).text
	assert_string_contains(player, "stand_idle")
	assert_string_contains(player, "12 ticks")
	assert_string_contains(player, "prone_crawl")
	assert_string_contains(player, "42 ticks")
	assert_string_contains(player, "25%")
	assert_string_contains(player, "source")
	assert_string_contains(player, "target")


func test_selected_model_detail_survives_registry_reorder_by_stable_identity() -> void:
	var runtime := StubRuntime.new()
	add_child_autofree(runtime)
	var crate := NovaObjectModel.new()
	crate.name = "Crate01"
	crate.set_meta("entity_ref", {"bms_id": 101, "kind": 3, "index": 0})
	var soldier := _rigged_model(runtime, "anim_walk")
	soldier.name = "Rifleman"
	soldier.set_animation_time(0.125)
	soldier.play_part_anim(1, 1, 4.0)
	soldier.set_ctrl_value("HEAT_GLOW", 32768)
	soldier.set_meta("entity_ref", {
		"bms_id": 220,
		"kind": 1,
		"index": 7,
		"item_id": 45,
	})
	runtime.add_child(crate)
	runtime.registry.build([
		{ "model": crate, "ref": { "bms_id": 101, "kind": 3, "index": 0 } },
		{ "model": soldier, "ref": { "bms_id": 220, "kind": 1, "index": 7 } },
	], [])
	var page := _make_page(null, runtime)
	page.refresh()

	var list := page.find_child("AnimModels", true, false) as ItemList
	var soldier_index := -1
	for i in range(list.item_count):
		if list.get_item_text(i).contains("Rifleman"):
			soldier_index = i
			break
	assert_gte(soldier_index, 0, "the animated model has a selectable row")
	if soldier_index < 0:
		return
	list.select(soldier_index)
	list.item_selected.emit(soldier_index)

	var detail := page.find_child("AnimModelDetail", true, false) as Label
	assert_not_null(detail, "selection opens a useful model detail card")
	if detail == null:
		return
	assert_string_contains(detail.text, "Rifleman")
	assert_string_contains(detail.text, "BMS 220")
	assert_string_contains(detail.text, "anim_walk")
	assert_string_contains(detail.text, "0.125 s")
	assert_string_contains(detail.text, "skinned")
	assert_string_contains(detail.text, "VEHICLE_SPECIAL1")
	assert_string_contains(detail.text, "HEAT_GLOW")
	assert_string_contains(detail.text, "32768")

	# Change which model is actively playing as well as registry order, so the
	# useful-model sort moves the selected row. Selection must follow BMS 220,
	# not whichever model inherits the old row index.
	var crate_root := NovaResourceRoot.new()
	assert_eq(crate_root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/anim")), OK)
	var crate_sk := NovaSkeletalAnim.new()
	assert_true(crate_sk.load_from_bad_files(crate_root, "idle.bad",
			{ "anim_idle": "idle.bad" }))
	crate.set_skeletal_anim(crate_sk)
	crate.play_body_clip("anim_idle")
	soldier.stop_body_clip()
	runtime.registry.build([
		{ "model": soldier, "ref": { "bms_id": 220, "kind": 1, "index": 7 } },
		{ "model": crate, "ref": { "bms_id": 101, "kind": 3, "index": 0 } },
	], [])
	page.refresh()
	var moved_soldier_index := -1
	for i in range(list.item_count):
		if list.get_item_text(i).contains("Rifleman"):
			moved_soldier_index = i
			break
	assert_ne(moved_soldier_index, soldier_index,
			"the model row actually moved during the regression scenario")
	assert_true(list.is_selected(moved_soldier_index),
			"the same BMS model remains selected when registry order changes")
	assert_string_contains(detail.text, "Rifleman")
	assert_string_contains(detail.text, "BMS 220")


func test_bone_toggles_ride_the_option_registry() -> void:
	var page := _make_page()
	assert_not_null(page.find_child("show_skeletons", true, false),
			"the skeleton view checkbox lives here now")
	assert_not_null(page.find_child("show_user_points", true, false),
			"the user-point view checkbox lives here now")
