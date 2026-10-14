// The host rules a session runs by its title (docs/titles/dfx2-vs-jo-re.md §7, DFX2-8..DFX2-19):
// the cfg block's table names the session's title, and dfx2.exe's caps, built-in score table and
// fanfare pair, round-end linger, deploy protection, and its want of a medic revive, heal and the
// downed-state resend each branch on it. Jointops.exe's values stand beside each.
#include <base/gameprofile/game_type.h>
#include <base/gameprofile/gameprofile.h>
#include <formats/gamecfg/game_cfg.h>
#include <runtime/inmatch/host_config.h>
#include <runtime/inmatch/host_settings.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_medic.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/session_status.h>
#include <runtime/world/world.h>

#include "common/test_expect.h"

#include <cstdio>
#include <memory>
#include <string>

using namespace opennova;
using namespace opennova::inmatch;

namespace {

constexpr int kJo = gameprofile::GAME_JO;
constexpr int kDfx2 = gameprofile::GAME_DFX2;

// The block's table is its title's: a dfx2.exe block makes a dfx2.exe session, whose create takes
// the cap into 1..251. [orig: CNapiGameSession_BuildAndCreateSession @0x569554..0x56956C] [orig:
//  @0x5676f5..0x56770e (dfx2.exe)]
int session_title_from_the_cfg_table() {
	gamecfg::GameCfg jo = gamecfg::defaults();
	jo.mp_max_players = 200; // the host file's MaxPlayers, which no screen clamps
	const HostScreenState jo_host = host_session_settings(jo);
	TEST_EXPECT(jo_host.config.title == kJo && jo_host.player_limit == 65 && jo.mp_max_players == 65);

	gamecfg::GameCfg dfx2 = gamecfg::defaults({}, 0, 0, gamecfg::ConfigTable::Dfx2);
	TEST_EXPECT(dfx2.mp_max_players == 16); // its table's default
	dfx2.mp_max_players = 200;
	dfx2.dedicated = 1;
	HostScreenState host = host_session_settings(dfx2);
	TEST_EXPECT(host.config.title == kDfx2 && host.player_limit == 200);
	TEST_EXPECT(host.config.max_players == 200u && host.config.player_slot_limit() == 201u);
	dfx2.mp_max_players = 400;
	host = host_session_settings(dfx2);
	TEST_EXPECT(host.player_limit == 251 && dfx2.mp_max_players == 251);
	TEST_EXPECT(host.config.player_slot_limit() == 252u); // the ceiling tests the pre-increment cap
	return 0;
}

// The table's fanfare pair and VAR values seed the session before score.ini: a VERSION 40 file
// without EXP_FANFARE keeps the title's pair; one that passes the gate replaces it.
// [orig: GameType_CreateDefaultSettings @0x52DD00 (@0x52dd30, @0x52dd45); ScoreConfig_LoadFile
//  @0x52DC5F..0x52DC9F] [orig: @0x52aeb0, @0x52aec5 (dfx2.exe)]
int score_table_seeds_by_title() {
	const std::string bare = "VERSION 40\r\nGAMETYPE \"TDM\"\r\n";
	for (const int title : {kJo, kDfx2}) {
		GameConfig config;
		config.title = title;
		config.game_type = game_type::kTeamDeathmatch;
		TEST_EXPECT(load_session_score_config(config, bare));
		TEST_EXPECT(config.exp_fanfare == (title == kDfx2 ? 0x0A04 : 0));
		TEST_EXPECT(config.session_status_stat_values.has_value());
		TEST_EXPECT((*config.session_status_stat_values)[3] == (title == kDfx2 ? 2 : 5));
		TEST_EXPECT((*config.session_status_stat_values)[37] == (title == kDfx2 ? 6 : 5));
		GameConfig with_pair = config;
		TEST_EXPECT(load_session_score_config(with_pair, bare + "EXP_FANFARE 3 9\r\n"));
		TEST_EXPECT(with_pair.exp_fanfare == 0x0903);
	}
	return 0;
}

// [orig: Server_ProcessRoundEnd @0x5164F0 (@0x5166c4)] [orig: @0x513730 (@0x51391e) (dfx2.exe)]
int round_end_linger() {
	TEST_EXPECT(round_end_linger_ticks_for(kJo) == 2790u);
	TEST_EXPECT(round_end_linger_ticks_for(kDfx2) == 1550u);
	return 0;
}

// dfx2.exe halves the deploy's protection into a target that is a vehicle or a spawn-point item;
// a deploy without one keeps 620, and Jointops.exe stores 620 in all three arms.
// [orig: Server_DeployPlayer @0x517740 (@0x517937, @0x517952, @0x517960)] [orig:
//  @0x514990 (@0x514b78, @0x514b93, @0x514ba1) (dfx2.exe)]
int deploy_protection() {
	world::Entity vehicle;
	vehicle.has_item_def = true;
	vehicle.item_type = 1;
	world::Entity zone;
	zone.has_item_def = true;
	zone.item_type = 5;
	zone.item_attrib = world::kItemAttribSpawnPoint;
	world::Entity crate;
	crate.has_item_def = true;
	crate.item_type = 5;
	for (const world::Entity *target : {&vehicle, &zone, &crate}) {
		TEST_EXPECT(deploy_spawn_protection_ticks(kJo, target) == 620);
	}
	TEST_EXPECT(deploy_spawn_protection_ticks(kJo, nullptr) == 620);
	TEST_EXPECT(deploy_spawn_protection_ticks(kDfx2, &vehicle) == 310);
	TEST_EXPECT(deploy_spawn_protection_ticks(kDfx2, &zone) == 310);
	TEST_EXPECT(deploy_spawn_protection_ticks(kDfx2, &crate) == 620);
	TEST_EXPECT(deploy_spawn_protection_ticks(kDfx2, nullptr) == 620);
	return 0;
}

// dfx2.exe keeps the medic dispatcher's tests over two empty functions: an admitted interaction
// revives and heals nothing. Its server tick has no downed-state resend either, so the resend's
// 62-tick countdown never runs. [orig: GameEvent_HandleMedicInteraction @0x4e6790; Server_TickUpdate
//  @0x51E2D0..0x51E378] [orig: @0x4e38d0 (nullsub_114 @0x4fcff0, nullsub_115 @0x4fd000);
//  Server_TickUpdate @0x51a810 (dfx2.exe)]
int no_revive_heal_or_downed_resend() {
	for (const int title : {kJo, kDfx2}) {
		auto storage = std::make_unique<world::World>();
		world::World &w = *storage;
		w.rules.title = title;
		w.registry.configure_pool(0, 4);
		world::Entity body;
		body.kind = world::EntityKind::Organic;
		body.has_item_def = true;
		body.item_type = 3;
		body.health = 10;
		body.health_max = 100;
		const world::EntityHandle victim = w.registry.spawn(0, body);
		body.health = 100;
		body.player_class = 5;
		const world::EntityHandle healer = w.registry.spawn(0, body);
		w.round_sim.medic_interactions.push_back(world::MedicInteraction{victim, healer, false});
		NapiNPServerCtx ctx;
		ctx.is_authority = 1;
		ctx.is_in_session = 1;
		Server_RouteMedicInteractions(ctx, w);
		TEST_EXPECT(w.round_sim.medic_interactions.empty());
		TEST_EXPECT(w.registry.get(victim)->health == 10); // no player slots: no heal in either

		world::MatchRules rules;
		rules.title = title;
		rules.game_type = game_type::kTeamDeathmatch;
		w.match.configure(rules);
		w.team_downed_resend_countdown = 0;
		Server_ResendTeamDownedStates(ctx, w);
		TEST_EXPECT(w.team_downed_resend_countdown == (title == kDfx2 ? 0u : 62u));
	}
	return 0;
}

} // namespace

int main() {
	if (session_title_from_the_cfg_table() != 0) return 1;
	if (score_table_seeds_by_title() != 0) return 1;
	if (round_end_linger() != 0) return 1;
	if (deploy_protection() != 0) return 1;
	if (no_revive_heal_or_downed_resend() != 0) return 1;
	std::printf("OK: title host rules\n");
	return 0;
}
