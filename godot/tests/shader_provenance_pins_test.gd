extends GutTest

## The provenance and technique-audit pins over godot/shaders: what
## tests/test_shader_resources.py pinned before the Python suite was retired
## (ADR 0038), minus the legs that decoded the retail SCR-wrapped .fx corpus
## (third_party/modsuperoed, removed with it). The audit JSONs still record
## that corpus by commit; the checks here hold them to their own arithmetic,
## to the pipeline manifest, to the shader math and to the runtime sources.
## shader_resource_contract_test.gd carries the structural graph checks.

const SHADER_ROOT := "res://shaders"
const OBJECT_ROOT := "res://shaders/object"
const PROVENANCE_PATH := "res://shaders/provenance.json"
const MANIFEST_PATH := "res://shaders/object/pipeline_manifest.json"
const TECHNIQUE_VALIDATION_PATH := "res://shaders/object/technique_validation.json"
const AUXILIARY_TECHNIQUE_VALIDATION_PATH := "res://shaders/object/auxiliary_technique_validation.json"
const RETAIL_EFFECT_INVENTORY_PATH := "res://shaders/object/retail_effect_inventory.json"
const PASS_CLASSES := [
	"TECHNIQUE_NORMAL", "TECHNIQUE_PROJSHAD", "TECHNIQUE_DEPTHMASK",
	"TECHNIQUE_CLIP", "TECHNIQUE_GLOW", "TECHNIQUE_MATCHTERRAIN",
]

var _include_re := RegEx.new()
var _address_re := RegEx.new()
var _commit_re := RegEx.new()


func before_all() -> void:
	_include_re.compile("(?m)^\\s*#include\\s+\"res://([^\"]+)\"")
	_address_re.compile("^0x[0-9a-fA-F]+$")
	_commit_re.compile("^[0-9a-f]{40}$")


# --- helpers -------------------------------------------------------------


func _repo_path(relative: String) -> String:
	return ProjectSettings.globalize_path("res://../" + relative)


func _read(path: String) -> String:
	assert_true(FileAccess.file_exists(path), "readable: %s" % path)
	return FileAccess.get_file_as_string(path)


func _read_repo(relative: String) -> String:
	return _read(_repo_path(relative))


func _load_json(path: String) -> Variant:
	var parsed = JSON.parse_string(_read(path))
	assert_not_null(parsed, "%s parses as JSON" % path)
	return parsed


func _collect(directory: String, suffixes: PackedStringArray, out: Array) -> void:
	var access := DirAccess.open(directory)
	assert_not_null(access, "directory must be readable: %s" % directory)
	if access == null:
		return
	access.list_dir_begin()
	var entry := access.get_next()
	while not entry.is_empty():
		if access.current_is_dir():
			_collect(directory.path_join(entry), suffixes, out)
		else:
			for suffix in suffixes:
				if entry.ends_with(suffix):
					out.append(directory.path_join(entry))
					break
		entry = access.get_next()
	access.list_dir_end()


func _shader_sources() -> Dictionary:
	var paths: Array = []
	_collect(SHADER_ROOT, PackedStringArray([".gdshader", ".gdshaderinc"]), paths)
	var out := {}
	for path in paths:
		out[String(path).trim_prefix(SHADER_ROOT + "/")] = _read(path)
	return out


func _include_closure_text(wrapper: String) -> String:
	var visited := {}
	var pending: Array = [wrapper]
	var parts := PackedStringArray()
	while not pending.is_empty():
		var path: String = pending.pop_back()
		if visited.has(path):
			continue
		visited[path] = true
		assert_true(FileAccess.file_exists(path), "missing shader include %s" % path)
		if not FileAccess.file_exists(path):
			continue
		var source := _read(path)
		parts.append(source)
		for m in _include_re.search_all(source):
			pending.append("res://" + m.get_string(1))
	return "\n".join(parts)


func _sorted(values: Array) -> Array:
	var out := values.duplicate()
	out.sort()
	return out


func _sorted_keys(dict: Dictionary) -> Array:
	return _sorted(dict.keys())


func _contains_all(haystack: String, tokens: Array, label: String) -> void:
	for token in tokens:
		assert_true(haystack.contains(String(token)), "%s carries %s" % [label, token])


func _contains_none(haystack: String, tokens: Array, label: String) -> void:
	for token in tokens:
		assert_false(haystack.contains(String(token)), "%s must not carry %s" % [label, token])


func _contract(provenance: Dictionary, id: String) -> Dictionary:
	for row in provenance["contracts"]:
		if row["id"] == id:
			return row
	assert_true(false, "provenance contract %s exists" % id)
	return {}


func _citation_addresses(contract: Dictionary) -> Dictionary:
	var out := {}
	for row in contract.get("retail_citations", []):
		out[String(row["address"]).to_lower()] = true
	return out


func _technique_enums(pipeline: Dictionary) -> Array:
	var out := {}
	for entry in pipeline["techniques"]:
		out[entry["engine_enum"]] = true
	return _sorted_keys(out)


# --- provenance scope ---------------------------------------------------


func test_validation_scope_is_the_locked_highest_quality_retail_profile() -> void:
	var provenance: Dictionary = _load_json(PROVENANCE_PATH)
	var active_exceptions := {}
	for contract in provenance["contracts"]:
		for exception in contract["exceptions"]:
			active_exceptions[exception] = true
	for closed in ["D-RMAT-8", "D-RORD-5", "D-RLIT-4", "D-RLIT-6"]:
		assert_false(active_exceptions.has(closed), "%s is no longer an active exception" % closed)
	var scope: Dictionary = provenance["quality_scope"]
	assert_eq(String(scope["profile"]), "highest-quality-retail-path")
	assert_eq(int(scope["shader_usage_level"]), 2)
	assert_true(bool(scope["screenshots_allowed_only_after_validation"]))
	assert_eq(String(scope["fixture_catalog"]), "docs/render/render-fixtures-retail-v5.json")
	assert_eq(String(scope["object_pass_class_validation"]),
			"godot/shaders/object/auxiliary_technique_validation.json")

	var fixture: Dictionary = _load_json(_repo_path(String(scope["fixture_catalog"])))
	var retail_profile: Dictionary = fixture["retail_video_profile_contract"]
	assert_eq(String(retail_profile["id"]), String(scope["fixture_contract"]))
	assert_eq(int(retail_profile["required_values"]["shader_usage_level"]), 2)

	# The pinned values are the engine's (runtime/menu/options_policy.h);
	# the shell script only selects and locks the authored widgets.
	var policy := _read_repo(String(scope["locked_menu_policy"]))
	_contains_all(policy, [
		"{\"SHADERUSAGE\", \"2\"}",
		"{\"TERRAINPOLY\", \"3\"}",
		"{\"OBJECTTEX\", \"3\"}",
		"{\"SHADOWQUALITY\", \"3\"}",
	], "the locked menu policy")
	var shell := _read_repo(String(scope["locked_menu_shell"]))
	_contains_all(shell, [
		"MenuFrame.video_quality_controls()",
		"driver.set_widget_disabled(id, true)",
	], "the locked menu shell")


