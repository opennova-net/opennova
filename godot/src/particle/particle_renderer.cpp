#include "particle/particle_renderer.h"
#include "world/game_world.h"

#include "env/mission_environment.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <base/io/hash.h>
#include <runtime/particle/emitter.h>
#include <runtime/renderer/particle_atlas.h>
#include <runtime/renderer/particle_color.h>
#include <runtime/renderer/particle_frame.h>
#include <runtime/renderer/particle_frame_plan.h>
#include <runtime/renderer/particle_quad_inputs.h>
#include <runtime/renderer/render_order.h>
#include <runtime/renderer/particle_density.h>

#include "particle/particle_atlas_device.h"
#include "particle/particle_compositor.h"
#include "particle/effect_distortion_drawer.h"
#include "particle/particle_convert.h"
#include "particle/particle_far_pass.h"
#include "particle/particle_first_person_batch.h"
#include "particle/particle_material_cache.h"
#include "render/display_decode.h"
#include "render/frame_fx.h"
#include "render/rd_glsl.h" // call_device_create
#include "render/scene_overlay_compositor.h"
#include "render/world_environment_lookup.h"
#include "util/string_convert.h"

using namespace godot;

namespace {

using Slot = opennova::renderer::ParticleDrawSlot;

// One retained value per compile slot, indexed by the engine's slot.
template <typename T>
struct SlotArray {
	std::array<T, opennova::renderer::kParticleDrawSlotCount> values{};
	T &operator[](Slot p_slot) { return values[static_cast<std::size_t>(p_slot)]; }
	const T &operator[](Slot p_slot) const { return values[static_cast<std::size_t>(p_slot)]; }
	void fill(const T &p_value) { values.fill(p_value); }
};

using ParticleEffectPair = std::array<Ref<ParticleCompositorEffect>, 2>;


// The view basis rows (world to view) as the engine's particle laws read them.
std::array<float, 9> basis_rows(const Basis &basis) {
	std::array<float, 9> rows;
	for (int row = 0; row < 3; ++row)
		for (int col = 0; col < 3; ++col)
			rows[row * 3 + col] = static_cast<float>(basis[row][col]);
	return rows;
}

opennova::renderer::ParticleVec3 particle_vec(const Vector3 &value) {
	return {value.x, value.y, value.z};
}

struct ParticleCameraFrame {
	Vector3 position{};
	Vector3 right{1.0f, 0.0f, 0.0f};
	Vector3 up{0.0f, 1.0f, 0.0f};
	Vector3 forward{0.0f, 0.0f, 1.0f};
	Basis view_basis{};
	std::array<float, 16> projection{};
	bool projection_valid = false;
	// The eye's six planes: the render submit's clip state (renderer::ParticleViewInput
	// frustum carries the witness).
	opennova::particle::ParticleViewFrustum frustum{};
};

// The manager camera matrix retail folds into view space per frame
// (CParticleManager_TransformToViewSpace @0x5ecc50 - docs/particles/ptl-format-re.md).
ParticleCameraFrame particle_camera_frame(Camera3D *camera) {
	ParticleCameraFrame result;
	if (camera == nullptr)
		return result;
	// get_camera_transform includes h_offset/v_offset, so classification and
	// billboards use the eye that the viewport actually renders.
	const Transform3D transform = camera->get_camera_transform();
	result.position = transform.origin;
	result.view_basis = transform.affine_inverse().basis;
	result.right = transform.basis.get_column(0);
	result.up = transform.basis.get_column(1);
	result.forward = -transform.basis.get_column(2);
	auto normalize_or = [](Vector3 value, const Vector3 &fallback) {
		if (value.length_squared() > 0.0f)
			return value.normalized();
		return fallback;
	};
	result.right = normalize_or(result.right, Vector3(1.0f, 0.0f, 0.0f));
	result.up = normalize_or(result.up, Vector3(0.0f, 1.0f, 0.0f));
	result.forward = normalize_or(result.forward, Vector3(0.0f, 0.0f, 1.0f));
	const Projection projection = camera->get_camera_projection();
	for (int column = 0; column < 4; ++column) {
		for (int row = 0; row < 4; ++row) {
			result.projection[static_cast<std::size_t>(column * 4 + row)] =
					projection.columns[column][row];
		}
	}
	result.projection_valid = true;
	// A point just past the near plane reads inside every plane (EffectWorld's
	// NOVISNOUPDATE state takes its probe the same way).
	result.frustum = particle_view_frustum(camera->get_frustum(),
			transform.origin - transform.basis.get_column(2) * (camera->get_near() + 1.0f));
	return result;
}

// A water mirror's submit clip state: its own eye over a square at its
// horizontal fov, whatever field its image projects (renderer::ParticleViewInput
// frustum carries the witness). The node's projection carries the horizontal
// fov in its first column, its frustum form (the RTT's half texel) included.
opennova::particle::ParticleViewFrustum mirror_cull_frustum(Camera3D *camera) {
	const Projection projection = camera->get_camera_projection();
	const double focal_x = static_cast<double>(projection.columns[0][0]);
	if (!(focal_x > 0.0))
		return {};
	const double fov_h = 2.0 * std::atan(1.0 / focal_x) * 180.0 / 3.14159265358979323846;
	const Projection square = Projection::create_perspective(static_cast<real_t>(fov_h), 1.0f,
			camera->get_near(), camera->get_far(), true);
	const Transform3D transform = camera->get_camera_transform();
	const Vector<Plane> planes = square.get_projection_planes(transform);
	TypedArray<Plane> plane_array;
	for (int index = 0; index < planes.size(); ++index)
		plane_array.push_back(planes[index]);
	return particle_view_frustum(plane_array,
			transform.origin - transform.basis.get_column(2) * (camera->get_near() + 1.0f));
}

// One comparable value for "what this slot draws": FNV-1a over the compiled
// vertex bytes (position, both colour words, UV; the 28-byte vertex has no
// padding). Two slots, or two renders of one slot, draw the same quads exactly
// when it matches. Computed only when a report is asked for.
std::uint64_t vertex_checksum(
		const std::vector<opennova::renderer::ParticleVertex> &vertices) {
	return opennova::io::fnv1a64_bytes(opennova::io::kFnv1a64Offset, vertices.data(),
			vertices.size() * sizeof(opennova::renderer::ParticleVertex));
}

// Diagnostics read each compiler's retained draw list in place: it stays
// valid until that slot compiles again and nothing on the render thread
// references it (World submissions are immutable copies), so no per-frame
// capture copy and no opt-in flag are needed for the first report to be live.
Dictionary draw_list_report(const opennova::renderer::ParticleDrawList &draw_list) {
	Dictionary result;
	const opennova::renderer::ParticleFrameDebugCounters &debug = draw_list.debug;
	result["frame_id"] = token_to_godot(draw_list.frame_id);
	result["vertex_checksum"] = token_to_godot(vertex_checksum(draw_list.vertices));
	result["compile_index"] = token_to_godot(debug.compile_index);
	result["input_emitters"] = static_cast<int64_t>(debug.input_emitters);
	result["selected_emitters"] = static_cast<int64_t>(debug.selected_emitters);
	result["input_particles"] = static_cast<int64_t>(debug.input_particles);
	result["domain_filtered_particles"] =
			static_cast<int64_t>(debug.domain_filtered_particles);
	result["section_hidden_emitters"] =
			static_cast<int64_t>(debug.section_hidden_emitters);
	result["water_filtered_emitters"] =
			static_cast<int64_t>(debug.water_filtered_emitters);
	result["water_filtered_particles"] =
			static_cast<int64_t>(debug.water_filtered_particles);
	result["empty_emitters"] = static_cast<int64_t>(debug.empty_emitters);
	result["frustum_culled_emitters"] = static_cast<int64_t>(debug.frustum_culled_emitters);
	result["frustum_culled_particles"] = static_cast<int64_t>(debug.frustum_culled_particles);
	result["invisible_particles"] = static_cast<int64_t>(debug.invisible_particles);
	result["lod_skipped_particles"] = static_cast<int64_t>(debug.lod_skipped_particles);
	result["truncated_particles"] = static_cast<int64_t>(debug.truncated_particles);
	result["rendered_quad_count"] = static_cast<int64_t>(debug.emitted_quads);
	result["draw_command_count"] = static_cast<int64_t>(debug.draw_commands);
	result["adjacent_state_merges"] =
			static_cast<int64_t>(debug.adjacent_state_merges);
	result["recursive_partition_calls"] =
			static_cast<int64_t>(debug.recursive_partition_calls);
	result["render_batch_leaves"] =
			static_cast<int64_t>(debug.render_batch_leaves);
	result["capacity_growths_this_compile"] =
			static_cast<int64_t>(debug.capacity_growths_this_compile);
	result["lifetime_capacity_growths"] =
			token_to_godot(debug.lifetime_capacity_growths);
	result["vertex_capacity"] = static_cast<int64_t>(debug.vertex_capacity);
	result["command_capacity"] = static_cast<int64_t>(debug.command_capacity);
	result["emitter_bounds_capacity"] =
			static_cast<int64_t>(debug.emitter_bounds_capacity);
	Array commands;
	commands.resize(static_cast<int64_t>(draw_list.commands.size()));
	for (std::size_t i = 0; i < draw_list.commands.size(); ++i) {
		const opennova::renderer::ParticleDrawCommand &command = draw_list.commands[i];
		Dictionary value;
		value["render_domain"] = static_cast<int>(command.domain);
		value["render_pass"] = static_cast<int>(command.pass);
		value["pipeline"] = static_cast<int>(command.pipeline);
		value["atlas_page"] = static_cast<int64_t>(command.atlas_page);
		value["atlas_type"] = static_cast<int>(command.atlas_type);
		value["variant"] = static_cast<int>(command.variant);
		value["first_quad"] = static_cast<int64_t>(command.first_quad);
		value["quad_count"] = static_cast<int64_t>(command.quad_count);
		commands[static_cast<int64_t>(i)] = value;
	}
	result["commands"] = commands;
	return result;
}

// A camera owns one composed resource regardless of how many independent
// EffectWorld/preview renderers target it. Keeping the coordinator outside any
// one renderer prevents A/B from cloning each other's installed compositor
// forever, and lets non-LIFO teardown remove only its own effect.
struct ParticleCameraCompositorState {
	Ref<Compositor> explicit_base;
	Ref<Compositor> effective_base;
	Ref<Compositor> base_source;
	Ref<Compositor> installed;
	std::vector<std::uint64_t> base_effect_ids;
	std::vector<std::pair<std::uint64_t,
			Ref<CompositorEffect>>> effects;
	bool camera_inherits_world = false;
	bool inherited_world_compositor = false;
};

std::map<std::uint64_t, ParticleCameraCompositorState> &
particle_camera_compositors() {
	static std::map<std::uint64_t, ParticleCameraCompositorState> states;
	return states;
}

// The compositor a camera with none of its own renders with in `viewport`: its
// WorldEnvironment's, else the one a DisplayDecode put on its scenario (an
// editor preview with no WorldEnvironment). The camera's composition keeps
// that chain's terminal decode, so the view shows the frame decoded once
// whether or not particles draw.
Ref<Compositor> world_compositor_for(Viewport *viewport) {
	if (viewport == nullptr)
		return Ref<Compositor>();
	if (WorldEnvironment *environment = find_world_environment(viewport))
		return environment->get_compositor();
	return DisplayDecode::scenario_compositor(viewport->find_world_3d());
}

// The viewport whose WorldEnvironment a second scene camera inherits. That
// camera renders the main view's World3D from a sibling SubViewport holding no
// WorldEnvironment of its own, and a camera without a compositor resolves the
// scenario's, so the chain to recompose around its particle pair (FrameFX and
// the display transfer last) is the main view's. A camera in a world of its
// own keeps its own viewport.
Viewport *second_scene_base_viewport(Camera3D *camera, Viewport *main_viewport) {
	Viewport *own_viewport = camera->get_viewport();
	if (own_viewport == nullptr || main_viewport == nullptr ||
			own_viewport == main_viewport)
		return own_viewport;
	return own_viewport->find_world_3d() == main_viewport->find_world_3d() ?
			main_viewport : own_viewport;
}

// The effects a renderer composes around a camera: its particle passes and
// the post-particle overlay pass that follows each view's pass B.
bool is_view_pass_effect(const Ref<CompositorEffect> &effect) {
	return effect.is_valid() &&
			(Object::cast_to<ParticleCompositorEffect>(effect.ptr()) != nullptr ||
					Object::cast_to<SceneOverlayCompositorEffect>(effect.ptr()) != nullptr);
}

std::vector<std::uint64_t> non_particle_effect_ids(
		const Ref<Compositor> &compositor) {
	std::vector<std::uint64_t> ids;
	if (compositor.is_null())
		return ids;
	const TypedArray<Ref<CompositorEffect>> effects =
			compositor->get_compositor_effects();
	ids.reserve(static_cast<std::size_t>(effects.size()));
	for (int64_t index = 0; index < effects.size(); ++index) {
		Ref<CompositorEffect> effect = effects[index];
		if (effect.is_valid() && !is_view_pass_effect(effect)) {
			ids.push_back(effect->get_instance_id());
		}
	}
	return ids;
}

Ref<Compositor> without_particle_effects(
		const Ref<Compositor> &source) {
	if (source.is_null())
		return Ref<Compositor>();
	const TypedArray<Ref<CompositorEffect>> source_effects =
			source->get_compositor_effects();
	TypedArray<Ref<CompositorEffect>> filtered;
	bool removed = false;
	for (int64_t index = 0; index < source_effects.size(); ++index) {
		Ref<CompositorEffect> effect = source_effects[index];
		if (is_view_pass_effect(effect)) {
			removed = true;
			continue;
		}
		filtered.push_back(effect);
	}
	if (!removed)
		return source;
	Ref<Compositor> result;
	result.instantiate();
	result->set_compositor_effects(filtered);
	return result;
}

void set_camera_base_source(ParticleCameraCompositorState &state,
		const Ref<Compositor> &source) {
	state.base_source = source;
	state.base_effect_ids = non_particle_effect_ids(source);
	state.effective_base = without_particle_effects(source);
	state.explicit_base = state.camera_inherits_world ?
			Ref<Compositor>() : state.effective_base;
	state.inherited_world_compositor =
			state.camera_inherits_world && source.is_valid();
}

void capture_camera_base(ParticleCameraCompositorState &state,
		Camera3D *camera, Viewport *viewport) {
	Ref<Compositor> explicit_base = camera->get_compositor();
	state.camera_inherits_world = explicit_base.is_null();
	set_camera_base_source(state, state.camera_inherits_world ?
			world_compositor_for(viewport) : explicit_base);
}

bool refresh_camera_base(ParticleCameraCompositorState &state,
		Viewport *viewport) {
	Ref<Compositor> source = state.camera_inherits_world ?
			world_compositor_for(viewport) : state.base_source;
	const std::vector<std::uint64_t> effect_ids =
			non_particle_effect_ids(source);
	if (source == state.base_source &&
			effect_ids == state.base_effect_ids)
		return false;
	set_camera_base_source(state, source);
	return true;
}

void rebuild_camera_compositor(ParticleCameraCompositorState &state,
		Camera3D *camera) {
	TypedArray<Ref<CompositorEffect>> effects;
	TypedArray<Ref<CompositorEffect>> terminal_effects;
	if (state.effective_base.is_valid()) {
		const TypedArray<Ref<CompositorEffect>> base_effects =
				state.effective_base->get_compositor_effects();
		for (int64_t index = 0; index < base_effects.size(); ++index) {
			Ref<CompositorEffect> effect = base_effects[index];
			if (effect.is_valid() &&
					Object::cast_to<FrameFxCompositorEffect>(
							effect.ptr()) != nullptr) {
				terminal_effects.push_back(effect);
			} else {
				effects.push_back(effect);
			}
		}
	}
	for (const auto &entry : state.effects) {
		bool already_present = false;
		for (int64_t index = 0; index < effects.size(); ++index) {
			Ref<CompositorEffect> existing = effects[index];
			if (existing.is_valid() &&
					existing->get_instance_id() == entry.first) {
				already_present = true;
				break;
			}
		}
		if (!already_present) {
			Ref<CompositorEffect> generic_effect = entry.second;
			effects.push_back(generic_effect);
		}
	}
	// Retail particles are part of the D3D9 gamma framebuffer. FrameFX and
	// the one terminal gamma-to-linear bridge must therefore run after every
	// particle compositor targeting this camera.
	for (int64_t index = 0; index < terminal_effects.size(); ++index) {
		effects.push_back(terminal_effects[index]);
	}
	state.installed.instantiate();
	state.installed->set_compositor_effects(effects);
	camera->set_compositor(state.installed);
}

} // namespace

