#include "authoring/model_viewport_applier.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <editor/assets/project_asset_source.h>
#include <editor/preview/model_viewport.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/view/session_view.h>
#include <runtime/renderer/fp_viewmodel_spec.h>
#include <runtime/world/player_present.h>

#include "authoring/preview_backdrop.h"
#include "env/mission_environment.h"
#include "object/avatar_database.h"
#include "player/player_viewmodel_rig.h"
#include "render/frame_fx.h"
#include "util/string_convert.h"
#include "util/texture_files.h"

namespace godot {

namespace {

Vector3 to_godot(const opennova::editor::PreviewVec3 &v) {
	return Vector3(v.x, v.y, v.z);
}

const opennova::editor::ModelViewport &model_of(const opennova::editor::ViewportModel &model) {
	return static_cast<const opennova::editor::ModelViewport &>(model);
}

} // namespace

ModelViewportApplier::ModelViewportApplier(SubViewport &viewport) : viewport_(&viewport) {
	viewport.set_msaa_3d(Viewport::MSAA_4X);
	// The editor's preview background, behind everything the picture draws.
	backdrop_ = make_preview_backdrop_3d();
	viewport.add_child(backdrop_);
	Node3D *root = memnew(Node3D);
	viewport.add_child(root);
	root_ = root;
	// Retail shaders write gamma-domain values and rely on one terminal display decode per
	// 3D view; the environment with no .env lights like the retail noon.
	root->add_child(memnew(DisplayDecode));
	environment_ = memnew(MissionEnvironment);
	root->add_child(environment_);
	camera_ = memnew(Camera3D);
	camera_->set_keep_aspect_mode(Camera3D::KEEP_WIDTH);
	camera_->set_fov(opennova::editor::OrbitCamera::fov_horizontal_degrees());
	camera_->set_current(true);
	root->add_child(camera_);
	object_ = memnew(ObjectModel);
	// Every level kept, the level the viewport picks applied as it changes (the build makes every
	// level's meshes): a level change swaps rows, the scene is not built again.
	object_->set_authored_lod_enabled(true);
	object_->set_presenter_driven_lod(true);
	clock_.instantiate();
	object_->set_panm_clock(clock_);
	root->add_child(object_);
	// The first-person arms (DI-13), as the game's view model builds them: no authored levels of their own,
	// the arms part's camo.
	arms_ = memnew(ObjectModel);
	arms_->set_panm_clock(clock_);
	arms_->set_avatar_part(ObjectModel::AVATAR_PART_ARMS);
	arms_->set_viewmodel_rig(true);
	root->add_child(arms_);
}

void ModelViewportApplier::background(opennova::editor::PreviewBackground background) {
	set_preview_backdrop(*backdrop_, background);
}

void ModelViewportApplier::make_fire_() {
	if (effects_) return;
	// A clip's fire (DI-24), made the first time a clip fires here: its effects (the particle renderer composes its
	// passes around the camera, so a picture that never fires keeps its chain), under the environment the particle
	// tints read, and its range.
	effects_ = std::make_unique<PreviewEffects>(*root_);
	effects_->set_environment_source(environment_);
	range_ = std::make_unique<PreviewRangeDraw>(*root_);
}

void ModelViewportApplier::rebuild(const opennova::editor::ViewportModel &viewport, const opennova::editor::SessionView &view,
		const opennova::editor::PreviewClock &) {
	// A build in flight dropped: its data and what it decoded go with it.
	build_.reset();
	const opennova::editor::ModelViewport &model = model_of(viewport);
	if (!model.model() || !view.findings.assets) {
		clear();
		return;
	}
	auto build = std::make_unique<Build>();
	// The textures read through the project's files, each name and stamp noted (a flipbook frame
	// loads when first drawn: the files stay noting), so a changed one builds again.
	auto files = std::make_shared<opennova::StampedFiles>(view.findings.assets);
	build->files = files;
	build->data.instantiate();
	build->data->open_from_model(model.model(), opennova::to_gd(model.path()), std::make_shared<opennova::TextureFiles>(files));
	// The rig an animation's model plays on, bound with the scene: its bones size the skinned meshes.
	build->skeleton_serial = model.skeleton_serial();
	if (model.skeleton()) {
		build->skeletal.instantiate();
		build->skeletal->set_rig(model.skeleton());
		build->bone_count = build->skeletal->get_bone_count();
	}
	// A weapon's first-person map (DI-13): the arms the viewport read, drawn with the gun's bone array (the
	// game builds the arms over the gun's rig, renderer::fp_viewmodel_spec's witness).
	const opennova::editor::FirstPersonSources &first_person = model.first_person();
	if (first_person.arms_model() && model.skeleton()) {
		build->arms.instantiate();
		build->arms->open_from_model(first_person.arms_model(), opennova::to_gd(first_person.arms_file()),
				std::make_shared<opennova::TextureFiles>(files));
		build->arms_skeletal.instantiate();
		build->arms_skeletal->set_rig(model.skeleton());
		if (const opennova::editor::FirstPersonCharacter *who = first_person.character())
			build->arms_camo = { who->camo[0], who->camo[1], who->camo[2] };
	}
	// The units: the gun's data, then the arms' (skinned for the gun's rig, its bones' count).
	build->parts.emplace_back(build->data, build->skeletal.is_valid(), build->bone_count);
	if (build->arms.is_valid()) build->parts.emplace_back(build->arms, build->skeletal.is_valid(), build->bone_count);
	build_ = std::move(build);
	// A clip's fire (DI-24): the project's files its effects' graphics, the scars' textures and the tracers' smoke are
	// read through (mounted again where one read moved) as a clip fires; a model's picture fires nothing.
	fire_files_ = model.animating() ? view.findings.assets : nullptr;
	if (mounted_) {
		if (fire_files_) {
			effects_->mount(fire_files_);
			range_->set_resource_root(effects_->root());
		} else {
			release_fire_();
		}
	}
}

void ModelViewportApplier::release_fire_() {
	if (!effects_) return;
	effects_->clear();
	effects_->set_environment_source(environment_);
	range_->clear();
	range_->set_resource_root(Ref<ResourceRoot>());
	mounted_ = false;
}

ApplierStep ModelViewportApplier::step(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::PreviewClock &clock, std::string &) {
	Build &build = *build_;
	while (build.part < build.parts.size() && build.parts[build.part].finished()) ++build.part;
	if (build.part < build.parts.size()) {
		build.parts[build.part].step();
		return ApplierStep::More;
	}
	if (!build.assembled) {
		// rebuild() builds only over a model, which open_from_model always holds: no scene unit
		// fails (the protocol's Failed waits for a unit that can).
		DEV_ASSERT(build.data->has_document());
		assemble_(build);
		build.assembled = true;
		return ApplierStep::More;
	}
	// The pose: the state as it is now, as an Update applies it (one that came while the build ran is
	// folded into this).
	apply_state_(viewport, clock);
	build_.reset();
	return ApplierStep::Built;
}

opennova::editor::OperationProgress ModelViewportApplier::progress() const {
	opennova::editor::OperationProgress progress;
	progress.unit = opennova::editor::OperationUnit::Steps;
	if (!build_) return progress;
	// Each part's units, then the scene and the pose.
	for (const ObjectDataBuild &part : build_->parts) {
		progress.done += part.done();
		progress.total += part.total();
	}
	progress.done += build_->assembled ? 1 : 0;
	progress.total += 2;
	// What the next unit makes.
	for (const ObjectDataBuild &part : build_->parts)
		if (!part.finished()) {
			progress.label = part.label();
			return progress;
		}
	progress.label = build_->assembled ? "pose" : "scene";
	return progress;
}

void ModelViewportApplier::assemble_(Build &build) {
	// The scene let go and the rig bound with no data held (nothing built either time), then the data
	// swapped in: the scene built once, with the rig's skeleton and every level, from the caches the
	// texture and mesh units filled.
	if (build.skeletal.is_valid() || object_->get_skeletal_anim().is_valid()) {
		object_->set_object_data(Ref<ObjectData>());
		object_->set_skeletal_anim(build.skeletal);
	}
	object_->set_object_data(build.data);
	data_ = build.data;
	// The first-person arms (DI-13) on their own instance of the gun's rig, or none.
	arms_->set_object_data(Ref<ObjectData>());
	arms_->set_skeletal_anim(build.arms_skeletal);
	if (build.arms.is_valid()) {
		arms_->set_object_data(build.arms);
		arms_->set_graphic_name(build.arms->get_source_path());
		// The arms' own camo triplet, as the game's per-submit writer stores it before each arms submit.
		AvatarDatabase::apply_part_camo(arms_, Vector3i(build.arms_camo[0], build.arms_camo[1], build.arms_camo[2]),
				PlayerViewmodelRig::kCtrlOwnerFpArmsCamo);
	}
	arms_data_ = build.arms;
	files_ = build.files;
	applied_ctrl_.clear();
	applied_lod_ = -1;
	applied_team_ = INT32_MIN + 1; // written again over the new scene
	applied_skeleton_ = build.skeleton_serial;
}

void ModelViewportApplier::update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) {
	apply_state_(model, clock);
}

