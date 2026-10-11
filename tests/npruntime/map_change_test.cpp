// The host's round-end map change (D-NET-331, inmatch/map_change.h) end to
// end over real UDP, with no retail data: a dedicated host (HostRole over a
// bound socket, booted through the one host boot over an in-memory mount)
// runs a two-map rotation whose first map is a launch-option Team Deathmatch
// map, and a LAN joiner (a bare ClientRuntime on a second socket) plays
// through it. The joiner keeps its one connection through every map change
// and reloads in place (C2S 0x48 / 0x47 / 0x33, the 0x60 and 0x64 transfers
// under the bumped tokens, the admission legs); the first map's second half
// swaps its team (and the side-to-team map), and the next map's first half
// has the sides back.
#include <base/gameprofile/game_type.h>
#include <formats/def/def.h>
#include <formats/mission/bms.h>
#include <formats/mission/mission.h>
#include <formats/mission/bms_edit.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/host_boot.h>
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/map_change.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/mission_rotation.h>
#include <runtime/inmatch/server_admin_command.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/session.h>
#include <runtime/mission/mission_sidecars.h>
#include <runtime/world/world.h>

#include "common/boot_file_source.h"
#include "common/synthetic_mission.h"

#include "net_datagram_socket.h"
#include "net_sockets.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

constexpr double kFrame = 1.0 / 62.5;

// A Team Deathmatch mission: the two placed entities every kernel test boots
// and one start marker per team (6003 team 1, 6004 team 2).
bms::File team_mission(const char *name) {
	bms::File m = test_mission::two_entity_mission();
	m.header.attrib_flags = bms::AttribFlags::TeamDeathmatch;
	std::snprintf(m.header.mission_name, sizeof(m.header.mission_name), "%s", name);
	for (const int32_t type_id : {6003, 6004}) {
		bms::Entity marker{};
		marker.type = bms::ItemType::Marker;
		marker.type_id = type_id;
		marker.x = (type_id == 6003 ? 40 : 80) << 16;
		marker.y = 40 << 16;
		marker.id = type_id;
		m.items.push_back(marker);
	}
	mission::sync_counts(m);
	return m;
}

mission_catalog::Row catalog_row(const char *file) {
	mission_catalog::Row r;
	r.file = file;
	r.title = file;
	r.game_mode = static_cast<uint32_t>(bms::AttribFlags::TeamDeathmatch);
	return r;
}

// The dedicated host over its bound socket: the role, its session and the
// rotation live for the whole run; each map boots a fresh kernel.
struct Host {
	std::map<std::string, std::string> files;
	std::vector<mission_catalog::Row> catalog{catalog_row("MAPA.BMS"), catalog_row("MAPB.BMS")};
	inmatch::HostRotation rotation;
	inmatch::HostRole role{inmatch::RoleKind::DedicatedHost};
	inmatch::Session session{role};
	inmatch::HostBoot boot;
	std::unique_ptr<mission::MissionKernel> kernel;
	net::ScopedSocket socket;
	std::unique_ptr<net::NetDatagramSocket> datagrams;
	uint16_t port = 0;
	int missions = 0;
	int map_changes = 0;
	// A kept slot that held an entity when the boot's bring-up returned.
	bool kept_slot_spawned_in_bringup = false;

	bool start() {
		socket = net::ScopedSocket(net::udp_bind(0, &port));
		if (!socket.is_valid()) return false;
		datagrams = std::make_unique<net::NetDatagramSocket>(socket.get());
		role.set_socket(datagrams.get());
		role.set_rotation(&rotation);
		// The host screen's START over MAPA (its Switch cell set: two halves)
		// and MAPB (its Switch cell cleared).
		inmatch::seed_rotation_from_host_screen(rotation, catalog, {0, 1}, {1, 0});
		return boot_current(/*next_mission=*/false);
	}