class ParticleRenderer::Impl {
public:
	ParticleFirstPersonBatch first_person;
	SlotArray<opennova::renderer::ParticleFrameCompiler> compilers;
	// A slot is present once it compiled for the current render and until
	// clear_draws or a render without its camera retires it.
	SlotArray<bool> slot_present;
	// The frame's render snapshot and its view-lit quads
	// (renderer::build_particle_render_input).
	opennova::renderer::ParticleRenderInput render_input;
	ParticleAtlasDevice atlas;
	ParticleMaterialCache materials;
	// Pass A of the main view and of the second scene view as render-list
	// runs at kRungParticleFarSide (ParticleFarPass), with their own
	// shaders/materials; the compositor far effect keeps only distortion.
	ParticleFarPass world_far_pass;
	ParticleFarPass second_scene_far_pass;
	opennova::renderer::ParticleDrawList far_distortion_scratch;
	ParticleEffectPair world_effects;
	ParticleEffectPair reflection_effects;
	ParticleEffectPair second_scene_effects;
	ParticleEffectPair inset_mirror_effects;
	// The post-particle overlay pass of each view, composed right after the
	// view's pair (runtime/renderer/scene_overlay.h).
	Ref<SceneOverlayCompositorEffect> world_overlay;
	Ref<SceneOverlayCompositorEffect> reflection_overlay;
	Ref<SceneOverlayCompositorEffect> second_scene_overlay;
	Ref<SceneOverlayCompositorEffect> inset_mirror_overlay;
	// FrameFX's type-0 row: the distortion subset's effect (attached to no
	// camera) and the drawer FrameFX calls; the drawer outlives effect sets.
	Ref<ParticleCompositorEffect> distortion_effect;
	std::shared_ptr<EffectDistortionDrawer> distortion_drawer =
			std::make_shared<EffectDistortionDrawer>();
	ObjectID attached_world_camera;
	ObjectID attached_reflection_camera;
	ObjectID attached_second_scene_camera;
	ObjectID attached_inset_mirror_camera;
	bool inherited_world_compositor = false;
	bool inherited_reflection_compositor = false;
	bool inherited_second_scene_compositor = false;
	bool inherited_inset_mirror_compositor = false;
	// The stride the frame's last scene pass left, which the next frame's
	// water mirror draws in (renderer::particle_frame_strides); the effect
	// world's scale starts at kParticleDensityScaleInitial.
	std::uint32_t last_pass_lod_divisor = opennova::renderer::particle_lod_divisor(
			opennova::renderer::kParticleDensityScaleInitial);
	ObjectID cached_environment_source;
	std::int64_t cached_environment_generation =
			std::numeric_limits<std::int64_t>::min();
	// The frame's particle fog (the scene's and the water mirror pair's) and
	// the manager's two tints, from the environment's engine state
	// (renderer::particle_pass_environment). Unfogged and neutral until an
	// environment source is attached.
	opennova::renderer::ParticlePassEnvironment environment;

