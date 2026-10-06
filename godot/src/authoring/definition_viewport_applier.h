#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <cstdint>
#include <memory>

#include "authoring/preview_effects.h"
#include "authoring/preview_model.h"
#include "authoring/viewport_applier.h"

namespace godot {

class MissionEnvironment;

// A definition viewport's device work (ADR 0046 DI-21): the record's picture as the viewport makes it
// (editor/preview/definition_viewport): the model it draws as the runtime's ObjectModel (authoring/preview_model,
// built over frames: its textures read through the project's files, a person's meshes skinned for its rig and
// its body posed as the spawn poses it, lifted by the spawn's rise), the destroy fade's registers and the
// sections the death pieces left on it at the preview clock, its effects (the scene the viewport's playback
// steps) drawn by the game's particle renderer (authoring/preview_effects, DI-14's helper), under the retail
// noon's environment with one display decode, through the camera the viewport's orbit camera places, a metre's
// grid on the ground while its options show it. Single-sampled, as the game's view draws and the particle
// renderer's passes need (DI-14's rule). A Rebuild builds the model again (another model, its file or a texture
// moved, the rig loaded again) and mounts the project's files for the effects' graphics; every frame the state
// is applied and the effects drawn as the viewport stepped them. Nothing here decides what is drawn.
class DefinitionViewportApplier final : public ViewportApplier {
public:
	explicit DefinitionViewportApplier(SubViewport &viewport);
	~DefinitionViewportApplier() override;

	void rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &view,
			const opennova::editor::PreviewClock &clock) override;
	ApplierStep step(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			std::string &failure) override;
	bool building() const override { return model_->building(); }
	opennova::editor::OperationProgress progress() const override { return model_->progress(); }
	void update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void clear() override;
	void apply(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			opennova::editor::ViewportDeviceReport &report) override;
	void tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void resize(int, int) override {}

	// Its nodes, for the device tests: the camera, the grid, the model, the effects drawn.
	Camera3D *camera() const { return camera_; }
	MeshInstance3D *grid() const { return grid_; }
	ObjectModel *object_model() const { return model_->object(); }
	PreviewEffects &effects() { return *effects_; }

private:
	// The viewport's state over the scene that stands: the registers, the hidden sections, the level, a person's
	// pose and lift, the camera and the grid.
	void apply_state_(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock);
	void place_(const opennova::editor::ViewportModel &model);
	// The effects' scene the viewport plays, taken where it opened another.
	void show_effects_(const opennova::editor::ViewportModel &model);

	Camera3D *camera_ = nullptr;
	MeshInstance3D *grid_ = nullptr;
	MissionEnvironment *environment_ = nullptr;
	std::unique_ptr<PreviewModel> model_;
	std::unique_ptr<PreviewEffects> effects_;
	bool mounted_ = false; // the project's files are mounted for the effects (a Rebuild) and not cleared since
};

} // namespace godot