func test_provenance_contracts_are_reviewable_and_citations_resolve_to_docs() -> void:
	var provenance: Dictionary = _load_json(PROVENANCE_PATH)
	var allowed: Array = provenance["parity_statuses"]
	for contract in provenance["contracts"]:
		var id := String(contract["id"])
		assert_true(allowed.has(contract["parity_status"]), id)
		assert_false(String(contract["light_behavior"]).is_empty(), id)
		assert_false((contract["docs"] as Array).is_empty(), id)
		assert_false((contract["retail_citations"] as Array).is_empty(), id)
		if contract["parity_status"] != "matching":
			assert_false((contract.get("exceptions", []) as Array).is_empty(),
					"%s must name every bounded parity exception" % id)

		var documentation := ""
		for doc_name in contract["docs"]:
			var doc_path := _repo_path(String(doc_name))
			assert_true(FileAccess.file_exists(doc_path), "missing provenance document %s" % doc_name)
			if FileAccess.file_exists(doc_path):
				documentation += FileAccess.get_file_as_string(doc_path).to_lower()

		for citation in contract["retail_citations"]:
			var address := String(citation["address"]).to_lower()
			var start := String(citation["function_start"]).to_lower()
			assert_not_null(_address_re.search(address), "%s address %s" % [id, address])
			assert_not_null(_address_re.search(start), "%s function_start %s" % [id, start])
			assert_false(String(citation["function"]).is_empty(), id)
			assert_true(documentation.contains(address),
					"%s citation %s is absent from its RE documents" % [id, address])


# --- environment capture ------------------------------------------------


func test_environment_cube_capture_is_live_and_highest_quality() -> void:
	var shared := _read(OBJECT_ROOT.path_join("shared.gdshaderinc"))
	var engine_header := _read_repo("engine/runtime/renderer/environment_cube.h")
	var capture_header := _read_repo("godot/src/env/environment_cube_capture.h")
	var capture_source := _read_repo("godot/src/env/environment_cube_capture.cpp")
	var blit_source := _read_repo("godot/src/render/environment_cube_blit.cpp")
	var water := _read(SHADER_ROOT.path_join("water.gdshader"))
	var project := _read("res://project.godot")

	# The witnessed facts live in the engine header (ADR 0042); the capture
	# node consumes them and never re-derives a number.
	_contains_all(engine_header, [
		"kEnvironmentCubeFaceSize = 256", "kEnvironmentCubeRefreshFrames = 128",
		"kEnvironmentCubeDimByte = 0x60", "kEnvironmentCubeFaceFovDegrees = 90.0f",
		"kEnvironmentCubeFaceNear = 0.5f", "kEnvironmentCubeFaceFar = 1000.0f",
		"kEnvironmentCubeEyeRaise = 1.0f", "kEnvironmentCubeTerrainClearance = 10.0f",
		"update_environment_cubemap @ 0x6106a0", "GTexture_RenderCubeMapFace @ 0x6864d0",
	], "the engine environment-cube header")
	_contains_all(capture_header, [
		"kCaptureSize =", "opennova::renderer::kEnvironmentCubeFaceSize",
		"kRefreshFrames =", "opennova::renderer::kEnvironmentCubeRefreshFrames",
		"kSkyDimByte =", "opennova::renderer::kEnvironmentCubeDimByte",
	], "the capture header")
	_contains_all(capture_source, [
		"camera->set_fov(opennova::renderer::kEnvironmentCubeFaceFovDegrees)",
		"camera->set_near(opennova::renderer::kEnvironmentCubeFaceNear)",
		"camera->set_far(opennova::renderer::kEnvironmentCubeFaceFar)",
		"opennova::renderer::environment_cube_eye_height(",
		"opennova::renderer::environment_cube_refresh_due(",
		"Water::VISUAL_LAYER_ENVIRONMENT_CAPTURE", "camera->set_compositor(capture_compositor)",
		"call_on_render_thread", "set_texture_rd_rid(cube)",
	], "the capture source")
	# No CPU readback, Image or Cubemap resource: the faces reach the
	# published RD cubemap through the RenderingDevice copy leg, which
	# applies the engine's dim-byte product in integer shader math.
	assert_false(capture_source.contains("get_image("))
	assert_false(capture_source.contains("linear_to_srgb"))
	_contains_all(blit_source, [
		"RenderingDevice::TEXTURE_TYPE_CUBE", "DATA_FORMAT_R8G8B8A8_UNORM",
		"(bytes * pc.dim_byte + 127u) / 255u",
	], "the environment cube blit")
	_contains_all(shared, [
		"global uniform samplerCube opennova_environment_cube",
		"texture(opennova_environment_cube, direction)",
		"pow(aligned, 800.0)", "pow(aligned, 40.0)", "static_lobe * opennova_sun_light * 2.0",
	], "object/shared.gdshaderinc")
	# The six capture cameras cull to the aliased water layer ALONE, so water
	# rejects them by exact camera mask, never by a
	# stale capture-origin distance.
	assert_true(water.contains("NOVA_ENVIRONMENT_CAPTURE_CAMERA_MASK = 1024u"))
	assert_true(water.contains("CAMERA_VISIBLE_LAYERS == NOVA_ENVIRONMENT_CAPTURE_CAMERA_MASK"))
	assert_false(water.contains("opennova_environment_capture_origin"))
	assert_true(project.contains("\"type\": \"samplerCube\""))


# --- retail effect audit (JSON invariants; the .fx decode legs are gone) --


func test_retail_effect_inventory_is_internally_consistent() -> void:
	var inventory: Dictionary = _load_json(RETAIL_EFFECT_INVENTORY_PATH)
	assert_eq(int(inventory["schema"]), 1)
	assert_eq(int(inventory["source_file_count"]), 44)
	assert_eq((inventory["source_files"] as Array).size(), 44)
	var classes: Dictionary = inventory["classes"]
	var expected_classes := PASS_CLASSES.duplicate()
	expected_classes.append("UNSPECIFIED")
	assert_eq(_sorted_keys(classes), _sorted(expected_classes))

	var declaration_count := 0
	var typed_count := 0
	var pass_count := 0
	for pass_class in classes:
		var entry: Dictionary = classes[pass_class]
		var evidence_declarations := 0
		for file_name in entry["evidence"]:
			evidence_declarations += (entry["evidence"][file_name] as Array).size()
		assert_eq(int(entry["declaration_count"]), evidence_declarations,
				"%s declaration_count matches its evidence" % pass_class)
		declaration_count += int(entry["declaration_count"])
		if pass_class != "UNSPECIFIED":
			typed_count += int(entry["declaration_count"])
		pass_count += int(entry["pass_declaration_count"])
		assert_false(String(entry["disposition"]).is_empty(), pass_class)
		assert_false(String(entry["disposition"]).contains("deferred"), pass_class)
	assert_eq(declaration_count, 59)
	assert_eq(int(inventory["technique_declaration_count"]), 59)
	assert_eq(typed_count, 57)
	assert_eq(int(inventory["typed_technique_declaration_count"]), 57)
	assert_eq(pass_count, 138)
	assert_eq(int(inventory["pass_declaration_count"]), 138)

	var normal: Dictionary = classes["TECHNIQUE_NORMAL"]
	var evidence_declarations := {}
	for file_name in normal["evidence"]:
		var names: Array = normal["evidence"][file_name]
		for ordinal in range(names.size()):
			evidence_declarations["%s|%d|%s" % [file_name, ordinal, names[ordinal]]] = true
	var selected := {}
	for entry in normal["highest_quality_selected"]:
		selected["%s|%d|%s" % [entry["file"], int(entry["ordinal"]), entry["name"]]] = true
	var excluded := {}
	for entry in normal["excluded_declarations"]:
		excluded["%s|%d|%s" % [entry["file"], int(entry["ordinal"]), entry["name"]]] = true
	assert_eq(selected.size(), 19)
	assert_eq(excluded.size(), 11)
	for key in selected:
		assert_false(excluded.has(key), "selected and excluded NORMAL declarations are disjoint")
	var partition := selected.duplicate()
	partition.merge(excluded)
	assert_eq(_sorted_keys(partition), _sorted_keys(evidence_declarations),
			"selected + excluded NORMAL declarations are exactly the audited evidence")
	var leaves_not_registered := false
	for entry in normal["excluded_declarations"]:
		if entry["file"] == "leaves.FX" and String(entry["reason"]).contains("not_registered"):
			leaves_not_registered = true
	assert_true(leaves_not_registered, "leaves.FX is excluded as not registered")

	var pipeline: Dictionary = _load_json(MANIFEST_PATH)
	var projected := {}
	for entry in normal["highest_quality_selected"]:
		for technique in entry["runtime_techniques"]:
			projected[technique] = true
	for entry in normal["external_runtime_techniques"]:
		projected[entry["runtime_technique"]] = true
	assert_eq(_sorted_keys(projected), _technique_enums(pipeline),
			"the selected NORMAL declarations project onto exactly the runtime techniques")


