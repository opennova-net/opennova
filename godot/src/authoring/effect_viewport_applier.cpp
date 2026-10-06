#include "authoring/effect_viewport_applier.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <editor/preview/effect_viewport.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/view/session_view.h>

#include "env/mission_environment.h"
#include "render/frame_fx.h"

namespace godot {

namespace {

// The grid's half side, in metres (a square every metre).
constexpr int kGridHalf = 10;

const opennova::editor::EffectViewport &effect_of(const opennova::editor::ViewportModel &model) {
	return static_cast<const opennova::editor::EffectViewport &>(model);
}

Vector3 to_godot(const opennova::editor::PreviewVec3 &v) {
	return Vector3(v.x, v.y, v.z);
}

// The grid: a line every metre across the ground the effect spawns on, the two through the spawn point
// brighter.
Ref<ArrayMesh> grid_mesh() {
	PackedVector3Array lines;
	PackedColorArray colors;
	const Color faint(0.45f, 0.45f, 0.45f, 0.35f), axis(0.7f, 0.7f, 0.7f, 0.6f);
	for (int i = -kGridHalf; i <= kGridHalf; ++i) {
		const Color color = i == 0 ? axis : faint;
		lines.push_back(Vector3(float(i), 0.0f, float(-kGridHalf)));
		lines.push_back(Vector3(float(i), 0.0f, float(kGridHalf)));
		lines.push_back(Vector3(float(-kGridHalf), 0.0f, float(i)));
		lines.push_back(Vector3(float(kGridHalf), 0.0f, float(i)));
		for (int k = 0; k < 4; ++k) colors.push_back(color);
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = lines;
	arrays[Mesh::ARRAY_COLOR] = colors;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_LINES, arrays);
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	mesh->surface_set_material(0, material);
	return mesh;
}

} // namespace

EffectViewportApplier::EffectViewportApplier(SubViewport &viewport) {
	// No multisampling, as the game's own view draws: the particle renderer's compositor passes draw over
	// the view's single-sampled colour and depth (a multisampled view's resolved depth is no attachment
	// they can bind; the model and mission devices' MSAA_4X would leave every particle undrawn).
	viewport.set_msaa_3d(Viewport::MSAA_DISABLED);
	Node3D *root = memnew(Node3D);
	viewport.add_child(root);
	// One terminal display decode for the view, and the environment with no .env, which lights like the
	// retail noon: the particle tints a mission's light gives (the model preview's recipe).
	root->add_child(memnew(DisplayDecode));
	environment_ = memnew(MissionEnvironment);
	root->add_child(environment_);
	camera_ = memnew(Camera3D);
	camera_->set_keep_aspect_mode(Camera3D::KEEP_WIDTH);
	camera_->set_fov(opennova::editor::OrbitCamera::fov_horizontal_degrees());
	camera_->set_current(true);
	root->add_child(camera_);
	grid_ = memnew(MeshInstance3D);
	grid_->set_name("Grid");
	grid_->set_mesh(grid_mesh());
	root->add_child(grid_);
	effects_ = std::make_unique<PreviewEffects>(*root);
	effects_->set_environment_source(environment_);
}

EffectViewportApplier::~EffectViewportApplier() = default;

void EffectViewportApplier::rebuild(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::SessionView &view, const opennova::editor::PreviewClock &clock) {
	const opennova::editor::EffectViewport &model = effect_of(viewport);
	if (!model.playback().scene() || !view.findings.assets) {
		clear();
		return;
	}
	// The project's files the graphics are read from (mounted again where one read moved), and the scene
	// the viewport opened last; drawn now as it stands.
	effects_->mount(view.findings.assets);
	effects_->show(model.playback().scene());
	shows_ = true;
	place_(viewport);
	effects_->render(int64_t(clock.ms()));
}

void EffectViewportApplier::update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &) {
	place_(model);
}

void EffectViewportApplier::clear() {
	effects_->clear();
	effects_->set_environment_source(environment_);
	shows_ = false;
}

void EffectViewportApplier::place_(const opennova::editor::ViewportModel &viewport) {
	const opennova::editor::EffectViewport &model = effect_of(viewport);
	const opennova::editor::OrbitCamera &camera = model.camera();
	opennova::editor::PreviewVec3 right, up, back;
	camera.axes(right, up, back);
	camera_->set_transform(Transform3D(Basis(to_godot(right), to_godot(up), to_godot(back)), to_godot(camera.eye())));
	camera_->set_near(camera.near_plane);
	camera_->set_far(camera.far_plane);
	grid_->set_visible(model.options().grid);
}

void EffectViewportApplier::apply(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &,
		opennova::editor::ViewportDeviceReport &report) {
	place_(model);
	if (!shows_) return;
	report.files = effects_->stamps();
	report.missing = effects_->missing();
}

void EffectViewportApplier::tick(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &clock) {
	if (!shows_) return;
	// The scene the viewport stepped, drawn as it stands (a scene opened again since the last Rebuild is the
	// next one's to take).
	(void)viewport;
	effects_->render(int64_t(clock.ms()));
}

} // namespace godot
