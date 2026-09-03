// Wave-1 native assets (ADR 0028 trunk, S3b/S16 full) on the retail data: the
// sim resolves every collision/occlusion .3di through its OWN SimModelCache,
// the boot installs the seat/mount table through the NATIVE extractor, the
// booted world keeps producing native hitboxes with ZERO provider declines,
// the by-name weapon install bakes the FSM from the retained weapon.def row,
// and the native inmatch::Session 62.5 Hz bank drives the loop
// [orig: Game_MainLoop @0x52b630]. The S9 boot's native file resolution (the
// mission text fallback, the .aip profile speeds) is checked against an
// independent read of the same files.
// Gated on OPENNOVA_JO_ASSETS (an extracted retail tree carrying 00TRg.bms);
// the attach count pins the recorded retail value on that tree (859).
#include "common/retail_mission_files.h"
#include "common/retail_paths.h"

#include <runtime/world/player_spawn.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

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

constexpr int kExpectAttach = 859;

std::string lower(std::string s) {
	for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

// The pre-S9 shell resolution of the .aip speed table, the independent
// reference for the engine's resolve_ai_profiles.
std::map<std::string, std::pair<int, int>> oracle_aip_speeds(const bms::File &mission, const ResourceIndex &index) {
	std::map<std::string, std::pair<int, int>> out;
	const auto visit = [&](const std::vector<bms::Entity> &entities) {
		for (const bms::Entity &e : entities) {
			std::string profile = lower(std::string(e.name2, strnlen(e.name2, sizeof(e.name2))));
			while (!profile.empty() && std::isspace(static_cast<unsigned char>(profile.back()))) profile.pop_back();
			if (profile.empty() || out.count(profile)) continue;
			std::vector<uint8_t> bytes;
			if (!index.has_file(profile + ".aip") || !index.read_file(profile + ".aip", bytes) || bytes.empty())
				continue;
			int patrol = -1, combat = -1;
			std::istringstream in(std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
			std::string line;
			while (std::getline(in, line)) {
				std::istringstream tokens(line);
				std::string key, value;
				if (!(tokens >> key >> value)) continue;
				key = lower(key);
				if (key == "patrol_speed") patrol = std::atoi(value.c_str());
				else if (key == "combat_speed") combat = std::atoi(value.c_str());
			}
			if (patrol != -1 || combat != -1) out[profile] = {patrol, combat};
		}
	};
	visit(mission.items);
	visit(mission.buildings);
	visit(mission.markers);
	visit(mission.organics);
	return out;
}

} // namespace

int main() {
	RETAIL_REQUIRE_OR_SKIP(assets, retail::assets(),
			"OPENNOVA_JO_ASSETS (an extracted retail tree carrying 00TRg.bms)");
	std::string error;

	// --- The bare sim: promote, the native attach sweep, the by-name weapon
	// install and the session bank.
	{
		testrig::RetailMissionRig rig;
		if (!rig.open(assets, "00TRg.bms", error)) return retail::skip(error.c_str());
		if (!expect(rig.items_ok, "items.def parses from the tree")) return 1;
		// The bare promote: no seat/mount table (the recorded attach count is
		// the model sweep alone), no terrain, no scripts, no host session.
		testrig::BootOptions bare;
		bare.playable = false;
		bare.terrain = false;
		bare.wac = false;
		bare.seat_specs = false;
		bare.listen_server = false;
		if (!expect(rig.boot(bare, error), "00TRg promotes")) {
			std::fprintf(stderr, "  %s\n", error.c_str());
			return 1;
		}
		std::printf("native-assets: attached native=%d (expect %d)\n", rig.collision_attached, kExpectAttach);
		expect(rig.collision_attached == kExpectAttach, "the native attach count matches the recorded retail value");
		expect(rig.weapon_defs_ok, "weapon.def loaded from the tree");
		w::PlayerSpawn spawn;
		spawn.team = 1;
		const w::EntityHandle h = w::spawn_player(rig.world, spawn);
		if (!expect(h.valid(), "spawn_local_player at the origin")) return 1;
		if (!expect(rig.install_weapon("WPN_M4AUTO"), "WPN_M4AUTO installs by name against the retail root")) return 1;
		std::printf("native-assets: m4 by-name: active=%d clip=%d\n", int(rig.local.weapon.active), rig.local.weapon.slot.clip);
		expect(rig.local.weapon.active, "the by-name install left an active weapon FSM");
		expect(!rig.install_weapon("WPN_NOT_A_WEAPON"), "an unknown weapon name does not install");

		inmatch::Session session(rig);
		if (!expect(session.begin_load().applied() && session.complete_load().applied(), "the session loads")) return 1;
		inmatch::FrameInput frame;
		frame.delta_seconds = 0.032;
		expect(session.advance(frame).ticks_run() == 2, "inmatch::Session::advance(0.032) runs 2 ticks");
		frame.delta_seconds = 0.001;
		expect(session.advance(frame).ticks_run() == 0, "inmatch::Session::advance(0.001) runs 0 ticks");
		frame.delta_seconds = 2.0;
		expect(session.advance(frame).ticks_run() == 31, "inmatch::Session::advance(2.0) runs 31 ticks (spiral clamp)");
	}

	// --- The S9 boot on the retail data through the engine's boot policy: the
	// booted world poses natively end to end, and the native file resolution
	// matches the independent reads.
	{
		testrig::RetailMissionRig rig;
		if (!rig.open(assets, "00TRg.bms", error)) return retail::skip(error.c_str());
		testrig::BootOptions options;
		if (!expect(rig.boot(options, error), "00TRg boots (S9)")) {
			std::fprintf(stderr, "  %s\n", error.c_str());
			return 1;
		}
		int hitbox_entities = 0;
		for (int round = 0; round < 8; ++round) {
			rig.tick(8);
			hitbox_entities = std::max(hitbox_entities,
					static_cast<int>(rig.hitboxes(rig.local.player_position(), 80.0f, 96, 24000).size()));
		}
		std::printf("native-assets: native pose: hitbox_entities=%d collision queries=%d declines=%d mounted declines=%d sources=%zu\n",
				hitbox_entities, rig.collision_queries, rig.collision_declines, rig.mounted_declines,
				rig.mounted_graphics.size());
		expect(hitbox_entities > 0, "the native collision provider produced hitboxes");
		expect(rig.collision_queries > 0 && rig.collision_declines == 0, "native collision never declined");
		expect(rig.mounted_declines == 0, "the native mounted resolver never declined");
		expect(!rig.mounted_graphics.empty(), "the boot installed native mounted model sources");

		// The mission text: <mission>.bin when it exists, else medmssn.bin.
		std::vector<uint8_t> legacy_text;
		if (rig.index.has_file("00TRg.bin")) rig.index.read_file("00TRg.bin", legacy_text);
		else rig.index.read_file("medmssn.bin", legacy_text);
		std::printf("native-assets: s9 text=%zu bytes (source %d) vs independent read %zu\n", rig.text_size,
				rig.text_source, legacy_text.size());
		expect(rig.text_size == legacy_text.size(), "the S9 mission-text resolution matches the independent read");

		const std::map<std::string, std::pair<int, int>> oracle = oracle_aip_speeds(rig.mission, rig.index);
		std::map<std::string, std::pair<int, int>> native;
		for (const mission::PromoteOptions::AiProfileRow &row : rig.ai_profiles)
			native[lower(row.profile)] = {row.data.patrol_speed, row.data.combat_speed};
		std::printf("native-assets: s9 aip rows native=%zu oracle=%zu\n", native.size(), oracle.size());
		expect(native.size() == oracle.size(), "the S9 .aip row count matches the independent walk");
		for (const auto &kv : oracle) {
			const auto found = native.find(kv.first);
			char msg[160];
			std::snprintf(msg, sizeof(msg), "profile '%s' speeds match (native %d/%d vs %d/%d)", kv.first.c_str(),
					found != native.end() ? found->second.first : -99, found != native.end() ? found->second.second : -99,
					kv.second.first, kv.second.second);
			expect(found != native.end() && found->second == kv.second, msg);
		}
	}

	if (failures == 0) std::printf("native_assets_00trg: the native asset paths hold on the retail tree\n");
	return failures == 0 ? 0 : 1;
}
