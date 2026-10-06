#pragma once

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <memory>

#include <runtime/particle/effect_scene.h>

#include "authoring/preview_effects.h"
#include "authoring/viewport_applier.h"

namespace godot {

class MissionEnvironment;

// A metre's squares on the ground a preview's picture stands on (the editor's aid: the effect device's and the
// definition device's), the lines through the origin brighter.
Ref<ArrayMesh> preview_grid_mesh();

// An effect viewport's device work (ADR 0046 DI-14): the scene the viewport's playback steps
// (editor/preview/effect_viewport: the engine's effect scene, the effect spawned and played on the
// preview clock) drawn by the game's particle renderer (authoring/preview_effects) through the camera the
// viewport's orbit camera places, under the retail noon's environment (the particle tints a mission's
// light gives, MissionEnvironment with no .env) and one display decode, as the model preview draws; the
// graphics read from the project's files. A metre's grid on the ground the effect spawns on while the
// options show it (the editor's aid). A Rebuild takes the scene opened last and mounts the project's
// files (again where a graphic read moved); every frame the scene is drawn as its owner stepped it.
// Nothing here simulates or decides what plays.
class EffectViewportApplier final : public ViewportApplier {
public:
	explicit EffectViewportApplier(SubViewport &viewport);
	~EffectViewportApplier() override;

	void rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &view,
			const opennova::editor::PreviewClock &clock) override;
	void update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void clear() override;
	void apply(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			opennova::editor::ViewportDeviceReport &report) override;
	void tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void resize(int, int) override {}

	// Its nodes and values, for the device tests: the camera, the grid, the effects drawn.
	Camera3D *camera() const { return camera_; }
	MeshInstance3D *grid() const { return grid_; }
	PreviewEffects &effects() { return *effects_; }

private:
	// The camera where the viewport's is, the grid as its options say.
	void place_(const opennova::editor::ViewportModel &model);

	Camera3D *camera_ = nullptr;
	MeshInstance3D *grid_ = nullptr;
	MissionEnvironment *environment_ = nullptr;
	std::unique_ptr<PreviewEffects> effects_;
	bool shows_ = false; // a scene was taken (a Rebuild) and not cleared since
};

} // namespace godot
