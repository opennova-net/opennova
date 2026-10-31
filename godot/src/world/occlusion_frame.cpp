// The render-occlusion frame (the former occlusion_frame_pass.gd): see the
// header for the preserved order and the ownership-bit contract.
#include "world/occlusion_frame.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <algorithm>
#include <cmath>

#include "env/celestial.h"
#include "env/mission_environment.h"
#include "env/sky_dome.h"
#include "env/slot_shadow.h"
#include "env/water.h"
#include "mission/mission_object_placer.h"
#include "object/entity_index.h"
#include "object/object_model.h"
#include "render/object_lod_frame.h"
#include "simulation/entity_presenter.h"
#include "simulation/simulation.h"
#include "terrain/foliage_dispatcher.h"
#include "terrain/terrain.h"
#include "world/scar_presenter.h"

#include <runtime/environment/water_mirror.h>
#include <runtime/renderer/scene_pass_gates.h>
#include <runtime/world/occlusion_camera.h> // frame_blink_letters

using namespace godot;

namespace {

template <typename T>
T *live(const ObjectID &p_id) {
	return Object::cast_to<T>(ObjectDB::get_instance(p_id));
}

ObjectID id_of(const Object *p_object) {
	return p_object != nullptr ? ObjectID(p_object->get_instance_id()) : ObjectID();
}

int64_t ticks_usec() {
	return static_cast<int64_t>(Time::get_singleton()->get_ticks_usec());
}

// A water mirror's collect view: the scene view's horizontal field on both
// axes of the square reflection target (engine world/occlusion.h
// OcclusionWorld::mirror_water_visible).
// A camera's horizontal field of view in degrees: the fov word its viewport is built with (its
// keep-aspect mode decides which axis the camera's own fov names).
double camera_fov_h_deg(const Camera3D *p_camera, double p_default) {
	float tan_h = 0.0f;
	float tan_v = 0.0f;
	if (p_camera == nullptr || !ObjectLodFrame::camera_tangents(p_camera, tan_h, tan_v)) {
		return p_default;
	}
	return Math::rad_to_deg(2.0 * std::atan(static_cast<double>(tan_h)));
}

} // namespace

OcclusionViewInputs godot::occlusion_view_inputs(const Camera3D *p_camera, const MissionEnvironment *p_env,
		const Water *p_water) {
	OcclusionViewInputs in;
	in.fov_h_deg = camera_fov_h_deg(p_camera, in.fov_h_deg);
	if (p_env != nullptr) {
		in.fog_dist = p_env->get_fog_distance();
	}
	if (p_water != nullptr && p_water->is_water_render_active()) {
		in.water_z = p_water->get_water_height();
	}
	return in;
}

void OcclusionFrame::_bind_methods() {
	ClassDB::bind_method(D_METHOD("setup", "terrain", "foliage", "sky", "celestial", "water",
			"env"), &OcclusionFrame::setup);
	ClassDB::bind_method(D_METHOD("set_frame_stats", "board"), &OcclusionFrame::set_frame_stats);
	ClassDB::bind_method(D_METHOD("apply_frame", "camera", "viewport_width", "viewport_height",
			"camera_xform", "forces_indoors", "object_detail", "mirror_eye"), &OcclusionFrame::apply_frame);
	ClassDB::bind_method(D_METHOD("reset"), &OcclusionFrame::reset);
}

void OcclusionFrame::setup(Terrain *p_terrain, FoliageDispatcher *p_foliage, SkyDome *p_sky,
		Celestial *p_celestial, Water *p_water, MissionEnvironment *p_env) {
	terrain_id_ = id_of(p_terrain);
	foliage_id_ = id_of(p_foliage);
	sky_id_ = id_of(p_sky);
	celestial_id_ = id_of(p_celestial);
	water_id_ = id_of(p_water);
	env_id_ = id_of(p_env);
	// The world's SlotShadow sits beside the terrain (game_world.tscn); its
	// drapes share the terrain gate (apply_scene_pass_gates).
	Node *world = p_terrain != nullptr ? p_terrain->get_parent() : nullptr;
	slot_shadow_id_ = id_of(world != nullptr
					? Object::cast_to<SlotShadow>(world->get_node_or_null(NodePath("SlotShadow")))
					: nullptr);
}

void OcclusionFrame::bind_mission(const Ref<Simulation> &p_sim, const Ref<EntityIndex> &p_index,
		EntityPresenter *p_entities, const Ref<MissionObjectPlacer> &p_placer) {
	sim_id_ = id_of(p_sim.ptr());
	index_id_ = id_of(p_index.ptr());
	entities_id_ = id_of(p_entities);
	placer_id_ = id_of(p_placer.ptr());
}

void OcclusionFrame::set_frame_stats(const Ref<FrameStats> &p_board) {
	frame_stats_ = p_board;
}

