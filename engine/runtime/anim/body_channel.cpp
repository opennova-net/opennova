#include "body_channel.h"

#include <base/io/tick_rate.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace opennova::anim {

namespace {

const std::string kResetKey = "anim_reset";
const std::string kNoKey;

// Two playheads the same within a relative 1e-5 (an absolute 1e-5 near zero): a re-pose at the
// time already posed is no pose change.
bool same_playhead(double a, double b) {
	if (a == b) return true;
	double tolerance = 1e-5 * std::fabs(a);
	if (tolerance < 1e-5) tolerance = 1e-5;
	return std::fabs(a - b) < tolerance;
}

// x mod y, the sign of y (a loop's wrapped playhead).
double positive_mod(double x, double y) {
	double value = std::fmod(x, y);
	if ((value < 0.0 && y > 0.0) || (value > 0.0 && y < 0.0)) value += y;
	return value + 0.0;
}

} // namespace

void BodyChannel::bind(Rig rig) {
	rig_ = std::move(rig);
	reset_remote();
	key_.clear();
	variant_ = 0;
	time_ = 0.0;
	playing_ = false;
	external_phase_ = false;
	stamp_valid_ = false;
	clear_blend();
	last_slot_ = -1;
	last_slot_key_.clear();
	posed_.clear();
	request_ = PoseRequest::kNone;
	dirty_ = true;
}

void BodyChannel::set_person(bool person) {
	if (person_ == person) return;
	person_ = person;
	dirty_ = true;
}

// AnimMap registration binds an absent semantic state key to the state-0 RESET clip.
const std::string &BodyChannel::resolve_key(const std::string &key) const {
	if (!rig_) return kNoKey;
	if (!key.empty() && rig_->has_clip(key)) return key;
	return rig_->has_clip(kResetKey) ? kResetKey : kNoKey;
}

bool BodyChannel::looping(const std::string &key, int variant) const {
	const SkeletalClips::LoadedClip *clip = rig_ ? rig_->find_clip_variant(key, variant) : nullptr;
	return clip != nullptr && clip->clip.loops();
}

double BodyChannel::clip_phase_seconds(const std::string &key, int phase_ticks, int variant,
		bool parked) const {
	// A parked tick samples the clip's last frame at its boundary
	// (anim::ClipTimeline::seconds_at(ticks, armed_boundary)).
	return rig_ ? rig_->clip_seconds_at_tick(key, phase_ticks, variant, parked ? phase_ticks : -1)
				: 0.0;
}

void BodyChannel::set_playhead(double seconds) {
	// Any playhead write outside play_at's own apply invalidates the applied-tick stamp.
	stamp_valid_ = false;
	const float length = rig_ ? rig_->clip_length(key_, variant_) : 0.0f;
	if (length <= 0.0f) {
		time_ = 0.0;
	} else if (looping(key_, variant_)) {
		time_ = positive_mod(seconds, double(length));
	} else {
		time_ = std::clamp(seconds, 0.0, double(length));
	}
}

void BodyChannel::clear_blend() {
	if (!blend_source_key_.empty() || blend_weight_ < 1.0f) dirty_ = true;
	blend_source_key_.clear();
	blend_source_time_ = 0.0;
	blend_weight_ = 1.0f;
	blend_source_variant_ = 0;
}

void BodyChannel::reset_pose() {
	key_.clear();
	variant_ = 0;
	time_ = 0.0;
	playing_ = false;
	external_phase_ = false;
	stamp_valid_ = false;
	clear_blend();
	posed_.clear();
	request_ = PoseRequest::kRest;
	dirty_ = false;
}

// A served variant (the FSM owner's ring entry) plays as given; variant 0 resolves its key.
bool BodyChannel::play(const std::string &key_in, int variant) {
	const std::string &key = variant == 0 ? resolve_key(key_in) : key_in;
	if (!rig_ || key.empty() || !rig_->has_clip(key)) return false;
	clear_blend();
	if (key == key_ && variant == variant_) {
		external_phase_ = false;
		stamp_valid_ = false;
		playing_ = true;
		return true;
	}
	key_ = key;
	variant_ = variant;
	time_ = 0.0;
	playing_ = true;
	external_phase_ = false;
	stamp_valid_ = false;
	dirty_ = true;
	return true;
}

void BodyChannel::play_at_time(const std::string &key, int variant, double seconds) {
	if (!rig_ || !rig_->has_clip(key)) return;
	clear_blend();
	const bool same_external = external_phase_ && key == key_ && variant == variant_ &&
			same_playhead(time_, seconds);
	key_ = key;
	variant_ = variant;
	set_playhead(seconds);
	playing_ = false;
	external_phase_ = true;
	if (same_external && !dirty_) return;
	dirty_ = true;
	request_pose();
}

