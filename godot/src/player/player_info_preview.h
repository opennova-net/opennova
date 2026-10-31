#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <runtime/menu/player_info_avatars.h>

namespace godot {

class AvatarComboRow;
class AvatarPartRow;
class ObjectModel;
class ResourceRoot;
class SkeletalAnim;

// The PLAYER_INFO portrait's model (engine menu::PlayerInfoPreview and
// kPlayerInfoPreviewParts carry the witnesses): its facing, hover zoom blend and
// hover, the menu updates its motion steps on, and the parts and idle it
// composes. AvatarPreview's nodes draw it; the clocks are this binding's device
// reads.
class PlayerInfoPreview : public RefCounted {
	GDCLASS(PlayerInfoPreview, RefCounted)

	opennova::menu::PlayerInfoPreview model_;

protected:
	static void _bind_methods();

public:
	// A fresh preview model, its facing drawn from the wall clock's seconds.
	void frame();
	bool is_framed() const { return model_.framed; }
	// The PLAYER_PREVIEW window's mouse-over edge (the menu pump's claim) under
	// the title's hover rule.
	void set_hovered(bool p_widget_hovered, bool p_title_hovers);
	bool is_hovered() const { return model_.hovered; }
	// `p_delta` seconds of wall clock; the menu updates run.
	int advance(double p_delta);
	float zoom_blend() const { return model_.zoom_blend; }
	int64_t yaw_bam() const { return model_.yaw_bam; }
	float camera_distance_scale(float p_rest_scale) const;
	// The combo's parts the portrait composes, in submit order (a part the
	// combo lacks is skipped).
	static TypedArray<AvatarPartRow> composed_parts(const Ref<AvatarComboRow> &p_combo);
	// The preview idle over the mount; null without the .bad pair or when it
	// fails to load (the parts then stand at rest).
	static Ref<SkeletalAnim> load_idle(const Ref<ResourceRoot> &p_root);
	// A skinned part plays the idle; a static one stands as built.
	static void pose_part(ObjectModel *p_model, const Ref<SkeletalAnim> &p_idle);
};

} // namespace godot