void ModelViewportApplier::apply_state_(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::PreviewClock &clock) {
	if (applied_skeleton_ != model_of(viewport).skeleton_serial()) bind_rig_(viewport);
	// A weapon's first-person map poses the gun as the game's view model draws it, its clip's bone array as
	// the clip builds it; any other clip poses the model through a Person's bone builder
	// (ObjectModel::set_viewmodel_rig).
	object_->set_viewmodel_rig(model_of(viewport).first_person().active());
	apply_registers_(viewport, clock);
	apply_first_person_registers_(viewport);
	play_clip_(viewport, clock);
	place_camera_(viewport);
}

// A weapon's first-person map (DI-13): the per-submit writers the game's view model runs on every part,
// TEX_TEAM the player's team byte (the engine's fp_ctrl_register_writes and viewmodel_team_byte); none
// written, and any left cleared, for another picture.
void ModelViewportApplier::apply_first_person_registers_(const opennova::editor::ViewportModel &viewport) {
	const opennova::editor::FirstPersonSources &first_person = model_of(viewport).first_person();
	const int team = first_person.active() ? opennova::renderer::viewmodel_team_byte(first_person.team()) : INT32_MIN;
	if (team == applied_team_) return;
	applied_team_ = team;
	for (ObjectModel *part : { object_, arms_ }) {
		const bool arms = part == arms_;
		const opennova::world::FpCtrlRegisterWrites writes =
				opennova::world::fp_ctrl_register_writes(first_person.active(), true, false, arms);
		part->begin_ctrl_update();
		PlayerViewmodelRig::write_fp_team(*part, writes.team && team != INT32_MIN, team);
		part->end_ctrl_update();
	}
}

