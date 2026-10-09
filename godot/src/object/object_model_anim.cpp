// ObjectModel: main-body skeletal channels (.bad/.adm), the retail
// remote body-state arbitration, the muzzle userpoint, PLAYPARTANIM part
// channels, aim overlay / weapon channel, and pose evaluation.
// Ported verbatim from object_body_anim.gd (2026-08-09 de-scripting);
// the skeletal reference is typed (Ref<SkeletalAnim>) so the former
// script-double eval fallbacks are gone.

#include "object/object_model.h"
#include <base/io/tick_rate.h>

#include <godot_cpp/core/math.hpp>

#include <runtime/anim/aim_overlay.h> // kOverlayClassCount (the nine overlay classes)
#include <runtime/world/infantry.h>

#include <algorithm>
#include <limits>

namespace godot {

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
// lookup [orig: ModelGPM_FindUserpointByName @ 0x5b2170 via Entity_ResolveBoneUserpoints (ex sub_545940)]. The
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
	const opennova::threedi::Threedi3di3 &model = object_data_->native_model();
	const int best = opennova::threedi::threedi_3di3_find_user_point(
			&model, muzzle_point_name_.utf8().get_data());
	if (best < 0) {
		return;
	}
	const int bone = model.user_points[best].subobject_index;
	if (bone < 0 || bone >= skeleton_->get_bone_count()) {
		return;
	}
	muzzle_bone_ = bone;
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
void ObjectModel::play_body_clip_variant_at_tick(const String &p_key,
		int p_variant, int p_ticks) {
	if (skeletal_.is_valid()) {
		play_body_clip_variant_at_time(p_key, p_variant,
				skeletal_->get_clip_phase_seconds(p_key, p_ticks, p_variant));
	}
}

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
// AnimMap phase counts simulation ticks; clip timelines retain retail rounding.
// p_variant is the ring entry the channel's re-init served (the simulation's
// InfantryState::anim_variant): every clip read runs on it.
void ObjectModel::play_body_clip_at(const String &p_key, int p_phase_ticks, int p_variant,
		bool p_parked) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->play_body_clip_at(p_key, p_phase_ticks, p_variant, p_parked);
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
			p_phase_ticks == body_phase_ticks_applied_ && key == anim_key_ &&
			p_variant == anim_variant_ && p_parked == body_phase_parked_applied_) {
		return;
	}
	const String previous_key = anim_key_;
	const int previous_variant = anim_variant_;
	const double previous_time = anim_time_;
	const bool previous_external = anim_external_phase_;
	const double seconds = clip_phase_seconds(key, p_phase_ticks, p_variant, p_parked);
	const bool same_external = previous_external && key == previous_key &&
			p_variant == previous_variant &&
			Math::is_equal_approx(previous_time, seconds);
	anim_key_ = key;
	anim_variant_ = p_variant;
	set_body_playhead(seconds);
	anim_playing_ = false;
	anim_external_phase_ = true;
	body_phase_stamp_valid_ = true;
	body_phase_ticks_applied_ = p_phase_ticks;
	body_phase_parked_applied_ = p_parked;
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
		int p_target_phase_ticks, double p_weight, int p_source_variant,
		int p_variant) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->play_body_blend_at(p_source_key, p_source_phase_ticks,
				p_target_key, p_target_phase_ticks, p_weight, p_source_variant,
				p_variant);
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
			play_body_clip_at(source_key, p_source_phase_ticks, p_source_variant);
		} else {
			reset_body_pose();
		}
		return;
	}
	if (!source_valid || p_weight >= 1.0) {
		play_body_clip_at(target_key, p_target_phase_ticks, p_variant);
		return;
	}
	pose_body_blend_at_times(source_key,
			clip_phase_seconds(source_key, p_source_phase_ticks, p_source_variant),
			target_key, clip_phase_seconds(target_key, p_target_phase_ticks, p_variant),
			static_cast<float>(p_weight), p_source_variant, p_variant);
}

