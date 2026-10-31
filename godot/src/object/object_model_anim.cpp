// ObjectModel: the device half of the main-body channel (the engine anim::BodyChannel owns the
// clip, its playhead, the blends, the remote arbitration, the weapon channel, the aim overlay, the
// override and the pose they compose: runtime/anim/body_channel.h), the muzzle userpoint, the
// PLAYPARTANIM part channels' device application, and the composed pose written to the Skeleton3D.
// Each verb fans out over the linked parts, runs on this model's channel, and lands what the
// channel asked of the device (land_body_request).

#include "object/object_model.h"

#include "util/string_convert.h"

#include <godot_cpp/core/math.hpp>

#include <runtime/anim/aim_overlay.h> // kOverlayClassCount (the nine overlay classes)
#include <runtime/world/entity_pose.h> // posed_bone_frames (the attach frames)

#include <algorithm>
#include <limits>

namespace godot {

void PanmClock::_bind_methods() {
	ClassDB::bind_method(D_METHOD("sample", "value_ms", "frame"), &PanmClock::sample);
	ClassDB::bind_method(D_METHOD("get_time_ms"), &PanmClock::get_time_ms);
	ClassDB::bind_method(D_METHOD("set_time_ms_for_test", "value_ms"),
			&PanmClock::set_time_ms_for_test);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "time_ms", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_NONE),
			"", "get_time_ms");
}

bool PanmClock::sample(int64_t p_value_ms, int64_t p_frame) {
	if (p_frame == sampled_frame_) {
		return false;
	}
	sampled_frame_ = p_frame;
	time_ms_ = p_value_ms & 0xffffffff;
	return true;
}

void PanmClock::set_time_ms_for_test(int64_t p_value_ms) {
	time_ms_ = p_value_ms & 0xffffffff;
	sampled_frame_ = -1;
}

void ObjectModel::set_viewmodel_rig(bool p_viewmodel) {
	if (viewmodel_rig_ == p_viewmodel) {
		return;
	}
	viewmodel_rig_ = p_viewmodel;
	body_.set_person(!p_viewmodel);
	wake_runtime_frame();
}

void ObjectModel::set_skeletal_anim(const Ref<SkeletalAnim> &p_skeletal) {
	wake_runtime_frame();
	skeletal_ = p_skeletal;
	reset_remote_body_state();
	body_.bind(skeletal_.is_valid() ? skeletal_->get_rig_handle() : opennova::assets::SkeletalRig());
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
// lookup, the engine's threedi_3di3_find_user_point (which carries the
// witness, reached through Entity_ResolveBoneUserpoints). The
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

// --- the engine's bone frames --------------------------------------------------

opennova::anim::SkeletalClips::RestTransform ObjectModel::to_pose_transform(
		const Transform3D &p_transform) {
	opennova::anim::SkeletalClips::RestTransform out;
	for (int r = 0; r < 3; ++r) {
		out.rows[r * 3 + 0] = static_cast<float>(p_transform.basis.rows[r].x);
		out.rows[r * 3 + 1] = static_cast<float>(p_transform.basis.rows[r].y);
		out.rows[r * 3 + 2] = static_cast<float>(p_transform.basis.rows[r].z);
	}
	out.origin = {static_cast<float>(p_transform.origin.x), static_cast<float>(p_transform.origin.y),
			static_cast<float>(p_transform.origin.z)};
	return out;
}

Transform3D ObjectModel::from_pose_transform(const opennova::anim::SkeletalClips::RestTransform &p_transform) {
	const float *r = p_transform.rows;
	Basis basis;
	basis.rows[0] = Vector3(r[0], r[1], r[2]);
	basis.rows[1] = Vector3(r[3], r[4], r[5]);
	basis.rows[2] = Vector3(r[6], r[7], r[8]);
	return Transform3D(basis, Vector3(p_transform.origin.x, p_transform.origin.y, p_transform.origin.z));
}

bool ObjectModel::get_posed_bone_frames(int p_bone, const Transform3D &p_model_world,
		opennova::world::PosedBoneFrames &r_frames) const {
	if (p_bone < 0) {
		return false;
	}
	// The channel's rig and pose (the device skeleton is not needed: the frames are the
	// engine's); a model nothing plays on stands at its rest rig.
	static const std::vector<opennova::anim::PoseBone> kRest;
	const opennova::anim::SkeletalClips *rig = body_.rig();
	const std::vector<opennova::anim::PoseBone> *pose = &body_.pose();
	if (rig == nullptr) {
		if (rest_rig_.is_null() || !rest_rig_->is_loaded()) {
			return false;
		}
		rig = &rest_rig_->native_rig();
		pose = &kRest;
	}
	return opennova::world::posed_bone_frames(*rig, *pose, to_pose_transform(p_model_world), p_bone,
			r_frames);
}

// --- the body channel's verbs -------------------------------------------------

void ObjectModel::land_body_request() {
	switch (body_.take_pose_request()) {
		case opennova::anim::BodyChannel::PoseRequest::kNone:
			return;
		case opennova::anim::BodyChannel::PoseRequest::kPose:
			advance_body_animation(0.0);
			return;
		case opennova::anim::BodyChannel::PoseRequest::kRest:
			if (skeleton_ != nullptr) {
				for (int bone = 0; bone < skeleton_->get_bone_count(); ++bone) {
					skeleton_->reset_bone_pose(bone);
				}
			}
			return;
	}
}

void ObjectModel::play_body_clip(const String &p_key) {
	play_body_clip_native(opennova::to_std(p_key));
}

void ObjectModel::play_body_clip_native(const std::string &p_key) {
	wake_runtime_frame();
	play_body_clip_variant_native(p_key, 0);
}

void ObjectModel::play_body_clip_variant(const String &p_key, int p_variant) {
	play_body_clip_variant_native(opennova::to_std(p_key), p_variant);
}

// A same-key VARIANT (multi-clip .adm rows): the FSM owner's ring serves the index and playback
// follows that latch until the next play (engine: runtime/world/player_weapon.h).
void ObjectModel::play_body_clip_variant_native(const std::string &p_key, int p_variant) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->play_body_clip_variant_native(p_key, p_variant);
	}
	wake_runtime_frame();
	body_.play(p_key, p_variant);
	land_body_request();
}

