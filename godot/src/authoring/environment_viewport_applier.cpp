#include "authoring/environment_viewport_applier.h"

#include <algorithm>

#include <editor/preview/environment_viewport.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/view/session_view.h>
#include <runtime/environment/environment_state.h>
#include <runtime/environment/weather_runtime.h>

#include "env/mission_environment.h"
#include "env/water.h"
#include "env/weather.h"

namespace godot {

namespace {

using opennova::editor::EnvironmentViewport;

const EnvironmentViewport &environment_of(const opennova::editor::ViewportModel &model) {
	return static_cast<const EnvironmentViewport &>(model);
}

} // namespace

EnvironmentViewportApplier::EnvironmentViewportApplier(SubViewport &viewport) :
		layers_(std::make_unique<PreviewSceneLayers>(viewport, "EnvironmentPreview", false)) {
	// Its camera is the 3D listener of its own world (S23 C: the Listen's rain loops pan about it,
	// as the game's camera).
	viewport.set_as_audio_listener_3d(true);
	listen_ = std::make_unique<PreviewSoundLoops>(&layers_->root());
}

EnvironmentViewportApplier::~EnvironmentViewportApplier() = default;

// --- the build -----------------------------------------------------------------------------------

void EnvironmentViewportApplier::rebuild(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::SessionView &view, const opennova::editor::PreviewClock &) {
	building_ = false;
	project_root_ = view.project.root;
	const EnvironmentViewport &model = environment_of(viewport);
	if (model.view_status() != opennova::editor::EnvironmentViewStatus::Ready || !view.findings.assets) {
		clear();
		return;
	}
	// What moved that neither layer read is the environment's (a model the sun read as it was first
	// drawn, the drops' texture): the bodies load again with it.
	layers_->mount(view, PreviewSceneLayers::kEnvironment);
	PreviewSceneLayers::Source source;
	source.header = model.header();
	source.mission = model.mission_name();
	// The file this viewport draws is the .env the drawn mission's terrain reads after its .trn
	// (D-TERRAIN-18).
	source.environment = model.file_name();
	source.terrain = model.options().terrain;
	layers_->plan(source);
	// The pose alone: made whole as it is taken.
	if (!layers_->building()) {
		apply_state_(model);
		return;
	}
	building_ = true;
}

ApplierStep EnvironmentViewportApplier::step(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::PreviewClock &, std::string &) {
	if (layers_->step())
		return ApplierStep::More;
	apply_state_(environment_of(viewport));
	building_ = false;
	return ApplierStep::Built;
}

opennova::editor::OperationProgress EnvironmentViewportApplier::progress() const {
	opennova::editor::OperationProgress progress;
	progress.unit = opennova::editor::OperationUnit::Steps;
	if (!building_)
		return progress;
	progress.done = layers_->units_done();
	progress.total = layers_->units_planned() + 1;
	progress.label = layers_->building() ? layers_->step_label() : "pose";
	return progress;
}

// --- the state -----------------------------------------------------------------------------------

void EnvironmentViewportApplier::update(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &) {
	apply_state_(environment_of(viewport));
}

void EnvironmentViewportApplier::apply_state_(const EnvironmentViewport &model) {
	const opennova::editor::EnvironmentViewportOptions &options = model.options();
	PreviewSceneLayers::Shown shown;
	shown.terrain = options.terrain;
	shown.foliage = options.terrain;
	shown.water = options.water;
	layers_->set_shown(shown);
	layers_->place_camera(model.camera());
}

void EnvironmentViewportApplier::apply_listen_(const EnvironmentViewport &model) {
	// Heard while the picture is drawn (held a moment past its last drawn frame, as the mission
	// device's Listen is).
	const opennova::editor::EnvironmentListen &listen = model.listen();
	listen_->listen(project_root_, listen.channels(), model.options().listen.on && listen.open() && !project_root_.empty(),
			presented_);
}

void EnvironmentViewportApplier::clear() {
	building_ = false;
	listen_->stop();
	layers_->clear();
	wanted_ = Wanted();
}

void EnvironmentViewportApplier::apply(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::PreviewClock &, opennova::editor::ViewportDeviceReport &report) {
	layers_->report(report);
	const EnvironmentViewport &model = environment_of(viewport);
	// The viewport's clock and weather, run at the next tick.
	const opennova::world::WeatherState &home = model.weather();
	wanted_.known = model.view_status() == opennova::editor::EnvironmentViewStatus::Ready;
	wanted_.ticks = model.weather_ticks();
	wanted_.clock_sets = model.clock_sets();
	wanted_.tod_fixed24 = home.tod_fixed24;
	wanted_.advance = home.tod_advance_per_tick;
	wanted_.precipitation_kind = home.precipitation_kind;
	wanted_.overcast_for_tod_q16 = home.overcast_for_tod_q16;
	wanted_.channels = home.core.scalar_channels;
	if (building_)
		return;
	apply_state_(model);
}

void EnvironmentViewportApplier::tick(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &clock) {
	clock_ms_ = int64_t(clock.ms());
	apply_listen_(environment_of(viewport));
	// No mirror pass unless this frame presents the picture.
	layers_->nodes().water->set_mirror_enabled(false);
	// The weather's state follows the viewport's every frame, drawn or not; what it publishes is
	// the presented frame's.
	if (layers_->environment_built())
		run_weather_();
}

void EnvironmentViewportApplier::run_weather_() {
	// The engine's state alone (the runtime, the environment's state): nothing here reaches the process-wide
	// globals, which the presented frame publishes.
	if (layers_->start_weather()) {
		ticks_seen_ = wanted_.ticks;
		resync_ = true;
	}
	opennova::env::WeatherRuntime &runtime = layers_->nodes().weather->runtime();
	opennova::env::EnvironmentState &state = layers_->nodes().environment->state();
	opennova::world::WeatherState &home = runtime.state();
	if (wanted_.known) {
		// The ticks the viewport's home ran since, then its clock and channels: its time, its rate,
		// the rain, the overcast, the fog.
		const uint64_t behind = wanted_.ticks > ticks_seen_ ? wanted_.ticks - ticks_seen_ : 0;
		layers_->run_weather(int(std::min<uint64_t>(
				behind, uint64_t(opennova::env::WeatherRuntime::kMaxCatchupTicks))));
		ticks_seen_ = std::max(ticks_seen_, wanted_.ticks);
		home.tod_fixed24 = wanted_.tod_fixed24;
		home.tod_advance_per_tick = wanted_.advance;
		home.core.scalar_channels = wanted_.channels;
		home.precipitation_kind = wanted_.precipitation_kind;
		home.overcast_for_tod_q16 = wanted_.overcast_for_tod_q16;
		home.compute_night_phase();
		state.sync_clock_from_weather();
	}
	// A clock set anew takes its colours at once; otherwise a zero-tick refresh of the clock's targets.
	if (resync_ || wanted_.clock_sets != clock_sets_seen_) {
		resync_ = false;
		clock_sets_seen_ = wanted_.clock_sets;
		runtime.resync_colors_now(&state);
	} else {
		runtime.tick_weather(&state, 0);
	}
}

// --- the frame -----------------------------------------------------------------------------------

void EnvironmentViewportApplier::present(double dt) {
	// Nothing to draw while the first build runs.
	if (!layers_->environment_built())
		return;
	++presented_;
	// In the game's leg order (game_world_frame.cpp).
	layers_->begin_frame();
	layers_->present_environment(dt);
	layers_->present_ground(dt, clock_ms_);
	layers_->present_sun_veil();
	layers_->present_particles(clock_ms_);
	layers_->present_drops();
	layers_->present_overlay(nullptr);
	layers_->present_frame_effects();
	layers_->present_clear_and_cube();
	layers_->end_frame();
}

} // namespace godot
