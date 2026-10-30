// The destruction effects' groups: what the world's destruction rows
// (DestructionEvents::effects) spawn and release, and the handles retail's
// producers hold and re-pose each tick from their entity's state: a wreck's
// Dead / Fire / Other banks (four handles each), a vehicle's critical fire,
// a death piece's trail. A transient row spawns and is forgotten. The groups
// are world-bound; each holder moves its own at retail's site, as nothing in
// the scene follows an owner (particle::EffectScene::move_group).
#pragma once

#include <runtime/particle/effect_scene.h>
#include <runtime/world/geom.h>

#include <cstdint>
#include <vector>

namespace opennova::world {

class World;
struct Entity;
struct DeathPiece;
struct DestructionEffectEvent;

// A destruction row's held handle as a scene slot: the entity's packed handle, the family
// (1 Dead, 2 Fire, 3 Other, 5 the vehicle fire) and the bank slot. A row with no identity, or
// family 0, holds none. Family 4, the vehicle smoke, is the entity's +0x1CC slot
// (entity_effect_slot, ItemEmitterSystem::store_damage_smoke).
inline constexpr uint64_t kDestructionEffectSlotDomain = 0x2000000000000000ull;
inline particle::EffectSlotToken destruction_effect_slot(uint16_t packed_handle, uint8_t family,
		uint8_t bank_slot) {
	return {kDestructionEffectSlotDomain | (uint64_t(packed_handle) << 16) |
			(uint64_t(family) << 8) | bank_slot};
}
// A death piece's trail handle (piece+0x8C) as a scene slot, by its pool slot.
inline constexpr uint64_t kDeathPieceTrailSlotDomain = 0x1000000000000000ull;
inline particle::EffectSlotToken death_piece_trail_slot(int piece_slot) {
	return {kDeathPieceTrailSlotDomain | uint64_t(uint32_t(piece_slot))};
}

class DestructionEmitters {
public:
	// Spawn or release every effect row the tick queued (World::out.destruction.effects), in
	// order, and empty the queue. Closed producers (ItemEmitterSystem::producers_open) leave the
	// rows: a dedicated host's discard drops them (discard_entity_presentation), as a device with
	// no effect system spawns nothing.
	void consume(World &world);
	// The death bank's per-tick re-pose: the slot's group moved to its point, its aim kept.
	// [orig: Entity_UpdateDeadWreckEffects @ 0x493140 - CEffectEmitter_UpdatePositionAndParams
	//  @ 0x49321E (Dead), @ 0x4932FD (Fire), @ 0x4933BF (Other), each with no direction]
	void move_bank(World &world, const Entity &entity, uint8_t family, uint8_t slot,
			const Vec3 &position);
	// The vehicle's critical fire (+0x400), re-posed at the head of its mover: the vehicle's
	// position, aimed along its matrix's third row.
	// [orig: Entity_UpdateVehiclePhysics @ 0x48AF00 - the matrix @ 0x48AF27..0x48AF30, its third
	//  row @ 0x48AF66, CEffectEmitter_UpdatePositionAndParams @ 0x48B114 at the Position (ebp
	//  @ 0x48AF1D); the sibling movers' calls @ 0x4841F4, @ 0x488CF4, @ 0x48D6D2, @ 0x49058A,
	//  @ 0x46FC4F]
	void move_vehicle_fire(World &world, const Entity &vehicle);
	// A death piece's trail (piece+0x8C): the reused ring slot's trail destroyed outright, then
	// the piece's type trail submitted at its position, unoriented and untagged, held in the slot.
	// [orig: DeathPiece_AllocSlot @ 0x57B4F0 - Effect_DestroyEmitterIfLive @ 0x57B528;
	//  Entity_SpawnDeathPieces @ 0x493400 - the submit @ 0x493813..0x493822, the store @ 0x49382A,
	//  the death callback @ 0x493840..0x493847]
	void destroy_piece_trail(World &world, int piece_slot);
	void spawn_piece_trail(World &world, int piece_slot, const DeathPiece &piece,
			const char *effect);
	// Its re-pose to the piece's position while airborne above the water, and its release.
	// [orig: Entity_ProcessDeathPiecePhysics @ 0x492DD0 - the re-pose @ 0x493068; the releases
	//  @ 0x492E2D (the water crossing), @ 0x492EA4 (the free under water), @ 0x492FC9 (the bounce
	//  budget spent)]
	void move_piece_trail(World &world, int piece_slot, const DeathPiece &piece);
	void release_piece_trail(World &world, int piece_slot);
	// The held groups whose entity row was destroyed or taken by another entity since are
	// released (a wreck's banks go with its row), then the count of the ones left.
	void sync(World &world);
	void reset() { held_.clear(); }
	std::size_t held() const { return held_.size(); }

private:
	void spawn_row(World &world, const DestructionEffectEvent &row);
	void release_row(World &world, const DestructionEffectEvent &row);
	struct Held {
		uint64_t slot = 0;
		uint16_t handle = 0xFFFF;
		uint64_t lifetime = 0;
	};
	std::vector<Held> held_;
};

} // namespace opennova::world
