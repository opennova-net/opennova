// The render-occlusion frame (the former occlusion_frame_pass.gd): see the
// header for the preserved order and the ownership-bit contract.
#include "world/occlusion_frame.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <algorithm>

#include "env/celestial.h"
#include "env/mission_environment.h"
#include "env/sky_dome.h"
#include "env/water.h"
#include "object/entity_index.h"
#include "object/object_model.h"
#include "simulation/entity_presenter.h"
#include "simulation/simulation.h"
#include "terrain/terrain.h"

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

} // namespace

void OcclusionFrame::_bind_methods() {
	ClassDB::bind_method(D_METHOD("setup", "terrain", "sky", "celestial", "water", "env"),
			&OcclusionFrame::setup);
	ClassDB::bind_method(D_METHOD("bind_mission", "sim", "index", "entities"),
			&OcclusionFrame::bind_mission);
	ClassDB::bind_method(D_METHOD("set_frame_stats", "board"), &OcclusionFrame::set_frame_stats);
	ClassDB::bind_method(D_METHOD("apply_blink_gates", "forces_indoors"),
			&OcclusionFrame::apply_blink_gates);
	ClassDB::bind_method(D_METHOD("apply_frame", "camera", "camera_xform", "forces_indoors"),
			&OcclusionFrame::apply_frame);
	ClassDB::bind_method(D_METHOD("enter_probe_skip"), &OcclusionFrame::enter_probe_skip);
	ClassDB::bind_method(D_METHOD("leave_probe_skip"), &OcclusionFrame::leave_probe_skip);
	ClassDB::bind_method(D_METHOD("rebind_placed_nodes"), &OcclusionFrame::rebind_placed_nodes);
	ClassDB::bind_method(D_METHOD("reset"), &OcclusionFrame::reset);
	ClassDB::bind_method(D_METHOD("is_blink_indoors"), &OcclusionFrame::is_blink_indoors);
	ClassDB::bind_method(D_METHOD("set_probe_timing", "enabled"), &OcclusionFrame::set_probe_timing);
	ClassDB::bind_method(D_METHOD("is_probe_timing"), &OcclusionFrame::is_probe_timing);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "blink_indoors"), "", "is_blink_indoors");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "probe_timing"), "set_probe_timing", "is_probe_timing");
}

void OcclusionFrame::setup(Terrain *p_terrain, SkyDome *p_sky, Celestial *p_celestial,
		Water *p_water, MissionEnvironment *p_env) {
	terrain_id_ = id_of(p_terrain);
	sky_id_ = id_of(p_sky);
	celestial_id_ = id_of(p_celestial);
	water_id_ = id_of(p_water);
	env_id_ = id_of(p_env);
}

void OcclusionFrame::bind_mission(const Ref<Simulation> &p_sim, const Ref<EntityIndex> &p_index,
		EntityPresenter *p_entities) {
	sim_id_ = id_of(p_sim.ptr());
	index_id_ = id_of(p_index.ptr());
	entities_id_ = id_of(p_entities);
}

void OcclusionFrame::set_frame_stats(const Ref<FrameStats> &p_board) {
	frame_stats_ = p_board;
}

Simulation *OcclusionFrame::sim() const { return live<Simulation>(sim_id_); }
EntityIndex *OcclusionFrame::entity_index() const { return live<EntityIndex>(index_id_); }
EntityPresenter *OcclusionFrame::entities() const { return live<EntityPresenter>(entities_id_); }
Terrain *OcclusionFrame::terrain() const { return live<Terrain>(terrain_id_); }
SkyDome *OcclusionFrame::sky() const { return live<SkyDome>(sky_id_); }
Celestial *OcclusionFrame::celestial() const { return live<Celestial>(celestial_id_); }
Water *OcclusionFrame::water() const { return live<Water>(water_id_); }
MissionEnvironment *OcclusionFrame::environment() const { return live<MissionEnvironment>(env_id_); }

