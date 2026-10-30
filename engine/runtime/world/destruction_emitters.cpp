#include <runtime/world/destruction_emitters.h>

#include <runtime/world/collision.h>
#include <runtime/world/destruction.h>
#include <runtime/world/item_effects.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <utility>

namespace opennova::world {

namespace {

const Entity *row_entity(const World &world, uint16_t handle) {
	return handle != EntityHandle::kInvalid ? world.registry.get(EntityHandle{handle}) : nullptr;
}

// The third row of the entity's placement matrix (its local up), as a scene vector.
particle::Vec3 matrix_third_row(const Entity &entity) {
	const CollisionMatrix matrix = entity_placement_matrix(entity);
	const Vec3 row{static_cast<float>(from_fixed(matrix.m[8])),
			static_cast<float>(from_fixed(matrix.m[9])), static_cast<float>(from_fixed(matrix.m[10]))};
	return effect_point(row);
}

} // namespace

void DestructionEmitters::consume(World &world) {
	if (!world.item_emitters.producers_open()) return;
	std::vector<DestructionEffectEvent> &queue = world.out.destruction.effects;
	if (queue.empty()) return;
	std::vector<DestructionEffectEvent> rows;
	rows.swap(queue);
	for (const DestructionEffectEvent &row : rows) {
		if (row.release)
			release_row(world, row);
		else
			spawn_row(world, row);
	}
}

void DestructionEmitters::spawn_row(World &world, const DestructionEffectEvent &row) {
	particle::EffectScene *scene = world.item_emitters.open_scene();
	if (scene == nullptr || row.effect.empty()) return;
	const Entity *entity = row_entity(world, row.attach_wire_handle);
	// The heavy-damage smoke is the vehicle's +0x1CC handle.
	if (row.family == 4) {
		if (entity != nullptr) world.item_emitters.store_damage_smoke(world, *entity, row.effect, row.pos);
		return;
	}
	particle::EffectSpawnRequest request;
	request.effect = scene->intern(row.effect);
	request.pose = particle::descriptor_pose(effect_point(row.pos), effect_point(row.dir));
	request.binding = particle::EffectBinding::World;
	request.source_tick = world.logic_tick;
	request.kill_plane_y = effect_water_height(world);
	// The row's descriptor tag (its producer sets it) gates the group by building section.
	if (row.section_tagged) request.section_gate = effect_section_gate(world, row.pos);
	// A family row of an identity holds its handle (a bank slot, the vehicle fire, a wire body's
	// slot emitter); every other row is a transient submit.
	if (row.family == 0 || row.attach_wire_handle == EntityHandle::kInvalid) {
		scene->spawn(request);
		return;
	}
	const particle::EffectSlotToken slot =
			destruction_effect_slot(row.attach_wire_handle, row.family, row.bank_slot);
	request.admission = particle::EffectAdmission::ReplaceOwned;
	request.slot = slot;
	if (!scene->spawn(request).spawned()) return;
	if (entity == nullptr) return;
	// The fire's mover re-aims it in the tick it spawns in.
	if (row.family == 5) {
		const particle::Vec3 up = matrix_third_row(*entity);
		scene->move_slot(slot, effect_point(row.pos), &up);
	}
	held_.erase(std::remove_if(held_.begin(), held_.end(),
			[&slot](const Held &held) { return held.slot == slot.value; }), held_.end());
	held_.push_back({slot.value, entity->handle.packed, entity->registry_spawn_id});
}

void DestructionEmitters::release_row(World &world, const DestructionEffectEvent &row) {
	particle::EffectScene *scene = world.item_emitters.scene();
	if (scene == nullptr || row.attach_wire_handle == EntityHandle::kInvalid) return;
	if (row.family == 4) {
		if (const Entity *entity = row_entity(world, row.attach_wire_handle))
			world.item_emitters.release_slot(*entity);
		else
			scene->detach_slot(entity_effect_slot(row.attach_wire_handle));
		return;
	}
	const particle::EffectSlotToken slot =
			destruction_effect_slot(row.attach_wire_handle, row.family, row.bank_slot);
	scene->detach_slot(slot);
	held_.erase(std::remove_if(held_.begin(), held_.end(),
			[&slot](const Held &held) { return held.slot == slot.value; }), held_.end());
}

void DestructionEmitters::move_bank(World &world, const Entity &entity, uint8_t family,
		uint8_t slot, const Vec3 &position) {
	if (particle::EffectScene *scene = world.item_emitters.scene())
		scene->move_slot(destruction_effect_slot(entity.handle.packed, family, slot),
				effect_point(position));
}

void DestructionEmitters::move_vehicle_fire(World &world, const Entity &vehicle) {
	particle::EffectScene *scene = world.item_emitters.scene();
	if (scene == nullptr) return;
	const particle::EffectSlotToken slot = destruction_effect_slot(vehicle.handle.packed, 5, 0);
	if (!scene->slot_group(slot)) return;
	const particle::Vec3 up = matrix_third_row(vehicle);
	scene->move_slot(slot, effect_point(vehicle.position), &up);
}

void DestructionEmitters::destroy_piece_trail(World &world, int piece_slot) {
	if (particle::EffectScene *scene = world.item_emitters.scene())
		scene->destroy_slot(death_piece_trail_slot(piece_slot));
}

void DestructionEmitters::spawn_piece_trail(World &world, int piece_slot, const DeathPiece &piece,
		const char *effect) {
	particle::EffectScene *scene = world.item_emitters.open_scene();
	if (scene == nullptr || effect == nullptr || effect[0] == '\0') return;
	particle::EffectSpawnRequest request;
	request.effect = scene->intern(effect);
	request.pose = particle::descriptor_pose(effect_point(piece.pos), {});
	request.binding = particle::EffectBinding::World;
	// Held in the piece's handle; the group's death clears it (the death callback sub_492D70).
	request.admission = particle::EffectAdmission::StoreOwned;
	request.slot = death_piece_trail_slot(piece_slot);
	request.source_tick = world.logic_tick;
	request.kill_plane_y = effect_water_height(world);
	scene->spawn(request);
}

void DestructionEmitters::move_piece_trail(World &world, int piece_slot, const DeathPiece &piece) {
	if (particle::EffectScene *scene = world.item_emitters.scene())
		scene->move_slot(death_piece_trail_slot(piece_slot), effect_point(piece.pos));
}

void DestructionEmitters::release_piece_trail(World &world, int piece_slot) {
	if (particle::EffectScene *scene = world.item_emitters.scene())
		scene->detach_slot(death_piece_trail_slot(piece_slot));
}

void DestructionEmitters::sync(World &world) {
	consume(world);
	particle::EffectScene *scene = world.item_emitters.scene();
	for (auto it = held_.begin(); it != held_.end();) {
		const Entity *entity = world.registry.get(EntityHandle{it->handle});
		const bool live = entity != nullptr && entity->registry_spawn_id == it->lifetime;
		const bool group = scene != nullptr && scene->slot_group({it->slot});
		if (live && group) {
			++it;
			continue;
		}
		if (scene != nullptr && !live) scene->detach_slot({it->slot});
		it = held_.erase(it);
	}
}

} // namespace opennova::world
