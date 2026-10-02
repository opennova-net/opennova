#include "authoring/mission_viewport_applier.h"

#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <editor/preview/mission_viewport.h>
#include <editor/preview/viewport_device.h>
#include <runtime/renderer/render_order.h>

#include "render/frame_fx.h"

namespace godot {

namespace {

Vector3 to_godot(const opennova::editor::PreviewVec3 &v) {
	return Vector3(v.x, v.y, v.z);
}

const opennova::editor::MissionViewport &mission_of(const opennova::editor::ViewportModel &model) {
	return static_cast<const opennova::editor::MissionViewport &>(model);
}

} // namespace

MissionViewportApplier::MissionViewportApplier(SubViewport &viewport) {
	viewport.set_msaa_3d(Viewport::MSAA_4X);
	root_ = memnew(Node3D);
	root_->set_name("Mission");
	viewport.add_child(root_);
	// Retail shaders write gamma-domain values and rely on one terminal display decode per 3D
	// view; the environment with no .env lights like the retail noon.
	root_->add_child(memnew(DisplayDecode));
	environment_ = memnew(MissionEnvironment);
	environment_->set_name("Environment");
	root_->add_child(environment_);
	camera_ = memnew(Camera3D);
	camera_->set_keep_aspect_mode(Camera3D::KEEP_WIDTH);
	camera_->set_fov(opennova::editor::OrbitCamera::fov_horizontal_degrees());
	camera_->set_current(true);
	root_->add_child(camera_);
}

void MissionViewportApplier::place_camera_(const opennova::editor::ViewportModel &viewport) {
	const opennova::editor::OrbitCamera &camera = mission_of(viewport).camera();
	opennova::editor::PreviewVec3 right, up, back;
	camera.axes(right, up, back);
	camera_->set_transform(Transform3D(Basis(to_godot(right), to_godot(up), to_godot(back)), to_godot(camera.eye())));
	camera_->set_near(opennova::renderer::kScenePassNearZ);
	camera_->set_far(opennova::renderer::scene_far_plane(environment_->get_fog_distance()));
}

void MissionViewportApplier::rebuild(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::SessionView &, const opennova::editor::PreviewClock &) {
	// Made whole as it is taken: nothing of it builds over the frames yet.
	place_camera_(viewport);
}

void MissionViewportApplier::update(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &) {
	place_camera_(viewport);
}

void MissionViewportApplier::apply(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &,
		opennova::editor::ViewportDeviceReport &) {
	place_camera_(viewport);
}

} // namespace godot
