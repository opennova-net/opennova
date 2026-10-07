#include "authoring/definition_viewport_applier.h"

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <editor/assets/project_asset_source.h>
#include <editor/preview/definition_viewport.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/view/session_view.h>
#include <runtime/renderer/fp_viewmodel_spec.h>
#include <runtime/world/impact_scar.h>
#include <runtime/world/player_present.h>

#include "authoring/effect_viewport_applier.h"
#include "env/mission_environment.h"
#include "render/frame_fx.h"
#include "util/color_convert.h"
#include "world/scar_draw_list.h"
#include "world/scar_presenter.h"

namespace godot {

namespace {

const opennova::editor::DefinitionViewport &definition_of(const opennova::editor::ViewportModel &model) {
	return static_cast<const opennova::editor::DefinitionViewport &>(model);
}

Vector3 to_godot(const opennova::editor::PreviewVec3 &v) {
	return Vector3(v.x, v.y, v.z);
}

// The scar draw list the editor's range compiled (in the preview's space, the device's) as the record the game's
// ScarPresenter uploads: the shared ring's quads as they stand, the strip table (the TGA name and the mode word
// each strip's effect is built from [orig: Scar_LoadTextures @0x5CC2E0]).
Ref<ScarDrawList> scar_record(const opennova::renderer::ScarDrawList &list) {
	Ref<ScarDrawList> out;
	out.instantiate();
	PackedVector3Array vertices;
	PackedVector2Array uvs;
	PackedColorArray colors;
	vertices.resize(int64_t(list.vertices.size()));
	uvs.resize(int64_t(list.vertices.size()));
	colors.resize(int64_t(list.vertices.size()));
	for (size_t i = 0; i < list.vertices.size(); ++i) {
		const opennova::renderer::ScarVertex &v = list.vertices[i];
		vertices[int64_t(i)] = Vector3(v.x, v.y, v.z);
		uvs[int64_t(i)] = Vector2(v.u, v.v);
		colors[int64_t(i)] = opennova::color_from_argb(v.argb);
	}
	PackedInt32Array owner, texture, section, flags, first, count, bms, world_first;
	PackedInt64Array spawn_origin;
	for (const opennova::renderer::ScarDrawBatch &batch : list.batches) {
		if (batch.entity_local) continue; // the range's target writes the shared ring
		owner.push_back(batch.owner_packed);
		texture.push_back(batch.texture);
		section.push_back(batch.section);
		flags.push_back(batch.building ? ScarDrawList::FLAG_BUILDING : 0);
		first.push_back(int32_t(batch.first_vertex));
		count.push_back(int32_t(batch.vertex_count));
		bms.push_back(0);
		spawn_origin.push_back(0);
		world_first.push_back(-1);
	}
	PackedStringArray strip_names;
	PackedInt32Array strip_mode_words;
	strip_names.resize(opennova::world::kScarTextureStripCount);
	strip_mode_words.resize(opennova::world::kScarTextureStripCount);
	for (int strip = 0; strip < opennova::world::kScarTextureStripCount; ++strip) {
		strip_names[strip] = String(opennova::world::scar_texture_strip_name(strip));
		strip_mode_words[strip] = int32_t(opennova::world::scar_texture_strip_mode_word(strip));
	}
	out->set_vertices(vertices);
	out->set_uvs(uvs);
	out->set_colors(colors);
	out->set_batch_owner(owner);
	out->set_batch_texture(texture);
	out->set_batch_section(section);
	out->set_batch_flags(flags);
	out->set_batch_first(first);
	out->set_batch_count(count);
	out->set_batch_bms_id(bms);
	out->set_batch_spawn_origin(spawn_origin);
	out->set_batch_world_first(world_first);
	out->set_strip_names(strip_names);
	out->set_strip_mode_words(strip_mode_words);
	out->set_slots_live(int(list.slots_live));
	out->set_slots_culled(int(list.slots_culled));
	return out;
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
	// A weapon's first-person arms (DI-22).
	arms_ = std::make_unique<PreviewModel>(*root);
	arms_->set_arms(0, 0, 0);
	effects_ = std::make_unique<PreviewEffects>(*root);
	effects_->set_environment_source(environment_);
	// A weapon's range: its target's face, its tracers, the scars on the face.
	target_mesh_.instantiate();
	target_ = memnew(MeshInstance3D);
	target_->set_name("Target");
	target_->set_mesh(target_mesh_);
	target_->set_visible(false);
	root->add_child(target_);
	tracer_mesh_.instantiate();
	tracers_ = memnew(MeshInstance3D);
	tracers_->set_name("Tracers");
	tracers_->set_mesh(tracer_mesh_);
	tracers_->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	root->add_child(tracers_);
	scars_ = memnew(ScarPresenter);
	scars_->set_name("Scars");
	root->add_child(scars_);
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
	// person's or a first-person gun's meshes skinned for its rig.
	auto files = std::make_shared<opennova::editor::StampedFiles>(view.findings.assets);
	model_->begin(model.model(), model.drawn().file, files, model.skeleton());
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
	scars_->set_resource_root(effects_->root());
	ribbons_.set_resource_root(effects_->root());
	scars_shown_ = UINT64_MAX;
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
	tracer_mesh_->clear_surfaces();
	target_->set_visible(false);
	scars_->clear();
	scars_shown_ = UINT64_MAX;
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
	if (!model_->built()) return;
	model_->set_registers(model.ctrl_at(clock));
	model_->set_hidden_sections(model.hidden_sections_at(clock));
	model_->set_level(model.lod());
	// A person stands where its spawn stands it, posed as its warmup leaves it.
	const opennova::editor::MissionPose &person = model.person();
	const bool posed = person.status == "posed" && model.skeleton();
	model_->set_lift(posed ? float(person.lift) : 0.0f);
	if (posed) model_->pose_body(person.pose);
	apply_weapon_(viewport, clock);
}

void DefinitionViewportApplier::apply_weapon_(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::PreviewClock &clock) {
	const opennova::editor::DefinitionViewport &model = definition_of(viewport);
	const bool weapon = model.weapon_record();
	const opennova::editor::DefinitionWeapon &fire = model.weapon();
	// The SIGHTS card replaces the view model while it is up: the frame draws one or the other.
	const bool card = weapon && fire.card_up();
	model_->object()->set_visible(!card);
	arms_->object()->set_visible(!card);
	// The gun and the arms posed by the first-person channel as the weapon's pump left it, both parts by the one
	// latch [orig: AnimMap_PlayAnimBySlot @0x40bda0], at its gated ticks (nothing free-runs the playhead).
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
	const int team = first_person ? opennova::renderer::viewmodel_team_byte(fire.first_person().team()) : INT32_MIN;
	if (team != applied_team_ && model_->built()) {
		applied_team_ = team;
		static const String kOwner("first_person:team");
		for (PreviewModel *part : {model_.get(), arms_.get()}) {
			ObjectModel *object = part->object();
			if (!part->built()) continue;
			const opennova::world::FpCtrlRegisterWrites writes =
					opennova::world::fp_ctrl_register_writes(first_person, true, false, part == arms_.get());
			object->begin_ctrl_update();
			if (writes.team && team != INT32_MIN) object->set_ctrl_override(kOwner, "TEX_TEAM", team);
			else object->clear_ctrl_override(kOwner, "TEX_TEAM");
			object->end_ctrl_update();
		}
	}
	// The target's face (the editor's aid): a grey quad where the range stands it.
	opennova::editor::PreviewVec3 corners[4];
	const bool target = weapon && fire.target_corners(corners);
	target_->set_visible(target);
	if (target) {
		target_mesh_->clear_surfaces();
		PackedVector3Array positions;
		PackedVector3Array normals;
		const Vector3 a = to_godot(corners[0]), b = to_godot(corners[1]), c = to_godot(corners[2]), d = to_godot(corners[3]);
		const Vector3 normal = (b - a).cross(d - a).normalized();
		for (const Vector3 &v : {a, b, c, a, c, d}) {
			positions.push_back(v);
			normals.push_back(normal);
		}
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = positions;
		arrays[Mesh::ARRAY_NORMAL] = normals;
		target_mesh_->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		Ref<StandardMaterial3D> material;
		material.instantiate();
		material->set_albedo(Color(0.42f, 0.42f, 0.40f));
		material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		material->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
		target_mesh_->surface_set_material(0, material);
	}
	// The scars on the face, uploaded again as the range moved.
	if (!weapon) {
		if (scars_shown_ != UINT64_MAX) scars_->clear();
		scars_shown_ = UINT64_MAX;
	} else if (mounted_ && fire.range().serial() != scars_shown_) {
		scars_shown_ = fire.range().serial();
		if (fire.range().scar_count() > 0) scars_->present(scar_record(fire.scars()), Dictionary());
		else scars_->clear();
	}
	// The tracers, built as the game's ribbon pass builds them against this camera.
	tracer_mesh_->clear_surfaces();
	if (!weapon || !mounted_) return;
	const std::vector<opennova::editor::DefinitionTrail> trails = fire.trails();
	if (trails.empty()) return;
	size_t floats = 0;
	for (const opennova::editor::DefinitionTrail &trail : trails) floats += trail.points.size() * 4;
	tracer_points_.assign(floats, 0.0f);
	std::vector<opennova::renderer::TracerChannelInput> channels;
	size_t at = 0;
	for (const opennova::editor::DefinitionTrail &trail : trails) {
		opennova::renderer::TracerChannelInput channel;
		channel.style_id = trail.style;
		channel.age = trail.age;
		channel.count = int(trail.points.size());
		channel.points = tracer_points_.data() + at;
		for (size_t i = 0; i < trail.points.size(); ++i) {
			tracer_points_[at++] = trail.points[i].x;
			tracer_points_[at++] = trail.points[i].y;
			tracer_points_[at++] = trail.points[i].z;
			tracer_points_[at++] = i < trail.widths.size() ? trail.widths[i] : 1.0f;
		}
		channels.push_back(channel);
	}
	const Transform3D eye = camera_->get_global_transform();
	const Vector3 forward = -eye.basis.get_column(2);
	opennova::renderer::TracerView view;
	view.camera = {float(eye.origin.x), float(eye.origin.y), float(eye.origin.z)};
	view.forward = {float(forward.x), float(forward.y), float(forward.z)};
	view.projection_x_scale = float(camera_->get_camera_projection()[0][0]);
	view.tick_ms = uint32_t(clock.ms());
	opennova::renderer::compile_tracer_ribbons(channels.data(), channels.size(), view,
			opennova::renderer::TracerPass::Main, tracer_frame_);
	ribbons_.emit(tracer_frame_, tracer_mesh_, opennova::renderer::tracer_rung(true));
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
	if (!building() && model_->built()) {
		// The destroy fade and the pieces' sections move with the clock; a weapon's channel, tracers and scars too.
		model_->set_registers(model.ctrl_at(clock));
		model_->set_hidden_sections(model.hidden_sections_at(clock));
		place_(viewport);
		apply_weapon_(viewport, clock);
	}
	model_->tick(int64_t(clock.ms()));
	arms_->tick(int64_t(clock.ms()));
	if (!mounted_) return;
	// The scene the viewport stepped, drawn as it stands.
	show_effects_(viewport);
	effects_->render(int64_t(clock.ms()));
}

} // namespace godot
