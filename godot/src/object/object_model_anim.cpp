// ObjectModel: main-body skeletal channels (.bad/.adm), the retail
// remote body-state arbitration, the muzzle userpoint, PLAYPARTANIM part
// channels, aim overlay / weapon channel, and pose evaluation.
// Ported verbatim from object_body_anim.gd (2026-08-09 de-scripting);
// the skeletal reference is typed (Ref<SkeletalAnim>) so the former
// script-double eval fallbacks are gone.

#include "object/object_model.h"

#include <godot_cpp/core/math.hpp>

#include <runtime/anim/aim_overlay.h> // kOverlayClassCount (the nine overlay classes)
#include <runtime/world/ai.h> // kPartAnimPhaseOne (the PLAYPARTANIM phase domain)
#include <runtime/world/infantry.h>

#include <algorithm>
#include <limits>

namespace godot {

namespace {

constexpr double kPartAnimTickS = 0.016;

const char *kPartAnimCtrlNames[2] = { "VEHICLE_SPECIAL1", "VEHICLE_SPECIAL2" };
const char *kPartAnimCtrlOwners[2] = { "present:part_anim:1", "present:part_anim:2" };

float f32(double p_value) {
	return static_cast<float>(p_value);
}

} // namespace

void ObjectModel::set_skeletal_anim(const Ref<SkeletalAnim> &p_skeletal) {
	wake_runtime_frame();
	skeletal_ = p_skeletal;
	reset_remote_body_state();
	anim_key_ = String();
	anim_variant_ = 0;
	anim_time_ = 0.0;
	anim_playing_ = false;
	anim_external_phase_ = false;
	body_phase_stamp_valid_ = false;
	clear_body_blend();
	last_slot_resolved_ = -1;
	last_slot_key_ = String();
	body_pose_dirty_ = true;
	rebuild();
}

// Whether this model resolved a bullet fire-origin userpoint onto its
// skeleton (the D-AI-6 fire-origin seam; infantry body models author one).
bool ObjectModel::has_muzzle() const {
	return muzzle_bone_ >= 0 && skeleton_ != nullptr;
}

// The def names the muzzle: items.def launchups_closeattack authors the launch
// userpoint (JO NPC riflemen: mflash01), pushed here by the placer. Resolve it
// case-insensitively against the model's userpoint table — retail's by-name
// lookup [orig: modelgpm_FindUserpointByName @ 0x5b2170 via Entity_ResolveBoneUserpoints (ex sub_545940)]. The
// rig is index-driven, so the userpoint's subobject row IS the bone index; no
// authored name (or no match) means no AI muzzle.
void ObjectModel::set_muzzle_point_name(const String &p_name) {
	if (muzzle_point_name_ == p_name) {
		return;
	}
	muzzle_point_name_ = p_name;
	resolve_muzzle_userpoint();
}

void ObjectModel::resolve_muzzle_userpoint() {
	muzzle_bone_ = -1;
	if (skeleton_ == nullptr || object_data_.is_null() ||
			muzzle_point_name_.is_empty()) {
		return;
	}
	const String wanted = muzzle_point_name_.to_lower();
	const int count = object_data_->get_user_point_count();
	int best = -1;
	for (int i = 0; i < count; ++i) {
		const Dictionary info = object_data_->get_user_point_info(i);
		if (String(info.get("name", "")).to_lower() == wanted) {
			best = i;
			break;
		}
	}
	if (best < 0) {
		return;
	}
	const Dictionary info2 = object_data_->get_user_point_info(best);
	const int bone = int(info2.get("subobject", -1));
	if (bone < 0 || bone >= skeleton_->get_bone_count()) {
		return;
	}
	muzzle_bone_ = bone;
	muzzle_model_pos_ = info2.get("position", Vector3());
}

// Play a main-body clip by ADM key. Missing semantic keys use this ADM's
// RESET binding when present.
void ObjectModel::play_body_clip(const String &p_key) {
	wake_runtime_frame();
	play_body_clip_variant(p_key, 0);
}

// play_body_clip selecting a same-key VARIANT (multi-clip .adm rows): the FSM
// owner's ring serves the index and playback follows that latch until the next
// play. [orig: AnimMap_PlayAnimBySlot @0x40bda0 latches the served ring entry]
void ObjectModel::play_body_clip_variant(const String &p_key, int p_variant) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->play_body_clip_variant(p_key, p_variant);
	}
	wake_runtime_frame();
	String key = p_key;
	if (p_variant == 0) {
		key = resolve_body_clip_key(key);
	}
	if (skeletal_.is_null() || key.is_empty() || !skeletal_->has_clip(key)) {
		return;
	}
	clear_body_blend();
	if (key == anim_key_ && p_variant == anim_variant_) {
		anim_external_phase_ = false;
		body_phase_stamp_valid_ = false;
		anim_playing_ = true;
		return;
	}
	anim_key_ = key;
	anim_variant_ = p_variant;
	anim_time_ = 0.0;
	anim_playing_ = true;
	anim_external_phase_ = false;
	body_phase_stamp_valid_ = false;
	body_pose_dirty_ = true;
}