// Pose a selected clip variant at an authoritative time; the frame clock never advances the
// playhead afterwards.
void ObjectModel::play_body_clip_variant_at_tick(const String &p_key,
		int p_variant, int p_ticks) {
	if (skeletal_.is_null()) {
		return;
	}
	const std::string key = opennova::to_std(p_key);
	play_body_clip_variant_at_time_native(key, p_variant,
			body_.clip_phase_seconds(key, p_ticks, p_variant));
}

void ObjectModel::play_body_clip_variant_at_time(const String &p_key,
		int p_variant, double p_seconds) {
	play_body_clip_variant_at_time_native(opennova::to_std(p_key), p_variant, p_seconds);
}

void ObjectModel::play_body_clip_variant_at_time_native(const std::string &p_key,
		int p_variant, double p_seconds) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->play_body_clip_variant_at_time_native(p_key, p_variant, p_seconds);
	}
	wake_runtime_frame();
	body_.play_at_time(p_key, p_variant, p_seconds);
	land_body_request();
}

void ObjectModel::play_body_clip_at(const String &p_key, int p_phase_ticks, int p_variant,
		bool p_parked) {
	play_body_clip_at_native(opennova::to_std(p_key), p_phase_ticks, p_variant, p_parked);
}

void ObjectModel::play_body_clip_at_native(const std::string &p_key, int p_phase_ticks,
		int p_variant, bool p_parked) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->play_body_clip_at_native(p_key, p_phase_ticks, p_variant, p_parked);
	}
	wake_runtime_frame();
	body_.play_at(p_key, p_phase_ticks, p_variant, p_parked);
	land_body_request();
}

void ObjectModel::play_body_blend_at(const String &p_source_key,
		int p_source_phase_ticks, const String &p_target_key,
		int p_target_phase_ticks, double p_weight, int p_source_variant,
		int p_variant) {
	play_body_blend_at_native(opennova::to_std(p_source_key), p_source_phase_ticks,
			opennova::to_std(p_target_key), p_target_phase_ticks, p_weight, p_source_variant,
			p_variant);
}

void ObjectModel::play_body_blend_at_native(const std::string &p_source_key,
		int p_source_phase_ticks, const std::string &p_target_key,
		int p_target_phase_ticks, double p_weight, int p_source_variant,
		int p_variant) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->play_body_blend_at_native(p_source_key, p_source_phase_ticks,
				p_target_key, p_target_phase_ticks, p_weight, p_source_variant,
				p_variant);
	}
	wake_runtime_frame();
	body_.play_blend_at(p_source_key, p_source_phase_ticks, p_target_key, p_target_phase_ticks,
			p_weight, p_source_variant, p_variant);
	land_body_request();
}

