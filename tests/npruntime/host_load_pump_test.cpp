// The session host's in-load server pump and the load's spawn gate (D-NET-116).
//
// Retail's session host pumps its server protocol inside the blocking mission load
// (Server_PumpNetworkTransport @0x4FD960 from Game_StartMission @0x5245E8 .. @0x525AC5:
// the manager receive @0x4FD977, the server receive @0x4FD981, the server send
// @0x4FD98B), so every C2S that arrives during the load is handled during it, and
// g_NetSpawnSuspended (set @0x524398, cleared @0x52631B) makes three of those handlers
// answer nothing: NapiNPServerMsg_0x029 (@0x514F1E, no S2C 0x51),
// NapiNPServerMsg_SendEmptySlots (@0x51A609, no S2C 0x5D) and the C2S 0x28 page through
// Server_CollectValidWeaponSlots (@0x516005, no S2C 0x4E). No server pump runs between
// the last in-load call (@0x525AC5) and the gate's clear, so the load end's S2C 0x7B
// (@0x526267) leaves at the first tick's send pump (Server_TickUpdate @0x51E487).
//
// Pinned here, core-only (a synthetic mount, a recording socket, no retail data):
//   - the three handlers drop under the gate and answer with it clear;
//   - a fresh session host's load runs all its pumps once its session stands (the
//     bring-up), with the gate set, so a hello queued during the load is answered during
//     the load; a dedicated host's load pumps ten times (@0x5245E8 and the nine of the
//     window @0x524A63 .. @0x525AC5, the render arm's @0x5259EC skipped @0x5259B4), each
//     stepping the send clocks as retail's receive does;
//   - a map change pumps at its teardown (the gate clear, the old World, ahead of the slot
//     reset: Game_TeardownMission @0x522588), at its load's head (the gate set, no World
//     yet), then the window's nine after the per-slot reset (the kept slot's ping word
//     already zeroed), never after the 0x7B;
//   - the gate is clear once the load ends.
#include <base/gameprofile/gameprofile.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <net/npwire/idatagram_socket.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/peer_addr.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/title/title_identity.h>
#include <runtime/inmatch/host_boot.h>
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/map_change.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/mission_rotation.h>
#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/session.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/mission/mission_sidecars.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include "common/boot_file_source.h"
#include "common/synthetic_mission.h"
#include "conn_fixture.h"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace w = opennova::world;
namespace ns = opennova::replication;

const TitleIdentity &kJo = *title_identity(gameprofile::GAME_JO);

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

constexpr double kFrame = 1.0 / 62.5;

// --------------------------------------------------------------------------
// The three handlers' gate.
// --------------------------------------------------------------------------

struct HandlerFixture {
	inmatch::NapiNPServerCtx ctx;
	w::World world;
	ns::UdpSessionTransport transport{ns::UdpSessionTransport::Role::Host};
	w::EntityHandle player;

	HandlerFixture() {
		inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
		ctx.is_in_session = 1;
		world.rules.mp_session = true;
		world.registry.configure_pool(0, 8);
		world.registry.configure_pool(1, 4);
		world.registry.configure_pool(2, 4);
		ctx.world = &world;
		w::PlayerSpawn spawn;
		spawn.position = {10.0f, 0.0f, 0.0f};
		spawn.team = 1;
		player = w::spawn_remote_player(world, spawn);
		inmatch::NapiNPConnection conn = conn_fixture::make_seeded_conn(
				inmatch::kFirstJoinerDcb, 1, &transport, ns::TransportMode::Client, player, true);
		conn.reply.player_slot = 1;
		conn.reply.player_name = "P1";
		conn.assigned_team = 1;
		conn.assigned_team_valid = true;
		ctx.np_protocol.connection_list.push_back(std::move(conn));
		// One entity on the team-change list, so a C2S 0x29 [0] resolves.
		ctx.team_change_entities.push_back(player);
	}

	std::vector<ProtocolMessage> dispatch(uint8_t tag, std::vector<uint8_t> body) {
		inmatch::ServerDispatchInputs inputs;
		inputs.server_ctx = &ctx;
		std::vector<ProtocolMessage> msgs;
		msgs.push_back(make_protocol_message(tag, std::move(body)));
		return inmatch::dispatch_session_replies(ctx.config, ctx.np_protocol.connection_list[0],
				msgs, 100, ctx.np_protocol.connection_list, &world, inputs);
	}
};

size_t count_tag(const std::vector<ProtocolMessage> &replies, uint8_t tag) {
	size_t n = 0;
	for (const ProtocolMessage &m : replies) n += m.tag == tag ? 1 : 0;
	return n;
}