// Pose a selected clip variant at an authoritative time; render-frame
// _process(delta) never advances the playhead afterwards.
void ObjectModel::play_body_clip_variant_at_time(const String &p_key,
		int p_variant, double p_seconds) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->play_body_clip_variant_at_time(p_key, p_variant, p_seconds);
	}
	wake_runtime_frame();
	if (skeletal_.is_null() || !skeletal_->has_clip(p_key)) {
		return;
	}
	clear_body_blend();
	const bool same_external = anim_external_phase_ && p_key == anim_key_ &&
			p_variant == anim_variant_ && Math::is_equal_approx(anim_time_, p_seconds);
	anim_key_ = p_key;
	anim_variant_ = p_variant;
	set_body_playhead(p_seconds);
	anim_playing_ = false;
	anim_external_phase_ = true;
	if (same_external && !body_pose_dirty_) {
		return;
	}
	body_pose_dirty_ = true;
	advance_body_animation(0.0);
}

// Pose a main-body clip at the authoritative infantry motor playhead. IDA's
// AnimMap phase advances in half-frame ticks: seconds = ticks / (2 * clip_fps).
void ObjectModel::play_body_clip_at(const String &p_key, int p_phase_ticks) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->play_body_clip_at(p_key, p_phase_ticks);
	}
	wake_runtime_frame();
	const String key = resolve_body_clip_key(p_key);
	if (key.is_empty()) {
		return;
	}
	clear_body_blend();
	// Repeat-call fast path: the stamp proves this exact (key, tick) pair is
	// what posed the skeleton last and nothing else dirtied the pose.
	if (body_phase_stamp_valid_ && anim_external_phase_ && !body_pose_dirty_ &&
			p_phase_ticks == body_phase_ticks_applied_ && key == anim_key_) {
		return;
	}
	const String previous_key = anim_key_;
	const double previous_time = anim_time_;
	const bool previous_external = anim_external_phase_;
	const float fps = skeletal_->get_clip_fps(key);
	double seconds = 0.0;
	if (fps > 0.0f) {
		seconds = double(MAX(p_phase_ticks, 0)) / (2.0 * fps);
	}
	const bool same_external = previous_external && key == previous_key &&
			Math::is_equal_approx(previous_time, seconds);
	anim_key_ = key;
	anim_variant_ = 0; // stamp-driven body path: variant rings stay on the 3P channel
	set_body_playhead(seconds);
	anim_playing_ = false;
	anim_external_phase_ = true;
	body_phase_stamp_valid_ = true;
	body_phase_ticks_applied_ = p_phase_ticks;
	if (same_external && !body_pose_dirty_) {
		return;
	}
	body_pose_dirty_ = true;
	advance_body_animation(0.0);
}

// Pose the authoritative outgoing + incoming PRIMARY channels at one shared
// blend weight, matching AnimChannel_BlendTwoChannels ->
// Entity_BuildBoneTransformMatrices. A missing semantic source or target uses
// this ADM's RESET binding; weight 1 takes the stamped single-channel path.
void ObjectModel::play_body_blend_at(const String &p_source_key,
		int p_source_phase_ticks, const String &p_target_key,
		int p_target_phase_ticks, double p_weight) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->play_body_blend_at(p_source_key, p_source_phase_ticks,
				p_target_key, p_target_phase_ticks, p_weight);
	}
	wake_runtime_frame();
	if (skeletal_.is_null()) {
		return;
	}
	const String source_key = resolve_body_clip_key(p_source_key);
	const String target_key = resolve_body_clip_key(p_target_key);
	const bool source_valid = !source_key.is_empty();
	const bool target_valid = !target_key.is_empty();
	if (!target_valid) {
		if (source_valid) {
			play_body_clip_at(source_key, p_source_phase_ticks);
		} else {
			reset_body_pose();
		}
		return;
	}
	if (!source_valid || p_weight >= 1.0) {
		play_body_clip_at(target_key, p_target_phase_ticks);
		return;
	}
	pose_body_blend_at_times(source_key,
			clip_phase_seconds(source_key, p_source_phase_ticks), target_key,
			clip_phase_seconds(target_key, p_target_phase_ticks),
			static_cast<float>(p_weight));
}

void ObjectModel::pose_body_blend_at_times(const String &p_source_key,
		double p_source_time, const String &p_target_key, double p_target_time,
		float p_weight) {
	anim_key_ = p_target_key;
	anim_variant_ = 0;
	set_body_playhead(p_target_time);
	anim_playing_ = false;
	anim_external_phase_ = true;
	body_phase_stamp_valid_ = false;
	body_blend_source_key_ = p_source_key;
	body_blend_source_time_ = p_source_time;
	body_blend_weight_ = CLAMP(p_weight, 0.0f, 1.0f);
	body_pose_dirty_ = true;
	advance_body_animation(0.0);
}