	Impl() {
		create_effects();
	}

	// Every compositor effect this renderer owns, whichever view it serves, so
	// no lifecycle path (hide, warm, shutdown, re-entry) can skip a view.
	template <typename Visitor>
	void for_each_effect(Visitor &&visit) {
		for (ParticleEffectPair *pair : {&world_effects, &reflection_effects,
					 &second_scene_effects, &inset_mirror_effects}) {
			for (Ref<ParticleCompositorEffect> &effect : *pair)
				visit(effect);
		}
		visit(distortion_effect);
	}

	template <typename Visitor>
	void for_each_overlay(Visitor &&visit) {
		for (Ref<SceneOverlayCompositorEffect> *overlay : {&world_overlay, &reflection_overlay,
					 &second_scene_overlay, &inset_mirror_overlay}) {
			visit(*overlay);
		}
	}

	// A fresh compositor set: the constructor's, and the replacement a
	// re-entering renderer needs. release_device_resources() retires an
	// effect for good (its render callback never runs again), so a renderer
	// that left the tree can only render again through new effects.
	void create_effects() {
		for_each_effect([](Ref<ParticleCompositorEffect> &effect) {
			effect.instantiate();
		});
		// The far side of the water plane precedes the transparent list in the
		// two views that draw the water surface: the main view and the second
		// scene view. The mirror admits no water surface, so its pair stays two
		// consecutive POST_TRANSPARENT passes.
		world_effects[0]->set_effect_callback_type(
				CompositorEffect::EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT);
		second_scene_effects[0]->set_effect_callback_type(
				CompositorEffect::EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT);
		for_each_overlay([](Ref<SceneOverlayCompositorEffect> &overlay) {
			overlay.instantiate();
		});
		// One device set for every view's overlay pass: a pipeline made for a
		// framebuffer format serves every view of that format.
		for (Ref<SceneOverlayCompositorEffect> *overlay :
				{&reflection_overlay, &second_scene_overlay, &inset_mirror_overlay}) {
			(*overlay)->share_device(world_overlay);
		}
		// The mirror's reflected scene draws its tracer ribbons, then its
		// coronas, after its particle passes; the Inset pass's own mirror draws
		// its own ribbons and closes its target the same way
		// (runtime/renderer/scene_overlay.h kMirrorOverlayOrder,
		// kInsetMirrorOverlayOrder).
		reflection_overlay->set_view_kind(SceneOverlayCompositorEffect::VIEW_MIRROR);
		inset_mirror_overlay->set_view_kind(SceneOverlayCompositorEffect::VIEW_INSET_MIRROR);
		// The second scene view is the weapon Inset pass: its tail draws its
		// own corona walk and no glare (runtime/renderer/scene_overlay.h
		// kInsetOverlayOrder).
		second_scene_overlay->set_view_kind(SceneOverlayCompositorEffect::VIEW_INSET);
		// The distortion subset draws inside FrameFX's type-0 row through the
		// drawer, never as a camera compositor pass.
		distortion_drawer->set_particle_effect(distortion_effect);
	}

	// ParticleRenderer::shutdown() detaches while camera and server ownership
	// are known-live. Late destruction must only discard retained references.
	~Impl() = default;

	void invalidate_catalog() {
		atlas.invalidate();
	}

	void invalidate_environment() {
		cached_environment_source = ObjectID();
		cached_environment_generation =
				std::numeric_limits<std::int64_t>::min();
	}

	void refresh_environment(Node *source) {
		MissionEnvironment *env = Object::cast_to<MissionEnvironment>(source);
		const ObjectID source_id = env != nullptr ?
				ObjectID(env->get_instance_id()) : ObjectID();
		std::int64_t generation =
				std::numeric_limits<std::int64_t>::min();
		bool has_generation = false;
		if (env != nullptr) {
			generation = env->get_scene_generation();
			has_generation = true;
		}
		if (has_generation && source_id == cached_environment_source &&
				generation == cached_environment_generation) {
			return;
		}

		// The environment's engine state; none (a preview) runs unfogged.
		environment = opennova::renderer::particle_pass_environment(
				env != nullptr ? &env->state() : nullptr,
				env != nullptr && env->is_underwater_view());
		cached_environment_source = source_id;
		cached_environment_generation = has_generation ? generation :
				std::numeric_limits<std::int64_t>::min();
	}

	// One view's composed passes, in their frame order: particle pass A, pass
	// B, then the post-particle overlay tail.
	using ViewPassGroup = std::array<Ref<CompositorEffect>, 3>;
	static ViewPassGroup view_group(const ParticleEffectPair &effects,
			const Ref<SceneOverlayCompositorEffect> &overlay) {
		return ViewPassGroup{ effects[0], effects[1], overlay };
	}

	void detach_compositor_group(ObjectID &attached_camera,
			const ViewPassGroup &effects, bool &inherited_compositor) {
		if (!attached_camera.is_valid())
			return;
		const std::uint64_t camera_id =
				static_cast<std::uint64_t>(attached_camera);
		auto &states = particle_camera_compositors();
		auto state_it = states.find(camera_id);
		Camera3D *camera = Object::cast_to<Camera3D>(
				ObjectDB::get_instance(camera_id));
		if (state_it != states.end()) {
			ParticleCameraCompositorState &state = state_it->second;
			state.effects.erase(std::remove_if(
					state.effects.begin(), state.effects.end(),
					[&effects](const auto &entry) {
						return std::any_of(effects.begin(), effects.end(),
								[&entry](const Ref<CompositorEffect> &effect) {
									return effect.is_valid() && entry.first ==
											effect->get_instance_id();
								});
					}), state.effects.end());
			if (state.effects.empty()) {
				if (camera != nullptr &&
						camera->get_compositor() == state.installed) {
					camera->set_compositor(state.explicit_base);
				}
				states.erase(state_it);
			} else if (camera != nullptr &&
					camera->get_compositor() == state.installed) {
				rebuild_camera_compositor(state, camera);
			}
		}
		attached_camera = ObjectID();
		inherited_compositor = false;
	}

	void detach_compositors() {
		detach_compositor_group(attached_world_camera,
				view_group(world_effects, world_overlay), inherited_world_compositor);
		detach_compositor_group(attached_reflection_camera,
				view_group(reflection_effects, reflection_overlay),
				inherited_reflection_compositor);
		detach_compositor_group(attached_second_scene_camera,
				view_group(second_scene_effects, second_scene_overlay),
				inherited_second_scene_compositor);
		detach_compositor_group(attached_inset_mirror_camera,
				view_group(inset_mirror_effects, inset_mirror_overlay),
				inherited_inset_mirror_compositor);
	}

