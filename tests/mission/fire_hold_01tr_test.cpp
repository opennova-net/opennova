// 01TR ("Training: Land Vehicles") on the retail data: the player's team holds
// its fire at the spawn. The nearest hostile rows are team-2 concrete lane
// dividers (items.def 102289..102291, decorations with no hp and no armor). An
// hp-0 def's init writes its two armor words to the invulnerable 0xFFFF
// [orig: Entity_InitFromModel @0x40DC95 / @0x40DC9F], and the AI target walk
// skips a candidate whose def carries that pair [orig: Entity_FindTargets
// @0x53AC3F..0x53AC59], so the riflemen by the spawn (SSNs 2, 70, 71) find
// nothing to shoot until the course events hand them targets. Before the port
// they emptied their rifles into the dividers from the first second.
// `--verbose` dumps the shooters and every round; `--seconds N` sets the watch.
// Gated on OPENNOVA_JO_DIR (a retail JO install carrying 01TR.bms).
#include "common/retail_mission_files.h"
#include "common/retail_paths.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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

} // namespace

int main(int argc, char **argv) {
	int seconds = 30;
	bool verbose = false;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = std::atoi(argv[++i]);
		else if (std::strcmp(argv[i], "--verbose") == 0) verbose = true;
	}
	RETAIL_REQUIRE_OR_SKIP(install, retail::install(),
			"OPENNOVA_JO_DIR (a retail JO install carrying 01TR.bms)");
	testrig::RetailMissionRig rig;
	std::string error;
	if (!rig.open(install, "01TR.bms", error)) return retail::skip(error.c_str());
	testrig::BootOptions options;
	if (!expect(rig.boot(options, error), "01TR boots")) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return 1;
	}
	const w::Entity *player = rig.local.player();
	const int player_team = player != nullptr ? int(player->team) : -1;
	if (verbose) {
		const auto dump = [&](int ssn) {
			const w::Entity *e = rig.world.registry.by_net_id(static_cast<uint16_t>(ssn));
			if (e == nullptr) { std::printf("dump ssn=%d: none\n", ssn); return; }
			const w::AiEntity *ai = rig.world.ai.for_handle(e->handle);
			std::printf("dump ssn=%d pool=%d pos=(%.1f %.1f %.1f) team=%d group=%d item=%d type=%d kind=%d "
					"flags=0x%x attrib=0x%x armor=(%d,%d) bms=%d sel=(%d %d %d %d)",
					ssn, e->handle.pool(), e->position.x, e->position.y, e->position.z, int(e->team),
					int(e->group_id), int(e->item_id), int(e->item_type), int(e->kind),
					unsigned(e->flags | e->engine_flags), unsigned(e->item_attrib), int(e->armor_impact),
					int(e->armor_kz), int(e->bms_id), int(e->target_selectors.exclusive_ssn),
					int(e->target_selectors.preferred_ssn), int(e->target_selectors.exclusive_group),
					int(e->target_selectors.preferred_group));
			if (ai != nullptr)
				std::printf(" ai: slot1=0x%x sight=%d attack=%d alert=%d active=%d",
						unsigned(ai->slot.f[1]), ai->slot.f[w::AiSlot::kSightRange],
						ai->slot.f[w::AiSlot::kAttackRange], int(ai->slot.bytes()[w::AiSlot::kAlertByte]),
						int(ai->inf.active));
			std::printf("\n");
		};
		if (player != nullptr)
			std::printf("player pos=(%.1f %.1f %.1f) team=%d\n", player->position.x, player->position.y,
					player->position.z, int(player->team));
		for (int ssn : {2, 70, 71, 1780, 1786, 1789}) dump(ssn);
	}
	std::map<int, int> fires_by_ssn;
	int team_fires = 0;
	for (int tick = 0; tick < seconds * 62; ++tick) {
		rig.tick();
		for (const w::FireEvent &f : rig.world.round_sim.fired) {
			const w::Entity *s = rig.world.registry.get(f.shooter);
			if (s == nullptr || s == rig.local.player()) continue;
			const w::AiEntity *ai = rig.world.ai.for_handle(f.shooter);
			const bool same_team = int(s->team) == player_team;
			if (same_team) ++team_fires;
			if (fires_by_ssn[s->net_id]++ == 0 || verbose) {
				const w::Entity *t = ai != nullptr ? rig.world.registry.get(ai->inf.combat_target) : nullptr;
				std::printf("fire t=%.2fs ssn=%d team=%d group=%d kind=%d item=%d mounted=%d blind=%d"
						" -> target ssn=%d team=%d group=%d kind=%d item=%d flags=0x%x\n",
						tick / 62.0, int(s->net_id), int(s->team), int(s->group_id), int(s->kind),
						int(s->item_id), int(s->mounted),
						ai != nullptr ? int(ai->slot.f[w::AiSlot::kBehaviorFlags] & 1) : -1,
						t ? int(t->net_id) : -1, t ? int(t->team) : -1, t ? int(t->group_id) : -1,
						t ? int(t->kind) : -1, t ? int(t->item_id) : -1,
						t ? unsigned(t->flags | t->engine_flags) : 0u);
			}
		}
		// The rig has no presenter to drain the fire events.
		rig.world.round_sim.fired.clear();
	}
	std::printf("fire_hold_01tr: %d rounds from the player's team (%d) in %d s; shooters:", team_fires,
			player_team, seconds);
	for (const auto &[ssn, n] : fires_by_ssn) std::printf(" %d:%d", ssn, n);
	std::printf("\n");
	expect(team_fires == 0, "no NPC of the player's team fires before the player rides the course");
	return failures == 0 ? 0 : 1;
}