// Seed a main-body clip from retail half-frame ticks, pose it immediately, and
// leave it free-running.
void ObjectModel::play_body_clip_seeded(const String &p_key, int p_phase_ticks) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->play_body_clip_seeded(p_key, p_phase_ticks);
	}
	wake_runtime_frame();
	if (select_body_clip_seeded(p_key, p_phase_ticks)) {
		advance_body_animation(0.0);
	}
}

bool ObjectModel::select_body_clip_seeded(const String &p_key, int p_phase_ticks) {
	const String key = resolve_body_clip_key(p_key);
	if (key.is_empty()) {
		return false;
	}
	clear_body_blend();
	anim_key_ = key;
	anim_variant_ = 0;
	set_body_playhead(clip_phase_seconds(key, p_phase_ticks));
	anim_playing_ = true;
	anim_external_phase_ = false;
	body_pose_dirty_ = true;
	return true;
}

// Apply one raw compact-organic body-state request with retail's remote
// transition arbitration. A phase belongs only to an immediately accepted
// player transition; queued states promote at tick zero when the current
// clip reaches its completion boundary.
bool ObjectModel::apply_remote_body_state(int p_state_id, const String &p_key,
		int p_flags, int p_phase_ticks) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->apply_remote_body_state(
				p_state_id, p_key, p_flags, p_phase_ticks);
	}
	wake_runtime_frame();
	if (p_state_id < 0 || resolve_body_clip_key(p_key).is_empty()) {
		return remote_body_needs_fixed_tick();
	}
	if (remote_state_ < 0) {
		accept_remote_body_state(p_state_id, p_key, p_flags, p_phase_ticks);
		return remote_body_needs_fixed_tick();
	}
	if (p_state_id == remote_state_) {
		clear_remote_body_pending();
		return remote_body_needs_fixed_tick();
	}
	// The queue gate is the shared native rule (world/infantry.h
	// remote_body_state_defers, [orig: @0x4c1169..0x4c1190 / @0x4c060a..
	// 0x4c0633]) — the same predicate the netsim record fold applies.
	if (opennova::world::remote_body_state_defers(
				static_cast<uint32_t>(remote_flags_), static_cast<uint32_t>(p_flags))) {
		queue_remote_body_state(p_state_id, p_key, p_flags);
		return remote_body_needs_fixed_tick();
	}
	accept_remote_body_state(p_state_id, p_key, p_flags, p_phase_ticks);
	return remote_body_needs_fixed_tick();
}

void ObjectModel::reset_remote_body_state() {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->reset_remote_body_state();
	}
	remote_state_ = -1;
	remote_flags_ = 0;
	clear_remote_body_pending();
	clear_remote_body_blend();
}

void ObjectModel::accept_remote_body_state(int p_state_id, const String &p_key,
		int p_flags, int p_phase_ticks) {
	clear_remote_body_pending();
	const bool had_current = remote_state_ >= 0 && !anim_key_.is_empty();
	remote_state_ = p_state_id;
	remote_flags_ = p_flags;
	const int target_phase = p_phase_ticks >= 0 ? p_phase_ticks : 0;
	if (had_current) {
		start_remote_body_blend(p_key, p_flags, target_phase);
	} else if (select_body_clip_seeded(p_key, target_phase)) {
		advance_body_animation(0.0);
	}
}

void ObjectModel::queue_remote_body_state(int p_state_id, const String &p_key,
		int p_flags) {
	remote_pending_state_ = p_state_id;
	remote_pending_key_ = p_key;
	remote_pending_flags_ = p_flags;
	// Completion-boundary arming: the seconds-domain sibling of netsim's
	// tick-domain arm — same three cases, units differ because this FSM owns
	// clip TIME. A hold clip whose length cannot resolve completes IMMEDIATELY
	// (the D-NET-209 hold-wedge safety).
	const float length = skeletal_->get_clip_length(anim_key_, anim_variant_);
	remote_pending_end_valid_ = true;
	if (length <= 0.0f) {
		remote_pending_end_time_ = 0.0;
	} else if (skeletal_->is_clip_looping(anim_key_, anim_variant_)) {
		remote_pending_end_time_ =
				(Math::floor(anim_time_ / double(length)) + 1.0) * double(length);
	} else {
		remote_pending_end_time_ = double(length);
	}
	// A request arriving after a one-shot already ended promotes immediately.
	if (promote_remote_body_pending_if_due()) {
		advance_body_animation(0.0);
	}
}

void ObjectModel::clear_remote_body_pending() {
	remote_pending_state_ = -1;
	remote_pending_key_ = String();
	remote_pending_flags_ = 0;
	remote_pending_end_time_ = 0.0;
	remote_pending_end_valid_ = false;
}

void ObjectModel::clear_remote_body_blend() {
	remote_blend_active_ = false;
	remote_blend_source_key_ = String();
	remote_blend_source_phase_ticks_ = 0;
	remote_blend_source_time_ = 0.0;
	remote_blend_target_phase_ticks_ = 0;
	remote_blend_weight_ = 1.0f;
	remote_blend_step_ = 0.0f;
	clear_body_blend();
}

