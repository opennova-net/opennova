#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <runtime/particle/effect_scene.h>

namespace opennova::editor {

// How an effect's picture plays (ADR 0046 DI-14, the effect viewport's options): spawned again as it
// dies (the editor's aid: the game spawns an effect once per event), and the mission wind the game
// feeds every emitter's GLOBALWIND drift (the BMS header's speed and direction, as
// particle::mission_wind_vector reads them [orig: Weather_SetMissionWind @ 0x5DE970]; 0 none).
struct EffectPlayOptions {
	bool loop = true;
	int wind_speed = 0;
	int wind_direction = 0;
	bool operator==(const EffectPlayOptions &other) const {
		return loop == other.loop && wind_speed == other.wind_speed && wind_direction == other.wind_direction;
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

// A spawn's status as the game's receipt names it on the wire (godot/src/particle/effect_scene's
// spawn_status_name: "spawned", "suppressed", "invalid_handle", ...), the one table the previews' bodies write; a
// spawn refused while the scene's effects are off is "disabled" (particle::EffectSpawnStatus::Disabled, which the
// receipt's table lacks).
const char *effect_spawn_status_token(particle::EffectSpawnStatus status);

// One effect played on the preview clock through the engine's own effect scene (ADR 0046 DI-14): the
// scene opened over the effect's closure (particle::effect_closure), the effect spawned at tick 0 of
// the clock as the game spawns it (effect_play_pose), stepped a game tick at a time as the clock runs
// (each tick the scene's fixed step, so what shows at a tick is the same however the frames fell), and
// spawned again at the tick it dies while it loops. A jump of the clock (a seek, a step back, more ticks
// at once than one advance takes) spawns it again pre-aged by its age at the tick sought, as the
// engine's catch-up spawn ages one (EffectSpawnRequest::initial_age_ticks, bounded by
// kEffectInitialAgeTickLimit: past that it shows the age the bound allows); an effect so old it died
// within its catch-up is spawned anew there while it loops. The scene is the device's picture: it reads
// the scene's snapshot and draws it (godot/src/authoring/preview_effects).
class EffectPlayback {
public:
	// The scene opened over `config` with `effect` to spawn (its name; a name the catalog lacks spawns
	// what the engine's intern makes of it). The playing cycle's age carries over (an edit shows the
	// effect at the age it had), spawned pre-aged at the next play.
	void open(const particle::EffectSceneConfig &config, const std::string &effect);
	// No scene: nothing plays.
	void close();
	// The next play spawns the effect again as a seek to its tick does (its age the tick's), a cycle anew.
	void restart() { jump_ = true; }

	// The scene played to the clock's tick `tick`.
	void play_to(int32_t tick, const EffectPlayOptions &options);

	// The scene the picture draws (null: none open). Shared with the device, which only reads it.
	const std::shared_ptr<particle::EffectScene> &scene() const { return scene_; }
	const std::string &effect() const { return effect_; }
	// The spawn now playing (none: the effect died and does not loop, or spawns nothing) and what the
	// last spawn came to.
	bool alive() const;
	particle::EffectSpawnStatus last_status() const { return last_status_; }
	// The clock tick the playing cycle counts its age from, the cycle's age at the tick played, the ticks
	// its spawn was pre-aged by, and how many spawns were made since the scene opened.
	int32_t cycle_start() const { return cycle_start_; }
	int32_t age() const { return tick_ - cycle_start_; }
	uint32_t pre_aged() const { return pre_aged_; }
	uint64_t spawns() const { return spawns_; }
	int32_t tick() const { return tick_; }
	bool played() const { return played_; }
	// Moves whenever the scene is opened or advanced: the snapshot a device drew is stale.
	uint64_t serial() const { return serial_; }

private:
	// A spawn at the clock's tick `tick`, pre-aged by `age` ticks (bounded), counted as the cycle that
	// began at tick - age.
	void spawn_(int32_t tick, int32_t age);

	std::shared_ptr<particle::EffectScene> scene_;
	std::string effect_;
	particle::EffectHandle handle_;
	particle::EffectGroupId group_;
	particle::EffectSpawnStatus last_status_ = particle::EffectSpawnStatus::InvalidHandle;
	bool played_ = false;
	bool jump_ = true;
	bool carry_ = false; // the next spawn carries the playing cycle's age (an open over a scene that played)
	int32_t tick_ = 0;
	int32_t cycle_start_ = 0;
	uint32_t pre_aged_ = 0;
	uint64_t spawns_ = 0;
	uint64_t serial_ = 0;
};

} // namespace opennova::editor
