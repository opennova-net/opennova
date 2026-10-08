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

	auto corona = explicit_submission(15, Q3Source::LightCorona, 0);
	snapshot.submissions.push_back(corona);

	Q3FrameCompiler compiler;
	const Q3DrawList &draw = compiler.compile(snapshot);
	CHECK(draw.commands.empty());
	CHECK(draw.rejected.size() == 6);
	CHECK(draw.rejected[0].reason == Q3RejectReason::UnsupportedObjectMaterial);
	CHECK(draw.rejected[1].reason == Q3RejectReason::GlowCopySuppressed);
	CHECK(draw.rejected[2].reason == Q3RejectReason::InvalidResourceLease);
	CHECK(draw.rejected[3].reason == Q3RejectReason::InvalidTransformRange);
	CHECK(draw.rejected[4].reason == Q3RejectReason::NonFiniteInput);
	CHECK(draw.rejected[5].reason == Q3RejectReason::UnsupportedSource);
	CHECK(draw.debug.rejected_submissions == 6);
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
	// Render_FillStaticCubemaps @ 0x58f290; g_WaterPSBumpReflectNV source
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

void check_mip_ceilings_pack_both_stages() {
	// 256x256 pixel-built Diffuse1 ends at level 6, a DDS detail has no
	// ceiling; both survive one push float. [orig:
	// GTexture_CreateFromPixelData_0 @ 0x6877BC..0x6877D8]
	const float packed = q3_pack_mip_ceilings(6.0f, kQ3NoMipCeiling);
	CHECK(q3_unpack_mip_ceiling(packed, 0) == 6.0f);
	CHECK(q3_unpack_mip_ceiling(packed, 1) == kQ3NoMipCeiling);
	const float both = q3_pack_mip_ceilings(0.0f, 14.0f);
	CHECK(q3_unpack_mip_ceiling(both, 0) == 0.0f);
	CHECK(q3_unpack_mip_ceiling(both, 1) == 14.0f);
	CHECK(q3_unpack_mip_ceiling(q3_pack_mip_ceilings(-1.0f, 15.0f), 0) == kQ3NoMipCeiling);
	// The effect stage's filter code rides above both ceilings (D-RMAT-22).
	CHECK(q3_unpack_filter_code(packed) == 0);
	const float filtered = q3_pack_mip_ceilings(6.0f, kQ3NoMipCeiling, 2);
	CHECK(q3_unpack_filter_code(filtered) == 2);
	CHECK(q3_unpack_mip_ceiling(filtered, 0) == 6.0f);
	CHECK(q3_unpack_mip_ceiling(filtered, 1) == kQ3NoMipCeiling);
	CHECK(q3_unpack_filter_code(q3_pack_mip_ceilings(0.0f, 14.0f, 1)) == 1);
	const Q3ObjectMaterialParameters defaults{};
	CHECK(defaults.diffuse_max_lod == kQ3NoMipCeiling);
	CHECK(defaults.detail_max_lod == kQ3NoMipCeiling);
}

void check_emissive_copies_saturate_colour_times_gain() {
	// SELFLUM / glass emissive: sat(colour x gain) x 2, so a gain above 1
	// lifts a sub-1 colour (0.25 x 2 -> 1.0, 0.1 x 4 -> 0.8) instead of being
	// clipped alone. [orig: _FFP.fx SELFLUM; Glass.fx TGlassFFP;
	// Material_ApplyShaderParameters @ 0x58E050..0x58E06A]
	const std::array<float, 3> doubled =
			q3_emissive_modulate2x({0.25f, 0.1f, 0.6f, 1.0f}, {2.0f, 4.0f, 2.0f});
	CHECK(std::fabs(doubled[0] - 1.0f) < 1.0e-6f);
	CHECK(std::fabs(doubled[1] - 0.8f) < 1.0e-6f);
	CHECK(std::fabs(doubled[2] - 2.0f) < 1.0e-6f);
	const std::array<float, 3> dim =
			q3_emissive_modulate2x({0.5f, 0.5f, 0.5f, 1.0f}, {0.5f, 1.0f, 0.0f});
	CHECK(std::fabs(dim[0] - 0.5f) < 1.0e-6f);
	CHECK(std::fabs(dim[1] - 1.0f) < 1.0e-6f);
	CHECK(dim[2] == 0.0f);
	// The disc/glow bloom copies take the same emissive: at the 06:30 03TR
	// gain of 19/16 the glare's grey SelfLumColor is lifted, not clipped to
	// the gain-free colour (the earlier SelfLum x min(gain, 1) x 2).
	Q3CelestialMaterialParameters glare;
	glare.self_lum = {0.2f, 0.24f, 0.9f};
	const std::array<float, 3> glow =
			q3_celestial_emissive(glare, {1.1875f, 1.1875f, 1.1875f});
	CHECK(std::fabs(glow[0] - 0.2f * 1.1875f * 2.0f) < 1.0e-6f);
	CHECK(std::fabs(glow[1] - 0.24f * 1.1875f * 2.0f) < 1.0e-6f);
	CHECK(std::fabs(glow[2] - 2.0f) < 1.0e-6f);
}