// The infantry motor's playhead: retail's AnimMap phase counts simulation ticks, the clip
// timelines keep its float rounding (ClipTimeline). `variant` is the ring entry the channel's
// re-init served (the simulation's InfantryState::anim_variant).
void BodyChannel::play_at(const std::string &key_in, int phase_ticks, int variant, bool parked) {
	const std::string &key = resolve_key(key_in);
	if (key.empty()) return;
	clear_blend();
	// The repeat fast path: the stamp proves this exact (key, tick) pair posed last and nothing
	// else dirtied the pose.
	if (stamp_valid_ && external_phase_ && !dirty_ && phase_ticks == stamp_ticks_ && key == key_ &&
			variant == variant_ && parked == stamp_parked_)
		return;
	const bool same_key = key == key_;
	const int previous_variant = variant_;
	const double previous_time = time_;
	const bool previous_external = external_phase_;
	const double seconds = clip_phase_seconds(key, phase_ticks, variant, parked);
	const bool same_external = previous_external && same_key && variant == previous_variant &&
			same_playhead(previous_time, seconds);
	key_ = key;
	variant_ = variant;
	set_playhead(seconds);
	playing_ = false;
	external_phase_ = true;
	stamp_valid_ = true;
	stamp_ticks_ = phase_ticks;
	stamp_parked_ = parked;
	if (same_external && !dirty_) return;
	dirty_ = true;
	request_pose();
}

void BodyChannel::play_blend_at(const std::string &source_key_in, int source_phase_ticks,
		const std::string &target_key_in, int target_phase_ticks, double weight,
		int source_variant, int variant) {
	if (!rig_) return;
	const std::string source_key = resolve_key(source_key_in);
	const std::string target_key = resolve_key(target_key_in);
	if (target_key.empty()) {
		if (!source_key.empty()) {
			play_at(source_key, source_phase_ticks, source_variant);
		} else {
			reset_pose();
		}
		return;
	}
	if (source_key.empty() || weight >= 1.0) {
		play_at(target_key, target_phase_ticks, variant);
		return;
	}
	pose_blend_at_times(source_key,
			clip_phase_seconds(source_key, source_phase_ticks, source_variant),
			target_key, clip_phase_seconds(target_key, target_phase_ticks, variant),
			static_cast<float>(weight), source_variant, variant);
}

void BodyChannel::pose_blend_at_times(const std::string &source_key, double source_time,
		const std::string &target_key, double target_time, float weight, int source_variant,
		int target_variant) {
	key_ = target_key;
	variant_ = target_variant;
	set_playhead(target_time);
	playing_ = false;
	external_phase_ = true;
	stamp_valid_ = false;
	blend_source_key_ = source_key;
	blend_source_time_ = source_time;
	blend_source_variant_ = source_variant;
	blend_weight_ = std::clamp(weight, 0.0f, 1.0f);
	dirty_ = true;
	request_pose();
}

void BodyChannel::play_seeded(const std::string &key, int phase_ticks) {
	if (select_seeded(key, phase_ticks)) request_pose();
}

bool BodyChannel::select_seeded(const std::string &key_in, int phase_ticks) {
	const std::string &key = resolve_key(key_in);
	if (key.empty()) return false;
	clear_blend();
	key_ = key;
	variant_ = 0;
	set_playhead(clip_phase_seconds(key_, phase_ticks));
	playing_ = true;
	external_phase_ = false;
	dirty_ = true;
	return true;
}

const std::string &BodyChannel::slot_key(int slot) {
	if (!rig_ || slot < 0) return kNoKey;
	if (slot != last_slot_) {
		last_slot_key_ = rig_->slot_to_key(slot);
		last_slot_ = slot;
	}
	return last_slot_key_;
}

bool BodyChannel::slot_play_key(int slot, std::string &r_key) const {
	if (!rig_ || slot < 0) return false;
	r_key = rig_->slot_to_key(slot);
	if (r_key.empty()) return false;
	return !(r_key == key_ && !external_phase_);
}

bool BodyChannel::set_time(double seconds) {
	if (!rig_ || key_.empty()) return false;
	clear_blend();
	external_phase_ = false;
	set_playhead(seconds);
	dirty_ = true;
	request_pose();
	return true;
}

void BodyChannel::stop() {
	playing_ = false;
	external_phase_ = false;
	stamp_valid_ = false;
	clear_blend();
	reset_remote();
}

