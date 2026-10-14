#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include <editor/preview/viewport_follow.h>
#include <formats/env/env_weather.h>

#include "authoring/preview_scene_layers.h"
#include "authoring/preview_sound_player.h"
#include "authoring/viewport_applier.h"

namespace opennova::editor {
class EnvironmentViewport;
} // namespace opennova::editor

namespace godot {

class Celestial;
class MissionEnvironment;
class Precipitation;
class Terrain;
class Water;
class Weather;

// An environment viewport's device work (ADR 0046 DI-19b): a mission's world as the game draws it
// with no simulation (PreviewSceneLayers over WorldScene's rows, S26), the sky the subject: the
// environment (the .env as the game would read it were it saved now, over the drawn mission's .trn
// and overcast.def, the mission header's overrides over it where the viewport says so), its weather
// (on the environment's own home, started as a mission's start starts it: seeded, then the 255-tick
// settle), the sky dome and its clouds, the sun, moon, glare and glint, the water, the terrain of
// the mission the viewport draws it over (with the mission's tiles), and the rain's drops (over the
// home's drop pool), all read through a ResourceRoot mounted over the project's files, and the
// camera where the viewport's camera is.
//
// Its clock and its weather are the viewport's (the portable half steps the game's weather home on
// the preview clock's game ticks, and takes the commands a script gives): every frame, drawn or
// not, the device runs on the engine's state alone the game's ticks that home ran since (the drops'
// fall, then the whole weather tick, colour legs and all), then takes the home's clock, its scalar
// channels (the rain and overcast springs, the fog, the sky height) and its precipitation kind, so
// what its colour legs, sky and drops draw is the viewport's time and weather; a frame it presents
// publishes them. A clock set anew (a scrub, a seek) takes the colours at once (the zero-tick
// refresh a paused transport's scrub takes).
//
// The frame's legs run in the game's order (the layers'). It builds over the Shell's frames in the
// layers' two layers, each rebuilt only where its key or a file it read moved. Its scene state is
// its own (E13).
class EnvironmentViewportApplier final : public ViewportApplier {
public:
	explicit EnvironmentViewportApplier(SubViewport &viewport);
	~EnvironmentViewportApplier() override;

	void rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &view,
			const opennova::editor::PreviewClock &clock) override;
	ApplierStep step(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			std::string &failure) override;
	bool building() const override { return building_; }
	opennova::editor::OperationProgress progress() const override;
	void update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void clear() override;
	void apply(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			opennova::editor::ViewportDeviceReport &report) override;
	void tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void resize(int, int) override {}
	bool ground_at(double x, double y, double &height) const override {
		return layers_->ground_at(x, y, height);
	}
	uint64_t scene_state() const override { return layers_->scene_state(); }
	void publish_scene_state() override { layers_->publish_scene_state(); }
	void present(double dt) override;

	// Its nodes, for the device tests.
	Camera3D *camera() const { return layers_->camera(); }
	MissionEnvironment *environment() const { return layers_->nodes().environment; }
	Weather *weather() const { return layers_->nodes().weather; }
	Celestial *celestial() const { return layers_->nodes().celestial; }
	Terrain *terrain() const { return layers_->nodes().terrain; }
	Water *water() const { return layers_->nodes().water; }
	Precipitation *precipitation() const { return layers_->nodes().precipitation; }
	// How many game ticks it ran, and how many builds of each layer.
	int64_t ticks_run() const { return layers_->ticks_run(); }
	int environment_builds() const { return layers_->environment_builds(); }
	int terrain_builds() const { return layers_->terrain_builds(); }

private:
	// What the viewport says its weather and clock are (taken at each apply, run at the next tick).
	struct Wanted {
		bool known = false;
		uint64_t ticks = 0; // the home's game ticks in all
		uint64_t clock_sets = 0;
		uint32_t tod_fixed24 = 0;
		uint32_t advance = 0;
		uint32_t precipitation_kind = 0;
		int32_t overcast_for_tod_q16 = 0;
		opennova::env::EnvScalarChannels channels;
	};

	void apply_state_(const opennova::editor::EnvironmentViewport &model);
	// The viewport's clock and weather into the home, the ticks it ran first.
	void run_weather_();
	// The Listen's rain loops (S23 C) as the viewport's mix binds them now.
	void apply_listen_(const opennova::editor::EnvironmentViewport &model);

	std::unique_ptr<PreviewSceneLayers> layers_;
	bool building_ = false; // the layers' units run, the pose after them
	// The Listen's rain loops (S23 C), the project's root their waves load from, and the frames
	// presented.
	std::unique_ptr<PreviewSoundLoops> listen_;
	std::string project_root_;
	uint64_t presented_ = 0;
	Wanted wanted_;
	uint64_t ticks_seen_ = 0; // the home's ticks this device has run
	uint64_t clock_sets_seen_ = 0;
	bool resync_ = true; // the colours taken at once at the next tick
	int64_t clock_ms_ = 0; // the preview clock's milliseconds, as the frame's tick read them
};

} // namespace godot