void ModelViewportApplier::clear() {
	build_.reset();
	object_->set_object_data(Ref<ObjectData>());
	data_.unref();
	arms_->set_object_data(Ref<ObjectData>());
	arms_data_.unref();
	files_.reset();
	applied_ctrl_.clear();
	applied_lod_ = -1;
	applied_team_ = INT32_MIN + 1;
	release_fire_();
	fire_files_.reset();
	if (single_sampled_) {
		viewport_->set_msaa_3d(Viewport::MSAA_4X);
		single_sampled_ = false;
	}
}

// The CTRL registers the picture reads at the clock (a register let go reads 0 again), and the sections
// the death pieces left hidden; nothing when the model holds them already (every pump applies the state,
// and every frame while the clock runs: the destroy fade moves with it).
void ModelViewportApplier::apply_registers_(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::PreviewClock &clock) {
	const opennova::editor::ModelViewport &model = model_of(viewport);
	// The model keeps its mask across the scenes it builds (each part made visible by it).
	const uint32_t hidden = model.hidden_sections_at(clock);
	if (hidden != applied_hidden_) {
		object_->set_destroyed_section_mask(int64_t(hidden));
		applied_hidden_ = hidden;
	}
	const std::map<std::string, int64_t> held = model.ctrl_at(clock);
	if (held == applied_ctrl_) return;
	object_->begin_ctrl_update();
	for (const auto &entry : applied_ctrl_) {
		if (held.find(entry.first) == held.end()) object_->clear_ctrl_value(opennova::to_gd(entry.first));
	}
	for (const auto &entry : held) {
		object_->set_ctrl_value(opennova::to_gd(entry.first), entry.second);
	}
	object_->end_ctrl_update();
	applied_ctrl_ = held;
}

void ModelViewportApplier::place_camera_(const opennova::editor::ViewportModel &viewport) {
	const opennova::editor::ModelViewport &model = model_of(viewport);
	const opennova::editor::OrbitCamera &camera = model.camera();
	opennova::editor::PreviewVec3 right, up, back;
	camera.axes(right, up, back);
	camera_->set_transform(Transform3D(Basis(to_godot(right), to_godot(up), to_godot(back)), to_godot(camera.eye())));
	camera_->set_near(camera.near_plane);
	camera_->set_far(camera.far_plane);
	// The first-person eye sees with the weapon's renderfov (DI-13), else the game's view.
	camera_->set_fov(camera.fov_degrees());
	if (model.status() != opennova::editor::ViewportStatus::Ready || data_.is_null()) return;
	const int lod = model.lod();
	if (lod >= 0 && lod != applied_lod_) {
		object_->set_active_lod(lod);
		applied_lod_ = lod;
	}
}

