#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include "authoring/viewport_applier.h"
#include "env/mission_environment.h"

namespace godot {

// A mission viewport's device work (ADR 0046 S14): the game's own nodes under the device's
// SubViewport, the camera where the viewport's orbit camera is. This first form shows the
// environment and the camera alone (the retail noon the environment has with no file, one display
// decode per 3D view, as the model's device): the mission's marks are the canvas's, drawn over the
// picture by the viewport's portable half, and sit on the pixels this camera projects them to. The
// terrain, the sky, the water and the placed entities, built over the Shell's frames, are the next
// commits'.
class MissionViewportApplier final : public ViewportApplier {
public:
	explicit MissionViewportApplier(SubViewport &viewport);

	void rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &view,
			const opennova::editor::PreviewClock &clock) override;
	void update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void clear() override {}
	void apply(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			opennova::editor::ViewportDeviceReport &report) override;
	void tick(const opennova::editor::ViewportModel &, const opennova::editor::PreviewClock &) override {}
	void resize(int, int) override {}

	// Its nodes, for the parity tests.
	Camera3D *camera() const { return camera_; }
	MissionEnvironment *environment() const { return environment_; }

private:
	// The camera where the viewport's is, the world pass's near plane.
	void place_camera_(const opennova::editor::ViewportModel &model);

	Node3D *root_ = nullptr;
	MissionEnvironment *environment_ = nullptr;
	Camera3D *camera_ = nullptr;
};

} // namespace godot