func test_projected_shadow_state_contracts_cover_every_technique() -> void:
	var pipeline: Dictionary = _load_json(MANIFEST_PATH)
	var techniques := _technique_enums(pipeline)
	var states: Dictionary = pipeline["projected_shadow_state_contracts"]
	assert_eq(_sorted_keys(states), techniques)
	var material_blend := {}
	var no_pass := {}
	var opaque := 0
	for name in states:
		match String(states[name]):
			"material_blend":
				material_blend[name] = true
			"no_pass":
				no_pass[name] = true
			"opaque":
				opaque += 1
	assert_eq(_sorted_keys(material_blend), ["Fixed", "FixedDetail", "SelfLit", "SelfLitDetail"])
	assert_eq(_sorted_keys(no_pass), ["Flag", "GlassFixed", "GlassSkinned", "Tracer"])
	assert_eq(opaque, 16)
	# The decoded-pass half (sixteen PROJSHAD bodies, the _FFP RSAlphaMode
	# quartet, the fifteen hard-opaque files, the skinned post/base pins) needed
	# the retail corpus; the material record keeps the one textual witness.
	var material_re := _read_repo("docs/render/render-material-re.md")
	assert_true(material_re.contains("Tracer.fx exposes only TECHNIQUE_NORMAL"))


func test_every_reachable_object_technique_is_audited_and_its_wrapper_keeps_the_contract() -> void:
	var provenance: Dictionary = _load_json(PROVENANCE_PATH)
	var validation: Dictionary = _load_json(_repo_path(
			String(provenance["quality_scope"]["object_technique_validation"])))
	var pipeline: Dictionary = _load_json(MANIFEST_PATH)

	assert_eq(int(validation["schema"]), 2)
	assert_eq(int(validation["quality_scope"]["shader_usage_level"]), 2)
	assert_eq(String(validation["quality_scope"]["retail_pass_class"]), "TECHNIQUE_NORMAL")
	assert_eq(String(validation["quality_scope"]["retail_effect_inventory"]),
			"godot/shaders/object/retail_effect_inventory.json")
	assert_eq(_sorted(validation["quality_scope"]["audited_pass_classes"]), _sorted(PASS_CLASSES))
	assert_eq(String(validation["quality_scope"]["pass_class_validation"]),
			"godot/shaders/object/auxiliary_technique_validation.json")
	assert_not_null(_commit_re.search(String(validation["source_corpus"]["commit"])),
			"the audit names its corpus commit")

	var audited := {}
	for entry in validation["techniques"]:
		audited[entry["engine_enum"]] = entry
	var selected := {}
	for entry in pipeline["techniques"]:
		selected[entry["engine_enum"]] = entry
	assert_eq(_sorted_keys(audited), _sorted_keys(selected))
	assert_eq(audited.size(), (validation["techniques"] as Array).size(), "techniques audit once")

	for engine_enum in audited:
		var entry: Dictionary = audited[engine_enum]
		var selected_entry: Dictionary = selected[engine_enum]
		assert_eq(entry["implementation"], selected_entry["implementation"], engine_enum)
		assert_eq(entry["policies"], selected_entry["policies"], engine_enum)
		assert_true(["none", "self_lum"].has(selected_entry["rgb_modulation"]), engine_enum)
		assert_true(["none", "ffp"].has(selected_entry["alpha_modulation"]), engine_enum)
		assert_true(["diffuse_alpha", "normal_alpha", "vertex_diffuse_alpha", "reflect_alpha", "zero"]
				.has(selected_entry["coverage_source"]), engine_enum)
		assert_true(["explicit", "explicit_unskinned_or_submit_skip_skinned", "normal_fallback",
				"skinned_submit_skip"].has(selected_entry["clip_class"]), engine_enum)
		for required_field in ["channels", "light_response", "state", "shader_tokens"]:
			var value = entry.get(required_field)
			var present := value != null and not (value is Array and (value as Array).is_empty()) \
					and not (value is Dictionary and (value as Dictionary).is_empty()) \
					and not (value is String and (value as String).is_empty())
			assert_true(present, "%s must explicitly audit %s" % [engine_enum, required_field])
		assert_eq(_sorted_keys(entry["responses"]), ["ambient", "directional", "hemisphere", "point"], engine_enum)
		for key in entry["responses"]:
			assert_true(entry["responses"][key] is bool, "%s response %s is a bool" % [engine_enum, key])
		var raster_checks: Dictionary = entry.get("raster_checks", {})
		for key in raster_checks:
			assert_true(["directional_axis_reversal", "hemisphere_axis_reversal"].has(key), engine_enum)
		var any_false := false
		for key in raster_checks:
			if raster_checks[key] == false:
				any_false = true
		if any_false:
			assert_false(String(entry.get("raster_check_note", "")).is_empty(), engine_enum)
		assert_true((validation["parity_statuses"] as Array).has(entry["parity_status"]), engine_enum)
		if entry["parity_status"] != "matching":
			assert_false((entry.get("exceptions", []) as Array).is_empty(),
					"%s must bound every residual" % engine_enum)

		var evidence: Dictionary = entry["evidence"]
		if evidence["kind"] == "scr_fx":
			# The decoded-symbol check needed the retail corpus; the audit still
			# has to name its files and symbols.
			assert_false((evidence["sources"] as Array).is_empty(), engine_enum)
			for source in evidence["sources"]:
				assert_false(String(source["file"]).is_empty(), engine_enum)
				assert_false((source["symbols"] as Array).is_empty(), engine_enum)
		else:
			assert_eq(String(evidence["kind"]), "binary_documented", engine_enum)
			var documentation := ""
			for name in evidence["docs"]:
				documentation += _read_repo(String(name)).to_lower() + "\n"
			for symbol in evidence["symbols"]:
				assert_true(documentation.contains(String(symbol).to_lower()),
						"%s: %s documented" % [engine_enum, symbol])

		for citation in evidence.get("binary_citations", []):
			assert_not_null(_address_re.search(String(citation["address"])), engine_enum)
			assert_false(String(citation["function"]).is_empty(), engine_enum)
			var cited_docs := ""
			for name in citation["docs"]:
				cited_docs += _read_repo(String(name)).to_lower() + "\n"
			assert_true(cited_docs.contains(String(citation["address"]).to_lower()), engine_enum)
			assert_true(cited_docs.contains(String(citation["function"]).to_lower()), engine_enum)

		var representative := OBJECT_ROOT.path_join(String(selected_entry["directory"])).path_join(
				"%s.gdshader" % entry["policies"][0])
		var closure := _include_closure_text(representative)
		var representative_source := _read(representative)
		for pair in [["rgb_modulation", "OBJ_RGB_MOD"], ["alpha_modulation", "OBJ_ALPHA_MOD"],
				["coverage_source", "OBJ_COVERAGE"], ["clip_class", "OBJ_CLIP"]]:
			var expected_macro := "#define %s_%s" % [pair[1], String(selected_entry[pair[0]]).to_upper()]
			assert_true(representative_source.contains(expected_macro),
					"%s: wrapper lost %s contract %s" % [engine_enum, pair[0], expected_macro])
		for token in entry["shader_tokens"]:
			assert_true(closure.contains(String(token)), "%s: shader math lost %s" % [engine_enum, token])

		var auxiliary_resources: Array = entry.get("auxiliary_resources", [])
		var auxiliary_tokens: Array = entry.get("auxiliary_shader_tokens", [])
		if not auxiliary_resources.is_empty() or not auxiliary_tokens.is_empty():
			assert_true(not auxiliary_resources.is_empty() and not auxiliary_tokens.is_empty(), engine_enum)
			var auxiliary_closure := ""
			for resource in auxiliary_resources:
				auxiliary_closure += _include_closure_text(SHADER_ROOT.path_join(String(resource))) + "\n"
			for token in auxiliary_tokens:
				assert_true(auxiliary_closure.contains(String(token)),
						"%s: auxiliary shader math lost %s" % [engine_enum, token])

		var implementation := _read(OBJECT_ROOT.path_join("technique").path_join(
				"%s.gdshaderinc" % entry["implementation"]))
		for token in entry.get("forbidden_shader_tokens", []):
			assert_false(implementation.contains(String(token)),
					"%s: forbidden topology %s reached its technique" % [engine_enum, token])


