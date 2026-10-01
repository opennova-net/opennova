#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <editor/preview/viewport_follow.h>

#include "authoring/viewport_applier.h"
#include "object/object_data.h"
#include "object/object_model.h"
#include "object/skeletal_anim.h"

namespace godot {

// A model viewport's device work (ADR 0046 S10p3, S13 V5): the runtime's own ObjectModel (its
// meshes, materials, CTRL registers, part animations) under the retail noon light and one display
// decode (avatar_preview.gd's recipe), built from the model the viewport shows (as it would save, or
// an animation's rig model), its textures read through the project's files (the open documents
// standing in for theirs, each read noted with its stamp, a flipbook's frame as it is first drawn
// too); the camera placed where the viewport's orbit camera is, the level drawn the one it picks;
// an animation's rig bound and its clip posed at the preview clock's tick; the part animations and
// material generators run at the clock's milliseconds on the device's own PanmClock.
//
// It builds over several frames (S13 V6), a unit a step: each material's textures (a stage's
// first texture that decodes, a flipbook's frames), then each level's meshes (skinned for the rig an
// animation binds), decoded and made into the new model data's caches off the scene; then the scene,
// the data swapped into the ObjectModel with its rig bound (built once, from those caches); then the
// pose (the registers, the level, the camera, the clip at the clock). The ObjectModel keeps every
// level (presenter-driven, the mission placer's mode), so a level the camera's distance picks
// afterwards swaps its rows in place rather than building the scene again. A Rebuild while one runs
// drops the build's data and begins anew; what the device draws meanwhile is the last picture (the
// device renders nothing half built). No unit fails: a build is planned only over a model.
class ModelViewportApplier final : public ViewportApplier {
public:
	explicit ModelViewportApplier(SubViewport &viewport);

	void rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &view) override;
	ApplierStep step(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			std::string &failure) override;
	bool building() const override { return build_ != nullptr; }
	opennova::editor::OperationProgress progress() const override;
	void update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void clear() override;
	void apply(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			opennova::editor::ViewportDeviceReport &report) override;
	void tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void resize(int, int) override {}

	// Its nodes, for the parity tests: the camera and the model.
	Camera3D *camera() const { return camera_; }
	ObjectModel *object_model() const { return object_; }

private:
	// One unit of a build, in the order they run.
	struct Unit {
		enum class Kind : uint8_t {
			Texture, // a material's stage: its first texture of `slot` that decodes (slot 3 falls back to 4)
			Frame, // a frame of a material's flipbook
			Meshes, // a level's meshes
			Scene, // the data swapped in, the rig bound: the scene built
			Pose, // the registers, the level, the camera, the clip at the clock
		};
		Kind kind = Kind::Scene;
		int material = -1; // Texture, Frame: the MTRL row
		int slot = 0; // Texture
		int frame = 0; // Frame
		int lod = -1; // Meshes
	};
	// A build in flight: the data the scene will hold, the files its textures are read through, the
	// rig it binds (null: none) and its serial, and its units.
	struct Build {
		Ref<ObjectData> data;
		std::shared_ptr<const opennova::editor::StampedFiles> files;
		Ref<SkeletalAnim> skeletal;
		int bone_count = 0;
		uint64_t skeleton_serial = UINT64_MAX;
		std::vector<Unit> units;
		size_t next = 0;
	};
	// The units of `build`'s data: its materials' stages and flipbook frames, its levels, the scene,
	// the pose.
	static void plan_(Build &build);
	// The scene: the build's data swapped in with its rig.
	void assemble_(Build &build);
	// The viewport's state over the picture that stands, in one place: the pose unit as a build ends,
	// an Update, and every pump after them (the rig bound again when it was loaded again, the
	// registers, the clip at `clock`, the camera and the level), so an Update folded into a build
	// applies as the build ends exactly as it would have.
	void apply_state_(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock);
	// The CTRL registers the options hold (one let go reads 0 again); nothing when they stand.
	void apply_registers_(const opennova::editor::ViewportModel &model);
	// The camera where the viewport's is, and the level it picks.
	void place_camera_(const opennova::editor::ViewportModel &model);
	// The rig the viewport loaded, bound to the model (again when it is loaded again).
	void bind_rig_(const opennova::editor::ViewportModel &model);
	void play_clip_(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock);

	Camera3D *camera_ = nullptr;
	ObjectModel *object_ = nullptr;
	Ref<ObjectData> data_;
	Ref<PanmClock> clock_;
	int64_t frame_ = 0;
	// The files the built scene read (its textures), noted as they are read.
	std::shared_ptr<const opennova::editor::StampedFiles> files_;
	std::map<std::string, int64_t> applied_ctrl_; // the registers the model holds now
	int applied_lod_ = -1;
	uint64_t applied_skeleton_ = UINT64_MAX; // the rig serial the model holds
	std::unique_ptr<Build> build_; // the build in flight (null: none)
};

} // namespace godot
