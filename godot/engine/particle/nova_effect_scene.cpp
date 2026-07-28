#include "nova_effect_scene.h"

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

namespace {

using opennova::particle::Color3;
using opennova::particle::CurveRef;
using opennova::particle::EffectBounds;
using opennova::particle::EffectLoadReport;
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

String godot_string(const std::string &value) {
	return String::utf8(value.c_str());
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

Dictionary load_report_dictionary(const EffectLoadReport &report) {
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

Dictionary curve_dictionary(const CurveRef &curve) {
	Dictionary result;
	result["name"] = godot_string(curve.name);
	result["reverse"] = curve.reverse;
	result["inverse"] = curve.inverse;
	result["present"] = curve.present;
	result["baked"] = curve.baked;
	PackedByteArray lut;
	if (curve.baked) {
		lut.resize(static_cast<int64_t>(curve.baked_lut.size()));
		std::memcpy(lut.ptrw(), curve.baked_lut.data(),
				curve.baked_lut.size());
	}
	result["lut"] = lut;
	return result;
}

Dictionary graphic_dictionary(const GraphicLayer &graphic) {
	Dictionary result;
	result["index"] = graphic.index;
	result["present"] = graphic.present;
	result["texture"] = godot_string(graphic.texture);
	result["blend_mode_raw"] = godot_string(graphic.blend_mode_raw);
	result["blend_mode"] = static_cast<int>(graphic.blend_mode);
	result["blend_mode_name"] =
			String::utf8(opennova::particle::blend_mode_name(graphic.blend_mode));
	result["flip_frames"] = graphic.flip_frames;
	result["flip_rate"] = graphic.flip_rate;
	result["color1"] = godot_color(graphic.color1);
	result["color2"] = godot_color(graphic.color2);
	result["color3"] = godot_color(graphic.color3);
	result["color4"] = godot_color(graphic.color4);
	result["color_overrides_set"] = graphic.color_overrides_set;
	result["alpha"] = graphic.alpha;
	result["scale"] = graphic.scale;
	result["scale_adj"] = graphic.scale_adj;
	result["scale_func"] = curve_dictionary(graphic.scale_func);
	result["alpha_func"] = curve_dictionary(graphic.alpha_func);
	result["red_func"] = curve_dictionary(graphic.red_func);
	result["green_func"] = curve_dictionary(graphic.green_func);
	result["blue_func"] = curve_dictionary(graphic.blue_func);

	Array uv_rects;
	uv_rects.resize(static_cast<int64_t>(graphic.baked_uv_rects.size()));
	for (std::size_t i = 0; i < graphic.baked_uv_rects.size(); ++i) {
		const auto &uv = graphic.baked_uv_rects[i];
		Dictionary value;
		value["u_min"] = uv.u_min;
		value["v_min"] = uv.v_min;
		value["u_max"] = uv.u_max;
		value["v_max"] = uv.v_max;
		value["inset"] = uv.inset;
		uv_rects[static_cast<int64_t>(i)] = value;
	}
	result["baked_uv_rects"] = uv_rects;
	return result;
}

Dictionary definition_dictionary(const ParticleDef &definition,
		std::size_t index) {
	Dictionary result;
	result["definition_index"] = static_cast<int64_t>(index);
	result["id"] = godot_string(definition.id);
	result["flags"] = static_cast<int64_t>(definition.flags);
	result["move"] = static_cast<int64_t>(definition.move);
	result["alpha"] = definition.alpha;
	result["scale"] = definition.scale;
	result["bump_scale"] = definition.bump_scale;
	result["scale_func"] = curve_dictionary(definition.scale_func);
	result["alpha_func"] = curve_dictionary(definition.alpha_func);
	result["red_func"] = curve_dictionary(definition.red_func);
	result["green_func"] = curve_dictionary(definition.green_func);
	result["blue_func"] = curve_dictionary(definition.blue_func);

	Array graphics;
	graphics.resize(static_cast<int64_t>(definition.graphics.size()));
	for (std::size_t i = 0; i < definition.graphics.size(); ++i) {
		graphics[static_cast<int64_t>(i)] =
				graphic_dictionary(definition.graphics[i]);
	}
	result["graphics"] = graphics;
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
	result["status"] = NovaEffectScene::SPAWN_STATUS_INVALID_REQUEST;
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
	return value >= NovaEffectScene::ADMISSION_ALWAYS &&
			value <= NovaEffectScene::ADMISSION_SUPPRESS_WHILE_OWNED;
}

bool valid_binding(int value) noexcept {
	return value >= NovaEffectScene::BINDING_WORLD &&
			value <= NovaEffectScene::BINDING_FOLLOW_OWNER;
}

bool valid_render_domain(int value) noexcept {
	return value >= NovaEffectScene::RENDER_DOMAIN_WORLD &&
			value <= NovaEffectScene::RENDER_DOMAIN_FIRST_PERSON;
}

bool valid_kill_plane(int value) noexcept {
	return value >= NovaEffectScene::KILL_PLANE_DISABLED &&
			value <= NovaEffectScene::KILL_PLANE_AT_OR_BELOW;
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

Dictionary frame_dictionary(
		const opennova::particle::ParticleFrameSnapshot &frame) {
	Dictionary result;
	result["frame_index"] = token_to_godot(frame.frame_index);
	result["simulation_time_seconds"] = frame.simulation_time_seconds;

	Array definitions;
	if (frame.definitions) {
		definitions.resize(static_cast<int64_t>(frame.definitions->size()));
		for (std::size_t i = 0; i < frame.definitions->size(); ++i) {
			definitions[static_cast<int64_t>(i)] =
					definition_dictionary((*frame.definitions)[i], i);
		}
	}
	result["definitions"] = definitions;

	Array groups;
	groups.resize(static_cast<int64_t>(frame.groups.size()));
	for (std::size_t i = 0; i < frame.groups.size(); ++i) {
		const auto &group = frame.groups[i];
		Dictionary value;
		value["group_id"] = token_to_godot(group.id.value);
		value["effect_handle"] = static_cast<int64_t>(group.effect.value);
		value["effect_name"] = godot_string(group.effect_name);
		value["source"] = godot_string(group.source);
		value["transform"] = godot_pose(group.pose);
		value["render_domain"] = static_cast<int>(group.render_domain);
		value["source_tick"] = token_to_godot(group.source_tick);
		value["source_order"] = token_to_godot(group.source_order);
		value["first_emitter"] =
				static_cast<int64_t>(group.first_emitter);
		value["emitter_count"] =
				static_cast<int64_t>(group.emitter_count);
		value["detached"] = group.detached;
		groups[static_cast<int64_t>(i)] = value;
	}
	result["groups"] = groups;

	Array emitters;
	emitters.resize(static_cast<int64_t>(frame.emitters.size()));
	for (std::size_t i = 0; i < frame.emitters.size(); ++i) {
		const auto &emitter = frame.emitters[i];
		Dictionary value;
		value["emitter_id"] = token_to_godot(emitter.id);
		value["group_index"] = static_cast<int64_t>(emitter.group_index);
		value["ordinal"] = static_cast<int64_t>(emitter.ordinal);
		value["definition_index"] =
				static_cast<int64_t>(emitter.definition_index);
		value["first_particle"] =
				static_cast<int64_t>(emitter.first_particle);
		value["particle_count"] =
				static_cast<int64_t>(emitter.particle_count);
		value["position"] = godot_vector(emitter.position);
		value["forward"] = godot_vector(emitter.forward);
		value["color_tint"] = godot_vector(emitter.color_tint);
		value["age"] = emitter.age;
		value["spring_const"] = emitter.spring_const;
		value["lod_divisor"] = static_cast<int64_t>(emitter.lod_divisor);
		value["kill_plane"] = static_cast<int>(emitter.kill_plane);
		value["kill_plane_y"] = emitter.kill_plane_y;
		emitters[static_cast<int64_t>(i)] = value;
	}
	result["emitters"] = emitters;

	Array particles;
	particles.resize(static_cast<int64_t>(frame.particles.size()));
	for (std::size_t i = 0; i < frame.particles.size(); ++i) {
		const auto &particle = frame.particles[i];
		Dictionary value;
		value["position"] = godot_vector(particle.position);
		value["velocity"] = godot_vector(particle.velocity);
		value["age"] = particle.age;
		value["lifetime"] = particle.lifetime;
		value["size"] = particle.size;
		value["curve_phase"] = particle.curve_phase;
		value["phase_rate"] = particle.phase_rate;
		value["rotation"] = particle.rotation;
		value["rotation_rate"] = particle.rotation_rate;
		value["yaw"] = particle.yaw;
		value["yaw_rate"] = particle.yaw_rate;
		value["pitch"] = particle.pitch;
		value["pitch_rate"] = particle.pitch_rate;
		value["color"] = godot_color(particle.color, particle.alpha);
		value["color_r"] = static_cast<int>(particle.color.r);
		value["color_g"] = static_cast<int>(particle.color.g);
		value["color_b"] = static_cast<int>(particle.color.b);
		value["alpha"] = static_cast<int>(particle.alpha);

		value["color_slot"] = static_cast<int>(particle.color_slot);
		value["graphic_layer"] = static_cast<int>(particle.graphic_layer);
		value["serial"] = static_cast<int>(particle.serial);
		value["flags"] = static_cast<int64_t>(particle.flags);
		particles[static_cast<int64_t>(i)] = value;
	}
	result["particles"] = particles;
	result["definition_count"] = definitions.size();
	result["group_count"] = groups.size();
	result["emitter_count"] = emitters.size();
	result["particle_count"] = particles.size();
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
		value["effect_name"] = godot_string(group.effect_name);
		value["source"] = godot_string(group.source);
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
					godot_string(emitter.definition_name);
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

NovaEffectScene::NovaEffectScene() = default;

void NovaEffectScene::_materialize_snapshot() const {
	if (!snapshot_dirty_)
		return;
	scene_.write_snapshot(last_frame_);
	snapshot_dirty_ = false;
}

void NovaEffectScene::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open", "files", "options"),
			&NovaEffectScene::open, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("intern", "effect_name"),
			&NovaEffectScene::intern);
	ClassDB::bind_method(D_METHOD("effect_name", "effect_handle"),
			&NovaEffectScene::effect_name);
	ClassDB::bind_method(D_METHOD("spawn", "request"),
			&NovaEffectScene::spawn);
	ClassDB::bind_method(D_METHOD("apply_owner_poses_in_place", "updates"),
			&NovaEffectScene::apply_owner_poses_in_place);
	ClassDB::bind_method(D_METHOD("apply_owner_poses", "updates"),
			&NovaEffectScene::apply_owner_poses);
	ClassDB::bind_method(D_METHOD("get_active_owner_tokens"),
			&NovaEffectScene::get_active_owner_tokens);
	ClassDB::bind_method(D_METHOD("detach", "group_id"),
			&NovaEffectScene::detach);
	ClassDB::bind_method(D_METHOD("detach_slot", "slot_token"),
			&NovaEffectScene::detach_slot);
	ClassDB::bind_method(D_METHOD("reset_runtime_state"),
			&NovaEffectScene::reset_runtime_state);
	ClassDB::bind_method(D_METHOD("advance", "delta_seconds"),
			&NovaEffectScene::advance);
	ClassDB::bind_method(D_METHOD("advance_in_place", "delta_seconds"),
			&NovaEffectScene::advance_in_place);
	ClassDB::bind_method(D_METHOD("get_frame_snapshot"),
			&NovaEffectScene::get_frame_snapshot);
	ClassDB::bind_method(D_METHOD("get_live_counts"),
			&NovaEffectScene::get_live_counts);
	ClassDB::bind_method(D_METHOD("inspect", "include_bounds"),
			&NovaEffectScene::inspect, DEFVAL(true));

	BIND_ENUM_CONSTANT(ADMISSION_ALWAYS);
	BIND_ENUM_CONSTANT(ADMISSION_REPLACE_OWNED);
	BIND_ENUM_CONSTANT(ADMISSION_SUPPRESS_WHILE_OWNED);
	BIND_ENUM_CONSTANT(BINDING_WORLD);
	BIND_ENUM_CONSTANT(BINDING_FOLLOW_OWNER);
	BIND_ENUM_CONSTANT(RENDER_DOMAIN_WORLD);
	BIND_ENUM_CONSTANT(RENDER_DOMAIN_FIRST_PERSON);

	BIND_ENUM_CONSTANT(KILL_PLANE_DISABLED);
	BIND_ENUM_CONSTANT(KILL_PLANE_ABOVE);
	BIND_ENUM_CONSTANT(KILL_PLANE_AT_OR_BELOW);
	BIND_ENUM_CONSTANT(SPAWN_STATUS_INVALID_REQUEST);
	BIND_ENUM_CONSTANT(SPAWN_STATUS_SPAWNED);
	BIND_ENUM_CONSTANT(SPAWN_STATUS_SUPPRESSED);
	BIND_ENUM_CONSTANT(SPAWN_STATUS_INVALID_HANDLE);
	BIND_ENUM_CONSTANT(SPAWN_STATUS_EMPTY_EFFECT);
	BIND_ENUM_CONSTANT(SPAWN_STATUS_MISSING_SLOT);
	BIND_ENUM_CONSTANT(SPAWN_STATUS_MISSING_OWNER);
	BIND_ENUM_CONSTANT(SPAWN_STATUS_GROUP_CAPACITY_REACHED);
	BIND_ENUM_CONSTANT(SPAWN_STATUS_EMITTER_CAPACITY_REACHED);
}

Dictionary NovaEffectScene::open(
		const TypedArray<NovaParticleFile> &p_files,
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
		Ref<NovaParticleFile> file = p_files[i];
		if (file.is_null()) {
			++ignored_document_count;
			continue;
		}
		opennova::particle::EffectCatalogDocument document;
		document.source = file->get_source_path().utf8().get_data();
		document.file = file->to_native();
		config.documents.push_back(std::move(document));
	}

	const EffectLoadReport report = scene_.open(config);
	advance_in_place(0.0);
	Dictionary result = load_report_dictionary(report);
	result["input_document_count"] = p_files.size();
	result["ignored_document_count"] = ignored_document_count;
	return result;
}

int64_t NovaEffectScene::intern(const String &p_effect_name) {
	const opennova::particle::EffectHandle handle =
			scene_.intern(p_effect_name.utf8().get_data());
	return static_cast<int64_t>(handle.value);
}

String NovaEffectScene::effect_name(int64_t p_effect_handle) const {
	return godot_string(scene_.effect_name(
			opennova::particle::EffectHandle{
				handle_from_godot(p_effect_handle)
			}));
}

Dictionary NovaEffectScene::spawn(const Dictionary &p_request) {
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

void NovaEffectScene::apply_owner_poses_in_place(const Array &p_updates) {
	scene_.apply_owner_poses(owner_pose_updates(p_updates));
	snapshot_dirty_ = true;
}

void NovaEffectScene::apply_owner_poses(const Array &p_updates) {
	apply_owner_poses_in_place(p_updates);
	advance_in_place(0.0);
}

PackedInt64Array NovaEffectScene::get_active_owner_tokens() const {
	const std::vector<opennova::particle::EffectOwnerToken> tokens =
			scene_.active_owner_tokens();
	PackedInt64Array result;
	result.resize(static_cast<int64_t>(tokens.size()));
	for (std::size_t i = 0; i < tokens.size(); ++i) {
		result[static_cast<int64_t>(i)] = token_to_godot(tokens[i].value);
	}
	return result;
}

void NovaEffectScene::detach(int64_t p_group_id) {
	scene_.detach(opennova::particle::EffectGroupId{
		token_from_godot(p_group_id)
	});
	advance_in_place(0.0);
}

void NovaEffectScene::detach_slot(int64_t p_slot_token) {
	scene_.detach_slot(opennova::particle::EffectSlotToken{
		token_from_godot(p_slot_token)
	});
	advance_in_place(0.0);
}

void NovaEffectScene::reset_runtime_state() {
	scene_.reset_runtime_state();
	snapshot_dirty_ = true;
}

void NovaEffectScene::advance_in_place(double p_delta_seconds) {
	opennova::particle::EffectAdvanceRequest request;
	request.delta_seconds = static_cast<float>(p_delta_seconds);
	scene_.advance_simulation(request);
	snapshot_dirty_ = true;
}

Dictionary NovaEffectScene::advance(double p_delta_seconds) {
	advance_in_place(p_delta_seconds);
	_materialize_snapshot();
	return frame_dictionary(last_frame_);
}

Dictionary NovaEffectScene::get_frame_snapshot() const {
	_materialize_snapshot();
	return frame_dictionary(last_frame_);
}

Dictionary NovaEffectScene::get_live_counts() const {
	const opennova::particle::EffectLiveCounts counts = scene_.live_counts();
	Dictionary result;
	result["group_count"] = static_cast<int64_t>(counts.group_count);
	result["emitter_count"] = static_cast<int64_t>(counts.emitter_count);
	result["particle_count"] = static_cast<int64_t>(counts.particle_count);
	return result;
}

Dictionary NovaEffectScene::inspect(bool p_include_bounds) const {
	return debug_dictionary(scene_.inspect(p_include_bounds));
}

const opennova::particle::ParticleFrameSnapshot &
NovaEffectScene::native_frame_snapshot() const {
	_materialize_snapshot();
	return last_frame_;
}