Simulation *OcclusionFrame::sim() const { return live<Simulation>(sim_id_); }
EntityIndex *OcclusionFrame::entity_index() const { return live<EntityIndex>(index_id_); }
EntityPresenter *OcclusionFrame::entities() const { return live<EntityPresenter>(entities_id_); }
MissionObjectPlacer *OcclusionFrame::placer() const { return live<MissionObjectPlacer>(placer_id_); }
Terrain *OcclusionFrame::terrain() const { return live<Terrain>(terrain_id_); }
FoliageDispatcher *OcclusionFrame::foliage() const { return live<FoliageDispatcher>(foliage_id_); }
SkyDome *OcclusionFrame::sky() const { return live<SkyDome>(sky_id_); }
Celestial *OcclusionFrame::celestial() const { return live<Celestial>(celestial_id_); }
Water *OcclusionFrame::water() const { return live<Water>(water_id_); }
MissionEnvironment *OcclusionFrame::environment() const { return live<MissionEnvironment>(env_id_); }
SlotShadow *OcclusionFrame::slot_shadow() const { return live<SlotShadow>(slot_shadow_id_); }
ObjectModel *OcclusionFrame::local_avatar() const { return live<ObjectModel>(local_avatar_id_); }

void OcclusionFrame::set_local_avatar(ObjectModel *p_avatar) {
	const ObjectID id = p_avatar != nullptr ? ObjectID(p_avatar->get_instance_id()) : ObjectID();
	if (id == local_avatar_id_) {
		return;
	}
	release_local_avatar();
	local_avatar_id_ = id;
}

void OcclusionFrame::release_local_avatar() {
	if (ObjectModel *avatar = local_avatar()) {
		if (avatar->is_occlusion_hidden()) {
			avatar->set_occlusion_hidden(false);
		}
	}
}

opennova::renderer::ScenePassGateEdges OcclusionFrame::apply_blink_gates(bool p_forces_indoors) {
	Simulation *s = sim();
	if (s == nullptr) {
		return {};
	}
	// The frame's letter view, the one the collect's camera reads too (engine
	// world::frame_blink_letters carries the witness).
	const uint32_t flags = opennova::world::frame_blink_letters(
			static_cast<uint32_t>(s->local_player_blink_flags()), p_forces_indoors);
	// The letters latch per sim tick; the pass gates combine them with the
	// eye's waterline side every display frame (apply_scene_pass_gates).
	const opennova::renderer::ScenePassGateEdges edges =
			opennova::renderer::scene_pass_gate_edges(blink_letters_, flags);
	blink_letters_ = flags;
	blink_indoors_ = (flags & opennova::world::kBlinkIndoorsBit) != 0;
	apply_scene_pass_gates();
	// Accum bit 0x8 (the authored water letter): both water passes skipped.
	// Letter bits only accumulate while inside a box, so the outdoors leg of
	// retail's override is implicit; the remaining g_BlinkWaterVisible legs
	// (a camera building straddling the water plane, the window latch) ride
	// the section-mask slice (engine: runtime/renderer/scene_pass_gates.h).
	// The label reaches the strips alone (renderer::scene_water_visible).
	const bool water_off = !opennova::renderer::scene_water_visible(flags, false);
	if (water_off != blink_water_suppressed_) {
		blink_water_suppressed_ = water_off;
		if (Water *w = water()) {
			w->set_blink_water_views(!water_off, !water_off);
		}
	}
	return edges;
}

