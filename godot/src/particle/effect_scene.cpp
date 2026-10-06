#include "particle/effect_scene.h"
#include <runtime/particle/script_effects.h>
#include "particle/effect_load_report.h"
#include "particle/effect_spawn_records.h"
#include "particle/particle_convert.h"
#include "util/string_convert.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

using namespace godot;
using opennova::to_gd;

namespace {

using opennova::particle::CurveRef;
using opennova::particle::EffectPose;
using opennova::particle::EffectSpawnStatus;
using opennova::particle::GraphicLayer;
using opennova::particle::ParticleDef;
using opennova::particle::Vec3;

std::uint64_t token_from_godot(int64_t value) noexcept {
	std::uint64_t result = 0;
	std::memcpy(&result, &value, sizeof(result));
	return result;
}

std::uint32_t handle_from_godot(int64_t value) noexcept {
	if (value <= 0 ||
			static_cast<std::uint64_t>(value) >
					std::numeric_limits<std::uint32_t>::max()) {
		return 0;
	}
	return static_cast<std::uint32_t>(value);
}

std::uint32_t non_negative_u32(int64_t value) noexcept {
	if (value <= 0) {
		return 0;
	}
	return static_cast<std::uint32_t>(std::min<std::uint64_t>(
			static_cast<std::uint64_t>(value),
			std::numeric_limits<std::uint32_t>::max()));
}

Vec3 native_vector(const Vector3 &value) noexcept {
	return {value.x, value.y, value.z};
}

EffectPose native_pose(const Transform3D &value) noexcept {
	EffectPose result;
	result.position = native_vector(value.origin);
	result.right = native_vector(value.basis.get_column(0));
	result.up = native_vector(value.basis.get_column(1));
	result.forward = native_vector(value.basis.get_column(2));
	return result;
}

std::size_t capacity_from_option(const Dictionary &options,
		const char *key, int64_t fallback) {
	const int64_t value = options.get(key, fallback);
	return value > 0 ? static_cast<std::size_t>(value) : 0;
}

String spawn_status_name(EffectSpawnStatus status) {
	switch (status) {
		case EffectSpawnStatus::Spawned: return "spawned";
		case EffectSpawnStatus::Suppressed: return "suppressed";
		case EffectSpawnStatus::InvalidHandle: return "invalid_handle";
		case EffectSpawnStatus::EmptyEffect: return "empty_effect";
		case EffectSpawnStatus::MissingSlot: return "missing_slot";
		case EffectSpawnStatus::MissingOwner: return "missing_owner";
		case EffectSpawnStatus::GroupCapacityReached:
			return "group_capacity_reached";
		case EffectSpawnStatus::EmitterCapacityReached:
			return "emitter_capacity_reached";
	}
	return "unknown";
}

Ref<EffectSpawnReceipt> spawn_receipt_record(const opennova::particle::EffectSpawnReceipt &receipt) {
	Ref<EffectSpawnReceipt> result;
	result.instantiate();
	result->set_status(static_cast<int>(receipt.status));
	result->set_status_name(spawn_status_name(receipt.status));
	result->set_effect_handle(static_cast<int64_t>(receipt.effect.value));
	result->set_group_id(token_to_godot(receipt.group.value));
	result->set_replaced_group_id(token_to_godot(receipt.replaced_group.value));
	result->set_spawned(receipt.spawned());
	result->set_accepted(receipt.accepted());
	return result;
}

Ref<EffectSpawnReceipt> invalid_spawn_request(const String &message) {
	Ref<EffectSpawnReceipt> result;
	result.instantiate();
	result->set_status(EffectScene::SPAWN_STATUS_INVALID_REQUEST);
	result->set_status_name("invalid_request");
	result->set_message(message);
	return result;
}

bool valid_admission(int value) noexcept {
	return value >= EffectScene::ADMISSION_ALWAYS &&
			value <= EffectScene::ADMISSION_STORE_OWNED;
}

bool valid_binding(int value) noexcept {
	return value >= EffectScene::BINDING_WORLD &&
			value <= EffectScene::BINDING_FOLLOW_OWNER;
}

bool valid_render_domain(int value) noexcept {
	return value >= EffectScene::RENDER_DOMAIN_WORLD &&
			value <= EffectScene::RENDER_DOMAIN_FIRST_PERSON;
}

bool valid_kill_plane(int value) noexcept {
	return value >= EffectScene::KILL_PLANE_DISABLED &&
			value <= EffectScene::KILL_PLANE_AT_OR_BELOW;
}

} // namespace