	void attach_compositor_group(Camera3D *camera, Viewport *viewport,
			ObjectID &attached_camera, const ViewPassGroup &effects,
			bool &inherited_compositor) {
		if (camera == nullptr) {
			detach_compositor_group(attached_camera, effects,
					inherited_compositor);
			return;
		}
		const std::uint64_t camera_id = camera->get_instance_id();
		const ObjectID camera_object_id(camera_id);
		if (attached_camera.is_valid() &&
				attached_camera != camera_object_id) {
			detach_compositor_group(attached_camera, effects,
					inherited_compositor);
		}

		auto &states = particle_camera_compositors();
		auto [state_it, inserted] = states.try_emplace(camera_id);
		ParticleCameraCompositorState &state = state_it->second;
		bool rebuild = inserted;
		if (inserted) {
			capture_camera_base(state, camera, viewport);
		} else if (camera->get_compositor() != state.installed) {
			// Another subsystem deliberately replaced our composition. Adopt
			// that as the new base and recompose every still-live renderer.
			capture_camera_base(state, camera, viewport);
			rebuild = true;
		} else if (refresh_camera_base(state, viewport)) {
			// Track both resource replacement and same-resource effect-list
			// replacement while our camera override remains installed.
			rebuild = true;
		}

		for (const Ref<CompositorEffect> &effect : effects) {
			const std::uint64_t effect_id = effect->get_instance_id();
			auto effect_it = std::find_if(state.effects.begin(),
					state.effects.end(), [effect_id](const auto &entry) {
						return entry.first == effect_id;
					});
			if (effect_it == state.effects.end()) {
				state.effects.emplace_back(effect_id, effect);
				rebuild = true;
			} else if (effect_it->second != effect) {
				effect_it->second = effect;
				rebuild = true;
			}
		}
		attached_camera = camera_object_id;
		inherited_compositor = state.inherited_world_compositor;
		if (rebuild)
			rebuild_camera_compositor(state, camera);
	}

	void clear_draws() {
		for_each_effect([](Ref<ParticleCompositorEffect> &effect) {
			effect->clear_submission();
		});
		world_far_pass.clear();
		second_scene_far_pass.clear();
		distortion_drawer->set_particles_present(false);
		first_person.clear();
		slot_present.fill(false);
	}

	// The catalog's atlas and pages, built again; every material bound the
	// old pages (the first-person batch's and pass A's alike).
	void rebuild_catalog(
			const ParticleAtlasDevice::Definitions &definitions,
			const Callable &provider, const String &texture_dir,
			bool procedural_fallback, std::uint32_t page_flags, const Callable &present) {
		materials.clear_materials();
		atlas.rebuild(definitions, provider, texture_dir, procedural_fallback, page_flags,
				present);
	}

	// The distortion commands of a far-side list, for the compositor far
	// effect (the render-list runs take every other command).
	const opennova::renderer::ParticleDrawList &far_distortion_only(
			const opennova::renderer::ParticleDrawList &draw_list) {
		far_distortion_scratch = draw_list;
		auto &commands = far_distortion_scratch.commands;
		commands.erase(std::remove_if(commands.begin(), commands.end(),
							   [](const opennova::renderer::ParticleDrawCommand &command) {
								   return command.pipeline !=
										   opennova::renderer::ParticlePipeline::Distort;
							   }),
				commands.end());
		return far_distortion_scratch;
	}

	// The compositor effect a world slot publishes into (null: the
	// first-person slot, which uploads its own batch).
	Ref<ParticleCompositorEffect> slot_effect(Slot p_slot) const {
		switch (p_slot) {
			case Slot::WorldFarSide: return world_effects[0];
			case Slot::WorldCameraSide: return world_effects[1];
			case Slot::ReflectionFarSide: return reflection_effects[0];
			case Slot::ReflectionCameraSide: return reflection_effects[1];
			case Slot::SecondSceneFarSide: return second_scene_effects[0];
			case Slot::SecondSceneCameraSide: return second_scene_effects[1];
			case Slot::InsetMirrorFarSide: return inset_mirror_effects[0];
			case Slot::InsetMirrorCameraSide: return inset_mirror_effects[1];
			case Slot::Distortion: return distortion_effect;
			case Slot::FirstPerson: break;
		}
		return Ref<ParticleCompositorEffect>();
	}

	// A slot no view compiles this render: nothing published, nothing
	// reported, and (its view's group already detached) nothing drawn.
	void retire_slot(Slot p_slot) {
		const Ref<ParticleCompositorEffect> effect = slot_effect(p_slot);
		if (effect.is_valid())
			effect->clear_submission();
		if (p_slot == Slot::SecondSceneFarSide)
			second_scene_far_pass.clear();
		slot_present[p_slot] = false;
	}

	// One compiled pass into its slot's device target: pass A of the main and
	// the Inset view draws as render-list runs at kRungParticleFarSide
	// (ParticleFarPass) with only its distortion on the compositor; the
	// distortion slot publishes into FrameFX's type-0 row and gates it
	// (renderer::particle_distortion_row_present); the first-person slot
	// uploads its batch; every other slot publishes into its effect.
	void upload_pass(ParticleRenderer *p_owner, const opennova::renderer::ParticlePassCompile &p_pass,
			const opennova::renderer::ParticleDrawList &p_draw_list,
			const ParticleCameraFrame &p_camera, int64_t p_time_ms, bool p_hidden) {
		const bool thermal = p_pass.thermal;
		if (p_pass.slot == Slot::FirstPerson) {
			upload_first_person_draw_list(p_draw_list, p_hidden, thermal);
			return;
		}
		const Ref<ParticleCompositorEffect> effect = slot_effect(p_pass.slot);
		if (p_pass.slot == Slot::WorldFarSide || p_pass.slot == Slot::SecondSceneFarSide) {
			const bool main_view = p_pass.slot == Slot::WorldFarSide;
			ParticleFarPass &far_pass = main_view ? world_far_pass : second_scene_far_pass;
			far_pass.upload(p_owner, p_draw_list,
					main_view ? ParticleFarPass::View::Main : ParticleFarPass::View::SecondScene,
					[this, thermal](const opennova::renderer::ParticleDrawCommand &command) {
						// Pass A binds the secondary materials too while the
						// main scene's thermal byte is set.
						return materials.far_material_for(command,
								opennova::renderer::particle_thermal_material(
										command.pipeline, thermal),
								atlas.pages());
					});
			publish_world_draw_list(effect, far_distortion_only(p_draw_list),
					p_camera.position, p_camera.forward, p_time_ms, thermal);
			return;
		}
		publish_world_draw_list(effect, p_draw_list, p_camera.position, p_camera.forward,
				p_time_ms, thermal);
		if (p_pass.slot == Slot::Distortion) {
			distortion_drawer->set_particles_present(
					opennova::renderer::particle_distortion_row_present(p_draw_list.debug));
			// The pass fog rides along for the tracer distortion ribbons.
			const opennova::renderer::ParticlePassFog &fog = environment.fog;
			distortion_drawer->set_pass_fog(fog.color, fog.start, fog.end, fog.type);
		}
	}

	void publish_world_draw_list(const Ref<ParticleCompositorEffect> &effect,
			const opennova::renderer::ParticleDrawList &draw_list,
			const Vector3 &camera_position, const Vector3 &camera_forward, int64_t time_ms,
			bool thermal) {
		auto submission = std::make_shared<ParticleWorldSubmission>();
		submission->frame_id = draw_list.frame_id;
		submission->thermal = thermal;
		submission->time_ms = static_cast<uint32_t>(time_ms);
		submission->commands = draw_list.commands;
		submission->atlas = atlas.snapshot();
		for (std::size_t component = 0; component < 3; ++component) {
			submission->camera_position[component] =
					camera_position[static_cast<int>(component)];
			submission->camera_forward[component] =
					camera_forward[static_cast<int>(component)];
		}
		const bool water_mirror = effect == reflection_effects[0] ||
				effect == reflection_effects[1] || effect == inset_mirror_effects[0] ||
				effect == inset_mirror_effects[1];
		const opennova::renderer::ParticlePassFog &fog =
				water_mirror ? environment.mirror_fog : environment.fog;
		submission->fog_color = fog.color;
		submission->fog_start = fog.start;
		submission->fog_end = fog.end;
		submission->fog_type = fog.type;
		if (draw_list.domain != opennova::renderer::ParticleRenderDomain::World) {
			submission->valid = false;
			submission->validation_error =
					"World backend received a non-World compiler draw_list";
		} else if (draw_list.vertices.size() % 4u != 0) {
			submission->valid = false;
			submission->validation_error =
					"Compiler draw_list does not contain complete particle quads";
		} else {
			const std::size_t quad_count = draw_list.vertices.size() / 4u;
			constexpr std::size_t expanded_bytes_per_quad =
					6u * sizeof(opennova::renderer::ParticleVertex);
			if (quad_count > static_cast<std::size_t>(
					std::numeric_limits<int64_t>::max()) /
					expanded_bytes_per_quad) {
				submission->valid = false;
				submission->validation_error =
						"Expanded World draw_list exceeds PackedByteArray limits";
			} else {
				submission->triangle_vertices.resize(static_cast<int64_t>(
						quad_count * expanded_bytes_per_quad));
				std::uint8_t *destination =
						submission->triangle_vertices.ptrw();
				// Retail's quad index pattern 0/1/2/1/3/2 (CParticleEmitter_BuildBillboardQuads
				// @0x5e6d60 - docs/particles/ptl-format-re.md): the first triangle
				// is the quad's contiguous first three vertices, so it copies
				// as one block.
				constexpr std::size_t stride = sizeof(opennova::renderer::ParticleVertex);
				for (std::size_t quad = 0; quad < quad_count; ++quad) {
					const opennova::renderer::ParticleVertex *source =
							draw_list.vertices.data() + quad * 4u;
					std::uint8_t *target = destination +
							quad * expanded_bytes_per_quad;
					std::memcpy(target, source, 3u * stride);
					std::memcpy(target + 3u * stride, source + 1, stride);
					std::memcpy(target + 4u * stride, source + 3, stride);
					std::memcpy(target + 5u * stride, source + 2, stride);
				}
			}
		}
		effect->publish(
				std::shared_ptr<const ParticleWorldSubmission>(
						std::move(submission)));
	}