void OcclusionFrame::apply_blink_gates(bool p_forces_indoors) {
	Simulation *s = sim();
	if (s == nullptr) {
		return;
	}
	// The mission force-indoors attribute ORs the indoors letter into the frame
	// view for BOTH consumers, matching run_occlusion_frame's camera input
	// [orig: Bms_AttribFlags & 0x10 @ 0x5ca1c8 -> accum |= 2].
	const int flags = s->local_player_blink_flags() |
			(p_forces_indoors ? static_cast<int>(Simulation::BLINK_INDOORS) : 0);
	// Accum bit 0x2 (indoors): the terrain render is skipped entirely — the
	// near-detail and far-foliage tiers are terrain children here, matching
	// retail where the detail cells ride the skipped terrain traversal and the
	// far patches carry their own bit-2 gate — and the skybox pass (dome +
	// celestials) is skipped [orig: render_main_scene @ 0x5c1353 (PolyTrn
	// skip), terrain_scene_render @ 0x5d0570, Terrain_RenderSkyboxPass skip
	// @ 0x5ca84f, Foliage_RenderFarPatchesPass skips @ 0x5c95bf/0x5c9665].
	const bool indoors = (flags & static_cast<int>(Simulation::BLINK_INDOORS)) != 0;
	if (indoors != blink_indoors_) {
		blink_indoors_ = indoors;
		if (Terrain *t = terrain()) {
			t->set_visible(!indoors);
		}
		if (SkyDome *dome = sky()) {
			dome->set_visible(!indoors);
		}
		if (Celestial *c = celestial()) {
			c->set_visible(!indoors);
		}
	}
	// Accum bit 0x8 (the authored water letter): both water passes skipped.
	// Letter bits only accumulate while inside a box, so the outdoors leg of
	// retail's override is implicit; the remaining g_BlinkWaterVisible legs
	// (a camera building straddling the water plane, the window latch) ride
	// the section-mask slice [orig: Terrain_RenderSceneWithReflection
	// @ 0x5c93cb / @ 0x5c95d2-0x5c95ea].
	const bool water_off = (flags & static_cast<int>(Simulation::BLINK_WATER_OFF)) != 0;
	if (water_off != blink_water_suppressed_) {
		blink_water_suppressed_ = water_off;
		if (Water *w = water()) {
			w->set_visible(!water_off);
		}
	}
}

