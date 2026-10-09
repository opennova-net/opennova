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

// The units that make one model's data ready for its scene, a unit a step (ADR 0046 S13 V6: the model device's
// builds, its gun's and a first-person map's arms', and the definition device's): each material's stages (its first
// texture of the slot that decodes, the normal stage falling back to the second normal slot) and flipbook frames, as
// ObjectModel::create_material and collect_anim_frames load them, then each level's meshes, skinned for `bone_count`
// bones of a rig where `skinned`, as the scene asks them of the data: decoded here, so the scene's loads hit the
// data's caches.
class ModelDataBuild {
public:
	ModelDataBuild(Ref<ObjectData> data, bool skinned, int bone_count);
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
			std::shared_ptr<const opennova::StampedFiles> files, std::shared_ptr<const opennova::anim::SkeletalClips> rig);
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
	// A first-person channel's clip on the rig (DI-22, the game's view model posed as the weapon pump steps it): the
	// clip at its gated ticks, or the primary slerped toward the ring's next entry by the weight while a loop wrap
	// fades it in [orig: AnimChannel_BlendTwoChannels @ 0x410DBD].
	void play_clip(const std::string &key, int variant, int ticks);
	void play_blend(const std::string &key, int ticks, const std::string &blend_key, int blend_ticks, float weight,
			int variant, int blend_variant);
	// The first-person arms (DI-22, as the model device's DI-13 arms): the avatar's arms part, no authored levels of
	// their own, their camo triplet written over each scene built, as the game's per-submit writer stores it before
	// each arms submit [orig: Avatar_SetArmsCamoCtrl @0x57a3b0].
	void set_arms(int camo0, int camo1, int camo2);
	// Every frame: its part animations and generators at the clock's milliseconds.
	void tick(int64_t ms);

	// The files the scene read (its textures), or the build in flight so far.
	opennova::FileStamps stamps() const;
	ObjectModel *object() const { return object_; }

private:
	// A build in flight: its data's units, then the scene.
	struct Build {
		std::shared_ptr<const opennova::StampedFiles> files;
		Ref<SkeletalAnim> skeletal;
		std::unique_ptr<ModelDataBuild> data;
	};
	void assemble_(Build &build);

	ObjectModel *object_ = nullptr;
	Ref<ObjectData> data_;
	Ref<PanmClock> clock_;
	int64_t frame_ = 0;
	std::shared_ptr<const opennova::StampedFiles> files_;
	std::map<std::string, int64_t> applied_ctrl_;
	uint32_t applied_hidden_ = 0;
	bool arms_ = false;
	int arms_camo_[3] = {0, 0, 0};
	int applied_lod_ = -1;
	std::unique_ptr<Build> build_;
};

} // namespace godot
