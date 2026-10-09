#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <editor/preview/viewport_follow.h>

#include "authoring/preview_effects.h"
#include "authoring/preview_model.h"
#include "authoring/preview_range_draw.h"
#include "authoring/viewport_applier.h"
#include "object/object_data.h"
#include "object/object_model.h"
#include "object/skeletal_anim.h"

namespace godot {

class MissionEnvironment;

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
//
// A weapon's first-person map (DI-13) draws the arms beside the gun: a second ObjectModel of the arms model
// the viewport read, built with the gun's (its textures and its meshes skinned for the gun's rig, built as
// the game's first-person view model builds its arms, without their authored levels), bound to its own
// instance of the gun's rig and posed by the same clip, its camo the character's; the camera stands where
// the viewport's eye does and sees with its field of view.
//
// A clip's fire events (DI-24, editor/preview/preview_clip_fire) draw the range the shots fly in: the effects the run
// spawned (the ammo's ai_launcheffect, each impact's row) by the game's particle renderer (authoring/preview_effects,
// the project's files mounted for their graphics), the tracers, the target's face and the scars on it
// (authoring/preview_range_draw), all made the first time a clip fires in the device. While anything fires the view
// is single-sampled, as the game's view draws and the particle renderer's passes need (DI-14's rule); else it keeps
// its 4x.
class ModelViewportApplier final : public ViewportApplier {
public:
	explicit ModelViewportApplier(SubViewport &viewport);

	void rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &view,
			const opennova::editor::PreviewClock &clock) override;
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
	// The editor's preview background behind the model.
	void background(opennova::editor::PreviewBackground background) override;

	// Its nodes, for the parity tests: the camera, the model and the first-person arms.
	Camera3D *camera() const { return camera_; }
	ObjectModel *object_model() const { return object_; }
	ObjectModel *arms_model() const { return arms_; }
	// A clip's fire (DI-24): the effects drawn and the range's target, tracers and scars (null until a clip fires
	// here), the view's multisampling.
	PreviewEffects *effects() const { return effects_.get(); }
	PreviewRangeDraw *range() const { return range_.get(); }
	bool single_sampled() const { return single_sampled_; }

private:
	// A build in flight: the data the scene will hold (and the first-person arms', null: none), the files
	// its textures are read through, the rig it binds (null: none) and its serial, and its units: each
	// data's (authoring/preview_model's ModelDataBuild: its materials' stages and flipbook frames, its
	// levels), the gun's then the arms', then the scene (the data swapped in, the rig bound) and the pose
	// (the registers, the level, the camera, the clip at the clock).
	struct Build {
		Ref<ObjectData> data;
		Ref<ObjectData> arms;
		Ref<SkeletalAnim> arms_skeletal;
		std::array<int, 3> arms_camo{};
		std::shared_ptr<const opennova::StampedFiles> files;
		Ref<SkeletalAnim> skeletal;
		int bone_count = 0;
		uint64_t skeleton_serial = UINT64_MAX;
		std::vector<ModelDataBuild> parts;
		size_t part = 0; // the part building; parts.size(): the scene, then the pose
		bool assembled = false;
	};
	// The scene: the build's data swapped in with its rig.
	void assemble_(Build &build);
	// The viewport's state over the picture that stands, in one place: the pose unit as a build ends,
	// an Update, and every pump after them (the rig bound again when it was loaded again, the
	// registers, the clip at `clock`, the camera and the level), so an Update folded into a build
	// applies as the build ends exactly as it would have.
	void apply_state_(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock);
	// The CTRL registers the picture reads at `clock` (the options' held ones, and the destroy fade's while
	// the damage state drives them: ModelViewport::ctrl_at; one let go reads 0 again), and the sections the
	// death pieces left (ModelViewport::hidden_sections_at, the model's destroyed-section mask, DI-10);
	// nothing when they stand.
	void apply_registers_(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock);
	// A weapon's first-person map's per-submit registers (DI-13): TEX_TEAM on the gun and the arms.
	void apply_first_person_registers_(const opennova::editor::ViewportModel &model);
	// The camera where the viewport's is, and the level it picks.
	void place_camera_(const opennova::editor::ViewportModel &model);
	// The rig the viewport loaded, bound to the model (again when it is loaded again).
	void bind_rig_(const opennova::editor::ViewportModel &model);
	void play_clip_(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock);
	// A clip's fire (DI-24): the effects' scene the viewport's run stepped, the range drawn against the camera, the
	// view single-sampled while anything fires.
	void apply_fire_(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock);
	// The clip fire's effects and range made (the first time a clip fires), and let go of (nothing drawn, nothing
	// mounted).
	void make_fire_();
	void release_fire_();

	SubViewport *viewport_ = nullptr;
	Node3D *root_ = nullptr;
	MeshInstance3D *backdrop_ = nullptr; // the editor's preview background (authoring/preview_backdrop)
	MissionEnvironment *environment_ = nullptr;
	Camera3D *camera_ = nullptr;
	ObjectModel *object_ = nullptr;
	ObjectModel *arms_ = nullptr; // the first-person arms on the gun's rig (no data: none)
	Ref<ObjectData> data_;
	Ref<ObjectData> arms_data_;
	Ref<PanmClock> clock_;
	int64_t frame_ = 0;
	// The files the built scene read (its textures), noted as they are read.
	std::shared_ptr<const opennova::StampedFiles> files_;
	std::map<std::string, int64_t> applied_ctrl_; // the registers the model holds now
	uint32_t applied_hidden_ = 0; // the destroyed-section mask the model holds now
	int applied_lod_ = -1;
	int applied_team_ = INT32_MIN + 1; // the TEX_TEAM written (INT32_MIN: none, cleared)
	uint64_t applied_skeleton_ = UINT64_MAX; // the rig serial the model holds
	std::unique_ptr<Build> build_; // the build in flight (null: none)
	std::unique_ptr<PreviewEffects> effects_;
	std::unique_ptr<PreviewRangeDraw> range_;
	std::shared_ptr<const opennova::editor::ProjectAssetSource> fire_files_; // an animation's project files
	bool mounted_ = false; // the project's files are mounted for the clip fire's effects
	bool single_sampled_ = false;
};

} // namespace godot
