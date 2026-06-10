extends GutTest

# NovaEditHistory: the GDExtension boxing of the shared C++ undo/redo core
# (libs/oned_edit). Covers the two flavors the GDScript editors use — the
# whole-document begin/commit bracket with undo_swap/redo_swap adoption, and
# the caller-applied push_step/pop_undo/pop_redo delta stacks — plus the
# NIL-as-empty sentinel contract and the dirty-vs-baseline query.


func _bytes(text: String) -> PackedByteArray:
	return text.to_utf8_buffer()


# --- Whole-document bracket -----------------------------------------------

func test_commit_without_change_records_nothing() -> void:
	var h := NovaEditHistory.new()
	var doc := _bytes("a")
	h.begin_edit(doc)
	assert_false(h.commit_edit(doc), "Unchanged document should record no step.")
	assert_false(h.can_undo())


func test_begin_commit_undo_swap_round_trip() -> void:
	var h := NovaEditHistory.new()
	var doc := _bytes("v1")
	h.begin_edit(doc)
	doc = _bytes("v2")
	assert_true(h.commit_edit(doc), "A real change should record one step.")
	assert_true(h.can_undo())

	var restored = h.undo_swap(doc)
	assert_ne(restored, null, "Undo with a recorded step should return a snapshot.")
	assert_eq(restored, _bytes("v1"))
	assert_true(h.can_redo())

	var forward = h.redo_swap(restored)
	assert_eq(forward, _bytes("v2"))
	assert_false(h.can_redo())


func test_undo_swap_on_empty_returns_null_not_falsy() -> void:
	var h := NovaEditHistory.new()
	var live := PackedByteArray()  # empty bytes are falsy but NOT null
	var result = h.undo_swap(live)
	assert_eq(typeof(result), TYPE_NIL, "Empty history must return null, the reserved sentinel.")


func test_equal_gate_is_content_compare_for_dictionaries() -> void:
	var h := NovaEditHistory.new()
	var doc := {"value": 1, "items": [1, 2]}
	h.begin_edit(doc.duplicate(true))
	# A distinct Dictionary instance with equal content: the gate must suppress it.
	assert_false(h.commit_edit({"value": 1, "items": [1, 2]}))
	h.begin_edit(doc.duplicate(true))
	assert_true(h.commit_edit({"value": 2, "items": [1, 2]}))


func test_coalescing_second_begin_is_inert() -> void:
	var h := NovaEditHistory.new()
	var doc := _bytes("start")
	h.begin_edit(doc)
	h.begin_edit(_bytes("mid"))  # inert: session already open, pending stays "start"
	assert_true(h.commit_edit(_bytes("end")))
	var restored = h.undo_swap(_bytes("end"))
	assert_eq(restored, _bytes("start"), "Coalesced gesture should undo to the first begin().")


func test_fresh_commit_after_undo_clears_redo() -> void:
	var h := NovaEditHistory.new()
	var doc := _bytes("1")
	h.begin_edit(doc); doc = _bytes("2"); h.commit_edit(doc)
	doc = h.undo_swap(doc)
	assert_true(h.can_redo())
	h.begin_edit(doc); doc = _bytes("9"); h.commit_edit(doc)
	assert_false(h.can_redo(), "Branching off after undo should drop the redo tail.")


# --- Delta flavor -----------------------------------------------------------

func test_push_pop_round_trip() -> void:
	var h := NovaEditHistory.new()
	h.push_step({"op": "paint", "index": 1})
	h.push_step({"op": "paint", "index": 2})
	assert_eq(h.undo_depth(), 2)

	var step = h.pop_undo()
	assert_eq(step, {"op": "paint", "index": 2})
	step = h.pop_undo()
	assert_eq(step, {"op": "paint", "index": 1})
	assert_eq(typeof(h.pop_undo()), TYPE_NIL, "Drained stack must return the null sentinel.")

	# The same steps come back from the redo side, in order.
	assert_eq(h.pop_redo(), {"op": "paint", "index": 1})
	assert_eq(h.pop_redo(), {"op": "paint", "index": 2})
	assert_eq(typeof(h.pop_redo()), TYPE_NIL)


func test_push_after_pop_clears_redo() -> void:
	var h := NovaEditHistory.new()
	h.push_step({"op": "a"})
	h.pop_undo()
	assert_true(h.can_redo())
	h.push_step({"op": "b"})
	assert_false(h.can_redo())


func test_set_limit_caps_steps() -> void:
	var h := NovaEditHistory.new()
	h.set_limit(2)
	h.push_step({"n": 1})
	h.push_step({"n": 2})
	h.push_step({"n": 3})
	assert_eq(h.undo_depth(), 2, "Cap should drop the oldest step.")
	assert_eq(h.pop_undo(), {"n": 3})
	assert_eq(h.pop_undo(), {"n": 2})
	assert_eq(typeof(h.pop_undo()), TYPE_NIL)


# --- Dirty vs baseline ------------------------------------------------------

func test_is_dirty_tracks_baseline_by_value() -> void:
	var h := NovaEditHistory.new()
	var doc := _bytes("clean")
	h.mark_clean(doc)
	assert_false(h.is_dirty(doc))
	assert_true(h.is_dirty(_bytes("edited")))
	# Hand the value back: dirty clears even if the stack could not reach it.
	assert_false(h.is_dirty(_bytes("clean")))


func test_is_dirty_fallback_without_baseline() -> void:
	var h := NovaEditHistory.new()
	assert_false(h.is_dirty(_bytes("x")))
	assert_true(h.is_dirty(_bytes("x"), true), "Caller fallback should be honoured pre-baseline.")


func test_clear_drops_history_but_not_baseline() -> void:
	var h := NovaEditHistory.new()
	var doc := _bytes("base")
	h.mark_clean(doc)
	h.begin_edit(doc)
	h.commit_edit(_bytes("changed"))
	h.clear()
	assert_false(h.can_undo())
	assert_false(h.is_dirty(_bytes("base")), "clear() must not move the clean baseline.")
	assert_true(h.is_dirty(_bytes("changed")))