int ObjectModel::body_phase_ticks(const String &p_key, double p_seconds) const {
	if (skeletal_.is_null() || p_key.is_empty()) {
		return 0;
	}
	const float fps = skeletal_->get_clip_fps(p_key);
	if (fps <= 0.0f) {
		return 0;
	}
	return MAX(static_cast<int>(Math::floor(p_seconds * 2.0 * fps + 0.000001)), 0);
}

void ObjectModel::start_remote_body_blend(const String &p_target_key,
		int p_target_flags, int p_target_phase_ticks) {
	const String target_key = resolve_body_clip_key(p_target_key);
	if (target_key.is_empty()) {
		return;
	}
	// A retarget during A->B retains A and replaces only B with C.
	const String source_key =
			remote_blend_active_ ? remote_blend_source_key_ : anim_key_;
	const int source_phase = remote_blend_active_
			? remote_blend_source_phase_ticks_
			: body_phase_ticks(anim_key_, anim_time_);
	const double source_time =
			remote_blend_active_ ? remote_blend_source_time_ : anim_time_;
	if (source_key.is_empty() || skeletal_.is_null() ||
			!skeletal_->has_clip(source_key)) {
		clear_remote_body_blend();
		if (select_body_clip_seeded(target_key, p_target_phase_ticks)) {
			advance_body_animation(0.0);
		}
		return;
	}
	remote_blend_active_ = true;
	remote_blend_source_key_ = source_key;
	remote_blend_source_phase_ticks_ = source_phase;
	remote_blend_source_time_ = source_time;
	remote_blend_target_phase_ticks_ = p_target_phase_ticks;
	remote_blend_weight_ = 0.0f;
	// float32 rounding matches retail's accumulated channel weight.
	remote_blend_step_ = f32((p_target_flags & 0x400) != 0 ? 1.0 / 15.0 : 0.1);
	pose_body_blend_at_times(remote_blend_source_key_, remote_blend_source_time_,
			target_key,
			clip_phase_seconds(target_key, remote_blend_target_phase_ticks_), 0.0f);
}

// Advance one receive-side simulation tick for an already accepted target.
// Both channels advance before the float32 target weight increments.
bool ObjectModel::advance_remote_body_blend_tick(int p_state_id) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->advance_remote_body_blend_tick(p_state_id);
	}
	wake_runtime_frame();
	if (!remote_blend_active_) {
		if (remote_pending_state_ >= 0) {
			promote_remote_body_pending_if_due();
			return remote_body_needs_fixed_tick();
		}
		return false;
	}
	if (p_state_id != remote_state_ && p_state_id != remote_pending_state_) {
		return false;
	}
	remote_blend_source_phase_ticks_ += 1;
	remote_blend_source_time_ += clip_half_tick_seconds(remote_blend_source_key_);
	remote_blend_target_phase_ticks_ += 1;
	remote_blend_weight_ =
			MIN(f32(double(remote_blend_weight_) + double(remote_blend_step_)), 1.0f);
	const String target_key = anim_key_;
	if (remote_blend_weight_ >= 1.0f) {
		const int target_phase = remote_blend_target_phase_ticks_;
		clear_remote_body_blend();
		if (select_body_clip_seeded(target_key, target_phase)) {
			advance_body_animation(0.0);
		}
		return remote_body_needs_fixed_tick();
	}
	pose_body_blend_at_times(remote_blend_source_key_, remote_blend_source_time_,
			target_key,
			clip_phase_seconds(target_key, remote_blend_target_phase_ticks_),
			remote_blend_weight_);
	return remote_body_needs_fixed_tick();
}

bool ObjectModel::remote_body_needs_fixed_tick() const {
	if (remote_blend_active_ || remote_pending_state_ >= 0) {
		return true;
	}
	for (ObjectModel *linked : live_presentation_links()) {
		if (linked->remote_body_needs_fixed_tick()) {
			return true;
		}
	}
	return false;
}

bool ObjectModel::promote_remote_body_pending_if_due() {
	if (remote_pending_state_ < 0 || !remote_pending_end_valid_) {
		return false;
	}
	if (anim_time_ + 0.000001 < remote_pending_end_time_) {
		return false;
	}
	const int state_id = remote_pending_state_;
	const String key = remote_pending_key_;
	const int flags = remote_pending_flags_;
	clear_remote_body_pending();
	remote_state_ = state_id;
	remote_flags_ = flags;
	// The queued packet's phase described the old current channel. Retail
	// starts the promoted request at the first frame.
	start_remote_body_blend(key, flags, 0);
	return true;
}

void ObjectModel::stop_body_clip() {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->stop_body_clip();
	}
	wake_runtime_frame();
	anim_playing_ = false;
	anim_external_phase_ = false;
	body_phase_stamp_valid_ = false;
	clear_body_blend();
	reset_remote_body_state();
}

