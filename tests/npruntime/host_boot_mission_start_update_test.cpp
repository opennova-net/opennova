// The mission start's server-info update inside the host boot (HostBootRequest::
// mission_start_update): a NovaWorld authority's listing publishes, and its session's periodic
// update sends it, ahead of the load, so the update leaves the load's length ahead of the round's
// first refresh.
//
// Retail's Game_StartMission applies the session settings (@0x524662), reads the mission text
// (TextResource_LoadMissionTextBin @0x5247CE), resets the slots (@0x5248BC), then on a NovaWorld
// authority in session runs Lobby_UpdateServerInfo (called @0x5248F5) and
// CPlayerManager_RebuildLists(1) (called @0x524903), the first mission's ID report (@0x524928),
// and CNapiGameSession_ProcessPeriodicUpdate (called @0x52493D), all ahead of the load window's
// first server pump (@0x524A63) and the terrain (Game_LoadTerrainDuringConnect @0x524B26..).
//
// Pinned here, core-only (a synthetic mount, a recording socket, no retail data):
//   - a fresh session's boot runs the update once, ahead of the fresh kernel, the terrain step
//     and every server pump but the load head's (@0x5245E8, D-NET-116), past the session's
//     create, whose keys it reads (D-NET-1254);
//   - a dedicated host's map change runs it past the head (the next map's session settings on
//     the context, the old World gone with the teardown) and ahead of the window's first pump,
//     which sends the kept joiner its S2C 0x25, and ahead of the next kernel and its terrain;
//   - a boot with no update installed runs as before.
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <net/npwire/idatagram_socket.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/peer_addr.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_keys.h>
#include <net/protocol/protocol.h>
#include <runtime/inmatch/host_boot.h>
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/map_change.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/mission_rotation.h>
#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/session.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/mission/mission_sidecars.h>
#include <runtime/world/world.h>

#include "common/boot_file_source.h"
#include "common/synthetic_mission.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../common/title_fixture.h"

namespace {

using namespace opennova;
namespace ns = opennova::replication;

const Protocol &kJo = *title_protocol("jo");

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

constexpr double kFrame = 1.0 / 62.5;

const char kServerScrk[] = "SERVERSTARTUPDATESCRK3456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789A";
const char kClientScrk[] = "CLIENTSTARTUPDATESCRK3456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789A";

// A socket that counts its receive drains (each server pump's drain loop ends on one empty
// read) and records what it sends.
struct RecordingSocket final : opennova::IDatagramSocket {
	PeerAddr peer{0x0100007Fu, 23456};
	std::vector<std::vector<uint8_t>> sent;
	std::vector<PeerAddr> sent_to;
	size_t drains = 0;

	int recv_from(uint8_t *, std::size_t, PeerAddr &) override {
		++drains;
		return 0;
	}
	void send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) override {
		sent.emplace_back(data, data + len);
		sent_to.push_back(to);
	}
	size_t sent_to_peer() const {
		size_t n = 0;
		for (const PeerAddr &to : sent_to)
			if (to == peer) ++n;
		return n;
	}
};

mission_catalog::Row catalog_row(const char *file) {
	mission_catalog::Row r;
	r.file = file;
	r.title = file;
	r.game_mode = static_cast<uint32_t>(bms::AttribFlags::TeamDeathmatch);
	return r;
}

// What the update saw when it ran.
struct UpdateSeen {
	int runs = 0;
	size_t drains = 0;
	size_t sent_to_peer = 0;
	bool kernel_was_old = false;
	bool world_present = true;
	bool terrain_ran = false;
	std::string mission_file;
	std::string server_name;
	// The session the update read: created, and its round-state init's keys.
	bool in_session = false;
	uint32_t baffle_key = 0;
	uint32_t pcid_key = 0;
};

struct DedicatedHost {
	std::map<std::string, std::string> files;
	std::vector<mission_catalog::Row> catalog{catalog_row("MAPA.BMS"), catalog_row("MAPB.BMS")};
	inmatch::HostRotation rotation;
	inmatch::HostRole role{inmatch::RoleKind::DedicatedHost};
	bool role_jo = test_titles::jo_controls(role);
	inmatch::Session session{role};
	inmatch::HostBoot boot;
	std::unique_ptr<mission::MissionKernel> kernel;
	RecordingSocket socket;
	bool terrain_ran = false;
	bool install_update = true;
	UpdateSeen seen;

	DedicatedHost() {
		role.set_socket(&socket);
		role.set_rotation(&rotation);
		inmatch::seed_rotation_from_host_screen(rotation, catalog, {0, 1}, {0, 0});
	}

