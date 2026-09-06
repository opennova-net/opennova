#pragma once

#include <godot_cpp/classes/animation.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include "model/clip_document.h"
#include "model/clip_set_source.h"

namespace godot {

// Samples an Animation over an authored rig into a .bad clip: the inverse of
// the runtime's clip load (SkeletalAnim over anim_sample.h). The rig is the
// scene's Skeleton3D with BN## bones (the model rows, ADR 0046); the clip's
// channels carry each row's model-space rotation relative to its rest, one
// key per frame tick plus the terminal key, against an identity bind (our
// reset clips hold the rest pose, so channel-at-reset is the identity the
// stored bind must invert to). Bone positions are the presentation-frame
// parent-relative pivots (what positions_from_model reconstructs under an
// identity bind), and a bone that leaves the pivots' forward kinematics gets
// a per-frame translation (flags bit 1). Events carry the spec's root motion
// per frame, the capsule heights above the ground proxy, and the triggers.
class ClipProjector : public RefCounted {
	GDCLASS(ClipProjector, RefCounted)

	String last_error_;

protected:
	static void _bind_methods();

public:
	// `p_model_root` is the instantiated authoring scene (its first Skeleton3D
	// is the rig); `p_animation` may be null when the spec holds the rest pose.
	// `p_ground_bone` is the bone the capsule heights are measured from
	// (negative = the last row). Null with get_last_error() set on failure.
	Ref<ClipDocument> project(Node3D *p_model_root, const Ref<Animation> &p_animation, const Ref<ClipSpec> &p_spec,
			int p_fps, int p_ground_bone);
	String get_last_error() const { return last_error_; }
};

} // namespace godot