// Play a main-body animation by canonical AI slot (opennova::world::BodyAnim).
// Idempotent: a repeated same-slot call does not restart a playing loop.
void ObjectModel::play_body_anim(int p_slot) {
	wake_runtime_frame();
	if (skeletal_.is_null() || p_slot < 0) {
		return;
	}
	const String key = skeletal_->slot_to_key(p_slot);
	if (key.is_empty()) {
		return;
	}
	if (key == anim_key_ && !anim_external_phase_) {
		return;
	}
	play_body_clip(key);
}

// Pose a main-body animation slot at the authoritative infantry motor playhead.
void ObjectModel::play_body_anim_at(int p_slot, int p_phase_ticks) {
	wake_runtime_frame();
	if (skeletal_.is_null() || p_slot < 0) {
		return;
	}
	if (p_slot != last_slot_resolved_) {
		last_slot_key_ = skeletal_->slot_to_key(p_slot);
		last_slot_resolved_ = p_slot;
	}
	if (last_slot_key_.is_empty()) {
		return;
	}
	play_body_clip_at(last_slot_key_, p_phase_ticks);
}

// Scrub the active body clip's playhead and pose IMMEDIATELY, even while
// paused. Looping clips wrap over the clip length, one-shots clamp.
void ObjectModel::set_animation_time(double p_seconds) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->set_animation_time(p_seconds);
	}
	wake_runtime_frame();
	if (skeletal_.is_null() || anim_key_.is_empty()) {
		return;
	}
	clear_body_blend();
	anim_external_phase_ = false;
	set_body_playhead(p_seconds);
	body_pose_dirty_ = true;
	advance_body_animation(0.0);
}

void ObjectModel::set_body_playhead(double p_seconds) {
	// Any playhead write outside play_body_clip_at's own apply invalidates the
	// applied-phase stamp.
	body_phase_stamp_valid_ = false;
	const float length = skeletal_->get_clip_length(anim_key_, anim_variant_);
	if (length <= 0.0f) {
		anim_time_ = 0.0;
	} else if (skeletal_->is_clip_looping(anim_key_, anim_variant_)) {
		anim_time_ = Math::fposmod(p_seconds, double(length));
	} else {
		anim_time_ = CLAMP(p_seconds, 0.0, double(length));
	}
}

String ObjectModel::resolve_body_clip_key(const String &p_key) const {
	if (skeletal_.is_null()) {
		return String();
	}
	if (!p_key.is_empty() && skeletal_->has_clip(p_key)) {
		return p_key;
	}
	// AnimMap registration binds absent semantic state keys to state-0 RESET.
	return skeletal_->has_clip("anim_reset") ? String("anim_reset") : String();
}

double ObjectModel::clip_phase_seconds(const String &p_key, int p_phase_ticks) const {
	const float fps = skeletal_->get_clip_fps(p_key);
	return fps > 0.0f ? double(MAX(p_phase_ticks, 0)) / (2.0 * fps) : 0.0;
}

double ObjectModel::clip_half_tick_seconds(const String &p_key) const {
	const float fps = skeletal_->get_clip_fps(p_key);
	return fps > 0.0f ? 1.0 / (2.0 * double(fps)) : 0.0;
}

void ObjectModel::clear_body_blend() {
	if (!body_blend_source_key_.is_empty() || body_blend_weight_ < 1.0f) {
		body_pose_dirty_ = true;
	}
	body_blend_source_key_ = String();
	body_blend_source_time_ = 0.0;
	body_blend_weight_ = 1.0f;
}

void ObjectModel::reset_body_pose() {
	anim_key_ = String();
	anim_variant_ = 0;
	anim_time_ = 0.0;
	anim_playing_ = false;
	anim_external_phase_ = false;
	body_phase_stamp_valid_ = false;
	clear_body_blend();
	if (skeleton_ != nullptr) {
		for (int bone = 0; bone < skeleton_->get_bone_count(); ++bone) {
			skeleton_->reset_bone_pose(bone);
		}
	}
	body_pose_dirty_ = false;
}

double ObjectModel::get_animation_time() const {
	if (skeletal_.is_null() || anim_key_.is_empty()) {
		return 0.0;
	}
	const float length = skeletal_->get_clip_length(anim_key_, anim_variant_);
	if (length <= 0.0f) {
		return 0.0;
	}
	if (skeletal_->is_clip_looping(anim_key_, anim_variant_)) {
		return Math::fposmod(anim_time_, double(length));
	}
	return CLAMP(anim_time_, 0.0, double(length));
}

// --- Part-animation channels (PLAYPARTANIM mission action) -----------------
// [orig: Entity_ApplyCommand @0x43ab60 case 0x22; the per-channel publisher
//  HUD_CacheEntityDisplayInfo @ 0x4A3E18..0x4A3E38 -> VEHICLE_SPECIAL1/2]

String ObjectModel::resolve_anim_channel_register(int p_slot) const {
	if (p_slot < 0 || p_slot > 1) {
		return String();
	}
	return String(kPartAnimCtrlNames[p_slot]);
}

String ObjectModel::resolve_anim_channel_owner(int p_slot) const {
	if (p_slot < 0 || p_slot > 1) {
		return String();
	}
	return String(kPartAnimCtrlOwners[p_slot]);
}

