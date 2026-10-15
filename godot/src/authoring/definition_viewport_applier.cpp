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
#include <runtime/renderer/fp_viewmodel_spec.h>
#include <runtime/world/player_present.h>

#include "authoring/effect_viewport_applier.h"
#include "authoring/preview_backdrop.h"
#include "authoring/preview_range_draw.h"
#include "env/mission_environment.h"
#include "player/player_viewmodel_rig.h"
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
	// The editor's preview background, behind everything the picture draws.
	backdrop_ = make_preview_backdrop_3d();
	viewport.add_child(backdrop_);
	Node3D *root = memnew(Node3D);
	viewport.add_child(root);
	// The game's FrameFx for the view (its one terminal display decode, and the distortion row a class-7 particle draws
	// on), and the environment with no .env, which lights like the retail noon (the model preview's recipe, and the
	// particle tints it gives).
	frame_effects_ = std::make_unique<PreviewFrameEffects>(*root, viewport, false);
	environment_ = memnew(MissionEnvironment);
	root->add_child(environment_);
	camera_ = memnew(Camera3D);
	camera_->set_keep_aspect_mode(Camera3D::KEEP_WIDTH);
	camera_->set_fov(opennova::editor::OrbitCamera::fov_horizontal_degrees());
	camera_->set_current(true);
	root->add_child(camera_);
	grid_ = memnew(MeshInstance3D);
	grid_->set_name("Grid");
	grid_->set_mesh(preview_grid_mesh(opennova::editor::PreviewBackground::Dark));
	root->add_child(grid_);
	model_ = std::make_unique<PreviewModel>(*root);
	// A weapon's first-person arms (DI-22).
	arms_ = std::make_unique<PreviewModel>(*root);
	arms_->set_arms(0, 0, 0);
	effects_ = std::make_unique<PreviewEffects>(*root);
	effects_->set_environment_source(environment_);
	// A weapon's range: its target's face, its tracers, the scars on the face (the model preview's clip fire's).
	range_ = std::make_unique<PreviewRangeDraw>(*root);
}

DefinitionViewportApplier::~DefinitionViewportApplier() = default;

void DefinitionViewportApplier::background(opennova::editor::PreviewBackground background) {
	set_preview_backdrop(*backdrop_, background);
	grid_->set_mesh(preview_grid_mesh(background));
}

void DefinitionViewportApplier::rebuild(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::SessionView &view, const opennova::editor::PreviewClock &clock) {
	const opennova::editor::DefinitionViewport &model = definition_of(viewport);
	// An ammo with no round model still shows its range (DI-23): the target, its impacts and their scars.
	if ((!model.model() && !model.range_shown()) || !view.findings.assets) {
		clear();
		return;
	}
	// The model built again over the project's files, each texture read noted (one that moves builds it again), a
	// person's or a first-person gun's meshes skinned for its rig.
	auto files = std::make_shared<opennova::StampedFiles>(view.findings.assets);
	if (model.model()) model_->begin(model.model(), model.drawn().file, files, model.skeleton());
	else model_->clear();
	// A weapon's first-person arms on the gun's rig, their camo the character's (DI-22, DI-13's recipe).
	const opennova::editor::DefinitionWeapon &weapon = model.weapon();
	const opennova::editor::FirstPersonSources &first_person = weapon.first_person();
	if (model.weapon_record() && weapon.first() && first_person.arms_model() && model.skeleton()) {
		const opennova::editor::FirstPersonCharacter *who = first_person.character();
		arms_->set_arms(who ? who->camo[0] : 0, who ? who->camo[1] : 0, who ? who->camo[2] : 0);
		arms_->begin(first_person.arms_model(), first_person.arms_file(), files, model.skeleton());
	} else {
		arms_->clear();
	}
	applied_team_ = INT32_MIN + 1;
	// The effects' graphics through the project's files (mounted again where one read moved); the scars' textures
	// and the tracers' smoke through the same root.
	effects_->mount(view.findings.assets);
	range_->set_resource_root(effects_->root());
	mounted_ = true;
	show_effects_(viewport);
	place_(viewport);
	effects_->render(int64_t(clock.ms()));
}

