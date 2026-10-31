#include <runtime/renderer/light_spawn.h>

#include <cmath>
#include <unordered_set>

#include <base/io/fixed.h>

namespace opennova::renderer {

using threedi::THREEDI_LIGHT_FLAG_DISABLE_CORONA;
using threedi::THREEDI_LIGHT_FLAG_DISABLE_OBJECTS;
using threedi::THREEDI_LIGHT_FLAG_DISABLE_TERRAIN;

uint64_t LightOwnerIds::wire(int64_t wire_handle) {
	return wire_handle >= 0 ? (kWireTag | (static_cast<uint64_t>(wire_handle) & kWireHandleMask))
							: 0;
}

uint64_t LightOwnerIds::static_source(int64_t source_index) {
	return source_index >= 0 ? (kStaticTag | static_cast<uint64_t>(source_index)) : 0;
}

std::array<int32_t, 3> light_position_fixed(const std::array<float, 3> &mission) {
	return {io::float_to_fp16_16_round_sat(static_cast<double>(mission[0])),
		io::float_to_fp16_16_round_sat(static_cast<double>(mission[1])),
		io::float_to_fp16_16_round_sat(static_cast<double>(mission[2]))};
}

std::array<int32_t, 3> light_placement_origin_fixed(const LightPlacement &placement) {
	// presentation (x, y, z) -> mission (x, -z, y).
	return light_position_fixed({placement[9], -placement[11], placement[10]});
}

namespace {

int32_t radius_fixed(float radius) {
	return static_cast<int32_t>(std::round(radius * io::kFp16One));
}

// The record's offset through the entity matrix, in mission space.
std::array<float, 3> placed_point(const threedi::ThreediLight &record,
		const LightPlacement &placement) {
	const float model[3] = {-record.offset[0], record.offset[1], record.offset[2]};
	float presentation[3];
	for (int row = 0; row < 3; ++row) {
		presentation[row] = placement[row * 3 + 0] * model[0] + placement[row * 3 + 1] * model[1] +
				placement[row * 3 + 2] * model[2] + placement[9 + row];
	}
	// presentation (x, y, z) -> mission (x, -z, y).
	return {presentation[0], -presentation[2], presentation[1]};
}

} // namespace

LightSpawnParams model_light_spawn(const threedi::ThreediLight &record,
		const LightPlacement &placement, uint64_t owner_entity, bool building,
		uint64_t blink_owner, int32_t blink_section) {
	LightSpawnParams params;
	params.position_fixed = light_position_fixed(placed_point(record, placement));
	params.radius_fixed = radius_fixed(record.atten_end);
	// The model-light spawn passes a white record colour; the authored colours
	// live in the gen block.
	params.rgb = {255, 255, 255};
	if (record.style > 0) {
		params.has_gen = true;
		params.gen.style = record.style;
		params.gen.phase = record.phase;
		params.gen.rate = record.rate;
		params.gen.color_start = {record.color_start[0], record.color_start[1],
			record.color_start[2], 255};
		params.gen.color_end = {record.color_end[0], record.color_end[1], record.color_end[2],
			255};
	}
	ModelLightOwnerInputs owner_inputs;
	owner_inputs.attach_bone = record.subobj_index;
	owner_inputs.spawning_entity = owner_entity;
	owner_inputs.spawner_is_building = building;
	owner_inputs.blink_owner_entity = blink_owner;
	owner_inputs.blink_section = blink_section;
	owner_inputs.blink_hit = blink_owner != 0;
	const ModelLightOwner owner = resolve_model_light_owner(owner_inputs);
	params.owner_entity = owner.entity;
	params.owner_section = owner.section;
	params.disable_corona = (record.flags & THREEDI_LIGHT_FLAG_DISABLE_CORONA) != 0;
	params.disable_terrain = (record.flags & THREEDI_LIGHT_FLAG_DISABLE_TERRAIN) != 0;
	params.disable_objects = (record.flags & THREEDI_LIGHT_FLAG_DISABLE_OBJECTS) != 0;
	return params;
}

LightHandle LightSpawner::spawn_entity(const EntityLightSpawn &spawn) {
	if (spawn.record_count == 0 || spawn.records == nullptr)
		return {};
	if (walked_.count(spawn.owner_entity) != 0)
		return entity_handle(spawn.owner_entity);
	// The building gate: a Building-type entity never queried, so its own
	// unattached records stay world lights.
	const uint64_t blink_owner = spawn.building ? 0 : spawn.blink_owner;
	const int32_t blink_section = spawn.building ? 0 : spawn.blink_section;
	// A batched static is an entity too: a subobject record binds to its
	// tagged owner [orig: Entity_SpawnGlowEffects @ 0x56c8ae;
	// SetOwnerGroup(entity,bone)], and every record overwrites the one word
	// [orig: @0x56c925..0x56c92c].
	LightHandle cached{};
	for (size_t i = 0; i < spawn.record_count; ++i) {
		cached = scene_.spawn(model_light_spawn(spawn.records[i], spawn.placement,
				spawn.owner_entity, spawn.building, blink_owner, blink_section));
	}
	// Walked even when the pool was full: retail walks an entity once.
	walked_[spawn.owner_entity] = true;
	entity_handles_[spawn.owner_entity] = cached;
	return cached;
}

bool LightSpawner::walked(uint64_t owner_entity) const {
	return walked_.count(owner_entity) != 0;
}

LightHandle LightSpawner::entity_handle(uint64_t owner_entity) const {
	const auto found = entity_handles_.find(owner_entity);
	return found != entity_handles_.end() ? found->second : LightHandle{};
}

void LightSpawner::destroy_entity(uint64_t owner_entity) {
	// [orig: Entity_Destroy @0x43e903..0x43e916]: the one word, not a list.
	const auto found = entity_handles_.find(owner_entity);
	if (found != entity_handles_.end()) {
		if (!found->second.is_null())
			scene_.despawn(found->second);
		entity_handles_.erase(found);
	}
	walked_.erase(owner_entity);
	const auto bms = owner_bms_.find(owner_entity);
	if (bms != owner_bms_.end()) {
		const auto registered = bms_owner_.find(bms->second);
		if (registered != bms_owner_.end() && registered->second == owner_entity)
			bms_owner_.erase(registered);
		owner_bms_.erase(bms);
	}
}

void LightSpawner::muzzle_fire(uint64_t shooter, const std::array<int32_t, 3> &muzzle_fixed) {
	const LightHandle handle = scene_.fire_muzzle_glow(entity_handle(shooter), muzzle_fixed, shooter);
	if (!handle.is_null())
		entity_handles_[shooter] = handle;
}

void LightSpawner::sync_round_glows(const std::vector<world::RoundGlowRow> &rows) {
	std::unordered_set<uint64_t> seen;
	seen.reserve(rows.size());
	for (const world::RoundGlowRow &row : rows) {
		seen.insert(row.id);
		const auto found = round_handles_.find(row.id);
		if (found == round_handles_.end() || found->second.is_null()) {
			// The spawn rides radius/2 above the round (round_glow_spawn_lift),
			// mode 1 / duration -1, terrain disabled [orig: RoundData_SpawnRound
			// @ 0x4ec8d6 vs the @ 0x4eaa9f SetPositionAndBounds follow].
			LightSpawnParams params;
			params.position_fixed = light_position_fixed(
					{row.pos.x, row.pos.y, row.pos.z + round_glow_spawn_lift(row.radius)});
			params.radius_fixed = radius_fixed(row.radius);
			params.rgb = {static_cast<uint8_t>((row.color_rgb24 >> 16) & 0xFFu),
				static_cast<uint8_t>((row.color_rgb24 >> 8) & 0xFFu),
				static_cast<uint8_t>(row.color_rgb24 & 0xFFu)};
			params.disable_terrain = true;
			const LightHandle handle = scene_.spawn(params);
			if (!handle.is_null())
				round_handles_[row.id] = handle;
		} else {
			// The per-tick follow re-centres the same instance at the raw round.
			scene_.set_position(found->second, light_position_fixed(
					{row.pos.x, row.pos.y, row.pos.z + kRoundGlowFollowLift}));
		}
	}
	for (auto it = round_handles_.begin(); it != round_handles_.end();) {
		if (seen.count(it->first) == 0) {
			scene_.despawn(it->second);
			it = round_handles_.erase(it);
		} else {
			++it;
		}
	}
}

void LightSpawner::register_building(int bms_id, uint64_t owner_entity) {
	if (bms_id == 0 || owner_entity == 0)
		return;
	bms_owner_[bms_id] = owner_entity;
	owner_bms_[owner_entity] = bms_id;
}

uint64_t LightSpawner::owner_for_bms(int bms_id) const {
	if (bms_id == 0)
		return 0;
	const auto found = bms_owner_.find(bms_id);
	return found != bms_owner_.end() ? found->second : 0;
}

void LightSpawner::reset() {
	scene_.clear();
	entity_handles_.clear();
	walked_.clear();
	bms_owner_.clear();
	owner_bms_.clear();
	round_handles_.clear();
}

} // namespace opennova::renderer