void ObjectModel::play_body_clip_seeded(const String &p_key, int p_phase_ticks) {
	const std::string key = opennova::to_std(p_key);
	for (ObjectModel *linked : live_presentation_links()) {
		linked->play_body_clip_seeded(p_key, p_phase_ticks);
	}
	wake_runtime_frame();
	body_.play_seeded(key, p_phase_ticks);
	land_body_request();
}

bool ObjectModel::apply_remote_body_state(int p_state_id, const String &p_key,
		int p_flags, int p_phase_ticks) {
	return apply_remote_body_state_native(p_state_id, opennova::to_std(p_key),
			static_cast<uint32_t>(p_flags), p_phase_ticks);
}

bool ObjectModel::apply_remote_body_state_native(int p_state_id, const std::string &p_key,
		uint32_t p_flags, int p_phase_ticks) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->apply_remote_body_state_native(p_state_id, p_key, p_flags, p_phase_ticks);
	}
	wake_runtime_frame();
	body_.remote_request(p_state_id, p_key, p_flags, p_phase_ticks);
	land_body_request();
	return remote_body_needs_fixed_tick();
}

void ObjectModel::reset_remote_body_state() {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->reset_remote_body_state();
	}
	body_.reset_remote();
}

bool ObjectModel::advance_remote_body_blend_tick(int p_state_id) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->advance_remote_body_blend_tick(p_state_id);
	}
	wake_runtime_frame();
	const bool ran = body_.remote_tick(p_state_id);
	land_body_request();
	return ran && remote_body_needs_fixed_tick();
}

bool ObjectModel::remote_body_needs_fixed_tick() const {
	if (body_.remote_needs_tick()) return true;
	for (ObjectModel *linked : live_presentation_links()) {
		if (linked->remote_body_needs_fixed_tick()) return true;
	}
	return false;
}

void ObjectModel::stop_body_clip() {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->stop_body_clip();
	}
	wake_runtime_frame();
	body_.stop();
	land_body_request();
}

// A main-body clip by canonical AI slot (opennova::world::BodyAnim). Idempotent: a repeated
// same-slot call does not restart a playing loop.
void ObjectModel::play_body_anim(int p_slot) {
	wake_runtime_frame();
	std::string key;
	if (body_.slot_play_key(p_slot, key)) {
		play_body_clip_native(key);
	}
}

// A main-body slot at the authoritative infantry motor playhead.
void ObjectModel::play_body_anim_at(int p_slot, int p_phase_ticks, bool p_parked) {
	wake_runtime_frame();
	const std::string &key = body_.slot_key(p_slot);
	if (key.empty()) {
		return;
	}
	play_body_clip_at_native(key, p_phase_ticks, 0, p_parked);
}

// Scrub the active body clip's playhead and pose IMMEDIATELY, even while
// paused. Looping clips wrap over the clip length, one-shots clamp.
void ObjectModel::set_animation_time(double p_seconds) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->set_animation_time(p_seconds);
	}
	wake_runtime_frame();
	body_.set_time(p_seconds);
	land_body_request();
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
	opennova::anim::WeaponChannel channel;
	channel.key = opennova::to_std(p_key);
	channel.phase_ticks = p_phase_ticks;
	channel.prev_key = opennova::to_std(p_prev_key);
	channel.prev_phase_ticks = p_prev_phase_ticks;
	channel.blend_weight = p_blend_weight;
	channel.variant = p_variant;
	channel.prev_variant = p_prev_variant;
	channel.parked = p_parked;
	set_weapon_channel_native(channel);
}

void ObjectModel::set_weapon_channel_native(const opennova::anim::WeaponChannel &p_channel) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->set_weapon_channel_native(p_channel);
	}
	wake_runtime_frame();
	body_.set_weapon_channel(p_channel);
}

static_assert(ObjectModel::kAimOverlayClasses ==
				static_cast<int>(opennova::anim::kOverlayClassCount),
		"the model's overlay slots mirror the engine's overlay classes");