void OcclusionFrame::apply_frame(Camera3D *p_camera, float p_viewport_width, float p_viewport_height,
		const Transform3D &p_camera_xform, bool p_forces_indoors, int p_object_detail,
		const Transform3D &p_mirror_eye) {
	Simulation *s = sim();
	if (s == nullptr) {
		return;
	}
	EntityIndex *registry = entity_index();
	if (registry == nullptr) {
		return;
	}
	// The viewport the view is built over: the camera's horizontal field (mode-invariant) over the
	// screen's rect, which the aspect mode's stretch never changes (engine world/occlusion_camera.h
	// occlusion_camera_from_view carries the witness, D-OCC-12).
	Water *w = water();
	const OcclusionViewInputs in = occlusion_view_inputs(p_camera, environment(), w);
	const double fov_h = in.fov_h_deg;
	const double fog = in.fog_dist;
	const double water_z = in.water_z;
	// The scene's water mirror collects ahead of it while its water pass is
	// active, over the view block (the eye without the scene's optics offsets,
	// which only its drawn reflection takes) and the view's horizontal field on
	// both axes; the label then reads the word as it stands, the mirror's or the
	// last collect's (renderer::SceneWaterWord).
	const bool mirror_ran = w != nullptr && w->is_water_pass_active();
	const bool mirror_verdict = mirror_ran &&
			s->run_mirror_occlusion(Simulation::MIRROR_MAIN, p_mirror_eye, p_camera_xform,
					fov_h,
					opennova::env::kReflectionRttSize, fog, water_z, p_forces_indoors,
					p_object_detail);
	main_water_label_ = blink_water_word_.label(blink_letters_, mirror_ran, mirror_verdict);
	apply_mirror_view(Simulation::MIRROR_MAIN, mirror_ran,
			p_camera_xform.origin.y < water_z);
	const bool stats_on = frame_stats_.is_valid() && frame_stats_->is_capture_active();
	const bool timing = probe_timing_ || stats_on;
	const int64_t native_start = timing ? ticks_usec() : 0;
	const bool local_collected = s->run_occlusion_frame(p_camera_xform, fov_h, p_viewport_width,
			p_viewport_height, fog, water_z, p_forces_indoors, p_object_detail);
	const int64_t native_end = timing ? ticks_usec() : 0;
	int64_t building_query_us = 0;
	int64_t building_apply_us = 0;
	int64_t cull_query_us = 0;
	int64_t cull_apply_us = 0;
	int64_t light_query_us = 0;
	int64_t light_apply_us = 0;
	int64_t water_apply_us = 0;

	// Building batch visibility + per-section masks (the raw verdict, bit N =
	// render part N, and the def's forced sections the part draw ORs over
	// it), applied as CHANGES: the sim diffs against what this shell last
	// applied, so a steady frame walks nothing. Batch culls claim the
	// occlusion-hidden bit; a batched static (no node) takes the verdict on
	// its placer instance instead.
	// (engine: runtime/mission/placement_traits.h)
	const int64_t building_query_start = timing ? ticks_usec() : 0;
	const PackedInt64Array changes = s->get_building_visibility_changes();
	if (timing) {
		building_query_us = ticks_usec() - building_query_start;
	}
	const int64_t building_apply_start = timing ? ticks_usec() : 0;
	MissionObjectPlacer *statics = placer();
	for (int64_t i = 0; i + 2 < changes.size(); i += 3) {
		const int64_t bms_id = changes[i];
		const int64_t packed = changes[i + 1];
		ObjectModel *node = occlusion_node(registry, bms_id);
		if (node == nullptr) {
			if (statics != nullptr) {
				statics->set_static_instance_occlusion_hidden(static_cast<int>(bms_id),
						!Simulation::building_visibility_visible(packed));
			}
			continue;
		}
		node->set_occlusion_section_mask(Simulation::building_visibility_mask(packed),
				changes[i + 2]);
		node->set_occlusion_hidden(!Simulation::building_visibility_visible(packed));
	}
	if (timing) {
		building_apply_us = ticks_usec() - building_apply_start;
	}

	// Entity render gates (the blink-hits gate + the outdoors three-ray latch),
	// also applied as changes. (engine: runtime/world/occlusion.h)
	const int64_t cull_query_start = timing ? ticks_usec() : 0;
	const PackedInt32Array culled_changes = s->get_render_culled_changes();
	if (timing) {
		cull_query_us = ticks_usec() - cull_query_start;
	}
	const int64_t cull_apply_start = timing ? ticks_usec() : 0;
	if (culled_changes.size() >= 2) {
		const int64_t added = culled_changes[0];
		for (int64_t i = 1; i < 1 + added; ++i) {
			ObjectModel *node = occlusion_node(registry, culled_changes[i]);
			if (node != nullptr) {
				node->set_occlusion_hidden(true);
			} else if (statics != nullptr) {
				statics->set_static_instance_occlusion_hidden(culled_changes[i], true);
			}
		}
		for (int64_t i = 2 + added; i < culled_changes.size(); ++i) {
			ObjectModel *node = occlusion_node(registry, culled_changes[i]);
			if (node != nullptr) {
				node->set_occlusion_hidden(false);
			} else if (statics != nullptr) {
				statics->set_static_instance_occlusion_hidden(culled_changes[i], false);
			}
		}
	}
	// The main view's drops the scene's water mirror admitted take that mirror's
	// explicit admit (Simulation's view_admissions_since carries the rule).
	if (mirror_active_[Simulation::MIRROR_MAIN]) {
		apply_mirror_admissions(Simulation::MIRROR_MAIN);
	}
	// The local player's body takes the main collect's own verdict (no
	// local-player exception in the walk).
	if (ObjectModel *avatar = local_avatar()) {
		const bool hidden = !local_collected;
		if (avatar->is_occlusion_hidden() != hidden) {
			avatar->set_occlusion_hidden(hidden);
		}
	}
	// The same collector gate over the rows the entity presenter's wire walk
	// draws (remote organics and runtime spawns with no placed identity), keyed
	// by wire handle: retail's client walks its wire-built pools exactly like
	// the host walks its own (the native gate carries the witness).
	const PackedInt32Array wire_culled_changes = s->get_wire_render_culled_changes();
	if (wire_culled_changes.size() >= 2) {
		if (EntityPresenter *presenter_for_cull = entities()) {
			const int64_t wire_added = wire_culled_changes[0];
			for (int64_t i = 1; i < 1 + wire_added; ++i) {
				presenter_for_cull->set_render_culled(wire_culled_changes[i], true);
			}
			for (int64_t i = 2 + wire_added; i < wire_culled_changes.size(); ++i) {
				presenter_for_cull->set_render_culled(wire_culled_changes[i], false);
			}
		}
	}
	// The frame's death-piece draws land on the destruction pass's piece
	// models, and its flying-round draws on the throwable pass's round models
	// (the collect rides the same native frame).
	if (EntityPresenter *presenter_for_pieces = entities()) {
		presenter_for_pieces->present_death_piece_draws_native(s->scene_draws().death_pieces);
		presenter_for_pieces->present_round_draws(s->scene_draws().rounds);
	}
	if (timing) {
		cull_apply_us = ticks_usec() - cull_apply_start;
	}

	// The per-drawn-entity lighting context (D-RLIT-3 plus the interior lerp),
	// also applied as changes: quality 1..4 maps to effectScale quality*0.25,
	// dimming only the directional term (engine/runtime/renderer/light_runtime.h
	// sun_visibility_factor owns the witness); a contained entity takes the
	// interior lerp by its daylight t and its interior light group. Placed and
	// wire identities share this feed; wire rays use the separately keyed
	// 17-tick candidate arena, and the entity presenter retains the context for
	// cold bodies/weapons (inmatch/role_feeds.h EntityLightingFeed carries the
	// witnesses: the per-entity setup, its stack push, the 0x80 submit flag
	// and the person wave's daylight aux).
	if (MissionEnvironment *env = environment()) {
		const int64_t light_query_start = timing ? ticks_usec() : 0;
		const std::vector<opennova::inmatch::EntityLightingChange> &changes =
				s->draw_lighting_changes(env->get_light_direction());
		if (timing) {
			light_query_us = ticks_usec() - light_query_start;
		}
		const int64_t light_apply_start = timing ? ticks_usec() : 0;
		EntityPresenter *presenter = entities();
		for (const opennova::inmatch::EntityLightingChange &change : changes) {
			const opennova::inmatch::EntityLighting &lighting = change.lighting;
			// quality -> effectScale maps engine-side (one owner:
			// renderer::sun_visibility_factor via sun_quality_factor).
			const float effect_scale = s->sun_quality_factor(lighting.quality);
			if (change.wire) {
				if (presenter != nullptr) {
					presenter->set_entity_lighting_context(change.handle, effect_scale,
							lighting.interior, lighting.light_transfer, lighting.interior_bms,
							lighting.interior_section);
				}
				continue;
			}
			ObjectModel *node = occlusion_node(registry, change.bms_id);
			if (node != nullptr) {
				node->set_entity_lighting_context(effect_scale, lighting.interior,
						lighting.light_transfer);
				node->set_interior_light_group(lighting.interior_bms, lighting.interior_section);
			}
		}
		if (timing) {
			light_apply_us = ticks_usec() - light_apply_start;
		}
	}

	// The g_BlinkWaterVisible override legs: with the authored water letter
	// suppressing (accum bit 0x8), the water strip still draws when the frame
	// latched the exterior or a camera building straddles the water plane
	// (engine renderer::scene_water_visible / SceneWaterWord carry the witness).
	const int64_t water_apply_start = timing ? ticks_usec() : 0;
	// The collect wrote the word.
	blink_water_word_.collect(s->occlusion_water_visible());
	if (w != nullptr) {
		// The main view's label, on its strip alone (the Inset's lands with
		// apply_inset_frame).
		w->set_blink_water_views(main_water_label_, main_water_label_);
		// The scene's water passes store this collect's verdict in the latch
		// the next scenes' water-active tests read (Water::latch_blink_water).
		w->latch_blink_water(main_water_label_, blink_water_word_.word);
	}
	if (timing) {
		water_apply_us = ticks_usec() - water_apply_start;
	}

	if (timing) {
		perf_occl_native_us_ = native_end - native_start;
		perf_occl_apply_us_ = ticks_usec() - native_end;
	}
	if (stats_on) {
		frame_stats_->add(FrameStats::OCCL_APPLY, perf_occl_apply_us_);
		// The native call's internal split; the remainder of the bound call
		// (marshalling + the handle collection) lands in the glue slot so the
		// pane's Occlusion group still sums to the whole frame cost.
		const int64_t build_us = s->get_last_occlusion_build_us();
		const int64_t probe_us = s->get_last_occlusion_probe_us();
		frame_stats_->add(FrameStats::OCCL_BUILD, build_us);
		frame_stats_->add(FrameStats::OCCL_PROBE, probe_us);
		frame_stats_->add(FrameStats::OCCL_GLUE,
				std::max<int64_t>(perf_occl_native_us_ - build_us - probe_us, 0));
		frame_stats_->add(FrameStats::OCCL_BUILDING_QUERY, building_query_us);
		frame_stats_->add(FrameStats::OCCL_BUILDING_APPLY, building_apply_us);
		frame_stats_->add(FrameStats::OCCL_CULL_QUERY, cull_query_us);
		frame_stats_->add(FrameStats::OCCL_CULL_APPLY, cull_apply_us);
		frame_stats_->add(FrameStats::OCCL_LIGHT_QUERY, light_query_us);
		frame_stats_->add(FrameStats::OCCL_LIGHT_APPLY, light_apply_us);
		frame_stats_->add(FrameStats::OCCL_WATER_APPLY, water_apply_us);
	}
}