void ObjectModel::pose_body_blend_at_times(const String &p_source_key,
		double p_source_time, const String &p_target_key, double p_target_time,
		float p_weight, int p_source_variant, int p_target_variant) {
	anim_key_ = p_target_key;
	anim_variant_ = p_target_variant;
	set_body_playhead(p_target_time);
	anim_playing_ = false;
	anim_external_phase_ = true;
	body_phase_stamp_valid_ = false;
	body_blend_source_key_ = p_source_key;
	body_blend_source_time_ = p_source_time;
	body_blend_source_variant_ = p_source_variant;
	body_blend_weight_ = CLAMP(p_weight, 0.0f, 1.0f);
	body_pose_dirty_ = true;
	advance_body_animation(0.0);
}

// Seed a main-body clip from simulation ticks, pose it immediately, and
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

// Clip lookup and pose writes adapt the shared native receive/playback state.
bool ObjectModel::apply_remote_body_state(int p_state_id, const String &p_key,
		int p_flags, int p_phase_ticks) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->apply_remote_body_state(p_state_id, p_key, p_flags, p_phase_ticks);
	}
	wake_runtime_frame();
	if (p_state_id < 0 || resolve_body_clip_key(p_key).is_empty()) {
		return remote_body_needs_fixed_tick();
	}
	const bool had_current = remote_body_.current() >= 0 && !anim_key_.is_empty();
	const auto arrival = remote_body_.request(p_state_id, static_cast<uint32_t>(p_flags));
	if (arrival == opennova::anim::BodyArrival::queue) {
		queue_remote_body_clip(p_key);
	} else if (arrival == opennova::anim::BodyArrival::commit) {
		accept_remote_body_clip(p_key, p_flags, p_phase_ticks, had_current);
	}
	return remote_body_needs_fixed_tick();
}

void ObjectModel::reset_remote_body_state() {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->reset_remote_body_state();
	}
	remote_body_.reset();
	remote_pending_key_ = String();
	clear_remote_body_blend();
}

void ObjectModel::accept_remote_body_clip(const String &p_key,
		int p_flags, int p_phase_ticks, bool p_had_current) {
	remote_pending_key_ = String();
	const int target_phase = p_phase_ticks >= 0 ? p_phase_ticks : 0;
	if (p_had_current) {
		start_remote_body_blend(p_key, p_flags, target_phase);
	} else if (select_body_clip_seeded(p_key, target_phase)) {
		advance_body_animation(0.0);
	}
}

void ObjectModel::queue_remote_body_clip(const String &p_key) {
	remote_pending_key_ = p_key;
	remote_body_.arm_completion(anim_time_,
			skeletal_->get_clip_length(anim_key_, anim_variant_),
			skeletal_->is_clip_looping(anim_key_, anim_variant_));
	if (promote_remote_body_pending_if_due()) advance_body_animation(0.0);
}

void ObjectModel::clear_remote_body_blend() {
	remote_body_.clear_blend();
	remote_blend_source_key_ = String();
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
	return MAX(static_cast<int>(Math::floor(p_seconds * opennova::io::kTicksPerSecondInt + 0.5)), 0);
}

void ObjectModel::start_remote_body_blend(const String &p_target_key,
		int p_target_flags, int p_target_phase_ticks) {
	const String target_key = resolve_body_clip_key(p_target_key);
	if (target_key.is_empty()) return;
	const String source_key = remote_body_.blending() ? remote_blend_source_key_ : anim_key_;
	if (source_key.is_empty() || skeletal_.is_null() || !skeletal_->has_clip(source_key)) {
		clear_remote_body_blend();
		if (select_body_clip_seeded(target_key, p_target_phase_ticks)) {
			advance_body_animation(0.0);
		}
		return;
	}
	remote_blend_source_key_ = source_key;
	remote_body_.begin_blend(body_phase_ticks(anim_key_, anim_time_), anim_time_,
			p_target_phase_ticks, static_cast<uint32_t>(p_target_flags));
	pose_body_blend_at_times(remote_blend_source_key_, remote_body_.source_time(),
			target_key, clip_phase_seconds(target_key, remote_body_.target_phase()),
			remote_body_.weight());
}

