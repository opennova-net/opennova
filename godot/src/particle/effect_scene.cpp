#include "particle/effect_scene.h"
#include "particle/effect_load_report.h"
#include "util/string_convert.h"

#include <algorithm>
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

using opennova::particle::Color3;
using opennova::particle::CurveRef;
using opennova::particle::EffectBounds;
using opennova::particle::EffectPose;
using opennova::particle::EffectSpawnReceipt;
using opennova::particle::EffectSpawnStatus;
using opennova::particle::GraphicLayer;
using opennova::particle::ParticleDef;
using opennova::particle::Vec3;

std::uint64_t token_from_godot(int64_t value) noexcept {
	std::uint64_t result = 0;
	std::memcpy(&result, &value, sizeof(result));
	return result;
}

int64_t token_to_godot(std::uint64_t value) noexcept {
	int64_t result = 0;
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

Vector3 godot_vector(Vec3 value) noexcept {
	return Vector3(value.x, value.y, value.z);
}

EffectPose native_pose(const Transform3D &value) noexcept {
	EffectPose result;
	result.position = native_vector(value.origin);
	result.right = native_vector(value.basis.get_column(0));
	result.up = native_vector(value.basis.get_column(1));
	result.forward = native_vector(value.basis.get_column(2));
	return result;
}

Transform3D godot_pose(const EffectPose &value) noexcept {
	Basis basis;
	basis.set_column(0, godot_vector(value.right));
	basis.set_column(1, godot_vector(value.up));
	basis.set_column(2, godot_vector(value.forward));
	return Transform3D(basis, godot_vector(value.position));
}

Color godot_color(Color3 value, std::uint8_t alpha = 255) noexcept {
	return Color(
			static_cast<float>(value.r) / 255.0f,
			static_cast<float>(value.g) / 255.0f,
			static_cast<float>(value.b) / 255.0f,
			static_cast<float>(alpha) / 255.0f);
}

std::size_t capacity_from_option(const Dictionary &options,
		const char *key, int64_t fallback) {
	const int64_t value = options.get(key, fallback);
	return value > 0 ? static_cast<std::size_t>(value) : 0;
}

Dictionary bounds_dictionary(const EffectBounds &bounds) {
	Dictionary result;
	result["valid"] = bounds.valid;
	result["minimum"] = godot_vector(bounds.minimum);
	result["maximum"] = godot_vector(bounds.maximum);
	if (bounds.valid) {
		const Vector3 minimum = godot_vector(bounds.minimum);
		result["aabb"] = AABB(minimum, godot_vector(bounds.maximum) - minimum);
	}
	return result;
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

Dictionary spawn_receipt_dictionary(const EffectSpawnReceipt &receipt) {
	Dictionary result;
	result["status"] = static_cast<int>(receipt.status);
	result["status_name"] = spawn_status_name(receipt.status);
	result["effect_handle"] = static_cast<int64_t>(receipt.effect.value);
	result["group_id"] = token_to_godot(receipt.group.value);
	result["replaced_group_id"] =
			token_to_godot(receipt.replaced_group.value);
	result["spawned"] = receipt.spawned();
	result["accepted"] = receipt.accepted();
	return result;
}

Dictionary invalid_spawn_request(const String &message) {
	Dictionary result;
	result["status"] = EffectScene::SPAWN_STATUS_INVALID_REQUEST;
	result["status_name"] = "invalid_request";
	result["message"] = message;
	result["effect_handle"] = 0;
	result["group_id"] = 0;
	result["replaced_group_id"] = 0;
	result["spawned"] = false;
	result["accepted"] = false;
	return result;
}

bool valid_admission(int value) noexcept {
	return value >= EffectScene::ADMISSION_ALWAYS &&
			value <= EffectScene::ADMISSION_SUPPRESS_WHILE_OWNED;
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

std::vector<opennova::particle::EffectOwnerPoseUpdate> owner_pose_updates(
		const Array &values) {
	std::vector<opennova::particle::EffectOwnerPoseUpdate> updates;
	updates.reserve(static_cast<std::size_t>(values.size()));
	for (int64_t i = 0; i < values.size(); ++i) {
		const Variant item = values[i];
		if (item.get_type() != Variant::DICTIONARY)
			continue;
		const Dictionary value = item;
		opennova::particle::EffectOwnerPoseUpdate update;
		update.owner.value = token_from_godot(
				static_cast<int64_t>(value.get("owner_token", 0)));
		update.pose = native_pose(static_cast<Transform3D>(
				value.get("transform", Transform3D())));
		update.present = static_cast<bool>(value.get("present", true));
		updates.push_back(update);
	}
	return updates;
}

// The inspect() diagnostic embed of the scene's retained load counters.
Dictionary load_report_dictionary(const opennova::particle::EffectLoadReport &report) {
	Dictionary result;
	result["document_count"] = static_cast<int64_t>(report.document_count);
	result["effect_count"] = static_cast<int64_t>(report.effect_count);
	result["particle_definition_count"] =
			static_cast<int64_t>(report.particle_definition_count);
	result["table_definition_count"] =
			static_cast<int64_t>(report.table_definition_count);
	result["duplicate_effect_count"] =
			static_cast<int64_t>(report.duplicate_effect_count);
	result["duplicate_particle_count"] =
			static_cast<int64_t>(report.duplicate_particle_count);
	result["unresolved_particle_reference_count"] =
			static_cast<int64_t>(report.unresolved_particle_reference_count);
	return result;
}

Dictionary debug_dictionary(
		const opennova::particle::EffectDebugSnapshot &debug) {
	Dictionary result;
	result["load"] = load_report_dictionary(debug.load);
	result["interned_effect_count"] =
			static_cast<int64_t>(debug.interned_effect_count);
	result["live_group_count"] =
			static_cast<int64_t>(debug.live_group_count);
	result["live_emitter_count"] =
			static_cast<int64_t>(debug.live_emitter_count);
	result["live_particle_count"] =
			static_cast<int64_t>(debug.live_particle_count);
	result["group_pool_high_water"] =
			static_cast<int64_t>(debug.group_pool_high_water);
	result["emitter_pool_high_water"] =
			static_cast<int64_t>(debug.emitter_pool_high_water);

	result["suppressed_spawn_count"] =
			static_cast<int64_t>(debug.suppressed_spawn_count);
	result["rejected_spawn_count"] =
			static_cast<int64_t>(debug.rejected_spawn_count);
	result["capacity_rejection_count"] =
			static_cast<int64_t>(debug.capacity_rejection_count);

	Array groups;
	groups.resize(static_cast<int64_t>(debug.groups.size()));
	for (std::size_t i = 0; i < debug.groups.size(); ++i) {
		const auto &group = debug.groups[i];
		Dictionary value;
		value["group_id"] = token_to_godot(group.id.value);
		value["effect_handle"] = static_cast<int64_t>(group.effect.value);
		value["effect_name"] = to_gd(group.effect_name);
		value["source"] = to_gd(group.source);
		value["admission"] = static_cast<int>(group.admission);
		value["binding"] = static_cast<int>(group.binding);
		value["render_domain"] = static_cast<int>(group.render_domain);
		value["slot_token"] = token_to_godot(group.slot.value);
		value["owner_token"] = token_to_godot(group.owner.value);
		value["detached"] = group.detached;
		value["transform"] = godot_pose(group.pose);
		value["source_tick"] = token_to_godot(group.source_tick);
		value["source_order"] = token_to_godot(group.source_order);
		value["bounds"] = bounds_dictionary(group.bounds);

		Array emitters;
		emitters.resize(static_cast<int64_t>(group.emitters.size()));
		for (std::size_t j = 0; j < group.emitters.size(); ++j) {
			const auto &emitter = group.emitters[j];
			Dictionary emitter_value;

			emitter_value["emitter_id"] = token_to_godot(emitter.id);
			emitter_value["ordinal"] =
					static_cast<int64_t>(emitter.ordinal);
			emitter_value["definition_index"] =
					static_cast<int64_t>(emitter.definition_index);
			emitter_value["definition_name"] =
					to_gd(emitter.definition_name);
			emitter_value["definition_flags"] =
					static_cast<int64_t>(emitter.definition_flags);
			emitter_value["alive_particle_count"] =
					static_cast<int64_t>(emitter.alive_particle_count);
			emitter_value["emitting"] = emitter.emitting;
			emitter_value["position"] = godot_vector(emitter.position);
			emitter_value["forward"] = godot_vector(emitter.forward);
			emitter_value["age"] = emitter.age;
			emitter_value["kill_plane"] = static_cast<int>(emitter.kill_plane);
			emitter_value["kill_plane_y"] = emitter.kill_plane_y;
			emitter_value["bounds"] = bounds_dictionary(emitter.bounds);
			emitters[static_cast<int64_t>(j)] = emitter_value;
		}
		value["emitters"] = emitters;
		groups[static_cast<int64_t>(i)] = value;
	}
	result["groups"] = groups;
	return result;
}

} // namespace

EffectScene::EffectScene() = default;

void EffectScene::_materialize_snapshot() const {
	if (!snapshot_dirty_)
		return;
	scene_.write_snapshot(last_frame_);
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
	ClassDB::bind_method(D_METHOD("apply_owner_poses_in_place", "updates"),
			&EffectScene::apply_owner_poses_in_place);
	ClassDB::bind_method(D_METHOD("apply_owner_poses", "updates"),
			&EffectScene::apply_owner_poses);
	ClassDB::bind_method(D_METHOD("get_active_owner_tokens"),
			&EffectScene::get_active_owner_tokens);
	ClassDB::bind_method(D_METHOD("detach", "group_id"),
			&EffectScene::detach);
	ClassDB::bind_method(D_METHOD("detach_slot", "slot_token"),
			&EffectScene::detach_slot);
	ClassDB::bind_method(D_METHOD("reset_runtime_state"),
			&EffectScene::reset_runtime_state);
	ClassDB::bind_method(D_METHOD("advance_in_place", "delta_seconds"),
			&EffectScene::advance_in_place);
	ClassDB::bind_method(D_METHOD("get_live_counts"),
			&EffectScene::get_live_counts);
	ClassDB::bind_method(D_METHOD("inspect", "include_bounds"),
			&EffectScene::inspect, DEFVAL(true));

	BIND_ENUM_CONSTANT(ADMISSION_ALWAYS);
	BIND_ENUM_CONSTANT(ADMISSION_REPLACE_OWNED);
	BIND_ENUM_CONSTANT(ADMISSION_SUPPRESS_WHILE_OWNED);
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
		document.source = file->get_source_path().utf8().get_data();
		document.file = file->to_native();
		config.documents.push_back(std::move(document));
	}

	const opennova::particle::EffectLoadReport report = scene_.open(config);
	advance_in_place(0.0);
	Ref<godot::EffectLoadReport> result;
	result.instantiate();
	result->assign(report, static_cast<int>(p_files.size()),
			static_cast<int>(ignored_document_count));
	return result;
}

int64_t EffectScene::intern(const String &p_effect_name) {
	const opennova::particle::EffectHandle handle =
			scene_.intern(p_effect_name.utf8().get_data());
	return static_cast<int64_t>(handle.value);
}

String EffectScene::effect_name(int64_t p_effect_handle) const {
	return to_gd(scene_.effect_name(
			opennova::particle::EffectHandle{
				handle_from_godot(p_effect_handle)
			}));
}

Dictionary EffectScene::spawn(const Dictionary &p_request) {
	const int admission = p_request.get("admission", ADMISSION_ALWAYS);
	const int binding = p_request.get("binding", BINDING_WORLD);
	const int render_domain =
			p_request.get("render_domain", RENDER_DOMAIN_WORLD);
	const int kill_plane =
			p_request.get("kill_plane", KILL_PLANE_DISABLED);

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
	request.effect.value = handle_from_godot(
			static_cast<int64_t>(p_request.get("effect_handle", 0)));
	request.pose = native_pose(
			static_cast<Transform3D>(p_request.get(
					"transform", Transform3D())));
	request.admission =
			static_cast<opennova::particle::EffectAdmission>(admission);
	request.binding =
			static_cast<opennova::particle::EffectBinding>(binding);
	request.render_domain =
			static_cast<opennova::particle::EffectRenderDomain>(render_domain);
	request.slot.value = token_from_godot(
			static_cast<int64_t>(p_request.get("slot_token", 0)));
	request.owner.value = token_from_godot(
			static_cast<int64_t>(p_request.get("owner_token", 0)));

	request.owner_relative_pose = native_pose(
			static_cast<Transform3D>(p_request.get(
					"owner_relative_transform", Transform3D())));
	request.initial_age_ticks = non_negative_u32(
			static_cast<int64_t>(p_request.get("initial_age_ticks", 0)));
	request.source_tick = token_from_godot(
			static_cast<int64_t>(p_request.get("source_tick", 0)));
	request.source_order = token_from_godot(
			static_cast<int64_t>(p_request.get("source_order", 0)));
	request.color_tint = native_vector(
			static_cast<Vector3>(p_request.get(
					"color_tint", Vector3(1.0, 1.0, 1.0))));
	request.spring_const = static_cast<float>(
			static_cast<double>(p_request.get("spring_const", 0.0)));
	request.lod_divisor = std::max<std::uint32_t>(
			non_negative_u32(
					static_cast<int64_t>(p_request.get("lod_divisor", 1))),
			1u);

	request.kill_plane =
			static_cast<opennova::particle::EffectKillPlane>(kill_plane);
	request.kill_plane_y = static_cast<float>(
			static_cast<double>(p_request.get("kill_plane_y", 0.0)));
	const EffectSpawnReceipt receipt = scene_.spawn(request);
	if (receipt.spawned()) {
		advance_in_place(0.0);
	}
	return spawn_receipt_dictionary(receipt);
}

void EffectScene::apply_owner_poses_in_place(const Array &p_updates) {
	scene_.apply_owner_poses(owner_pose_updates(p_updates));
	snapshot_dirty_ = true;
}

void EffectScene::apply_owner_poses(const Array &p_updates) {
	apply_owner_poses_in_place(p_updates);
	advance_in_place(0.0);
}

PackedInt64Array EffectScene::get_active_owner_tokens() const {
	const std::vector<opennova::particle::EffectOwnerToken> tokens =
			scene_.active_owner_tokens();
	PackedInt64Array result;
	result.resize(static_cast<int64_t>(tokens.size()));
	for (std::size_t i = 0; i < tokens.size(); ++i) {
		result[static_cast<int64_t>(i)] = token_to_godot(tokens[i].value);
	}
	return result;
}

void EffectScene::detach(int64_t p_group_id) {
	scene_.detach(opennova::particle::EffectGroupId{
		token_from_godot(p_group_id)
	});
	advance_in_place(0.0);
}

void EffectScene::detach_slot(int64_t p_slot_token) {
	scene_.detach_slot(opennova::particle::EffectSlotToken{
		token_from_godot(p_slot_token)
	});
	advance_in_place(0.0);
}

void EffectScene::reset_runtime_state() {
	scene_.reset_runtime_state();
	snapshot_dirty_ = true;
}

void EffectScene::advance_in_place(double p_delta_seconds) {
	opennova::particle::EffectAdvanceRequest request;
	request.delta_seconds = static_cast<float>(p_delta_seconds);
	scene_.advance_simulation(request);
	snapshot_dirty_ = true;
}

Dictionary EffectScene::get_live_counts() const {
	const opennova::particle::EffectLiveCounts counts = scene_.live_counts();
	Dictionary result;
	result["group_count"] = static_cast<int64_t>(counts.group_count);
	result["emitter_count"] = static_cast<int64_t>(counts.emitter_count);
	result["particle_count"] = static_cast<int64_t>(counts.particle_count);
	return result;
}

Dictionary EffectScene::inspect(bool p_include_bounds) const {
	return debug_dictionary(scene_.inspect(p_include_bounds));
}

const opennova::particle::ParticleFrameSnapshot &
EffectScene::native_frame_snapshot() const {
	_materialize_snapshot();
	return last_frame_;
}
