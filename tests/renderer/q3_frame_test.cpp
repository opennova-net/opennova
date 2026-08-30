#include <runtime/renderer/q3_frame.h>

#include <runtime/renderer/material_classify.h>
#include <runtime/renderer/render_order.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
	do {                                                                        \
		if (!(condition)) {                                                      \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
			++failures;                                                           \
		}                                                                       \
	} while (0)

using namespace opennova::renderer;

Q3ResourceLease resource(std::uint64_t id) {
	return {id, 7};
}

Q3Matrix4 translated(float x, float y, float z) {
	Q3Matrix4 result{};
	result.values[12] = x;
	result.values[13] = y;
	result.values[14] = z;
	return result;
}

Q3SubmissionSnapshot object_submission(std::uint64_t id,
		const char *shader, float view_depth, std::size_t first_transform,
		std::size_t transform_count) {
	Q3SubmissionSnapshot submission{};
	submission.submission_id = id;
	submission.source = Q3Source::Object;
	submission.geometry_kind = transform_count == 1 ?
			Q3GeometryKind::Rigid : Q3GeometryKind::StaticInstances;
	submission.geometry = resource(100 + id);
	submission.material = resource(200 + id);
	submission.surface_index = static_cast<std::uint32_t>(id);
	submission.view_depth = view_depth;
	submission.first_transform = first_transform;
	submission.transform_count = transform_count;
	submission.object.classification = classify_object_material(
			shader, 0, 2, shader == std::string_view("FFP_GLASS") ? 1 : 0, 0);
	submission.object.base_texture = resource(300 + id);
	return submission;
}

Q3SubmissionSnapshot explicit_submission(std::uint64_t id, Q3Source source,
		std::size_t first_transform) {
	Q3SubmissionSnapshot submission{};
	submission.submission_id = id;
	submission.source = source;
	submission.geometry_kind = Q3GeometryKind::Rigid;
	submission.geometry = resource(100 + id);
	submission.material = resource(200 + id);
	submission.surface_index = static_cast<std::uint32_t>(id);
	submission.view_depth = 64.0f;
	submission.first_transform = first_transform;
	submission.transform_count = 1;
	if (source == Q3Source::Water) {
		submission.water.noise_color_texture = resource(400 + id);
		submission.water.noise_normal_texture = resource(500 + id);
	} else {
		submission.celestial.diffuse_texture = resource(600 + id);
	}
	return submission;
}

void check_technique_derivation_and_ordering() {
	Q3FrameSnapshot snapshot{};
	snapshot.frame_id = 42;
	snapshot.scene_generation = 9;
	for (int i = 0; i < 8; ++i)
		snapshot.transforms.push_back(translated(static_cast<float>(i), 0.0f, 0.0f));

	// Retail's Q3 object queue sorts back-to-front, then FrameFX redraws water,
	// celestial bodies, and the sun glow in that fixed bracket.
	snapshot.submissions.push_back(object_submission(1, "FF_ST_OP_LUM", 10.0f, 0, 2));
	snapshot.submissions.push_back(explicit_submission(3, Q3Source::Water, 3));
	snapshot.submissions.push_back(object_submission(2, "FFP_GLASS", 100.0f, 2, 1));
	snapshot.submissions.push_back(explicit_submission(4, Q3Source::CelestialBody, 4));
	snapshot.submissions.push_back(explicit_submission(5, Q3Source::SunGlow, 5));

	Q3FrameCompiler compiler;
	const Q3DrawList &draw = compiler.compile(snapshot);
	CHECK(draw.frame_id == 42);
	CHECK(draw.scene_generation == 9);
	CHECK(draw.commands.size() == 5);
	CHECK(draw.commands[0].submission_id == 2);
	CHECK(draw.commands[0].technique == Q3Technique::RotatedSpecularGlass);
	CHECK(draw.commands[0].sort_key == transparent_sort_key(100.0f));
	CHECK(draw.commands[1].submission_id == 1);
	CHECK(draw.commands[1].technique == Q3Technique::NormalCopy);
	CHECK(draw.commands[1].sort_key == transparent_sort_key(10.0f));
	CHECK(draw.commands[2].technique == Q3Technique::WaterNightVision);
	CHECK(draw.commands[3].technique == Q3Technique::CelestialBody);
	CHECK(draw.commands[4].technique == Q3Technique::SunGlow);

	CHECK(draw.commands[0].first_transform == 0);
	CHECK(draw.commands[0].transform_count == 1);
	CHECK(draw.transforms[0].values[12] == 2.0f);
	CHECK(draw.commands[1].first_transform == 1);
	CHECK(draw.commands[1].transform_count == 2);
	CHECK(draw.transforms[1].values[12] == 0.0f);
	CHECK(draw.transforms[2].values[12] == 1.0f);
	CHECK(draw.debug.emitted_by_technique[
			static_cast<std::size_t>(Q3Technique::NormalCopy)] == 1);
	CHECK(draw.debug.emitted_by_technique[
			static_cast<std::size_t>(Q3Technique::RotatedSpecularGlass)] == 1);
	CHECK(draw.rejected.empty());

	// The compiler owns an immutable copy until its next compile.
	snapshot.transforms[2].values[12] = 999.0f;
	snapshot.submissions[2].material.generation = 99;
	CHECK(draw.transforms[0].values[12] == 2.0f);
	CHECK(draw.commands[0].material.generation == 7);
}