EffectScene::EffectScene() = default;

void EffectScene::_materialize_snapshot() const {
	if (!snapshot_dirty_)
		return;
	scene_->write_snapshot(last_frame_);
	snapshot_dirty_ = false;
}

void EffectScene::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open", "files", "options"),
			&EffectScene::open, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("intern", "effect_name"),
			&EffectScene::intern);
	ClassDB::bind_method(D_METHOD("effect_name", "effect_handle"),
			&EffectScene::effect_name);
	ClassDB::bind_method(D_METHOD("spawn", "request"),
			&EffectScene::spawn);
	ClassDB::bind_method(D_METHOD("detach", "group_id"),
			&EffectScene::detach);
	ClassDB::bind_method(D_METHOD("set_group_parameters", "group_id", "rate_control",
					"offset_control"),
			&EffectScene::set_group_parameters);
	ClassDB::bind_method(D_METHOD("reset_runtime_state"),
			&EffectScene::reset_runtime_state);
	ClassDB::bind_method(D_METHOD("advance_in_place", "delta_seconds"),
			&EffectScene::advance_in_place);

	BIND_ENUM_CONSTANT(ADMISSION_ALWAYS);
	BIND_ENUM_CONSTANT(ADMISSION_REPLACE_OWNED);
	BIND_ENUM_CONSTANT(ADMISSION_SUPPRESS_WHILE_OWNED);
    BIND_ENUM_CONSTANT(ADMISSION_STORE_OWNED);
	BIND_ENUM_CONSTANT(BINDING_WORLD);
	BIND_ENUM_CONSTANT(BINDING_FOLLOW_OWNER);
	BIND_ENUM_CONSTANT(RENDER_DOMAIN_WORLD);
	BIND_ENUM_CONSTANT(RENDER_DOMAIN_FIRST_PERSON);

	BIND_ENUM_CONSTANT(KILL_PLANE_DISABLED);
	BIND_ENUM_CONSTANT(SPAWN_STATUS_SPAWNED);
}

Ref<EffectLoadReport> EffectScene::open(
		const TypedArray<ParticleFile> &p_files,
		const Dictionary &p_options) {
	opennova::particle::EffectSceneConfig config;
	config.simulation_tick_seconds = static_cast<float>(
			static_cast<double>(p_options.get(
					"simulation_tick_seconds", 1.0 / 62.5)));
	config.max_live_groups =
			capacity_from_option(p_options, "max_live_groups", 1024);
	config.max_live_emitters =
			capacity_from_option(p_options, "max_live_emitters", 4096);
	config.random_seed = static_cast<std::uint32_t>(
			static_cast<int64_t>(p_options.get("random_seed", 1)));

	config.documents.reserve(static_cast<std::size_t>(p_files.size()));
	int64_t ignored_document_count = 0;
	for (int64_t i = 0; i < p_files.size(); ++i) {
		Ref<ParticleFile> file = p_files[i];
		if (file.is_null()) {
			++ignored_document_count;
			continue;
		}
		opennova::particle::EffectCatalogDocument document;
		document.source = opennova::to_std(file->get_source_path());
		document.file = file->native_file();
		config.documents.push_back(std::move(document));
	}

	const opennova::particle::EffectLoadReport report = scene_->open(config);
	advance_in_place(0.0);
	Ref<godot::EffectLoadReport> result;
	result.instantiate();
	result->assign(report, static_cast<int>(p_files.size()),
			static_cast<int>(ignored_document_count));
	return result;
}

void EffectScene::spawn_script_effect(const opennova::world::ScriptEffectEvent &event,
        int64_t slot, int64_t owner, uint32_t age_ticks, float water_height,
        const opennova::particle::EffectSectionGate &gate) {
    opennova::particle::spawn_script_effect(*scene_, event,
            {token_from_godot(slot)}, {token_from_godot(owner)}, age_ticks, water_height, gate);
    snapshot_dirty_ = true;
}