func test_every_retail_pass_class_has_a_runtime_or_exclusion_disposition() -> void:
	var audit: Dictionary = _load_json(AUXILIARY_TECHNIQUE_VALIDATION_PATH)
	var inventory: Dictionary = _load_json(RETAIL_EFFECT_INVENTORY_PATH)
	var pipeline: Dictionary = _load_json(MANIFEST_PATH)

	assert_eq(int(audit["schema"]), 1)
	assert_eq(int(audit["quality_scope"]["shader_usage_level"]), 2)
	assert_true(bool(audit["quality_scope"]["screenshots_allowed_only_after_validation"]))
	assert_not_null(_commit_re.search(String(audit["source_corpus"]["commit"])))
	var classes: Dictionary = audit["pass_classes"]
	assert_eq(_sorted_keys(classes), _sorted_keys(inventory["classes"]))

	var expected_scopes := {
		"TECHNIQUE_NORMAL": "live",
		"TECHNIQUE_PROJSHAD": "live",
		"TECHNIQUE_DEPTHMASK": "unreachable",
		"TECHNIQUE_CLIP": "live",
		"TECHNIQUE_GLOW": "live",
		"TECHNIQUE_MATCHTERRAIN": "live",
		"UNSPECIFIED": "excluded_lower_quality",
	}
	var expected_probes := {
		"TECHNIQUE_NORMAL": ["lighting", "channels"],
		"TECHNIQUE_PROJSHAD": ["projshadow"],
		"TECHNIQUE_DEPTHMASK": [],
		"TECHNIQUE_CLIP": ["clip"],
		"TECHNIQUE_GLOW": ["glow"],
		"TECHNIQUE_MATCHTERRAIN": ["matchterrain"],
		"UNSPECIFIED": [],
	}
	var probe_source := _read("res://probes/render/render_swatch_probe.gd") \
			+ _read("res://probes/render/render_swatch_support.gd") \
			+ _read("res://probes/render/render_swatch_lighting_modes.gd") \
			+ _read("res://probes/render/render_swatch_pass_modes.gd")

	for pass_class in classes:
		var entry: Dictionary = classes[pass_class]
		var inventory_entry: Dictionary = inventory["classes"][pass_class]
		assert_eq(String(entry["scope"]), String(expected_scopes[pass_class]), pass_class)
		assert_eq(entry["disposition"], inventory_entry["disposition"], pass_class)
		assert_eq(int(entry["declaration_count"]), int(inventory_entry["declaration_count"]), pass_class)
		assert_eq(int(entry["pass_declaration_count"]), int(inventory_entry["pass_declaration_count"]), pass_class)
		assert_true(["matching", "matching_with_bounded_residuals"].has(entry["parity_status"]), pass_class)
		if entry["parity_status"] == "matching_with_bounded_residuals":
			assert_false((entry["bounded_residuals"] as Array).is_empty(), pass_class)
		else:
			assert_eq(entry["bounded_residuals"], [], pass_class)
		assert_eq(entry["raster_probes"], expected_probes[pass_class], pass_class)
		for mode in entry["raster_probes"]:
			assert_true(probe_source.contains("\"%s\"" % mode), "%s probe %s" % [pass_class, mode])

		# The decoded-symbol leg needed the retail corpus; the audit still has
		# to name its evidence.
		assert_false((entry["source_evidence"] as Array).is_empty(), pass_class)
		for evidence in entry["source_evidence"]:
			assert_false(String(evidence["file"]).is_empty(), pass_class)
			assert_false((evidence["symbols"] as Array).is_empty(), pass_class)

		assert_false((entry["runtime_evidence"] as Array).is_empty(), pass_class)
		for evidence in entry["runtime_evidence"]:
			var path := _repo_path(String(evidence["path"]))
			assert_true(FileAccess.file_exists(path), "missing runtime evidence %s" % evidence["path"])
			if not FileAccess.file_exists(path):
				continue
			var source := FileAccess.get_file_as_string(path)
			for token in evidence["tokens"]:
				assert_true(source.contains(String(token)),
						"%s: %s absent from %s" % [pass_class, token, evidence["path"]])

	var techniques := _technique_enums(pipeline)
	assert_eq(techniques.size(), 24)
	for contract_name in ["projected_shadow_contracts", "projected_shadow_state_contracts",
			"match_terrain_contracts", "glow_contracts"]:
		assert_eq(_sorted_keys(pipeline[contract_name]), techniques, contract_name)
	var with_clip := {}
	for entry in pipeline["techniques"]:
		if not String(entry["clip_class"]).is_empty():
			with_clip[entry["engine_enum"]] = true
	assert_eq(_sorted_keys(with_clip), techniques)
	var auxiliary := {}
	for resource in pipeline["auxiliary_resources"]:
		auxiliary[resource["path"]] = true
	assert_eq(_sorted_keys(auxiliary), [
		"postmultiply/environment_textured.gdshader",
		"postmultiply/environment_textured_cutout.gdshader",
		"postmultiply/environment_textured_cutout_double_sided.gdshader",
		"postmultiply/environment_textured_double_sided.gdshader",
	])
	assert_false(String(classes["TECHNIQUE_DEPTHMASK"]["unreachable_reason"]).is_empty())


