#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <editor/preview/model_preview_camera.h>
#include <runtime/particle/effect_scene.h>
#include <runtime/particle/emitter.h>

namespace opennova::editor {

// The camera's view as the NOVISNOUPDATE gate reads it: the six planes of `camera` on a device `width` x `height`
// (its horizontal field of view held, as the devices' Camera3D keeps its width), in the preview's space, each
// normalized and holding what the camera sees on its inside, as the game hands the effect scene the camera that drew
// the last frame (EffectWorld's set_view_frustum). A NOVISNOUPDATE emitter whose bounds fall outside it does not
// advance, frozen until seen again [orig: CParticleEmitter_AdvanceFrame @ 0x5e6588..0x5e65f3, the engine's
// particle::emitter_advance]. No planes (every emitter advances) for a device of no size.
particle::ParticleViewFrustum preview_view_frustum(const OrbitCamera &camera, int width, int height);
// The same view with the world pass's near and far planes in place of the orbit camera's:
// renderer::kScenePassNearZ and renderer::scene_far_plane of `fog_distance` (the fog level the
// scene draws under), the planes the game's camera holds when its effects test a NOVISNOUPDATE
// emitter (GameWorld's scene environment leg, Camera3D's frustum to EffectWorld's
// set_view_frustum), so an emitter past the fog is outside, as retail's clip test puts a point
// whose depth less its radius passes the current fog distance [orig: Viewport_TransformAndClipPoint
// @ 0x4115e0, the fog test @ 0x411644]. A mission's view, whose world draws under its fog. No
// planes for a device of no size, or no fog distance known yet (0).
particle::ParticleViewFrustum scene_pass_view_frustum(
		const OrbitCamera &camera, int width, int height, float fog_distance);

// The timed spawns an effect scene plays (a definition's, a mission's shots'): each one's clock tick, and whether it
// was made since the scene last emptied (as long as `ticks`; its owner sizes both).
struct EffectTimedSpawns {
	std::vector<int32_t> ticks;
	std::vector<bool> made;
	// No spawns, none made.
	void clear() {
		ticks.clear();
		made.clear();
	}
	// `ticks` the spawns now: kept made where `extends` (spawns added after the same ones), else none made.
	void plan(std::vector<int32_t> now, bool extends) {
		ticks = std::move(now);
		if (extends) made.resize(ticks.size(), false);
		else made.assign(ticks.size(), false);
	}
};

// How an editor effect scene plays on the preview clock (ADR 0046 DI-14, DI-21, DI-22): the effect viewport's one
// spawn (preview/effect_playback), a definition's spawns (preview/definition_effects) and a mission's slots and shots
// (preview/mission_effects), each the engine's own effect scene. The scene advanced a game tick at a time, each step
// its own fixed one (1 / io::kTickHz: what shows at a tick is the same however the frames fell) through the view's
// planes (preview_view_frustum), each timed spawn made on its tick, and one whose tick the clock passed before it was
// planned made at once pre-aged by its age. A jump of the clock (a seek, a step back, more ticks at once than one
// advance takes, particle::kEffectAdvanceTickLimit, or a restart) empties the scene, has the owner make again what it
// holds standing, then makes every timed spawn due, each pre-aged by its age then, as the engine's catch-up spawn ages
// one (EffectSpawnRequest::initial_age_ticks, bounded by kEffectInitialAgeTickLimit).
class EffectSchedule {
public:
	// A timed spawn made pre-aged by `age` ticks (0 on its own tick).
	using Make = std::function<void(size_t spawn, int32_t age)>;
	// After a jump to `tick` empties the scene: what the owner holds standing made again (the effect viewport's one
	// spawn, a mission's slots; the timed spawns follow).
	using Jumped = std::function<void(int32_t tick)>;

	// The scene played to the clock's tick `tick` (at least 0). True when what it holds moved: a jump, or ticks run (a
	// late timed spawn made with no tick run leaves it false, as the owners' serials always read it).
	bool play_to(particle::EffectScene &scene, int32_t tick, const particle::ParticleViewFrustum &frustum,
			EffectTimedSpawns &timed, const Make &make, const Jumped &jumped);
	// The tick held where no scene plays.
	void hold(int32_t tick);
	// The next play a jump.
	void restart() { jump_ = true; }
	// Not played since what it plays was planned again: the next play a jump, the tick kept.
	void unplay() {
		played_ = false;
		jump_ = true;
	}
	// Never played: the next play a jump, at tick 0.
	void reset();
	bool played() const { return played_; }
	int32_t tick() const { return tick_; }

private:
	int32_t tick_ = 0;
	bool played_ = false;
	bool jump_ = true;
};

} // namespace opennova::editor
