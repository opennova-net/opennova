#include <editor/preview/effect_playback.h>

#include <algorithm>
#include <cmath>

#include <runtime/particle/emitter.h>

namespace opennova::editor {

particle::EffectPose effect_play_pose() {
	// EffectWorld::forward_pose of +Y: forward up, its up hint turned to +X as the forward is vertical.
	return effect_descriptor_pose({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f});
}

particle::EffectPose effect_forward_pose(const particle::Vec3 &at, const particle::Vec3 &forward) {
	// EffectWorld::forward_pose: the forward normalized, the up hint +Y (+X where the forward is vertical),
	// right = hint x forward, up = forward x right; a zero forward keeps the identity basis.
	particle::EffectPose pose;
	pose.position = at;
	const float length = std::sqrt(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
	if (length * length <= 0.000001f) return pose;
	const particle::Vec3 f{forward.x / length, forward.y / length, forward.z / length};
	const particle::Vec3 hint = std::fabs(f.y) > 0.999f ? particle::Vec3{1.0f, 0.0f, 0.0f} : particle::Vec3{0.0f, 1.0f, 0.0f};
	const auto cross = [](const particle::Vec3 &a, const particle::Vec3 &b) {
		return particle::Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
	};
	const auto unit = [](particle::Vec3 v) {
		const float n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
		return n > 0.0f ? particle::Vec3{v.x / n, v.y / n, v.z / n} : v;
	};
	pose.right = unit(cross(hint, f));
	pose.up = unit(cross(f, pose.right));
	pose.forward = f;
	return pose;
}

particle::EffectPose effect_descriptor_pose(const particle::Vec3 &at, const particle::Vec3 &orientation) {
	const float squared = orientation.x * orientation.x + orientation.y * orientation.y + orientation.z * orientation.z;
	return effect_forward_pose(at, squared <= 0.000001f ? particle::Vec3{0.0f, 1.0f, 0.0f} : orientation);
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
