extends GutTest

# DebugPickList: the shell-owned debug pick set. Cap with oldest-first
# eviction, dedupe-by-handle (refresh in place), clear, and the picked signal
# carrying the handle — the model contract the pick session (the F3 Entities
# selection forward) builds on.


func _pick(handle: int, name: String = "thing", tick: int = 1) -> DebugPickCard:
	var card := DebugPickCard.new()
	card.hit = true
	card.entity_handle = handle
	card.pool = 1
	card.kind = 1
	card.index = handle
	card.bms_id = 1000 + handle
	card.item_id = 7
	card.name = name
	card.position_godot = Vector3(handle, 0, 0)
	card.bound_radius = 2.0
	card.hit_position_godot = Vector3(handle, 1, 0)
	card.tick = tick
	return card


func _miss(blocked: String) -> DebugPickCard:
	var card := DebugPickCard.new()
	card.blocked = blocked
	return card


func _hit_without_handle() -> DebugPickCard:
	var card := DebugPickCard.new()
	card.hit = true
	return card


func test_add_dedupes_by_handle_and_refreshes_the_row() -> void:
	var list := DebugPickList.new()
	var picked: Array[int] = []
	list.picked.connect(func(handle: int) -> void: picked.append(handle))
	assert_eq(list.add(_pick(5, "crate", 10)), 0)
	assert_eq(list.add(_pick(9, "jeep", 11)), 1)
	assert_eq(list.get_picks().size(), 2)
	assert_eq(list.add(_pick(5, "crate", 99)), 0,
			"re-picking a listed entity refreshes ITS row, never appends")
	assert_eq(list.get_picks().size(), 2)
	assert_eq(list.get_picks()[0].tick, 99,
			"the refreshed row carries the newest pick metadata")
	assert_eq(picked, [5, 9, 5], "every landed pick announces its handle, refreshes included")


func test_rejects_misses_and_evicts_the_oldest_at_cap() -> void:
	var list := DebugPickList.new()
	var picked: Array[int] = []
	list.picked.connect(func(handle: int) -> void: picked.append(handle))
	assert_eq(list.add(_miss("terrain")), -1,
			"terrain/water misses never become rows")
	assert_eq(list.add(_hit_without_handle()), -1,
			"a hit without a handle never becomes a row")
	assert_true(picked.is_empty(), "a rejected pick announces nothing")
	for i in range(DebugPickList.MAX_PICKS):
		assert_eq(list.add(_pick(i)), i)
	assert_eq(list.add(_pick(100)), DebugPickList.MAX_PICKS - 1,
			"a full list evicts its oldest row and appends (picking never wedges)")
	assert_eq(list.get_picks().size(), DebugPickList.MAX_PICKS)
	assert_eq(list.get_picks()[0].entity_handle, 1, "the oldest row left")
	assert_eq(list.get_picks()[DebugPickList.MAX_PICKS - 1].entity_handle, 100,
			"the new row is last")
	assert_eq(list.add(_pick(3, "again", 50)), 2,
			"refreshing a listed entity still works at cap")


func test_clear_empties_the_list() -> void:
	var list := DebugPickList.new()
	list.add(_pick(1))
	list.add(_pick(2))
	list.clear()
	assert_eq(list.get_picks().size(), 0)
	list.clear()
	assert_eq(list.get_picks().size(), 0, "clearing an empty list is a no-op")


func test_get_picks_returns_deep_copies() -> void:
	var list := DebugPickList.new()
	list.add(_pick(4))
	var copy := list.get_picks()[0]
	copy.name = "mutated"
	assert_eq(list.get_picks()[0].name, "thing",
			"mutating a returned card never desyncs the list")
