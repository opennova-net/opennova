extends GutTest

# Covers NovaMusicScript.get_section_model: the read-only structural model that
# drives the editor's section map. The model reads opcodes (libs/mus
# mus_build_section_model), so the topology is exact -- setstate transitions are
# distinguished from frame-setup `enter`, and tablexec switch fan-out is captured
# (the old string-parsed graph saw none of this).

const FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"


func _model() -> Array:
	var ms := load(FIXTURE) as NovaMusicScript
	assert_not_null(ms, "fixture loads as NovaMusicScript")
	if ms == null:
		return []
	return ms.get_section_model(StringName(ms.get_default_script_name()))


func _by_name(model: Array, name: String) -> Dictionary:
	for d in model:
		if String(d.get("name", "")) == name:
			return d
	return {}


func _edge_names(section: Dictionary) -> Array:
	var out: Array = []
	for e in section.get("edges", []):
		out.append(String(e.get("to_name", "")))
	return out


func test_model_has_all_sections() -> void:
	var model := _model()
	assert_eq(model.size(), 8, "gamescript has 8 sections")
	if model.is_empty():
		return
	var first: Dictionary = model[0]
	for key in ["name", "index", "is_entry", "is_idle_loop", "edges", "plays"]:
		assert_true(first.has(key), "section dict has key '%s'" % key)


func test_exactly_one_entry() -> void:
	var model := _model()
	var entries := 0
	for d in model:
		if bool(d.get("is_entry", false)):
			entries += 1
	assert_eq(entries, 1, "exactly one entry section")


func test_missionnull_is_idle_loop() -> void:
	var mn := _by_name(_model(), "Missionnull")
	assert_false(mn.is_empty(), "Missionnull present")
	if mn.is_empty():
		return
	assert_true(bool(mn.get("is_idle_loop", false)), "Missionnull flagged idle-loop")
	assert_eq(_edge_names(mn), ["Missionnull"], "Missionnull self-loops")


func test_testmission_branches_two_ways() -> void:
	var tm := _by_name(_model(), "Testmission")
	assert_false(tm.is_empty(), "Testmission present")
	if tm.is_empty():
		return
	var names := _edge_names(tm)
	assert_true(names.has("Missionnull"), "Testmission -> Missionnull")
	assert_true(names.has("Multiplayerstart"), "Testmission -> Multiplayerstart")
	# Both are real transitions (kind 0), not switch/branch.
	for e in tm.get("edges", []):
		assert_eq(int(e.get("kind", -1)), 0, "Testmission edges are transitions")


func test_win000_play_list() -> void:
	var w := _by_name(_model(), "Win000")
	assert_false(w.is_empty(), "Win000 present")
	if w.is_empty():
		return
	var plays: Array = w.get("plays", [])
	assert_eq(plays.size(), 6, "Win000 plays 6 tracks")
	if plays.size() == 6:
		assert_eq(int(plays[0].get("track", -1)), 2, "first play is track 2")
		assert_eq(int(plays[5].get("track", -1)), 7, "last play is track 7")


func test_begin_has_switch_fanout() -> void:
	var b := _by_name(_model(), "Begin")
	assert_false(b.is_empty(), "Begin present")
	if b.is_empty():
		return
	var has_switch := false
	for e in b.get("edges", []):
		if int(e.get("kind", -1)) == 1:
			has_switch = true
	assert_true(has_switch, "Begin has a switch (tablexec) edge")
	var names := _edge_names(b)
	for target in ["Missionnull", "Missionwin", "Missionlose"]:
		assert_true(names.has(target), "Begin switch reaches %s" % target)