void OcclusionFrame::apply_inset_frame(Camera3D *p_camera, float p_viewport_width,
		bool p_forces_indoors, int p_object_detail, const Transform3D *p_mirror_block) {
	Simulation *s = sim();
	EntityIndex *registry = entity_index();
	if (s == nullptr || registry == nullptr || p_camera == nullptr) {
		release_inset();
		return;
	}
	InsetOcclusionRequest request;
	request.camera = p_camera->get_global_transform();
	// The Inset's own viewport: its camera's horizontal field over its target's rect.
	request.viewport_width = p_viewport_width;
	request.viewport_height = p_camera->get_viewport() != nullptr
			? p_camera->get_viewport()->get_visible_rect().size.y
			: p_viewport_width;
	request.fov_h_deg = camera_fov_h_deg(p_camera, request.fov_h_deg);
	if (MissionEnvironment *env = environment()) {
		request.fog_dist_units = env->get_fog_distance();
	}
	Water *w = water();
	if (w != nullptr && w->is_water_render_active()) {
		request.water_z_units = w->get_water_height();
	}
	request.force_indoors = p_forces_indoors;
	request.object_detail = p_object_detail;
	// The Inset scene's own water mirror collects ahead of its collect while
	// the Inset's water pass is active: that test reads the latch the main
	// scene's water pass left this frame (Water::open_inset_water_latch) over
	// the Inset's own terrain bounds, fed ahead of this collect, and the
	// mirror's eye is the view block it reflects (engine environment/
	// water_mirror.h inset_mirror_source). Its label then reads the word as
	// it stands.
	bool mirror_ran = false;
	if (w != nullptr) {
		w->open_inset_water_latch();
		mirror_ran = p_mirror_block != nullptr && w->is_inset_water_pass_active();
	}
	const bool mirror_verdict = mirror_ran &&
			s->run_mirror_occlusion(Simulation::MIRROR_INSET, *p_mirror_block, *p_mirror_block,
					request.fov_h_deg,
					opennova::env::kReflectionRttSize, request.fog_dist_units,
					request.water_z_units, p_forces_indoors, p_object_detail);
	const bool inset_water_label =
			blink_water_word_.label(blink_letters_, mirror_ran, mirror_verdict);
	apply_mirror_view(Simulation::MIRROR_INSET, mirror_ran,
			mirror_ran && p_mirror_block->origin.y < request.water_z_units);
	const InsetOcclusionView &inset = s->run_inset_occlusion(request);
	inset_active_ = true;
	inset_foliage_mask_anchors_ = inset.foliage_mask_anchors;
	// The Inset collect wrote the word. Each view's label on its own strip
	// (apply_frame's main one kept), then the Inset's water passes store its
	// verdict in the latch for the next frame's main scene.
	blink_water_word_.collect(inset.water_visible);
	if (w != nullptr) {
		w->set_blink_water_views(main_water_label_, inset_water_label);
		w->latch_blink_water(inset_water_label, blink_water_word_.word);
	}
	MissionObjectPlacer *statics = placer();
	const auto note = [this](ObjectModel *p_node) {
		inset_nodes_[p_node->get_instance_id()] = ObjectID(p_node->get_instance_id());
	};
	// The Inset view's building verdicts, as changes (the main leg's triple
	// form): its raw section mask and batch/TOC visibility.
	const PackedInt64Array &changes = inset.building_changes;
	for (int64_t i = 0; i + 2 < changes.size(); i += 3) {
		const int64_t bms_id = changes[i];
		const int64_t packed = changes[i + 1];
		ObjectModel *node = occlusion_node(registry, bms_id);
		if (node == nullptr) {
			if (statics != nullptr) {
				statics->set_static_instance_inset_occlusion_hidden(static_cast<int>(bms_id),
						!Simulation::building_visibility_visible(packed));
			}
			continue;
		}
		note(node);
		node->set_inset_occlusion_section_mask(Simulation::building_visibility_mask(packed),
				changes[i + 2]);
		node->set_inset_occlusion_hidden(!Simulation::building_visibility_visible(packed));
	}
	// The Inset collect's entity render gates, as changes.
	const PackedInt32Array &culled = inset.culled_changes;
	if (culled.size() >= 2) {
		const int64_t added = culled[0];
		for (int64_t i = 1; i < 1 + added; ++i) {
			if (ObjectModel *node = occlusion_node(registry, culled[i])) {
				note(node);
				node->set_inset_occlusion_hidden(true);
			} else if (statics != nullptr) {
				statics->set_static_instance_inset_occlusion_hidden(culled[i], true);
			}
		}
		for (int64_t i = 2 + added; i < culled.size(); ++i) {
			if (ObjectModel *node = occlusion_node(registry, culled[i])) {
				note(node);
				node->set_inset_occlusion_hidden(false);
			} else if (statics != nullptr) {
				statics->set_static_instance_inset_occlusion_hidden(culled[i], false);
			}
		}
	}
	// The main view's drops it admitted take its explicit admit (Simulation's
	// view_admissions_since carries the rule), and its water mirror's likewise.
	for (int64_t i = 0; i < inset.admitted_changes.size(); ++i) {
		const int32_t bms_id = inset.admitted_changes[i];
		if (ObjectModel *node = occlusion_node(registry, bms_id)) {
			note(node);
			node->set_inset_occlusion_hidden(false);
		} else if (statics != nullptr) {
			statics->set_static_instance_inset_occlusion_hidden(bms_id, false);
		}
	}
	if (mirror_active_[Simulation::MIRROR_INSET]) {
		apply_mirror_admissions(Simulation::MIRROR_INSET);
	}
	// The local player's body, by the Inset collect's own verdict.
	if (ObjectModel *avatar = local_avatar()) {
		note(avatar);
		avatar->set_inset_occlusion_hidden(!inset.local_collected);
	}
	// The same collect over the rows the wire walk draws, keyed by handle,
	// and its own death-piece and flying-round draws.
	if (EntityPresenter *presenter = entities()) {
		presenter->present_death_piece_draws_inset(inset.draws.death_pieces);
		presenter->present_round_draws_inset(inset.draws.rounds);
		presenter->set_wire_inset_view(true);
		if (ScarPresenter *scars = presenter->scar_presenter()) {
			scars->set_inset_view(true, request.camera.origin);
		}
		const PackedInt32Array &wire = inset.culled_wire_changes;
		if (wire.size() >= 2) {
			const int64_t added = wire[0];
			for (int64_t i = 1; i < 1 + added; ++i) {
				presenter->set_render_culled_inset(wire[i], true);
			}
			for (int64_t i = 2 + added; i < wire.size(); ++i) {
				presenter->set_render_culled_inset(wire[i], false);
			}
		}
	}
}

