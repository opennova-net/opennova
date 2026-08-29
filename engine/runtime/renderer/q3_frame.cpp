#include <runtime/renderer/q3_frame.h>

#include <runtime/renderer/render_order.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace opennova::renderer {
namespace {

// [orig: Q3 object flush and fixed follow-up draw bracket @ 0x582a54..0x582a80]

bool range_valid(std::size_t first, std::size_t count, std::size_t size) {
	return first <= size && count <= size - first;
}

bool finite(float value) {
	return std::isfinite(value);
}

bool finite(const Q3Vec2 &value) {
	return finite(value.x) && finite(value.y);
}

bool finite(const Q3Vec3 &value) {
	return finite(value.x) && finite(value.y) && finite(value.z);
}

bool finite(const Q3Vec4 &value) {
	return finite(value.x) && finite(value.y) && finite(value.z) &&
			finite(value.w);
}

template <std::size_t N>
bool finite(const std::array<float, N> &values) {
	for (float value : values) {
		if (!finite(value))
			return false;
	}
	return true;
}

bool finite(const Q3Matrix4 &matrix) {
	return finite(matrix.values);
}

bool parameters_finite(const Q3SubmissionSnapshot &submission) {
	if (!finite(submission.view_depth))
		return false;
	switch (submission.source) {
		case Q3Source::Object:
			return finite(submission.object.classification.alpha_test_value) &&
					finite(submission.object.self_lum_color) &&
					finite(submission.object.reflect_color) &&
					finite(submission.object.alpha_mod) &&
					finite(submission.object.uv_transform);
		case Q3Source::Water:
			return finite(submission.water.water_color) &&
					finite(submission.water.water_uv) &&
					finite(submission.water.reflection_uv_scale);
		case Q3Source::CelestialBody:
		case Q3Source::SunGlow:
			return finite(submission.celestial.tint) &&
					finite(submission.celestial.opacity) &&
					finite(submission.celestial.anchor_camera_world) &&
					finite(submission.celestial.glare_direction);
		case Q3Source::LightCorona:
			return false;
	}
	return false;
}

bool resource_leases_valid(const Q3SubmissionSnapshot &submission) {
	if (!submission.geometry.valid() || !submission.material.valid())
		return false;
	switch (submission.source) {
		case Q3Source::Object:
			// NORMAL-copy coverage and Glass coverage both use Diffuse1 where
			// their authored material provides it. Fixed-function multitexture
			// LUM rows also multiply the authored Detail/UV2 alpha; the
			// technique-specific check below requires both leases.
			return true;
		case Q3Source::Water:
			return submission.water.noise_color_texture.valid() &&
					submission.water.noise_normal_texture.valid() &&
					(!submission.water.has_reflection ||
							submission.water.reflection_texture.valid());
		case Q3Source::CelestialBody:
		case Q3Source::SunGlow:
			return submission.celestial.diffuse_texture.valid();
		case Q3Source::LightCorona:
			return false;
	}
	return false;
}

std::optional<Q3Technique> technique_for(
		const Q3SubmissionSnapshot &submission, Q3RejectReason &reason) {
	switch (submission.source) {
		case Q3Source::Object: {
			if ((submission.submit_flags & kSubmitNoGlowCopy) != 0) {
				reason = Q3RejectReason::GlowCopySuppressed;
				return std::nullopt;
			}
			const ObjectMaterialClassification &classification =
					submission.object.classification;
			if (!classification.known_shader ||
					!classification.is_glow_capable) {
				reason = Q3RejectReason::UnsupportedObjectMaterial;
				return std::nullopt;
			}
			if (classification.family == ObjectShaderFamily::Glass)
				return Q3Technique::RotatedSpecularGlass;
			if (classification.family == ObjectShaderFamily::FixedFunction &&
					classification.is_luminance &&
					classification.blend != ObjectBlendMode::Multiplicative)
				return Q3Technique::NormalCopy;
			reason = Q3RejectReason::UnsupportedObjectMaterial;
			return std::nullopt;
		}
		case Q3Source::Water:
			return Q3Technique::WaterNightVision;
		case Q3Source::CelestialBody:
			return Q3Technique::CelestialBody;
		case Q3Source::SunGlow:
			return Q3Technique::SunGlow;
		case Q3Source::LightCorona:
			reason = Q3RejectReason::UnsupportedSource;
			return std::nullopt;
	}
	reason = Q3RejectReason::UnsupportedSource;
	return std::nullopt;
}

int technique_stage(Q3Technique technique) {
	switch (technique) {
		case Q3Technique::NormalCopy:
		case Q3Technique::RotatedSpecularGlass:
			return 0;
		case Q3Technique::WaterNightVision:
			return 1;
		case Q3Technique::CelestialBody:
			return 2;
		case Q3Technique::SunGlow:
			return 3;
		case Q3Technique::Count:
			break;
	}
	return 4;
}

void reject(Q3DrawList &draw_list, std::size_t input_index,
		const Q3SubmissionSnapshot &submission, Q3RejectReason reason) {
	draw_list.rejected.push_back(
			{submission.submission_id, input_index, reason});
	++draw_list.debug.rejected_submissions;
	if (reason == Q3RejectReason::InvalidResourceLease)
		++draw_list.debug.invalid_resource_submissions;
	if (submission.source == Q3Source::LightCorona)
		++draw_list.debug.unsupported_light_coronas;
}

} // namespace

