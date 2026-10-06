#include "authoring/definition_viewport_applier.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <editor/assets/project_asset_source.h>
#include <editor/preview/definition_viewport.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/view/session_view.h>

#include "authoring/effect_viewport_applier.h"
#include "env/mission_environment.h"
#include "render/frame_fx.h"

namespace godot {

namespace {

const opennova::editor::DefinitionViewport &definition_of(const opennova::editor::ViewportModel &model) {
	return static_cast<const opennova::editor::DefinitionViewport &>(model);
}

Vector3 to_godot(const opennova::editor::PreviewVec3 &v) {
	return Vector3(v.x, v.y, v.z);
}

} // namespace

DefinitionViewportApplier::DefinitionViewportApplier(SubViewport &viewport) {
	// Single-sampled, as the game's own view draws: the particle renderer's compositor passes bind the view's depth
	// (DI-14's rule).
	viewport.set_msaa_3d(Viewport::MSAA_DISABLED);
	Node3D *root = memnew(Node3D);
	viewport.add_child(root);
	// One terminal display decode for the view, and the environment with no .env, which lights like the retail noon
	// (the model preview's recipe, and the particle tints it gives).
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
	grid_->set_mesh(preview_grid_mesh());
	root->add_child(grid_);
	model_ = std::make_unique<PreviewModel>(*root);
	effects_ = std::make_unique<PreviewEffects>(*root);
	effects_->set_environment_source(environment_);
}

DefinitionViewportApplier::~DefinitionViewportApplier() = default;

void DefinitionViewportApplier::rebuild(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::SessionView &view, const opennova::editor::PreviewClock &clock) {
	const opennova::editor::DefinitionViewport &model = definition_of(viewport);
	if (!model.model() || !view.findings.assets) {
		clear();
		return;
	}
	// The model built again over the project's files, each texture read noted (one that moves builds it again), a
	// person's meshes skinned for its rig.
	model_->begin(model.model(), model.drawn().file, std::make_shared<opennova::editor::StampedFiles>(view.findings.assets),
			model.skeleton());
	// The effects' graphics through the project's files (mounted again where one read moved).
	effects_->mount(view.findings.assets);
	mounted_ = true;
	show_effects_(viewport);
	place_(viewport);
	effects_->render(int64_t(clock.ms()));
}

ApplierStep DefinitionViewportApplier::step(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::PreviewClock &clock, std::string &) {
	if (!model_->step()) return ApplierStep::More;
	// The state as it is now, as an Update applies it (one that came while the build ran is folded into this).
	apply_state_(viewport, clock);
	return ApplierStep::Built;
}

void DefinitionViewportApplier::update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) {
	apply_state_(model, clock);
}

void DefinitionViewportApplier::clear() {
	model_->clear();
	effects_->clear();
	effects_->set_environment_source(environment_);
	mounted_ = false;
}

void DefinitionViewportApplier::place_(const opennova::editor::ViewportModel &viewport) {
	const opennova::editor::DefinitionViewport &model = definition_of(viewport);
	const opennova::editor::OrbitCamera &camera = model.camera();
	opennova::editor::PreviewVec3 right, up, back;
	camera.axes(right, up, back);
	camera_->set_transform(Transform3D(Basis(to_godot(right), to_godot(up), to_godot(back)), to_godot(camera.eye())));
	camera_->set_near(camera.near_plane);
	camera_->set_far(camera.far_plane);
	grid_->set_visible(model.options().grid);
}

void DefinitionViewportApplier::show_effects_(const opennova::editor::ViewportModel &viewport) {
	const std::shared_ptr<opennova::particle::EffectScene> &scene = definition_of(viewport).effects().scene();
	if (mounted_ && scene != effects_->scene()) effects_->show(scene);
}

void DefinitionViewportApplier::apply_state_(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::PreviewClock &clock) {
	const opennova::editor::DefinitionViewport &model = definition_of(viewport);
	place_(viewport);
	if (!model_->built()) return;
	model_->set_registers(model.ctrl_at(clock));
	model_->set_hidden_sections(model.hidden_sections_at(clock));
	model_->set_level(model.lod());
	// A person stands where its spawn stands it, posed as its warmup leaves it.
	const opennova::editor::MissionPose &person = model.person();
	const bool posed = person.status == "posed" && model.skeleton();
	model_->set_lift(posed ? float(person.lift) : 0.0f);
	if (posed) model_->pose_body(person.pose);
}

void DefinitionViewportApplier::apply(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &clock,
		opennova::editor::ViewportDeviceReport &report) {
	if (model_->building()) {
		// A build runs: the textures its units read so far, and nothing applied over a scene it may be swapping.
		report.files = model_->stamps();
		return;
	}
	apply_state_(viewport, clock);
	show_effects_(viewport);
	report.files = model_->stamps();
	if (mounted_) {
		report.files.add(effects_->stamps());
		report.missing = effects_->missing();
	}
}

void DefinitionViewportApplier::tick(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &clock) {
	const opennova::editor::DefinitionViewport &model = definition_of(viewport);
	if (!model_->building() && model_->built()) {
		// The destroy fade and the pieces' sections move with the clock.
		model_->set_registers(model.ctrl_at(clock));
		model_->set_hidden_sections(model.hidden_sections_at(clock));
	}
	model_->tick(int64_t(clock.ms()));
	if (!mounted_) return;
	// The scene the viewport stepped, drawn as it stands.
	show_effects_(viewport);
	effects_->render(int64_t(clock.ms()));
}

} // namespace godot