bool ObjectModel::advance_remote_body_blend_tick(int p_state_id) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->advance_remote_body_blend_tick(p_state_id);
	}
	wake_runtime_frame();
	if (!remote_body_.blending()) {
		if (remote_body_.has_pending()) {
			promote_remote_body_pending_if_due();
			return remote_body_needs_fixed_tick();
		}
		return false;
	}
	if (!remote_body_.advance_blend(p_state_id)) return false;
	remote_body_.set_source_time(clip_phase_seconds(
			remote_blend_source_key_, remote_body_.source_phase()));
	const String target_key = anim_key_;
	if (remote_body_.weight() >= 1.0f) {
		const int target_phase = remote_body_.target_phase();
		clear_remote_body_blend();
		if (select_body_clip_seeded(target_key, target_phase)) {
			advance_body_animation(0.0);
		}
		return remote_body_needs_fixed_tick();
	}
	pose_body_blend_at_times(remote_blend_source_key_, remote_body_.source_time(),
			target_key, clip_phase_seconds(target_key, remote_body_.target_phase()),
			remote_body_.weight());
	return remote_body_needs_fixed_tick();
}

bool ObjectModel::remote_body_needs_fixed_tick() const {
	if (remote_body_.needs_tick()) return true;
	for (ObjectModel *linked : live_presentation_links()) {
		if (linked->remote_body_needs_fixed_tick()) return true;
	}
	return false;
}

bool ObjectModel::promote_remote_body_pending_if_due() {
	if (!remote_body_.promote_if_due(anim_time_)) return false;
	const String key = remote_pending_key_;
	remote_pending_key_ = String();
	start_remote_body_blend(key, static_cast<int>(remote_body_.flags()), 0);
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
void ObjectModel::play_body_anim_at(int p_slot, int p_phase_ticks, bool p_parked) {
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
	play_body_clip_at(last_slot_key_, p_phase_ticks, 0, p_parked);
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

double ObjectModel::clip_phase_seconds(const String &p_key, int p_phase_ticks,
		int p_variant, bool p_parked) const {
	// A parked tick samples the clip's last frame at its boundary
	// (anim::ClipTimeline::seconds_at(ticks, armed_boundary)).
	return skeletal_->get_clip_phase_seconds(p_key, p_phase_ticks, p_variant,
			p_parked ? p_phase_ticks : -1);
}

void ObjectModel::clear_body_blend() {
	if (!body_blend_source_key_.is_empty() || body_blend_weight_ < 1.0f) {
		body_pose_dirty_ = true;
	}
	body_blend_source_key_ = String();
	body_blend_source_time_ = 0.0;
	body_blend_weight_ = 1.0f;
	body_blend_source_variant_ = 0;
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

// --- Part-animation channel device application ----------------------------

void ObjectModel::play_part_anim(int p_channel, int p_play_type, double p_time_s) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->play_part_anim(p_channel, p_play_type, p_time_s);
	}
	wake_runtime_frame();
	controls_.play_part(p_channel, p_play_type, p_time_s);
}

void ObjectModel::restart_part_anim(int p_channel, int p_play_type, double p_time_s) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->restart_part_anim(p_channel, p_play_type, p_time_s);
	}
	wake_runtime_frame();
	using Restart = opennova::renderer::ModelControls::Restart;
	const Restart result = controls_.restart_part(p_channel, p_play_type, p_time_s);
	if (result == Restart::Invalid) return;
	if (result == Restart::Seeded) finish_ctrl_change(false);
	// Keep the linked-node play notification after their restart notification.
	play_part_anim(p_channel, p_play_type, p_time_s);
}

