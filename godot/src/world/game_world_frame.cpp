// GameWorld's frame: the ONE static leg table the witnessed per-frame order
// lives in (ADR 0043 decision 9), the loop that runs it with one RAII
// LegScope timing, the frozen-pose capture replay and the editor preview
// refresh as the second and third tables through the same loop, the leg
// bodies with their witness citations, and the per-frame device latch. Every
// leg table lives in this file: a row elsewhere would be a second frame
// order. The former game_frame_pipeline.gd + world_device_frame.gd
// (slice G10).

#include "world/game_world.h"
#include "render/render_view.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "audio/music_director.h"
#include "env/env_records.h"
#include "lights/light_scene.h"
#include "object/object_shader_cache.h"

using namespace godot;

namespace {

int64_t now_us() {
	return static_cast<int64_t>(Time::get_singleton()->get_ticks_usec());
}

// ONE RAII leg timer: samples the clock only while a board is active, banks
// the span on that board's slot when the leg body returns. A row with no
// slot (-1) or a frame with no capturing board reads no clock at all; a leg
// that reports it did not run cancels the scope so its slot's sample count
// stays the count of frames the leg actually ran (blink only on ticked
// frames, the session only while a runtime plays).
struct LegScope {
	FrameStats *stats;
	int slot;
	int64_t start_us;

	LegScope(FrameStats *p_stats, int p_slot) :
			stats(p_slot >= 0 ? p_stats : nullptr), slot(p_slot),
			start_us(stats != nullptr ? now_us() : 0) {}
	void cancel() { stats = nullptr; }
	~LegScope() {
		if (stats != nullptr) {
			stats->add(slot, now_us() - start_us);
		}
	}
};

constexpr int kNoSlot = -1;

} // namespace

// --- THE LEG TABLE (the point of the slice) ----------------------------------
// The witnessed order, one row per device leg, in the order the frame runs
// them around inmatch::Session::advance() (the session row). The leg bodies
// below carry the witness citations; the row comments carry the ordering
// rationale the old pipeline kept beside each call.
const GameWorld::FrameLeg GameWorld::kFrameLegs[] = {
	// The per-frame camera/timing latch (panm clock sample, probe-skip latches).
	{ "begin", kNoSlot, &GameWorld::leg_begin, kLegNone },
	// The one typed session call; a terminal outcome emits session_lost and
	// STOPS the frame, so later device phases cannot run against a lost or
	// failed session.
	{ "session", FrameStats::WORLD_RUNTIME, &GameWorld::leg_session, kStopsFrame },
	// Place the local-player camera/viewmodel from the state the session tick
	// just produced, BEFORE every camera-driven render leg reads it (D-RORD-8).
	{ "local_view", FrameStats::WORLD_LOCAL_VIEW, &GameWorld::leg_local_view, kLegNone },
	// Retail re-applies fog/ambient per scene pass. Classify the adjusted
	// render eye after camera placement and publish that pass payload before
	// terrain, foliage, objects, viewmodel, and particles consume it.
	{ "scene_environment", FrameStats::WORLD_SCENE_ENV, &GameWorld::leg_scene_environment, kLegNone },
	// The environment presenters (weather smoothing, sun direction, sky dome,
	// celestial bodies): ex-self-clocked _process nodes, now ordered here so
	// they take THIS frame's render eye and every later leg consumes what they
	// published this frame, never last frame's.
	{ "environment_nodes", FrameStats::WORLD_ENV_NODES, &GameWorld::leg_environment_nodes, kLegNone },
	// Terrain samples the viewport camera directly and tracks the visible
	// terrain bounds the water leg's g_WaterActive test reads, so it runs
	// first: in the original frame terrain_setup_view_and_lighting precedes
	// the reflection prerender and the water pass draws after the terrain
	// pass (the witnessed order is cited at the engine's render frame,
	// docs/render/render-order-re.md); the water noise the terrain binds is
	// the previous frame's, exactly as the original water pass regenerates it
	// after the terrain draw.
	{ "terrain", FrameStats::WORLD_TERRAIN, &GameWorld::leg_terrain, kLegNone },
	{ "water", FrameStats::WORLD_WATER, &GameWorld::leg_water, kLegNone },
	// Foliage receives the same live render transform and consumes this
	// frame's detail-cell handoff, so it follows terrain (banked at finish).
	{ "foliage", kNoSlot, &GameWorld::leg_foliage, kLegNone },
	// Net-session edges; a failed streamed-asset leg STOPS the frame.
	{ "network", FrameStats::WORLD_NETWORK_FRAME, &GameWorld::leg_network, kStopsFrame },
	// Blink flags only change on sim ticks (the body gates on did_tick).
	{ "blink", FrameStats::WORLD_BLINK, &GameWorld::leg_blink, kLegNone },
	// The OCCL_* slots land inside OcclusionFrame.apply_frame itself.
	{ "occlusion", kNoSlot, &GameWorld::leg_occlusion, kLegNone },
	{ "iris", FrameStats::WORLD_IRIS, &GameWorld::leg_iris, kLegNone },
	// The sun-veil stop-down feed for the weather ticks banked above (the
	// veil alpha itself rides the Celestial shader-global push).
	{ "sun_veil", FrameStats::WORLD_SUN_VEIL, &GameWorld::leg_sun_veil, kLegNone },
	// The EffectWorld point-light select for this camera (after iris
	// publishes the frame's ambient scale, before the lit material draws
	// consume the pushed globals).
	{ "lights", FrameStats::WORLD_LIGHT, &GameWorld::leg_lights, kLegNone },
	// Per-model runtime advance (ex-self-clocked ObjectModel _process): after
	// occlusion resolves visibility, before the particle composite over it.
	{ "materials", FrameStats::WORLD_MATERIAL, &GameWorld::leg_materials, kLegNone },
	// Compile focused Q3 only after this frame's celestial, water, occlusion,
	// and object-material producers have published their final transforms
	// and parameters. The typed snapshot is consumed by the terminal
	// compositor; it is never self-clocked from a stale process callback.
	{ "framefx", FrameStats::WORLD_FRAMEFX, &GameWorld::leg_framefx, kLegNone },
	// The render-slot ground shadows plan against the light select published
	// above and stamp the model subtrees the material frame just rebuilt
	// (ex-self-clocked SlotShadow _process, which ran after the whole frame).
	{ "slot_shadows", FrameStats::WORLD_SLOT_SHADOW, &GameWorld::leg_slot_shadows, kLegNone },
	{ "particles", FrameStats::WORLD_PARTICLES, &GameWorld::leg_particles, kLegNone },
	// The precipitation streaks after the particle pass and the trails,
	// before the murk overlay (retail Terrain_RenderSceneWithReflection
	// @ 0x5c96a6).
	{ "precipitation", FrameStats::WORLD_WEATHER, &GameWorld::leg_precipitation, kLegNone },
	// The audio leg (banked at finish).
	{ "audio", kNoSlot, &GameWorld::leg_audio, kLegNone },
	{ "clear", FrameStats::WORLD_CLEAR, &GameWorld::leg_clear, kLegNone },
	{ "environment_cube", FrameStats::WORLD_ENV_CUBE, &GameWorld::leg_environment_cube, kLegNone },
	// Banks the foliage/audio counters + the RENDER_WATER_* / RENDER_Q3_* /
	// RENDER_SLOT_* rows.
	{ "finish", kNoSlot, &GameWorld::leg_finish, kLegNone },
};
const int GameWorld::kFrameLegCount = sizeof(GameWorld::kFrameLegs) / sizeof(GameWorld::kFrameLegs[0]);