	inmatch::NapiNPServerCtx &ctx() { return role.state.host_owner.ctx; }

	bool boot_current(bool next_mission) {
		const std::string map_file = rotation.list.map_file;
		inmatch::HostBootRequest r;
		test_titles::jo_config(r.host_cfg.config);
		r.mission = test_mission::two_entity_mission();
		r.mission.header.attrib_flags = bms::AttribFlags::TeamDeathmatch;
		mission::sync_counts(r.mission);
		r.mission_basename = mission::mission_base_name(map_file);
		r.files = test_boot::source_over(&files);
		r.session = &session;
		r.role = &role;
		r.host = &role;
		r.host_cfg.protocol = &kJo;
		r.host_cfg.config.server_name = map_file == "MAPA.BMS" ? "Start A" : "Start B";
		r.host_cfg.config.game_type = rotation.list.map_game_type;
		r.host_cfg.config.mission_file = map_file;
		r.host_cfg.config.max_players = 9;
		r.host_cfg.config.num_teams = 2;
		r.host_cfg.socket_mode = inmatch::SocketMode::Lan;
		r.host_cfg.serve_and_play = false;
		r.next_mission = next_mission;
		r.boot_options.playable = false;
		r.boot_options.mp_session = true;
		r.boot_options.terrain = false;
		r.boot_options.game_type = rotation.list.map_game_type;
		r.boot_options.player_limit = 9;
		r.boot_options.team_count = 2;
		mission::MissionKernel *const old_kernel = kernel.get();
		terrain_ran = false;
		r.fresh_kernel = [this]() -> mission::MissionKernel & {
			auto fresh = std::make_unique<mission::MissionKernel>();
			test_titles::jo_world(fresh->world);
			if (kernel) fresh->carry_across_load_from(*kernel);
			kernel = std::move(fresh);
			return *kernel;
		};
		r.before_terrain = [this](mission::MissionKernel &) { terrain_ran = true; };
		if (install_update) {
			r.mission_start_update = [this, old_kernel] {
				++seen.runs;
				seen.drains = socket.drains;
				seen.sent_to_peer = socket.sent_to_peer();
				seen.kernel_was_old = kernel.get() == old_kernel;
				seen.world_present = ctx().world != nullptr;
				seen.terrain_ran = terrain_ran;
				seen.mission_file = ctx().config.mission_file;
				seen.server_name = ctx().config.server_name;
				seen.in_session = ctx().is_in_session != 0;
				seen.baffle_key = ctx().game_server_baffle_key;
				seen.pcid_key = ctx().cookie_key_table.current();
			};
		}
		std::string error;
		if (!inmatch::boot_host_mission(std::move(r), boot, error)) {
			std::printf("boot: %s\n", error.c_str());
			return false;
		}
		if (!inmatch::start_host_mission(boot, inmatch::HostStartDevice{}, error)) {
			std::printf("boot: %s\n", error.c_str());
			return false;
		}
		inmatch::end_host_mission_load(boot);
		return true;
	}
};

// A remote in the match on the old mission, its send clock dictated (holdoff 12) and its
// countdown at 3 when the round's teardown starts: its S2C 0x25 leaves at the load window's
// first pump.
void add_kept_joiner(DedicatedHost &host) {
	inmatch::HostOwner &owner = host.role.state.host_owner;
	const PeerAddr peer = host.socket.peer;
	inmatch::PeerLink &link = owner.peers[peer];
	link.transport = std::make_unique<ns::UdpSessionTransport>(ns::UdpSessionTransport::Role::Host);
	inmatch::NapiNPConnection c;
	c.peer = peer;
	c.type = inmatch::NapiNPConnection::kTypeServerSide;
	c.connection_id = inmatch::kFirstJoinerDcb;
	c.server_scrk = kServerScrk;
	c.client_scrk = kClientScrk;
	c.server_sk = 0x55667788u;
	c.client_ck = 0x10203040u;
	c.link.transport = link.transport.get();
	c.link.mode = ns::TransportMode::Client;
	c.seq.outbound_message_limit = JO_SESSION_OUTBOUND_MESSAGE_MAX;
	c.admission_stage = inmatch::GameAdmissionStage::Complete;
	c.phase = inmatch::ConnectionPhase::InMatch;
	c.slot_state = inmatch::PlayerSlotState::Joined;
	c.s2c_send_holdoff_dictated = true;
	c.s2c_send_holdoff_ticks = 12;
	c.s2c_send_holdoff_countdown = 3;
	c.s2c_send_boundary_open = false;
	host.ctx().np_protocol.connection_list.push_back(std::move(c));
}

