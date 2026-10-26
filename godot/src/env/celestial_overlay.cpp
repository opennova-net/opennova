#include "env/celestial_overlay.h"

#include <godot_cpp/classes/camera3d.hpp>

#include <runtime/environment/water_mirror.h>
#include <runtime/renderer/device_fog.h>
#include <runtime/renderer/scene_overlay.h>

#include "env/celestial.h"
#include "env/mission_environment.h"
#include "env/water.h"
#include "object/env_light_state.h"
#include "object/object_model.h"
#include "render/scene_overlay_compositor.h"

namespace godot {

namespace {

Ref<EnvLightValues> light_values_of(const MissionEnvironment &p_env) {
	const Ref<EnvLightState> light_state = p_env.get_light_state();
	return light_state.is_valid() ? light_state->get_values() : Ref<EnvLightValues>();
}

} // namespace

void append_underwater_murk_overlay(const MissionEnvironment &p_env, const Water &p_water,
		SceneOverlaySubmission &r_submission) {
	const Vector3 lit = p_env.get_underwater_overlay_color();
	const float rgb[3] = { static_cast<float>(lit.x), static_cast<float>(lit.y),
		static_cast<float>(lit.z) };
	opennova::renderer::append_underwater_murk_overlay(rgb,
			static_cast<uint8_t>(p_env.get_underwater_overlay_alpha_byte()),
			p_water.get_water_height(), r_submission.frame);
}

void append_celestial_overlays(SceneOverlayModelSurfaces &r_bodies, Celestial &p_celestial,
		MissionEnvironment &p_env, const Water *p_water, Camera3D &p_camera, SceneOverlaySubmission &r_submission) {
	const Ref<EnvLightValues> light = light_values_of(p_env);
	if (light.is_null()) {
		return;
	}
	const Transform3D eye = p_camera.get_camera_transform();
	const Vector3 forward = -eye.basis.get_column(2).normalized();
	const opennova::env::SceneFogValues fog = p_env.state().build_scene_fog(p_env.is_underwater_view());
	const Celestial::OverlayBodies bodies = p_celestial.get_overlay_bodies();
	const float frame_scale[3] = { static_cast<float>(light->gain.x),
		static_cast<float>(light->gain.y), static_cast<float>(light->gain.z) };
	const float glare_scale[3] = { opennova::renderer::kSunGlareLightScale,
		opennova::renderer::kSunGlareLightScale, opennova::renderer::kSunGlareLightScale };
	struct Leg {
		const Celestial::OverlayBody &body;
		opennova::renderer::SceneOverlaySlot slot;
		const float *light_scale;
		bool gated;
	};
	const bool water_height_set = p_water != nullptr && p_water->get_water_height() != 0.0f;
	const Leg legs[] = {
		{ bodies.glint, opennova::renderer::SceneOverlaySlot::WaterGlint, frame_scale,
				!water_height_set },
		{ bodies.glare, opennova::renderer::SceneOverlaySlot::SunGlare, glare_scale, false },
	};
	for (const Leg &leg : legs) {
		if (leg.body.model == nullptr) {
			continue;
		}
		r_bodies.take_over(leg.body.model);
		if (!leg.body.drawn || leg.gated) {
			continue;
		}
		const float view_depth = static_cast<float>(
				(leg.body.model->get_global_position() - eye.origin).dot(forward));
		const float visibility = opennova::renderer::device_fog_visibility(view_depth,
				fog.start, fog.end, fog.type, light->fog_enabled);
		r_bodies.append(leg.slot, leg.body.model, leg.light_scale, visibility, r_submission, {});
	}
}

namespace {

// One mirror's sky redraw at that mirror's own camera, in its pass's own slots.
void append_mirror_sky_redraw(SceneOverlayModelSurfaces &r_bodies, Celestial &p_celestial,
		const EnvLightValues &p_light, const opennova::env::SceneFogValues &p_fog,
		const Camera3D &p_mirror, opennova::renderer::SceneOverlaySlot p_discs_slot,
		opennova::renderer::SceneOverlaySlot p_glow_slot, SceneOverlaySubmission &r_submission) {
	const Transform3D eye = p_mirror.get_global_transform();
	const Vector3 forward = -eye.basis.get_column(2).normalized();
	const Celestial::MirrorRedraw redraw = p_celestial.get_mirror_redraw(forward);
	const float light_scale[3] = { static_cast<float>(p_light.gain.x),
		static_cast<float>(p_light.gain.y), static_cast<float>(p_light.gain.z) };
	SceneOverlayModelSurfaces::AppendOptions options;
	options.offset = eye.origin - redraw.anchor;
	options.depth = opennova::renderer::SceneOverlayDepth::FarBand;
	const auto visibility_of = [&](ObjectModel *p_model) {
		const float view_depth = static_cast<float>(
				(p_model->get_global_position() + options.offset - eye.origin).dot(forward));
		return opennova::renderer::device_fog_visibility(view_depth, p_fog.start, p_fog.end,
				p_fog.type, p_light.fog_enabled);
	};
	for (ObjectModel *disc : { redraw.sun, redraw.moon }) {
		if (disc != nullptr) {
			r_bodies.append(p_discs_slot, disc, light_scale, visibility_of(disc), r_submission,
					options);
		}
	}
	if (redraw.glare_drawn && redraw.glare != nullptr) {
		options.self_lum = &redraw.glare_self_lum;
		r_bodies.append(p_glow_slot, redraw.glare, light_scale, visibility_of(redraw.glare),
				r_submission, options);
	}
}

} // namespace

void append_water_mirror_overlays(SceneOverlayModelSurfaces &r_bodies, Celestial *p_celestial,
		MissionEnvironment *p_env, Water &p_water, SceneOverlaySubmission &r_submission) {
	Camera3D *mirror = p_water.get_reflection_camera();
	if (mirror == nullptr || !mirror->is_inside_tree()) {
		return;
	}
	opennova::renderer::append_mirror_dim_overlay(opennova::env::kReflectionDimFactor,
			r_submission.frame);
	if (p_celestial == nullptr || p_env == nullptr) {
		return;
	}
	const Ref<EnvLightValues> light = light_values_of(*p_env);
	if (light.is_null()) {
		return;
	}
	const opennova::env::SceneFogValues fog = p_env->state().build_water_mirror_fog();
	append_mirror_sky_redraw(r_bodies, *p_celestial, *light.ptr(), fog, *mirror,
			opennova::renderer::SceneOverlaySlot::MirrorCelestialBodies,
			opennova::renderer::SceneOverlaySlot::MirrorSunGlow, r_submission);
	// The weapon Inset's mirror, while the Inset pass renders it, closes its own target the same
	// way at its own eye (renderer/scene_overlay.h kInsetMirrorOverlayOrder carries the witness).
	Camera3D *inset_mirror = p_water.is_inset_mirror_rendered()
			? p_water.get_inset_reflection_camera()
			: nullptr;
	if (inset_mirror != nullptr && inset_mirror->is_inside_tree()) {
		append_mirror_sky_redraw(r_bodies, *p_celestial, *light.ptr(), fog, *inset_mirror,
				opennova::renderer::SceneOverlaySlot::InsetMirrorCelestialBodies,
				opennova::renderer::SceneOverlaySlot::InsetMirrorSunGlow, r_submission);
	}
}

} // namespace godot
