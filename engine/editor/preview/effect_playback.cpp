#include <editor/preview/effect_playback.h>

#include <algorithm>

#include <runtime/particle/emitter.h>

namespace opennova::editor {

particle::EffectPose effect_play_pose() {
	// Forward up, its up hint turned to +X as the forward is vertical.
	return particle::descriptor_pose({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f});
}

void EffectPlayback::open(const particle::EffectSceneConfig &config, const std::string &effect) {
	scene_ = std::make_shared<particle::EffectScene>();
	scene_->open(config);
	effect_ = effect;
	handle_ = scene_->intern(effect);
	group_ = particle::EffectGroupId();
	last_status_ = particle::EffectSpawnStatus::InvalidHandle;
	schedule_.restart();
	++serial_;
}

void EffectPlayback::close() {
	if (scene_) ++serial_;
	scene_.reset();
	effect_.clear();
	handle_ = particle::EffectHandle();
	group_ = particle::EffectGroupId();
	last_status_ = particle::EffectSpawnStatus::InvalidHandle;
	schedule_.unplay();
	pre_aged_ = 0;
}

bool EffectPlayback::alive() const {
	return scene_ && group_ && scene_->contains_group(group_);
}

void EffectPlayback::spawn_(int32_t tick) {
	const int32_t age = std::max(tick, 0);
	particle::EffectSpawnRequest request;
	request.effect = handle_;
	request.pose = effect_play_pose();
	request.initial_age_ticks = uint32_t(age);
	const particle::EffectSpawnReceipt receipt = scene_->spawn(request);
	last_status_ = receipt.status;
	pre_aged_ = std::min(uint32_t(age), particle::kEffectInitialAgeTickLimit);
	// A spawn whose catch-up outlived every emitter is gone already (EffectScene::spawn releases it).
	group_ = receipt.spawned() && scene_->contains_group(receipt.group) ? receipt.group : particle::EffectGroupId();
	++spawns_;
}

void EffectPlayback::play_to(int32_t tick, const EffectPlayOptions &options, const particle::ParticleViewFrustum &frustum) {
	if (!scene_) return;
	scene_->set_global_wind(particle::mission_wind_vector(options.wind_speed, options.wind_direction));
	tick = std::max(tick, 0);
	// A jump: the effect spawned again at its age then, its one spawn's on tick 0 (older than it
	// lives, nothing stands).
	const auto jumped = [&](int32_t at) { spawn_(at); };
	const int32_t bound = int32_t(particle::kEffectInitialAgeTickLimit);
	if (!schedule_.jumps_to(tick) || tick <= bound) {
		if (schedule_.play_to(*scene_, tick, frustum, timed_, EffectSchedule::Make(), jumped)) ++serial_;
		return;
	}
	// A jump past the engine's catch-up bound, which ages a spawn no further than the bound: spawned at the age the
	// bound allows, then stepped on a game tick at a time (the schedule's continuous plays) to the tick sought, so it
	// shows its age there as a clock run from tick 0 shows it; while it lives, kEffectSeekStepTicks at most.
	const int32_t from = std::max(bound, tick - kEffectSeekStepTicks);
	schedule_.restart(); // the jump spawns it again there, whatever the tick it left
	schedule_.play_to(*scene_, from, frustum, timed_, EffectSchedule::Make(), jumped);
	int32_t at = from;
	while (at < tick && alive()) {
		at = std::min(tick, at + int32_t(particle::kEffectAdvanceTickLimit));
		schedule_.play_to(*scene_, at, frustum, timed_, EffectSchedule::Make(), jumped);
	}
	// Dead on the way (or spawned nothing): nothing stands, the clock's tick held.
	if (at < tick) schedule_.hold(tick);
	pre_aged_ += uint32_t(at - from);
	++serial_;
}

} // namespace opennova::editor