// The frozen-pose capture replay (debug_refresh_render_pose): re-evaluates
// only camera-dependent production render state for an exact-pose visual
// capture. The caller must first make the camera current and stop its normal
// presenter. This deliberately does not drive the mission session, weather
// clock, material animation, particles, or audio. Accumulator state that the
// live pipeline needs many frames to reach (the iris exposure chase, the
// glare occlusion window) is instead SETTLED at the capture pose through the
// witnessed per-tick math, so a frozen fixture measures the steady state a
// resting retail camera shows rather than a starved accumulator (D-RLIT-2
// fixture starvation). Same loop, delta 0, no timing.
const GameWorld::FrameLeg GameWorld::kFrozenPoseRefresh[] = {
	// The celestial device normally self-refreshes during a live frame; drive
	// it first at the frozen pose so the settled glare brightness, the veil
	// alpha global, and this frame's stop-down all exist before the exposure
	// settle chases them.
	{ "celestial_settle", kNoSlot, &GameWorld::leg_celestial_settle, kLegNone },
	{ "sun_veil", kNoSlot, &GameWorld::leg_sun_veil, kLegNone },
	// The frozen path never runs the live iris/exposure legs, so a fixture
	// used to publish the modulator's mission-reset identity gain -- the flat
	// exposure half of the D-RLIT-2 fixture starvation. Stamp the marched
	// samples for the capture pose and chase the modulator to its settled
	// state through the witnessed math only (Weather.settle_exposure holds
	// the freeze contract: no weather time, no mission clock).
	{ "iris_stamp", kNoSlot, &GameWorld::leg_iris_stamp, kLegNone },
	{ "weather_settle", kNoSlot, &GameWorld::leg_weather_settle, kLegNone },
	// Keep the same camera-producer order as the live table, omitting every
	// time-owning leg. Terrain publishes the detail-cell handoff consumed by
	// foliage; occlusion then resolves the world visibility for this exact
	// view.
	{ "scene_environment", kNoSlot, &GameWorld::leg_scene_environment, kLegNone },
	{ "terrain", kNoSlot, &GameWorld::leg_terrain, kLegNone },
	{ "foliage", kNoSlot, &GameWorld::leg_foliage, kLegNone },
	{ "occlusion", kNoSlot, &GameWorld::leg_occlusion, kLegNone },
	// These native devices advance as live legs (sky/celestial before
	// terrain, water between terrain and foliage). A fixture freezes their
	// parent before moving the capture camera, so drive their public
	// zero-delta frame seams explicitly after that move.
	{ "sky_settle", kNoSlot, &GameWorld::leg_sky_settle, kLegNone },
	{ "water_settle", kNoSlot, &GameWorld::leg_water_settle, kLegNone },
	// Rebuild the particle draw lists for the moved capture camera. The
	// effect SIM stays frozen (only fixed ticks advance it, and the runtime
	// is paused); render_frame re-orients billboards and re-attaches the
	// compositor to the now-current view. Without this the last pre-freeze
	// draw list -- built for the old camera pose -- is all that renders, and
	// captures lose every live emitter (the 00TRa fire-barrel flame was the
	// exposing case).
	{ "particles", kNoSlot, &GameWorld::leg_particles, kLegNone },
	// Reselect the point lights for the fixture camera the same way (the
	// flicker phase freezes with the weather ring, matching the phase
	// contract).
	{ "lights", kNoSlot, &GameWorld::leg_lights, kLegNone },
	// Re-plan the render-slot ground shadows for the moved capture camera
	// (slot priority and the capture poses are camera-relative).
	{ "slot_shadows", kNoSlot, &GameWorld::leg_slot_shadows, kLegNone },
	{ "clear", kNoSlot, &GameWorld::leg_clear, kLegNone },
};
const int GameWorld::kFrozenPoseRefreshCount =
		sizeof(GameWorld::kFrozenPoseRefresh) / sizeof(GameWorld::kFrozenPoseRefresh[0]);

// The editor preview refresh (refresh_preview): the OpenNova World plugin
// re-renders the loaded native documents at the fixed authored time from the
// editor camera, once per editor frame, through a scoped RenderView. No
// session, kernel, wall clock, script, audio, or effect simulation enters
// this table. Unlike the frozen replay the preview's environment nodes are
// not frozen, so the live device legs advance them here at delta 0. The
// preview never creates a Simulation, so the weather runtime never ticks;
// the settle prefix below is what lifts every refresh off the mission-reset
// identity gain (the D-RLIT-2 starved look). Same loop, delta 0, no timing.
const GameWorld::FrameLeg GameWorld::kPreviewRefresh[] = {
	// The frozen replay's settle prefix, minus iris_stamp: the iris march
	// is a Simulation query (stamp_iris_samples returns without one), so a
	// preview stamps no samples and feed_exposure_target takes retail's
	// no-sample outdoor fallback, which weather_settle chases to its fixed
	// point.
	{ "celestial_settle", kNoSlot, &GameWorld::leg_celestial_settle, kLegNone },
	{ "sun_veil", kNoSlot, &GameWorld::leg_sun_veil, kLegNone },
	{ "weather_settle", kNoSlot, &GameWorld::leg_weather_settle, kLegNone },
	// The live camera-producer order at delta 0. environment_nodes and
	// water are the live legs whose zero-delta seams the frozen replay
	// reaches through sky_settle / water_settle; the preview runs the legs
	// themselves, so those two settle rows are not repeated.
	{ "scene_environment", kNoSlot, &GameWorld::leg_scene_environment, kLegNone },
	{ "environment_nodes", kNoSlot, &GameWorld::leg_environment_nodes, kLegNone },
	{ "terrain", kNoSlot, &GameWorld::leg_terrain, kLegNone },
	{ "water", kNoSlot, &GameWorld::leg_water, kLegNone },
	{ "foliage", kNoSlot, &GameWorld::leg_foliage, kLegNone },
	// The point-light select for the editor camera: the preview load
	// re-attaches the placed models' authored light records (the director
	// walk mission start runs), so the lit statics, the terrain light rows
	// and the coronas render as in the game.
	{ "lights", kNoSlot, &GameWorld::leg_lights, kLegNone },
	{ "materials", kNoSlot, &GameWorld::leg_materials, kLegNone },
	{ "clear", kNoSlot, &GameWorld::leg_clear, kLegNone },
	// The chrome/environment cube: retail's forced + every-128-frames
	// capture, centered on the render camera when no player exists. This
	// is the leg that honours update_preview_settings' force_capture.
	{ "environment_cube", kNoSlot, &GameWorld::leg_environment_cube, kLegNone },
	// Omitted frozen-replay rows, each for a stated reason:
	//   iris_stamp   -- stamp_iris_samples needs a Simulation (above).
	//   occlusion    -- OcclusionFrame.apply_frame is the Simulation's
	//                   visibility walk; without one every placed node
	//                   stays visible, which is the preview's contract.
	//   sky_settle, water_settle -- environment_nodes / water run here.
	//   particles    -- the preview never starts the EffectWorld.
	//   slot_shadows -- the render-slot shadow pass is a runtime
	//                   compositor SlotShadow keeps dormant in the editor.
	// Omitted live rows: begin / finish (the timing latch and the wall-clock
	// panm sample the preview deliberately never takes), session,
	// local_view, network, blink, iris (the live sampler; needs a
	// Simulation), framefx (runtime compositor, dormant in the editor),
	// precipitation (needs the runtime), audio.
};
const int GameWorld::kPreviewRefreshCount =
		sizeof(GameWorld::kPreviewRefresh) / sizeof(GameWorld::kPreviewRefresh[0]);

