extends GutTest

## The textual contract over godot/shaders: what tests/test_shader_resources.py
## (structural half) and tests/test_object_shader_resources.py pinned before the
## Python suite was retired (ADR 0038). Provenance one-to-one coverage, the
## include graph (no cycles, no orphans, nothing leaving res://shaders), one
## UID sidecar per resource, one shader_type per wrapper, the object pipeline
## manifest topology, the vertex point-light products each technique consumes,
## the slot-capture camera signature, and the transitive-source golden.
## Dropped with their inputs: the retail .fx decode legs (the
## third_party/modsuperoed corpus) and the wrapper-generator idempotence check
## (scripts/generate_object_shaders.py); the checked-in wrappers are the
## authoritative sources now. shader_resource_validation_test.gd is the
## device-side companion (every resource loaded through Godot).

const SHADER_ROOT := "res://shaders"
const OBJECT_ROOT := "res://shaders/object"
const PROVENANCE_PATH := "res://shaders/provenance.json"
const MANIFEST_PATH := "res://shaders/object/pipeline_manifest.json"
const HASH_GOLDEN_PATH := "res://tests/object_shader_resource_hashes.golden.json"
const VERTEX_POINT_LIGHT_VARYINGS := {
	"v_point_light_diffuse": "ffp_diffuse",
	"v_pixel_point_factor": "self_shadowed_attenuation",
	"v_pixel_point_attenuation": "attenuation",
}

var _include_re := RegEx.new()
var _shader_type_re := RegEx.new()
var _function_head_re := RegEx.new()
var _identifier_re := RegEx.new()
var _line_comment_re := RegEx.new()


func before_all() -> void:
	_include_re.compile("(?m)^\\s*#include\\s+\"res://([^\"]+)\"")
	_shader_type_re.compile("(?m)^\\s*shader_type\\s+\\w+\\s*;")
	_function_head_re.compile("(?m)^\\s*(?:void|bool|int|float|vec[234]|mat[34])\\s+(\\w+)\\s*\\(")
	_identifier_re.compile("\\b[A-Za-z_]\\w*\\b")
	_line_comment_re.compile("//[^\\n]*")


# --- helpers -------------------------------------------------------------


func _repo_path(relative: String) -> String:
	return ProjectSettings.globalize_path("res://../" + relative)


func _read(path: String) -> String:
	assert_true(FileAccess.file_exists(path), "readable: %s" % path)
	return FileAccess.get_file_as_string(path)


# Python's read_text() universal newlines: every CRLF/CR becomes LF.
func _normalized(path: String) -> String:
	return _read(path).replace("\r\n", "\n").replace("\r", "\n")


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


func _shader_sources() -> Array:
	var out: Array = []
	_collect(SHADER_ROOT, PackedStringArray([".gdshader", ".gdshaderinc"]), out)
	out.sort()
	return out


func _relative(path: String) -> String:
	return path.trim_prefix(SHADER_ROOT + "/")


func _direct_includes_of(source: String) -> Array:
	var out: Array = []
	for m in _include_re.search_all(source):
		out.append("res://" + m.get_string(1))
	return out


func _direct_includes(path: String) -> Array:
	return _direct_includes_of(_read(path))


# Every source reachable from the wrapper (the wrapper included), asserting no
# cycle, no missing include, and nothing outside res://shaders.
func _include_closure(wrapper: String) -> Array:
	var visited := {}
	var active: Array = []
	_visit_include(wrapper, visited, active)
	var out := visited.keys()
	out.sort()
	return out


func _visit_include(path: String, visited: Dictionary, active: Array) -> void:
	var chain: Array = active.duplicate()
	chain.append(path)
	assert_false(active.has(path), "shader include cycle: " + " -> ".join(
			chain.map(func(item: String) -> String: return _relative(item))))
	if active.has(path) or visited.has(path):
		return
	var owner: String = _relative(active[-1]) if not active.is_empty() else _relative(path)
	assert_true(FileAccess.file_exists(path), "missing shader include referenced by %s" % owner)
	assert_true(path.begins_with(SHADER_ROOT + "/"), "shader include leaves res://shaders: %s" % path)
	if not FileAccess.file_exists(path):
		return
	active.append(path)
	for included in _direct_includes(path):
		_visit_include(included, visited, active)
	active.pop_back()
	visited[path] = true