// The bloom pass's disc and glow keep a fragment where retail's LESSEQUAL
// holds between the far band and the beauty depth written through the scene
// viewport [orig: Render_SetViewportFarDepth @ 0x58a840; Render_SetViewport
// @ 0x58a720]. The reverse-Z scale must reproduce that test for any beauty
// camera near plane sharing the scene far plane.
void check_far_band_matches_the_retail_depth_test() {
	CHECK(kQ3SceneViewportMaxZ == 0.99996948f);
	const double far_plane = 701.0; // scene_far_plane(700 u fog)
	const double retail_near = 0.2;  // g_ProjectionNearZ
	const auto retail_depth = [&](double x) {
		return far_plane / (far_plane - retail_near) * (1.0 - retail_near / x);
	};
	const auto retail_keeps = [&](double w, double d) {
		return kQ3FarBandMinZ + (double(kQ3FarBandMaxZ) - kQ3FarBandMinZ) *
						retail_depth(w) <=
				double(kQ3SceneViewportMaxZ) * retail_depth(d);
	};
	for (const double camera_near : {0.05, 0.2, 1.0}) {
		const auto reverse = [&](double x) {
			return camera_near * (far_plane - x) / (x * (far_plane - camera_near));
		};
		int disagreements = 0;
		for (double w = 30.0; w <= 64.0; w += 2.0) {
			for (double d = 1.0; d < far_plane; d += 1.5) {
				const double band = q3_far_band_reverse_z(float(reverse(w)));
				const bool kept = band >= reverse(d);
				// float rounding of the band may flip the pixel sitting on the
				// threshold itself; nothing else may differ.
				const double threshold = double(kQ3SceneViewportMaxZ) /
						(kQ3FarBandMinZ / far_plane +
								(double(kQ3FarBandMaxZ) - kQ3FarBandMinZ) / w);
				if (kept != retail_keeps(w, d) && std::fabs(d - threshold) > 1.0)
					++disagreements;
			}
		}
		CHECK(disagreements == 0);
	}
	// The 03TR 06:30 pose: a disc 60 u deep behind terrain 490 u out stays
	// hidden (retail's threshold is ~577 u), and shows over 650 u terrain and
	// cleared sky. The former [1 - MaxZ, 1 - MinZ] remap kept it from ~427 u.
	const auto reverse_godot = [&](double x) {
		return 0.05 * (far_plane - x) / (x * (far_plane - 0.05));
	};
	const double disc = q3_far_band_reverse_z(float(reverse_godot(60.0)));
	CHECK(!retail_keeps(60.0, 490.0) && disc < reverse_godot(490.0));
	CHECK(retail_keeps(60.0, 650.0) && disc >= reverse_godot(650.0));
	CHECK(disc >= 0.0);
}

// The first-person pass submits the gun with render flags 0x80 (a held
// weapon-slot reference) or 0, never kSubmitNoGlowCopy, so its glow-capable
// rigid strips join the world's copies in the one back-to-front object
// queue FrameFX flushes; only 0x100 (the sky pass's celestial submits)
// suppresses a copy. [orig: Player_RenderFirstPersonViewModel
// @ 0x4def5c..0x4def6a; Render_CollectRenderObjectsForBatch
// @ 0x5d93b5..0x5d9449]
void check_first_person_copies_share_the_object_queue() {
	CHECK(q3_object_source_admitted(false));
	CHECK(!q3_object_source_admitted(true));
	Q3FrameSnapshot snapshot{};
	snapshot.transforms.resize(4);

	snapshot.submissions.push_back(object_submission(70, "FF_ST_OP_LUM", 40.0f, 0, 1));
	auto armed_gun = object_submission(71, "FF_ST_OP_LUM", 0.6f, 1, 1);
	armed_gun.submit_flags = kSubmitAltStream;
	snapshot.submissions.push_back(armed_gun);
	auto scope_glass = object_submission(72, "FFP_GLASS", 0.4f, 2, 1);
	snapshot.submissions.push_back(scope_glass);
	auto celestial = object_submission(73, "FF_ST_AD_LUM", 500.0f, 3, 1);
	celestial.submit_flags = kSubmitNoGlowCopy | kSubmitAltStream;
	snapshot.submissions.push_back(celestial);

	Q3FrameCompiler compiler;
	const Q3DrawList &draw = compiler.compile(snapshot);
	CHECK(draw.commands.size() == 3);
	CHECK(draw.commands[0].submission_id == 70);
	CHECK(draw.commands[1].submission_id == 71);
	CHECK(draw.commands[1].technique == Q3Technique::NormalCopy);
	CHECK(draw.commands[1].sort_key == transparent_sort_key(0.6f));
	CHECK(draw.commands[2].submission_id == 72);
	CHECK(draw.commands[2].technique == Q3Technique::RotatedSpecularGlass);
	CHECK(draw.rejected.size() == 1);
	CHECK(draw.rejected[0].submission_id == 73);
	CHECK(draw.rejected[0].reason == Q3RejectReason::GlowCopySuppressed);
}

int main() {
	check_shading_constants_are_engine_homed();
	check_technique_derivation_and_ordering();
	check_fail_closed_rejections();
	check_invisible_and_invalid_texture_paths();
	check_object_blend_and_coverage_contracts();
	check_multitexture_detail_contract();
	check_stale_geometry_leases_are_rejected();
	check_emissive_copies_saturate_colour_times_gain();
	check_mip_ceilings_pack_both_stages();
	check_far_band_matches_the_retail_depth_test();
	check_first_person_copies_share_the_object_queue();

	if (failures != 0) {
		std::printf("renderer_q3_frame: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("renderer_q3_frame ok\n");
	return 0;
}
