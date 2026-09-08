// D-AI-5 on the retail data: the slice-1 threat loop (world-wac-ai-re §17).
// CP01 boots on the engine's own boot policy, the local player walks toward
// the nearest foot NPC (retail start markers deliberately spawn FARTHEST from
// the enemy set, net-re §5.2c — out of perception range), then stands still
// and watches: hostile AI fire must damage and then kill the player, and the
// AI fire events the presentation layer drains must have been produced.
// Movement rides the same PlayerInput packet the shell's input router fills;
// the look is set straight at the target in the engine's BAM frame.
// Gated on OPENNOVA_JO_DIR (a retail JO install carrying CP01.bms).
#include "common/retail_mission_files.h"
#include "common/retail_paths.h"

#include <runtime/terrain_query/height_field.h>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

using namespace opennova;
namespace w = opennova::world;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

constexpr int kMaxMissionSeconds = 420;
constexpr float kStopDistance = 15.0f;

struct Nearest {
	w::AiEntity *ai = nullptr;
	const w::Entity *entity = nullptr;
	float distance = 1e30f;
};

// The nearest live HOSTILE foot NPC that scans for threats: a mounted
// occupant rides its seat and fires via the D-AI-2 emplacement solver, and an
// NPC authored with the no-scan slot bit never perceives anyone
// [orig: aiSlot byte+4 & 1 -> no scan] — both are out of the slice-1 loop.
Nearest nearest_npc(testrig::RetailMissionRig &rig) {
	Nearest best;
	const w::Vec3 player = rig.local.player_position();
	const w::AiEntity *self = rig.local.player_ai();
	const uint8_t own_team = self != nullptr ? self->team : 1;
	for (int i = 0; i < rig.world.ai.count(); ++i) {
		w::AiEntity *e = rig.world.ai.at(i);
		if (e == nullptr || !e->inf.active) continue;
		if ((e->slot.f[1] & 1) != 0 || e->team == 0 || e->team == own_team) continue;
		const w::Entity *ent = rig.world.registry.get(e->handle);
		if (ent == nullptr || !ent->alive || ent->mounted) continue;
		const float d = testrig::distance(testrig::ai_position(*e), player);
		if (d < 0.5f) continue; // self
		if (d < best.distance) {
			best.distance = d;
			best.ai = e;
			best.entity = ent;
		}
	}
	return best;
}

} // namespace