func _matching_contracts(path: String, provenance: Dictionary) -> Array:
	var rel := _relative(path)
	var out: Array = []
	for contract in provenance["contracts"]:
		for pattern in contract["patterns"]:
			if rel.match(String(pattern)):
				out.append(contract)
				break
	return out


func _sorted(values: Array) -> Array:
	var out := values.duplicate()
	out.sort()
	return out


func _sorted_keys(dict: Dictionary) -> Array:
	return _sorted(dict.keys())


func _manifest() -> Dictionary:
	return _load_json(MANIFEST_PATH)


func _expected_generated_paths(manifest: Dictionary) -> Array:
	var out := {}
	for technique in manifest["techniques"]:
		for policy in technique["policies"]:
			for suffix in ["", "_double_sided"]:
				out[OBJECT_ROOT.path_join(String(technique["directory"])).path_join(
						"%s%s.gdshader" % [policy, suffix])] = true
	return _sorted_keys(out)


func _expected_auxiliary_paths(manifest: Dictionary) -> Array:
	var out := {}
	for resource in manifest["auxiliary_resources"]:
		out[OBJECT_ROOT.path_join(String(resource["path"]))] = true
	return _sorted_keys(out)


func _expected_shader_paths(manifest: Dictionary) -> Array:
	var out := {}
	for path in _expected_generated_paths(manifest):
		out[path] = true
	for path in _expected_auxiliary_paths(manifest):
		out[path] = true
	return _sorted_keys(out)


# One wrapper and every recursively included source, in include order: the
# pending list takes each file's includes at its FRONT in file order, and a
# file already seen is skipped when popped (the pytest's transitive_sources).
func _transitive_sources(wrapper: String) -> Array:
	var pending: Array = [wrapper]
	var seen := {}
	var closure: Array = []
	while not pending.is_empty():
		var path: String = String(pending.pop_front()).simplify_path()
		if seen.has(path):
			continue
		seen[path] = true
		var source := _normalized(path)
		closure.append([path, source])
		var includes := _direct_includes_of(source)
		for included in includes:
			assert_true(FileAccess.file_exists(included),
					"missing include from %s" % _relative(path))
		for i in range(includes.size() - 1, -1, -1):
			pending.push_front(includes[i])
	return closure


func _transitive_source_hash(wrapper: String) -> String:
	var parts := PackedStringArray()
	for pair in _transitive_sources(wrapper):
		parts.append("@@ %s\n%s" % ["godot/" + String(pair[0]).trim_prefix("res://"), pair[1]])
	var context := HashingContext.new()
	context.start(HashingContext.HASH_SHA256)
	context.update("\n".join(parts).to_utf8_buffer())
	return context.finish().hex_encode()


# --- provenance / include graph -----------------------------------------


func test_provenance_contract_covers_every_shader_resource_once() -> void:
	var provenance: Dictionary = _load_json(PROVENANCE_PATH)
	var sources := _shader_sources()
	assert_eq(int(provenance["schema"]), 2)
	assert_eq(sources.size(), 193, "the runtime inventory must stay closed")
	assert_eq(int(provenance["resource_count"]), 193)
	var ids := {}
	for contract in provenance["contracts"]:
		ids[contract["id"]] = true
	assert_eq(ids.size(), (provenance["contracts"] as Array).size(), "contract ids are unique")
	for source in sources:
		var contracts := _matching_contracts(source, provenance)
		var names := contracts.map(func(c: Dictionary) -> String: return String(c["id"]))
		assert_eq(contracts.size(), 1,
				"%s must have exactly one provenance contract; got %s" % [_relative(source), names])


func test_all_includes_resolve_without_cycles_or_orphans() -> void:
	var sources := _shader_sources()
	var includes := {}
	var reachable := {}
	for path in sources:
		if path.ends_with(".gdshaderinc"):
			includes[path] = true
		elif path.ends_with(".gdshader"):
			for reached in _include_closure(path):
				if reached.ends_with(".gdshaderinc"):
					reachable[reached] = true
	assert_eq(_sorted_keys(reachable), _sorted_keys(includes),
			"every include is reachable from a wrapper and nothing reaches outside the set")