	void upload_first_person_draw_list(
			const opennova::renderer::ParticleDrawList &draw_list, bool hidden, bool thermal) {
		first_person.upload(draw_list, hidden,
				[this, thermal](const opennova::renderer::ParticleDrawCommand &command) {
					return materials.material_for(command, thermal, atlas.pages());
				});
	}
};

ParticleRenderer::ParticleRenderer() : impl_(std::make_unique<Impl>()) {}

ParticleRenderer::~ParticleRenderer() = default;

void ParticleRenderer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("warm_pipelines", "position"),
			&ParticleRenderer::warm_pipelines);
	ClassDB::bind_method(D_METHOD("set_scene", "scene"),
			&ParticleRenderer::set_scene);
	ClassDB::bind_method(D_METHOD("get_scene"),
			&ParticleRenderer::get_scene);
	ClassDB::bind_method(D_METHOD("set_image_provider", "provider"),
			&ParticleRenderer::set_image_provider);
	ClassDB::bind_method(D_METHOD("get_image_provider"),
			&ParticleRenderer::get_image_provider);
	ClassDB::bind_method(D_METHOD("set_environment_source", "source"),
			&ParticleRenderer::set_environment_source);
	ClassDB::bind_method(D_METHOD("get_environment_source"),
			&ParticleRenderer::get_environment_source);
	ClassDB::bind_method(D_METHOD("set_water_plane", "height", "reflection_camera"),
			&ParticleRenderer::set_water_plane);
	ClassDB::bind_method(D_METHOD("set_second_scene_camera", "camera"),
			&ParticleRenderer::set_second_scene_camera);
	ClassDB::bind_method(D_METHOD("set_session_render_settings", "texcompression_level",
								 "particle_density"),
			&ParticleRenderer::set_session_render_settings);
	ClassDB::bind_method(D_METHOD("set_main_view_nvg_scene", "nvg_scene"),
			&ParticleRenderer::set_main_view_nvg_scene);
	ClassDB::bind_method(D_METHOD("get_second_scene_camera"),
			&ParticleRenderer::get_second_scene_camera);
	ClassDB::bind_method(D_METHOD("set_inset_mirror_camera", "camera"),
			&ParticleRenderer::set_inset_mirror_camera);
	ClassDB::bind_method(D_METHOD("get_inset_mirror_camera"),
			&ParticleRenderer::get_inset_mirror_camera);
	ClassDB::bind_method(D_METHOD("set_hidden", "hidden"),
			&ParticleRenderer::set_hidden);
	ClassDB::bind_method(D_METHOD("get_hidden"),
			&ParticleRenderer::get_hidden);
	ClassDB::bind_method(D_METHOD("set_procedural_fallback_enabled", "enabled"),
			&ParticleRenderer::set_procedural_fallback_enabled);
	ClassDB::bind_method(D_METHOD("get_procedural_fallback_enabled"),
			&ParticleRenderer::get_procedural_fallback_enabled);
	ClassDB::bind_method(D_METHOD("render_now", "time_ms"),
			&ParticleRenderer::render_now);
	ClassDB::bind_method(D_METHOD("build_catalog", "present"),
			&ParticleRenderer::build_catalog, DEFVAL(Callable()));
	ClassDB::bind_method(D_METHOD("shutdown"), &ParticleRenderer::shutdown);
	ClassDB::bind_method(D_METHOD("attach_distortion_row", "frame_fx"),
			&ParticleRenderer::attach_distortion_row);
	ClassDB::bind_method(D_METHOD("get_rendered_quad_count"),
			&ParticleRenderer::get_rendered_quad_count);
	ClassDB::bind_method(D_METHOD("get_draw_command_count"),
			&ParticleRenderer::get_draw_command_count);
	ClassDB::bind_method(D_METHOD("get_debug_draw_list_report"),
			&ParticleRenderer::get_debug_draw_list_report);
	ClassDB::bind_method(D_METHOD("get_scene_overlay_slot_positions", "slot"),
			&ParticleRenderer::get_scene_overlay_slot_positions);
	ClassDB::bind_method(D_METHOD("get_debug_emitter_bounds"),
			&ParticleRenderer::get_debug_emitter_bounds);
	ClassDB::bind_method(D_METHOD("get_unresolved_texture_names"),
			&ParticleRenderer::get_unresolved_texture_names);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "scene"),
			"set_scene", "get_scene");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "image_provider"),
			"set_image_provider", "get_image_provider");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "environment_source",
			PROPERTY_HINT_NODE_TYPE, "Node"),
			"set_environment_source", "get_environment_source");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "hidden"),
			"set_hidden", "get_hidden");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "procedural_fallback_enabled"),
			"set_procedural_fallback_enabled",
			"get_procedural_fallback_enabled");
}

void ParticleRenderer::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		_restore_device_state();
	} else if (p_what == NOTIFICATION_READY) {
		impl_->first_person.ensure(this);
		set_process(false);
		render_now(GameWorld::current_frame_clock_ms());
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		shutdown();
	}
}

// The re-entry half of the EXIT_TREE/shutdown contract (the same shape
// DisplayDecode follows: release on exit, recreate on entry). A renderer that
// left the tree, or was released explicitly, holds retired compositor effects
// and a latched shutdown_; entering the tree again replaces the effects and
// clears the latch so the next render_now attaches and publishes as on the
// first entry. The retained CPU state (catalog, atlas snapshot, first-person
// batch child) is untouched: the new effects re-upload from it on publish.
void ParticleRenderer::_restore_device_state() {
	if (!shutdown_)
		return;
	shutdown_ = false;
	if (!impl_)
		return;
	impl_->create_effects();
	const bool hidden = hidden_;
	impl_->for_each_effect([hidden](Ref<ParticleCompositorEffect> &effect) {
		effect->set_particles_hidden(hidden);
	});
}

void ParticleRenderer::_invalidate_catalog() {
	if (impl_)
		impl_->invalidate_catalog();
}

void ParticleRenderer::set_scene(const Ref<EffectScene> &p_scene) {
	if (scene_ == p_scene)
		return;
	scene_ = p_scene;
	_invalidate_catalog();
	if (scene_.is_null() && impl_)
		impl_->clear_draws();
}

Ref<EffectScene> ParticleRenderer::get_scene() const {
	return scene_;
}

void ParticleRenderer::set_image_provider(const Callable &p_provider) {
	image_provider_ = p_provider;
	_invalidate_catalog();
}

Callable ParticleRenderer::get_image_provider() const {
	return image_provider_;
}

void ParticleRenderer::set_texture_dir(const String &p_texture_dir) {
	if (texture_dir_ == p_texture_dir)
		return;
	texture_dir_ = p_texture_dir;
	_invalidate_catalog();
}

void ParticleRenderer::set_environment_source(Node *p_source) {
	const ObjectID next = p_source != nullptr ?
			ObjectID(p_source->get_instance_id()) : ObjectID();
	if (environment_source_ == next)
		return;
	environment_source_ = next;
	if (impl_)
		impl_->invalidate_environment();
}

Node *ParticleRenderer::get_environment_source() const {
	if (!environment_source_.is_valid())
		return nullptr;
	return Object::cast_to<Node>(ObjectDB::get_instance(
			static_cast<std::uint64_t>(environment_source_)));
}

void ParticleRenderer::set_water_plane(float p_height,
		Camera3D *p_reflection_camera) {
	water_height_ = p_height;
	reflection_camera_ = p_reflection_camera != nullptr ?
			ObjectID(p_reflection_camera->get_instance_id()) : ObjectID();
}

void ParticleRenderer::set_session_render_settings(int p_texcompression_level,
		int p_particle_density) {
	session_texcompression_level_ = p_texcompression_level;
	session_particle_density_ = opennova::renderer::clamp_particle_density(p_particle_density);
}

void ParticleRenderer::set_main_view_nvg_scene(bool p_nvg_scene) {
	main_view_nvg_scene_ = p_nvg_scene;
}

void ParticleRenderer::set_second_scene_camera(Camera3D *p_camera) {
	second_scene_camera_ = p_camera != nullptr ?
			ObjectID(p_camera->get_instance_id()) : ObjectID();
}

