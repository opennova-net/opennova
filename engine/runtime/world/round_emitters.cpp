#include <runtime/world/round_emitters.h>

#include <runtime/world/ammo_table.h>
#include <runtime/world/collision.h>
#include <runtime/world/item_effects.h>
#include <runtime/world/present_drains.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/world.h>

#include <algorithm>

namespace opennova::world {

void RoundEmitters::sync(World &world) {
	particle::EffectScene *scene = world.item_emitters.open_scene();
	if (scene == nullptr) return;
	std::vector<int64_t> live;
	for (int i = 0; i < RoundSim::kCapacity; ++i) {
		const LiveRound &round = world.round_sim.rounds[static_cast<size_t>(i)];
		if (!round.active || !round.move_effect_live) continue;
		const AmmoTableEntry *ammo = world.tables.ammo.by_index(round.ammo_index);
		if (ammo == nullptr || ammo->impact_effects[1].effect.empty()) continue;
		const int64_t key = round_presentation_key(round, i);
		const particle::EffectSlotToken slot = round_move_slot(key);
		live.push_back(key);
		if (std::find(held_.begin(), held_.end(), key) != held_.end()) {
			// The held handle re-poses (a dead group's handle stays held: nothing spawns again
			// until the emitter is released).
			const auto words = round.pos.words();
			const CollisionMatrix matrix = collision_matrix_from_placement(
					round.yaw_bam, round.pitch_bam, round.roll_bam, words.data(), 0);
			particle::Vec3 at;
			particle::Vec3 forward;
			const bool aimed = emitter_transform(world, round.item_type_id,
					round_visible_item_id(round) != 0, matrix, round.pos, at, forward);
			scene->move_slot(slot, at, aimed ? &forward : nullptr);
			continue;
		}
		particle::EffectSpawnRequest request;
		request.effect = scene->intern(ammo->impact_effects[1].effect);
		request.pose = particle::descriptor_pose(effect_point(round.pos), effect_point(round.vel));
		request.admission = particle::EffectAdmission::ReplaceOwned;
		request.binding = particle::EffectBinding::World;
		request.slot = slot;
		request.source_tick = world.logic_tick;
		request.kill_plane_y = effect_water_height(world);
		const auto words = round.pos.words();
		request.section_gate = effect_section_gate(world, words.data());
		if (scene->spawn(request).spawned()) held_.push_back(key);
	}
	for (auto it = held_.begin(); it != held_.end();) {
		if (std::find(live.begin(), live.end(), *it) != live.end()) {
			++it;
			continue;
		}
		scene->detach_slot(round_move_slot(*it));
		it = held_.erase(it);
	}
}

} // namespace opennova::world