void ModelViewportApplier::bind_rig_(const opennova::editor::ViewportModel &viewport) {
	const opennova::editor::ModelViewport &model = model_of(viewport);
	applied_skeleton_ = model.skeleton_serial();
	if (!model.skeleton() || data_.is_null()) {
		object_->set_skeletal_anim(Ref<SkeletalAnim>());
		if (arms_data_.is_valid()) arms_->set_skeletal_anim(Ref<SkeletalAnim>());
		return;
	}
	Ref<SkeletalAnim> skeletal;
	skeletal.instantiate();
	skeletal->set_rig(model.skeleton());
	object_->set_skeletal_anim(skeletal);
	// The arms ride their own instance of the gun's rig (DI-13).
	if (arms_data_.is_valid()) {
		Ref<SkeletalAnim> arms;
		arms.instantiate();
		arms->set_rig(model.skeleton());
		arms_->set_skeletal_anim(arms);
	}
}

void ModelViewportApplier::play_clip_(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &clock) {
	const opennova::editor::ModelViewport &model = model_of(viewport);
	// The clip's own tick (a repeated one-shot's taken again from its start): the portable half's.
	if (model.skeleton() && !model.clip_key().empty() && data_.is_valid()) {
		object_->play_body_clip_at(opennova::to_gd(model.clip_key()), int(model.clip_ticks(clock)), model.clip_variant());
		// The first-person arms pose by the same clip on the gun's rig (DI-13).
		if (arms_data_.is_valid())
			arms_->play_body_clip_at(opennova::to_gd(model.clip_key()), int(model.clip_ticks(clock)), model.clip_variant());
	}
}

void ModelViewportApplier::apply_fire_(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::PreviewClock &clock) {
	const opennova::editor::ClipFire &fire = model_of(viewport).clip_fire();
	const bool firing = fire_files_ && fire.armed();
	if (firing && !mounted_) {
		make_fire_();
		effects_->mount(fire_files_);
		range_->set_resource_root(effects_->root());
		mounted_ = true;
	}
	if (firing != single_sampled_) {
		viewport_->set_msaa_3d(firing ? Viewport::MSAA_DISABLED : Viewport::MSAA_4X);
		single_sampled_ = firing;
	}
	if (!firing) {
		if (!effects_) return;
		range_->clear();
		if (effects_->scene()) {
			// The last scene's particles let go: drawn once with none.
			effects_->show(nullptr);
			effects_->render(int64_t(clock.ms()));
		}
		return;
	}
	opennova::editor::PreviewVec3 corners[4];
	range_->show_target(fire.target_corners(corners) ? corners : nullptr);
	range_->show_scars(fire.range().serial(), fire.range().scar_count(), [&fire]() { return fire.scars(); });
	range_->show_tracers(fire.trails(), *camera_, int64_t(clock.ms()));
	const std::shared_ptr<opennova::particle::EffectScene> &scene = fire.effects().scene();
	if (scene != effects_->scene()) effects_->show(scene);
}

void ModelViewportApplier::apply(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &clock,
		opennova::editor::ViewportDeviceReport &report) {
	if (build_) {
		// A build runs: the textures its units read so far, and nothing applied over a scene it may be
		// swapping (its pose applies the state as it ends).
		report.files = build_->files->stamps();
		return;
	}
	apply_state_(viewport, clock);
	apply_fire_(viewport, clock);
	if (files_) report.files = files_->stamps();
	if (mounted_) {
		report.files.add(effects_->stamps());
		report.missing = effects_->missing();
	}
}

void ModelViewportApplier::tick(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &clock) {
	if (!build_) {
		apply_registers_(viewport, clock);
		play_clip_(viewport, clock);
		apply_fire_(viewport, clock);
	}
	// A model with live part animations or a dynamic material stays awake and reads it.
	clock_->sample(int64_t(clock.ms()), ++frame_);
	// The clip fire's effects as the viewport stepped them (DI-24).
	if (mounted_ && effects_->scene()) effects_->render(int64_t(clock.ms()));
}

} // namespace godot