void OcclusionFrame::apply_mirror_view(int p_view, bool p_ran, bool p_below_water) {
	Simulation *s = sim();
	EntityIndex *registry = entity_index();
	if (!p_ran || s == nullptr || registry == nullptr) {
		release_mirror(p_view);
		return;
	}
	mirror_active_[p_view] = true;
	ObjectModel::set_mirror_view_state(p_view, true, p_below_water);
	MissionObjectPlacer *statics = placer();
	if (statics != nullptr) {
		statics->set_static_mirror_view_state(p_view, true, p_below_water);
	}
	const MirrorOcclusionView &view = s->mirror_occlusion(static_cast<Simulation::MirrorView>(p_view));
	// Its building verdicts, as changes (the main leg's triple form): its raw
	// section mask and batch/TOC visibility.
	const PackedInt64Array &changes = view.building_changes;
	for (int64_t i = 0; i + 2 < changes.size(); i += 3) {
		if (ObjectModel *node = occlusion_node(registry, changes[i])) {
			node->set_mirror_occlusion_section_mask(p_view,
					Simulation::building_visibility_mask(changes[i + 1]), changes[i + 2]);
			node->set_mirror_occlusion_hidden(p_view,
					!Simulation::building_visibility_visible(changes[i + 1]));
		} else if (statics != nullptr) {
			statics->set_static_instance_mirror_occlusion_hidden(p_view,
					static_cast<int>(changes[i]),
					!Simulation::building_visibility_visible(changes[i + 1]));
		}
	}
	// Its entity render gates, as changes.
	const PackedInt32Array &culled = view.culled_changes;
	if (culled.size() >= 2) {
		const int64_t added = culled[0];
		for (int64_t i = 1; i < 1 + added; ++i) {
			if (ObjectModel *node = occlusion_node(registry, culled[i])) {
				node->set_mirror_occlusion_hidden(p_view, true);
			} else if (statics != nullptr) {
				statics->set_static_instance_mirror_occlusion_hidden(p_view, culled[i], true);
			}
		}
		for (int64_t i = 2 + added; i < culled.size(); ++i) {
			if (ObjectModel *node = occlusion_node(registry, culled[i])) {
				node->set_mirror_occlusion_hidden(p_view, false);
			} else if (statics != nullptr) {
				statics->set_static_instance_mirror_occlusion_hidden(p_view, culled[i], false);
			}
		}
	}
	// The local player's body, by this mirror's own verdict.
	if (ObjectModel *avatar = local_avatar()) {
		avatar->set_mirror_occlusion_hidden(p_view, !view.local_collected);
	}
	// The same collect over the rows the wire walk draws, keyed by handle, and
	// the subscene's own death-piece draws.
	if (EntityPresenter *presenter = entities()) {
		presenter->present_death_piece_draws_mirror(p_view, view.death_piece_draws);
		presenter->set_wire_mirror_view(p_view, true);
		const PackedInt32Array &wire = view.culled_wire_changes;
		if (wire.size() >= 2) {
			const int64_t added = wire[0];
			for (int64_t i = 1; i < 1 + added; ++i) {
				presenter->set_render_culled_mirror(p_view, wire[i], true);
			}
			for (int64_t i = 2 + added; i < wire.size(); ++i) {
				presenter->set_render_culled_mirror(p_view, wire[i], false);
			}
		}
	}
}

