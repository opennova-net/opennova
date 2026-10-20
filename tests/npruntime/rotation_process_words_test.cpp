// The map rotation's process words across two host sessions (D-NET-353): retail's half toggle,
// SETNEXT latch and side-to-team map are process globals that no session start or round init
// resets, so a session that ended during a launch-option map's second half hands the set toggle
// and the swapped sides to the next session in the process: its team assignment reads the
// swapped map, and its first launch-option map ends after one half. The game world owns the one
// HostRotation for its run; this pins the engine side, a second session over the same owner.
// [orig: g_RotationIsFlipped (writers Game_TeardownMission @0x522519,
//  PostMenu_RouteMissionExit @0x5685F9 / @0x568607 / @0x568624); g_RotationSetNextLatch
//  (CAdminServer_HandleMissionCommand @0x4066DF, Game_TeardownMission @0x522520);
//  g_RotationSideTeam0 / g_RotationSideTeam1 (.data 01 02, swapped @0x522501..0x522512, read by
//  Server_AssignPlayerTeam @0x4FE44B / @0x4FE492); Server_InitNewRoundState's LASTGAME clear
//  @0x51CAA4 the only rotation word a session create resets]
#include <base/gameprofile/game_type.h>
#include <formats/mission/bms.h>
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/map_change.h>
#include <runtime/inmatch/mission_rotation.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>
#include <vector>

using namespace opennova;
using namespace opennova::inmatch;

namespace {

int failures = 0;
#define CHECK(c) \
	do { \
		if (!(c)) { \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
			++failures; \
		} \
	} while (0)

mission_catalog::Row row(const char *file, bms::AttribFlags mode) {
	mission_catalog::Row r;
	r.file = file;
	r.game_mode = static_cast<uint32_t>(mode);
	return r;
}

// One host session over the process's rotation: the host screen's START seed, two kept slots
// (teams 1 and 2), the round ends through the map change.
struct Session {
	std::vector<mission_catalog::Row> cat;
	std::unique_ptr<HostRole> role = std::make_unique<HostRole>(RoleKind::DedicatedHost);

	Session(HostRotation &rotation, const std::vector<mission_catalog::Row> &catalog,
			const std::vector<int32_t> &rows, const std::vector<int32_t> &switches)
		: cat(catalog) {
		seed_rotation_from_host_screen(rotation, cat, rows, switches);
		role->set_rotation(&rotation);
		NapiNPServerCtx &ctx = role->state.host_owner.ctx;
		ctx.rotation = &rotation;
		ctx.is_authority = 1;
		ctx.is_in_session = 1;
		Server_InitNewRoundState(ctx); // the session create's round init
		for (uint8_t team : {uint8_t{1}, uint8_t{2}}) {
			NapiNPConnection conn;
			conn.phase = ConnectionPhase::Spawned;
			conn.connection_id = 10u + team;
			conn.assigned_team_valid = true;
			conn.assigned_team = team;
			conn.reply.player_slot = team;
			ctx.np_protocol.connection_list.push_back(conn);
		}
	}
	MapChangeStep round_end() {
		uint16_t slot = 3;
		for (NapiNPConnection &conn : role->state.host_owner.ctx.np_protocol.connection_list)
			conn.link.owned_entity = world::EntityHandle::make(0, slot++);
		return begin_host_map_change(*role, cat);
	}
};

// The side arm of the team assignment: side A's password answers through the map.
uint8_t side_a_team(HostRotation &rotation) {
	auto world = std::make_unique<world::World>();
	world->registry.configure_pool(0, 8);
	GameConfig config;
	config.game_type = game_type::kTeamDeathmatch;
	config.num_teams = 2;
	config.side_a_password = "blue";
	std::vector<NapiNPConnection> roster(1);
	roster[0].join_password = "blue";
	return Server_ReservePlayerTeam(config, true, roster, roster[0], *world, &rotation);
}

void test_words_carry_into_the_next_session() {
	const std::vector<mission_catalog::Row> cat = {
		row("TDM_A.BMS", bms::AttribFlags::TeamDeathmatch),
		row("DM_C.BMS", bms::AttribFlags::Deathmatch),
	};
	std::vector<int32_t> switches(cat.size(), 0);
	switches[0] = 1; // TDM_A plays as two halves
	HostRotation process; // the game world's, alive for the process
	CHECK(side_a_team(process) == 1);
	{
		// Session 1 ends during TDM_A's second half: the toggle set, the sides swapped.
		Session one(process, cat, {0, 1}, switches);
		CHECK(one.round_end() == MapChangeStep::NextMission);
		CHECK(process.is_flipped && process.side_team[0] == 2 && process.side_team[1] == 1);
		process.last_game = true; // a LASTGAME toggle the next create clears
	}
	// Session 2 in the same process: its START seed and round init reset the list and
	// LASTGAME, not the toggle or the side map.
	Session two(process, cat, {0, 1}, switches);
	CHECK(!process.last_game && process.list.cursor == 0 && process.list.map_file == "TDM_A.BMS");
	CHECK(process.is_flipped && process.side_team[0] == 2 && process.side_team[1] == 1);
	CHECK(side_a_team(process) == 2); // side A's password lands on team 2
	// Its first launch-option map ends after one half: the swap back, the toggle cleared and
	// the advance.
	CHECK(two.round_end() == MapChangeStep::NextMission);
	CHECK(!process.is_flipped && process.list.cursor == 1 && process.list.map_file == "DM_C.BMS");
	CHECK(process.side_team[0] == 1 && process.side_team[1] == 2);
}

void test_setnext_latch_carries() {
	const std::vector<mission_catalog::Row> cat = {
		row("DM_C.BMS", bms::AttribFlags::Deathmatch),
		row("TDM_A.BMS", bms::AttribFlags::TeamDeathmatch),
	};
	std::vector<int32_t> switches(cat.size(), 0);
	switches[1] = 1;
	HostRotation process;
	{
		// A SETNEXT on the map without the option leaves the latch set; the session ends there.
		Session one(process, cat, {0, 1}, switches);
		process.setnext_latch = true;
	}
	Session two(process, cat, {1, 0}, switches);
	CHECK(process.setnext_latch && !process.is_flipped);
	// The next session's first launch-option map: the latch skips the swap and its second half.
	CHECK(two.round_end() == MapChangeStep::NextMission);
	CHECK(!process.setnext_latch && !process.is_flipped && process.list.cursor == 1);
	CHECK(process.side_team[0] == 1);
}

} // namespace

int main() {
	test_words_carry_into_the_next_session();
	test_setnext_latch_carries();
	if (failures != 0) {
		std::printf("rotation_process_words: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("rotation_process_words: ok\n");
	return 0;
}
