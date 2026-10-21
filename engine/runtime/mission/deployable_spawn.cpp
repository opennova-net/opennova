// A vehicle bay's deployable: the authority's C2S 0x40 spawn, and the runtime spawns'
// shared seat-table merge and profile load.
// [orig: Entity_SpawnDeployable @0x51C2B0]
#include <runtime/mission/mission_kernel.h>
#include <runtime/mission/item_traits.h>
#include <runtime/mission/seat_spec_extract.h>
#include <runtime/world/entity_spawn.h>
#include <runtime/world/player_spawn.h>
#include <formats/aip/aip.h>

#include <algorithm>
#include <cstring>

namespace opennova::mission {

void MissionKernel::merge_item_seat_specs(int32_t item_type) {
	const def::DefItemsFile *table = items_table();
	if (table == nullptr) return;
	mission::SeatSpecExtraction extracted;
	mission::extract_item_seat_specs(*table,
			[this](const std::string &graphic) { return assets().model(graphic).get(); },
			{int(item_type) + kItemIdOffset}, extracted);
	for (const ItemSeatSpec &spec : extracted.specs) {
		const auto existing = std::find_if(seat_specs.begin(), seat_specs.end(),
				[&](const ItemSeatSpec &row) { return row.type_id == spec.type_id; });
		if (existing == seat_specs.end()) seat_specs.push_back(spec);
		else *existing = spec;
	}
	for (const auto &graphic : extracted.graphic_by_type) mounted_graphics[graphic.first] = graphic.second;
	std::sort(seat_specs.begin(), seat_specs.end(),
			[](const ItemSeatSpec &a, const ItemSeatSpec &b) { return a.type_id < b.type_id; });
	mission::stamp_seat_spec_turret_limits(world, seat_specs);
}

const aip::Profile *MissionKernel::find_or_load_ai_profile(const std::string &profile) {
	if (profile.empty()) return nullptr;
	for (const PromoteOptions::AiProfileRow &row : ai_profiles)
		if (row.profile == profile) return &row.data;
	// The boot resolver's file rule: <name>.aip, a file that parses no witnessed field
	// contributing no row (runtime_boot.cpp resolve_ai_profiles).
	const std::string file_name = profile + ".aip";
	std::vector<uint8_t> text;
	if (!files_.valid() || !files_.has_file(file_name) || !files_.read_file(file_name, text) ||
			text.empty())
		return nullptr;
	aip::Profile parsed = aip::parse_profile(text.data(), text.size());
	if (parsed.type == 0 && parsed.patrol_speed == -1 && parsed.combat_speed == -1)
		return nullptr;
	ai_profiles.push_back({profile, std::move(parsed)});
	return &ai_profiles.back().data;
}

world::EntityHandle MissionKernel::spawn_deployable(uint16_t item_id, uint8_t team,
		const int32_t position[6]) {
	using namespace world;
	const def::DefItemsFile *table = items_table();
	if (table == nullptr) return {};
	// The clone refuses a template whose ItemTypeIndex is 0: a type no row carries (or the
	// first row's) [orig: ItemList_FindIndexByTypeId @0x49E100 (the call @0x51C317);
	//  Entity_CloneFromTemplateToPool1 @0x439830 (@0x439835..0x43983E)].
	if (item_index_by_type_id(world, item_id) == 0) return {};
	const def::DefItemDef *item = mission::find_item_def(*table, int(item_id) + kItemIdOffset);
	if (item == nullptr) return {};
	merge_item_seat_specs(item_id);

	// The template: the block's position and Yaw word, pitch and roll 0, the team, the
	// group word, refNum and spawn phase 0, SSN 12000, the def bound.
	// [orig: Entity_SpawnDeployable @0x51C2B0 - the memsets @0x51C2EA / @0x51C2FD, the
	//  type index @0x51C325, x / y / z @0x51C332..0x51C358, Yaw = the block's fourth word
	//  @0x51C349, the team @0x51C368, pitch / roll 0 @0x51C36F / @0x51C376, +0x11C 0
	//  @0x51C37D, +0x7C = 12000 @0x51C385, +0x215 0 @0x51C390, +0x2AC 0 @0x51C3CB]
	Entity seed;
	seed.kind = world::EntityKind::Item;
	seed.item_id = item_id;
	seed.position.set_q16({position[0], position[1], position[2]});
	seed.spawn_position = seed.position;
	seed.heading = position[3];
	seed.spawn_heading = position[3];
	seed.pitch_bam = 0;
	seed.roll_bam = 0;
	seed.team = team;
	seed.group_id = 0;
	seed.ref_num = 0;
	seed.net_id = 12000;
	seed.spawn_phase = 0;
	seed.spawn_origin = kSpawnOriginNone;
	initialize_item_seats(seed, seat_specs);
	// Pool 1, includeInUse 1 [orig: Entity_CloneFromTemplateToPool1 @0x439830 - the
	//  includeInUse push @0x439840, Pool_AllocEntry @0x439844; the call @0x51C3FE].
	const EntityHandle handle = world.registry.spawn(1, seed);
	if (!handle.valid()) return {};
	// The models, health at its ceiling, the model init (an hp-0 def's Health 1), the
	// item init's timers. The Armor word (def +0x17E) has no field here.
	// [orig: @0x51C410..0x51C42C (+0x30 / +0x34 / +0x38), Entity_SetHealthToMax @0x43C270
	//  (the call @0x51C42F), the Armor store @0x51C437..0x51C43F, +0xB0 0 @0x51C446,
	//  Entity_InitFromModel @0x40DC30 (the call @0x51C44C)]
	mission::resolve_item_traits(world, *table, item_wire_class_, &assets(), handle);
	{
		Entity &entity = *world.registry.get(handle);
		if ((entity.engine_flags & 0x4000000u) == 0)
			entity.health = max_health_with_difficulty(world, entity);
	}

	// The AI slot of an AI-class def: the clone allocates it and copies the template's
	// zeroed record, whose +156 holds the def's default_aip (sprintf'd from def +0x8B8, the
	// 16 bytes the 172-byte copy carries); the def's class init then builds the brain from
	// the profile that name loads, else helo1, at the template's Yaw. Only the five brain
	// rows build one (the promote's predicate); the slot's owner and name are restored
	// after the init.
	// [orig: Entity_CloneFromTemplateToPool1 @0x439830 (the attrib test @0x439869,
	//  Entity_AllocateAISlot @0x40D2C0 (the call @0x43986C), the record copy @0x43988A);
	//  the sprintf @0x51C3F1; the def+0x148 init callback @0x51C454..0x51C462;
	//  Entity_InitHelicopterAIFromDef @0x4683C0 / Entity_InitVehicleAIFromDef @0x4686C0
	//  (the +0x9C name tests @0x4684A4 / @0x4687A3, helo1 @0x4684C9); the owner and name
	//  @0x51C467..0x51C484]
	if ((item->attrib & kItemAttribAIData) != 0) {
		Entity &entity = *world.registry.get(handle);
		size_t length = 0;
		while (length < 16 && item->default_aip[length] != '\0') ++length;
		entity.ai_text_file.assign(item->default_aip, length);
		const PromoteOptions::AiProfileDefaults defaults = ai_profile_defaults_fn()(item_id);
		if (defaults.known) {
			std::string name;
			for (size_t i = 0; i < length; ++i) {
				const char c = item->default_aip[i];
				name.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c);
			}
			if (name.empty()) name = "helo1";
			const aip::Profile *profile = find_or_load_ai_profile(name);
			world.ai.release(handle);
			const int index = world.ai.attach(handle);
			AiEntity &ai = *world.ai.at(index);
			ai.heading = position[3];
			ai.team = team;
			ai.brain.f[AiBrain::kCurState] = 0;
			ai.brain.f[AiBrain::kPendState] = 0;
			ai.brain.f[AiBrain::kFallback] = 0;
			ai.brain.f[AiBrain::kStep] = 16;
			ai.profile.flags96 = 0;
			ai.profile.flags100 = 0;
			if (profile != nullptr) initialize_ai_profile(ai, *profile);
			initialize_item_class_brain(ai, world, world.ai, profile, defaults.helicopter_init);
			ai.net_id = 12000;
			ai.health = 100;
			entity.spawn_phase = world.vehicle_ai_spawn_phase;
			world.vehicle_ai_spawn_phase = (world.vehicle_ai_spawn_phase + 1) & 15;
		}
	}
	mission::resolve_ai_weapons(world, *table, handle, &assets());
	// The bound sphere, the deployable bit, then the addeweap children, which copy the
	// carrier's Flags [orig: Entity_ComputeBoundingSphere @0x5C69A0 (the call @0x51C48D),
	//  `or [esi+24h], 1000h` @0x51C492, Entity_SpawnAddEWeapChildren @0x40F300 (the call
	//  @0x51C49A; the Flags copy @0x40F404..0x40F408)].
	ensure_collision_instance(world, handle);
	world.registry.get(handle)->flags |= 0x1000u;
	const ItemAttachmentSpawns children = spawn_item_attachments(world, {handle}, seat_specs);
	for (EntityHandle child : children.handles) {
		mission::resolve_item_traits(world, *table, item_wire_class_, &assets(), child);
		ensure_collision_instance(world, child);
	}
	collision.refresh_after_registry_change(world);
	return handle;
}

} // namespace opennova::mission