void check_skinned_ranges_are_copied() {
	Q3FrameSnapshot snapshot{};
	snapshot.transforms.push_back(translated(7.0f, 8.0f, 9.0f));
	snapshot.bone_palette.push_back(translated(1.0f, 2.0f, 3.0f));
	snapshot.bone_palette.push_back(translated(4.0f, 5.0f, 6.0f));

	Q3SubmissionSnapshot submission = object_submission(20,
			"FF_ST_OP_LUM", 20.0f, 0, 1);
	submission.geometry_kind = Q3GeometryKind::Skinned;
	submission.first_bone = 0;
	submission.bone_count = 2;
	snapshot.submissions.push_back(submission);

	Q3FrameCompiler compiler;
	const Q3DrawList &draw = compiler.compile(snapshot);
	CHECK(draw.commands.size() == 1);
	CHECK(draw.commands[0].geometry_kind == Q3GeometryKind::Skinned);
	CHECK(draw.commands[0].first_bone == 0);
	CHECK(draw.commands[0].bone_count == 2);
	CHECK(draw.bone_palette.size() == 2);
	CHECK(draw.bone_palette[1].values[13] == 5.0f);
}

void check_fail_closed_rejections() {
	Q3FrameSnapshot snapshot{};
	snapshot.transforms.push_back(Q3Matrix4{});

	auto normal = object_submission(10, "FF_ST_OP", 10.0f, 0, 1);
	snapshot.submissions.push_back(normal);

	auto suppressed = object_submission(11, "FF_ST_OP_LUM", 10.0f, 0, 1);
	suppressed.submit_flags = kSubmitNoGlowCopy;
	snapshot.submissions.push_back(suppressed);

	auto stale = object_submission(12, "FF_ST_OP_LUM", 10.0f, 0, 1);
	stale.geometry.generation = 0;
	snapshot.submissions.push_back(stale);

	auto overrun = object_submission(13, "FF_ST_OP_LUM", 10.0f, 1, 1);
	snapshot.submissions.push_back(overrun);

	auto bad_depth = object_submission(14, "FF_ST_OP_LUM",
			std::numeric_limits<float>::quiet_NaN(), 0, 1);
	snapshot.submissions.push_back(bad_depth);

	auto bad_bones = object_submission(16, "FF_ST_OP_LUM", 10.0f, 0, 1);
	bad_bones.geometry_kind = Q3GeometryKind::Skinned;
	bad_bones.bone_count = 1;
	snapshot.submissions.push_back(bad_bones);

	auto corona = explicit_submission(15, Q3Source::LightCorona, 0);
	snapshot.submissions.push_back(corona);

	Q3FrameCompiler compiler;
	const Q3DrawList &draw = compiler.compile(snapshot);
	CHECK(draw.commands.empty());
	CHECK(draw.rejected.size() == 7);
	CHECK(draw.rejected[0].reason == Q3RejectReason::UnsupportedObjectMaterial);
	CHECK(draw.rejected[1].reason == Q3RejectReason::GlowCopySuppressed);
	CHECK(draw.rejected[2].reason == Q3RejectReason::InvalidResourceLease);
	CHECK(draw.rejected[3].reason == Q3RejectReason::InvalidTransformRange);
	CHECK(draw.rejected[4].reason == Q3RejectReason::NonFiniteInput);
	CHECK(draw.rejected[5].reason == Q3RejectReason::InvalidBoneRange);
	CHECK(draw.rejected[6].reason == Q3RejectReason::UnsupportedSource);
	CHECK(draw.debug.rejected_submissions == 7);
	CHECK(draw.debug.unsupported_light_coronas == 1);
}

