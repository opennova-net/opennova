extends GutTest

# NovaDebugPickList: the shell-owned debug pick set. Cap, dedupe-by-handle
# (refresh in place), removal, and the changed signal — the model contract
# the overlay's picks section and the highlight view both build on.


func _pick(handle: int, name: String = "thing", tick: int = 1) -> Dictionary:
	return {
		"hit": true,
		"entity_handle": handle,
		"pool": 1,
		"kind": 1,
		"index": handle,
		"bms_id": 1000 + handle,
		"net_id": 0,
		"item_id": 7,
		"name": name,
		"position_godot": Vector3(handle, 0, 0),
		"bound_radius": 2.0,
		"hit_position_godot": Vector3(handle, 1, 0),
		"tick": tick,
	}


func test_add_dedupes_by_handle_and_refreshes_the_row() -> void:
	var list := NovaDebugPickList.new()
	var emissions := [0]
	list.changed.connect(func(): emissions[0] += 1)
	assert_eq(list.add(_pick(5, "crate", 10)), 0)
	assert_eq(list.add(_pick(9, "jeep", 11)), 1)
	assert_eq(list.size(), 2)
	assert_eq(list.add(_pick(5, "crate", 99)), 0,
			"re-picking a listed entity refreshes ITS row, never appends")
	assert_eq(list.size(), 2)
	assert_eq(int(list.get_picks()[0].get("tick", -1)), 99,
			"the refreshed row carries the newest pick metadata")
	assert_eq(emissions[0], 3, "every mutation announces itself")


func test_rejects_misses_and_caps_without_evicting() -> void:
	var list := NovaDebugPickList.new()
	assert_eq(list.add({"hit": false, "blocked": "terrain"}), -1,
			"terrain/water misses never become rows")
	for i in range(NovaDebugPickList.MAX_PICKS):
		assert_eq(list.add(_pick(i)), i)
	assert_true(list.is_full())
	assert_eq(list.add(_pick(100)), -1, "a full list rejects instead of evicting")
	assert_eq(list.size(), NovaDebugPickList.MAX_PICKS)
	assert_eq(list.add(_pick(3, "again", 50)), 3,
			"...but refreshing a listed entity still works at cap")


func test_remove_and_clear_announce() -> void:
	var list := NovaDebugPickList.new()
	list.add(_pick(1))
	list.add(_pick(2))
	var emissions := [0]
	list.changed.connect(func(): emissions[0] += 1)
	list.remove_at(0)
	assert_eq(list.size(), 1)
	assert_eq(int(list.get_picks()[0].get("entity_handle", -1)), 2)
	list.remove_at(99)
	assert_eq(emissions[0], 1, "an out-of-range remove is a silent no-op")
	list.clear()
	assert_eq(list.size(), 0)
	list.clear()
	assert_eq(emissions[0], 2, "clearing an empty list is a silent no-op")


func test_get_picks_returns_deep_copies() -> void:
	var list := NovaDebugPickList.new()
	list.add(_pick(4))
	var copy: Dictionary = list.get_picks()[0]
	copy["name"] = "mutated"
	assert_eq(String(list.get_picks()[0].get("name", "")), "thing",
			"mutating a returned card never desyncs the list")