// Play a model part animation from a local runtime controller. The authoritative
// AI path integrates the same fields in AiSystem and presents them through
// set_part_phase().
void ObjectModel::play_part_anim(int p_channel, int p_play_type, double p_time_s) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->play_part_anim(p_channel, p_play_type, p_time_s);
	}
	wake_runtime_frame();
	const int slot = p_channel - 1;
	if (slot < 0 || slot > 1) {
		return; // the original validates channel in {1,2}
	}
	if (p_play_type < -1 || p_play_type > 1) {
		return;
	}
	const String reg = resolve_anim_channel_register(slot);
	if (reg.is_empty()) {
		return;
	}
	if (p_play_type == 0) {
		part_anims_.erase(reg); // Stop: freeze the part at its current value
		return;
	}
	if (part_anims_.is_empty()) {
		part_anim_tick_accum_s_ = 0.0;
	}
	PartAnimChannel channel;
	channel.dir = p_play_type;
	channel.rate = ObjectData::part_anim_rate_for_seconds(p_time_s);
	channel.value = ctrl_values_.has(reg) ? int64_t(ctrl_values_[reg]) : 0;
	part_anims_[reg] = channel;
}

// Seed a locally controlled channel at its rest start (0 forward / max reverse)
// then play. Stop must freeze the part where it is (no reseed).
void ObjectModel::restart_part_anim(int p_channel, int p_play_type, double p_time_s) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->restart_part_anim(p_channel, p_play_type, p_time_s);
	}
	wake_runtime_frame();
	const int slot = p_channel - 1;
	if (slot < 0 || slot > 1 || p_play_type < -1 || p_play_type > 1) {
		return;
	}
	const String reg = resolve_anim_channel_register(slot);
	if (!reg.is_empty() && p_play_type != 0) {
		// Rest start: 0 forward, one full phase (world/ai.h) reversed.
		ctrl_values_[reg] = p_play_type >= 0
				? 0
				: static_cast<int64_t>(opennova::world::kPartAnimPhaseOne);
		ctrl_value_owners_.erase(reg);
		finish_ctrl_change(false);
	}
	play_part_anim(p_channel, p_play_type, p_time_s);
}

// Pose a part channel directly to the engine-computed signed dword (the
// faithful runtime path: the AI brain integrates the PLAYPARTANIM phase
// in-engine and the owner writes it to the PANM control register here).
void ObjectModel::set_part_phase(int p_channel, int64_t p_phase) {
	wake_runtime_frame();
	const int slot = p_channel - 1;
	const String reg = resolve_anim_channel_register(slot);
	const String owner = resolve_anim_channel_owner(slot);
	if (reg.is_empty() || owner.is_empty()) {
		return;
	}
	const int64_t next_phase = ctrl_dword(p_phase);
	part_anims_.erase(reg); // the engine owns this channel's phase
	set_ctrl_override(owner, reg, next_phase);
}

// Release PLAYPARTANIM's ownership of one semantic register — distinct from
// publishing zero (retail suppresses VEHICLE_SPECIAL1 altogether for
// ItemDefAttrib FastRope 0x1000 while still publishing SPECIAL2).
void ObjectModel::clear_part_phase(int p_channel) {
	wake_runtime_frame();
	const int slot = p_channel - 1;
	const String reg = resolve_anim_channel_register(slot);
	const String owner = resolve_anim_channel_owner(slot);
	if (reg.is_empty() || owner.is_empty()) {
		return;
	}
	part_anims_.erase(reg);
	clear_ctrl_override(owner, reg);
}

void ObjectModel::clear_part_anims() {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->clear_part_anims();
	}
	wake_runtime_frame();
	part_anims_.clear();
	part_anim_tick_accum_s_ = 0.0;
}

Dictionary ObjectModel::get_active_part_anims() const {
	Dictionary result;
	for (const KeyValue<String, PartAnimChannel> &kv : part_anims_) {
		Dictionary entry;
		entry["register"] = kv.key;
		entry["dir"] = kv.value.dir;
		entry["rate"] = kv.value.rate;
		entry["value"] = kv.value.value;
		result[kv.key] = entry;
	}
	return result;
}

