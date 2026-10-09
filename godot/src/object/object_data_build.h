#pragma once

#include <godot_cpp/classes/ref.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "object/object_data.h"

namespace godot {

// The units that make one model's data ready for its scene, a unit a step: each material's stages (the
// texture of each stage slot the material holds, as ObjectData::load_material_stage_texture resolves it for
// ObjectModel::create_material) and flipbook frames (as ObjectModel::collect_anim_frames loads them), then
// each level's meshes, skinned for `bone_count` bones of a rig where `skinned`, as the scene asks them of
// the data: decoded here, so the scene's loads hit the data's caches. A device that builds a large model
// over several frames steps it before it hands the data to an ObjectModel (the editor's model and
// definition previews, on the editor trunk, PR #665).
class ObjectDataBuild {
public:
	ObjectDataBuild(Ref<ObjectData> data, bool skinned, int bone_count);
	const Ref<ObjectData> &data() const { return data_; }
	size_t total() const { return units_.size(); }
	size_t done() const { return next_; }
	bool finished() const { return next_ >= units_.size(); }
	// One unit (nothing once finished).
	void step();
	// What the next unit makes: "textures" or "meshes" ("" finished).
	const char *label() const;

private:
	struct Unit {
		enum class Kind : uint8_t { Texture, Frame, Meshes };
		Kind kind = Kind::Texture;
		int material = -1;
		int slot = 0;
		int frame = 0;
		int lod = -1;
	};
	Ref<ObjectData> data_;
	bool skinned_ = false;
	int bone_count_ = 0;
	std::vector<Unit> units_;
	size_t next_ = 0;
};

} // namespace godot
