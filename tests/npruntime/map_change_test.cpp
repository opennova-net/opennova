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
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/host_boot.h>
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/map_change.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/mission_rotation.h>
#include <runtime/inmatch/server_admin_command.h>
#include <runtime/inmatch/session.h>
#include <runtime/world/world.h>

#include "common/boot_file_source.h"
#include "common/synthetic_mission.h"

#include "net_datagram_socket.h"
#include "net_sockets.h"

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
		r.mission_basename = map_file.substr(0, map_file.rfind('.'));
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

} // namespace

int main() {
	if (net::startup() != 0) return (std::printf("FAIL net::startup\n"), 1);
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