	bool boot_current(bool next_mission) {
		const std::string map_file = rotation.list.map_file;
		inmatch::HostBootRequest r;
		r.mission = team_mission(map_file == "MAPA.BMS" ? "Map A" : "Map B");
		r.mission_basename = mission::mission_base_name(map_file);
		r.files = test_boot::source_over(&files);
		r.session = &session;
		r.role = &role;
		r.host = &role;
		r.host_cfg.config.server_name = "Rotation Test";
		r.host_cfg.config.game_type = rotation.list.map_game_type;
		r.host_cfg.config.mission_file = map_file;
		r.host_cfg.config.mission_name = r.mission.get_mission_name();
		r.host_cfg.config.max_players = 9;
		r.host_cfg.config.num_teams = 2;
		r.host_cfg.config.replay_enabled = 0;
		r.host_cfg.socket_mode = inmatch::SocketMode::Lan;
		r.host_cfg.serve_and_play = false;
		r.next_mission = next_mission;
		r.boot_options.playable = false;
		r.boot_options.mp_session = true;
		r.boot_options.terrain = false;
		r.boot_options.game_type = rotation.list.map_game_type;
		r.boot_options.player_limit = 9;
		r.boot_options.team_count = 2;
		r.after_bringup = [this](bool) {
			for (const inmatch::NapiNPConnection &c : ctx().np_protocol.connection_list)
				if (c.link.owned_entity.valid()) kept_slot_spawned_in_bringup = true;
		};
		r.fresh_kernel = [this]() -> mission::MissionKernel & {
			auto fresh = std::make_unique<mission::MissionKernel>();
			if (kernel) fresh->carry_across_load_from(*kernel);
			kernel = std::move(fresh);
			return *kernel;
		};
		std::string error;
		if (!inmatch::boot_host_mission(std::move(r), boot, error) ||
				!inmatch::start_host_mission(boot, inmatch::HostStartDevice{}, error)) {
			std::printf("boot: %s\n", error.c_str());
			return false;
		}
		++missions;
		// A map change's round init spawns the kept slots after the PreMission
		// pass, none of them in the bring-up ahead of it (D-NET-354); a fresh
		// session has none. [orig: Game_StartMission @0x524360 -- the
		//  EventTrigger_UpdateAllWithFlag2 call @0x525B86, the
		//  Server_InitAllPlayerEntitiesForRound call @0x525BAF]
		const std::vector<std::string> &trace = kernel->boot_trace;
		const auto at = [&trace](const char *name) {
			return static_cast<size_t>(std::find(trace.begin(), trace.end(), name) - trace.begin());
		};
		if (next_mission) {
			++map_changes;
			CHECK(at("premission") < trace.size());
			CHECK(at("premission") < at("round_init") && at("round_init") + 1 == trace.size());
			CHECK(!kept_slot_spawned_in_bringup);
		} else {
			CHECK(at("round_init") == trace.size());
		}
		kept_slot_spawned_in_bringup = false;
		return true;
	}

	// One outer frame; a round end's mission exit runs the map change.
	// False once the session ended.
	bool frame() {
		inmatch::FrameInput input;
		input.delta_seconds = kFrame;
		const inmatch::FrameOutcome outcome = session.advance(input);
		kernel->world.out.discard_presentation();
		if (!outcome.terminal()) return true;
		const int32_t reason = role.state.host_owner.ctx.mission_exit_reason;
		if (reason != inmatch::kMissionExitMapCycle && reason != inmatch::kMissionExitRoundOver)
			return false;
		if (inmatch::begin_host_map_change(role, catalog) == inmatch::MapChangeStep::RotationEnded) {
			(void)session.close();
			return false;
		}
		return boot_current(/*next_mission=*/true);
	}