// [orig: NapiNPServerMsg_0x029 @0x514F10 (@0x514F1E / jnz @0x514F25);
//  NapiNPServerMsg_SendEmptySlots @0x51A600 (@0x51A609 / jnz @0x51A610);
//  NapiNPServerMsg_HandleWeaponLoadoutRequest @0x51A550 -> Server_CollectValidWeaponSlots
//  @0x516000 (@0x516005 / jnz @0x516022), the send @0x51A5D1..0x51A5F4]
void test_handlers_drop_under_the_load_gate() {
	// The kill page's request: the whole window, the walk from handle 0.
	const std::vector<uint8_t> page_request = {0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0};
	for (const uint32_t suspended : {0u, 1u}) {
		HandlerFixture f;
		f.ctx.net_spawn_suspended = suspended;
		const size_t expected = suspended != 0 ? 0 : 1;
		CHECK(count_tag(f.dispatch(c2s::TEAM_SPAWN_ACK, {0, 0}), s2c::TEAM_CHANGE_CONFIRM) ==
				expected);
		CHECK(count_tag(f.dispatch(c2s::EMPTY_SLOT_SWEEP_REQUEST, {}), s2c::EMPTY_SLOT_SWEEP) ==
				expected);
		CHECK(count_tag(f.dispatch(c2s::LOADOUT_REQUEST, page_request), s2c::KILL_BY_SLOT) ==
				expected);
	}
}

// --------------------------------------------------------------------------
// The load's pump points.
// --------------------------------------------------------------------------

// A socket that records each receive drain: the drain loop calls recv_from until it
// returns 0, so a pump with nothing left to read makes one empty call. A queued
// datagram is handed over first.
struct RecordingSocket final : opennova::IDatagramSocket {
	std::deque<std::vector<uint8_t>> inbound;
	PeerAddr peer{0x0100007Fu, 23456};
	int empty_drains = 0;
	std::vector<std::vector<uint8_t>> sent;
	std::function<void()> on_drain;

	int recv_from(uint8_t *buf, std::size_t cap, PeerAddr &from) override {
		if (!inbound.empty()) {
			std::vector<uint8_t> d = std::move(inbound.front());
			inbound.pop_front();
			const std::size_t n = d.size() < cap ? d.size() : cap;
			for (std::size_t i = 0; i < n; ++i) buf[i] = d[i];
			from = peer;
			return static_cast<int>(n);
		}
		++empty_drains;
		if (on_drain) on_drain();
		return 0;
	}
	void send_to(const PeerAddr &, const uint8_t *data, std::size_t len) override {
		sent.emplace_back(data, data + len);
	}
};