func test_every_shader_resource_has_exactly_one_uid_sidecar() -> void:
	var sources := _shader_sources()
	var uids: Array = []
	_collect(SHADER_ROOT, PackedStringArray([".uid"]), uids)
	var targets: Array = []
	for uid in uids:
		targets.append(String(uid).trim_suffix(".uid"))
	assert_eq(_sorted(targets), sources)


func test_every_wrapper_declares_one_shader_type_and_inherits_a_citation() -> void:
	var provenance: Dictionary = _load_json(PROVENANCE_PATH)
	for wrapper in _shader_sources():
		if not wrapper.ends_with(".gdshader"):
			continue
		var closure := _include_closure(wrapper)
		var parts := PackedStringArray()
		for path in closure:
			parts.append(_read(path))
		var combined := "\n".join(parts)
		assert_eq(_shader_type_re.search_all(combined).size(), 1,
				"%s must declare exactly one shader_type" % _relative(wrapper))
		var contracts := _matching_contracts(wrapper, provenance)
		assert_false(contracts.is_empty())
		if contracts.is_empty():
			continue
		var cited: bool = combined.to_lower().contains("[orig:") or \
				not (contracts[0]["retail_citations"] as Array).is_empty()
		assert_true(cited, "%s has no retail provenance" % _relative(wrapper))


# --- object pipeline manifest -------------------------------------------


func test_object_shader_runtime_does_not_compose_source_strings() -> void:
	var engine_header := _read(_repo_path("engine/runtime/renderer/object_shader_template.h"))
	var engine_source := _read(_repo_path("engine/runtime/renderer/object_shader_template.cpp"))
	var godot_cache := _read(_repo_path("godot/src/object/object_shader_cache.cpp"))
	assert_false((engine_header + engine_source + godot_cache).contains("compose_object_shader_glsl"))
	assert_false(godot_cache.contains("set_code("))


func test_object_shader_topology_is_compile_time_not_uniform_driven() -> void:
	var includes: Array = []
	_collect(OBJECT_ROOT, PackedStringArray([".gdshaderinc"]), includes)
	var parts := PackedStringArray()
	for path in includes:
		parts.append(_read(path))
	var implementation := "\n".join(parts)
	for removed in ["u_cap_", "u_object_family", "u_environment_source", "u_specular_source"]:
		assert_false(implementation.contains(removed), "%s left the object includes" % removed)

	var bindings := "\n".join(PackedStringArray([
		_read(_repo_path("godot/src/object/object_shader_cache.cpp")),
		_read(_repo_path("godot/src/object/object_model_materials.cpp")),
		_read("res://probes/render/render_swatch_probe.gd"),
		_read("res://probes/render/render_swatch_support.gd"),
		_read("res://probes/render/render_swatch_lighting_modes.gd"),
		_read("res://probes/render/render_swatch_pass_modes.gd"),
	]))
	for removed in ["u_cap_", "u_object_family", "u_environment_source",
			"u_specular_source", "u_emissive"]:
		assert_false(bindings.contains(removed), "%s left the runtime bindings" % removed)

	assert_false(FileAccess.file_exists(OBJECT_ROOT.path_join("object_core.gdshaderinc")))
	var family: Array = []
	if DirAccess.dir_exists_absolute(OBJECT_ROOT.path_join("family")):
		_collect(OBJECT_ROOT.path_join("family"), PackedStringArray([".gdshaderinc"]), family)
	assert_true(family.is_empty(), "no family/*.gdshaderinc")