void OcclusionFrame::apply_draw_husk_leg() {
	Simulation *s = sim();
	EntityPresenter *presenter = entities();
	if (s == nullptr || presenter == nullptr) {
		return;
	}
	std::vector<opennova::world::HuskSwapEvent> swaps;
	std::vector<opennova::world::DrawFadeStamp> fades;
	s->take_draw_husk_output(swaps, fades);
	presenter->present_draw_husk_leg(swaps, fades);
}

void OcclusionFrame::apply_mirror_admissions(int p_view) {
	Simulation *s = sim();
	EntityIndex *registry = entity_index();
	if (s == nullptr || registry == nullptr) {
		return;
	}
	MissionObjectPlacer *statics = placer();
	const PackedInt32Array &admitted =
			s->mirror_occlusion(static_cast<Simulation::MirrorView>(p_view)).admitted_changes;
	for (int64_t i = 0; i < admitted.size(); ++i) {
		if (ObjectModel *node = occlusion_node(registry, admitted[i])) {
			node->set_mirror_occlusion_hidden(p_view, false);
		} else if (statics != nullptr) {
			statics->set_static_instance_mirror_occlusion_hidden(p_view, admitted[i], false);
		}
	}
}

void OcclusionFrame::release_mirror(int p_view) {
	if (!mirror_active_[p_view]) {
		return;
	}
	mirror_active_[p_view] = false;
	ObjectModel::set_mirror_view_state(p_view, false, false);
	if (MissionObjectPlacer *statics = placer()) {
		statics->set_static_mirror_view_state(p_view, false, false);
	}
	if (EntityPresenter *presenter = entities()) {
		presenter->set_wire_mirror_view(p_view, false);
	}
	if (Simulation *s = sim()) {
		s->release_mirror_occlusion(static_cast<Simulation::MirrorView>(p_view));
	}
}