bms::File team_mission(const char *name) {
	bms::File m = test_mission::two_entity_mission();
	m.header.attrib_flags = bms::AttribFlags::TeamDeathmatch;
	std::snprintf(m.header.mission_name, sizeof(m.header.mission_name), "%s", name);
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

struct Host {
	std::map<std::string, std::string> files;
	std::vector<mission_catalog::Row> catalog{catalog_row("MAPA.BMS"), catalog_row("MAPB.BMS")};
	inmatch::HostRotation rotation;
	inmatch::HostRole role{inmatch::RoleKind::DedicatedHost};
	inmatch::Session session{role};
	inmatch::HostBoot boot;
	std::unique_ptr<mission::MissionKernel> kernel;
	RecordingSocket socket;
	// What the boot's hooks saw: the drains so far at the bring-up's tail, and the
	// load gate there.
	int drains_at_bringup = -1;
	uint32_t gate_at_bringup = 0;
	size_t sent_at_bringup = 0;

	inmatch::NapiNPServerCtx &ctx() { return role.state.host_owner.ctx; }

	bool boot_current(bool next_mission) {
		const std::string map_file = rotation.list.map_file;
		inmatch::HostBootRequest r;
		r.host_cfg.title = &kJo;
		r.mission = team_mission(map_file == "MAPA.BMS" ? "Map A" : "Map B");
		r.mission_basename = mission::mission_base_name(map_file);
		r.files = test_boot::source_over(&files);
		r.session = &session;
		r.role = &role;
		r.host = &role;
		r.host_cfg.config.server_name = "Load Pump";
		r.host_cfg.config.game_type = rotation.list.map_game_type;
		r.host_cfg.config.mission_file = map_file;
		r.host_cfg.config.mission_name = r.mission.get_mission_name();
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
		r.after_bringup = [this](bool) {
			drains_at_bringup = socket.empty_drains;
			gate_at_bringup = ctx().net_spawn_suspended;
			sent_at_bringup = socket.sent.size();
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
		return true;
	}
};

// A fresh session host: its session stands inside the load, and the load pumps it
// once there, with the gate set; a hello that reached the socket during the load is
// answered then, ahead of the first tick. No pump follows the load end's 0x7B.
// [orig: Server_PumpNetworkTransport @0x4FD960 (Game_StartMission @0x52589D .. @0x525AC5,
//  ahead of the PreMission pass @0x525B86); the gate @0x524398 / @0x52631B]
void test_fresh_load_pumps_its_session() {
	auto host = std::make_unique<Host>();
	host->role.set_socket(&host->socket);
	host->role.set_rotation(&host->rotation);
	inmatch::seed_rotation_from_host_screen(host->rotation, host->catalog, {0, 1}, {0, 0});
	// A LAN probe waiting in the socket when the load starts.
	ClientHello hello = make_title_client_hello(kJo, 1);
	hello.co = "LoadProbe";
	host->socket.inbound.push_back(
			nw_encode_outbound(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello)));
	CHECK(host->boot_current(/*next_mission=*/false));
	// The bring-up pumped ten times, the gate held, and the 0x81 left inside the load.
	CHECK(host->drains_at_bringup == 10);
	CHECK(host->gate_at_bringup == 1);
	CHECK(host->sent_at_bringup == 1);
	CHECK(host->socket.inbound.empty());
	// Nothing pumped after the bring-up: the load end's 0x7B waits for the first tick.
	CHECK(host->socket.empty_drains == 10);
	CHECK(host->ctx().net_spawn_suspended == 0);
	CHECK(host->session.state() == inmatch::State::Running);
}

// A map change: the teardown's pump (the gate clear, the old World, ahead of the slot
// reset), the next load's first (the gate set, the World gone), the window's nine after
// the per-slot reset, none after the 0x7B; the gate is clear once the load ends.
// [orig: Game_TeardownMission @0x522588 (ahead of the reset @0x52269B); Game_StartMission
//  @0x524360 - the set @0x524398, Server_PumpNetworkTransport @0x5245E8, the per-slot reset
//  @0x5248BC, the pumps @0x524A63 .. @0x525AC5, the 0x7B @0x526267, the clear @0x52631B;
//  Server_ResetAllPlayerSlotFrameState @0x4FC320 (the ping word @0x4FC356)]
void test_map_change_load_pumps_after_the_slot_reset() {
	auto host = std::make_unique<Host>();
	host->role.set_socket(&host->socket);
	host->role.set_rotation(&host->rotation);
	inmatch::seed_rotation_from_host_screen(host->rotation, host->catalog, {0, 1}, {0, 0});
	CHECK(host->boot_current(/*next_mission=*/false));
	// A kept connection whose ping word the next mission's per-slot reset zeroes.
	inmatch::NapiNPConnection kept;
	kept.connection_id = inmatch::kFirstJoinerDcb;
	kept.type = inmatch::NapiNPConnection::kTypeServerSide;
	kept.reply.rtt_ms = 77;
	host->ctx().np_protocol.connection_list.push_back(std::move(kept));
	// Each drain from here: whether a World stood, the gate, and the kept ping word.
	struct Drain {
		bool world = false;
		uint32_t gate = 0;
		int32_t rtt_ms = -1;
	};
	std::vector<Drain> drains;
	host->socket.on_drain = [&host, &drains]() {
		Drain d;
		d.world = host->ctx().world != nullptr;
		d.gate = host->ctx().net_spawn_suspended;
		for (const inmatch::NapiNPConnection &c : host->ctx().np_protocol.connection_list)
			if (c.connection_id == inmatch::kFirstJoinerDcb)
				d.rtt_ms = static_cast<int32_t>(c.reply.rtt_ms);
		drains.push_back(d);
	};
	// The round end's mission exit ends the frame; the map change follows.
	host->ctx().mission_exit_reason = inmatch::kMissionExitMapCycle;
	inmatch::FrameInput input;
	input.delta_seconds = kFrame;
	bool ended = false;
	for (int f = 0; f < 16 && !ended; ++f) ended = host->session.advance(input).terminal();
	CHECK(ended);
	drains.clear();
	CHECK(inmatch::begin_host_map_change(host->role, host->catalog) ==
			inmatch::MapChangeStep::NextMission);
	CHECK(drains.size() == 2);
	if (drains.size() == 2) {
		// The teardown's: the old World, the gate clear.
		CHECK(drains[0].world);
		CHECK(drains[0].gate == 0);
		CHECK(drains[0].rtt_ms == 77);
		// The load's first: no World yet, the gate set.
		CHECK(!drains[1].world);
		CHECK(drains[1].gate == 1);
		CHECK(drains[1].rtt_ms == 77);
	}
	CHECK(host->boot_current(/*next_mission=*/true));
	CHECK(drains.size() == 11);
	for (size_t i = 2; i < drains.size(); ++i) {
		CHECK(drains[i].world);
		CHECK(drains[i].gate == 1);
		CHECK(drains[i].rtt_ms == 0);
	}
	CHECK(host->ctx().net_spawn_suspended == 0);
	CHECK(host->session.state() == inmatch::State::Running);
}

} // namespace

int main() {
	test_handlers_drop_under_the_load_gate();
	test_fresh_load_pumps_its_session();
	test_map_change_load_pumps_after_the_slot_reset();
	if (failures != 0) {
		std::printf("host_load_pump: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("host_load_pump: ok\n");
	return 0;
}
