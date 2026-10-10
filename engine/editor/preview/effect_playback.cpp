#include <editor/preview/effect_playback.h>

#include <algorithm>

#include <runtime/particle/emitter.h>

namespace opennova::editor {

particle::EffectPose effect_play_pose() {
	// Forward up, its up hint turned to +X as the forward is vertical.
	return particle::descriptor_pose({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f});
}

void EffectPlayback::open(const particle::EffectSceneConfig &config, const std::string &effect) {
	// Another effect counts its age from tick 0; the same one keeps its playing cycle's.
	if (effect != effect_) cycle_start_ = 0;
	scene_ = std::make_shared<particle::EffectScene>();
	scene_->open(config);
	effect_ = effect;
	handle_ = scene_->intern(effect);
	group_ = particle::EffectGroupId();
	last_status_ = particle::EffectSpawnStatus::InvalidHandle;
	jump_ = true;
	++serial_;
}

void EffectPlayback::close() {
	if (scene_) ++serial_;
	scene_.reset();
	effect_.clear();
	handle_ = particle::EffectHandle();
	group_ = particle::EffectGroupId();
	last_status_ = particle::EffectSpawnStatus::InvalidHandle;
	played_ = false;
	jump_ = true;
	cycle_start_ = 0;
	pre_aged_ = 0;
}

bool EffectPlayback::alive() const {
	return scene_ && group_ && scene_->contains_group(group_);
}

void EffectPlayback::spawn_(int32_t tick, int32_t age) {
	age = std::max(age, 0);
	particle::EffectSpawnRequest request;
	request.effect = handle_;
	request.pose = effect_play_pose();
	request.initial_age_ticks = uint32_t(age);
	const particle::EffectSpawnReceipt receipt = scene_->spawn(request);
	last_status_ = receipt.status;
	pre_aged_ = std::min(uint32_t(age), particle::kEffectInitialAgeTickLimit);
	cycle_start_ = tick - age;
	// A spawn whose catch-up outlived every emitter is gone already (EffectScene::spawn releases it).
	group_ = receipt.spawned() && scene_->contains_group(receipt.group) ? receipt.group : particle::EffectGroupId();
	++spawns_;
}

void EffectPlayback::play_to(int32_t tick, const EffectPlayOptions &options) {
	if (!scene_) return;
	tick = std::max(tick, 0);
	scene_->set_global_wind(particle::mission_wind_vector(options.wind_speed, options.wind_direction));
	const bool continuous = played_ && !jump_ && tick >= tick_ &&
			tick - tick_ <= int32_t(particle::kEffectAdvanceTickLimit);
	if (!continuous) {
		// A jump: the scene emptied and the effect spawned again at its age then: the playing cycle's where
		// the tick is in it, else its age since tick 0.
		scene_->reset_runtime_state();
		const int32_t age = played_ && tick >= cycle_start_ ? tick - cycle_start_ : tick;
		spawn_(tick, age);
		// Older than it lives: while it loops, a cycle begins at the tick.
		if (!group_ && options.loop && age > 0 && last_status_ == particle::EffectSpawnStatus::Spawned) spawn_(tick, 0);
		jump_ = false;
		played_ = true;
		tick_ = tick;
		++serial_;
		return;
	}
	// A game tick at a time (the scene's own fixed step), a cycle begun at the tick the last one died.
	particle::EffectAdvanceRequest step;
	step.delta_seconds = 1.0f / static_cast<float>(io::kTickHz);
	const bool dead_spawn = last_status_ == particle::EffectSpawnStatus::Spawned;
	if (options.loop && dead_spawn && !alive()) spawn_(tick_, 0);
	for (int32_t at = tick_ + 1; at <= tick; ++at) {
		scene_->advance_simulation(step);
		if (group_ && !scene_->contains_group(group_)) {
			group_ = particle::EffectGroupId();
			if (options.loop) spawn_(at, 0);
		}
	}
	if (tick != tick_) ++serial_;
	tick_ = tick;
}

} // namespace opennova::editor