// A fresh session: once, ahead of the fresh kernel, the terrain and every server pump but the
// load head's [orig: Game_StartMission - Server_PumpNetworkTransport @0x5245E8 ahead of
// Lobby_UpdateServerInfo @0x5248F5 and of the window's @0x524A63].
void test_a_fresh_session_updates_ahead_of_its_load() {
	auto host = std::make_unique<DedicatedHost>();
	CHECK(host->boot_current(/*next_mission=*/false));
	CHECK(host->seen.runs == 1);
	CHECK(host->seen.kernel_was_old); // no kernel yet: fresh_kernel had not run
	CHECK(!host->seen.terrain_ran);
	CHECK(host->seen.drains == 1); // the head's pump (D-NET-116)
	CHECK(host->socket.drains > host->seen.drains); // the window's pumps and the rest followed it
	CHECK(host->session.state() == inmatch::State::Running);
	// The session's create ran ahead of it (D-NET-1254): the update reads the keys its
	// round-state init drew, the ones the session keeps [orig: CNapiGameSession_BuildAndCreateSession
	// @0x5694D0 - Server_InitNewRoundState called @0x5695B3, ahead of GameLoop_Init's
	// Game_StartMission call @0x526377].
	CHECK(host->seen.in_session);
	CHECK(host->seen.baffle_key != 0u && host->seen.baffle_key == host->ctx().game_server_baffle_key);
	CHECK(host->seen.pcid_key != 0u && host->seen.pcid_key == host->ctx().cookie_key_table.keys[0]);
}

// A map change: past the head, ahead of the window's first pump (the kept joiner's 0x25), the
// next kernel and its terrain.
void test_a_map_change_updates_past_its_head_ahead_of_the_window() {
	auto host = std::make_unique<DedicatedHost>();
	CHECK(host->boot_current(/*next_mission=*/false));
	host->ctx().mission_exit_reason = inmatch::kMissionExitMapCycle;
	inmatch::FrameInput input;
	input.delta_seconds = kFrame;
	bool ended = false;
	for (int f = 0; f < 16 && !ended; ++f) ended = host->session.advance(input).terminal();
	CHECK(ended);
	add_kept_joiner(*host);
	host->seen = UpdateSeen{};
	const size_t sent_before = host->socket.sent_to_peer();
	const size_t drains_before = host->socket.drains;
	CHECK(inmatch::begin_host_map_change(host->role, host->catalog) ==
			inmatch::MapChangeStep::NextMission);
	// The teardown's pump and the load head's (@0x5245E8) precede the update.
	const size_t drains_at_head = host->socket.drains;
	CHECK(drains_at_head == drains_before + 2);
	CHECK(host->socket.sent_to_peer() == sent_before);
	CHECK(host->boot_current(/*next_mission=*/true));
	CHECK(host->seen.runs == 1);
	// Past the head: the next map's session settings stand on the context.
	CHECK(host->seen.mission_file == "MAPB.BMS");
	CHECK(host->seen.server_name == "Start B");
	CHECK(!host->seen.world_present);
	// Ahead of the window: no pump since the head's, so the kept joiner's 0x25 had not left.
	CHECK(host->seen.drains == drains_at_head);
	CHECK(host->seen.sent_to_peer == sent_before);
	// Ahead of the load: the old kernel still stood and the terrain had not loaded.
	CHECK(host->seen.kernel_was_old);
	CHECK(!host->seen.terrain_ran);
	// The window's pumps followed it and sent the 0x25.
	CHECK(host->socket.drains > drains_at_head);
	CHECK(host->socket.sent_to_peer() > sent_before);
	CHECK(host->session.state() == inmatch::State::Running);
}

// No update installed: the boot runs as before.
void test_no_update_installed() {
	auto host = std::make_unique<DedicatedHost>();
	host->install_update = false;
	CHECK(host->boot_current(/*next_mission=*/false));
	CHECK(host->seen.runs == 0);
	CHECK(host->session.state() == inmatch::State::Running);
}

} // namespace

int main() {
	test_a_fresh_session_updates_ahead_of_its_load();
	test_a_map_change_updates_past_its_head_ahead_of_the_window();
	test_no_update_installed();
	if (failures != 0) {
		std::printf("npruntime_host_boot_mission_start_update: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("npruntime_host_boot_mission_start_update: ok\n");
	return 0;
}
