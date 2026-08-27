extends GutTest

# The probe catalog's contract (ADR 0041): unique names, scripts under
# res://probes/ that load as GameProbe, schemas whose defaults validate, and
# probe sources that never print, never read the environment, and never
# reach into the test suite.

const PROBES_ROOT := "res://probes"
const FORBIDDEN_SOURCE := {
	"print(": "probes log through ctx.log, never print()",
	"OS.get_environment(": "probes take typed args, never environment variables",
	"OS.has_environment(": "probes take typed args, never environment variables",
	"res://tests/": "probes must not depend on the GUT suite",
}


func test_definitions_are_unique_and_live_under_probes() -> void:
	var seen := {}
	for def in ProbeCatalog.definitions():
		assert_false(seen.has(def.name), "duplicate probe name %s" % def.name)
		seen[def.name] = true
		assert_false(def.description.is_empty(), "%s has a description" % def.name)
		assert_true(def.script_path.begins_with(PROBES_ROOT + "/"),
				"%s lives under %s (source-only, never exported): %s" % [def.name, PROBES_ROOT, def.script_path])
		assert_eq(ProbeCatalog.definition(def.name), def)
	assert_null(ProbeCatalog.definition("no_such_probe"))


func test_every_available_probe_loads_and_its_defaults_validate() -> void:
	if ProbeCatalog.definitions().is_empty():
		pass_test("the catalog is empty in this build")
		return
	for def in ProbeCatalog.definitions():
		if not ProbeCatalog.is_available(def):
			fail_test("%s is listed but its script %s is missing" % [def.name, def.script_path])
			continue
		var probe := ProbeCatalog.load_probe(def)
		assert_not_null(probe, "%s loads as a GameProbe" % def.name)
		var required: Array = def.input_schema.get("required", [])
		if required.is_empty():
			var validated := ProbeSchema.validate(def.input_schema, {})
			assert_true(validated.ok, "%s validates with no args: %s" % [def.name, str(validated.errors)])


func test_probe_sources_keep_the_contract() -> void:
	var offenders := PackedStringArray()
	for path in _gd_files(PROBES_ROOT):
		var text := FileAccess.get_file_as_string(path)
		for line in text.split("\n"):
			var code := line.split("#", 1)[0]
			for needle in FORBIDDEN_SOURCE:
				if code.contains(needle):
					offenders.append("%s: %s (%s)" % [path, line.strip_edges(), FORBIDDEN_SOURCE[needle]])
	assert_eq(offenders.size(), 0, "\n".join(offenders))


func test_runner_lists_the_catalog() -> void:
	var runner: ProbeRunner = add_child_autofree(ProbeRunner.new())
	var listing := runner.list()
	assert_eq((listing["probes"] as Array).size(), ProbeCatalog.definitions().size())


static func _gd_files(root: String) -> PackedStringArray:
	var out := PackedStringArray()
	var dir := DirAccess.open(root)
	if dir == null:
		return out
	dir.list_dir_begin()
	var entry := dir.get_next()
	while entry != "":
		var path := root.path_join(entry)
		if dir.current_is_dir():
			out.append_array(_gd_files(path))
		elif entry.ends_with(".gd"):
			out.append(path)
		entry = dir.get_next()
	dir.list_dir_end()
	return out