const Q3DrawList &Q3FrameCompiler::compile(const Q3FrameSnapshot &snapshot) {
	draw_list_.frame_id = snapshot.frame_id;
	draw_list_.scene_generation = snapshot.scene_generation;
	draw_list_.commands.clear();
	draw_list_.transforms.clear();
	draw_list_.bone_palette.clear();
	draw_list_.rejected.clear();
	draw_list_.debug = {};
	draw_list_.debug.input_submissions = snapshot.submissions.size();

	prepared_.clear();
	prepared_.reserve(snapshot.submissions.size());

	for (std::size_t i = 0; i < snapshot.submissions.size(); ++i) {
		const Q3SubmissionSnapshot &submission = snapshot.submissions[i];
		if (!submission.visible) {
			++draw_list_.debug.invisible_submissions;
			continue;
		}

		Q3RejectReason reason = Q3RejectReason::UnsupportedSource;
		const std::optional<Q3Technique> technique =
				technique_for(submission, reason);
		if (!technique.has_value()) {
			reject(draw_list_, i, submission, reason);
			continue;
		}
		if (!resource_leases_valid(submission) ||
				(*technique == Q3Technique::NormalCopy &&
						(!submission.object.base_texture.valid() ||
								(submission.object.classification.has_detail &&
										!submission.object.detail_texture.valid())))) {
			reject(draw_list_, i, submission,
					Q3RejectReason::InvalidResourceLease);
			continue;
		}
		const bool transform_shape_valid =
				(submission.geometry_kind == Q3GeometryKind::Rigid &&
						submission.transform_count == 1) ||
				(submission.geometry_kind == Q3GeometryKind::StaticInstances &&
						submission.transform_count > 0) ||
				(submission.geometry_kind == Q3GeometryKind::Skinned &&
						submission.transform_count == 1);
		if (!transform_shape_valid ||
				submission.transform_count >
						std::numeric_limits<std::uint32_t>::max() ||
				!range_valid(submission.first_transform,
						submission.transform_count, snapshot.transforms.size())) {
			reject(draw_list_, i, submission,
					Q3RejectReason::InvalidTransformRange);
			continue;
		}
		const bool bones_valid =
				submission.geometry_kind == Q3GeometryKind::Skinned ?
						submission.bone_count > 0 &&
							submission.bone_count <=
									std::numeric_limits<std::uint32_t>::max() &&
							range_valid(submission.first_bone,
								submission.bone_count,
								snapshot.bone_palette.size()) :
						submission.bone_count == 0;
		if (!bones_valid) {
			reject(draw_list_, i, submission, Q3RejectReason::InvalidBoneRange);
			continue;
		}
		bool all_finite = parameters_finite(submission);
		for (std::size_t transform = 0;
				all_finite && transform < submission.transform_count; ++transform) {
			all_finite = finite(snapshot.transforms[
					submission.first_transform + transform]);
		}
		for (std::size_t bone = 0;
				all_finite && bone < submission.bone_count; ++bone) {
			all_finite = finite(snapshot.bone_palette[
					submission.first_bone + bone]);
		}
		if (!all_finite) {
			reject(draw_list_, i, submission, Q3RejectReason::NonFiniteInput);
			continue;
		}

		const std::uint32_t sort_key =
				technique_stage(*technique) == 0 ?
						transparent_sort_key(submission.view_depth) : 0;
		prepared_.push_back({i, *technique, sort_key});
	}

	std::sort(prepared_.begin(), prepared_.end(),
			[](const PreparedSubmission &lhs,
					const PreparedSubmission &rhs) {
				const int lhs_stage = technique_stage(lhs.technique);
				const int rhs_stage = technique_stage(rhs.technique);
				if (lhs_stage != rhs_stage)
					return lhs_stage < rhs_stage;
				if (lhs_stage == 0 && lhs.sort_key != rhs.sort_key)
					return lhs.sort_key < rhs.sort_key;
				// Equal Q3 keys and the fixed follow-up draws retain producer
				// order. D-RORD-6 deliberately excludes retail's stale-key bug.
				return lhs.input_index < rhs.input_index;
			});

	for (const PreparedSubmission &prepared_submission : prepared_) {
		const Q3SubmissionSnapshot &submission =
				snapshot.submissions[prepared_submission.input_index];
		Q3DrawCommand command{};
		command.submission_id = submission.submission_id;
		command.input_index = prepared_submission.input_index;
		command.technique = prepared_submission.technique;
		command.geometry_kind = submission.geometry_kind;
		command.geometry = submission.geometry;
		command.material = submission.material;
		command.surface_index = submission.surface_index;
		command.sort_key = prepared_submission.sort_key;
		command.first_transform =
				static_cast<std::uint32_t>(draw_list_.transforms.size());
		command.transform_count =
				static_cast<std::uint32_t>(submission.transform_count);
		draw_list_.transforms.insert(draw_list_.transforms.end(),
			snapshot.transforms.begin() +
					static_cast<std::ptrdiff_t>(submission.first_transform),
			snapshot.transforms.begin() +
					static_cast<std::ptrdiff_t>(submission.first_transform +
						submission.transform_count));

		command.first_bone =
				static_cast<std::uint32_t>(draw_list_.bone_palette.size());
		command.bone_count = static_cast<std::uint32_t>(submission.bone_count);
		if (submission.bone_count > 0) {
			draw_list_.bone_palette.insert(draw_list_.bone_palette.end(),
				snapshot.bone_palette.begin() +
						static_cast<std::ptrdiff_t>(submission.first_bone),
				snapshot.bone_palette.begin() +
						static_cast<std::ptrdiff_t>(submission.first_bone +
							submission.bone_count));
		}
		command.object = submission.object;
		command.water = submission.water;
		command.celestial = submission.celestial;
		draw_list_.commands.push_back(command);
		++draw_list_.debug.emitted_submissions;
		++draw_list_.debug.emitted_by_technique[
				static_cast<std::size_t>(command.technique)];
	}

	return draw_list_;
}

} // namespace opennova::renderer