# --- lighting / terrain / foliage pins ----------------------------------


func test_lighting_contracts_reach_the_shader_math() -> void:
	_contains_all(_read(OBJECT_ROOT.path_join("shared.gdshaderinc")), [
		"opennova_light_block_hemi_sky", "opennova_light_block_hemi_ground",
		"opennova_light_block_dir", "opennova_light_block_dir_color", "opennova_fog_enabled",
		"u_entity_light", "obj_dir_light_color", "u_point_light_count", "obj_ff_lighting",
		"obj_point_light_sum", "obj_bump_diffuse_lighting", "obj_phong_lighting",
		"obj_phong_map_specular", "obj_environment_cube_approx", "obj_apply_additive_fog",
		"v_dir_self_shadow", "v_pixel_point_factor", "obj_pixel_point_vertex_factors",
	], "object/shared.gdshaderinc")
	# ADR 0043: the terrain is lit by the Godot scene — the include carries
	# only the authored surface (splat/albedo) composition.
	_contains_all(_read(SHADER_ROOT.path_join("terrain_lighting.gdshaderinc")),
			["terrain_surface_albedo", "TERRAIN_ALBEDO_SCALE"], "terrain_lighting.gdshaderinc")
	_contains_none(_read(SHADER_ROOT.path_join("terrain_lighting.gdshaderinc")),
			["u_sun_light", "terrain_point_light_pool", "apply_terrain_fog"],
			"terrain_lighting.gdshaderinc")
	_contains_all(_read(SHADER_ROOT.path_join("foliage_detail.gdshaderinc")),
			["opennova_sky_ambient", "opennova_sun_light", "opennova_sun_direction"], "foliage_detail.gdshaderinc")
	var sky := _read(SHADER_ROOT.path_join("sky.gdshader"))
	assert_true(sky.contains("dot(dome_normal, u_sun_dir)"))
	assert_true(sky.contains("dot(dome_normal, u_light_dir)"))


func test_highest_quality_foliage_rejects_the_inert_d3d_light_premise() -> void:
	var foliage := _read(SHADER_ROOT.path_join("foliage_detail.gdshaderinc"))
	assert_true(foliage.contains("tile.a * opennova_sun_light + opennova_sky_ambient"))
	assert_true(foliage.contains("lit * u_emitter_color * 8.0"))
	_contains_none(foliage, ["u_point_light_count", "u_point_light_posr_0",
			"opennova_static_point_light_rows", "terrain_point_light_pool"], "foliage_detail.gdshaderinc")
	_contains_all(foliage, [
		"Light_SelectAndEnableForDraw for each patch @ 0x60a5dc",
		"Foliage_WindSwayVS @ 0x60087a..0x600883",
		"writes oD0 = c6", "failed-VS fixed-function",
	], "foliage_detail.gdshaderinc")

	var contract := _contract(_load_json(PROVENANCE_PATH), "foliage")
	assert_eq(String(contract["light_behavior"]), "terrain_tile_cache_environment_or_depth_only")
	var citations := _citation_addresses(contract)
	for address in ["0x5ff630", "0x60087a", "0x60a5dc"]:
		assert_true(citations.has(address), "foliage cites %s" % address)


func test_retail_tile_set_atlas_is_carried_not_misclassified_as_a_lightmap() -> void:
	_contains_all(_read_repo("engine/runtime/terrain/terrain_tile_composer.h"),
			["mission .til RGB", "const Rgba8Image *tilestrip", "compose_terrain_tile_page"],
			"terrain_tile_composer.h")
	_contains_all(_read_repo("engine/runtime/terrain/terrain_tile_composer.cpp"),
			["til_build_entry_render_uv_quad", "sample_tilestrip", "compose_overlay_rgba",
			"add_dot3_alpha(output, dot3_alpha)"], "terrain_tile_composer.cpp")
	_contains_all(_read_repo("godot/src/terrain/terrain_tile_cache_device.cpp"),
			["get_tilestrip_tex()", "snapshot->tilestrip", "result.tilestrip = &tilestrip"],
			"terrain_tile_cache_device.cpp")
	var terrain := _read(SHADER_ROOT.path_join("terrain.gdshader"))
	var foliage := _read(SHADER_ROOT.path_join("foliage_detail.gdshaderinc"))
	for shader in [terrain, foliage]:
		assert_true(shader.contains("uniform sampler2DArray u_tile_cache"))
		assert_true(shader.contains("texture(u_tile_cache"))
	assert_true(terrain.contains("terrain_surface_albedo"))
	assert_true(foliage.contains("tile.a * opennova_sun_light + opennova_sky_ambient"))

	var contract := _contract(_load_json(PROVENANCE_PATH), "terrain-surface")
	var citations := _citation_addresses(contract)
	for address in ["0x604a90", "0x60ddd4"]:
		assert_true(citations.has(address), "terrain-surface cites %s" % address)
	assert_false((contract["exceptions"] as Array).has("D-RLIT-6"))


func test_terrain_static_shadow_final_composite_is_explicit_and_cited() -> void:
	_contains_all(_read_repo("engine/runtime/terrain/terrain_static_shadow_alpha.h"),
			["RGBA writes and additive", "ONE/ONE", "PSDepthAlpha",
			"composite_terrain_static_shadow_pixel", "temporary_blue"],
			"terrain_static_shadow_alpha.h")
	_contains_all(_read_repo("engine/runtime/terrain/terrain_static_shadow_alpha.cpp"),
			["destination_rgba[0]", "destination_rgba[1]", "destination_rgba[2]",
			"static_cast<int>(destination_rgba[3])", "composite_terrain_static_shadow_pixel("],
			"terrain_static_shadow_alpha.cpp")
	_contains_all(_read_repo("tests/terrain/terrain_static_shadow_alpha_test.cpp"),
			["test_retail_additive_composite_contract", "zero-RGB ONE/ONE source preserves RGB",
			"saturates like RGBA8 retail"], "terrain_static_shadow_alpha_test.cpp")
	var contract := _contract(_load_json(PROVENANCE_PATH), "terrain-surface")
	assert_true(_citation_addresses(contract).has("0x60e0c6"))


func test_max_quality_tile_page_projection_is_one_c7_c8_cutover() -> void:
	_contains_all(_read_repo("engine/runtime/terrain/terrain_tile_composition_cache.h"), [
		"TerrainTilePageProjection", "Foliage_RenderFarPatches @0x60A1DE..0x60A34F",
		"c7/c8 uploads", "failed-vertex-", "page_projection(",
	], "terrain_tile_composition_cache.h")
	_contains_all(_read_repo("engine/runtime/terrain/terrain_tile_composition_cache.cpp"), [
		"world_x - world_origin_x", "world_z - world_origin_z", "1.0f / static_cast<float>(span)",
	], "terrain_tile_composition_cache.cpp")
	assert_true(_read_repo("engine/runtime/terrain/terrain_static_shadow_raster.cpp").contains(
			"TerrainTileCompositionCache::page_projection(input.page)"))

	var terrain := _read(SHADER_ROOT.path_join("terrain.gdshader"))
	var foliage := _read(SHADER_ROOT.path_join("foliage_detail.gdshaderinc"))
	var match_terrain := _read(OBJECT_ROOT.path_join("match_terrain.gdshaderinc"))
	var object_shared := _read(OBJECT_ROOT.path_join("shared.gdshaderinc"))
	for shader in [terrain, foliage]:
		assert_true(shader.contains("u_instance_tile_cache_projection"))
		assert_false(shader.contains("u_instance_tile_cache_origin_span"))
	for shader in [object_shared, match_terrain]:
		assert_true(shader.contains("u_match_terrain_page_projection"))
		assert_false(shader.contains("u_match_terrain_page_origin_span"))
	var runtime_consumers := "\n".join(PackedStringArray([
		_read_repo("godot/src/terrain/terrain.cpp"),
		_read_repo("godot/src/terrain/foliage_dispatcher.cpp"),
		_read_repo("godot/src/object/object_model.cpp"),
	]))
	assert_eq(runtime_consumers.count("TerrainTileCompositionCache::page_projection("), 3)
	assert_false(runtime_consumers.contains("tile_cache_origin_span"))
	assert_false(runtime_consumers.contains("match_terrain_page_origin_span"))

	var provenance: Dictionary = _load_json(PROVENANCE_PATH)
	assert_true(_citation_addresses(_contract(provenance, "terrain-surface")).has("0x60db67"))
	var foliage_citations := _citation_addresses(_contract(provenance, "foliage"))
	for address in ["0x6006f0", "0x60a220"]:
		assert_true(foliage_citations.has(address), "foliage cites %s" % address)