void check_invisible_and_invalid_texture_paths() {
	Q3FrameSnapshot snapshot{};
	snapshot.transforms.push_back(Q3Matrix4{});

	auto hidden = object_submission(30, "FF_ST_OP_LUM", 5.0f, 0, 1);
	hidden.visible = false;
	snapshot.submissions.push_back(hidden);

	auto no_base = object_submission(31, "FF_ST_OP_LUM", 5.0f, 0, 1);
	no_base.object.base_texture = {};
	snapshot.submissions.push_back(no_base);

	auto water = explicit_submission(32, Q3Source::Water, 0);
	water.water.noise_normal_texture = {};
	snapshot.submissions.push_back(water);

	auto no_disc = explicit_submission(33, Q3Source::CelestialBody, 0);
	no_disc.celestial.diffuse_texture = {};
	snapshot.submissions.push_back(no_disc);

	Q3FrameCompiler compiler;
	const Q3DrawList &draw = compiler.compile(snapshot);
	CHECK(draw.commands.empty());
	CHECK(draw.rejected.size() == 3);
	CHECK(draw.debug.invisible_submissions == 1);
	CHECK(draw.debug.invalid_resource_submissions == 3);
}

void check_object_blend_and_coverage_contracts() {
	Q3FrameSnapshot snapshot{};
	snapshot.transforms.resize(3);

	auto additive = object_submission(40, "FF_ST_AD_LUM", 30.0f, 0, 1);
	CHECK(additive.object.classification.blend == ObjectBlendMode::Additive);
	// The SELFLUM re-shade's RGB modulator rides the snapshot unchanged.
	additive.object.self_lum_color = {0.25f, 0.5f, 0.75f, 1.0f};
	snapshot.submissions.push_back(additive);

	auto textureless_glass = object_submission(41, "FFP_GLASS", 20.0f, 1, 1);
	textureless_glass.object.base_texture = {};
	textureless_glass.object.alpha_mod = 0.25f;
	textureless_glass.object.classification.alpha_test = true;
	textureless_glass.object.classification.alpha_test_invert = true;
	textureless_glass.object.classification.alpha_test_value = 0.75f;
	textureless_glass.object.classification.is_two_sided = false;
	snapshot.submissions.push_back(textureless_glass);

	auto unwitnessed_multiplicative = object_submission(42,
			"FF_ST_OP_LUM", 10.0f, 2, 1);
	unwitnessed_multiplicative.object.classification.blend =
			ObjectBlendMode::Multiplicative;
	snapshot.submissions.push_back(unwitnessed_multiplicative);

	Q3FrameCompiler compiler;
	const Q3DrawList &draw = compiler.compile(snapshot);
	CHECK(draw.commands.size() == 2);
	CHECK(draw.commands[0].submission_id == 40);
	CHECK(draw.commands[0].object.classification.blend ==
			ObjectBlendMode::Additive);
	CHECK(draw.commands[0].object.self_lum_color.x == 0.25f);
	CHECK(draw.commands[0].object.self_lum_color.z == 0.75f);
	CHECK(draw.commands[1].submission_id == 41);
	CHECK(draw.commands[1].technique == Q3Technique::RotatedSpecularGlass);
	CHECK(!draw.commands[1].object.base_texture.valid());
	CHECK(draw.commands[1].object.alpha_mod == 0.25f);
	CHECK(draw.commands[1].object.classification.alpha_test_invert);
	CHECK(!draw.commands[1].object.classification.is_two_sided);
	CHECK(draw.rejected.size() == 1);
	CHECK(draw.rejected[0].submission_id == 42);
	CHECK(draw.rejected[0].reason == Q3RejectReason::UnsupportedObjectMaterial);
}

void check_multitexture_detail_contract() {
	Q3FrameSnapshot snapshot{};
	snapshot.transforms.resize(2);

	auto complete = object_submission(50, "FF_MT_OP_LUM", 20.0f, 0, 1);
	CHECK(complete.object.classification.has_detail);
	complete.object.detail_texture = resource(750);
	snapshot.submissions.push_back(complete);

	auto missing_detail = object_submission(51, "FF_MT_OP_LUM", 10.0f, 1, 1);
	CHECK(missing_detail.object.classification.has_detail);
	snapshot.submissions.push_back(missing_detail);

	Q3FrameCompiler compiler;
	const Q3DrawList &draw = compiler.compile(snapshot);
	CHECK(draw.commands.size() == 1);
	CHECK(draw.commands[0].submission_id == 50);
	CHECK(draw.commands[0].object.detail_texture.valid());
	CHECK(draw.rejected.size() == 1);
	CHECK(draw.rejected[0].submission_id == 51);
	CHECK(draw.rejected[0].reason == Q3RejectReason::InvalidResourceLease);
}