PackedStringArray GameWorld::frame_leg_names() {
	PackedStringArray names;
	for (int i = 0; i < kFrameLegCount; ++i) {
		names.append(kFrameLegs[i].name);
	}
	return names;
}

PackedStringArray GameWorld::frozen_pose_leg_names() {
	PackedStringArray names;
	for (int i = 0; i < kFrozenPoseRefreshCount; ++i) {
		names.append(kFrozenPoseRefresh[i].name);
	}
	return names;
}

PackedStringArray GameWorld::preview_leg_names() {
	PackedStringArray names;
	for (int i = 0; i < kPreviewRefreshCount; ++i) {
		names.append(kPreviewRefresh[i].name);
	}
	return names;
}

bool GameWorld::frame_leg_stops_frame(const String &p_name) {
	for (int i = 0; i < kFrameLegCount; ++i) {
		if (p_name == kFrameLegs[i].name) {
			return (kFrameLegs[i].flags & kStopsFrame) != 0;
		}
	}
	return false;
}

// --- the loop -------------------------------------------------------------------

void GameWorld::run_leg_table(const FrameLeg *p_legs, int p_count, FrameContext &r_ctx, bool p_timed) {
	for (int i = 0; i < p_count; ++i) {
		const FrameLeg &leg = p_legs[i];
		LegResult result = kLegRan;
		{
			// The board is latched by the begin row (frame_stats_on_), so every
			// leg in one frame writes to the same board even if the overlay
			// changes page during that frame.
			LegScope scope(p_timed && frame_stats_on_ ? frame_stats_.ptr() : nullptr, leg.slot);
			result = (this->*leg.run)(r_ctx);
			if (result == kLegSkipped) {
				scope.cancel();
			}
		}
		if (result == kLegStop && (leg.flags & kStopsFrame) != 0) {
			return;
		}
	}
}

void GameWorld::tick(const Vector3 &p_camera_pos, const Transform3D &p_camera_xform, double p_delta,
		const Ref<MissionFrameInput> &p_frame_input) {
	double delta = p_delta;
	if (delta < 0.0) {
		delta = Simulation::tick_dt();
	}
	advance_frame(p_camera_pos, p_camera_xform, delta, p_frame_input);
}

Ref<MissionFrameOutcome> GameWorld::advance_frame(const Vector3 &p_camera_pos,
		const Transform3D &p_camera_xform, double p_delta, const Ref<MissionFrameInput> &p_input) {
	if (preview_active_) return Ref<MissionFrameOutcome>();
	Ref<MissionFrameInput> input = p_input;
	if (input.is_null()) {
		input.instantiate();
	}
	input->set_delta_seconds(p_delta);
	input->set_camera_sample(p_camera_pos, -p_camera_xform.basis.get_column(2), true);
	frame_camera_pos_ = p_camera_pos;
	frame_camera_xform_ = p_camera_xform;
	frame_delta_ = p_delta;
	FrameContext ctx;
	ctx.input = input;
	run_leg_table(kFrameLegs, kFrameLegCount, ctx, true);
	return ctx.outcome;
}

Error GameWorld::debug_refresh_render_pose(Camera3D *p_camera) {
	if (!world_ready_ || !is_inside_tree()) {
		return ERR_UNAVAILABLE;
	}
	if (p_camera == nullptr || !p_camera->is_inside_tree() || !p_camera->is_current() ||
			p_camera->get_viewport() != get_viewport()) {
		return ERR_INVALID_PARAMETER;
	}
	frame_camera_pos_ = p_camera->get_camera_transform().origin;
	frame_camera_xform_ = p_camera->get_global_transform();
	FrameContext ctx;
	run_leg_table(kFrozenPoseRefresh, kFrozenPoseRefreshCount, ctx, false);
	return OK;
}

// --- the table rows -------------------------------------------------------------