void ObjectModel::set_part_phase(int p_channel, int64_t p_phase) {
	wake_runtime_frame();
	const int ordinal = opennova::renderer::ModelControls::part_register(p_channel);
	if (ordinal < 0) return;
	static const std::string owners[] = {
		opennova::renderer::ModelControls::part_owner(1),
		opennova::renderer::ModelControls::part_owner(2)};
	controls_.release_part(p_channel);
	set_ctrl_override_native(owners[p_channel - 1], ordinal, p_phase);
}

void ObjectModel::clear_part_phase(int p_channel) {
	wake_runtime_frame();
	const int ordinal = opennova::renderer::ModelControls::part_register(p_channel);
	if (ordinal < 0) return;
	static const std::string owners[] = {
		opennova::renderer::ModelControls::part_owner(1),
		opennova::renderer::ModelControls::part_owner(2)};
	controls_.release_part(p_channel);
	clear_ctrl_override_native(owners[p_channel - 1], ordinal);
}

void ObjectModel::clear_part_anims() {
	for (ObjectModel *linked : live_presentation_links()) linked->clear_part_anims();
	wake_runtime_frame();
	controls_.clear_parts();
}

PackedStringArray ObjectModel::get_active_part_anim_registers() const {
	PackedStringArray result;
	for (int ordinal : controls_.active_part_registers()) {
		result.push_back(String(opennova::threedi::threedi_ctrl_register_name(static_cast<size_t>(ordinal))));
	}
	return result;
}

bool ObjectModel::advance_part_anims(double p_delta) {
	const bool changed = controls_.advance_parts(p_delta);
	bounds_dirty_ = bounds_dirty_ || changed;
	return changed;
}

void ObjectModel::set_weapon_channel(const String &p_key, int p_phase_ticks,
		const String &p_prev_key, int p_prev_phase_ticks, float p_blend_weight,
		int p_variant, int p_prev_variant, bool p_parked) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->set_weapon_channel(p_key, p_phase_ticks, p_prev_key,
				p_prev_phase_ticks, p_blend_weight, p_variant, p_prev_variant, p_parked);
	}
	wake_runtime_frame();
	if (p_key == wpn_key_ && p_phase_ticks == wpn_phase_ticks_ &&
			p_prev_key == wpn_prev_key_ && p_prev_phase_ticks == wpn_prev_phase_ticks_ &&
			p_blend_weight == wpn_blend_weight_ && p_variant == wpn_variant_ &&
			p_prev_variant == wpn_prev_variant_ && p_parked == wpn_parked_) {
		return;
	}
	wpn_parked_ = p_parked;
	wpn_key_ = p_key;
	wpn_phase_ticks_ = p_phase_ticks;
	wpn_prev_key_ = p_prev_key;
	wpn_prev_phase_ticks_ = p_prev_phase_ticks;
	wpn_blend_weight_ = p_blend_weight;
	wpn_variant_ = p_variant;
	wpn_prev_variant_ = p_prev_variant;
	body_pose_dirty_ = true;
}

bool ObjectModel::has_weapon_channel() const {
	return !(wpn_key_.is_empty() && wpn_phase_ticks_ < 0);
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

bool ObjectModel::has_body_blend() const {
	return !(body_blend_source_key_.is_empty() && body_blend_weight_ >= 1.0f);
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
		wpn_time = skeletal_->get_clip_phase_seconds(wpn_key_, wpn_phase_ticks_, wpn_variant_,
				wpn_parked_ ? wpn_phase_ticks_ : -1);
		if (!wpn_prev_key_.is_empty())
			wpn_prev_time = skeletal_->get_clip_phase_seconds(
					wpn_prev_key_, wpn_prev_phase_ticks_, wpn_prev_variant_);
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
				wpn_blend_weight_, wpn_variant_, wpn_prev_variant_,
				body_blend_source_variant_, anim_variant_);
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