double BodyChannel::playhead() const {
	if (!rig_ || key_.empty()) return 0.0;
	const float length = rig_->clip_length(key_, variant_);
	if (length <= 0.0f) return 0.0;
	if (looping(key_, variant_)) return positive_mod(time_, double(length));
	return std::clamp(time_, 0.0, double(length));
}

// --- the receive-side arbitration -------------------------------------------------------------

// The channel owns the current/pending acceptance because it also owns the clip time and its
// completion: every raw wire request reaches it [orig: NetPacket_SerializeInfantryEntityState
// @0x4C0859; NetPacket_SerializePlayerState @0x4C11A6].
void BodyChannel::remote_request(int state, const std::string &key, uint32_t flags, int phase_ticks) {
	if (state < 0 || resolve_key(key).empty()) return;
	const bool had_current = remote_.current() >= 0 && !key_.empty();
	const BodyArrival arrival = remote_.request(state, flags);
	if (arrival == BodyArrival::queue) {
		queue_remote(key);
	} else if (arrival == BodyArrival::commit) {
		accept_remote(key, flags, phase_ticks, had_current);
	}
}

void BodyChannel::reset_remote() {
	remote_.reset();
	remote_pending_key_.clear();
	clear_remote_blend();
}

void BodyChannel::accept_remote(const std::string &key, uint32_t flags, int phase_ticks,
		bool had_current) {
	remote_pending_key_.clear();
	const int target_phase = phase_ticks >= 0 ? phase_ticks : 0;
	if (had_current) {
		start_remote_blend(key, flags, target_phase);
	} else if (select_seeded(key, target_phase)) {
		request_pose();
	}
}

void BodyChannel::queue_remote(const std::string &key) {
	remote_pending_key_ = key;
	remote_.arm_completion(time_, rig_->clip_length(key_, variant_), looping(key_, variant_));
	if (promote_remote_if_due()) request_pose();
}

void BodyChannel::clear_remote_blend() {
	remote_.clear_blend();
	remote_blend_source_key_.clear();
	clear_blend();
}

int BodyChannel::phase_ticks_of(const std::string &key, double seconds) const {
	if (!rig_ || key.empty()) return 0;
	if (rig_->clip_fps(key) <= 0.0f) return 0;
	return std::max(static_cast<int>(std::floor(seconds * io::kTicksPerSecondInt + 0.5)), 0);
}

void BodyChannel::start_remote_blend(const std::string &target_key_in, uint32_t flags,
		int target_phase) {
	const std::string target_key = resolve_key(target_key_in);
	if (target_key.empty()) return;
	const std::string source_key = remote_.blending() ? remote_blend_source_key_ : key_;
	if (source_key.empty() || !rig_ || !rig_->has_clip(source_key)) {
		clear_remote_blend();
		if (select_seeded(target_key, target_phase)) request_pose();
		return;
	}
	remote_blend_source_key_ = source_key;
	remote_.begin_blend(phase_ticks_of(key_, time_), time_, target_phase, flags);
	pose_blend_at_times(remote_blend_source_key_, remote_.source_time(), target_key,
			clip_phase_seconds(target_key, remote_.target_phase()), remote_.weight(), 0, 0);
}

bool BodyChannel::remote_tick(int state) {
	if (!remote_.blending()) {
		if (remote_.has_pending()) {
			promote_remote_if_due();
			return true;
		}
		return false;
	}
	if (!remote_.advance_blend(state)) return false;
	remote_.set_source_time(clip_phase_seconds(remote_blend_source_key_, remote_.source_phase()));
	const std::string target_key = key_;
	if (remote_.weight() >= 1.0f) {
		const int target_phase = remote_.target_phase();
		clear_remote_blend();
		if (select_seeded(target_key, target_phase)) request_pose();
		return true;
	}
	pose_blend_at_times(remote_blend_source_key_, remote_.source_time(), target_key,
			clip_phase_seconds(target_key, remote_.target_phase()), remote_.weight(), 0, 0);
	return true;
}

bool BodyChannel::promote_remote_if_due() {
	if (!remote_.promote_if_due(time_)) return false;
	const std::string key = std::move(remote_pending_key_);
	remote_pending_key_.clear();
	start_remote_blend(key, remote_.flags(), 0);
	return true;
}

// --- the layers -------------------------------------------------------------------------------

bool BodyChannel::set_weapon_channel(const WeaponChannel &channel) {
	if (channel == weapon_) return false;
	weapon_ = channel;
	dirty_ = true;
	return true;
}