	inmatch::NapiNPServerCtx &ctx() { return role.state.host_owner.ctx; }
	inmatch::NapiNPConnection *remote() {
		for (inmatch::NapiNPConnection &c : ctx().np_protocol.connection_list)
			if (c.type == inmatch::NapiNPConnection::kTypeServerSide) return &c;
		return nullptr;
	}
	int remotes() {
		int n = 0;
		for (const inmatch::NapiNPConnection &c : ctx().np_protocol.connection_list)
			if (c.type == inmatch::NapiNPConnection::kTypeServerSide) ++n;
		return n;
	}
	uint8_t remote_entity_team() {
		inmatch::NapiNPConnection *c = remote();
		if (c == nullptr) return 0xFF;
		const world::Entity *e = kernel->world.registry.get(c->link.owned_entity);
		return e != nullptr ? e->team : 0xFF;
	}
};

// A playable listen host's map change (serve_and_play): its own loopback slot is
// a kept slot, so the round init after the PreMission pass spawns its player
// once, binds that body to its items.def row at the spawn (its sound profile)
// and the mission start's Attack & Defend latch, after the round init, finds it.
// [orig: Game_StartMission @0x524360 -- the Server_InitAllPlayerEntitiesForRound
//  call @0x525BAF, the sub_524110 call @0x5260C1; Entity_SpawnFromAnimSlotProperty
//  @0x43C390 -> Entity_InitFromModel @0x40DC30]
struct PlayHost {
	std::map<std::string, std::string> files;
	std::vector<mission_catalog::Row> catalog;
	inmatch::HostRotation rotation;
	inmatch::HostRole role{inmatch::RoleKind::ListenHost};
	inmatch::Session session{role};
	inmatch::HostBoot boot;
	std::unique_ptr<mission::MissionKernel> kernel;
	std::vector<def::DefItemDef> rows;
	def::DefItemsFile items{};
	net::ScopedSocket socket;
	std::unique_ptr<net::NetDatagramSocket> datagrams;

	PlayHost() {
		uint16_t port = 0;
		socket = net::ScopedSocket(net::udp_bind(0, &port));
		datagrams = std::make_unique<net::NetDatagramSocket>(socket.get());
		role.set_socket(datagrams.get());
		files["SndProf.def"] =
				"begin \"default\"\r\nend\r\n"
				"begin \"SP_Host\"\r\n     SSLFootGND     T_DIRT_L\r\nend\r\n";
		mission_catalog::Row row = catalog_row("ADMAP.BMS");
		row.game_mode = static_cast<uint32_t>(bms::AttribFlags::AttackAndDefend);
		catalog.push_back(row);
		// The player's row names its sound profile; the placed item is an A&D objective (attrib 0x8000).
		rows.resize(2);
		rows[0].id = static_cast<int>(world::kPlayerInfantryTypeId) + static_cast<int>(mission::kItemIdOffset);
		rows[0].hp = 150;
		std::snprintf(rows[0].sound_profile, sizeof(rows[0].sound_profile), "SP_Host");
		rows[1].id = 164 + static_cast<int>(mission::kItemIdOffset);
		rows[1].hp = 100;
		rows[1].attrib = 0x8000u;
		items.entries = rows.data();
		items.count = rows.size();
		role.set_rotation(&rotation);
		// Its Switch cell set: the first half's end replays it, the sides swapped.
		inmatch::seed_rotation_from_host_screen(rotation, catalog, {0}, {1});
	}

	bool boot_map(bool next_mission) {
		inmatch::HostBootRequest r;
		r.mission = test_mission::two_entity_mission();
		r.mission.header.attrib_flags = bms::AttribFlags::AttackAndDefend;
		mission::sync_counts(r.mission);
		r.mission_basename = "ADMAP";
		r.files = test_boot::source_over(&files);
		r.items = &items;
		r.session = &session;
		r.role = &role;
		r.host = &role;
		r.host_cfg.config.server_name = "Play Host";
		r.host_cfg.config.game_type = game_type::kAttackDefend;
		r.host_cfg.config.mission_file = "ADMAP.BMS";
		r.host_cfg.config.max_players = 9;
		r.host_cfg.config.num_teams = 2;
		r.host_cfg.socket_mode = inmatch::SocketMode::Lan;
		r.host_cfg.serve_and_play = true;
		r.next_mission = next_mission;
		r.boot_options.playable = true;
		r.boot_options.mp_session = true;
		r.boot_options.terrain = false;
		r.boot_options.game_type = game_type::kAttackDefend;
		r.boot_options.player_limit = 9;
		r.boot_options.team_count = 2;
		r.fresh_kernel = [this]() -> mission::MissionKernel & {
			auto fresh = std::make_unique<mission::MissionKernel>();
			if (kernel) fresh->carry_across_load_from(*kernel);
			kernel = std::move(fresh);
			return *kernel;
		};
		std::string error;
		if (!inmatch::boot_host_mission(std::move(r), boot, error) ||
				!inmatch::start_host_mission(boot, inmatch::HostStartDevice{}, error)) {
			std::printf("play boot: %s\n", error.c_str());
			return false;
		}
		return true;
	}

