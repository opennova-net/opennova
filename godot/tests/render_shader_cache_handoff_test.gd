extends GutTest

# REN-1 T1's thin GUT leg (ADR 0023): pins the GDScript -> native handoff of
# the material classification chain. The exhaustive input-matrix pinning is
# the renderer_state_vectors ctest (tests/renderer/state_vectors_test.cpp);
# this leg only proves the ObjectShaderCache binding reaches the same
# chain and hands back real shaders.
#
# EXPECTED_KEYS was dumped ONCE from the committed golden
# (tests/renderer/render_state_vectors.golden). The re-dump discipline is the
# ctest's: a change here is a producer change and carries its witness citation.

# [tag, material_flags, emissive_type, is_glass_flag, alpha_byte] -> key
const EXPECTED_KEYS := {
	"FF_ST_OP/base": [["FF_ST_OP", 0x00, 0, 0, 128], 0x00000004],
	"FF_ST_AB/base": [["FF_ST_AB", 0x00, 0, 0, 128], 0x00000005],
	"FF_MT_OP/base": [["FF_MT_OP", 0x00, 0, 0, 128], 0x00001004],
	"FFP_GLASS/base": [["FFP_GLASS", 0x00, 0, 0, 128], 0x0000401a],
	"VS_PHONGT/base": [["VS_PHONGT", 0x00, 0, 0, 128], 0x00002408],
	"VS_DOT3DIFFOBJ/base": [["VS_DOT3DIFFOBJ", 0x00, 0, 0, 128], 0x00000c08],
	"VS_DOT3DIFF2/base": [["VS_DOT3DIFF2", 0x00, 0, 0, 128], 0x00001410],
	# 0x5410 -> 0x21410 at REN-4 + this audit: the old dump's GLASS bit on the
	# SkB*T rows was table drift (no ReflectColor reference), while the witnessed
	# skinned vertex topology now has an explicit compile-time capability bit.
	# [orig: HLSLEffect_LoadFromFile probe @ 0x5ae690; D-RMAT-4].
	"VS_SKBUMPDIFFT2/base": [["VS_SKBUMPDIFFT2", 0x00, 0, 0, 128], 0x00021410],
	"VS_FLAG/base": [["VS_FLAG", 0x00, 0, 0, 128], 0x0000000c],
	"FF_ST_OP_LUM/em2": [["FF_ST_OP_LUM", 0x00, 2, 0, 128], 0x00000304],
	"FF_ST_OP/all-flags": [["FF_ST_OP", 0x07, 0, 0, 200], 0x000000e4],
	"VS_LEAVESWIND/unknown": [["VS_LEAVESWIND", 0x00, 0, 0, 128], 0x00000000],
}


func test_classify_binding_matches_golden_keys() -> void:
	var cache = ObjectShaderCache.get_singleton()
	assert_not_null(cache, "shader cache singleton")
	for label in EXPECTED_KEYS:
		var row: Array = EXPECTED_KEYS[label]
		var inputs: Array = row[0]
		var key: int = cache.classify(inputs[0], inputs[1], inputs[2], inputs[3], inputs[4])
		assert_eq(key, int(row[1]), "classify key for %s" % label)


func test_known_shader_tag_table_reaches_gdscript() -> void:
	var cache = ObjectShaderCache.get_singleton()
	var tags: PackedStringArray = cache.get_known_shader_tags()
	# 46 entries, including VS_TRACER, match the runtime registry retail builds
	# at boot (REN-2; docs/render/render-material-re.md).
	assert_eq(tags.size(), 46, "table mirrors the runtime registry")
	assert_true("FF_ST_OP" in tags, "table carries FF_ST_OP")
	assert_true("FFP_GLASS" in tags, "table carries FFP_GLASS")
	assert_true("VS_TRACER" in tags, "table carries the runtime-only VS_TRACER row")
	assert_false("VS_LEAVESWIND" in tags, "unshipped tags stay out of the table")


