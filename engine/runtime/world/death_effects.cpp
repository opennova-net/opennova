#include "destruction.h"
#include "carrier_motion.h"
#include "collision.h"
#include "world.h"

#include <algorithm>
#include <base/io/bam.h>

namespace opennova::world {
namespace {

CollisionMatrix effect_frame(const Entity &e) {
	return entity_placement_matrix(e);
}
Vec3 transformed(const CollisionMatrix &matrix, const Vec3 &local, bool point) {
	const int32_t source[3] = { to_fixed(local.x), to_fixed(local.y), to_fixed(local.z) };
	int32_t out[3];
	matrix.rotate_point(source, out);
	if (point)
		for (int i = 0; i < 3; ++i)
			out[i] = io::bam_add(out[i], matrix.m[4 * i + 3]);
	return { float(from_fixed(out[0])), float(from_fixed(out[1])), float(from_fixed(out[2])) };
}
DestructionEffectEvent attached(const Entity &e, uint8_t family, uint8_t slot) {
	DestructionEffectEvent event;
	event.attach_net_id = e.net_id;
	event.attach_bms_id = e.bms_id;
	event.attach_wire_handle = e.handle.packed;
	event.attach_spawn_origin = e.spawn_origin;
	event.family = family;
	event.bank_slot = slot;
	event.pos = e.position;
	return event;
}
void stop_slot(Entity &e, uint8_t family, uint8_t slot, DestructionEvents &events) {
	auto event = attached(e, family, slot);
	event.release = true;
	events.effects.push_back(std::move(event));
	e.death_effect_active[family - 1] &= uint8_t(~(1u << slot));
}
void spawn_slot(Entity &e, const CollisionMatrix &matrix, uint8_t family, uint8_t slot,
		const std::string &effect, const DeathEffectPoint &point, DestructionEvents &events) {
	auto event = attached(e, family, slot);
	event.effect = effect;
	event.attach_local_pos = point.local_pos;
	event.pos = transformed(matrix, point.local_pos, true);
	event.dir = transformed(matrix, point.local_dir, false);
	// The spawner emits all masked points, but owns only its first four handles.
	if (slot < 4)
		e.death_effect_active[family - 1] |= uint8_t(1u << slot);
	else
		event.family = 0;
	events.effects.push_back(std::move(event));
}
// The nth matching bit identifies the position. An exhausted nonzero mask
// leaves the caller's scratch position untouched, just as the original does.
// [orig: Entity_GetBoneFirePosition @0x4927A0]
void slot_position(const Entity &e, const DeathEffectBank &bank, int slot,
		const CollisionMatrix &matrix, Vec3 &position) {
	if (bank.mask == 0)
		position = e.position;
	else if (slot < int(bank.points.size()))
		position = transformed(matrix, bank.points[slot].local_pos, true);
}
} // namespace

// Three four-handle banks, with an origin fallback only for the death family.
// [orig: Entity_InitDeathSounds @0x4939B0; Entity_SpawnMaskedEffectBank @0x5F7620]
// The water test precedes the independent Fire and Other bank scans.
// [orig: @0x493A88, @0x493B6C, @0x493BBA]
void spawn_death_effect_banks(
		Entity &e, const ItemDeathTraits &traits, bool underwater, DestructionEvents &events) {
	std::fill_n(e.death_effect_active, 3, uint8_t(0));
	e.death_effect_underwater = underwater && !traits.particleh2odeath.empty();
	if (!traits.husk_model_loaded)
		return;
	const auto matrix = effect_frame(e);
	const std::string *effects[3] = { e.death_effect_underwater ? &traits.particleh2odeath
																: &traits.particledeath,
		&traits.particlefire, &traits.particleother };
	for (uint8_t bank = 0; bank < 3; ++bank) {
		if (effects[bank]->empty())
			continue;
		const auto &points = traits.effect_banks[bank].points;
		for (size_t i = 0; i < points.size(); ++i)
			spawn_slot(e, matrix, bank + 1, uint8_t(i), *effects[bank], points[i], events);
		if (bank == 0 && points.empty())
			spawn_slot(e, matrix, 1, 0, *effects[0], {}, events);
	}
}

// [orig: Entity_TransitionToGroundDeath @0x493080; Entity_RespawnVehicle @0x45FF40]
void release_death_effect_bank(Entity &e, uint8_t family, DestructionEvents &events) {
	if (family < 1 || family > 3)
		return;
	for (uint8_t slot = 0; slot < 4; ++slot)
		stop_slot(e, family, slot, events);
}

// The callback site controls ordering against death-piece random draws. Fire
// rolls use the raw mask bit for the handle index, not its nth matched bit.
// [orig: Entity_UpdateDeadWreckEffects @0x493140]
void update_dead_wreck_effects(World &world, Entity &e, const ItemDeathTraits *traits,
		float water_height, DestructionEvents &events) {
	if (traits == nullptr || !traits->husk_model_loaded)
		return;
	const float water = water_height <= -1.0e8f ? 0.0f : water_height;
	const auto matrix = effect_frame(e);
	Vec3 position = e.position;
	for (uint8_t slot = 0; slot < 4; ++slot) {
		if ((e.death_effect_active[0] & (1u << slot)) == 0)
			continue;
		slot_position(e, traits->effect_banks[0], slot, matrix, position);
		if (e.death_effect_underwater ? position.z >= water : position.z < water)
			stop_slot(e, 1, slot, events);
	}
	const auto &fire = traits->effect_banks[1];
	for (uint8_t slot = 0; slot < 4; ++slot) {
		slot_position(e, fire, slot, matrix, position);
		const bool masked = (fire.mask & (1u << slot)) != 0;
		if (masked && world.next_prng16_c() < kFireCrackleThreshold && position.z >= water) {
			events.effects.push_back({ kFireCrackleEffect, position, { 0, 0, 1 } });
			world.out.fire_sounds.play_with_distance_delay(kFireCrackleSound, e.position, e.bms_id);
			++events.crackles;
		}
		if ((e.death_effect_active[1] & (1u << slot)) != 0) {
			if (position.z < water) {
				stop_slot(e, 2, slot, events);
				events.effects.push_back({ "Effect_Boat01Steam", position, {} });
			}
		} else if (masked && position.z >= water && !traits->particlefire.empty()) {
			// Restarted fire uses an unoriented descriptor at the computed point.
			auto event = attached(e, 2, slot);
			event.effect = traits->particlefire;
			event.pos = position;
			if (slot < fire.points.size())
				event.attach_local_pos = fire.points[slot].local_pos;
			events.effects.push_back(std::move(event));
			e.death_effect_active[1] |= uint8_t(1u << slot);
		}
	}
	for (uint8_t slot = 0; slot < 4; ++slot) {
		if ((e.death_effect_active[2] & (1u << slot)) == 0)
			continue;
		slot_position(e, traits->effect_banks[2], slot, matrix, position);
		if (position.z < water)
			stop_slot(e, 3, slot, events);
	}
}
} // namespace opennova::world
