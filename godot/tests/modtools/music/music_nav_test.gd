extends GutTest

# MusicNavController: the pure navigation model behind the music workspace
# breadcrumb (trail, back/forward, rename/remove healing). No Controls here --
# live_mode owns the widgets; these tests pin the history semantics.

const MusicNav = preload("res://modtools/music/ui/music_nav.gd")

var _events: Array = []


func _make_nav():
	var nav = MusicNav.new()
	_events = []
	nav.location_changed.connect(func(e: Dictionary): _events.append(e))
	return nav


func _names(trail: Array) -> Array:
	var out := []
	for e in trail:
		out.append("Map" if String(e.get("kind", "")) == "map" else String(e.get("name", "")))
	return out


func test_starts_on_map_with_no_history():
	var nav = _make_nav()
	assert_true(nav.is_on_map(), "fresh nav sits on the map")
	assert_eq(nav.current_section(), "", "no section open")
	assert_false(nav.can_go_back(), "nothing to go back to")
	assert_false(nav.can_go_forward(), "nothing to go forward to")
	assert_eq(_names(nav.trail()), ["Map"], "trail is just the map")


func test_push_navigation_grows_the_trail():
	var nav = _make_nav()
	nav.navigate_to(MusicNav.section_entry("Begin"))
	nav.navigate_to(MusicNav.section_entry("Battle"))
	assert_eq(_names(nav.trail()), ["Map", "Begin", "Battle"], "each hop is recorded")
	assert_eq(nav.current_section(), "Battle")
	assert_true(nav.can_go_back())
	assert_false(nav.can_go_forward())
	assert_eq(_events.size(), 2, "each navigation announces itself")


func test_back_and_forward_walk_the_history():
	var nav = _make_nav()
	nav.navigate_to(MusicNav.section_entry("Begin"))
	nav.navigate_to(MusicNav.section_entry("Battle"))
	nav.go_back()
	assert_eq(nav.current_section(), "Begin", "back lands on the previous hop")
	assert_true(nav.can_go_forward(), "forward is now available")
	nav.go_back()
	assert_true(nav.is_on_map(), "back again lands on the map")
	assert_false(nav.can_go_back())
	nav.go_forward()
	assert_eq(nav.current_section(), "Begin", "forward retraces the trail")
	nav.go_forward()
	assert_eq(nav.current_section(), "Battle")
	assert_false(nav.can_go_forward())


func test_push_after_back_drops_forward_history():
	var nav = _make_nav()
	nav.navigate_to(MusicNav.section_entry("Begin"))
	nav.navigate_to(MusicNav.section_entry("Battle"))
	nav.go_back()
	nav.navigate_to(MusicNav.section_entry("Victory"))
	assert_eq(_names(nav.trail()), ["Map", "Begin", "Victory"], "branching rewrites the future")
	assert_false(nav.can_go_forward(), "old forward history is gone")


func test_replace_does_not_grow_the_trail():
	# Follow-live uses replace so a transitioning VM doesn't flood the history.
	var nav = _make_nav()
	nav.navigate_to(MusicNav.section_entry("Begin"))
	nav.navigate_to(MusicNav.section_entry("Battle"), false)
	nav.navigate_to(MusicNav.section_entry("Victory"), false)
	assert_eq(_names(nav.trail()), ["Map", "Victory"], "replace swaps the current hop in place")
	nav.go_back()
	assert_true(nav.is_on_map(), "back skips the replaced-away states")


func test_navigate_to_current_location_reemits_without_growing():
	var nav = _make_nav()
	nav.navigate_to(MusicNav.section_entry("Begin"))
	var before := _events.size()
	nav.navigate_to(MusicNav.section_entry("Begin"))
	assert_eq(_names(nav.trail()), ["Map", "Begin"], "no duplicate hop")
	assert_eq(_events.size(), before + 1, "still announces (an idempotent refresh)")


func test_jump_to_breadcrumb_segment_preserves_forward():
	var nav = _make_nav()
	nav.navigate_to(MusicNav.section_entry("Begin"))
	nav.navigate_to(MusicNav.section_entry("Battle"))
	nav.navigate_to(MusicNav.section_entry("Victory"))
	nav.jump_to(1)
	assert_eq(nav.current_section(), "Begin", "segment click jumps onto that hop")
	assert_eq(_names(nav.trail()), ["Map", "Begin"], "trail shows the path up to the cursor")
	assert_true(nav.can_go_forward(), "the later hops stay reachable via forward")
	nav.go_forward()
	nav.go_forward()
	assert_eq(nav.current_section(), "Victory")


func test_rename_section_rewrites_every_hop():
	var nav = _make_nav()
	nav.navigate_to(MusicNav.section_entry("Begin"))
	nav.navigate_to(MusicNav.section_entry("Battle"))
	nav.navigate_to(MusicNav.section_entry("Begin"))
	nav.rename_section("Begin", "Intro")
	assert_eq(_names(nav.trail()), ["Map", "Intro", "Battle", "Intro"], "every hop renamed")
	assert_eq(nav.current_section(), "Intro")


func test_remove_section_scrubs_history_and_lands_on_survivor():
	var nav = _make_nav()
	nav.navigate_to(MusicNav.section_entry("Begin"))
	nav.navigate_to(MusicNav.section_entry("Doomed"))
	var before := _events.size()
	nav.remove_section("Doomed")
	assert_eq(nav.current_section(), "Begin", "standing on the removed state lands on the previous hop")
	assert_eq(_names(nav.trail()), ["Map", "Begin"], "removed state scrubbed from the trail")
	assert_eq(_events.size(), before + 1, "the forced move is announced")


func test_remove_section_collapses_duplicate_hops():
	var nav = _make_nav()
	nav.navigate_to(MusicNav.section_entry("Begin"))
	nav.navigate_to(MusicNav.section_entry("Doomed"))
	nav.navigate_to(MusicNav.section_entry("Begin"))
	nav.remove_section("Doomed")
	assert_eq(_names(nav.trail()), ["Map", "Begin"], "Begin > Doomed > Begin collapses to one Begin")
	assert_eq(nav.current_section(), "Begin", "still standing on Begin")


func test_remove_section_not_current_is_silent():
	var nav = _make_nav()
	nav.navigate_to(MusicNav.section_entry("Doomed"))
	nav.navigate_to(MusicNav.section_entry("Begin"))
	var before := _events.size()
	nav.remove_section("Doomed")
	assert_eq(nav.current_section(), "Begin", "current location untouched")
	assert_eq(_names(nav.trail()), ["Map", "Begin"], "dead hop scrubbed behind the scenes")
	assert_eq(_events.size(), before, "no announcement when the location didn't change")


func test_reset_returns_to_a_bare_map():
	var nav = _make_nav()
	nav.navigate_to(MusicNav.section_entry("Begin"))
	nav.navigate_to(MusicNav.section_entry("Battle"))
	nav.reset()
	assert_true(nav.is_on_map())
	assert_eq(_names(nav.trail()), ["Map"])
	assert_false(nav.can_go_back())
	assert_false(nav.can_go_forward())