func test_water_plane_handoff_mirrors_the_transparent_ladder_underwater() -> void:
	var cache := ObjectShaderCache.get_singleton()
	cache.set_water_plane(4.0, true)
	assert_eq(cache.alpha_rung_for_height(3.0),
			ObjectShaderCache.RENDER_RUNG_ALPHA_FAR_SIDE)
	assert_eq(cache.alpha_rung_for_height(5.0),
			ObjectShaderCache.RENDER_RUNG_ALPHA_CAMERA_SIDE)
	cache.set_water_plane(4.0, false)
	assert_eq(cache.alpha_rung_for_height(3.0),
			ObjectShaderCache.RENDER_RUNG_ALPHA_CAMERA_SIDE)
	assert_eq(cache.alpha_rung_for_height(5.0),
			ObjectShaderCache.RENDER_RUNG_ALPHA_FAR_SIDE)
	cache.clear_water_plane()


func test_shader_for_key_selects_checked_in_resources() -> void:
	var cache = ObjectShaderCache.get_singleton()
	var ff_key: int = cache.classify("FF_ST_OP", 0, 0, 0, 128)
	var shader: Shader = cache.get_shader_for_key(ff_key)
	assert_not_null(shader, "FF shader resource loads")
	assert_eq(shader.resource_path, "res://shaders/object/fixed/opaque.gdshader",
			"opaque FF selects the checked-in opaque resource")

	var glass_key: int = cache.classify("FFP_GLASS", 0, 0, 1, 128)
	var glass: Shader = cache.get_shader_for_key(glass_key)
	assert_eq(glass.resource_path, "res://shaders/object/glass/additive.gdshader",
			"glass selects the additive resource")

	var same: Shader = cache.get_shader_for_key(ff_key)
	assert_eq(shader, same, "cache returns the same Shader per key")


func test_all_finite_family_and_render_policy_resources_load() -> void:
	var manifest: Dictionary = JSON.parse_string(FileAccess.get_file_as_string(
			"res://shaders/object/pipeline_manifest.json"))
	assert_eq(int(manifest.get("schema", 0)), 1, "known pipeline manifest schema")
	var count := 0
	for technique in manifest.get("techniques", []):
		for policy in technique.get("policies", []):
			for suffix in ["", "_double_sided"]:
				var path: String = "res://shaders/object/%s/%s%s.gdshader" % [
					technique.get("directory", ""), policy, suffix]
				assert_true(FileAccess.file_exists(path), "%s is checked in" % path)
				assert_not_null(ResourceLoader.load(path, "Shader"),
						"%s imports as Shader" % path)
				count += 1
	assert_eq(count, int(manifest.get("resource_count", -1)),
			"manifest covers the bounded resource matrix")


