#include "authoring/orbit_stage.h"

#include <godot_cpp/classes/viewport.hpp>

#include <editor/preview/model_preview_camera.h>
#include <editor/session/preview_background.h>

#include "authoring/preview_backdrop.h"
#include "env/mission_environment.h"

namespace godot {

OrbitStage make_orbit_stage(SubViewport &viewport, bool grid) {
	OrbitStage stage;
	viewport.set_msaa_3d(Viewport::MSAA_DISABLED);
	stage.backdrop = make_preview_backdrop_3d();
	viewport.add_child(stage.backdrop);
	stage.root = memnew(Node3D);
	viewport.add_child(stage.root);
	stage.frame_effects = std::make_unique<PreviewFrameEffects>(*stage.root, viewport, false);
	stage.environment = memnew(MissionEnvironment);
	stage.root->add_child(stage.environment);
	stage.camera = memnew(Camera3D);
	stage.camera->set_keep_aspect_mode(Camera3D::KEEP_WIDTH);
	stage.camera->set_fov(opennova::editor::OrbitCamera::fov_horizontal_degrees());
	stage.camera->set_current(true);
	stage.root->add_child(stage.camera);
	if (grid) {
		stage.grid = memnew(MeshInstance3D);
		stage.grid->set_name("Grid");
		stage.grid->set_mesh(preview_grid_mesh(opennova::editor::PreviewBackground::Dark));
		stage.root->add_child(stage.grid);
	}
	return stage;
}

} // namespace godot