int64_t EffectScene::intern(const String &p_effect_name) {
	const opennova::particle::EffectHandle handle =
			scene_->intern(p_effect_name.utf8().get_data());
	return static_cast<int64_t>(handle.value);
}

String EffectScene::effect_name(int64_t p_effect_handle) const {
	return to_gd(scene_->effect_name(
			opennova::particle::EffectHandle{
				handle_from_godot(p_effect_handle)
			}));
}

Ref<EffectSpawnReceipt> EffectScene::spawn(const Ref<EffectSpawnRequest> &p_request) {
	if (p_request.is_null()) {
		return invalid_spawn_request("null request");
	}
	const int admission = p_request->get_admission();
	const int binding = p_request->get_binding();
	const int render_domain = p_request->get_render_domain();
	const int kill_plane = p_request->get_kill_plane();

	if (!valid_admission(admission)) {
		return invalid_spawn_request("invalid admission");
	}
	if (!valid_binding(binding)) {
		return invalid_spawn_request("invalid binding");
	}
	if (!valid_render_domain(render_domain)) {
		return invalid_spawn_request("invalid render_domain");
	}
	if (!valid_kill_plane(kill_plane)) {
		return invalid_spawn_request("invalid kill_plane");
	}

	opennova::particle::EffectSpawnRequest request;
	request.effect.value = handle_from_godot(p_request->get_effect_handle());
	request.pose = native_pose(p_request->get_transform());
	request.admission =
			static_cast<opennova::particle::EffectAdmission>(admission);
	request.binding =
			static_cast<opennova::particle::EffectBinding>(binding);
	request.render_domain =
			static_cast<opennova::particle::EffectRenderDomain>(render_domain);
	request.slot.value = token_from_godot(p_request->get_slot_token());
	request.owner.value = token_from_godot(p_request->get_owner_token());

	request.owner_relative_pose = native_pose(p_request->get_owner_relative_transform());
	request.initial_age_ticks = non_negative_u32(p_request->get_initial_age_ticks());
	request.force_zone = uint16_t(p_request->get_force_zone());
	request.source_tick = token_from_godot(p_request->get_source_tick());
	request.source_order = token_from_godot(p_request->get_source_order());
	request.spring_const = p_request->get_spring_const();
	request.lod_divisor = std::max<std::uint32_t>(
			non_negative_u32(p_request->get_lod_divisor()), 1u);

	request.kill_plane =
			static_cast<opennova::particle::EffectKillPlane>(kill_plane);
	request.kill_plane_y = p_request->get_kill_plane_y();
	request.section_gate.tagged = p_request->get_section_tagged();
	const PackedInt64Array hits = p_request->get_blink_hits();
	for (int64_t i = 0; i < hits.size() &&
			i < static_cast<int64_t>(request.section_gate.blink_hits.size()); ++i) {
		request.section_gate.blink_hits[static_cast<std::size_t>(i)] =
				static_cast<std::uint32_t>(hits[i]);
	}
	const opennova::particle::EffectSpawnReceipt receipt = scene_->spawn(request);
	if (receipt.spawned()) {
		advance_in_place(0.0);
	}
	return spawn_receipt_record(receipt);
}

void EffectOwnerPoseBatch::add(int64_t p_owner_token, const Transform3D &p_transform) {
	opennova::particle::EffectOwnerPoseUpdate update;
	update.owner.value = token_from_godot(p_owner_token);
	update.pose = native_pose(p_transform);
	update.present = true;
	updates_.push_back(update);
}

void EffectOwnerPoseBatch::add_absent(int64_t p_owner_token) {
	opennova::particle::EffectOwnerPoseUpdate update;
	update.owner.value = token_from_godot(p_owner_token);
	update.present = false;
	updates_.push_back(update);
}

void EffectScene::apply_owner_poses_in_place(const EffectOwnerPoseBatch &p_batch) {
	scene_->apply_owner_poses(p_batch.updates());
	snapshot_dirty_ = true;
}

void EffectScene::apply_owner_poses(const EffectOwnerPoseBatch &p_batch) {
	apply_owner_poses_in_place(p_batch);
	advance_in_place(0.0);
}