	inmatch::NapiNPConnection *loopback() {
		for (inmatch::NapiNPConnection &c : role.state.host_owner.ctx.np_protocol.connection_list)
			if (c.type == inmatch::NapiNPConnection::kTypeClientSide) return &c;
		return nullptr;
	}

	// The pool-0 bodies the loopback slot owns.
	int own_bodies() {
		inmatch::NapiNPConnection *c = loopback();
		if (c == nullptr) return -1;
		int n = 0;
		kernel->world.registry.for_each_in_pool(0, [&](const world::Entity &e) {
			if (e.item_id == world::kPlayerInfantryTypeId && e.owner_connection_id == c->connection_id) ++n;
		});
		return n;
	}
};

void test_play_host_map_change() {
	PlayHost host;
	CHECK(host.boot_map(/*next_mission=*/false));
	if (host.kernel == nullptr) return;
	// The fresh session's own player, once the session pumps it in.
	for (int f = 0; f < 120 && !(host.loopback() != nullptr && host.loopback()->link.owned_entity.valid()); ++f) {
		inmatch::FrameInput input;
		input.delta_seconds = kFrame;
		(void)host.session.advance(input);
		host.kernel->world.out.discard_presentation();
	}
	CHECK(host.loopback() != nullptr && host.loopback()->link.owned_entity.valid());
	// The round's end: the Cycle command and its linger, to the mission exit.
	CHECK(inmatch::Server_ExecuteServerCommand(host.role.state.host_owner.ctx, &host.kernel->world, "Cycle", "", {})
					.handled);
	bool ended = false;
	for (int f = 0; f < 4000 && !ended; ++f) {
		inmatch::FrameInput input;
		input.delta_seconds = kFrame;
		ended = host.session.advance(input).terminal();
		host.kernel->world.out.discard_presentation();
	}
	if (!ended)
		std::printf("play host: exit %d, world exit %d, in session %u\n",
				host.role.state.host_owner.ctx.mission_exit_reason, host.kernel->world.mission_exit_reason,
				unsigned(host.role.state.host_owner.ctx.is_in_session));
	CHECK(ended);
	CHECK(inmatch::begin_host_map_change(host.role, host.catalog) == inmatch::MapChangeStep::NextMission);
	CHECK(host.boot_map(/*next_mission=*/true));
	inmatch::NapiNPConnection *own = host.loopback();
	CHECK(own != nullptr && own->link.owned_entity.valid());
	if (own == nullptr || !own->link.owned_entity.valid()) return;
	// Spawned once, by the round init alone (the kernel's own spawn step stood down).
	CHECK(host.own_bodies() == 1);
	CHECK(host.kernel->world.cached.local_player == own->link.owned_entity);
	const std::vector<std::string> &trace = host.kernel->boot_trace;
	CHECK(std::find(trace.begin(), trace.end(), "spawn_local_player") == trace.end());
	CHECK(!trace.empty() && trace.back() == "round_init");
	// Bound at its spawn: its row's sound profile, not the slotless default.
	const world::AiEntity *body = host.kernel->world.ai.for_handle(own->link.owned_entity);
	const world::Entity *e = host.kernel->world.registry.get(own->link.owned_entity);
	CHECK(body != nullptr && body->profile.sound_profile == 1);
	CHECK(e != nullptr && e->has_item_def);
	// The A&D latch found the host's player and the objective.
	CHECK(host.kernel->local.attack_defend_role != 0);
	std::printf("map_change: the listen host's own player respawned once, bound, A&D latched\n");
}

