#include "player/player_info_preview.h"

#include "object/avatar_records.h"
#include "object/object_data.h"
#include "object/object_model.h"
#include "object/skeletal_anim.h"
#include "resource_index/resource_root.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <formats/avatars/preview_animation.h>
#include <runtime/world/body_anim.h>

#include <ctime>

using namespace godot;

void PlayerInfoPreview::frame() {
	// The clock word the reseed takes (the CRT _time64's seconds, a platform primitive).
	model_.frame(static_cast<uint32_t>(std::time(nullptr)));
}

void PlayerInfoPreview::set_hovered(bool p_widget_hovered, bool p_title_hovers) {
	model_.set_hovered(p_widget_hovered, p_title_hovers);
}

int PlayerInfoPreview::advance(double p_delta) {
	// GetTickCount's word: the low 32 bits of the millisecond clock.
	return model_.advance(p_delta, static_cast<uint32_t>(Time::get_singleton()->get_ticks_msec()));
}

float PlayerInfoPreview::camera_distance_scale(float p_rest_scale) const {
	return model_.camera_distance_scale(p_rest_scale);
}

TypedArray<AvatarPartRow> PlayerInfoPreview::composed_parts(const Ref<AvatarComboRow> &p_combo) {
	TypedArray<AvatarPartRow> out;
	if (p_combo.is_null()) {
		return out;
	}
	for (const opennova::avatars::AvatarPartKind kind : opennova::menu::kPlayerInfoPreviewParts) {
		const Ref<AvatarPartRow> part =
				kind == opennova::avatars::AVATAR_PART_HEAD ? p_combo->get_head() : p_combo->get_body();
		if (part.is_valid()) {
			out.push_back(part);
		}
	}
	return out;
}

Ref<SkeletalAnim> PlayerInfoPreview::load_idle(const Ref<ResourceRoot> &p_root) {
	const String skeleton(opennova::avatars::kPreviewSkeletonBad);
	const String idle(opennova::avatars::kPreviewIdleBad);
	if (p_root.is_null() || !p_root->has_file(skeleton) || !p_root->has_file(idle)) {
		return Ref<SkeletalAnim>();
	}
	Ref<SkeletalAnim> skeletal;
	skeletal.instantiate();
	Dictionary clips;
	clips[String(opennova::world::body_anim_adm_key(opennova::world::kBodyAnimIdle))] = idle;
	return skeletal->load_from_bad_files(p_root, skeleton, clips) ? skeletal : Ref<SkeletalAnim>();
}

void PlayerInfoPreview::pose_part(ObjectModel *p_model, const Ref<SkeletalAnim> &p_idle) {
	const Ref<ObjectData> data = p_model != nullptr ? p_model->get_object_data() : Ref<ObjectData>();
	if (p_idle.is_null() || data.is_null() || !data->is_skinned(0)) {
		return;
	}
	p_model->set_skeletal_anim(p_idle);
	p_model->play_body_clip(String(opennova::world::body_anim_adm_key(opennova::world::kBodyAnimIdle)));
}

void PlayerInfoPreview::_bind_methods() {
	ClassDB::bind_method(D_METHOD("frame"), &PlayerInfoPreview::frame);
	ClassDB::bind_method(D_METHOD("is_framed"), &PlayerInfoPreview::is_framed);
	ClassDB::bind_method(D_METHOD("set_hovered", "widget_hovered", "title_hovers"),
			&PlayerInfoPreview::set_hovered);
	ClassDB::bind_method(D_METHOD("is_hovered"), &PlayerInfoPreview::is_hovered);
	ClassDB::bind_method(D_METHOD("advance", "delta"), &PlayerInfoPreview::advance);
	ClassDB::bind_method(D_METHOD("zoom_blend"), &PlayerInfoPreview::zoom_blend);
	ClassDB::bind_method(D_METHOD("yaw_bam"), &PlayerInfoPreview::yaw_bam);
	ClassDB::bind_method(D_METHOD("camera_distance_scale", "rest_scale"),
			&PlayerInfoPreview::camera_distance_scale);
	ClassDB::bind_static_method("PlayerInfoPreview", D_METHOD("composed_parts", "combo"),
			&PlayerInfoPreview::composed_parts);
	ClassDB::bind_static_method("PlayerInfoPreview", D_METHOD("load_idle", "resource_root"),
			&PlayerInfoPreview::load_idle);
	ClassDB::bind_static_method("PlayerInfoPreview", D_METHOD("pose_part", "model", "idle"),
			&PlayerInfoPreview::pose_part);
}