Camera3D *ParticleRenderer::get_second_scene_camera() const {
	if (!second_scene_camera_.is_valid())
		return nullptr;
	return Object::cast_to<Camera3D>(ObjectDB::get_instance(
			static_cast<std::uint64_t>(second_scene_camera_)));
}

void ParticleRenderer::set_inset_mirror_camera(Camera3D *p_camera) {
	inset_mirror_camera_ = p_camera != nullptr ?
			ObjectID(p_camera->get_instance_id()) : ObjectID();
}

Camera3D *ParticleRenderer::get_inset_mirror_camera() const {
	if (!inset_mirror_camera_.is_valid())
		return nullptr;
	return Object::cast_to<Camera3D>(ObjectDB::get_instance(
			static_cast<std::uint64_t>(inset_mirror_camera_)));
}

void ParticleRenderer::warm_pipelines(const Vector3 &p_position) {
	clear_warm_pipelines();
	if (!impl_)
		return;
	impl_->for_each_effect([](Ref<ParticleCompositorEffect> &effect) {
		effect->request_pipeline_warm();
	});
	// Every overlay combine's pipeline, made in the same frame for each view format that frame
	// draws (D-NET-1375; renderer/scene_overlay.h kSceneOverlayPipelineStates carries the
	// witness).
	impl_->for_each_overlay([](Ref<SceneOverlayCompositorEffect> &overlay) {
		overlay->request_pipeline_warm();
	});
	Ref<QuadMesh> quad;
	quad.instantiate();
	quad->set_size(Vector2(0.01f, 0.01f));
	for (std::size_t i = 0; i < ParticleMaterialCache::kShaderCount; ++i) {
		Ref<Shader> shader = impl_->materials.shader_for_index(i);
		if (shader.is_null())
			continue;
		Ref<ShaderMaterial> material;
		material.instantiate();
		material->set_shader(shader);
		MeshInstance3D *quad_instance = memnew(MeshInstance3D);
		quad_instance->set_mesh(quad);
		quad_instance->set_material_override(material);
		quad_instance->set_cast_shadows_setting(
				GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		add_child(quad_instance);
		quad_instance->set_global_position(p_position);
		warm_nodes_.push_back(quad_instance);
	}
}

void ParticleRenderer::clear_warm_pipelines() {
	if (impl_) {
		impl_->for_each_effect([](Ref<ParticleCompositorEffect> &effect) {
			effect->cancel_pipeline_warm();
		});
		impl_->for_each_overlay([](Ref<SceneOverlayCompositorEffect> &overlay) {
			overlay->cancel_pipeline_warm();
		});
	}
	for (Node *node : warm_nodes_) {
		if (node != nullptr)
			node->queue_free();
	}
	warm_nodes_.clear();
}

void ParticleRenderer::clear_draws() {
	if (impl_)
		impl_->clear_draws();
}

void ParticleRenderer::shutdown() {
	if (shutdown_)
		return;
	shutdown_ = true;
	clear_warm_pipelines();
	if (!impl_)
		return;

	impl_->clear_draws();
	impl_->for_each_effect([](Ref<ParticleCompositorEffect> &effect) {
		effect->set_enabled(false);
	});
	impl_->for_each_overlay([](Ref<SceneOverlayCompositorEffect> &overlay) {
		overlay->set_enabled(false);
		overlay->clear_submission();
	});
	scene_overlay_submission_.reset();
	impl_->detach_compositors();

	// Detaching affects the next render setup. Drain a callback already queued
	// on the render thread before releasing the RIDs it can still consume.
	RenderingServer *server = RenderingServer::get_singleton();
	if (server != nullptr && server->get_rendering_device() != nullptr)
		server->force_sync();
	impl_->for_each_effect([](Ref<ParticleCompositorEffect> &effect) {
		effect->release_device_resources();
	});
	impl_->for_each_overlay([](Ref<SceneOverlayCompositorEffect> &overlay) {
		overlay->release_device_resources();
	});
	impl_->distortion_drawer->release_device_resources();
}

std::shared_ptr<EffectDistortionDrawer> ParticleRenderer::distortion_drawer() const {
	return impl_ ? impl_->distortion_drawer : std::shared_ptr<EffectDistortionDrawer>();
}

void ParticleRenderer::attach_distortion_row(Node *p_frame_fx) {
	if (FrameFx *frame_fx = Object::cast_to<FrameFx>(p_frame_fx))
		frame_fx->set_distortion_drawer(distortion_drawer());
}

void ParticleRenderer::create_particle_devices() {
	if (shutdown_ || !impl_)
		return;
	impl_->for_each_effect([](Ref<ParticleCompositorEffect> &effect) {
		effect->create_device();
	});
}

void ParticleRenderer::create_tracer_pool_devices() {
	if (shutdown_ || !impl_)
		return;
	call_device_create(callable_mp(this, &ParticleRenderer::_create_ribbon_device_on_render_thread));
	impl_->for_each_overlay([](Ref<SceneOverlayCompositorEffect> &overlay) {
		overlay->create_device();
	});
}

void ParticleRenderer::_create_ribbon_device_on_render_thread() {
	RenderingServer *server = RenderingServer::get_singleton();
	if (!shutdown_ && impl_ && server != nullptr)
		impl_->distortion_drawer->create_device(server->get_rendering_device());
}

void ParticleRenderer::publish_scene_overlay(
		const std::shared_ptr<const SceneOverlaySubmission> &p_submission) {
	if (shutdown_ || !impl_)
		return;
	scene_overlay_submission_ = p_submission;
	impl_->for_each_overlay([&p_submission](Ref<SceneOverlayCompositorEffect> &overlay) {
		overlay->publish(p_submission);
	});
}

PackedVector3Array ParticleRenderer::get_scene_overlay_slot_positions(int p_slot) const {
	PackedVector3Array positions;
	if (!scene_overlay_submission_) {
		return positions;
	}
	const opennova::renderer::SceneOverlayFrame &frame = scene_overlay_submission_->frame;
	for (const opennova::renderer::SceneOverlayBatch &batch : frame.batches) {
		if (static_cast<int>(batch.slot) != p_slot) {
			continue;
		}
		for (uint32_t i = 0; i < batch.vertex_count; ++i) {
			const opennova::renderer::SceneOverlayVertex &v = frame.vertices[batch.first_vertex + i];
			positions.push_back(Vector3(v.position[0], v.position[1], v.position[2]));
		}
	}
	return positions;
}

void ParticleRenderer::set_hidden(bool p_hidden) {
	if (hidden_ == p_hidden)
		return;
	hidden_ = p_hidden;
	if (!impl_)
		return;
	impl_->for_each_effect([p_hidden](Ref<ParticleCompositorEffect> &effect) {
		effect->set_particles_hidden(p_hidden);
	});
	if (hidden_) {
		// The master switch is also a CPU switch: drop retained submissions once
		// and let the next visible process rebuild from the latest scene frame.
		impl_->clear_draws();
		return;
	}
	impl_->first_person.set_hidden(hidden_);
}

bool ParticleRenderer::get_hidden() const {
	return hidden_;
}

void ParticleRenderer::set_procedural_fallback_enabled(bool p_enabled) {
	if (procedural_fallback_enabled_ == p_enabled)
		return;
	procedural_fallback_enabled_ = p_enabled;
	_invalidate_catalog();
}

bool ParticleRenderer::get_procedural_fallback_enabled() const {
	return procedural_fallback_enabled_;
}

void ParticleRenderer::build_catalog(const Callable &p_present) {
	if (shutdown_ || !impl_ || scene_.is_null())
		return;
	const opennova::particle::ParticleFrameSnapshot &frame =
			scene_->native_frame_snapshot();
	const std::uint32_t page_flags = opennova::renderer::particle_atlas_page_creation_flags(
			session_texcompression_level_, session_particle_density_);
	if (impl_->atlas.needs_rebuild(frame.definitions, page_flags)) {
		impl_->rebuild_catalog(frame.definitions, image_provider_, texture_dir_,
				procedural_fallback_enabled_, page_flags, p_present);
	}
}

void ParticleRenderer::render_now(int64_t p_time_ms) {
	if (shutdown_ || !impl_)
		return;
	impl_->first_person.ensure(this);
	Viewport *viewport = get_viewport();
	Camera3D *camera = viewport != nullptr ? viewport->get_camera_3d() : nullptr;
	Camera3D *reflection_camera = reflection_camera_.is_valid() ?
			Object::cast_to<Camera3D>(ObjectDB::get_instance(
					static_cast<std::uint64_t>(reflection_camera_))) : nullptr;
	if (reflection_camera == camera)
		reflection_camera = nullptr;
	// A camera that already owns the World or the mirror pair keeps that one,
	// and a camera outside the tree renders nothing (and has no transform).
	Camera3D *second_scene_camera = get_second_scene_camera();
	if (second_scene_camera != nullptr && (second_scene_camera == camera ||
			second_scene_camera == reflection_camera ||
			!second_scene_camera->is_inside_tree()))
		second_scene_camera = nullptr;
	Camera3D *inset_mirror_camera = get_inset_mirror_camera();
	if (inset_mirror_camera != nullptr && (inset_mirror_camera == camera ||
			inset_mirror_camera == reflection_camera ||
			inset_mirror_camera == second_scene_camera ||
			!inset_mirror_camera->is_inside_tree()))
		inset_mirror_camera = nullptr;
	// Attached even while the particles are hidden: the hidden particle passes
	// are disabled, and each view's overlay pass (not particles) still draws.
	impl_->attach_compositor_group(camera, viewport,
			impl_->attached_world_camera,
			Impl::view_group(impl_->world_effects, impl_->world_overlay),
			impl_->inherited_world_compositor);
	impl_->attach_compositor_group(reflection_camera,
			reflection_camera != nullptr ? reflection_camera->get_viewport() : nullptr,
			impl_->attached_reflection_camera,
			Impl::view_group(impl_->reflection_effects, impl_->reflection_overlay),
			impl_->inherited_reflection_compositor);
	impl_->attach_compositor_group(second_scene_camera,
			second_scene_camera != nullptr ?
					second_scene_base_viewport(second_scene_camera, viewport) : nullptr,
			impl_->attached_second_scene_camera,
			Impl::view_group(impl_->second_scene_effects, impl_->second_scene_overlay),
			impl_->inherited_second_scene_compositor);
	impl_->attach_compositor_group(inset_mirror_camera,
			inset_mirror_camera != nullptr ? inset_mirror_camera->get_viewport() : nullptr,
			impl_->attached_inset_mirror_camera,
			Impl::view_group(impl_->inset_mirror_effects, impl_->inset_mirror_overlay),
			impl_->inherited_inset_mirror_compositor);
	if (hidden_)
		return;
	if (scene_.is_null()) {
		impl_->clear_draws();
		return;
	}

	build_catalog();
	const opennova::particle::ParticleFrameSnapshot &frame =
			scene_->native_frame_snapshot();
	// Each view's particle stride from the session's density (D-RMAT-24;
	// renderer::particle_frame_strides).
	const opennova::renderer::ParticleFrameStrides strides = opennova::renderer::particle_frame_strides(
			session_particle_density_, main_view_nvg_scene_, impl_->last_pass_lod_divisor);
	impl_->last_pass_lod_divisor = strides.last_pass;

	const ParticleCameraFrame world_camera = particle_camera_frame(camera);
	const bool camera_above_water = world_camera.position.y >= water_height_;
	impl_->refresh_environment(get_environment_source());
	// The main scene hands its particle passes the frame's thermal byte; every
	// other scene call passes zero (renderer::particle_thermal_material).
	const MissionEnvironment *thermal_source =
			Object::cast_to<MissionEnvironment>(get_environment_source());
	const bool thermal = thermal_source != nullptr && thermal_source->is_thermal_view();
	opennova::renderer::build_particle_render_input(frame, impl_->atlas.definition_visuals(),
			impl_->atlas.entries(), basis_rows(world_camera.view_basis),
			impl_->environment.tints, impl_->render_input);

	// The device state each view's compiles read: its camera frame (a mirror
	// culls over its own square, renderer::ParticleViewInput::frustum) and
	// whether the frame renders it.
	auto mirror_rendered = [](Camera3D *p_mirror) {
		const SubViewport *mirror_viewport = p_mirror != nullptr ?
				Object::cast_to<SubViewport>(p_mirror->get_viewport()) : nullptr;
		return mirror_viewport == nullptr ||
				mirror_viewport->get_update_mode() != SubViewport::UPDATE_DISABLED;
	};
	std::array<ParticleCameraFrame, 4> view_cameras{};
	view_cameras[static_cast<std::size_t>(opennova::renderer::ParticleFrameView::Main)] = world_camera;
	opennova::renderer::ParticleFrameViews views;
	views.main_above_water = camera_above_water;
	views.thermal = thermal;
	views.strides = strides;
	views.mirror = reflection_camera != nullptr;
	views.mirror_rendered = mirror_rendered(reflection_camera);
	if (views.mirror) {
		ParticleCameraFrame &mirror = view_cameras[static_cast<std::size_t>(
				opennova::renderer::ParticleFrameView::Mirror)];
		mirror = particle_camera_frame(reflection_camera);
		mirror.frustum = mirror_cull_frustum(reflection_camera);
	}
	views.inset = second_scene_camera != nullptr;
	if (views.inset) {
		// The original runs its one scene routine again for this camera, so the
		// far/camera-side bracket belongs to this view's own eye.
		const ParticleCameraFrame second_camera = particle_camera_frame(second_scene_camera);
		views.inset_above_water = second_camera.position.y >= water_height_;
		view_cameras[static_cast<std::size_t>(opennova::renderer::ParticleFrameView::Inset)] =
				second_camera;
	}
	views.inset_mirror = inset_mirror_camera != nullptr;
	views.inset_mirror_rendered = mirror_rendered(inset_mirror_camera);
	if (views.inset_mirror) {
		ParticleCameraFrame &mirror = view_cameras[static_cast<std::size_t>(
				opennova::renderer::ParticleFrameView::InsetMirror)];
		mirror = particle_camera_frame(inset_mirror_camera);
		mirror.frustum = mirror_cull_frustum(inset_mirror_camera);
	}

	// The engine's frame plan, executed in order: each compile builds its
	// eye's view input and uploads into that slot's device target.
	const opennova::renderer::ParticleFramePlan plan = opennova::renderer::plan_particle_frame(views);
	for (std::size_t index = 0; index < plan.compile_count; ++index) {
		const opennova::renderer::ParticlePassCompile &pass = plan.compiles[index];
		const ParticleCameraFrame &view_camera = view_cameras[static_cast<std::size_t>(pass.view)];
		if (pass.relight)
			opennova::renderer::relight_particle_render_input(impl_->render_input,
					basis_rows(view_camera.view_basis));
		opennova::renderer::ParticleViewInput view;
		view.lod_divisor = pass.lod_divisor;
		view.domain = pass.domain;
		view.water_subset = pass.water_subset;
		view.water_height = water_height_;
		view.position = particle_vec(view_camera.position);
		view.right = particle_vec(view_camera.right);
		view.up = particle_vec(view_camera.up);
		view.forward = particle_vec(view_camera.forward);
		std::copy(view_camera.projection.begin(), view_camera.projection.end(),
				view.projection);
		view.projection_valid = view_camera.projection_valid;
		view.projection_near_is_one = true;
		view.frustum = view_camera.frustum;
		const opennova::renderer::ParticleDrawList &draw_list =
				impl_->compilers[pass.slot].compile(impl_->render_input.snapshot, view);
		impl_->slot_present[pass.slot] = true;
		impl_->upload_pass(this, pass, draw_list, view_camera, p_time_ms, hidden_);
	}
	for (std::size_t index = 0; index < plan.retired_count; ++index)
		impl_->retire_slot(plan.retired[index]);
}

int64_t ParticleRenderer::get_rendered_quad_count() const {
	if (!impl_)
		return 0;
	std::size_t total = 0;
	for (const Slot slot : {
			Slot::WorldFarSide, Slot::WorldCameraSide, Slot::FirstPerson}) {
		if (impl_->slot_present[slot])
			total += impl_->compilers[slot].draw_list().debug.emitted_quads;
	}
	return static_cast<int64_t>(total);
}

int64_t ParticleRenderer::get_draw_command_count() const {
	if (!impl_)
		return 0;
	std::size_t total = 0;
	for (const Slot slot : {
			Slot::WorldFarSide, Slot::WorldCameraSide, Slot::FirstPerson}) {
		if (impl_->slot_present[slot])
			total += impl_->compilers[slot].draw_list().debug.draw_commands;
	}
	return static_cast<int64_t>(total);
}

Dictionary ParticleRenderer::get_debug_draw_list_report() const {
	Dictionary result;
	if (!impl_)
		return result;
	auto slot_report = [this](Slot slot) {
		return impl_->slot_present[slot] ?
				draw_list_report(impl_->compilers[slot].draw_list()) :
				Dictionary();
	};
	result["world_far_side"] = slot_report(Slot::WorldFarSide);
	// The render-list runs pass A drew this render (ParticleFarPass).
	result["world_far_render_runs"] =
			static_cast<int64_t>(impl_->world_far_pass.run_count());
	result["second_scene_far_render_runs"] =
			static_cast<int64_t>(impl_->second_scene_far_pass.run_count());
	result["world_camera_side"] = slot_report(Slot::WorldCameraSide);
	result["reflection_far_side"] = slot_report(Slot::ReflectionFarSide);
	result["reflection_camera_side"] = slot_report(Slot::ReflectionCameraSide);
	result["second_scene_far_side"] = slot_report(Slot::SecondSceneFarSide);
	result["second_scene_camera_side"] = slot_report(Slot::SecondSceneCameraSide);
	result["inset_mirror_far_side"] = slot_report(Slot::InsetMirrorFarSide);
	result["inset_mirror_camera_side"] = slot_report(Slot::InsetMirrorCameraSide);
	result["first_person"] = slot_report(Slot::FirstPerson);
	result["distortion"] = slot_report(Slot::Distortion);
	result["distortion_backend"] = impl_->distortion_effect->get_backend_report();
	result["distortion_present"] = impl_->distortion_drawer->frame_has_distortion();
	result["distortion_ribbon_indices"] = static_cast<int64_t>(
			impl_->distortion_drawer->published_ribbon_indices());
	result["distortion_ribbon_draws"] = static_cast<int64_t>(
			impl_->distortion_drawer->drawn_ribbon_draws());
	result["world_far_backend"] =
			impl_->world_effects[0]->get_backend_report();
	result["world_camera_backend"] =
			impl_->world_effects[1]->get_backend_report();
	result["reflection_far_backend"] =
			impl_->reflection_effects[0]->get_backend_report();
	result["reflection_camera_backend"] =
			impl_->reflection_effects[1]->get_backend_report();
	result["second_scene_far_backend"] =
			impl_->second_scene_effects[0]->get_backend_report();
	result["second_scene_camera_backend"] =
			impl_->second_scene_effects[1]->get_backend_report();
	result["inset_mirror_far_backend"] =
			impl_->inset_mirror_effects[0]->get_backend_report();
	result["inset_mirror_camera_backend"] =
			impl_->inset_mirror_effects[1]->get_backend_report();
	Dictionary world_overlay;
	impl_->world_overlay->write_backend_report(world_overlay);
	result["world_overlay_backend"] = world_overlay;
	Dictionary reflection_overlay;
	impl_->reflection_overlay->write_backend_report(reflection_overlay);
	result["reflection_overlay_backend"] = reflection_overlay;
	Dictionary second_scene_overlay;
	impl_->second_scene_overlay->write_backend_report(second_scene_overlay);
	result["second_scene_overlay_backend"] = second_scene_overlay;
	Dictionary inset_mirror_overlay;
	impl_->inset_mirror_overlay->write_backend_report(inset_mirror_overlay);
	result["inset_mirror_overlay_backend"] = inset_mirror_overlay;
	result["first_person_backend"] = "array_mesh_fallback_tool_only";
	result["world_compositor_attached"] =
			impl_->attached_world_camera.is_valid();
	result["reflection_compositor_attached"] =
			impl_->attached_reflection_camera.is_valid();
	result["second_scene_compositor_attached"] =
			impl_->attached_second_scene_camera.is_valid();
	result["inset_mirror_compositor_attached"] =
			impl_->attached_inset_mirror_camera.is_valid();
	result["world_compositor_inherited_effects"] =
			impl_->inherited_world_compositor;
	result["reflection_compositor_inherited_effects"] =
			impl_->inherited_reflection_compositor;
	result["second_scene_compositor_inherited_effects"] =
			impl_->inherited_second_scene_compositor;
	result["world_mesh_instance"] = false;
	result["shutdown"] = shutdown_;
	result["water_height"] = water_height_;
	Dictionary environment_fog;
	const opennova::renderer::ParticlePassFog &fog = impl_->environment.fog;
	environment_fog["color"] = Vector3(fog.color[0], fog.color[1], fog.color[2]);
	environment_fog["start"] = fog.start;
	environment_fog["end"] = fog.end;
	environment_fog["type"] = fog.type;
	result["environment_fog"] = environment_fog;
	const std::vector<ParticleAtlasDevice::Page> &pages = impl_->atlas.pages();
	const std::vector<opennova::renderer::ParticleAtlasEntry> &entries = impl_->atlas.entries();
	result["atlas_page_count"] = static_cast<int64_t>(pages.size());
	result["atlas_generation"] = static_cast<int64_t>(impl_->atlas.generation());
	result["atlas_entry_count"] = static_cast<int64_t>(entries.size());
	std::size_t resolved_entries = 0;
	for (const opennova::renderer::ParticleAtlasEntry &entry : entries) {
		if (entry.placement.valid)
			++resolved_entries;
	}
	result["atlas_resolved_entry_count"] =
			static_cast<int64_t>(resolved_entries);
	result["atlas_rejected_entry_count"] =
			static_cast<int64_t>(impl_->atlas.rejected_entries());
	Array atlas_pages;
	atlas_pages.resize(static_cast<int64_t>(pages.size()));
	for (std::size_t i = 0; i < pages.size(); ++i) {
		Dictionary value;
		value["page"] = static_cast<int64_t>(i);
		value["type"] = static_cast<int>(pages[i].type);
		value["side"] = pages[i].side;
		value["levels"] = pages[i].levels;
		value["max_lod"] = pages[i].max_lod;
		value["texture_mipmaps"] = pages[i].mipmapped;
		value["device_side"] = pages[i].device_side;
		value["dxt5"] = pages[i].dxt5;
		atlas_pages[static_cast<int64_t>(i)] = value;
	}
	result["atlas_pages"] = atlas_pages;
	result["atlas_page_flags"] = static_cast<int64_t>(impl_->atlas.page_flags());
	result["session_texcompression_level"] = session_texcompression_level_;
	result["session_particle_density"] = session_particle_density_;
	result["main_view_nvg_scene"] = main_view_nvg_scene_;
	Array atlas_entries;
	atlas_entries.resize(static_cast<int64_t>(entries.size()));
	for (std::size_t i = 0; i < entries.size(); ++i) {
		const opennova::renderer::ParticleAtlasEntry &entry = entries[i];
		Dictionary value;
		value["name"] = opennova::to_gd(entry.name);
		value["type"] = static_cast<int>(entry.type);
		value["width"] = entry.width;
		value["height"] = entry.height;
		value["resolved"] = entry.placement.valid;
		if (entry.placement.valid) {
			value["page"] = static_cast<int64_t>(entry.placement.page);
			value["pixel_rect"] = Rect2i(entry.placement.x, entry.placement.y,
					entry.width, entry.height);
			// Preserve the scalar diagnostics key as the horizontal inset while
			// exposing the per-axis values used for tiny atlas frames.
			value["inset"] = entry.placement.inset_u;
			value["inset_u"] = entry.placement.inset_u;
			value["inset_v"] = entry.placement.inset_v;
		}
		atlas_entries[static_cast<int64_t>(i)] = value;
	}
	result["atlas_entries"] = atlas_entries;
	result["unresolved_texture_count"] =
			static_cast<int64_t>(impl_->atlas.unresolved_names().size());
	result["hidden"] = hidden_;
	result["procedural_fallback_enabled"] = procedural_fallback_enabled_;
	return result;
}

Array ParticleRenderer::get_debug_emitter_bounds() const {
	Array result;
	if (!impl_)
		return result;
	for (const Slot slot : {
			Slot::WorldFarSide, Slot::WorldCameraSide, Slot::FirstPerson}) {
		if (!impl_->slot_present[slot])
			continue;
		for (const opennova::renderer::ParticleEmitterDrawBounds &bounds :
				impl_->compilers[slot].draw_list().emitter_bounds) {
			Dictionary value;
			value["emitter_id"] = token_to_godot(bounds.emitter_id);
			value["render_domain"] = slot == Slot::FirstPerson ?
					static_cast<int>(opennova::renderer::ParticleRenderDomain::FirstPerson) :
					static_cast<int>(opennova::renderer::ParticleRenderDomain::World);
			value["draw_scope"] = slot == Slot::WorldFarSide ?
					String("world_far_side") :
					(slot == Slot::WorldCameraSide ? String("world_camera_side") :
							String("first_person"));
			value["first_quad"] = static_cast<int64_t>(bounds.first_quad);
			value["quad_count"] = static_cast<int64_t>(bounds.quad_count);
			value["bounds_valid"] = bounds.bounds.valid;
			value["bounds"] = bounds.bounds.valid ? godot_aabb(bounds.bounds) : AABB();
			result.push_back(value);
		}
	}
	return result;
}

void ParticleRenderer::collect_debug_emitter_bounds(
		std::vector<opennova::renderer::ParticleEmitterDrawBounds> &r_out) const {
	r_out.clear();
	if (!impl_)
		return;
	for (const Slot slot : {
			Slot::WorldFarSide, Slot::WorldCameraSide, Slot::FirstPerson}) {
		if (!impl_->slot_present[slot])
			continue;
		const auto &bounds = impl_->compilers[slot].draw_list().emitter_bounds;
		r_out.insert(r_out.end(), bounds.begin(), bounds.end());
	}
}

PackedStringArray ParticleRenderer::get_unresolved_texture_names() const {
	PackedStringArray result;
	if (!impl_)
		return result;
	const std::vector<std::string> &names = impl_->atlas.unresolved_names();
	result.resize(static_cast<int64_t>(names.size()));
	for (std::size_t i = 0; i < names.size(); ++i)
		result[static_cast<int64_t>(i)] = opennova::to_gd(names[i]);
	return result;
}
