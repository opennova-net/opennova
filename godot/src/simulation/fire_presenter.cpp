#include "simulation/fire_presenter.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <runtime/audio/oneshot_play.h>
#include <runtime/renderer/tracer_frame.h>
#include <runtime/world/player_present.h> // fire_effect_plan (the effect admission, ADR 0040 ladder E0)

#include "audio/mission_audio.h"
#include "env/mission_environment.h"
#include "lights/effect_light_director.h"
#include "object/model_user_point.h"
#include "object/object_data.h"
#include "object/object_model.h"
#include "particle/effect_distortion_drawer.h"
#include "particle/effect_world.h"
#include "render/scene_overlay_compositor.h"
#include "render/visual_layers.h"
#include "simulation/entity_presenter.h"
#include "simulation/simulation.h"
#include "util/axes.h"
#include "util/color_convert.h"
#include "resource_index/resource_root.h"
#include "util/string_convert.h"
#include "world/game_world.h"

namespace godot {

namespace {

using opennova::renderer::TracerShader;

// One ribbon mesh's instance under `p_container`, drawn by the cameras that
// admit `p_layer` alone.
ObjectID add_ribbon_instance(Node3D *p_container, const char *p_name, const Ref<ArrayMesh> &p_mesh,
		uint32_t p_layer) {
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_name(p_name);
	instance->set_mesh(p_mesh);
	instance->set_layer_mask(p_layer);
	instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	p_container->add_child(instance);
	return ObjectID(instance->get_instance_id());
}

} // namespace

FirePresenter::FirePresenter(EntityPresenter *p_owner) :
		owner_(p_owner) {}

Simulation *FirePresenter::sim() const {
	return sim_id_.is_valid()
			? Object::cast_to<Simulation>(ObjectDB::get_instance(sim_id_))
			: nullptr;
}

MissionAudio *FirePresenter::audio() const {
	return audio_id_.is_valid()
			? Object::cast_to<MissionAudio>(ObjectDB::get_instance(audio_id_))
			: nullptr;
}

EffectWorld *FirePresenter::fx() const {
	return fx_id_.is_valid()
			? Object::cast_to<EffectWorld>(ObjectDB::get_instance(fx_id_))
			: nullptr;
}

EffectLightDirector *FirePresenter::lights() const {
	return lights_id_.is_valid()
			? Object::cast_to<EffectLightDirector>(ObjectDB::get_instance(lights_id_))
			: nullptr;
}

MissionEnvironment *FirePresenter::environment() const {
	return environment_id_.is_valid()
			? Object::cast_to<MissionEnvironment>(ObjectDB::get_instance(environment_id_))
			: nullptr;
}

void FirePresenter::free_mesh_instance() {
	const auto free_instance = [](ObjectID &r_id) {
		if (r_id.is_valid()) {
			Node *instance = Object::cast_to<Node>(ObjectDB::get_instance(r_id));
			if (instance != nullptr) {
				instance->queue_free();
			}
		}
		r_id = ObjectID();
	};
	free_instance(mesh_instance_id_);
	mesh_.unref();
	for (MirrorRibbons &mirror : mirrors_) {
		free_instance(mirror.mesh_instance_id);
		mirror.mesh.unref();
	}
}

void FirePresenter::set_mirror_cameras(Camera3D *p_mirror, Camera3D *p_inset_mirror) {
	mirrors_[0].camera_id = p_mirror != nullptr ? p_mirror->get_instance_id() : ObjectID();
	mirrors_[1].camera_id =
			p_inset_mirror != nullptr ? p_inset_mirror->get_instance_id() : ObjectID();
}

// A mirror renders while its target updates (env/water.h: the main scene's
// mirror while its water pass runs, the Inset's while the Inset pass renders it).
Camera3D *FirePresenter::rendering_mirror_camera(int p_mirror) const {
	const ObjectID id = mirrors_[static_cast<size_t>(p_mirror)].camera_id;
	Camera3D *camera = id.is_valid() ? Object::cast_to<Camera3D>(ObjectDB::get_instance(id))
									 : nullptr;
	if (camera == nullptr || !camera->is_inside_tree()) {
		return nullptr;
	}
	const SubViewport *target = Object::cast_to<SubViewport>(camera->get_viewport());
	if (target != nullptr && target->get_update_mode() == SubViewport::UPDATE_DISABLED) {
		return nullptr;
	}
	return camera;
}

void FirePresenter::set_main_ribbon_layer(uint32_t p_layer) {
	MeshInstance3D *instance = mesh_instance_id_.is_valid()
			? Object::cast_to<MeshInstance3D>(ObjectDB::get_instance(mesh_instance_id_))
			: nullptr;
	if (instance != nullptr && instance->get_layer_mask() != p_layer) {
		instance->set_layer_mask(p_layer);
	}
}

Ref<ArrayMesh> FirePresenter::mirror_ribbon_mesh(int p_mirror) const {
	return p_mirror == 0 || p_mirror == 1 ? mirrors_[static_cast<size_t>(p_mirror)].mesh
										  : Ref<ArrayMesh>();
}

void FirePresenter::setup(Simulation *p_sim, Node3D *p_container, MissionAudio *p_audio,
		EffectWorld *p_fx, EffectLightDirector *p_lights,
		const Ref<ResourceRoot> &p_resource_root, MissionEnvironment *p_environment) {
	sim_id_ = p_sim != nullptr ? p_sim->get_instance_id() : ObjectID();
	audio_id_ = p_audio != nullptr ? p_audio->get_instance_id() : ObjectID();
	fx_id_ = p_fx != nullptr ? p_fx->get_instance_id() : ObjectID();
	lights_id_ = p_lights != nullptr ? p_lights->get_instance_id() : ObjectID();
	environment_id_ = p_environment != nullptr ? p_environment->get_instance_id() : ObjectID();
	ribbons_.set_resource_root(p_resource_root);
	// A re-setup replaces the previous tracer geometry instead of stranding it.
	free_mesh_instance();
	if (p_container != nullptr) {
		// The scene views' ribbons (the beauty camera's and the weapon Inset's)
		// on the bit no mirror admits, each mirror's on its own.
		mesh_.instantiate();
		mesh_instance_id_ = add_ribbon_instance(
				p_container, "FireTracers", mesh_, visual_layers::SCENE_VIEW_NO_REFLECTION);
		static constexpr const char *kMirrorNames[2] = { "FireTracersMirror",
			"FireTracersInsetMirror" };
		static constexpr uint32_t kMirrorLayers[2] = { visual_layers::MIRROR_VIEW,
			visual_layers::INSET_MIRROR_VIEW };
		for (size_t m = 0; m < mirrors_.size(); ++m) {
			mirrors_[m].mesh.instantiate();
			mirrors_[m].mesh_instance_id = add_ribbon_instance(
					p_container, kMirrorNames[m], mirrors_[m].mesh, kMirrorLayers[m]);
		}
	}
}

void FirePresenter::teardown() {
	free_mesh_instance();
}

void FirePresenter::present() {
	Simulation *s = sim();
	if (s == nullptr) {
		return;
	}
	std::vector<opennova::world::FirePresentationRow> fires;
	s->drain_fire_presentation_rows(fires);
	present_fires(fires);
	std::vector<opennova::world::ReadyFireSound> fire_sounds;
	s->drain_fire_sounds(fire_sounds);
	present_fire_sounds(fire_sounds);
	std::vector<opennova::world::SoundSlotEvent> slot_sounds;
	s->drain_slot_sounds(slot_sounds);
	present_slot_sounds(slot_sounds);
	std::vector<opennova::world::SoundEmitterEvent> emitters;
	s->drain_sound_emitter_events(emitters);
	present_sound_emitters(emitters);
	draw_tracer_rows(s->get_tracer_trails());
}

// The body slot-sound drain (footsteps/foley/landing thumps/death screams): the
// sim resolves each entity's SndProf.def profile slot to its authored set name
// and emits the (foot-level) position; this plays them full-volume positional
// with NO propagation-delay leg — footsteps play immediately, unlike fire
// (engine: runtime/inmatch/client_effects.cpp). The local player's own body sounds DO play (retail
// plays your own steps; only fire has an action-slot presentation to defer to).
// Slots 43/44 (chute flap / freefall) refire by design; a body re-firing the
// same wave retakes its own channel through the sim's own-channel key
// (audio::oneshot_sound_id over the packed source handle).
void FirePresenter::present_slot_sounds(const std::vector<opennova::world::SoundSlotEvent> &p_events) {
	if (p_events.empty()) {
		return;
	}
	MissionAudio *audio_node = audio();
	if (audio_node == nullptr) {
		return;
	}
	for (const opennova::world::SoundSlotEvent &ev : p_events) {
		if (ev.set_name[0] == '\0') {
			continue;
		}
		// Mission-frame 16.16 -> godot (util/axes.h).
		const Vector3 pos = godot_from_fixed3(ev.pos);
        const int source_id = sim() ? sim()->sound_source_bms_id(ev.source_handle) : 0;
        const int sound_id = static_cast<int>(
                opennova::audio::oneshot_sound_id(ev.source_handle, source_id));
		if (audio_node->slot_soundset(String(ev.set_name), pos, source_id, sound_id)) {
			++stat_sounds_;
		}
	}
}

// Entity-attached loop registrations (vehicle idle/drive/reverse today) share
// the native ambient emitter table and its loudest-eight physical pool. The
// audio layer replays the bounded latest intents at their producer ticks before
// advancing to the end of a catch-up frame, preserving the 30-tick keep-alive.
// (engine: runtime/audio/ambient_mixer.h)
void FirePresenter::present_sound_emitters(
		const std::vector<opennova::world::SoundEmitterEvent> &p_events) {
	if (p_events.empty()) {
		return;
	}
	MissionAudio *audio_node = audio();
	if (audio_node == nullptr) {
		return;
	}
	audio_node->apply_sound_emitter_events(p_events);
}

void FirePresenter::present_fires(const std::vector<opennova::world::FirePresentationRow> &p_events) {
	if (p_events.empty()) {
		return;
	}
	EffectWorld *fx_world = fx();
	EffectLightDirector *light_director = lights();
	for (const opennova::world::FirePresentationRow &ev : p_events) {
		// The admission (which legs, which effect, which anchor) is the engine's
		// world::fire_effect_plan; this pass resolves the anchor against the node
		// it renders and spawns.
		const opennova::world::FireEffectPlan plan = opennova::world::fire_effect_plan(ev);
		if (light_director != nullptr && plan.glow) {
			Vector3 glow_pos = mission_to_godot(ev.origin);
			if (plan.glow_at_muzzle) {
				const Vector3 glow_anchor = owner_->muzzle_world_for(
						ev.shooter_handle, opennova::to_gd(plan.userpoint));
				if (glow_anchor.is_finite()) {
					glow_pos = glow_anchor;
				}
			}
			light_director->on_muzzle_fire(ev.shooter_handle, glow_pos);
		}
		if (ev.is_local_player) {
			continue;
		}
		++stat_fires_;
		Vector3 origin = mission_to_godot(ev.origin);
		if (plan.spawn_at_muzzle) {
			// The rendered gun's own userpoint is the anchor (the muzzle-authority
			// decision); an unresolvable anchor keeps the row's origin.
			const Vector3 anchored = owner_->muzzle_world_for(
					ev.shooter_handle, opennova::to_gd(plan.userpoint));
			if (anchored.is_finite()) {
				origin = anchored;
			}
		}
		if (fx_world != nullptr && plan.spawn) {
			// The shooter is the descriptor tag (retail
			// WeaponSlot_FireAndSpawnEffects @ 0x53F582): the section gate applies.
			fx_world->spawn_effect(opennova::to_gd(plan.effect), origin,
					mission_to_godot(ev.forward), true);
			++stat_effects_;
		}
	}
}

// The sim's ready fire sounds: immediate near shots, the adm-arm action-row
// sets, and expired propagation-delayed slots, gated and counted down on the
// logic clock (world/fire_sound.h). Each row plays positionally; the set's
// max-range cull runs in the audio bank (D-AI-8).
// (engine: runtime/world/fire_sound.h)
void FirePresenter::present_fire_sounds(const std::vector<opennova::world::ReadyFireSound> &p_sounds) {
	if (p_sounds.empty()) {
		return;
	}
	MissionAudio *audio_node = audio();
	if (audio_node == nullptr) {
		return;
	}
	for (const opennova::world::ReadyFireSound &row : p_sounds) {
		if (row.interface_set) audio_node->ui_soundset(opennova::to_gd(row.set_name));
		else audio_node->fire_soundset(opennova::to_gd(row.set_name), mission_to_godot(row.pos),
				row.source_bms_id, static_cast<int>(row.sound_id));
		++stat_sounds_;
	}
}

// The compiled ribbon draws [orig: CEffectChannel_RenderRibbon @ 0x5DB8A0 —
// the math and the style tables live natively in
// engine/runtime/renderer/tracer_frame.cpp]. This pass drains the sim's trail
// rows (framed [style_id, age, count, then count x (x, y, z, w)] per channel),
// compiles them against the viewport's render camera and uploads one surface
// per run of same-material channel draws, in pool order, on the frame's
// tracer rung. The clear runs BEFORE the empty return: that is also what drops
// warm_pipelines' surfaces.
void FirePresenter::draw_tracer_rows(const PackedFloat32Array &p_rows) {
	if (mesh_.is_null()) {
		return;
	}
	mesh_->clear_surfaces();
	set_main_ribbon_layer(visual_layers::SCENE_VIEW_NO_REFLECTION);
	for (MirrorRibbons &mirror : mirrors_) {
		if (mirror.mesh.is_valid()) {
			mirror.mesh->clear_surfaces();
		}
	}
	EffectWorld *fx_world = fx();
	const std::shared_ptr<EffectDistortionDrawer> distortion =
			fx_world != nullptr ? fx_world->distortion_drawer() : nullptr;
	// This frame's distortion ribbons replace the last ones, even when none draw.
	auto publish_distortion = [&](bool p_channels_present) {
		if (distortion) {
			distortion->set_ribbon_channels_present(p_channels_present);
			distortion->publish_ribbons(distortion_frame_);
		}
	};
	distortion_frame_.clear();
	if (p_rows.is_empty()) {
		publish_distortion(false);
		return;
	}
	Viewport *viewport = owner_ != nullptr && owner_->is_inside_tree() ? owner_->get_viewport()
																	   : nullptr;
	Camera3D *camera = viewport != nullptr ? viewport->get_camera_3d() : nullptr;
	if (camera == nullptr) {
		publish_distortion(false);
		return;
	}
	const opennova::renderer::TracerView view = tracer_view_from_camera(
			*camera, static_cast<std::uint32_t>(GameWorld::current_frame_clock_ms()));
	channels_.clear();
	const float *r = p_rows.ptr();
	const int64_t size = p_rows.size();
	int64_t i = 0;
	while (r != nullptr && i + 2 < size) {
		opennova::renderer::TracerChannelInput c;
		c.style_id = static_cast<int>(r[i]);
		c.age = static_cast<int>(r[i + 1]);
		c.count = static_cast<int>(r[i + 2]);
		i += 3;
		if (c.count <= 0 || i + static_cast<int64_t>(c.count) * 4 > size) {
			break;
		}
		c.points = r + i;
		i += static_cast<int64_t>(c.count) * 4;
		channels_.push_back(c);
	}
	opennova::renderer::compile_tracer_ribbons(channels_.data(), channels_.size(), view,
			opennova::renderer::TracerPass::Main, frame_);
	// The distortion pass's ribbons for FrameFX's type-0 row, and its content
	// gate: an active channel of a +0x828 style (retail
	// CEffectEmitterPool_HasDistortionChannels @ 0x5DB7F0).
	opennova::renderer::compile_tracer_ribbons(channels_.data(), channels_.size(), view,
			opennova::renderer::TracerPass::Distortion, distortion_frame_);
	publish_distortion(distortion_frame_.channels > 0);
	stat_tracer_peak_ = MAX(stat_tracer_peak_, static_cast<int64_t>(frame_.channels));
	const MissionEnvironment *env = environment();
	const int rung = opennova::renderer::tracer_rung(env == nullptr || !env->is_underwater_view());
	ribbons_.emit(frame_, mesh_, rung);
	// Each rendering water mirror's reflected subscene runs the pool's main pass
	// again under its own view (engine renderer/tracer_frame.h TracerView carries
	// the witness): the same rows, built against that mirror's camera, on the bit
	// only it admits. The shared materials keep the frame's rung.
	for (size_t m = 0; m < mirrors_.size(); ++m) {
		Camera3D *mirror = rendering_mirror_camera(static_cast<int>(m));
		if (mirror == nullptr || mirrors_[m].mesh.is_null()) {
			continue;
		}
		opennova::renderer::compile_tracer_ribbons(channels_.data(), channels_.size(),
				tracer_view_from_camera(*mirror, view.tick_ms),
				opennova::renderer::TracerPass::Main, mirror_frame_);
		ribbons_.emit(mirror_frame_, mirrors_[m].mesh, rung);
	}
}

// retail Render_NVGLaserBeamsForVisiblePersons @ 0x5c63b0 walks the frame's visible persons (the drawn
// bodies) into Entity_RenderNVGLaserBeam @ 0x5c6090. The action point is
// Entity_ComputeBoneTransform @ 0x401890 for a person on foot:
// Entity_GetCameraTransform @ 0x4b8c00 poses the weapon matrix with
// Entity_BuildBoneTransformMatrices @ 0x4b1290 (the held-weapon anchor draw 5
// also places its gun with), and Userpoint_ComputeWorldTransform @ 0x56c420
// takes the held definition's +0x2D4 userpoint of its +0x170 model through
// it, position and authored direction. The drawn gun is that model at that
// matrix, so its posed userpoint IS the action point; no drawn gun, no pose.
// The beam is drawn when its run holds more than one point (@ 0x5c6381..0x5c6387).
int FirePresenter::append_nvg_laser_beams(const std::vector<NvgLaserSource> &p_sources,
		const NvgLaserView &p_view, SceneOverlaySubmission &r_submission) {
	if (owner_ == nullptr || p_sources.empty()) {
		return 0;
	}
	Simulation *s = sim();
	const Vector3 forward = -p_view.eye.basis.get_column(2).normalized();
	opennova::renderer::TracerView view;
	view.camera = {static_cast<float>(p_view.eye.origin.x),
			static_cast<float>(p_view.eye.origin.y), static_cast<float>(p_view.eye.origin.z)};
	view.forward = {static_cast<float>(forward.x), static_cast<float>(forward.y),
			static_cast<float>(forward.z)};
	view.projection_x_scale = p_view.projection_x_scale;
	view.tick_ms = p_view.tick_ms;
	int drawn = 0;
	for (const NvgLaserSource &source : p_sources) {
		opennova::world::NvgLaserGate gate = source.gate;
		gate.nvg_active = p_view.nvg_active;
		gate.camera_mode = p_view.camera_mode;
		if (!opennova::world::nvg_laser_beam_drawn(gate)) {
			continue;
		}
		ObjectModel *body = owner_->resolve_wire_handle(source.handle);
		ObjectModel *weapon = owner_->held_weapon_node(source.handle);
		// The view's own draw: the main view the nodes, the Inset view the
		// nodes while the two views agree, else the twins its collect's
		// verdicts built (ObjectModel's view split); the gun is posed for
		// either view.
		const auto view_draws = [&p_view](ObjectModel *p_model) {
			return p_view.inset_view && p_model->is_view_split()
					? p_model->get_view_twin_count() > 0
					: p_model->is_visible_in_tree();
		};
		if (body == nullptr || !view_draws(body) || weapon == nullptr || !view_draws(weapon)) {
			continue;
		}
		const Ref<ObjectData> data = weapon->get_object_data();
		const Ref<ModelUserPoint> point = data.is_valid()
				? data->get_user_point_info(source.launch_userpoint - 1)
				: Ref<ModelUserPoint>();
		if (point.is_null()) {
			continue;
		}
		const Transform3D part = weapon->subobject_model_to_world(point->get_subobject());
		const opennova::world::Vec3 origin = godot_to_mission<opennova::world::Vec3>(
				part.xform(point->get_position()));
		const opennova::world::Vec3 direction = godot_to_mission<opennova::world::Vec3>(
				part.basis.xform(point->get_rotation()));
		const int32_t origin_q16[3] = {opennova::world::to_fixed(origin.x),
				opennova::world::to_fixed(origin.y), opennova::world::to_fixed(origin.z)};
		const int32_t direction_q16[3] = {opennova::world::to_fixed(direction.x),
				opennova::world::to_fixed(direction.y), opennova::world::to_fixed(direction.z)};
		const int32_t clip = s != nullptr
				? s->nvg_laser_clip_distance(source.handle, origin_q16, direction_q16)
				: opennova::world::kNvgLaserRangeQ16;
		float points[opennova::world::kNvgLaserMaxPoints * 4];
		const int count =
				opennova::world::nvg_laser_beam_points(origin_q16, direction_q16, clip, points);
		if (count <= 1) {
			continue;
		}
		for (int i = 0; i < count; ++i) {
			float *at = points + i * 4;
			const Vector3 godot_at =
					mission_to_godot(opennova::world::Vec3{at[0], at[1], at[2]});
			at[0] = static_cast<float>(godot_at.x);
			at[1] = static_cast<float>(godot_at.y);
			at[2] = static_cast<float>(godot_at.z);
		}
		laser_frame_.clear();
		opennova::renderer::append_tracer_beam(points, count,
				opennova::world::kNvgLaserTracerStyle, view, laser_frame_);
		opennova::renderer::append_nvg_laser_overlay(laser_frame_,
				r_submission.texture_index(ribbons_.smoke_texture(),
						opennova::renderer::texture_stage_flags(
								opennova::renderer::TextureStage::TracerSmoke)),
				p_view.fog, r_submission.frame,
				p_view.inset_view ? opennova::renderer::SceneOverlaySlot::InsetNvgLaserBeams
								  : opennova::renderer::SceneOverlaySlot::NvgLaserBeams);
		++drawn;
	}
	return drawn;
}

void FirePresenter::warm_pipelines(const Vector3 &p_position) {
	if (mesh_.is_null()) {
		return;
	}
	mesh_->clear_surfaces();
	// The load's forced frame can draw before FrameFx gives the beauty camera
	// the scene views' bit (it does so in a frame's own leg), so the warm
	// surfaces ride the world layer every camera mask admits; the next build
	// puts the instance back (draw_tracer_rows).
	set_main_ribbon_layer(visual_layers::WORLD);
	// One zero-area triangle per normal-pass material.
	const std::pair<TracerShader, bool> materials[] = {{TracerShader::Stock, true},
			{TracerShader::Smoke, false}, {TracerShader::NvgLaser, true}};
	for (const std::pair<TracerShader, bool> &entry : materials) {
		const Ref<ShaderMaterial> material = ribbons_.material(entry.first, entry.second);
		if (material.is_null()) {
			continue;
		}
		PackedVector3Array positions;
		PackedColorArray colors;
		PackedVector2Array uvs;
		PackedInt32Array indices;
		for (int i = 0; i < 3; ++i) {
			positions.push_back(p_position + Vector3(0.001f * i, 0, 0));
			colors.push_back(Color(0, 0, 0, 0));
			uvs.push_back(Vector2());
			indices.push_back(i);
		}
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = positions;
		arrays[Mesh::ARRAY_COLOR] = colors;
		arrays[Mesh::ARRAY_TEX_UV] = uvs;
		arrays[Mesh::ARRAY_TEX_UV2] = uvs;
		arrays[Mesh::ARRAY_INDEX] = indices;
		mesh_->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		mesh_->surface_set_material(mesh_->get_surface_count() - 1, material);
	}
}

Ref<FirePresentStats> FirePresenter::get_stats() const {
	Ref<FirePresentStats> stats;
	stats.instantiate();
	stats->fires = stat_fires_;
	stats->sounds = stat_sounds_;
	stats->effects = stat_effects_;
	stats->tracer_peak = stat_tracer_peak_;
	return stats;
}

} // namespace godot