ApplierStep DefinitionViewportApplier::step(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::PreviewClock &clock, std::string &) {
	// The model's units, then the arms', a unit a step.
	if (model_->building()) {
		model_->step();
		if (model_->building() || arms_->building()) return ApplierStep::More;
	} else if (arms_->building()) {
		arms_->step();
		if (arms_->building()) return ApplierStep::More;
	}
	// The state as it is now, as an Update applies it (one that came while the build ran is folded into this).
	apply_state_(viewport, clock);
	return ApplierStep::Built;
}

opennova::editor::OperationProgress DefinitionViewportApplier::progress() const {
	opennova::editor::OperationProgress out = model_->progress();
	if (arms_->building()) {
		const opennova::editor::OperationProgress arms = arms_->progress();
		out.done += arms.done;
		out.total += arms.total;
		if (!model_->building()) out.label = arms.label;
	}
	return out;
}

void DefinitionViewportApplier::update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) {
	apply_state_(model, clock);
}

void DefinitionViewportApplier::clear() {
	model_->clear();
	arms_->clear();
	effects_->clear();
	effects_->set_environment_source(environment_);
	mounted_ = false;
	range_->clear();
	range_->set_resource_root(Ref<ResourceRoot>());
	applied_team_ = INT32_MIN + 1;
}

void DefinitionViewportApplier::place_(const opennova::editor::ViewportModel &viewport) {
	const opennova::editor::DefinitionViewport &model = definition_of(viewport);
	const opennova::editor::OrbitCamera &camera = model.camera();
	opennova::editor::PreviewVec3 right, up, back;
	camera.axes(right, up, back);
	camera_->set_transform(Transform3D(Basis(to_godot(right), to_godot(up), to_godot(back)), to_godot(camera.eye())));
	camera_->set_near(camera.near_plane);
	camera_->set_far(camera.far_plane);
	// The first-person eye sees with the weapon's renderfov (DI-22, DI-13's), else the game's view.
	camera_->set_fov(camera.fov_degrees());
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
	if (model_->built()) {
		model_->set_registers(model.ctrl_at(clock));
		model_->set_hidden_sections(model.hidden_sections_at(clock));
		model_->set_level(model.lod());
		// A person stands where its spawn stands it, posed as its warmup leaves it.
		const opennova::editor::MissionPose &person = model.person();
		const bool posed = person.status == "posed" && model.skeleton();
		model_->set_lift(posed ? float(person.lift) : 0.0f);
		if (posed) model_->pose_body(person.pose);
	} else if (!model.range_shown()) {
		return;
	}
	apply_weapon_(viewport, clock);
}

