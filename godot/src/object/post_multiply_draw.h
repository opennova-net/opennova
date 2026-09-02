#pragma once

// The auxiliary postmultiply draw of one retained surface slot: retail's
// multi-pass effects submit the same strip geometry again under the
// postmultiply material (ObjectModel::SurfaceSlot::auxiliary). It is its own
// node class so every walker that must skip or classify the pair (the
// model's layer restamp, SlotCaptureAdapter's geometry collector, the GUT
// slot census) asks the node type instead of a node-metadata flag.

#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {

class PostMultiplyDraw : public MeshInstance3D {
	GDCLASS(PostMultiplyDraw, MeshInstance3D)

protected:
	static void _bind_methods() {}
};

} // namespace godot