func _between(source: String, start_token: String, end_token: String) -> String:
	var start := source.find(start_token)
	assert_gte(start, 0, "source carries %s" % start_token)
	if start < 0:
		return ""
	var after := source.substr(start + start_token.length())
	var stop := after.find(end_token)
	assert_gte(stop, 0, "source carries %s after %s" % [end_token, start_token])
	return after.substr(0, stop) if stop >= 0 else after


func test_object_point_lights_preserve_the_retail_stage_split() -> void:
	var shared := _read(OBJECT_ROOT.path_join("shared.gdshaderinc"))
	var fixed_point := _between(shared, "vec3 obj_point_light_one", "vec3 obj_point_light_sum")
	var pixel_vertex_factor := _between(shared, "float obj_pixel_point_vertex_factor_one",
			"vec4 obj_pixel_point_vertex_factors")
	var pixel_vertex_attenuation := _between(shared, "float obj_pixel_point_vertex_attenuation_one",
			"vec4 obj_pixel_point_vertex_factors")
	var pixel_fragment_points := _between(shared, "vec3 obj_bump_diffuse_point_one",
			"vec3 obj_environment_cube_approx")

	# Fixed-function lights are D3D vertex lights and keep the D3D range gate.
	assert_true(fixed_point.contains("if (dist >= col.w)"))
	assert_true(fixed_point.contains("ndotl"))
	# DOT3/Phong authored effects interpolate attenuation+self-shadow from
	# their vertex programs; only mapped-normal N.L/N.H remains per fragment.
	assert_true(pixel_vertex_factor.contains("obj_self_shadow(geom_normal_ws, to_light)"))
	assert_true(pixel_vertex_factor.contains("1.0 + posr.w * distance_to_light * distance_to_light"))
	assert_false(pixel_vertex_factor.contains("col.w"))
	assert_false(pixel_vertex_attenuation.contains("obj_self_shadow"))
	assert_true(pixel_vertex_attenuation.contains("1.0 + posr.w * distance_to_light * distance_to_light"))
	assert_false(pixel_fragment_points.contains("obj_self_shadow"))
	assert_false(pixel_fragment_points.contains("dist >="))
	_contains_all(pixel_fragment_points, ["vertex_factor", "v_pixel_point_factor", "v_pixel_point_attenuation"],
			"the per-fragment point helpers")

	var vertex_standard := _read(OBJECT_ROOT.path_join("vertex_standard.gdshaderinc"))
	var vertex_flag := _read(OBJECT_ROOT.path_join("vertex_flag.gdshaderinc"))
	assert_true(vertex_standard.contains("obj_pixel_point_vertex_factors"))
	assert_true(vertex_standard.contains("obj_pixel_point_vertex_attenuations"))
	assert_true(vertex_flag.contains("v_pixel_point_factor = vec4(0.0)"))
	assert_true(vertex_flag.contains("v_pixel_point_attenuation = vec4(0.0)"))

	var validation: Dictionary = _load_json(TECHNIQUE_VALIDATION_PATH)
	var audited := {}
	for entry in validation["techniques"]:
		audited[entry["engine_enum"]] = entry
	assert_eq(audited["PhongObjectSpecularPhongMap"]["responses"],
			{"directional": true, "hemisphere": false, "ambient": true, "point": true})
	for name in ["Flag", "GlassFixed", "GlassSkinned"]:
		assert_false(bool(audited[name]["responses"]["point"]), name)


func test_point_lights_present_as_scene_omni_lights() -> void:
	# ADR 0043: the pool's presentation is real OmniLight3D nodes (clustered
	# Forward+ replaces the retired per-draw select, instance uniforms and the
	# static RGBAF atlas). The engine keeps the intent fold; the binding syncs
	# nodes.
	var light_scene := _read_repo("godot/src/lights/light_scene.cpp")
	_contains_all(light_scene, ["collect_scene_lights", "sync_scene_lights",
			"PARAM_ENERGY, 2.0f", "PARAM_SPECULAR, 0.0f",
			"godot_from_mission_float(row.position)"], "the light scene")
	var engine_scene := _read_repo("engine/runtime/renderer/light_scene.cpp")
	_contains_all(engine_scene, ["collect_scene_lights", "apply_rgb_gen",
			"point_light_color(rgb, slot.blend, ambient_scale"],
			"the engine pool")


func test_material_rgb_alpha_and_coverage_channels_are_technique_specific() -> void:
	var pipeline: Dictionary = _load_json(MANIFEST_PATH)
	assert_eq(pipeline["specular_alpha_contracts"], {
		"PhongTangentSpecular": "pow8_brightness",
		"PhongTangentSpecularSkinned": "pow8_brightness",
		"PhongObjectSpecular": "pow8_brightness",
		"PhongObjectSpecularPhongMap": "phong_map_brightness_and_exponent",
		"EnvironmentPhong": "pow8_brightness",
	})

	var single := _read(OBJECT_ROOT.path_join("sampling/single.gdshaderinc"))
	var detail := _read(OBJECT_ROOT.path_join("sampling/detail.gdshaderinc"))
	var surface := _read(OBJECT_ROOT.path_join("surface.gdshaderinc"))
	var self_lit := _read(OBJECT_ROOT.path_join("technique/self_lit.gdshaderinc"))
	for sampling in [single, detail]:
		_contains_all(sampling, ["#ifdef OBJ_RGB_MOD_SELF_LUM", "base.rgb *= u_rgb_mod",
				"#ifdef OBJ_ALPHA_MOD_FFP", "base.a *= u_alpha_mod"], "the sampling include")
	_contains_all(surface, [
		"OBJ_COVERAGE_VERTEX_DIFFUSE_ALPHA", "coverage_alpha = v_dir_self_shadow",
		"OBJ_COVERAGE_NORMAL_ALPHA", "coverage_alpha = obj_normal_alpha()",
		"OBJ_COVERAGE_REFLECT_ALPHA", "coverage_alpha = u_reflect_color.a",
		"OBJ_COVERAGE_ZERO", "coverage_alpha = 0.0",
	], "surface.gdshaderinc")
	assert_true(self_lit.contains("output_alpha = 0.0"))

	var wrapper_macros := {
		"Fixed": ["fixed", "OBJ_ALPHA_MOD_FFP", "OBJ_RGB_MOD_NONE"],
		"FixedSkinned": ["fixed_skinned", "OBJ_ALPHA_MOD_NONE", "OBJ_RGB_MOD_NONE"],
		"SelfLit": ["self_lit", "OBJ_ALPHA_MOD_NONE", "OBJ_RGB_MOD_SELF_LUM"],
		"PhongObjectSpecular": ["phong_object_specular", "OBJ_ALPHA_MOD_NONE", "OBJ_RGB_MOD_NONE"],
	}
	for technique in wrapper_macros:
		var row: Array = wrapper_macros[technique]
		var wrapper := _read(OBJECT_ROOT.path_join(String(row[0])).path_join("opaque.gdshader"))
		assert_true(wrapper.contains("#define %s" % row[1]), technique)
		assert_true(wrapper.contains("#define %s" % row[2]), technique)