func test_manifest_covers_every_resource_and_selector_technique() -> void:
	var manifest := _manifest()
	var generated := _expected_generated_paths(manifest)
	var auxiliary := _expected_auxiliary_paths(manifest)
	var expected := _expected_shader_paths(manifest)
	var actual: Array = []
	_collect(OBJECT_ROOT, PackedStringArray([".gdshader"]), actual)
	actual.sort()
	assert_eq(actual, expected)
	for path in generated:
		assert_false(auxiliary.has(path), "generated and auxiliary wrappers are disjoint")
	assert_eq(generated.size(), 128)
	assert_eq(int(manifest["resource_count"]), 128)
	assert_eq(auxiliary.size(), 4)
	assert_eq(int(manifest["auxiliary_resource_count"]), 4)
	assert_eq(actual.size(), 132)
	assert_eq(int(manifest["total_resource_count"]), 132)
	for path in actual:
		assert_ne(String(path).get_base_dir(), OBJECT_ROOT, "no interim root-level wrappers")

	var reachable := {}
	for wrapper in expected:
		for pair in _transitive_sources(wrapper):
			var path := String(pair[0])
			if path.ends_with(".gdshaderinc") and path.begins_with(OBJECT_ROOT + "/"):
				reachable[path] = true
	var actual_includes: Array = []
	_collect(OBJECT_ROOT, PackedStringArray([".gdshaderinc"]), actual_includes)
	assert_eq(_sorted(actual_includes), _sorted_keys(reachable), "no orphan object shader includes")

	var uids: Array = []
	_collect(OBJECT_ROOT, PackedStringArray([".uid"]), uids)
	var uid_targets: Array = []
	for uid in uids:
		uid_targets.append(String(uid).trim_suffix(".uid"))
	assert_eq(_sorted(uid_targets), _sorted(actual + actual_includes))

	var cache_source := _read(_repo_path("godot/src/object/object_shader_cache.cpp"))
	var engine_header := _read(_repo_path("engine/runtime/renderer/object_shader_template.h"))
	for technique in manifest["techniques"]:
		assert_true(cache_source.contains("ObjectShaderTechnique::%s" % technique["engine_enum"]))
		assert_true(engine_header.contains(String(technique["engine_enum"])))
		assert_true(cache_source.contains("return \"%s\"" % technique["directory"]))


func test_auxiliary_shader_resources_match_their_manifest_contracts() -> void:
	var manifest := _manifest()
	var resources := {}
	for resource in manifest["auxiliary_resources"]:
		resources[resource["path"]] = resource
	var expected := {
		"postmultiply/environment_textured.gdshader": {
			"path": "postmultiply/environment_textured.gdshader",
			"pass_class": "TECHNIQUE_NORMAL_P3",
			"contract": "diffuse1_gamma_postmultiply",
			"coverage": "full",
			"cull": "back",
		},
		"postmultiply/environment_textured_double_sided.gdshader": {
			"path": "postmultiply/environment_textured_double_sided.gdshader",
			"pass_class": "TECHNIQUE_NORMAL_P3",
			"contract": "diffuse1_gamma_postmultiply",
			"coverage": "full",
			"cull": "disabled",
		},
		"postmultiply/environment_textured_cutout.gdshader": {
			"path": "postmultiply/environment_textured_cutout.gdshader",
			"pass_class": "TECHNIQUE_NORMAL_P3",
			"contract": "diffuse1_gamma_postmultiply",
			"coverage": "diffuse_alpha",
			"cull": "back",
		},
		"postmultiply/environment_textured_cutout_double_sided.gdshader": {
			"path": "postmultiply/environment_textured_cutout_double_sided.gdshader",
			"pass_class": "TECHNIQUE_NORMAL_P3",
			"contract": "diffuse1_gamma_postmultiply",
			"coverage": "diffuse_alpha",
			"cull": "disabled",
		},
	}
	assert_eq(_sorted_keys(resources), _sorted_keys(expected))
	for key in expected:
		assert_eq(resources.get(key, {}), expected[key], key)
		var resource: Dictionary = expected[key]
		var source := _read(OBJECT_ROOT.path_join(key))
		var expected_cull := "cull_back" if resource["cull"] == "back" else "cull_disabled"
		assert_true(source.contains("unshaded"), key)
		assert_true(source.contains("depth_draw_never"), key)
		assert_true(source.contains(expected_cull), key)
		assert_true(source.contains(
				"#include \"res://shaders/object/postmultiply/environment_textured_body.gdshaderinc\""), key)
		if resource["coverage"] == "diffuse_alpha":
			assert_true(source.contains("#define OBJ_POSTMULTIPLY_CUTOUT"), key)

	assert_false(DirAccess.dir_exists_absolute(OBJECT_ROOT.path_join("glow_proxy")))
	var output := _read(OBJECT_ROOT.path_join("output_opaque.gdshaderinc"))
	for token in ["is_q3_pass", "OBJ_GLOW_ROTATED_SPECULAR",
			"OBJ_GLOW_NORMAL_COPY", "OBJ_GLOW_NO_PASS", "OBJ_Q3_DEPTH_OCCLUDER"]:
		assert_false(output.contains(token), "%s retired from beauty output" % token)
	assert_false(FileAccess.file_exists(OBJECT_ROOT.path_join("glow.gdshaderinc")),
			"the direct typed adapter owns Glass/LUM Q3 techniques")
	assert_false(output.contains("u_glow_hdr_scale"))

	var body := _read(OBJECT_ROOT.path_join("postmultiply/environment_textured_body.gdshaderinc"))
	for token in ["texture(u_diffuse, v_raw_uv)", "textureLod(u_post_screen, SCREEN_UV, 0.0)",
			"2.0 * source_gamma * destination_gamma", "u_alpha_test_invert"]:
		assert_true(body.contains(token), token)


