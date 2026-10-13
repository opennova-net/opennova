#include <editor/preview/effect_playback.h>

#include <algorithm>

#include <runtime/particle/emitter.h>

namespace opennova::editor {

particle::EffectPose effect_play_pose() {
	// Forward up, its up hint turned to +X as the forward is vertical.
	return particle::descriptor_pose({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f});
}

void EffectPlayback::open(const particle::EffectSceneConfig &config, const std::string &effect) {
	// Another effect counts its age from tick 0; the same one keeps its spawn's.
	if (effect != effect_) cycle_start_ = 0;
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

void EffectPlayback::play_to(int32_t tick, const EffectPlayOptions &options, const particle::ParticleViewFrustum &frustum) {
	if (!scene_) return;
	scene_->set_global_wind(particle::mission_wind_vector(options.wind_speed, options.wind_direction));
	// A jump: the effect spawned again at its age then, the spawn's where the tick is past it, else its age since
	// tick 0 (older than it lives, nothing stands).
	const auto jumped = [&](int32_t at) {
		spawn_(at, schedule_.played() && at >= cycle_start_ ? at - cycle_start_ : at);
	};
	if (schedule_.play_to(*scene_, tick, frustum, timed_, EffectSchedule::Make(), jumped)) ++serial_;
}

} // namespace opennova::editor
