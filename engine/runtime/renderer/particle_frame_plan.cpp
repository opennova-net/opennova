#include <runtime/renderer/particle_frame_plan.h>

#include <algorithm>

#include <formats/env/env_weather.h>
#include <runtime/renderer/particle_color.h>

namespace opennova::renderer {

namespace {

void add_compile(ParticleFramePlan &plan, ParticleDrawSlot slot, ParticleFrameView view,
		ParticleRenderDomain domain, ParticleWaterSubset subset, bool thermal,
		std::uint32_t lod_divisor, bool relight) {
	ParticlePassCompile &compile = plan.compiles[plan.compile_count++];
	compile.slot = slot;
	compile.view = view;
	compile.domain = domain;
	compile.water_subset = subset;
	compile.thermal = thermal;
	compile.lod_divisor = lod_divisor;
	compile.relight = relight;
}

// A secondary view's pair: far side then camera side over its eye's side,
// relit for that eye first, never under the thermal byte.
void add_secondary_pair(ParticleFramePlan &plan, ParticleFrameView view, bool above_water,
		ParticleDrawSlot far_slot, ParticleDrawSlot camera_slot, std::uint32_t lod_divisor) {
	add_compile(plan, far_slot, view, ParticleRenderDomain::World,
			particle_water_subset_for_side(above_water, false), false, lod_divisor, true);
	add_compile(plan, camera_slot, view, ParticleRenderDomain::World,
			particle_water_subset_for_side(above_water, true), false, lod_divisor, false);
}

void retire_pair(ParticleFramePlan &plan, ParticleDrawSlot far_slot,
		ParticleDrawSlot camera_slot) {
	plan.retired[plan.retired_count++] = far_slot;
	plan.retired[plan.retired_count++] = camera_slot;
}

void include_point(ParticleAabb &bounds, const ParticleVec3 &point, float radius) {
	const ParticleVec3 minimum{point.x - radius, point.y - radius, point.z - radius};
	const ParticleVec3 maximum{point.x + radius, point.y + radius, point.z + radius};
	if (!bounds.valid) {
		bounds.min = minimum;
		bounds.max = maximum;
		bounds.valid = true;
		return;
	}
	bounds.min.x = std::min(bounds.min.x, minimum.x);
	bounds.min.y = std::min(bounds.min.y, minimum.y);
	bounds.min.z = std::min(bounds.min.z, minimum.z);
	bounds.max.x = std::max(bounds.max.x, maximum.x);
	bounds.max.y = std::max(bounds.max.y, maximum.y);
	bounds.max.z = std::max(bounds.max.z, maximum.z);
}

ParticleVec3 particle_vec(const particle::Vec3 &value) {
	return {value.x, value.y, value.z};
}

ParticlePassFog pass_fog(const env::SceneFogValues &fog) {
	ParticlePassFog out;
	out.color = {fog.color.r, fog.color.g, fog.color.b};
	out.start = fog.start;
	out.end = fog.end;
	out.type = std::clamp<std::int32_t>(fog.type, 0, 3);
	return out;
}

} // namespace

ParticleFramePlan plan_particle_frame(const ParticleFrameViews &views) {
	ParticleFramePlan plan;
	const std::uint32_t main_stride = views.strides.main_view;
	add_compile(plan, ParticleDrawSlot::WorldFarSide, ParticleFrameView::Main,
			ParticleRenderDomain::World,
			particle_water_subset_for_side(views.main_above_water, false), views.thermal,
			main_stride, false);
	add_compile(plan, ParticleDrawSlot::WorldCameraSide, ParticleFrameView::Main,
			ParticleRenderDomain::World,
			particle_water_subset_for_side(views.main_above_water, true), views.thermal,
			main_stride, false);
	add_compile(plan, ParticleDrawSlot::Distortion, ParticleFrameView::Main,
			ParticleRenderDomain::World, ParticleWaterSubset::Distortion, views.thermal,
			main_stride, false);
	add_compile(plan, ParticleDrawSlot::FirstPerson, ParticleFrameView::Main,
			ParticleRenderDomain::FirstPerson, ParticleWaterSubset::All, views.thermal,
			main_stride, false);
	// The main scene's mirror selects its subsets by the main eye's side.
	if (views.mirror && compiles_mirror_particles(views.mirror_rendered)) {
		add_secondary_pair(plan, ParticleFrameView::Mirror, views.main_above_water,
				ParticleDrawSlot::ReflectionFarSide, ParticleDrawSlot::ReflectionCameraSide,
				views.strides.mirror);
	} else {
		retire_pair(plan, ParticleDrawSlot::ReflectionFarSide,
				ParticleDrawSlot::ReflectionCameraSide);
	}
	if (views.inset) {
		add_secondary_pair(plan, ParticleFrameView::Inset, views.inset_above_water,
				ParticleDrawSlot::SecondSceneFarSide, ParticleDrawSlot::SecondSceneCameraSide,
				views.strides.inset);
	} else {
		retire_pair(plan, ParticleDrawSlot::SecondSceneFarSide,
				ParticleDrawSlot::SecondSceneCameraSide);
	}
	if (views.inset_mirror && compiles_mirror_particles(views.inset_mirror_rendered)) {
		add_secondary_pair(plan, ParticleFrameView::InsetMirror, views.main_above_water,
				ParticleDrawSlot::InsetMirrorFarSide, ParticleDrawSlot::InsetMirrorCameraSide,
				views.strides.inset_mirror);
	} else {
		retire_pair(plan, ParticleDrawSlot::InsetMirrorFarSide,
				ParticleDrawSlot::InsetMirrorCameraSide);
	}
	return plan;
}

bool particle_distortion_row_present(const ParticleFrameDebugCounters &counters) {
	// Every class-7 child of a visible group, whatever the submit made of it
	// [orig: EffectWorld_HasDistortionParticles @ 0x5F6640].
	return counters.selected_emitters + counters.empty_emitters +
			counters.frustum_culled_emitters > 0;
}

void build_particle_render_input(const particle::ParticleFrameSnapshot &frame,
		const std::vector<ParticleDefinitionVisual> &visuals,
		const std::vector<ParticleAtlasEntry> &entries, const std::array<float, 9> &view_rows,
		const ParticleQuadTints &tints, ParticleRenderInput &out) {
	ParticleFrameSnapshot &snapshot = out.snapshot;
	snapshot.frame_id = frame.frame_index;
	snapshot.emitters.clear();
	snapshot.particles.clear();
	out.lit_quads.clear();
	if (!frame.definitions || visuals.size() != frame.definitions->size())
		return;
	snapshot.emitters.reserve(frame.emitters.size());
	snapshot.particles.reserve(frame.particles.size());

	for (const particle::EffectEmitterFrameSnapshot &source_emitter : frame.emitters) {
		if (source_emitter.definition_index >= frame.definitions->size() ||
				source_emitter.definition_index >= visuals.size())
			continue;
		const particle::ParticleDef &definition =
				(*frame.definitions)[source_emitter.definition_index];
		const ParticleDefinitionVisual &visual = visuals[source_emitter.definition_index];

		ParticleEmitterSnapshot emitter;
		emitter.emitter_id = source_emitter.id;
		emitter.position = particle_vec(source_emitter.position);
		emitter.first_particle = snapshot.particles.size();
		const bool group_known = source_emitter.group_index < frame.groups.size();
		emitter.domain = group_known &&
						frame.groups[source_emitter.group_index].render_domain ==
								particle::EffectRenderDomain::FirstPerson
				? ParticleRenderDomain::FirstPerson
				: ParticleRenderDomain::World;
		emitter.distortion_class = particle::particle_def_is_distortion_class(definition);
		emitter.group_visible =
				!group_known || frame.groups[source_emitter.group_index].section_visible;

		int fallback_layer = 0;
		for (int layer = 0; layer < kParticleGraphicLayerCount; ++layer) {
			if (visual.layers[static_cast<std::size_t>(layer)].present) {
				fallback_layer = layer;
				break;
			}
		}

		const std::size_t first = std::min(source_emitter.first_particle, frame.particles.size());
		const std::size_t available = frame.particles.size() - first;
		const std::size_t count = std::min(source_emitter.particle_count, available);
		for (std::size_t particle_index = 0; particle_index < count; ++particle_index) {
			const particle::Particle &particle = frame.particles[first + particle_index];
			// The manager/emitter sort box takes the simulator's base half-size,
			// even for a particle the view's stride leaves unbuilt.
			include_point(emitter.bounds, particle_vec(particle.position), particle.size * 0.5f);

			int layer_index = std::clamp<int>(static_cast<int>(particle.graphic_layer), 0,
					kParticleGraphicLayerCount - 1);
			if (!visual.layers[static_cast<std::size_t>(layer_index)].present)
				layer_index = fallback_layer;
			const ParticleLayerVisual &layer = visual.layers[static_cast<std::size_t>(layer_index)];
			const particle::GraphicLayer &graphic =
					definition.graphics[static_cast<std::size_t>(layer_index)];
			const ParticleQuadInputs inputs = particle_quad_inputs(
					particle, definition, graphic, visual.ambient_lit, tints, view_rows);

			ParticleQuadSnapshot quad;
			quad.serial = particle.serial;
			quad.center = particle_vec(particle.position);
			quad.half_width = inputs.half_extent;
			quad.half_height = inputs.half_extent;
			quad.roll = inputs.roll;
			quad.yaw = particle.yaw;
			quad.pitch = particle.pitch;
			quad.camera_pull = source_emitter.camera_pull;
			quad.primary_color = inputs.primary_color;
			quad.secondary_color = inputs.secondary_color;
			quad.alignment = inputs.alignment;
			quad.state.pipeline = static_cast<ParticlePipeline>(layer.type);
			quad.state.pass = quad.state.pipeline == ParticlePipeline::Distort
					? ParticleRenderPass::Distortion
					: ParticleRenderPass::Color;

			std::size_t entry_index = kParticleMissingEntry;
			if (!layer.frame_entries.empty()) {
				entry_index = layer.frame_entries[static_cast<std::size_t>(
						std::max(inputs.frame, 0)) % layer.frame_entries.size()];
			}
			quad.visible = entry_index != kParticleMissingEntry && entry_index < entries.size() &&
					entries[entry_index].placement.valid;
			if (quad.visible) {
				const ParticleAtlasEntry &entry = entries[entry_index];
				quad.state.atlas.page = entry.placement.page;
				quad.state.atlas.type = entry.type;
				quad.state.atlas.rect = entry.placement.rect;
				quad.state.atlas.inset_u = entry.placement.inset_u;
				quad.state.atlas.inset_v = entry.placement.inset_v;
			}
			if (inputs.lit) {
				ParticleLitQuad lit;
				lit.particle_index = snapshot.particles.size();
				lit.bump_scale = definition.bump_scale;
				lit.roll = particle.rotation;
				lit.alpha = inputs.lit_alpha;
				out.lit_quads.push_back(lit);
			}
			snapshot.particles.push_back(quad);
		}
		emitter.particle_count = snapshot.particles.size() - emitter.first_particle;
		snapshot.emitters.push_back(emitter);
	}
}

void relight_particle_render_input(ParticleRenderInput &input,
		const std::array<float, 9> &view_rows) {
	for (const ParticleLitQuad &lit : input.lit_quads) {
		input.snapshot.particles[lit.particle_index].primary_color =
				particle_lit_primary_color(lit.bump_scale, lit.roll, lit.alpha, view_rows);
	}
}

ParticlePassEnvironment particle_pass_environment(const env::EnvironmentState *state,
		bool underwater_view) {
	ParticlePassEnvironment out;
	if (state == nullptr)
		return out;
	out.fog = pass_fog(state->build_scene_fog(underwater_view));
	out.mirror_fog = pass_fog(state->build_water_mirror_fog());
	// g_EnvTerrainLightCombined's bytes over 128 [orig: Render_EmitterEffect
	// @ 0x5f70c0 - the manager's +0x3E8 @ 0x5f7163].
	const env::Rgb combined = env::combine_terrain_light(state->sun_light(), state->sky_ambient());
	constexpr float kByteOver128 = 255.0f / 128.0f;
	out.tints.ambient = {combined.r * kByteOver128, combined.g * kByteOver128,
		combined.b * kByteOver128};
	// `min(2 * modulator_byte, 255) / 128`: color_src_gain already carries the
	// modulator byte over 64, so only the saturation remains.
	const env::Rgb gain = state->color_src_gain();
	constexpr float kSaturated = 255.0f / 128.0f;
	out.tints.modulator = {std::min(gain.r, kSaturated), std::min(gain.g, kSaturated),
		std::min(gain.b, kSaturated)};
	return out;
}

} // namespace opennova::renderer