void EffectScene::detach(int64_t p_group_id) {
	scene_->detach(opennova::particle::EffectGroupId{
		token_from_godot(p_group_id)
	});
	advance_in_place(0.0);
}

void EffectScene::detach_slot(int64_t p_slot_token) {
	scene_->detach_slot(opennova::particle::EffectSlotToken{
		token_from_godot(p_slot_token)
	});
	advance_in_place(0.0);
}

bool EffectScene::set_group_parameters(int64_t p_group_id, float p_rate_control,
		float p_offset_control) {
	const bool changed = scene_->set_group_parameters(
			opennova::particle::EffectGroupId{ token_from_godot(p_group_id) },
			p_rate_control, p_offset_control);
	if (changed) {
		snapshot_dirty_ = true;
	}
	return changed;
}

bool EffectScene::trigger_group_children(int64_t p_group_id, const Vector3 &p_position,
		const Vector3 &p_forward, int p_force_zone) {
	const bool triggered = scene_->trigger_group_children(
			opennova::particle::EffectGroupId{ token_from_godot(p_group_id) },
			opennova::particle::Vec3{ float(p_position.x), float(p_position.y),
					float(p_position.z) },
			opennova::particle::Vec3{ float(p_forward.x), float(p_forward.y),
					float(p_forward.z) },
			static_cast<std::uint16_t>(p_force_zone & 0xFFFF));
	if (triggered) {
		snapshot_dirty_ = true;
	}
	return triggered;
}

void EffectScene::reset_runtime_state() {
	scene_->reset_runtime_state();
	snapshot_dirty_ = true;
}

void EffectScene::advance_in_place(double p_delta_seconds) {
	advance_with_forces(p_delta_seconds, nullptr);
}

void EffectScene::advance_with_forces(
		double p_delta_seconds, const opennova::particle::ParticleForceField *forces) {
	opennova::particle::EffectAdvanceRequest request;
	request.delta_seconds = static_cast<float>(p_delta_seconds);
	request.forces = forces;
	request.frustum = frustum_;
	request.section_masks = section_masks_;
	scene_->advance_simulation(request);
	snapshot_dirty_ = true;
}

void EffectScene::set_global_wind(const Vector3 &p_wind) {
	scene_->set_global_wind(native_vector(p_wind));
}

void EffectScene::set_view_frustum(const TypedArray<Plane> &p_planes,
		const Vector3 &p_inside_probe) {
	frustum_ = {};
	if (p_planes.size() != 6) {
		return;
	}
	// Godot planes answer `normal . p - d`; the simulator wants
	// `a x + b y + c z + d >= 0` inside. Camera3D::get_frustum points its
	// normals outward, so the inside probe reads negative ("under") and the
	// set is flipped; a plane set that already reads the probe inside is kept.
	float sign = 1.0f;
	{
		const Plane first = p_planes[0];
		if (first.normal.dot(p_inside_probe) - first.d < 0.0f) {
			sign = -1.0f;
		}
	}
	for (int index = 0; index < 6; ++index) {
		const Plane plane = p_planes[index];
		if (!std::isfinite(plane.normal.x) || !std::isfinite(plane.normal.y) ||
				!std::isfinite(plane.normal.z) || !std::isfinite(plane.d)) {
			frustum_ = {};
			return;
		}
		frustum_.planes[index][0] = sign * plane.normal.x;
		frustum_.planes[index][1] = sign * plane.normal.y;
		frustum_.planes[index][2] = sign * plane.normal.z;
		frustum_.planes[index][3] = -sign * plane.d;
	}
	frustum_.valid = true;
}

void EffectScene::clear_view_frustum() {
	frustum_ = {};
}

void EffectScene::share_native_scene(std::shared_ptr<opennova::particle::EffectScene> p_scene) {
	if (!p_scene) {
		p_scene = std::make_shared<opennova::particle::EffectScene>();
		p_scene->open(opennova::particle::EffectSceneConfig());
	}
	scene_ = std::move(p_scene);
	snapshot_dirty_ = true;
}

const opennova::particle::ParticleFrameSnapshot &
EffectScene::native_frame_snapshot() const {
	_materialize_snapshot();
	return last_frame_;
}
