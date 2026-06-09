extends RefCounted

# Navigation controller for the music workspace canvas: where the user is (the
# state map, or one state's blueprint), how they got there (the breadcrumb
# trail), and browser-style back/forward. Pure model -- it owns no Controls.
# live_mode is the sole listener of location_changed and does the canvas swap;
# every drill / back-to-map / breadcrumb click routes through here so the trail
# can never disagree with what's on screen.
#
# Entries are {"kind": "map"} or {"kind": "section", "name": String}. The
# history always starts at the map; the trail (everything up to the cursor) is
# the path the user took, not a containment hierarchy -- the state machine is
# flat, so "deeper" drilling is just more hops at the same level.
#
# No class_name (preload as a const), matching the other music modtools helpers.

signal location_changed(entry: Dictionary)

var _history: Array = [{"kind": "map"}]
var _cursor: int = 0


static func map_entry() -> Dictionary:
	return {"kind": "map"}


static func section_entry(name: String) -> Dictionary:
	return {"kind": "section", "name": name}


func current() -> Dictionary:
	return _history[_cursor]


func is_on_map() -> bool:
	return String(current().get("kind", "")) == "map"


func current_section() -> String:
	var e := current()
	return String(e.get("name", "")) if String(e.get("kind", "")) == "section" else ""


# The breadcrumb trail: every entry up to and including the cursor.
func trail() -> Array:
	return _history.slice(0, _cursor + 1)


func can_go_back() -> bool:
	return _cursor > 0


func can_go_forward() -> bool:
	return _cursor < _history.size() - 1


# Go somewhere. push=true records a new hop (and drops any forward history,
# browser-style); push=false replaces the current entry -- used by follow-live
# so a transitioning VM doesn't flood the trail with every state it enters.
# Navigating to the place we're already at just re-emits (an idempotent
# refresh), so callers never need a "same place?" guard.
func navigate_to(entry: Dictionary, push: bool = true) -> void:
	if _same(entry, current()):
		location_changed.emit(current())
		return
	# Never replace the map root out of the trail: a follow-live drill that starts
	# from the map records one hop, then keeps replacing that hop.
	if not push and is_on_map():
		push = true
	if push:
		_history.resize(_cursor + 1)
		_history.append(entry)
		_cursor += 1
	else:
		_history[_cursor] = entry
		_dedupe_consecutive()
	location_changed.emit(current())


func go_back() -> void:
	if can_go_back():
		_cursor -= 1
		location_changed.emit(current())


func go_forward() -> void:
	if can_go_forward():
		_cursor += 1
		location_changed.emit(current())


# A breadcrumb segment click: move the cursor onto that trail entry, keeping
# everything after it reachable via forward.
func jump_to(index: int) -> void:
	if index < 0 or index >= _history.size() or index == _cursor:
		return
	_cursor = index
	location_changed.emit(current())


# A state was renamed: rewrite every trail entry so back/forward and the
# breadcrumb keep pointing at it. Silent -- the caller refreshes its own UI.
func rename_section(old_name: String, new_name: String) -> void:
	for e in _history:
		if String(e.get("kind", "")) == "section" and String(e.get("name", "")) == old_name:
			e["name"] = new_name


# A state was deleted (or no longer exists): scrub it from the history and
# collapse any duplicate hops that exposes. If the user was standing on it,
# this lands them on the previous surviving entry and announces the move.
func remove_section(name: String) -> void:
	var before := current()
	var kept: Array = []
	var new_cursor := _cursor
	for i in range(_history.size()):
		var e: Dictionary = _history[i]
		if String(e.get("kind", "")) == "section" and String(e.get("name", "")) == name:
			if i <= _cursor:
				new_cursor -= 1
		else:
			kept.append(e)
	if kept.is_empty():
		kept = [map_entry()]
		new_cursor = 0
	_history = kept
	_cursor = clampi(new_cursor, 0, _history.size() - 1)
	_dedupe_consecutive()
	if not _same(current(), before):
		location_changed.emit(current())


# Forget everything (a different project was opened): back to just the map.
func reset() -> void:
	_history = [map_entry()]
	_cursor = 0
	location_changed.emit(current())


func _same(a: Dictionary, b: Dictionary) -> bool:
	return String(a.get("kind", "")) == String(b.get("kind", "")) \
		and String(a.get("name", "")) == String(b.get("name", ""))


# Collapse consecutive duplicates (a replace or removal can leave Map > A > A),
# keeping the cursor on the same logical entry.
func _dedupe_consecutive() -> void:
	var i := 1
	while i < _history.size():
		if _same(_history[i], _history[i - 1]):
			_history.remove_at(i)
			if i <= _cursor:
				_cursor -= 1
		else:
			i += 1
	_cursor = clampi(_cursor, 0, _history.size() - 1)