void OcclusionFrame::release_inset() {
	release_mirror(Simulation::MIRROR_INSET);
	if (!inset_active_) {
		return;
	}
	inset_active_ = false;
	inset_foliage_mask_anchors_ = PackedVector3Array();
	// The strip back on the water layer, at the main view's label.
	if (Water *w = water()) {
		const bool main_water = w->is_main_view_water();
		w->set_blink_water_views(main_water, main_water);
	}
	for (const KeyValue<uint64_t, ObjectID> &entry : inset_nodes_) {
		if (ObjectModel *node = live<ObjectModel>(entry.value)) {
			node->clear_inset_occlusion();
		}
	}
	inset_nodes_.clear();
	if (MissionObjectPlacer *statics = placer()) {
		statics->clear_static_instance_inset_occlusion();
	}
	if (EntityPresenter *presenter = entities()) {
		presenter->set_wire_inset_view(false);
		if (ScarPresenter *scars = presenter->scar_presenter()) {
			scars->set_inset_view(false, Vector3());
		}
	}
	if (Simulation *s = sim()) {
		s->release_inset_occlusion();
	}
}

void OcclusionFrame::enter_probe_skip() {
	if (Water *w = water()) {
		w->set_blink_water_views(!blink_water_suppressed_, !blink_water_suppressed_);
	}
	release_overrides(false);
}

void OcclusionFrame::leave_probe_skip() {
	reset_apply_baseline();
}

ObjectModel *OcclusionFrame::occlusion_node(EntityIndex *p_registry, int64_t p_bms_id) {
	if (const ObjectID *cached = occlusion_node_cache_.getptr(p_bms_id)) {
		if (ObjectModel *node = live<ObjectModel>(*cached)) {
			return node;
		}
	}
	ObjectModel *node = p_registry->resolve_single(p_bms_id);
	if (node == nullptr) {
		occlusion_node_cache_.erase(p_bms_id);
		return nullptr;
	}
	occlusion_node_cache_[p_bms_id] = node->get_instance_id();
	return node;
}