func test_every_wrapper_matches_manifest_topology() -> void:
	var manifest := _manifest()
	var policies: Dictionary = manifest["policies"]
	for technique in manifest["techniques"]:
		var engine_enum := String(technique["engine_enum"])
		for policy_name in technique["policies"]:
			var policy: Dictionary = policies[policy_name]
			for suffix in ["", "_double_sided"]:
				var path := OBJECT_ROOT.path_join(String(technique["directory"])).path_join(
						"%s%s.gdshader" % [policy_name, suffix])
				var source := _read(path)
				var label := "%s/%s%s" % [technique["directory"], policy_name, suffix]
				for expected in [
					"#define OBJ_RGB_MOD_%s" % String(technique["rgb_modulation"]).to_upper(),
					"#define OBJ_ALPHA_MOD_%s" % String(technique["alpha_modulation"]).to_upper(),
					"#define OBJ_COVERAGE_%s" % String(technique["coverage_source"]).to_upper(),
					"#define OBJ_CLIP_%s" % String(technique["clip_class"]).to_upper(),
					"#define OBJ_VERTEX_POINT_LIGHTS_%s" % String(technique["vertex_point_lights"]).to_upper(),
					"#define OBJ_MATCHTERRAIN_%s" % String(manifest["match_terrain_contracts"][engine_enum]).to_upper(),
					"#include \"res://shaders/object/shared.gdshaderinc\"",
					"#include \"res://shaders/object/sampling/%s.gdshaderinc\"" % technique["sampling"],
					"#include \"res://shaders/object/coverage/%s.gdshaderinc\"" % policy["coverage"],
					"#include \"res://shaders/object/normal/%s.gdshaderinc\"" % technique["normal"],
					"#include \"res://shaders/object/match_terrain.gdshaderinc\"",
					"#include \"res://shaders/object/fog/%s.gdshaderinc\"" % technique.get("fog", policy["fog"]),
					"#include \"res://shaders/object/technique/%s.gdshaderinc\"" % technique["implementation"],
					"/object/%s.gdshaderinc\"" % ("vertex_flag" if technique["vertex"] == "flag" else "vertex_standard"),
					"/object/%s.gdshaderinc\"" % ("output_alpha" if policy["writes_alpha"] else "output_opaque"),
					"depth_draw_opaque" if policy["depth"] == "opaque" else "depth_draw_never",
				]:
					assert_true(source.contains(String(expected)), "%s carries %s" % [label, expected])
				assert_false(source.contains("depth_prepass_alpha"), label)


# --- vertex point-light products ----------------------------------------


# Map every top-level GLSL function name to its brace-delimited body text.
func _glsl_function_bodies(source: String) -> Dictionary:
	source = _line_comment_re.sub(source, "", true)
	var bodies := {}
	for head in _function_head_re.search_all(source):
		var open_brace := source.find("{", head.get_end())
		if open_brace < 0:
			continue
		var depth := 0
		for index in range(open_brace, source.length()):
			var ch := source[index]
			if ch == "{":
				depth += 1
			elif ch == "}":
				depth -= 1
				if depth == 0:
					bodies[head.get_string(1)] = source.substr(open_brace, index + 1 - open_brace)
					break
	return bodies