// Advance at retail's fixed 16 ms cadence with wrapping signed-dword ADD/SUB.
// Only strict overshoot clamps and stops; landing exactly on an endpoint keeps
// the direction live for one more tick.
// [orig: Entity_UpdateSuspensionBounce @0x456740..0x4567A9]
bool ObjectModel::advance_part_anims(double p_delta) {
	if (part_anims_.is_empty() || p_delta <= 0.0) {
		return false;
	}
	part_anim_tick_accum_s_ += p_delta;
	const int tick_count = static_cast<int>(
			Math::floor((part_anim_tick_accum_s_ + 0.000000001) / kPartAnimTickS));
	if (tick_count <= 0) {
		return false;
	}
	part_anim_tick_accum_s_ -= double(tick_count) * kPartAnimTickS;
	bool changed = false;
	for (int tick = 0; tick < tick_count; ++tick) {
		if (part_anims_.is_empty()) {
			break;
		}
		// Writes straight into ctrl_values_ (NOT set_ctrl_value, which would
		// eagerly re-evaluate per channel); the enclosing apply_runtime_state
		// applies the result once, in the same frame.
		Vector<String> finished;
		for (KeyValue<String, PartAnimChannel> &kv : part_anims_) {
			const int64_t previous = kv.value.value;
			int32_t phase = static_cast<int32_t>(previous);
			const bool step_finished = opennova::world::part_anim_step(phase,
					static_cast<int32_t>(kv.value.dir), static_cast<int32_t>(kv.value.rate));
			const int64_t next_value = phase;
			kv.value.value = next_value;
			// A register entering the table is a change even when its first
			// step lands on the phase it started from.
			const bool inserted = !ctrl_values_.has(kv.key);
			ctrl_values_[kv.key] = next_value;
			ctrl_value_owners_.erase(kv.key);
			changed = changed || inserted || next_value != previous;
			if (step_finished) {
				finished.push_back(kv.key);
			}
		}
		for (const String &reg : finished) {
			part_anims_.erase(reg);
		}
	}
	if (changed) {
		ctrl_native_cache_valid_ = false;
	}
	bounds_dirty_ = bounds_dirty_ || changed;
	return changed;
}

void ObjectModel::set_weapon_channel(const String &p_key, int p_phase_ticks,
		const String &p_prev_key, int p_prev_phase_ticks, float p_blend_weight,
		int p_variant, int p_prev_variant) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->set_weapon_channel(p_key, p_phase_ticks, p_prev_key,
				p_prev_phase_ticks, p_blend_weight, p_variant, p_prev_variant);
	}
	wake_runtime_frame();
	if (p_key == wpn_key_ && p_phase_ticks == wpn_phase_ticks_ &&
			p_prev_key == wpn_prev_key_ && p_prev_phase_ticks == wpn_prev_phase_ticks_ &&
			p_blend_weight == wpn_blend_weight_ && p_variant == wpn_variant_ &&
			p_prev_variant == wpn_prev_variant_) {
		return;
	}
	wpn_key_ = p_key;
	wpn_phase_ticks_ = p_phase_ticks;
	wpn_prev_key_ = p_prev_key;
	wpn_prev_phase_ticks_ = p_prev_phase_ticks;
	wpn_blend_weight_ = p_blend_weight;
	wpn_variant_ = p_variant;
	wpn_prev_variant_ = p_prev_variant;
	body_pose_dirty_ = true;
}

Dictionary ObjectModel::get_weapon_channel() const {
	Dictionary out;
	if (wpn_key_.is_empty() && wpn_phase_ticks_ < 0) {
		return out;
	}
	out["key"] = wpn_key_;
	out["phase_ticks"] = wpn_phase_ticks_;
	out["prev_key"] = wpn_prev_key_;
	out["prev_phase_ticks"] = wpn_prev_phase_ticks_;
	out["blend_weight"] = wpn_blend_weight_;
	out["variant"] = wpn_variant_;
	out["prev_variant"] = wpn_prev_variant_;
	return out;
}

static_assert(ObjectModel::kAimOverlayClasses ==
				static_cast<int>(opennova::anim::kOverlayClassCount),
		"the model's overlay slots mirror the engine's overlay classes");

void ObjectModel::set_aim_overlay_deltas(const Basis *p_deltas) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->set_aim_overlay_deltas(p_deltas);
	}
	wake_runtime_frame();
	bool overlay_changed = !aim_overlay_valid_;
	for (int c = 0; c < kAimOverlayClasses && !overlay_changed; ++c) {
		overlay_changed = aim_overlay_deltas_[static_cast<size_t>(c)] != p_deltas[c];
	}
	bool classes_changed = false;
	if (aim_overlay_classes_.is_empty() && skeletal_.is_valid()) {
		const PackedInt32Array next_classes = skeletal_->get_overlay_classes();
		classes_changed = next_classes != aim_overlay_classes_;
		aim_overlay_classes_ = next_classes;
	}
	if (!overlay_changed && !classes_changed) {
		return;
	}
	std::copy_n(p_deltas, kAimOverlayClasses, aim_overlay_deltas_.begin());
	aim_overlay_valid_ = true;
	body_pose_dirty_ = true;
}

void ObjectModel::clear_aim_overlay() {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->clear_aim_overlay();
	}
	wake_runtime_frame();
	if (!aim_overlay_valid_) {
		return;
	}
	aim_overlay_valid_ = false;
	body_pose_dirty_ = true;
}