void OcclusionFrame::release_overrides(bool /*p_reset_semantics*/) {
	release_mirror(Simulation::MIRROR_MAIN);
	release_inset();
	for (const KeyValue<int64_t, ObjectID> &entry : occlusion_node_cache_) {
		ObjectModel *node = occlusion_hidden_release_node(entry.key);
		if (node != nullptr) {
			node->set_occlusion_hidden(false);
			node->set_occlusion_section_mask(-1, 0);
		}
	}
	occlusion_node_cache_.clear();
	release_local_avatar();
	if (MissionObjectPlacer *statics = placer()) {
		statics->clear_static_instance_occlusion();
	}
	reset_apply_baseline();
}

void OcclusionFrame::rebind_placed_nodes() {
	occlusion_node_cache_.clear();
	reset_apply_baseline();
}

void OcclusionFrame::clear_wire_render_culled() {
	if (EntityPresenter *presenter = entities()) {
		presenter->clear_render_culled();
	}
}

ObjectModel *OcclusionFrame::occlusion_hidden_release_node(int64_t p_bms_id) const {
	const ObjectID *cached = occlusion_node_cache_.getptr(p_bms_id);
	return cached != nullptr ? live<ObjectModel>(*cached) : nullptr;
}

void OcclusionFrame::reset_apply_baseline() {
	if (Simulation *s = sim()) {
		s->reset_occlusion_apply_baseline();
	}
	clear_wire_render_culled();
}

void OcclusionFrame::reset() {
	reset_blink_frame_gates();
	release_overrides(true);
	// The runtime dies with the load (the world queue_frees its MissionRoot
	// right after this): forget the per-mission members, exactly as the
	// pass's per-frame get_runtime() re-read found null from here on.
	sim_id_ = ObjectID();
	index_id_ = ObjectID();
	entities_id_ = ObjectID();
	placer_id_ = ObjectID();
}

// The blink-letter and waterline pass gates (renderer/scene_pass_gates.h
// carries the witnesses): the indoors letter hides the terrain render (the
// skipped traversal also collects no detail cells), closes the scene core's
// two detail-foliage passes on the dispatcher beside it while the MODEL masks
// keep drawing, and hides the water mirror's sky bracket; the sky letter or an
// eye at/below the water hides the main frame's sky bracket (dome + sun/moon
// discs). The sun glow, the glint and the veil are never gated here.
void OcclusionFrame::apply_scene_pass_gates() {
	MissionEnvironment *env = environment();
	const bool eye_at_or_below_water =
			env != nullptr && env->is_underwater_overlay_view();
	const opennova::renderer::ScenePassGates gates =
			opennova::renderer::scene_pass_gates(blink_letters_,
					eye_at_or_below_water);
	// Edge-triggered like the letters themselves, so another owner's terrain
	// visibility write is never overridden on a steady frame.
	if (gates.terrain != terrain_gate_) {
		terrain_gate_ = gates.terrain;
		if (Terrain *t = terrain()) {
			t->set_visible(gates.terrain);
		}
	}
	// The dispatcher's setter is the edge (it acts only on a change).
	if (FoliageDispatcher *f = foliage()) {
		f->set_detail_passes_drawn(gates.detail_foliage);
	}
	// The slot drapes draw inside the terrain sector pass, so the terrain
	// gate skips them too; the slot captures keep running
	// (renderer::kRungSlotDrape carries the witness). The setter is the edge.
	if (SlotShadow *shadow = slot_shadow()) {
		shadow->set_terrain_pass_drawn(gates.terrain);
	}
	if (SkyDome *dome = sky()) {
		dome->set_pass_gates(gates.sky, gates.mirror_sky);
	}
	if (env != nullptr) {
		env->set_sky_dome_drawn(gates.sky);
	}
	if (Celestial *c = celestial()) {
		c->set_sky_pass_gates(gates.sky, gates.mirror_sky);
	}
}

void OcclusionFrame::reset_blink_frame_gates() {
	blink_letters_ = 0;
	if (!terrain_gate_) {
		if (Terrain *t = terrain()) {
			t->set_visible(true);
		}
	}
	terrain_gate_ = true;
	if (FoliageDispatcher *f = foliage()) {
		f->set_detail_passes_drawn(true);
	}
	if (SlotShadow *shadow = slot_shadow()) {
		shadow->set_terrain_pass_drawn(true);
	}
	if (SkyDome *dome = sky()) {
		dome->set_pass_gates(true, true);
	}
	if (MissionEnvironment *env = environment()) {
		env->set_sky_dome_drawn(true);
	}
	if (Celestial *c = celestial()) {
		c->set_sky_pass_gates(true, true);
	}
	if (Water *w = water()) {
		w->set_blink_water_views(true, true);
	}
	blink_indoors_ = false;
	blink_water_suppressed_ = false;
}