func _varyings_reachable_from(bodies: Dictionary, root: String, varyings: Array) -> Array:
	var reached := {}
	var pending: Array = [root]
	var visited := {}
	while not pending.is_empty():
		var name: String = pending.pop_back()
		if visited.has(name) or not bodies.has(name):
			continue
		visited[name] = true
		var identifiers := {}
		for m in _identifier_re.search_all(bodies[name]):
			identifiers[m.get_string()] = true
		for identifier in identifiers:
			if varyings.has(identifier):
				reached[identifier] = true
			if bodies.has(identifier):
				pending.append(identifier)
	return _sorted_keys(reached)


func test_vertex_point_light_products_match_technique_consumption() -> void:
	# The vertex stage evaluates only the point-light product its technique
	# reads. Every retail vertex program publishes one EffectWorld point-light
	# product (D3D vertex diffuse, oD0 attenuation x self-shadow, or attenuation
	# alone); the manifest names it per technique so vertex_standard skips the
	# other <=4-light loops. This proves the named product is exactly the
	# varying the technique's fragment math can observe, so gating never zeroes
	# a live term.
	var manifest := _manifest()
	var shared := _normalized(OBJECT_ROOT.path_join("shared.gdshaderinc"))
	var vertex_standard := _normalized(OBJECT_ROOT.path_join("vertex_standard.gdshaderinc"))
	var vertex_flag := _normalized(OBJECT_ROOT.path_join("vertex_flag.gdshaderinc"))
	var varyings: Array = VERTEX_POINT_LIGHT_VARYINGS.keys()

	for varying in VERTEX_POINT_LIGHT_VARYINGS:
		var product := String(VERTEX_POINT_LIGHT_VARYINGS[varying])
		var define := "#ifdef OBJ_VERTEX_POINT_LIGHTS_%s" % product.to_upper()
		var define_at := vertex_standard.find(define)
		assert_gte(define_at, 0, "vertex_standard must gate %s on %s" % [varying, define])
		if define_at < 0:
			continue
		var after_define := vertex_standard.substr(define_at + define.length())
		var gated := after_define.substr(0, after_define.find("#endif"))
		var else_at := gated.find("#else")
		assert_gte(else_at, 0, "%s gate has an #else" % varying)
		if else_at < 0:
			continue
		assert_true(gated.substr(0, else_at).contains("%s = obj_" % varying), varying)
		var zero := "vec3(0.0)" if varying == "v_point_light_diffuse" else "vec4(0.0)"
		assert_true(gated.substr(else_at).contains("%s = %s;" % [varying, zero]), varying)
		assert_true(vertex_flag.contains("%s = %s;" % [varying, zero]), varying)

	var products := VERTEX_POINT_LIGHT_VARYINGS.values()
	products.append("none")
	for technique in manifest["techniques"]:
		var product := String(technique["vertex_point_lights"])
		assert_true(products.has(product), "%s names a known product" % technique["engine_enum"])
		if technique["vertex"] == "flag":
			assert_eq(product, "none", "vertex_flag publishes no point-light product")
			continue
		var implementation := _normalized(OBJECT_ROOT.path_join("technique").path_join(
				"%s.gdshaderinc" % technique["implementation"]))
		var bodies := _glsl_function_bodies(shared + "\n" + implementation)
		assert_true(bodies.has("obj_evaluate_surface"), "%s defines obj_evaluate_surface" % technique["engine_enum"])
		var consumed := _varyings_reachable_from(bodies, "obj_evaluate_surface", varyings)
		var expected := {}
		for varying in consumed:
			expected[VERTEX_POINT_LIGHT_VARYINGS[varying]] = true
		if expected.is_empty():
			expected["none"] = true
		assert_eq(_sorted_keys(expected), [product],
				"%s declares vertex_point_lights=%s but its technique math reads %s" % [
					technique["engine_enum"], product, consumed])


# --- slot capture / projected shadow ------------------------------------


