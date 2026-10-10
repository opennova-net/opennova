#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <runtime/particle/effect_closure.h>
#include <runtime/particle/effect_scene.h>

namespace opennova::editor {

class PreviewEffectCatalog;

// One effect a definition's picture spawns (ADR 0046 DI-21): the effect's name, the pose the game's spawn
// hands the scene (the preview's space: particle::forward_pose for an item's particle slot, particle::descriptor_pose
// for a death's effect), the clock tick it spawns on, and what spawns it: its source ("particle_slot", the
// death banks' "dead", "fire" and "other", "death" for a class's one effect at the item) and the user point
// it spawns at ("" the item's origin).
struct DefinitionSpawn {
	std::string effect;
	particle::EffectPose pose;
	int32_t tick = 0;
	std::string source;
	std::string point;
};
bool same_spawns(const std::vector<DefinitionSpawn> &a, const std::vector<DefinitionSpawn> &b);

// A definition's effects on the preview clock (ADR 0046 DI-21; the effect preview's playback, DI-14's
// EffectPlayback, for several spawns at their poses and ticks): the engine's own effect scene opened over
// the closures of the spawns' effects (PreviewEffectCatalog::closures: what the game spawns for each name,
// cut to what those spawns read), each spawn made on its tick as the clock runs a game tick at a time (the
// scene's own fixed step: what shows at a tick is the same however the frames fell), never again (the game
// spawns each once; an effect that dies is gone). A jump of the clock (a seek, a step back, more ticks at
// once than one advance takes) empties the scene and makes every spawn due again, pre-aged by its age then
// (EffectSpawnRequest::initial_age_ticks, the engine's catch-up, bounded by kEffectInitialAgeTickLimit).
// The scene is the device's picture (godot/src/authoring/preview_effects draws it).
class DefinitionEffects {
public:
	// The spawns `spawns` over `catalog`: the scene opened anew where the spawns or the catalog moved (a jump
	// at the next play), else kept as it plays; kept too where `spawns` adds spawns of effects the scene holds
	// after the same ones (a weapon's shots, DI-22), each new one made at its tick, or at the next play pre-aged
	// where the clock passed it. True when it opened anew.
	bool plan(const PreviewEffectCatalog &catalog, uint64_t catalog_serial, std::vector<DefinitionSpawn> spawns);
	// No scene: nothing plays.
	void close();
	// The scene played to the clock's tick `tick`.
	void play_to(int32_t tick);

	// The scene the picture draws (null: no spawn, or none whose effect the catalog finds). Shared with the
	// device, which only reads it.
	const std::shared_ptr<particle::EffectScene> &scene() const { return scene_; }
	const std::vector<DefinitionSpawn> &spawns() const { return spawns_; }
	// What the game resolves a spawn's effect to (null for a name no spawn has).
	const particle::EffectClosure *closure_of(const std::string &effect) const;
	// Each spawn's last spawn (InvalidHandle before its tick) and whether its group lives now.
	particle::EffectSpawnStatus status(size_t spawn) const;
	bool alive(size_t spawn) const;
	int32_t tick() const { return tick_; }
	// Moves whenever the scene is opened or advanced (the snapshot a device drew is stale), and how many
	// times it was opened (a test's measure).
	uint64_t serial() const { return serial_; }
	uint64_t opens() const { return opens_; }

private:
	void spawn_(size_t spawn, int32_t age);

	std::shared_ptr<particle::EffectScene> scene_;
	std::vector<DefinitionSpawn> spawns_;
	std::vector<std::string> names_;
	std::vector<particle::EffectClosure> closures_;
	std::vector<particle::EffectGroupId> groups_;
	std::vector<particle::EffectSpawnStatus> statuses_;
	std::vector<bool> made_; // each spawn made since the scene last emptied
	uint64_t catalog_serial_ = UINT64_MAX;
	bool played_ = false;
	int32_t tick_ = 0;
	uint64_t serial_ = 0;
	uint64_t opens_ = 0;
};

} // namespace opennova::editor
