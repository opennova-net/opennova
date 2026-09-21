extends GutTest

# The probe catalog's contract (ADR 0041): unique names, scripts under
# res://probes/ that load as GameProbe, schemas whose defaults validate.

const PROBES_ROOT := "res://probes"


func test_definitions_are_unique_and_live_under_probes() -> void:
	var seen := {}
	for def in ProbeDef.definitions():
		assert_false(seen.has(def.name), "duplicate probe name %s" % def.name)
		seen[def.name] = true
		assert_false(def.description.is_empty(), "%s has a description" % def.name)
		assert_true(def.script_path.begins_with(PROBES_ROOT + "/"),
				"%s lives under %s (source-only, never exported): %s" % [def.name, PROBES_ROOT, def.script_path])
		assert_eq(ProbeDef.definition(def.name), def)
	assert_null(ProbeDef.definition("no_such_probe"))


func test_every_available_probe_loads_and_its_defaults_validate() -> void:
	if ProbeDef.definitions().is_empty():
		pass_test("the catalog is empty in this build")
		return
	for def in ProbeDef.definitions():
		if not def.is_available():
			fail_test("%s is listed but its script %s is missing" % [def.name, def.script_path])
			continue
		var probe := def.load_probe()
		assert_not_null(probe, "%s loads as a GameProbe" % def.name)
		var required: Array = def.input_schema.get("required", [])
		if required.is_empty():
			var validated := def.validate_args({})
			assert_true(validated.ok, "%s validates with no args: %s" % [def.name, str(validated.errors)])


func test_runner_lists_the_catalog() -> void:
	var runner: ProbeRunner = add_child_autofree(ProbeRunner.new())
	var listing := runner.list()
	assert_eq((listing["probes"] as Array).size(), ProbeDef.definitions().size())