void DefinitionViewportApplier::apply_weapon_(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::PreviewClock &clock) {
	const opennova::editor::DefinitionViewport &model = definition_of(viewport);
	// A weapon's range, or an ammo's fired alone (DI-23).
	const bool weapon = model.range_shown();
	const opennova::editor::DefinitionWeapon &fire = model.weapon();
	// The SIGHTS card replaces the view model while it is up: the frame draws one or the other.
	const bool card = weapon && fire.card_up();
	model_->object()->set_visible(!card);
	arms_->object()->set_visible(!card);
	// The gun and the arms posed by the first-person channel as the weapon's pump left it, both parts by the one
	// latch (the original's AnimMap_PlayAnimBySlot @0x40bda0, cited in engine/), at its gated ticks (nothing free-runs the playhead).
	if (weapon && fire.first() && model.skeleton()) {
		const opennova::editor::DefinitionWeaponClip clip = fire.clip();
		for (PreviewModel *part : {model_.get(), arms_.get()}) {
			if (!part->built()) continue;
			if (clip.blending)
				part->play_blend(clip.key, clip.ticks, clip.blend_key, clip.blend_ticks, clip.blend_weight, clip.variant,
						clip.blend_variant);
			else
				part->play_clip(clip.key, clip.variant, clip.ticks);
		}
	}
	// TEX_TEAM the player's team byte on every first-person part, as the view model's per-submit writer stores it
	// (the engine's fp_ctrl_register_writes and viewmodel_team_byte); none, and any left cleared, otherwise.
	const bool first_person = weapon && fire.first() && fire.first_person().active();
	// The gun and the arms where the scope's ease stands the view model (the identity at the hip, the camera's
	// place), as the game's presenter stands it at the camera each frame (DefinitionWeapon::viewmodel_root).
	if (first_person && fire.eye()) {
		const opennova::editor::FirstPersonRoot &root = fire.viewmodel_root();
		const Transform3D placed(Basis(to_godot(root.x), to_godot(root.y), to_godot(root.z)), to_godot(root.origin));
		model_->object()->set_transform(placed);
		arms_->object()->set_transform(placed);
	} else {
		arms_->object()->set_transform(Transform3D());
	}
	const int team = first_person ? opennova::renderer::viewmodel_team_byte(fire.first_person().team()) : INT32_MIN;
	if (team != applied_team_ && model_->built()) {
		applied_team_ = team;
		for (PreviewModel *part : {model_.get(), arms_.get()}) {
			ObjectModel *object = part->object();
			if (!part->built()) continue;
			const opennova::world::FpCtrlRegisterWrites writes =
					opennova::world::fp_ctrl_register_writes(first_person, true, false, part == arms_.get());
			object->begin_ctrl_update();
			PlayerViewmodelRig::write_fp_team(*object, writes.team && team != INT32_MIN, team);
			object->end_ctrl_update();
		}
	}
	// The range (the target's face where it stands it, the scars uploaded again as it moved, the tracers built as the
	// game's ribbon pass builds them against this camera); none for a record that is no weapon.
	if (!weapon) {
		range_->clear();
		return;
	}
	opennova::editor::PreviewVec3 corners[4];
	range_->show_target(fire.target_corners(corners) ? corners : nullptr);
	range_->show_scars(fire.range().serial(), fire.range().scar_count(), [&fire]() { return fire.scars(); });
	range_->show_tracers(fire.trails(), *camera_, int64_t(clock.ms()));
}

void DefinitionViewportApplier::apply(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &clock,
		opennova::editor::ViewportDeviceReport &report) {
	if (building()) {
		// A build runs: the textures its units read so far, and nothing applied over a scene it may be swapping.
		report.files = model_->stamps();
		report.files.add(arms_->stamps());
		return;
	}
	apply_state_(viewport, clock);
	show_effects_(viewport);
	report.files = model_->stamps();
	report.files.add(arms_->stamps());
	if (mounted_) {
		report.files.add(effects_->stamps());
		report.missing = effects_->missing();
	}
}

void DefinitionViewportApplier::tick(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &clock) {
	const opennova::editor::DefinitionViewport &model = definition_of(viewport);
	if (!building() && !model_->built() && model.range_shown()) {
		// An ammo with no round model (DI-23): its range moves with the clock all the same.
		place_(viewport);
		apply_weapon_(viewport, clock);
	} else if (!building() && model_->built()) {
		// The destroy fade and the pieces' sections move with the clock; a weapon's channel, tracers and scars too.
		model_->set_registers(model.ctrl_at(clock));
		model_->set_hidden_sections(model.hidden_sections_at(clock));
		place_(viewport);
		apply_weapon_(viewport, clock);
	}
	model_->tick(int64_t(clock.ms()));
	arms_->tick(int64_t(clock.ms()));
	// The scene the viewport stepped, drawn as it stands, then the frame's FrameFX.
	if (mounted_) {
		show_effects_(viewport);
		effects_->render(int64_t(clock.ms()));
	}
	frame_effects_->present(mounted_ ? effects_->renderer() : nullptr);
}

} // namespace godot
