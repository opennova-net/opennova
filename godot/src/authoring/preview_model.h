#pragma once

#include <godot_cpp/classes/node3d.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <editor/preview/viewport_follow.h>
#include <editor/session/operation_progress.h>
#include <runtime/anim/skeletal_clips.h>
#include <runtime/assets/asset_store.h>
#include <runtime/world/infantry.h>

#include "object/object_data.h"
#include "object/object_model.h"
#include "object/skeletal_anim.h"

namespace godot {

// A model a preview device draws (ADR 0046 DI-21, the definition device's): the runtime's own ObjectModel
// (its meshes, materials, CTRL registers, part animations, destroyed sections, body clips on a rig), every
// level kept (the presenter-driven mode, the mission placer's: a level the camera's distance picks swaps its
// rows in place), its part animations and material generators on a PanmClock of its own, its textures read
// through the project's files (each read noted with its stamp). It builds over several frames a unit at a time,
// as the model device does (S13 V6, ModelViewportApplier): each material's textures (a stage's first texture
// that decodes, a flipbook's frames), each level's meshes (skinned for the rig it binds), then the scene (the
// data swapped in, the rig bound); a build begun drops one in flight, and the ObjectModel draws the last scene
// built until the next one is. Nothing here decides what is drawn: its owner says.
class PreviewModel {
public:
	// Its ObjectModel added under `parent`.
	explicit PreviewModel(Node3D &parent);
	PreviewModel(const PreviewModel &) = delete;
	PreviewModel &operator=(const PreviewModel &) = delete;

	// A build of `model` (named `path`) begun, its textures read through `files`, its meshes skinned for and bound
	// to `rig` (null: none); a null model clears what it drew.
	void begin(const opennova::assets::Model &model, const std::string &path,
			std::shared_ptr<const opennova::editor::StampedFiles> files, std::shared_ptr<const opennova::anim::SkeletalClips> rig);
	bool building() const { return build_ != nullptr; }
	// One unit of the build: true when it was the last (the scene assembled).
	bool step();
	opennova::editor::OperationProgress progress() const;
	// What it drew dropped, a build in flight with it.
	void clear();
	// A scene stands (a build ended over a model).
	bool built() const { return data_.is_valid(); }

	// The state over the scene that stands: the CTRL registers held (one let go reads 0 again), the destroyed
	// sections hidden, the level drawn, a body posed as the game's presenter dispatches a person's row (the clip at
	// its playhead, or the outgoing one blended under it), and where its origin stands.
	void set_registers(const std::map<std::string, int64_t> &held);
	void set_hidden_sections(uint32_t mask);
	void set_level(int lod);
	void pose_body(const opennova::world::InfantryBodyPose &pose);
	void set_lift(float metres);
	// Every frame: its part animations and generators at the clock's milliseconds.
	void tick(int64_t ms);

	// The files the scene read (its textures), or the build in flight so far.
	opennova::editor::FileStamps stamps() const;
	ObjectModel *object() const { return object_; }

private:
	struct Unit {
		enum class Kind : uint8_t { Texture, Frame, Meshes, Scene };
		Kind kind = Kind::Scene;
		int material = -1;
		int slot = 0;
		int frame = 0;
		int lod = -1;
	};
	struct Build {
		Ref<ObjectData> data;
		std::shared_ptr<const opennova::editor::StampedFiles> files;
		Ref<SkeletalAnim> skeletal;
		int bone_count = 0;
		std::vector<Unit> units;
		size_t next = 0;
	};
	static void plan_(Build &build);
	void assemble_(Build &build);

	ObjectModel *object_ = nullptr;
	Ref<ObjectData> data_;
	Ref<PanmClock> clock_;
	int64_t frame_ = 0;
	std::shared_ptr<const opennova::editor::StampedFiles> files_;
	std::map<std::string, int64_t> applied_ctrl_;
	uint32_t applied_hidden_ = 0;
	int applied_lod_ = -1;
	std::unique_ptr<Build> build_;
};

} // namespace godot