void OcclusionFrame::apply_frame(Camera3D *p_camera, const Transform3D &p_camera_xform,
		bool p_forces_indoors) {
	Simulation *s = sim();
	if (s == nullptr) {
		return;
	}
	EntityIndex *registry = entity_index();
	if (registry == nullptr) {
		return;
	}
	double fov_y = 70.0;
	double near = 0.05;
	double aspect = 16.0 / 9.0;
	if (p_camera != nullptr) {
		fov_y = p_camera->get_fov();
		near = p_camera->get_near();
		if (Viewport *viewport = p_camera->get_viewport()) {
			const Vector2 vs = viewport->get_visible_rect().size;
			if (vs.y > 0.0f) {
				aspect = static_cast<double>(vs.x) / static_cast<double>(vs.y);
			}
		}
	}
	double fog = 1000.0;
	MissionEnvironment *env = environment();
	if (env != nullptr) {
		fog = env->get_fog_distance();
	}
	Water *w = water();
	double water_z = -100000.0;
	if (w != nullptr && w->is_water_render_active()) {
		water_z = w->get_water_height();
	}
	const bool stats_on = frame_stats_.is_valid() && frame_stats_->is_capture_active();
	const bool timing = probe_timing_ || stats_on;
	const int64_t native_start = timing ? ticks_usec() : 0;
	s->run_occlusion_frame(p_camera_xform, fov_y, aspect, near, fog, water_z, p_forces_indoors);
	const int64_t native_end = timing ? ticks_usec() : 0;
	int64_t building_query_us = 0;
	int64_t building_apply_us = 0;
	int64_t cull_query_us = 0;
	int64_t cull_apply_us = 0;
	int64_t light_query_us = 0;
	int64_t light_apply_us = 0;
	int64_t water_apply_us = 0;

	// Building batch visibility + per-section masks (bit N = render part N,
	// forced-visible def bits already merged by the sim), applied as CHANGES:
	// the sim diffs against what this shell last applied, so a steady frame
	// walks nothing. Batch culls claim the occlusion-hidden bit; the same
	// verdicts as the full-walk form land on the nodes.
	// [orig: Terrain_RenderSectorModels @ 0x5c5d30]
	const int64_t building_query_start = timing ? ticks_usec() : 0;
	const PackedInt64Array changes = s->get_building_visibility_changes();
	if (timing) {
		building_query_us = ticks_usec() - building_query_start;
	}
	const int64_t building_apply_start = timing ? ticks_usec() : 0;
	for (int64_t i = 0; i + 1 < changes.size(); i += 2) {
		const int64_t bms_id = changes[i];
		ObjectModel *node = occlusion_node(registry, bms_id);
		if (node == nullptr) {
			continue; // a node-less (batched static) verdict: the blind spot named in the header
		}
		const int64_t packed = changes[i + 1];
		node->set_section_visibility_mask(Simulation::building_visibility_mask(packed));
		node->set_occlusion_hidden(!Simulation::building_visibility_visible(packed));
	}
	if (timing) {
		building_apply_us = ticks_usec() - building_apply_start;
	}

	// Entity render gates (the blink-hits gate + the outdoors three-ray latch),
	// also applied as changes. [orig: the collector gates @ 0x5c7022-0x5c708a / §3.4]
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
			}
		}
		for (int64_t i = 2 + added; i < culled_changes.size(); ++i) {
			ObjectModel *node = occlusion_node(registry, culled_changes[i]);
			if (node != nullptr) {
				node->set_occlusion_hidden(false);
			}
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
	if (timing) {
		cull_apply_us = ticks_usec() - cull_apply_start;
	}

	// The per-drawn-entity sun-visibility factor (D-RLIT-3), also applied as
	// changes: quality 1..4 maps to effectScale quality*0.25, dimming only the
	// directional term (engine/runtime/renderer/light_runtime.h
	// sun_visibility_factor owns the witness). Placed and wire identities share
	// this triple feed; wire rays use the separately keyed 17-tick candidate
	// arena, and the entity presenter retains the value for cold bodies/weapons.
	// [orig: setup_terrain_effect_for_entity @ 0x5c74a0, pushed per sector
	// entity draw @ 0x5c7bff]
	if (env != nullptr) {
		const int64_t light_query_start = timing ? ticks_usec() : 0;
		const PackedInt64Array sun_changes = s->get_draw_lighting_changes(env->get_light_direction());
		if (timing) {
			light_query_us = ticks_usec() - light_query_start;
		}
		const int64_t light_apply_start = timing ? ticks_usec() : 0;
		EntityPresenter *presenter = entities();
		for (int64_t i = 0; i + 2 < sun_changes.size(); i += 3) {
			const int wire_handle = static_cast<int>(sun_changes[i]);
			const int64_t bms_id = sun_changes[i + 1];
			// quality -> effectScale maps engine-side (one owner:
			// renderer::sun_visibility_factor via sun_quality_factor).
			const float effect_scale = s->sun_quality_factor(static_cast<int>(sun_changes[i + 2]));
			if (wire_handle >= 0) {
				if (presenter != nullptr) {
					presenter->set_entity_lighting_context(wire_handle, effect_scale, false, 0.0f);
				}
				continue;
			}
			ObjectModel *sun_node = occlusion_node(registry, bms_id);
			if (sun_node != nullptr) {
				sun_node->set_entity_lighting_context(effect_scale, false, 0.0f);
			}
		}
		if (timing) {
			light_apply_us = ticks_usec() - light_apply_start;
		}
	}

	// The g_BlinkWaterVisible override legs the slice-1 gate deferred: with the
	// authored water letter suppressing (accum bit 0x8), the water still renders
	// when the frame latched the exterior or a camera building straddles the
	// water plane. [orig: @ 0x5c93cb / @ 0x5c95d2 + g_BlinkWaterVisible
	// @ 0x29ACE40]
	const int64_t water_apply_start = timing ? ticks_usec() : 0;
	if (w != nullptr) {
		w->set_visible(!blink_water_suppressed_ || s->occlusion_water_visible());
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

void OcclusionFrame::enter_probe_skip() {
	if (Water *w = water()) {
		w->set_visible(!blink_water_suppressed_);
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
	for (const KeyValue<int64_t, ObjectID> &entry : occlusion_node_cache_) {
		ObjectModel *node = occlusion_hidden_release_node(entry.key);
		if (node != nullptr) {
			node->set_occlusion_hidden(false);
			node->set_section_visibility_mask(-1);
		}
	}
	occlusion_node_cache_.clear();
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
}

void OcclusionFrame::reset_blink_frame_gates() {
	if (blink_indoors_) {
		if (Terrain *t = terrain()) {
			t->set_visible(true);
		}
		if (SkyDome *dome = sky()) {
			dome->set_visible(true);
		}
		if (Celestial *c = celestial()) {
			c->set_visible(true);
		}
	}
	if (blink_water_suppressed_) {
		if (Water *w = water()) {
			w->set_visible(true);
		}
	}
	blink_indoors_ = false;
	blink_water_suppressed_ = false;
}
