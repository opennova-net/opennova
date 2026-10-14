#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <editor/preview/effect_schedule.h>
#include <runtime/particle/effect_scene.h>

namespace opennova::editor {

// How an effect's picture plays (ADR 0046 DI-14, the effect viewport's options): the mission wind the
// game feeds every emitter's GLOBALWIND drift (the BMS header's speed and direction, as
// particle::mission_wind_vector reads them [orig: Weather_SetMissionWind @ 0x5DE970]; 0 none).
struct EffectPlayOptions {
	int wind_speed = 0;
	int wind_direction = 0;
	bool operator==(const EffectPlayOptions &other) const {
		return wind_speed == other.wind_speed && wind_direction == other.wind_direction;
	}
	bool operator!=(const EffectPlayOptions &other) const { return !(*this == other); }
};

// The spawn pose of an effect played alone: at the origin with no orientation, which the game's
// descriptor spawn hands its group as a zero vector, so every EMITVECTOR member emits around world +Y
// (a terrain or water impact's case [orig: CEffectWorld_SpawnEmitterAtPosition @ 0x5F6E52..0x5F6E5C ->
// CEffectEmitter_SetOrientationFromDirection @ 0x5E5D51]; particle::descriptor_pose at the origin). The
// other spawns' poses are the engine's own (runtime/particle/effect_scene.h): particle::forward_pose, an
// attached spawn's, and particle::descriptor_pose, a descriptor's, each in the preview's space.
particle::EffectPose effect_play_pose();

// One effect played on the preview clock through the engine's own effect scene (ADR 0046 DI-14): the
// scene opened over the effect's closure (particle::effect_closure), the effect spawned at tick 0 of
// the clock as the game spawns it (effect_play_pose), once, as the game spawns an effect once per event,
// and played on the preview clock as every editor effect scene is (EffectSchedule: a game tick at a time
// through the view's planes; a jump of the clock spawns it again pre-aged by its age at the tick sought,
// bounded by kEffectInitialAgeTickLimit: past that it shows the age the bound allows), so the picture at a
// tick is the spawn's at that age. The scene is the device's picture: it reads the scene's snapshot and
// draws it (godot/src/authoring/preview_effects).
class EffectPlayback {
public:
	// The scene opened over `config` with `effect` to spawn (its name; a name the catalog lacks spawns
	// what the engine's intern makes of it). The spawn's age carries over (an edit shows the effect at the
	// age it had), spawned pre-aged at the next play.
	void open(const particle::EffectSceneConfig &config, const std::string &effect);
	// No scene: nothing plays.
	void close();

	// The scene played to the clock's tick `tick`, `frustum` the view's (preview_view_frustum; none: every emitter
	// advances).
	void play_to(int32_t tick, const EffectPlayOptions &options, const particle::ParticleViewFrustum &frustum = {});

	// The scene the picture draws (null: none open). Shared with the device, which only reads it.
	const std::shared_ptr<particle::EffectScene> &scene() const { return scene_; }
	const std::string &effect() const { return effect_; }
	// The spawn now playing (none: the effect died, or spawns nothing) and what the last spawn came to.
	bool alive() const;
	particle::EffectSpawnStatus last_status() const { return last_status_; }
	// The spawn's age at the tick played (it spawns once, on tick 0), the ticks it was pre-aged by,
	// and how many spawns were made since the scene opened.
	int32_t age() const { return schedule_.tick(); }
	uint32_t pre_aged() const { return pre_aged_; }
	uint64_t spawns() const { return spawns_; }
	int32_t tick() const { return schedule_.tick(); }
	bool played() const { return schedule_.played(); }
	// Moves whenever the scene is opened or advanced: the snapshot a device drew is stale.
	uint64_t serial() const { return serial_; }

private:
	// The one spawn as it stands at the clock's tick `tick`: pre-aged by its age then (bounded),
	// spawned on tick 0.
	void spawn_(int32_t tick);

	std::shared_ptr<particle::EffectScene> scene_;
	std::string effect_;
	particle::EffectHandle handle_;
	particle::EffectGroupId group_;
	particle::EffectSpawnStatus last_status_ = particle::EffectSpawnStatus::InvalidHandle;
	EffectSchedule schedule_;
	EffectTimedSpawns timed_; // none: the one spawn stands, made again at a jump
	uint32_t pre_aged_ = 0;
	uint64_t spawns_ = 0;
	uint64_t serial_ = 0;
};

} // namespace opennova::editor