void ObjectModel::set_aim_overlay_quats(const opennova::anim::Quat *p_deltas) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->set_aim_overlay_quats(p_deltas);
	}
	wake_runtime_frame();
	body_.set_aim_overlay(p_deltas);
}

void ObjectModel::clear_aim_overlay() {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->clear_aim_overlay();
	}
	wake_runtime_frame();
	body_.clear_aim_overlay();
}

void ObjectModel::set_aim_overlay(const Array &p_deltas) {
	// The script form: an empty Array clears the overlay; otherwise index =
	// overlay class, and a missing or non-Basis slot reads identity.
	if (p_deltas.is_empty()) {
		clear_aim_overlay();
		return;
	}
	opennova::anim::Quat deltas[kAimOverlayClasses];
	for (int c = 0; c < kAimOverlayClasses; ++c) {
		if (c < p_deltas.size() && p_deltas[c].get_type() == Variant::BASIS) {
			const Quaternion q = static_cast<Basis>(p_deltas[c]).get_rotation_quaternion();
			deltas[c] = {static_cast<float>(q.w), static_cast<float>(q.x), static_cast<float>(q.y),
					static_cast<float>(q.z)};
		}
	}
	set_aim_overlay_quats(deltas);
}

Array ObjectModel::get_aim_overlay() const {
	Array out;
	if (!body_.aim_overlay_valid()) {
		return out;
	}
	out.resize(kAimOverlayClasses);
	for (int c = 0; c < kAimOverlayClasses; ++c) {
		const opennova::anim::Quat &q = body_.aim_overlay()[static_cast<size_t>(c)];
		out[c] = Basis(Quaternion(q.x, q.y, q.z, q.w));
	}
	return out;
}

// Both collapse inputs fold their bones in the skin at once, like play_body_clip_at re-poses:
// a mount edge or a cut must land on the current pose without waiting for the next
// runtime-frame advance (models no longer self-clock — the frame driver advances the awake
// set, and a caller may read the skin same-frame); the frame pass keeps them folded as the
// pose moves (apply_skeletal_binds).
void ObjectModel::set_right_hand_collapsed(bool p_collapsed) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->set_right_hand_collapsed(p_collapsed);
	}
	wake_runtime_frame();
	if (p_collapsed == collapse_right_hand_) {
		return;
	}
	collapse_right_hand_ = p_collapsed;
	land_collapsed_bones();
}

// The fold lands on the current pose: the body re-poses first, as a mount edge always has,
// so an overlay or clip not yet advanced is current before its bones fold.
void ObjectModel::land_collapsed_bones() {
	body_.mark_dirty();
	if (body_.active()) {
		advance_body_animation(0.0);
	}
	if (apply_skeletal_binds()) {
		publish_skin_palette();
	}
}

void ObjectModel::set_cut_sections(int64_t p_mask) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->set_cut_sections(p_mask);
	}
	wake_runtime_frame();
	const uint32_t mask = static_cast<uint32_t>(p_mask);
	if (mask == cut_sections_) {
		return;
	}
	cut_sections_ = mask;
	land_collapsed_bones();
}

void ObjectModel::set_body_pose_override(const std::vector<opennova::anim::PoseBone> &p_pose,
		float p_weight) {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->set_body_pose_override(p_pose, p_weight);
	}
	wake_runtime_frame();
	body_.set_override(p_pose, p_weight);
	land_body_request();
}

void ObjectModel::clear_body_pose_override() {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->clear_body_pose_override();
	}
	if (!body_.clear_override()) {
		return;
	}
	wake_runtime_frame();
	land_body_request();
}

// One frame of the body channel on the engine clock's elapsed seconds (the frame driver's,
// world::FrameTickElapsed; 0 while this model is paused), then the channel's composed pose
// written to the Skeleton3D, the device write alone. write_pose false advances the clock and
// latches the dirt without writing bones: the hidden-model leg.
void ObjectModel::advance_body_animation(double p_delta, bool p_write_pose) {
	if (skeleton_ == nullptr || !body_.active()) {
		return;
	}
	body_.advance(is_playing_ ? p_delta : 0.0);
	// The advance's own promotion poses below, with the clock's move.
	(void)body_.take_pose_request();
	if (!p_write_pose || !body_.dirty()) {
		return;
	}
	if (body_.compose()) {
		skeletal_->write_native_pose(skeleton_, body_.pose());
	}
}

} // namespace godot
