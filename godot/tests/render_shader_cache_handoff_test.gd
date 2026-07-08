extends GutTest

# REN-1 T1's thin GUT leg (ADR 0023): pins the GDScript -> native handoff of
# the material classification chain. The exhaustive input-matrix pinning is
# the renderer_state_vectors ctest (tests/renderer/state_vectors_test.cpp);
# this leg only proves the NovaObjectShaderCache binding reaches the same
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
	# 0x5410 -> 0x1410 at REN-4: the OED dump's GLASS bit on the SkB*T rows was
	# table drift — the runtime capability probe never sets it (no ReflectColor
	# reference) [orig: HLSLEffect_LoadFromFile probe @ 0x5ae690; D-RMAT-4].
	"VS_SKBUMPDIFFT2/base": [["VS_SKBUMPDIFFT2", 0x00, 0, 0, 128], 0x00001410],
	"VS_FLAG/base": [["VS_FLAG", 0x00, 0, 0, 128], 0x0000000c],
	"FF_ST_OP_LUM/em2": [["FF_ST_OP_LUM", 0x00, 2, 0, 128], 0x00000304],
	"FF_ST_OP/all-flags": [["FF_ST_OP", 0x07, 0, 0, 200], 0x000000e4],
	"VS_LEAVESWIND/unknown": [["VS_LEAVESWIND", 0x00, 0, 0, 128], 0x00000000],
}


func test_classify_binding_matches_golden_keys() -> void:
	var cache = NovaObjectShaderCache.get_singleton()
	assert_not_null(cache, "shader cache singleton")
	for label in EXPECTED_KEYS:
		var row: Array = EXPECTED_KEYS[label]
		var inputs: Array = row[0]
		var key: int = cache.classify(inputs[0], inputs[1], inputs[2], inputs[3], inputs[4])
		assert_eq(key, int(row[1]), "classify key for %s" % label)


func test_known_shader_tag_table_reaches_gdscript() -> void:
	var cache = NovaObjectShaderCache.get_singleton()
	var tags: PackedStringArray = cache.get_known_shader_tags()
	# 46 = OED's 45-entry gMaterialInfoTable + VS_TRACER, matching the runtime
	# registry retail builds at boot (REN-2; docs/render/render-material-re.md).
	assert_eq(tags.size(), 46, "table mirrors the runtime registry (45 OED + VS_TRACER)")
	assert_true("FF_ST_OP" in tags, "table carries FF_ST_OP")
	assert_true("FFP_GLASS" in tags, "table carries FFP_GLASS")
	assert_true("VS_TRACER" in tags, "table carries the runtime-only VS_TRACER row")
	assert_false("VS_LEAVESWIND" in tags, "unshipped tags stay out of the table")


func test_shader_for_key_composes_real_shaders() -> void:
	var cache = NovaObjectShaderCache.get_singleton()
	var ff_key: int = cache.classify("FF_ST_OP", 0, 0, 0, 128)
	var shader: Shader = cache.get_shader_for_key(ff_key)
	assert_not_null(shader, "FF shader composes")
	assert_true(shader.code.contains("shader_type spatial"), "FF shader is spatial")
	assert_true(shader.code.contains("obj_ff_lighting"), "FF shader lights via ff helper")

	var glass_key: int = cache.classify("FFP_GLASS", 0, 0, 1, 128)
	var glass: Shader = cache.get_shader_for_key(glass_key)
	assert_true(glass.code.contains("blend_add"), "glass shader is additive")

	var same: Shader = cache.get_shader_for_key(ff_key)
	assert_eq(shader, same, "cache returns the same Shader per key")