void check_stale_geometry_leases_are_rejected() {
	// The adapter publishes the current generation of every geometry entry it
	// leased; a submission still holding an older (re-packed or evicted)
	// generation must never reach the draw list. Unpublished resources are not
	// generation-checked, so identity-only material/texture leases still pass.
	Q3FrameSnapshot snapshot{};
	snapshot.transforms.resize(3);

	auto current = object_submission(60, "FF_ST_OP_LUM", 10.0f, 0, 1);
	current.geometry = {900, 4};
	snapshot.submissions.push_back(current);

	auto stale = object_submission(61, "FF_ST_OP_LUM", 20.0f, 1, 1);
	stale.geometry = {901, 2};
	snapshot.submissions.push_back(stale);

	auto unpublished = explicit_submission(62, Q3Source::Water, 2);
	unpublished.geometry = {902, 1};
	snapshot.submissions.push_back(unpublished);

	snapshot.resource_generations.push_back({900, 4});
	snapshot.resource_generations.push_back({901, 3});

	Q3FrameCompiler compiler;
	const Q3DrawList &draw = compiler.compile(snapshot);
	CHECK(draw.commands.size() == 2);
	CHECK(draw.commands[0].submission_id == 60);
	CHECK(draw.commands[0].geometry.generation == 4);
	CHECK(draw.commands[1].submission_id == 62);
	CHECK(draw.rejected.size() == 1);
	CHECK(draw.rejected[0].submission_id == 61);
	CHECK(draw.rejected[0].reason == Q3RejectReason::StaleResourceLease);
	CHECK(draw.debug.stale_lease_submissions == 1);
	CHECK(draw.debug.invalid_resource_submissions == 0);

	// The owner re-packs: the same lease is now stale and the frame drops it.
	snapshot.resource_generations[0].generation = 5;
	const Q3DrawList &next = compiler.compile(snapshot);
	CHECK(next.commands.size() == 1);
	CHECK(next.commands[0].submission_id == 62);
	CHECK(next.debug.stale_lease_submissions == 2);
}

void check_shading_constants_are_engine_homed() {
	// The device adapter splices these into its GLSL; the witnessed values
	// live here [orig: Glass.fx TGlassFFP TECHNIQUE_GLOW;
	// Render_FillStaticCubemaps @ 0x58f290; Water_PSBumpReflectNV source
	// @ 0x7dbd28].
	CHECK(kQ3GlassWhiteLobeGain == 1.4f);
	CHECK(kQ3GlassWhiteLobePower == 800.0f);
	CHECK(kQ3GlassWarmLobeColor[0] == 1.0f);
	CHECK(kQ3GlassWarmLobeColor[1] == 248.0f / 255.0f);
	CHECK(kQ3GlassWarmLobeColor[2] == 240.0f / 255.0f);
	CHECK(kQ3GlassWarmLobePower == 40.0f);
	CHECK(kQ3WaterNvLumaWeights[0] == 0.25f);
	CHECK(kQ3WaterNvLumaWeights[1] == 0.60f);
	CHECK(kQ3WaterNvLumaWeights[2] == 0.15f);
	CHECK(kQ3WaterNvBrightBias == 0.15f);
	// The bloom pass's far-band viewport for the discs and the sun glow
	// [orig: Render_SetViewportFarDepth @ 0x58a840].
	CHECK(kQ3FarBandMinZ == 0.98000002f);
	CHECK(kQ3FarBandMaxZ == 0.99996948f);
	CHECK(kQ3FarBandMinZ < kQ3FarBandMaxZ && kQ3FarBandMaxZ < 1.0f);
}

} // namespace

int main() {
	check_shading_constants_are_engine_homed();
	check_technique_derivation_and_ordering();
	check_skinned_ranges_are_copied();
	check_fail_closed_rejections();
	check_invisible_and_invalid_texture_paths();
	check_object_blend_and_coverage_contracts();
	check_multitexture_detail_contract();
	check_stale_geometry_leases_are_rejected();

	if (failures != 0) {
		std::printf("renderer_q3_frame: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("renderer_q3_frame ok\n");
	return 0;
}