// A joiner the host admits mid-match: the spawn pump's body binds its items.def
// row at its spawn, as every retail body does, so it carries its row's sound
// profile (the chute slot sounds' source). (D-NET-395)
// [orig: CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0 -> Server_BuildPlayerInfoAndAdd
//  @0x51d560 -> Server_PlayerAdd -> Entity_SpawnFromAnimSlotProperty @0x43C390 ->
//  Entity_InitFromModel, the call @0x43C492]
void test_play_host_mid_match_join_binds_body() {
	PlayHost host;
	CHECK(host.boot_map(/*next_mission=*/false));
	if (host.kernel == nullptr) return;
	for (int f = 0; f < 120; ++f) {
		inmatch::FrameInput input;
		input.delta_seconds = kFrame;
		(void)host.session.advance(input);
		host.kernel->world.out.discard_presentation();
	}
	inmatch::NapiNPServerCtx &ctx = host.role.state.host_owner.ctx;
	inmatch::NapiNPConnection joining;
	joining.type = inmatch::NapiNPConnection::kTypeServerSide;
	joining.connection_id = inmatch::kFirstJoinerDcb;
	joining.self_id_seen = true;
	joining.phase = inmatch::ConnectionPhase::Joined;
	ctx.np_protocol.connection_list.push_back(joining);
	CHECK(inmatch::Server_ProcessPendingPlayerSpawns(ctx, host.kernel->world) == 1);
	const world::EntityHandle body_handle = ctx.np_protocol.connection_list.back().link.owned_entity;
	CHECK(body_handle.valid() && body_handle != host.kernel->world.cached.local_player);
	const world::AiEntity *body = host.kernel->world.ai.for_handle(body_handle);
	CHECK(body != nullptr && body->profile.sound_profile == 1);
	std::printf("map_change: a mid-match joiner's body bound at its spawn\n");
}

} // namespace