int main() {
	RETAIL_REQUIRE_OR_SKIP(install, retail::install(),
			"OPENNOVA_JO_DIR (a retail JO install carrying CP01.bms)");
	testrig::RetailMissionRig rig;
	std::string error;
	if (!rig.open(install, "CP01.bms", error)) return retail::skip(error.c_str());
	testrig::BootOptions options;
	if (!expect(rig.boot(options, error), "CP01 boots")) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return 1;
	}
	if (!expect(rig.local.has_local_player(), "the host's own player spawned")) return 1;
	if (!expect(rig.ammo_ok, "ammo.def loaded (AI rounds need the ballistics table)")) return 1;
	rig.install_weapon("WPN_M4AUTO");

	const Nearest first = nearest_npc(rig);
	if (!expect(first.ai != nullptr, "CP01 carries at least one foot NPC")) return 1;
	std::printf("threat: nearest NPC %.1f u (item %d team %d hp %d)\n", first.distance,
			first.entity->item_id, int(first.ai->team), int(first.ai->health));

	// Exercise threat/fire behavior from an open approach to this authored NPC.
	// Crew assignment can change the farthest-point spawn choice; a straight
	// 200-unit walk from that spawn can hit the camp walls before perception.
	bool staged = false;
	const w::Vec3 target_pos = testrig::ai_position(*first.ai);
	for (int i = 0; i < 16 && !staged; ++i) {
		const double angle = i * 6.283185307179586 / 16;
		w::Vec3 start{ target_pos.x + float(24 * std::cos(angle)),
			target_pos.y + float(24 * std::sin(angle)), target_pos.z };
		if (rig.world.tables.terrain)
			start.z = terrain::height_field_height_world_bilinear(
							  *rig.world.tables.terrain, start.x, -start.y) +
					0.9f;
		if (std::abs(start.z - target_pos.z) > 2.0f)
			continue;
		const int32_t from[3] = { int32_t(start.x * 65536), int32_t(start.y * 65536),
			int32_t((start.z + 0.8f) * 65536) };
		const int32_t to[3] = { first.ai->pos[0], first.ai->pos[1], first.ai->pos[2] + 52428 };
		if (!rig.world.ai.line_of_sight_clear(
					rig.world, from, to, rig.world.cached.local_player, first.entity->handle))
			continue;
		rig.world.commands.set_entity_position(rig.world.cached.local_player, start);
		staged = true;
	}
	if (!expect(staged, "an unobstructed ground approach to the authored NPC exists"))
		return 1;

	// --- Approach: walk at the nearest NPC until it is inside the stop distance
	// or fire already lands.
	const int32_t hp0 = rig.local.player_health();
	int seconds = 0;
	int fires = 0;
	const auto count_fire_events = [&]() {
		fires += static_cast<int>(rig.world.round_sim.fired.size());
		rig.world.round_sim.fired.clear();
	};
	const auto mission_second = [&]() {
		for (int t = 0; t < 62; ++t) {
			rig.tick();
			count_fire_events();
		}
		++seconds;
	};
	rig.local.input.forward = true;
	while (seconds < kMaxMissionSeconds) {
		const Nearest npc = nearest_npc(rig);
		if (npc.ai != nullptr) {
			const w::Vec3 me = rig.local.player_position();
			w::Vec3 target = testrig::ai_position(*npc.ai);
			target.z = me.z;
			rig.local.aim_at(me, target);
		}
		mission_second();
		if (rig.local.player_health() < hp0) break; // already under fire
		const Nearest npc_now = nearest_npc(rig);
		if (npc_now.ai == nullptr) continue;
		if (npc_now.distance <= kStopDistance) break;
		if (seconds % 10 == 0)
			std::printf("threat: approach t=%ds dist=%.1fu hp=%d npc_state=%d\n", seconds,
					npc_now.distance, rig.local.player_health(), npc_now.ai->brain.cur_state());
	}
	rig.local.input.forward = false;
	{
		const Nearest here = nearest_npc(rig);
		std::printf("threat: holding at %.1fu (t=%ds) — waiting for hostile fire\n",
				here.distance, seconds);
		const w::Entity *pe = rig.local.player();
		const w::AiEntity *pa = rig.local.player_ai();
		if (pe != nullptr && pa != nullptr)
			std::printf("threat: player item=%d def=%d radar_sig=%d heat_sig=%d engine_flags=0x%x flags=0x%x refcount=%d team=%d/%d alive=%d hp=%d pool=%d\n",
					pe->item_id, int(pe->has_item_def), pe->radar_sig, pe->heat_sig, unsigned(pe->engine_flags),
					unsigned(pe->flags), int(pe->ai_target_refcount), int(pe->team), int(pa->team), int(pe->alive),
					pe->health, pe->handle.pool());
		if (here.ai != nullptr) {
			const w::AiProfile &pr = here.ai->profile;
			std::printf("threat: npc see_all=%d profile_type=%d prio=%d authority=%d in_session=%d team=%d slot_class=[%d %d %d %d] class_priority=[%d %d %d %d] fov=%d/%d flags100=0x%x\n",
					int(here.ai->see_all), pr.type, here.ai->brain.f[w::AiBrain::kPriorityTarget],
					int(rig.world.ai.is_authority), int(rig.world.ai.is_in_session), int(here.ai->team),
					pr.slot_class[0], pr.slot_class[1], pr.slot_class[2], pr.slot_class[3],
					pr.class_priority[0], pr.class_priority[1], pr.class_priority[2], pr.class_priority[3],
					pr.fov_primary, pr.fov_secondary, unsigned(pr.flags100));
			std::printf("threat: rig ai profiles=%zu:", rig.ai_profiles.size());
			for (const mission::PromoteOptions::AiProfileRow &row : rig.ai_profiles) std::printf(" %s", row.profile.c_str());
			std::printf("\n");
			if (pe != nullptr) {
				int32_t sa[3], sb[3];
				rig.world.ai.weapon_aim_origin(rig.world, *here.ai, sa);
				rig.world.ai.weapon_aim_origin(rig.world, *pe, sb);
				const bool los = rig.world.ai.line_of_sight_clear(rig.world, sa, sb, here.ai->handle, pe->handle);
				std::printf("threat: npc slot1=0x%x aim npc=(%.2f, %.2f, %.2f) player=(%.2f, %.2f, %.2f) player eye_offset=(%.2f, %.2f, %.2f) los_clear=%d ground under player=%.2f\n",
						unsigned(here.ai->slot.f[1]), sa[0] / 65536.0f, sa[1] / 65536.0f, sa[2] / 65536.0f,
						sb[0] / 65536.0f, sb[1] / 65536.0f, sb[2] / 65536.0f, pe->eye_offset_x / 65536.0f,
						pe->eye_offset_y / 65536.0f, pe->eye_offset_z / 65536.0f, int(los),
						rig.ground_height(pe->position.x, pe->position.y));
			}
		}
	}

	// --- Watch: stand still until NPC fire kills the player.
	int damaged_at = -1;
	while (seconds < kMaxMissionSeconds) {
		const int32_t hp = rig.local.player_health();
		if (hp < hp0 && damaged_at < 0) {
			damaged_at = seconds;
			std::printf("threat: DAMAGED t=%ds hp %d -> %d (hostile fire landed)\n", seconds, hp0, hp);
		}
		if (hp <= 0) {
			std::printf("threat: DEAD t=%ds — NPC fire killed the player (fire events %d)\n", seconds, fires);
			expect(damaged_at >= 0, "the player took damage before dying");
			expect(fires > 0, "the AI fire events the presentation drains were produced");
			if (failures == 0)
				std::printf("ai_threat: acquire -> fire -> damage -> kill observed on CP01\n");
			return failures == 0 ? 0 : 1;
		}
		mission_second();
		if (seconds % 10 == 0) {
			const Nearest npc = nearest_npc(rig);
			const w::AiEntity *self = rig.local.player_ai();
			std::printf("threat: t=%ds hp=%d nearest=%.1fu state=%d npc_hp=%d sight=%d attack=%d alert=%d tgt=%d team=%d/%d find_calls=%d fires=%d\n",
					seconds, hp, npc.distance, npc.ai != nullptr ? npc.ai->brain.cur_state() : -1,
					npc.ai != nullptr ? int(npc.ai->health) : 0,
					npc.ai != nullptr ? npc.ai->slot.f[w::AiSlot::kSightRange] : -1,
					npc.ai != nullptr ? npc.ai->slot.f[w::AiSlot::kAttackRange] : -1,
					npc.ai != nullptr ? int(npc.ai->slot.bytes()[w::AiSlot::kAlertByte]) : -1,
					npc.ai != nullptr ? int(npc.ai->inf.combat_target.valid()) : -1,
					npc.ai != nullptr ? int(npc.ai->team) : -1, self != nullptr ? int(self->team) : -1,
					rig.world.ai.find_target_calls, fires);
		}
	}
	std::fprintf(stderr, "FAIL: player hp=%d after %ds (damaged_at=%d) — no kill observed\n",
			rig.local.player_health(), kMaxMissionSeconds, damaged_at);
	return 1;
}