func test_projshadow_lives_in_the_slot_capture_pass_not_the_wrappers() -> void:
	# The PROJSHAD pass is SlotShadow's RenderingDevice pass over the engine's
	# per-technique coverage and blend tables: the object wrappers carry no
	# capture branch, no camera-mask signature and no per-technique PROJSHAD
	# define any more, Water reserves no capture layers, and the pass keeps
	# the retail clear, the 4x resolve, the PRE_OPAQUE ordering and the
	# blended alpha-blend variant.
	var shared := _normalized(OBJECT_ROOT.path_join("shared.gdshaderinc"))
	assert_false(shared.contains("NOVA_SLOT_CAPTURE_LAYER_MASK"),
			"the twelve-layer capture signature is retired")
	assert_false(shared.contains("obj_is_slot_shadow_capture"),
			"no wrapper predicate selects a capture camera")
	var includes: Array = []
	_collect(OBJECT_ROOT, PackedStringArray([".gdshaderinc"]), includes)
	for path in includes:
		var source := _normalized(path)
		var label := String(path).get_file()
		assert_false(source.contains("obj_proj_shadow_coverage"),
				"%s carries no PROJSHAD coverage sampler" % label)
		assert_false(source.contains("OBJ_PROJSHAD_"),
				"%s carries no PROJSHAD define" % label)
		assert_false(source.contains("opennova_slot_shadow_capture"),
				"%s reintroduced the per-fragment capture-eye gate" % label)
	var wrappers: Array = []
	_collect(OBJECT_ROOT, PackedStringArray([".gdshader"]), wrappers)
	assert_gt(wrappers.size(), 100)
	for path in wrappers:
		assert_false(_read(path).contains("OBJ_PROJSHAD_"),
				"%s carries no PROJSHAD define" % String(path).get_file())
	var water_header := _read(_repo_path("godot/src/env/water.h"))
	assert_false(water_header.contains("VISUAL_LAYER_SLOT_CAPTURE_MASK"),
			"Water reserves no capture layers")

	var slot_shadow := _read(_repo_path("godot/src/env/slot_shadow.cpp"))
	assert_false(slot_shadow.contains("set_cull_mask("),
			"SlotShadow aims no camera at capture layers")
	assert_false(slot_shadow.contains("SubViewport"),
			"SlotShadow owns no capture viewport chain")
	assert_true(slot_shadow.contains("SlotCaptureRequest"),
			"SlotShadow publishes typed capture requests")
	var adapter := _read(_repo_path("godot/src/render/slot_capture_adapter.cpp"))
	for token in ["kSlotCaptureClearArgb",
			"RenderingDevice::TEXTURE_SAMPLES_4",
			"texture_resolve_multisample(",
			"EFFECT_CALLBACK_TYPE_PRE_OPAQUE",
			"object_projected_shadow_coverage(",
			"object_projected_shadow_policy(",
			"frag_color = vec4(0.0, 0.0, 0.0, alpha);",
			"BLEND_FACTOR_SRC_ALPHA", "BLEND_FACTOR_ONE_MINUS_SRC_ALPHA"]:
		assert_true(adapter.contains(token), token)


# --- transitive-source golden -------------------------------------------


# The golden's payload: one transitive-source hash per manifest wrapper.
# tests/tools/shader_hashes_regen.gd writes it back after a witnessed change.
func _hash_payload() -> Dictionary:
	var manifest := _manifest()
	var hashes := {}
	for path in _expected_shader_paths(manifest):
		hashes["godot/" + String(path).trim_prefix("res://")] = _transitive_source_hash(path)
	return {
		"schema": 1,
		"algorithm": "sha256-normalized-transitive-include-closure-v1",
		"resource_count": hashes.size(),
		"resources": hashes,
	}


func test_transitive_shader_sources_match_golden() -> void:
	# Keep the old composed-source regression sensitivity after static
	# splitting. Deliberate witnessed shader changes regenerate the golden with
	# tests/tools/shader_hashes_regen.gd (run alone; it is deliberately red so
	# a regen run is never mistaken for a green validation) and must review the
	# resulting diff.
	var payload := _hash_payload()
	var hashes: Dictionary = payload["resources"]

	var golden: Dictionary = _load_json(HASH_GOLDEN_PATH)
	assert_eq(int(golden["schema"]), 1)
	assert_eq(String(golden["algorithm"]), String(payload["algorithm"]))
	assert_eq(int(golden["resource_count"]), hashes.size())
	var golden_resources: Dictionary = golden["resources"]
	assert_eq(_sorted_keys(golden_resources), _sorted_keys(hashes), "the golden covers exactly the manifest wrappers")
	for key in hashes:
		assert_eq(String(golden_resources.get(key, "")), String(hashes[key]),
				"%s transitive source hash (regenerate with tests/tools/shader_hashes_regen.gd after a witnessed change)" % key)