bool BodyChannel::set_aim_overlay(const Quat deltas[kOverlayClassCount]) {
	bool changed = !overlay_valid_;
	for (size_t c = 0; c < kOverlayClassCount && !changed; ++c) {
		const Quat &a = overlay_[c];
		const Quat &b = deltas[c];
		changed = a.w != b.w || a.x != b.x || a.y != b.y || a.z != b.z;
	}
	if (!changed) return false;
	std::copy(deltas, deltas + kOverlayClassCount, overlay_.begin());
	overlay_valid_ = true;
	dirty_ = true;
	return true;
}

bool BodyChannel::clear_aim_overlay() {
	if (!overlay_valid_) return false;
	overlay_valid_ = false;
	dirty_ = true;
	return true;
}

void BodyChannel::set_override(const std::vector<PoseBone> &pose, float weight) {
	override_ = pose;
	override_weight_ = std::clamp(weight, 0.0f, 1.0f);
	dirty_ = true;
	request_pose();
}

bool BodyChannel::clear_override() {
	if (override_weight_ <= 0.0f && override_.empty()) return false;
	override_.clear();
	override_weight_ = 0.0f;
	dirty_ = true;
	request_pose();
	return true;
}

// --- the clock and the pose -------------------------------------------------------------------

void BodyChannel::advance(double seconds) {
	if (!active()) return;
	if (playing_ && !external_phase_ && seconds != 0.0) {
		time_ += seconds;
		dirty_ = true;
	}
	if (promote_remote_if_due()) dirty_ = true;
}

BodyChannel::PoseRequest BodyChannel::take_pose_request() {
	const PoseRequest request = request_;
	request_ = PoseRequest::kNone;
	return request;
}

bool BodyChannel::needs_frame(bool clock_running) const {
	if (!active()) return false;
	return dirty_ || (clock_running && playing_ && !external_phase_) || remote_.has_pending();
}

bool BodyChannel::eval_clip_pose(double offset_seconds, std::vector<PoseBone> &r_pose) const {
	if (!active()) return false;
	rig_->eval_pose(key_, std::max(0.0, time_ + offset_seconds), variant_, r_pose);
	rig_->pose_person_rows_past_clip(r_pose, rig_->clip_file_bones(key_, variant_), nullptr, {});
	return !r_pose.empty();
}

bool BodyChannel::compose() {
	if (!active()) return false;
	const SkeletalClips &rig = *rig_;
	if (override_weight_ > 0.0f && !override_.empty()) {
		if (override_weight_ >= 1.0f) {
			posed_ = override_;
		} else {
			std::vector<PoseBone> clip;
			eval_clip_pose(0.0, clip);
			blend_poses(clip, override_, override_weight_, posed_);
		}
		dirty_ = false;
		return true;
	}
	const std::vector<uint8_t> &classes = rig.overlay_classes();
	const bool use_overlay = overlay_valid_ && !classes.empty();
	const bool use_primary_blend = !blend_source_key_.empty() && blend_weight_ < 1.0f;
	// The primary sample: the target alone, or blended from the outgoing channel.
	if (use_primary_blend) {
		rig.eval_pose_blended(blend_source_key_, blend_source_time_, key_, time_, blend_weight_,
				posed_, blend_source_variant_, variant_);
	} else {
		rig.eval_pose(key_, time_, variant_, posed_);
	}
	// The weapon splice and the aim overlay ride the overlay alone: a body with no overlay draws
	// its primary channel bare.
	if (use_overlay) {
		double weapon_seconds = 0.0;
		double weapon_prev_seconds = 0.0;
		if (!weapon_.key.empty()) {
			weapon_seconds = clip_phase_seconds(weapon_.key, weapon_.phase_ticks, weapon_.variant,
					weapon_.parked);
			if (!weapon_.prev_key.empty())
				weapon_prev_seconds = clip_phase_seconds(weapon_.prev_key, weapon_.prev_phase_ticks,
						weapon_.prev_variant);
		}
		rig.apply_pose_overlay(posed_, overlay_.data(), classes, weapon_.key, weapon_seconds,
				weapon_.prev_key, weapon_prev_seconds, weapon_.blend_weight, weapon_.variant,
				weapon_.prev_variant);
	}
	if (person_) {
		static const std::vector<uint8_t> kNoClasses;
		rig.pose_person_rows_past_clip(posed_, rig.clip_file_bones(key_, variant_),
				use_overlay ? overlay_.data() : nullptr, use_overlay ? classes : kNoClasses);
	}
	dirty_ = false;
	return true;
}

} // namespace opennova::anim