func test_water_reflection_clip_class_matches_every_reachable_retail_effect() -> void:
	var pipeline: Dictionary = _load_json(MANIFEST_PATH)
	var actual := {}
	for entry in pipeline["techniques"]:
		actual[entry["engine_enum"]] = entry["clip_class"]
	assert_eq(actual, {
		"Fixed": "explicit",
		"FixedSkinned": "skinned_submit_skip",
		"FixedDetail": "explicit",
		"SelfLit": "explicit",
		"SelfLitDetail": "explicit",
		"Tracer": "normal_fallback",
		"Flag": "normal_fallback",
		"PhongTangentDiffuse": "explicit_unskinned_or_submit_skip_skinned",
		"PhongTangentSpecular": "explicit",
		"PhongTangentSpecularSkinned": "skinned_submit_skip",
		"PhongObjectDiffuse": "explicit_unskinned_or_submit_skip_skinned",
		"PhongObjectSpecular": "normal_fallback",
		"PhongObjectSpecularPhongMap": "skinned_submit_skip",
		"Dot3Tangent": "explicit",
		"Dot3TangentDetail": "explicit",
		"Dot3TangentSkinned": "skinned_submit_skip",
		"Dot3TangentDetailSkinned": "skinned_submit_skip",
		"Dot3Object": "skinned_submit_skip",
		"Dot3ObjectDetail": "skinned_submit_skip",
		"EnvironmentMirror": "normal_fallback",
		"EnvironmentMirrorTextured": "normal_fallback",
		"EnvironmentPhong": "normal_fallback",
		"GlassFixed": "explicit",
		"GlassSkinned": "skinned_submit_skip",
	})

	var inventory: Dictionary = _load_json(RETAIL_EFFECT_INVENTORY_PATH)
	assert_eq(inventory["classes"]["TECHNIQUE_CLIP"]["evidence"], {
		"_FFP.fx": ["TBoringFFPClip"],
		"BDiffT2.fx": ["TSegTanDiff_Clip"],
		"Dot3DiffO.fx": ["TSegObjDiff_clip"],
		"Dot3DiffT.fx": ["TSegTanDiff_Clip"],
		"Glass.fx": ["TGlassFFP_clip"],
		"PhongT.fx": ["TSegTanDiff_Clip"],
	})

	_contains_all(_read(OBJECT_ROOT.path_join("shared.gdshaderinc")), [
		"opennova_water_reflection_clip_active", "opennova_water_reflection_eye",
		"opennova_water_height - 0.1", "OBJ_CLIP_EXPLICIT",
		"OBJ_CLIP_EXPLICIT_UNSKINNED_OR_SUBMIT_SKIP_SKINNED", "discard",
	], "object/shared.gdshaderinc")
	assert_true(_read(OBJECT_ROOT.path_join("surface.gdshaderinc")).contains(
			"obj_apply_water_reflection_clip(camera_position_world)"))
	_contains_all(_read_repo("godot/src/env/water.cpp"), [
		"\"opennova_water_reflection_eye\"", "\"opennova_water_reflection_clip_active\"",
		"!view.below_water",
	], "water.cpp")