int main() {
	if (net::startup() != 0) return (std::printf("FAIL net::startup\n"), 1);
	test_play_host_map_change();
	test_play_host_mid_match_join_binds_body();
	Host host;
	CHECK(host.start());
	if (host.kernel == nullptr) return 1;
	CHECK(host.rotation.list.map_file == "MAPA.BMS" && host.rotation.list.map_launch_option == 1);
	CHECK(host.rotation.previous_game_type == game_type::kTeamDeathmatch);

	const net::Endpoint host_ep{{127, 0, 0, 1}, host.port};
	uint16_t joiner_port = 0;
	net::ScopedSocket joiner_sock(net::udp_bind(0, &joiner_port));
	CHECK(joiner_sock.is_valid());
	inmatch::ClientRuntime client("RotationJoiner");
	auto ship = [&](const std::vector<uint8_t> &d) {
		if (!d.empty()) net::udp_send_to(joiner_sock.get(), host_ep, d.data(), d.size());
	};
	auto drain = [&]() {
		uint8_t rx[4096];
		net::Endpoint from{};
		for (;;) {
			const int n = net::udp_recv_from(joiner_sock.get(), rx, sizeof(rx), from, 0);
			if (n <= 0) break;
			client.receive(rx, static_cast<size_t>(n));
		}
	};
	uint32_t tick = 1;
	// One frame of each side; a stored joiner exit of 4 in session is the Game
	// Loop, the reload in place.
	int reloads = 0;
	bool host_up = true;
	auto step = [&]() {
		host_up = host.frame();
		drain();
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) ship(d);
		++tick;
		const int32_t reason = client.mission_exit_reason();
		if (reason == 0) return;
		CHECK(reason == inmatch::kMissionExitRoundOver);
		CHECK(inmatch::main_frame_exit(reason, /*in_session=*/true, /*authority=*/false) ==
				inmatch::MainFrameExit::GameLoop);
		CHECK(client.begin_mission_reload());
		++reloads;
	};
	auto run_until = [&](auto done, int frames) {
		for (int f = 0; f < frames && host_up && !done(); ++f) step();
		return done();
	};

	// --- the join into map A's first half.
	ship(client.start());
	CHECK(run_until([&] { return client.in_match() && client.is_deployed(); }, 1200));
	inmatch::NapiNPConnection *conn = host.remote();
	CHECK(conn != nullptr);
	if (conn == nullptr) return 1;
	const uint32_t connection_id = conn->connection_id;
	CHECK(conn->assigned_team == 1 && host.remote_entity_team() == 1);
	CHECK(client.assigned_team() == 1);
	CHECK(client.self_handle() == conn->link.owned_entity.packed);

	// One round end: the Cycle command ends the round with the 620-tick linger;
	// frames run until the joiner is back in a match in the next mission.
	auto round_end = [&](int missions) {
		CHECK(inmatch::Server_ExecuteServerCommand(host.ctx(), &host.kernel->world, "Cycle", "", {})
						.handled);
		const int reloads_before = reloads;
		return run_until([&] {
			return reloads > reloads_before && host.missions == missions && client.in_match() &&
					client.is_deployed() && host.remote() != nullptr &&
					host.remote()->link.owned_entity.valid() &&
					client.self_handle() == host.remote()->link.owned_entity.packed;
		}, 4000);
	};

	// --- map A's second half: the same entry, the sides swapped.
	CHECK(round_end(2));
	CHECK(host.rotation.is_flipped && host.rotation.list.cursor == 0);
	CHECK(host.rotation.side_team[0] == 2 && host.rotation.side_team[1] == 1);
	CHECK(host.remotes() == 1 && host.remote()->connection_id == connection_id);
	CHECK(host.remote()->assigned_team == 2 && host.remote_entity_team() == 2);
	// The 0x47's S2C 0x75 re-latched the joiner's team.
	CHECK(client.assigned_team() == 2);
	CHECK(client.map_file() == "MAPA.BMS");
	CHECK(host.ctx().server_info_transfer_id == 2 && host.ctx().mission_metadata_transfer_id == 2);
	CHECK(!client.session_lost());

	// --- map B: the swap undone at the second half's end, the advance taken.
	CHECK(round_end(3));
	CHECK(!host.rotation.is_flipped && host.rotation.list.cursor == 1);
	CHECK(host.rotation.side_team[0] == 1 && host.rotation.side_team[1] == 2);
	CHECK(host.ctx().config.mission_file == "MAPB.BMS");
	CHECK(host.remotes() == 1 && host.remote()->connection_id == connection_id);
	CHECK(host.remote()->assigned_team == 1 && host.remote_entity_team() == 1);
	CHECK(client.assigned_team() == 1);
	// The 0x64 block named the next map's file [orig: @0x4324DD].
	CHECK(client.map_file() == "MAPB.BMS");
	CHECK(reloads == 2);
	CHECK(host.map_changes == 2);
	std::printf("map_change: joiner kept connection %u through %d missions\n", connection_id,
			host.missions);

	// --- map B's end with REPLAY off: the rotation runs out, the session ends
	//     with StopServer's goodbye, and the joiner's link ends with it.
	CHECK(inmatch::Server_ExecuteServerCommand(host.ctx(), &host.kernel->world, "Cycle", "", {})
					.handled);
	for (int f = 0; f < 2000 && host_up; ++f) step();
	CHECK(!host_up);
	for (int f = 0; f < 120 && !client.session_lost(); ++f) {
		drain();
		(void)client.Client_ProcessNetworkFrame(tick++);
	}
	CHECK(client.session_lost());
	CHECK(client.has_disconnect_event() &&
			client.last_disconnect_event().ddstr == "NP.C:SH:STOP");
	net::shutdown();
	if (failures != 0) {
		std::printf("map_change: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("map_change: ok\n");
	return 0;
}