void ObjectModel::set_aim_overlay(const Array &p_deltas) {
	// The script form: an empty Array clears the overlay; otherwise index =
	// overlay class, and a missing or non-Basis slot reads identity (the shape
	// the Array store had before the typed table).
	if (p_deltas.is_empty()) {
		clear_aim_overlay();
		return;
	}
	Basis deltas[kAimOverlayClasses];
	for (int c = 0; c < kAimOverlayClasses; ++c) {
		deltas[c] = Basis();
		if (c < p_deltas.size() && p_deltas[c].get_type() == Variant::BASIS)
			deltas[c] = static_cast<Basis>(p_deltas[c]);
	}
	set_aim_overlay_deltas(deltas);
}

Array ObjectModel::get_aim_overlay() const {
	Array out;
	if (!aim_overlay_valid_) {
		return out;
	}
	out.resize(kAimOverlayClasses);
	for (int c = 0; c < kAimOverlayClasses; ++c) {
		out[c] = aim_overlay_deltas_[static_cast<size_t>(c)];
	}
	return out;
}

Dictionary ObjectModel::get_body_blend() const {
	Dictionary out;
	if (body_blend_source_key_.is_empty() && body_blend_weight_ >= 1.0f) {
		return out;
	}
	out["source_key"] = body_blend_source_key_;
	out["source_time"] = body_blend_source_time_;
	out["weight"] = body_blend_weight_;
	return out;
}

void ObjectModel::set_right_hand_collapsed(bool p_collapsed) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->set_right_hand_collapsed(p_collapsed);
	}
	wake_runtime_frame();
	if (p_collapsed == collapse_right_hand_) {
		return;
	}
	collapse_right_hand_ = p_collapsed;
	body_pose_dirty_ = true;
	// Re-pose immediately, like play_body_clip_at: the collapse is a mount-edge
	// presentation flag and must land on the current pose without waiting for
	// the next runtime-frame advance (models no longer self-clock — the frame
	// driver advances the awake set, and a caller may read the pose same-frame).
	if (!anim_key_.is_empty()) {
		advance_body_animation(0.0);
	}
}

// Pose the Skeleton3D from the active main-body clip: advance the playhead
// while playing, then one native pose write (the whole evaluate-and-write
// loop lives on SkeletalAnim). write_pose=false advances the clip clock
// and latches dirt without writing bones — the hidden-model leg.
void ObjectModel::advance_body_animation(double p_delta, bool p_write_pose) {
	if (skeleton_ == nullptr || skeletal_.is_null() || anim_key_.is_empty()) {
		return;
	}
	if (is_playing_ && anim_playing_ && !anim_external_phase_ && p_delta != 0.0) {
		anim_time_ += p_delta;
		body_pose_dirty_ = true;
	}
	if (promote_remote_body_pending_if_due()) {
		body_pose_dirty_ = true;
	}
	if (!p_write_pose) {
		return;
	}
	if (!body_pose_dirty_) {
		return;
	}
	const bool use_overlay =
			aim_overlay_valid_ && !aim_overlay_classes_.is_empty();
	const bool use_primary_blend =
			!body_blend_source_key_.is_empty() && body_blend_weight_ < 1.0f;
	double wpn_time = 0.0;
	double wpn_prev_time = 0.0;
	if (use_overlay && !wpn_key_.is_empty()) {
		// Weapon-channel playhead: half-frame ticks -> seconds, the
		// play_body_clip_at convention. The outgoing clip converts against ITS
		// OWN fps — the two channels keep independent playheads through the blend.
		const float wfps = skeletal_->get_clip_fps(wpn_key_, wpn_variant_);
		if (wfps > 0.0f) {
			wpn_time = double(MAX(wpn_phase_ticks_, 0)) / (2.0 * wfps);
		}
		if (!wpn_prev_key_.is_empty()) {
			const float pfps = skeletal_->get_clip_fps(wpn_prev_key_, wpn_prev_variant_);
			if (pfps > 0.0f) {
				wpn_prev_time = double(MAX(wpn_prev_phase_ticks_, 0)) / (2.0 * pfps);
			}
		}
	}
	static const PackedInt32Array empty_classes;
	const Basis *overlay_deltas = use_overlay ? aim_overlay_deltas_.data() : nullptr;
	if (use_primary_blend) {
		skeletal_->pose_skeleton_blended(skeleton_, body_blend_source_key_,
				body_blend_source_time_, anim_key_, anim_time_, body_blend_weight_,
				use_overlay ? aim_overlay_classes_ : empty_classes,
				overlay_deltas,
				use_overlay ? wpn_key_ : String(), wpn_time, collapse_right_hand_,
				use_overlay ? wpn_prev_key_ : String(), wpn_prev_time,
				wpn_blend_weight_, wpn_variant_, wpn_prev_variant_);
	} else {
		skeletal_->pose_skeleton_deltas(skeleton_, anim_key_, anim_time_, anim_variant_,
				use_overlay ? aim_overlay_classes_ : empty_classes,
				overlay_deltas,
				use_overlay ? wpn_key_ : String(), wpn_time, collapse_right_hand_,
				use_overlay ? wpn_prev_key_ : String(), wpn_prev_time,
				wpn_blend_weight_, wpn_variant_, wpn_prev_variant_);
	}
	body_pose_dirty_ = false;
}

} // namespace godot