GameWorld::LegResult GameWorld::leg_begin(FrameContext &r_ctx) {
    if (MissionAudio *audio = get_mission_audio()) {
        audio->set_listener_position(frame_camera_pos_);
    }
	begin_device_frame();
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_session(FrameContext &r_ctx) {
	MissionRoot *presentation = get_runtime();
	if (presentation == nullptr || !presentation->is_playing()) {
		return kLegSkipped;
	}
	const int64_t runtime_start = frame_timing_ ? now_us() : 0;
	r_ctx.outcome = presentation->advance_session_frame(r_ctx.input);
	if (frame_timing_) {
		perf_runtime_us_ = now_us() - runtime_start;
	}
	if (r_ctx.outcome.is_valid() && r_ctx.outcome->is_terminal()) {
		session_frame_failed(r_ctx.outcome->get_error());
		return kLegStop;
	}
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_local_view(FrameContext &r_ctx) {
	present_local_view_frame();
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_scene_environment(FrameContext &r_ctx) {
	apply_scene_environment_frame();
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_environment_nodes(FrameContext &r_ctx) {
	render_environment_nodes_frame();
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_terrain(FrameContext &r_ctx) {
	render_terrain_frame();
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_water(FrameContext &r_ctx) {
	render_water_frame();
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_foliage(FrameContext &r_ctx) {
	render_foliage_frame();
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_network(FrameContext &r_ctx) {
	return drive_network_frame() ? kLegRan : kLegStop;
}

GameWorld::LegResult GameWorld::leg_blink(FrameContext &r_ctx) {
	// Blink flags only change on sim ticks; the leg runs only after a batch
	// that ran at least one.
	if (!(r_ctx.outcome.is_valid() && r_ctx.outcome->did_tick())) {
		return kLegSkipped;
	}
	apply_blink_frame();
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_occlusion(FrameContext &r_ctx) {
	apply_occlusion_frame();
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_iris(FrameContext &r_ctx) {
	// The iris feed samples only a ready world (the body gates the same way);
	// an idle world banks nothing here.
	if (!world_ready_) {
		return kLegSkipped;
	}
	sample_iris_frame();
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_sun_veil(FrameContext &r_ctx) {
	render_sun_veil_frame();
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_lights(FrameContext &r_ctx) {
	render_light_frame();
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_materials(FrameContext &r_ctx) {
	render_material_frame();
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_framefx(FrameContext &r_ctx) {
	sync_framefx_frame();
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_slot_shadows(FrameContext &r_ctx) {
	render_slot_shadow_frame();
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_particles(FrameContext &r_ctx) {
	render_particle_frame();
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_precipitation(FrameContext &r_ctx) {
	render_precipitation_frame();
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_audio(FrameContext &r_ctx) {
	mix_audio_frame(r_ctx.outcome.is_valid() ? r_ctx.outcome->get_ticks_run() : 0);
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_clear(FrameContext &r_ctx) {
	update_clear_frame();
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_environment_cube(FrameContext &r_ctx) {
	render_environment_cube_frame();
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_finish(FrameContext &r_ctx) {
	finish_device_frame();
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_celestial_settle(FrameContext &r_ctx) {
	if (celestial_ != nullptr) {
		// The glare occlusion brightness accumulates over ~a dozen live
		// frames; a frozen fixture gets exactly one zero-delta advance, which
		// left the sun glow invisible at any pose (the D-RLIT-2 fixture
		// starvation's other half). Settle the witnessed ray/window/step leg
		// at this pose first, then publish it through the normal frame.
		celestial_->settle_glare_occlusion();
		celestial_->advance_frame(0.0);
	}
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_iris_stamp(FrameContext &r_ctx) {
	stamp_iris_samples(frame_camera_xform_);
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_weather_settle(FrameContext &r_ctx) {
	Weather *settle_weather = get_weather_node();
	if (settle_weather != nullptr) {
		settle_weather->settle_exposure();
	}
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_sky_settle(FrameContext &r_ctx) {
	if (sky_dome_ != nullptr) {
		sky_dome_->advance_frame(0.0);
	}
	return kLegRan;
}

GameWorld::LegResult GameWorld::leg_water_settle(FrameContext &r_ctx) {
	if (water_ != nullptr) {
		// Water's public frame seam retargets the mirror/strip and advances
		// its render-noise counter exactly once. The fixture freezes
		// immediately after this call and records that non-canonical phase in
		// its manifest.
		water_->advance_frame(0.0);
	}
	return kLegRan;
}

// --- the per-frame latch ----------------------------------------------------------

void GameWorld::begin_device_frame() {
	device_frame_start_us_ = now_us();
	sample_panm_clock();
	frame_probe_enabled_ = perf_probe_enabled_;
	frame_stats_on_ = frame_stats_.is_valid() && frame_stats_->is_capture_active();
	frame_timing_ = frame_probe_enabled_ || frame_stats_on_;
	frame_skip_occlusion_ = frame_probe_enabled_ && perf_probe_skip_occl_;
	perf_foliage_us_ = 0;
	perf_runtime_us_ = 0;
	perf_audio_us_ = 0;
	if (frame_probe_enabled_ && world_ready_) {
		if (frame_skip_occlusion_ != perf_probe_occlusion_skipped_) {
			if (frame_skip_occlusion_) {
				occlusion_->enter_probe_skip();
			} else {
				occlusion_->leave_probe_skip();
			}
		}
		perf_probe_occlusion_skipped_ = frame_skip_occlusion_;
	} else if (frame_probe_enabled_) {
		perf_probe_occlusion_skipped_ = false;
	}
}

void GameWorld::sample_panm_clock() {
	panm_clock_->sample_frame();
	MissionRoot *runtime = get_runtime();
	if (runtime != nullptr) {
		runtime->set_presentation_time_ms(panm_clock_->get_time_ms());
	}
}

void GameWorld::finish_device_frame() {
	perf_tick_us_ = now_us() - device_frame_start_us_;
	if (frame_stats_on_) {
		frame_stats_->add(FrameStats::WORLD_FOLIAGE, perf_foliage_us_);
		frame_stats_->add(FrameStats::WORLD_AUDIO, perf_audio_us_);
	}
	sample_water_render_stats(frame_stats_on_);
	sample_auxiliary_render_stats(frame_stats_on_);
}

// --- the leg bodies -----------------------------------------------------------------

void GameWorld::render_terrain_frame() {
	// The engine compiles the terrain patch draw list for this frame's camera
	// and Terrain applies it (ADR 0033 R2). Runs before the foliage leg, whose
	// dispatcher consumes the draw list's fresh detail-cell handoff -- the old
	// self-driven _process walk left foliage reading a stale cell list. The
	// frame places the local view first, and Terrain samples that live
	// viewport camera inside render_frame (D-RORD-8).
	if (world_ready_ && terrain_ != nullptr) {
		terrain_->render_frame();
		// MATCHTERRAIN consumes the same composed-page generation Terrain just
		// published. Refreshing here preserves retail's terrain-before-entity
		// order and prevents a moving crouched body from sampling a stale
		// page.
		ObjectModel::refresh_match_terrain_frame(terrain_);
	}
}

void GameWorld::render_foliage_frame() {
	const int64_t foliage_start = now_us();
	perf_foliage_us_ = 0;
	if (world_ready_ && dispatcher_ != nullptr) {
		// The silhouette tier is the hide-in-grass mechanic: retail's
		// sector-entity walk generates model foliage only around
		// CROUCHED/PRONE infantry standing on terrain -- never around placed
		// objects, whose MoveOrder stays 0
		// [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7dc2/0x5c7ded
		// (MoveOrder & 0x300), groundEntity gate @ 0x5c7dd5..0x5c7df7].
		PackedVector3Array silhouette_anchors;
		MissionRoot *runtime = get_runtime();
		if (runtime != nullptr) {
			Ref<Simulation> anchor_sim = runtime->get_sim();
			if (anchor_sim.is_valid()) {
				silhouette_anchors = anchor_sim->get_foliage_mask_anchor_positions();
			}
		}
		dispatcher_->set_silhouette_anchors(silhouette_anchors);
		dispatcher_->render_frame(render_camera_xform());
		perf_foliage_us_ = now_us() - foliage_start;
	}
}

bool GameWorld::drive_network_frame() {
	MissionRoot *runtime = get_runtime();
	if (!(world_ready_ && runtime != nullptr && runtime->is_playing())) {
		return true;
	}
	if (join_wire_assets_pending_ && !apply_join_wire_til_if_ready()) {
		report_join_wire_asset_failure(
				"join: host sent an incomplete or invalid S2C 0x45 terrain stream");
		return false;
	}
	// Net-session edges (admission/deploy/loss) + the gate's occupancy report.
	drive_.observe_tick(runtime);
	return get_runtime() != nullptr;
}

// The precipitation presenter leg: the kernel re-floors the drop pool for
// this frame's camera and the renderer compiles the streaks (retail
// render_weather_trail_particles @ 0x5dee10 -- after the camera-side particle
// pass, before the foliage billboards).
void GameWorld::render_precipitation_frame() {
	MissionRoot *runtime = get_runtime();
	if (world_ready_ && runtime != nullptr && precipitation_ != nullptr) {
		if (env_ != nullptr && env_->is_raining()) {
			Viewport *viewport = get_viewport();
			Ref<Simulation> sim = get_sim();
			precipitation_->render_frame(sim.ptr(),
					viewport != nullptr ? viewport->get_camera_3d() : nullptr);
		} else {
			// Below the rain gate the drawer never runs (retail returns at
			// 0x5dee48 before touching the device).
			precipitation_->hide_frame();
		}
	}
}

void GameWorld::apply_blink_frame() {
	if (world_ready_) {
		occlusion_->apply_blink_gates(mission_forces_indoors_);
	}
}

// The local-player VIEW placement, as a device leg: the camera/viewmodel
// move from the state THIS frame's session tick produced, BEFORE
// occlusion/iris/particles read the camera (D-RORD-8) [orig: the render
// frame builds its view from the current player state before
// collect+submit, Render_ProcessMainSceneFrame @ 0x5ca0f0]. A world without
// a presenter (tests, dedicated) skips it; main_game covers the frames that
// never reach this leg.
void GameWorld::present_local_view_frame() {
	LocalPlayerPresenter *presenter = local_view_presenter();
	if (presenter != nullptr) {
		presenter->after_world_tick();
	}
}

// Compile the typed focused-Q3 snapshot after the camera and every live
// celestial/water/object producer has published this frame's final state.
// The immutable draw list is consumed by the terminal compositor against
// resolved beauty depth; there is no shared-world auxiliary camera or Q3
// viewport.
void GameWorld::sync_framefx_frame() {
	if (framefx_ != nullptr) {
		framefx_->advance_frame();
	}
}

// The environment presenters' per-frame advance (ex-self-clocked _process
// bodies): weather smoothing toward the fixed-tick targets, the sun's
// direction law, the sky dome and the celestial bodies following the render
// eye. Ordered after the scene-environment classify so the lit consumers
// below (terrain, foliage, objects) read this frame's pushed globals.
void GameWorld::render_environment_nodes_frame() {
	if (weather_ != nullptr) {
		weather_->advance_frame(frame_delta_);
	}
	if (sun_shadow_ != nullptr) {
		sun_shadow_->advance_frame(frame_delta_);
	}
	if (sky_dome_ != nullptr) {
		sky_dome_->advance_frame(frame_delta_);
	}
	if (celestial_ != nullptr) {
		celestial_->advance_frame(frame_delta_);
	}
}

// The water strip march + mirror camera for this frame's render eye
// (ex-self-clocked Water._process), under retail's per-frame water-active
// test: the terrain leg just tracked the visible terrain bounds, and the
// occlusion frame's Blink water verdict is last frame's, as retail reads it
// [orig: terrain_setup_view_and_lighting @ 0x60fe40].
void GameWorld::render_water_frame() {
	if (water_ == nullptr) {
		return;
	}
	if (terrain_ != nullptr) {
		water_->set_visible_terrain_bounds(terrain_->has_visible_terrain_bounds(),
				terrain_->get_visible_terrain_min_height(), terrain_->get_visible_terrain_max_height());
	}
	Ref<Simulation> sim = get_sim();
	water_->set_blink_water_visible(sim.is_valid() && sim->occlusion_water_visible());
	water_->advance_frame(frame_delta_);
}

// Select the main scene's per-pass fog after the local player has placed the
// camera, and before every rendered consumer submits. Camera offsets are
// part of the render transform, so a v_offset-only waterline crossing must
// flip the same state as Water/Terrain. [orig: Environment_ApplyFogAndAmbient
// @ 0x57e440]
void GameWorld::apply_scene_environment_frame() {
	if (env_ == nullptr) {
		return;
	}
	// The device leg only samples: the strict-vs-inclusive waterline
	// comparison semantics live in the engine behind apply_render_eye.
	bool water_active = is_water_render_active() && is_inside_tree();
	float eye_y = 0.0f;
	if (water_active) {
		Camera3D *cam = render_camera();
		if (cam == nullptr) {
			water_active = false;
		} else {
			eye_y = cam->get_camera_transform().origin.y;
		}
	}
	env_->apply_render_eye(eye_y, water_active ? water_->get_water_height() : 0.0f, water_active);
	// Publish the same adjusted render eye to the per-strip Q1/Q2 classifier.
	// This frame leg runs after camera placement and before ObjectModel's
	// retained material walk, so water crossings flip the ladder immediately.
	ObjectShaderCache *shader_cache = ObjectShaderCache::get_singleton();
	if (water_active) {
		const float water_height = water_->get_water_height();
		shader_cache->set_water_plane(water_height, eye_y >= water_height);
	} else {
		shader_cache->clear_water_plane();
	}
}

// The live camera the imminent render uses (null for a headless world or a
// viewport without a current camera).
Camera3D *GameWorld::render_camera() const {
	return RenderView::camera(const_cast<GameWorld *>(this));
}

// The view the imminent render uses: the live camera AFTER the local-view
// leg placed it; the frame-entry stash only when no camera exists (headless
// worlds/tests) (D-RORD-8).
Transform3D GameWorld::render_camera_xform() const {
	Camera3D *cam = render_camera();
	if (cam != nullptr) {
		return cam->get_global_transform();
	}
	return frame_camera_xform_;
}

void GameWorld::apply_occlusion_frame() {
	// The render-occlusion frame is camera-driven: it runs every render frame
	// (retail collects visible entities per scene render, not per sim tick),
	// and consumes the RENDER camera the local-view leg just placed
	// (D-RORD-8).
	// [orig: Terrain_CollectVisibleEntities @ 0x5c9160 from
	// Terrain_RenderSceneWithReflection @ 0x5c94f0]
	if (world_ready_ && !frame_skip_occlusion_) {
		occlusion_->apply_frame(render_camera(), render_camera_xform(), mission_forces_indoors_);
	}
}

void GameWorld::sample_iris_frame() {
	if (world_ready_) {
		// The iris re-target reads the render view too (D-RORD-8) [orig:
		// retail re-targets from the local player's view every render pass].
		stamp_iris_samples(render_camera_xform());
	}
}

// The sun-veil weather feed [orig: Environment_ApplySunVeilAndExposureStopdown
// @ 0x5ad8b0 runs once per main scene frame]: Celestial computed this frame's
// veil pair in its own advance (and pushed the white-quad alpha global);
// forward the exposure stop-down half to modulator-2's witnessed writer so
// the next weather ticks chase it. Zero-safe with either node absent.
void GameWorld::render_sun_veil_frame() {
	if (!world_ready_) {
		return;
	}
	Weather *veil_weather = get_weather_node();
	if (veil_weather == nullptr || celestial_ == nullptr) {
		return;
	}
	veil_weather->set_sun_veil_stopdown(celestial_->get_sun_veil_stopdown());
}

// The render-slot ground-shadow plan for this camera (after the material
// frame: render_light_frame pushed this frame's LightScene and light context
// into the device, the material frame may have rebuilt the model subtrees
// the capture channels are stamped on, and slot priority plus the capture
// poses are camera-relative) [orig: render_shadow_pass @ 0x5d7b70 once per
// main scene frame].
void GameWorld::render_slot_shadow_frame() {
	if (slot_shadow_ != nullptr) {
		slot_shadow_->advance_frame();
	}
}

// Retail refreshes TexCubeEnvironment during the offscreen preparation leg:
// all six 256-square faces together initially/when forced and every 128
// render frames, centered on the simulation player and clamped above
// terrain. [orig: update_environment_cubemap @ 0x6106a0].
void GameWorld::render_environment_cube_frame() {
	if (!world_ready_ || environment_cube_ == nullptr) {
		return;
	}
	Vector3 capture_position = render_camera_xform().origin;
	Ref<Simulation> capture_sim = get_sim();
	if (capture_sim.is_valid()) {
		capture_position = capture_sim->get_local_player_position();
	}
	environment_cube_->advance_frame(capture_position);
}

void GameWorld::mix_audio_frame(int p_ticks_run) {
	const int64_t audio_start = now_us();
	MissionAudio *audio = get_mission_audio();
	if (world_ready_ && audio != nullptr) {
		// Ambient soundloop regions read that same clock [orig:
		// Entity_CalcTimeOfDayRegion @ 0x408110].
		if (env_ != nullptr) {
			audio->set_time_of_day_hhmm(env_->get_time_of_day());
		}
		// Marker eval/registration rides the sim's logic-tick clock -- the
		// witnessed pool-2 stagger [orig: Entity_UpdateAllEntities @ 0x4c225a];
		// the per-frame call below is only the live-slot mix + voice binds
		// [orig: SoundEmitter_UpdateAndMixTop8 @ 0x521341]. A world with no
		// ticking runtime (editor idle) free-runs the eval clock off render
		// delta instead.
		MissionRoot *runtime = get_runtime();
		if (p_ticks_run > 0 && runtime != nullptr) {
			Ref<Simulation> audio_sim = runtime->get_sim();
			if (audio_sim.is_valid()) {
				audio->advance_ticks(audio_sim->get_logic_tick());
				// The weather tick's thunder one-shots, placed around the
				// listener (godot/src/audio/mission_audio.cpp carries the
				// cites).
				std::vector<opennova::world::WeatherSoundEvent> thunder;
				audio_sim->drain_weather_sounds(thunder);
				audio->play_weather_sounds(thunder, frame_camera_xform_);
                std::vector<opennova::world::ScriptSoundEvent> script_sounds;
                audio_sim->drain_script_sounds(script_sounds);
                audio->play_script_sounds(script_sounds, frame_camera_xform_);
			}
		}
		audio->tick(frame_camera_pos_, frame_delta_);
		music_var_pump();
		perf_audio_us_ = now_us() - audio_start;
	}
}

void GameWorld::render_material_frame() {
	// The per-model runtime advance (PANM registers, dynamic materials,
	// part/body anim, staggered env restamp) -- the ex-self-clocked
	// ObjectModel _process, now one static driver over the shared awake set
	// at a defined ladder slot (after occlusion resolves visibility, before
	// the particle composite) [orig: Terrain_RenderSectorModels @ 0x5c5d30
	// computes model runtime constants during the render sector walk].
	Camera3D *camera = render_camera();
	Viewport *viewport = camera != nullptr ? camera->get_viewport() : nullptr;
	if (camera != nullptr) {
		const Vector2 viewport_size = viewport->get_visible_rect().size;
		ObjectModel::update_authored_lods(camera->get_global_transform(), camera->get_fov(),
				viewport_size.x, viewport_size.y);
		// The retained static instances select their RLOD per entity from the
		// same camera frame (the placer rewrites only the slots that crossed).
		if (placer_.is_valid()) {
			placer_->update_static_lods(camera->get_global_transform(), camera->get_fov(),
					viewport_size.x, viewport_size.y);
		}
	}
	if (!frame_stats_on_) {
		ObjectModel::advance_awake_frame(frame_delta_);
		return;
	}
	const PackedInt64Array profile = ObjectModel::profile_awake_frame(frame_delta_);
	if (profile.size() < ObjectModel::AWAKE_PROFILE_SLOT_COUNT) {
		return;
	}
	frame_stats_->add(FrameStats::MODEL_CLOCK_ANIMATION,
			profile[ObjectModel::AWAKE_PROFILE_CLOCK_ANIMATION_US]);
	frame_stats_->add(FrameStats::MODEL_PANM, profile[ObjectModel::AWAKE_PROFILE_PANM_US]);
	frame_stats_->add(FrameStats::MODEL_MATERIAL, profile[ObjectModel::AWAKE_PROFILE_MATERIAL_US]);
	frame_stats_->add(FrameStats::MODEL_ORDER_BOUNDS,
			profile[ObjectModel::AWAKE_PROFILE_ORDER_BOUNDS_US]);
	frame_stats_->add(FrameStats::MODEL_AWAKE_MODELS,
			profile[ObjectModel::AWAKE_PROFILE_AWAKE_MODELS]);
	frame_stats_->add(FrameStats::MODEL_RENDERABLE_MODELS,
			profile[ObjectModel::AWAKE_PROFILE_RENDERABLE_MODELS]);
}

void GameWorld::render_particle_frame() {
	EffectWorld *effect_world = get_effect_world();
	if (effect_world != nullptr) {
		effect_world->render_frame();
	}
}

// The EffectWorld point-light device leg: per visible model, select the
// witnessed <= 4 pool lights for that draw context and write them as
// per-instance shader parameters (godot/src/lights/effect_light_director
// carries the seam notes). The viewmodel parts ride along with the local
// player as owner so first-person self-lights gate correctly.
void GameWorld::render_light_frame() {
	if (light_director_.is_null() || !is_inside_tree()) {
		return;
	}
	TypedArray<ObjectModel> viewmodel_parts;
	LocalPlayerPresenter *presenter = local_view_presenter();
	if (presenter != nullptr) {
		viewmodel_parts = presenter->vm_parts();
	}
	int viewmodel_owner = -1;
	Ref<Simulation> sim = get_sim();
	if (sim.is_valid() && sim->has_local_player()) {
		viewmodel_owner = sim->get_local_player_wire_handle();
	}
	// The render view (the live viewport camera; the editor camera under a
	// preview RenderView), like every other camera-driven leg (D-RORD-8).
	light_director_->render_frame(render_camera(), viewmodel_parts, viewmodel_owner, frame_stats_on_);
	// The terrain leg of the same pool: the next terrain frame re-draws its
	// patches with the pool lights they overlap.
	render_terrain_light_leg();
	// Feed the render-slot shadow device the same point-light context (its
	// per-slot dominant-light pick reads the shared pool) plus the local
	// player state for the retail priority/drape gates.
	if (slot_shadow_ != nullptr) {
		slot_shadow_->set_light_scene(light_director_->scene());
		slot_shadow_->set_light_context(light_director_->light_gain(),
				static_cast<int>(Time::get_singleton()->get_ticks_msec()), weather_);
		if (resource_root_.is_valid()) {
			slot_shadow_->set_resource_root(resource_root_);
		}
		if (presenter != nullptr) {
			slot_shadow_->set_local_player_model(presenter->avatar());
			slot_shadow_->set_local_player_first_person(!presenter->is_third_person());
		}
		if (sim.is_valid()) {
			slot_shadow_->set_local_player_prone(
					sim->get_local_player_stance_latch() == Simulation::STANCE_PRONE);
		}
	}
}

// The terrain leg of the EffectWorld light pool (the former
// terrain_light_leg.gd): hand the terrain node the shared pool and the frame
// time once per light frame, so its next render frame re-draws every patch
// with the pool lights that patch overlaps.
//
// Retail re-draws each terrain batch once more per passing pool light,
// additively, through the two-stage projected-texture pass
// [orig: render_terrain_sector_batch @0x6092A0, the per-light else-arm
// @0x609870..0x6098BC -> Light_SetupTerrainProjectedPass @0x5AA830]. The
// collect (<= 16 per batch, both light groups cleared, the authored
// terrain-disable flag), the projection contract and the pixel constants are
// portable in renderer::LightScene::collect_terrain_pass_rows; the Terrain
// device packs the rows into its per-slot texture and the terrain shader sums
// the two MODULATE2X stages (docs/render/render-lighting-re.md).
//
// Device fold: the table runs render_light_frame AFTER this frame's terrain
// draw, so the pool state a patch re-draws with is one frame old at 62 Hz.
// The patch <-> slot match is exact -- the terrain collects against its own
// draw list -- and the table order stays as witnessed (D-RORD). A null
// director (no mission) retires the leg; the terrain clears its rows
// texture.
void GameWorld::render_terrain_light_leg() {
	if (terrain_ == nullptr) {
		return;
	}
	Ref<LightScene> scene = light_director_.is_valid() ? light_director_->scene() : Ref<LightScene>();
	terrain_->set_light_context(scene, static_cast<int>(Time::get_singleton()->get_ticks_msec()));
}

void GameWorld::update_clear_frame() {
	if (!world_ready_ || !is_visible_in_tree()) {
		restore_idle_frame_clear_color();
	} else {
		update_frame_clear_color();
	}
}

// The two compositor passes the root-viewport rows cannot split out: the
// focused Q3 draw list and the slot-shadow captures. Both report typed
// per-frame counts (the compile of this frame, the draw of the previous
// one). Focused Q3 and the slot-shadow captures both draw inside the root
// compositor (POST_TRANSPARENT and PRE_OPAQUE); their per-pass counts come
// off the effects' typed reports. Their GPU spans are carved back out of the
// root rows by RD timestamps the effects capture only while the Stats tab
// does -- capture_timestamp barriers the RD graph, so the toggle rides
// stats_on (re-applied every captured frame so a late-built effect still
// hears it, switched off on the capture's falling edge).
void GameWorld::sample_auxiliary_render_stats(bool p_stats_on) {
	if (!p_stats_on) {
		if (stats_aux_gpu_timing_) {
			stats_aux_gpu_timing_ = false;
			if (framefx_ != nullptr) {
				framefx_->set_gpu_timing_enabled(false);
			}
			if (slot_shadow_ != nullptr) {
				slot_shadow_->set_gpu_timing_enabled(false);
			}
		}
		return;
	}
	stats_aux_gpu_timing_ = true;
	if (framefx_ != nullptr) {
		framefx_->set_gpu_timing_enabled(true);
		const Dictionary q3_report = framefx_->get_backend_report();
		frame_stats_->add(FrameStats::RENDER_Q3_OBJECTS, int64_t(q3_report.get("q3_drawn_commands", 0)));
		frame_stats_->add(FrameStats::RENDER_Q3_DRAWS, int64_t(q3_report.get("q3_gpu_draw_calls", 0)));
		if (bool(q3_report.get("q3_gpu_valid", false))) {
			frame_stats_->add(FrameStats::RENDER_Q3_GPU, int64_t(q3_report.get("q3_gpu_us", 0)));
		}
	}
	if (slot_shadow_ != nullptr) {
		slot_shadow_->set_gpu_timing_enabled(true);
		const Dictionary slot_report = slot_shadow_->get_report();
		frame_stats_->add(FrameStats::RENDER_SLOT_OBJECTS,
				int64_t(slot_report.get("slot_surfaces_compiled", 0)));
		frame_stats_->add(FrameStats::RENDER_SLOT_DRAWS, int64_t(slot_report.get("slot_draw_calls", 0)));
		frame_stats_->add(FrameStats::RENDER_SLOT_CAPTURES,
				int64_t(slot_report.get("slot_captures_drawn", 0)));
		frame_stats_->add(FrameStats::RENDER_SLOT_PACKED_VERTICES,
				int64_t(slot_report.get("slot_packed_vertices", 0)));
		frame_stats_->add(FrameStats::RENDER_SLOT_SKINNED,
				int64_t(slot_report.get("slot_skinned_commands", 0)));
		if (bool(slot_report.get("slot_gpu_valid", false))) {
			frame_stats_->add(FrameStats::RENDER_SLOT_GPU, int64_t(slot_report.get("slot_gpu_us", 0)));
		}
	}
}

bool GameWorld::is_water_render_stats_measured() const {
	return stats_water_vp_id_.is_valid() && ObjectDB::get_instance(stats_water_vp_id_) != nullptr;
}

// Water-reflection RTT sampling for the Stats tab: flip measured render time
// on the reflection SubViewport only while the tab captures, then land the
// previous frame's CPU/GPU times on the board. Identity-latched so a freed
// viewport never sees a stale-RID RenderingServer call.
void GameWorld::sample_water_render_stats(bool p_stats_on) {
	SubViewport *viewport = nullptr;
	if (p_stats_on && water_ != nullptr) {
		viewport = water_->get_reflection_viewport();
	}
	SubViewport *previous = Object::cast_to<SubViewport>(ObjectDB::get_instance(stats_water_vp_id_));
	RenderingServer *rs = RenderingServer::get_singleton();
	if (previous != viewport) {
		if (previous != nullptr) {
			rs->viewport_set_measure_render_time(previous->get_viewport_rid(), false);
		}
		stats_water_vp_id_ = viewport != nullptr ? ObjectID(viewport->get_instance_id()) : ObjectID();
		if (viewport != nullptr) {
			rs->viewport_set_measure_render_time(viewport->get_viewport_rid(), true);
		}
	}
	if (viewport == nullptr) {
		return;
	}
	const RID rid = viewport->get_viewport_rid();
	frame_stats_->add(FrameStats::RENDER_WATER_CPU,
			static_cast<int64_t>(rs->viewport_get_measured_render_time_cpu(rid) * 1000.0));
	frame_stats_->add(FrameStats::RENDER_WATER_GPU,
			static_cast<int64_t>(rs->viewport_get_measured_render_time_gpu(rid) * 1000.0));
	// What the mirror pass actually re-rendered (previous frame): the
	// witnessed reflection re-renders the world scene [orig:
	// Water_ReflectionPrerender @ 0x5c2780], so its submission count is a
	// first-class stats row.
	frame_stats_->add(FrameStats::RENDER_WATER_OBJECTS,
			rs->viewport_get_render_info(rid, RenderingServer::VIEWPORT_RENDER_INFO_TYPE_VISIBLE,
					RenderingServer::VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME));
	frame_stats_->add(FrameStats::RENDER_WATER_DRAWS,
			rs->viewport_get_render_info(rid, RenderingServer::VIEWPORT_RENDER_INFO_TYPE_VISIBLE,
					RenderingServer::VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME));
}

void GameWorld::stop_water_render_stats() {
	SubViewport *previous = Object::cast_to<SubViewport>(ObjectDB::get_instance(stats_water_vp_id_));
	if (previous != nullptr) {
		RenderingServer::get_singleton()->viewport_set_measure_render_time(
				previous->get_viewport_rid(), false);
	}
	stats_water_vp_id_ = ObjectID();
}

// The marched iris-exposure feed (D-RLIT-2): three camera-ray samples from
// the sim each render frame, consumed by Weather's exposure re-target on its
// next tick [orig: Environment_ApplyFogAndAmbient @ 0x57e512 ->
// compute_ambient_light_along_direction @ 0x5c7a00 -- retail re-targets from
// the local player's view every render pass]. The render-occlusion frame it
// used to share a section with (blink letter gates + the section-mask/portal
// apply) is OcclusionFrame (godot/src/world/occlusion_frame.cpp); the iris
// march stays here as the weather feed.
void GameWorld::stamp_iris_samples(const Transform3D &p_camera_xform) {
	Weather *weather = weather_;
	Ref<Simulation> sim = get_sim();
	if (weather == nullptr || sim.is_null()) {
		return;
	}
	Vector3 light_dir(0, 1, 0);
	if (env_ != nullptr) {
		light_dir = env_->get_light_direction();
	}
	weather->set_iris_samples(sim->compute_iris_samples(p_camera_xform.origin,
			-p_camera_xform.basis.get_column(2), light_dir));
}

// --- Frame clear color (env divergence #21, closed) ----------------------------

void GameWorld::restore_idle_frame_clear_color() {
	clear_env_generation_ = -1;
	const Ref<Environment> environment = frame_clear_environment();
	if (environment.is_valid()) environment->set_bg_color(idle_frame_clear_color_);
}

// The witnessed frame clear: the horizon-blended skyfog above water, the lit
// water color underwater [orig: Render_ProcessMainSceneFrame @ 0x5ca776..
// 0x5ca792 - clear color = alternate_fog ? 0x808080 : cam above water ?
// skyfog[0] : Env_WaterColorLit; the vehicle alternate-fog view is not
// modeled yet]. Both branches serve RENDER-SPACE (x2-gained) colors, consumed
// VERBATIM by the modulate2x-path Clear this renderer reproduces (D-RMAT-7):
// above water the post-blend DOUBLED skyfog, underwater Env_WaterColorLit =
// water x light >> 7; the halving branch [orig: @ 0x67715d] is the
// non-modulate2x fallback with no Godot analog. The ClearColor Environment
// must stay BG_COLOR with ambient disabled - BG_SKY with no sky renders black
// and swallows these writes (GUT-pinned).
void GameWorld::update_frame_clear_color() {
	Ref<Environment> environment = frame_clear_environment();
	if (environment.is_null() || env_ == nullptr) return;
	// The clear SELECTION (black indoors / skyfog above water / lit water
	// underwater) is the engine's (environment_state.h carries the witness);
	// this device classifies the eye and writes the color. The sentinel
	// generation (-2) forces a recompute on indoors exit.
	if (occlusion_->is_blink_indoors()) {
		if (clear_env_generation_ != -2) {
			clear_env_generation_ = -2;
			environment->set_bg_color(env_->frame_clear_color_for(true, true));
		}
		return;
	}
	const bool above = !env_->is_underwater_view();
	Ref<EnvLightState> light_state = env_->get_light_state();
	const int64_t gen = light_state.is_valid() ? light_state->get_generation() : 0;
	if (gen == clear_env_generation_ && above == clear_above_water_) {
		return;
	}
	clear_env_generation_ = gen;
	clear_above_water_ = above;
	environment->set_bg_color(env_->frame_clear_color_for(false, above));
}

// Re-drive the gamemus vars from the local player each frame, the way the
// original does from the local player's body update [orig:
// Entity_UpdateInfantryPlayerBody @ 0x4b40e0, gate entity ==
// g_local_player_entity @ 0x4b6234; full map docs/audio/mus-sbf-re.md §Game
// music driving]. Pumped here: Var7 = health % (cur*100/max, 100 when max <=
// cur [orig: @ 0x4b6315-0x4b6324]) and Var10 = team [orig: @ 0x4b62fc].
// Witnessed-but-unpumped seams (the shipped gamemus reads none of them --
// docs/audio/mus-sbf-re.md (D-MUS-VARPUMP)): Var2 view pitch (the original
// writes raw engine angle units, unwitnessed conversion), Var5/Var6 threat
// distance / threat-targets-me (Entity_FindNearestThreat @ 0x4b0990
// unported), Var3/Var4 (low-confidence), Var8 game type (retail scoring-mode
// ids not yet mapped to our sessions). The var writes cross to the shell's
// MusicService through the music_var_changed signal.
void GameWorld::music_var_pump() {
	MissionRoot *runtime = get_runtime();
	if (runtime == nullptr || !runtime->has_player()) {
		return;
	}
	Ref<Simulation> pump_sim = runtime->get_sim();
	if (pump_sim.is_null()) {
		return;
	}
	emit_signal("music_var_changed", static_cast<int>(MusicDirector::GAME_VAR_HEALTH_PCT),
			pump_sim->get_local_player_health_percent());
	emit_signal("music_var_changed", static_cast<int>(MusicDirector::GAME_VAR_TEAM),
			runtime->local_player_team());
}

// --- the perf counters --------------------------------------------------------------

Ref<RuntimePerfCounters> GameWorld::get_runtime_perf_counters() const {
	Ref<RuntimePerfCounters> out;
	out.instantiate();
	out->set_tick_us(perf_tick_us_);
	out->set_foliage_us(perf_foliage_us_);
	out->set_runtime_us(perf_runtime_us_);
	out->set_audio_us(perf_audio_us_);
	MissionRoot *runtime = get_runtime();
	if (runtime != nullptr) {
		out->set_runtime(runtime->get_perf_counters());
	}
	const Dictionary foliage_backend = dispatcher_ != nullptr ? dispatcher_->get_backend_report()
															  : Dictionary();
	if (dispatcher_ != nullptr) {
		out->set_foliage(dispatcher_->get_frame_stats());
	}
	out->set_foliage_backend(foliage_backend);
	if (framefx_ != nullptr) {
		out->set_framefx(framefx_->get_backend_report());
	}
	out->set_mission_placement(mission_stats_);
	out->set_static_live_populations(get_static_live_population_count());
	MissionAudio *audio = get_mission_audio();
	if (audio != nullptr) {
		out->set_audio(audio->get_perf_counters());
	}
	// The instance-uniform geometry estimate (runtime_perf_counters.h carries
	// the rule): the foliage draw pools, the placer's static populations
	// (visible batches plus their shadow twins), and every surface instance
	// of every live ObjectModel scene.
	const int64_t foliage_pool = int64_t(foliage_backend.get("pool_size", 0));
	int64_t static_populations = 0;
	if (mission_stats_.is_valid()) {
		static_populations = mission_stats_->get_batches() + mission_stats_->get_static_shadow_batches();
	}
	const int64_t object_geometry = ObjectModel::get_live_geometry_instance_count();
	const int64_t buffer_size = int64_t(ProjectSettings::get_singleton()->get_setting(
			"rendering/limits/global_shader_variables/buffer_size", 0));
	out->set_estimate_total(foliage_pool + static_populations + object_geometry);
	out->set_estimate_budget(buffer_size / RuntimePerfCounters::INSTANCE_UNIFORM_VALUES_PER_GEOMETRY);
	out->set_estimate_foliage_pool(foliage_pool);
	out->set_estimate_static_populations(static_populations);
	out->set_estimate_object_geometry(object_geometry);
	return out;
}