func test_gamma_encoded_retail_effect_math_crosses_godot_linear_boundary_once() -> void:
	var sources := _shader_sources()
	for name in sources:
		var source: String = sources[name]
		assert_false(source.contains(": source_color"), name)
		assert_false(source.contains("gamma_to_linear"), name)
		assert_false(source.contains("linear_to_gamma"), name)

	for name in ["particle/particle_blend_additive.gdshader", "particle/particle_blend_blend.gdshader",
			"particle/particle_blend_bump.gdshader", "particle/particle_blend_bumpadd.gdshader",
			"particle/particle_blend_mod.gdshader", "particle/particle_blend_mod2x.gdshader",
			"particle/particle_blend_premult.gdshader"]:
		var source: String = sources.get(name, "")
		assert_true(source.contains("#include \"res://shaders/color.gdshaderinc\""), name)
		assert_true(source.contains("scene_output"), name)

	var distort: String = sources.get("particle/particle_blend_distort.gdshader", "")
	assert_false(distort.contains("scene_output"))
	assert_false(distort.contains("display_decode_gamma"))

	var nvg: String = sources.get("nvg_view.gdshader", "")
	assert_true(nvg.contains("display_encode_gamma"))
	assert_true(nvg.contains("display_decode_gamma"))

	var drape: String = sources.get("slot_shadow_drape.gdshader", "")
	assert_true(drape.contains("scene_output(factor)"))
	assert_false(drape.contains("pow(factor"))
	assert_true(drape.contains("vec3 ambient = vec3(1.0) - (1.0 - fade) * u_slot_term[i].rgb;"))
	assert_true(drape.contains("vec3 shadowed = s.rgb + ambient;"))
	assert_false(drape.contains("u_slot_term[i].rgb * s.a"))

	var color_contract: String = sources.get("color.gdshaderinc", "")
	_contains_all(color_contract, ["scene_output", "scene_input",
			"display_decode_gamma", "display_encode_gamma"], "color.gdshaderinc")
	assert_false(color_contract.contains("gamma_to_linear"))
	assert_false(color_contract.contains("linear_to_gamma"))

	var frame_renderer := _read_repo("godot/src/render/frame_fx.cpp")
	_contains_all(frame_renderer, [
		"FramePass::GammaDecode", "EFFECT_CALLBACK_TYPE_POST_TRANSPARENT",
		"framebuffer_blend_domain\"] = \"gamma\"",
		"kBeautyCameraMask = 0x00018C01u",
		"DATA_FORMAT_R8G8B8A8_UNORM", "direction_for_degrees(30.0f, 1.0f / 1024.0f)",
		"1.0f / 2048.0f", "* 0.50", "* 0.46", "* 0.35", "* 0.19",
		"Vector2i(kFrameFxSide, kFrameFxSide), 90.0f", "Vector2i(kFrameFxSide, kFrameFxSide), 0.0f",
		"direction_for_degrees(45.0f, 0.0027621093f)",
		"BlendMode::SourceAlphaAdd, FramePass::FinalAverage",
		"result[\"capture_filter\"] = \"linear_rgba8_highest_quality\"",
	], "frame_fx.cpp")
	assert_false(frame_renderer.contains("kQ3CameraMask"),
			"the typed Q3 adapter is the sole focused renderer")
	var q3_adapter := _read_repo("godot/src/render/q3_frame_adapter.cpp")
	_contains_all(q3_adapter, [
		"glsl_float(kQ3GlassWhiteLobeGain)",
		"glsl_float(kQ3GlassWhiteLobePower)",
		"glsl_float(kQ3GlassWarmLobeColor[1])",
		"glsl_float(kQ3GlassWarmLobePower)",
		"glsl_float(kQ3WaterNvLumaWeights[0])",
		"glsl_float(kQ3WaterNvBrightBias)",
		"pow(aligned, @GLASS_WHITE_POWER@)",
		"pow(aligned, @GLASS_WARM_POWER@)",
		"const Vector3 light_gain = frame->light_gain",
		"runtime/renderer/device_fog.h",
		"float q3_fog_visibility(float dist, float fog_start, float fog_end,",
		"4.1588830833596715 / safe_end",
		"const float fog_start = frame->fog_start;",
		"fog_visibility",
		"coverage > pc.params.z",
		"pc.draw_color.rgb * lobe * fog_visibility",
		"texture(secondary_texture, detail_uv).rgb * 2.0",
		"vec3 lit = base * pc.draw_color.rgb;",
		"frag_color = vec4(clamp(fogged, 0.0, 1.0), 0.0);",
		"draw.object.self_lum_color.x * std::min(light_gain.x, 1.0f) * 2.0f",
		"view_dot_sq * view_dot_sq",
		"model_uniform_scale",
		"glsl_float(1.0f - kQ3FarBandMaxZ)",
		"glsl_float(kQ3FarBandMaxZ - kQ3FarBandMinZ)",
		"(@FAR_BAND_REV_MIN@ + z_rev * @FAR_BAND_REV_SPAN@)",
		"depth->set_enable_depth_test(true)",
		"classification.is_two_sided",
		"fallback_pixel.ptrw()[0] = 255",
		"(in_position - pc.camera_local.xyz) * (1.0 - 3.0e-4)",
		"case ObjectBlendMode::Additive:",
		"Q3DeviceBlend::Add",
		"result[\"q3_sun_depth_test\"] = true",
		"result[\"q3_far_band\"] = Vector2(kQ3FarBandMinZ, kQ3FarBandMaxZ)",
		"cache.acquire(request",
		"result[\"q3_readbacks_this_frame\"]",
		"result[\"q3_instance_row_reads_this_frame\"]",
		"result[\"q3_records_touched_this_frame\"]",
		"request.geometry_generation = record.geometry_generation",
		"Q3SourceRegistry::live_records()",
		"frustum.outside(row.world_bounds)",
	], "q3_frame_adapter.cpp")
	# The per-source records: the material block is read through the
	# material only when its parameter version moved, the MultiMesh rows
	# once per instance generation, and the producer-published arrays ride
	# the record.
	var q3_registry := _read_repo("godot/src/render/q3_source_registry.cpp")
	_contains_all(q3_registry, [
		"\"u_rgb_mod\"",
		"object.self_lum_color = {self_lum.x, self_lum.y, self_lum.z, 1.0f}",
		"object.detail_texture = lease_for(detail)",
		"glare_view_fade = bool_parameter",
		"r_surface.parameter_version == version",
		"rows_generation != r_record.instance_generation",
		"++r_counters.instance_row_reads",
		"callable_mp_static(&Q3SourceRegistry::on_visibility_changed)",
	], "q3_source_registry.cpp")
	# The interleaved Q3 stream (with the detail UV2 row) is packed once per
	# cache entry generation, never re-read through the server per frame.
	var q3_cache := _read_repo("godot/src/render/q3_geometry_cache.cpp")
	_contains_all(q3_cache, [
		"Mesh::ARRAY_TEX_UV2",
		"++counters_.readbacks",
		"++counters_.instance_row_reads",
		"kQ3VertexStride",
	], "q3_geometry_cache.cpp")

	# The first-person viewmodel draws inside the beauty pass through the
	# shader-side renderfov projection + depth band; no composite shader.
	assert_false(sources.has("viewmodel_composite.gdshader"))
	var viewmodel_pass: String = sources.get("viewmodel_pass.gdshaderinc", "")
	_contains_all(viewmodel_pass, ["global uniform vec4 opennova_viewmodel_projection",
			"instance uniform bool u_viewmodel_pass", "NOVA_VIEWMODEL_DEPTH_WINDOW = 0.1"],
			"viewmodel_pass.gdshaderinc")

	var probe := _read("res://probes/render/render_swatch_probe.gd") \
			+ _read("res://probes/render/render_swatch_support.gd") \
			+ _read("res://probes/render/render_swatch_lighting_modes.gd") \
			+ _read("res://probes/render/render_swatch_pass_modes.gd")
	assert_true(probe.contains("[\"SRCALPHA/INVSRCALPHA\", -0.3, 128]"))
	assert_true(probe.contains("[\"ONE/ONE\", 0.3, 96]"))


func test_environment_techniques_do_not_invent_fresnel_or_diffuse_terms() -> void:
	var combined := ""
	for name in ["environment", "environment_textured", "glass", "glass_skinned"]:
		combined += _read(OBJECT_ROOT.path_join("technique").path_join("%s.gdshaderinc" % name))
	var executable := PackedStringArray()
	for line in combined.split("\n"):
		if not line.lstrip(" \t").begins_with("//"):
			executable.append(line)
	var text := "\n".join(executable).to_lower()
	_contains_none(text, ["fresnel", "obj_ff_lighting", "obj_bump_diffuse_lighting"],
			"the environment/glass techniques")


func test_windowed_probe_exercises_direction_hemisphere_and_gameplay_points() -> void:
	var probe := _read("res://probes/render/render_swatch_probe.gd") \
			+ _read("res://probes/render/render_swatch_support.gd") \
			+ _read("res://probes/render/render_swatch_lighting_modes.gd") \
			+ _read("res://probes/render/render_swatch_pass_modes.gd")
	_contains_all(probe, [
		"\"direction_a\"", "\"direction_b\"", "\"hemi_sky\"", "\"hemi_ground\"", "\"ambient_off\"",
		"\"ambient_on\"", "\"point_off\"", "\"point_on\"", "\"point_static\"",
		"set_instance_shader_parameter(\"u_point_light_count\"", "MultiMeshInstance3D.new()",
		"use_custom_data = true", "set_instance_custom_data",
		"\"opennova_static_point_light_rows\", static_point_atlas",
		"static atlas point-light route was not pixel-identical",
		"\"res://shaders/object/technique_validation.json\"",
		"did not reverse its directional lobe", "did not reverse sky/ground response",
		"invented a directional response", "invented a hemisphere response",
		"invented an ambient response", "invented a gameplay point-light response",
		"\"rgb_low\"", "\"rgb_high\"", "\"specular_alpha_low\"", "\"specular_alpha_high\"",
		"\"coverage_low\"", "\"coverage_high\"", "\"alpha_gen_low\"", "\"alpha_gen_high\"",
		"parsed.get(\"specular_alpha_contracts\", {})", "\"specular alpha\"", "\"coverage source\"",
		"\"RgbGen\"", "\"AlphaGen\"", "did not react to %s", "invented a %s response",
		"\"inactive\"", "\"disarmed\"", "\"wrong_eye\"", "\"reflection\"",
		"\"object-water-reflection-clip\"", "\"reflection CLIP\"",
		"clipped above waterHeight-0.1", "clipped an unrelated camera",
	], "render_swatch_probe.gd")