func test_configure_material_selects_compile_time_techniques() -> void:
	var cache = ObjectShaderCache.get_singleton()

	var detail := ShaderMaterial.new()
	cache.configure_material_for_key(
			detail, cache.classify("VS_SKBUMPDIFFT2", 0, 0, 0, 128))
	assert_eq(detail.shader.resource_path,
			"res://shaders/object/dot3_tangent_detail_skinned/opaque.gdshader",
			"skinned detail DOT3 keeps its fixed-function encoded-vector topology")
	var no_detail_key: int = cache.classify("VS_SKBUMPDIFFT2", 0, 0, 0, 128)
	no_detail_key &= ~ObjectShaderCache.CAP_DETAIL
	var no_detail := ShaderMaterial.new()
	cache.configure_material_for_key(no_detail, no_detail_key)
	assert_eq(no_detail.shader.resource_path,
			"res://shaders/object/dot3_tangent_skinned/opaque.gdshader",
			"missing detail texture selects the compiled single-stage downgrade")

	var skinned_basic := ShaderMaterial.new()
	cache.configure_material_for_key(
			skinned_basic, cache.classify("VS_SKBASIC", 0, 0, 0, 128))
	assert_eq(skinned_basic.shader.resource_path,
			"res://shaders/object/fixed_skinned/opaque.gdshader",
			"SkBasic stays distinct from _FFP AlphaGen/SELFLUM topology")

	var phong := ShaderMaterial.new()
	cache.configure_material_for_key(phong, cache.classify("VS_PHONGT", 0, 0, 0, 128))
	assert_eq(phong.shader.resource_path,
			"res://shaders/object/phong_tangent_specular/opaque.gdshader",
			"Phong tangent/specular topology is compiled into its resource")
	var object_normal := ShaderMaterial.new()
	cache.configure_material_for_key(
			object_normal, cache.classify("VS_DOT3DIFFOBJ", 0, 0, 0, 128))
	assert_eq(object_normal.shader.resource_path,
			"res://shaders/object/phong_object_diffuse/opaque.gdshader",
			"object-space normal topology is a distinct resource")
	var skinned_tangent_phong := ShaderMaterial.new()
	cache.configure_material_for_key(skinned_tangent_phong,
			cache.classify("VS_SKBUMPPHONGT", 0, 0, 0, 128))
	assert_eq(skinned_tangent_phong.shader.resource_path,
			"res://shaders/object/phong_tangent_specular_skinned/opaque.gdshader",
			"skinned tangent Phong keeps its authored directional topology")
	var phong_map := ShaderMaterial.new()
	cache.configure_material_for_key(phong_map,
			cache.classify("VS_SKBUMPPHONGOBJ", 0, 0, 0, 128))
	assert_eq(phong_map.shader.resource_path,
			"res://shaders/object/phong_object_specular_phong_map/opaque.gdshader",
			"skinned object Phong selects the Diffuse1-alpha PhongMap path")

	var environment := ShaderMaterial.new()
	cache.configure_material_for_key(
			environment, cache.classify("VS_ENVPHONGT", 0, 0, 0, 128))
	assert_eq(environment.shader.resource_path,
			"res://shaders/object/environment_tangent_specular/opaque.gdshader",
			"environment plus authored Phong passes are compiled into the technique")
	var mirror := ShaderMaterial.new()
	cache.configure_material_for_key(
			mirror, cache.classify("VS_BUMPMIRRT", 0, 0, 0, 128))
	assert_eq(mirror.shader.resource_path,
			"res://shaders/object/environment_tangent/opaque.gdshader",
			"untextured mirror selects its exact reflection combine")
	var textured_mirror := ShaderMaterial.new()
	cache.configure_material_for_key(
			textured_mirror, cache.classify("VS_BMTXMIRRT", 0, 0, 0, 128))
	assert_eq(textured_mirror.shader.resource_path,
			"res://shaders/object/environment_tangent_textured/opaque.gdshader",
			"textured mirror preserves the authored Diffuse1 post-multiply")
	var skinned_glass := ShaderMaterial.new()
	cache.configure_material_for_key(
			skinned_glass, cache.classify("VS_SKGLASS", 0, 0, 0, 128))
	assert_eq(skinned_glass.shader.resource_path,
			"res://shaders/object/glass_skinned/additive.gdshader",
			"skinned glass selects its untextured vertex-color path")
	var emissive_flag := ShaderMaterial.new()
	cache.configure_material_for_key(
			emissive_flag, cache.classify("VS_FLAG", 0, 2, 0, 128))
	assert_eq(emissive_flag.shader.resource_path,
			"res://shaders/object/flag/opaque.gdshader",
			"Flag.fx has no invented self-lit technique")

	var luminance := ShaderMaterial.new()
	cache.configure_material_for_key(
			luminance, cache.classify("FF_ST_OP_LUM", 0, 2, 0, 128))
	assert_eq(luminance.shader.resource_path,
			"res://shaders/object/self_lit/opaque.gdshader",
			"luminance/emissive selects a dedicated self-lit technique")

	var tracer := ShaderMaterial.new()
	cache.configure_material_for_key(tracer, cache.classify("VS_TRACER", 0, 0, 0, 128))
	assert_eq(tracer.shader.resource_path, "res://shaders/object/tracer/additive.gdshader",
			"tracer selects its branch-free technique resource")


func test_every_canonical_classification_resolves_without_fallback() -> void:
	var cache = ObjectShaderCache.get_singleton()
	for tag in cache.get_known_shader_tags():
		for flags in [0, ObjectShaderCache.MATERIAL_FLAG_ALPHA_TEST,
				ObjectShaderCache.MATERIAL_FLAG_TWO_SIDED,
				ObjectShaderCache.MATERIAL_FLAG_ALPHA_TEST |
						ObjectShaderCache.MATERIAL_FLAG_TWO_SIDED]:
			for emissive_type in [0, 2]:
				for glass_flag in [0, 1]:
					var key: int = cache.classify(
						tag, flags, emissive_type, glass_flag, 128)
					assert_not_null(cache.get_shader_for_key(key),
							"%s key %08x resolves exactly" % [tag, key])
