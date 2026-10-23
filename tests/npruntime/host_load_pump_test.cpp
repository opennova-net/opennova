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
//   - a fresh session host's load pumps its session (created ahead of the load) at retail's
//     points, with the gate set: the head's pump past the create and ahead of the
//     mission-start update, the window's two and the three stages' before the World stands,
//     the bring-up's four over it, so a hello queued before the load is answered at its head;
//     a dedicated host's load pumps ten times (@0x5245E8 and the nine of the window
//     @0x524A63 .. @0x525AC5, the render arm's @0x5259EC skipped @0x5259B4), each stepping
//     the send clocks as retail's receive does;
//   - a map change pumps at its teardown (the gate clear, the old World, ahead of the slot
//     reset: Game_TeardownMission @0x522588), at its load's head (the gate set, no World
//     yet), then the window's nine after the per-slot reset (the kept slot's ping word
//     already zeroed; the first five before the next World stands), never after the 0x7B;
//   - the gate is clear once the load ends;
//   - the reset's 0x25 to a kept remote leaves at the load pump its send countdown's
//     residual names (the head for 1), ahead of the 0x7B, or with the 0x7B at the first
//     tick once the residual passes the load's pumps (holdoff 12, residual 11), and an
//     admitting remote's C2S 0x01 read inside the load is answered there (its S2C 0x02).
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
#include <cstddef>
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
		// The World on the heap (the 1 MB Windows stack).
		const auto f = std::make_unique<HandlerFixture>();
		f->ctx.net_spawn_suspended = suspended;
		const size_t expected = suspended != 0 ? 0 : 1;
		CHECK(count_tag(f->dispatch(c2s::TEAM_SPAWN_ACK, {0, 0}), s2c::TEAM_CHANGE_CONFIRM) ==
				expected);
		CHECK(count_tag(f->dispatch(c2s::EMPTY_SLOT_SWEEP_REQUEST, {}), s2c::EMPTY_SLOT_SWEEP) ==
				expected);
		CHECK(count_tag(f->dispatch(c2s::LOADOUT_REQUEST, page_request), s2c::KILL_BY_SLOT) ==
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
	std::vector<PeerAddr> sent_to;
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
	void send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) override {
		sent.emplace_back(data, data + len);
		sent_to.push_back(to);
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
	// load gate there; the drains, the sends and the World at the mission-start update.
	int drains_at_bringup = -1;
	uint32_t gate_at_bringup = 0;
	size_t sent_at_bringup = 0;
	int drains_at_update = -1;
	size_t sent_at_update = 0;
	bool world_at_update = true;

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
		r.mission_start_update = [this] {
			drains_at_update = socket.empty_drains;
			sent_at_update = socket.sent.size();
			world_at_update = ctx().world != nullptr;
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

// A fresh session host: its session stands before the load (the create), and the load pumps it
// at retail's points from its head, the gate set at each: the head's pump past the create and
// ahead of the mission-start update, which answers a hello that reached the socket before the
// load; the window's two and the three stages' before the World stands; the bring-up's four
// over it, ahead of the PreMission pass. No pump follows the load end's 0x7B.
// [orig: Game_StartMission @0x524360 - the gate @0x524398, Server_PumpNetworkTransport
//  @0x5245E8 ahead of Lobby_UpdateServerInfo @0x5248F5, @0x524A63 / @0x524F76 (the window),
//  @0x524F8C / @0x525256 / @0x5253EA (the stages) ahead of Entity_InitAllFromModels @0x52567F,
//  then @0x52589D .. @0x525AC5 ahead of the PreMission pass @0x525B86 (the render arm's
//  @0x5259EC skipped by the dedicated test @0x5259B4), the clear @0x52631B; GameLoop_Init's
//  Game_StartMission call @0x526377 past the create]
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
	// Each drain: whether a World stood, and the gate.
	struct Drain {
		bool world = false;
		uint32_t gate = 0;
	};
	std::vector<Drain> drains;
	host->socket.on_drain = [&host, &drains]() {
		drains.push_back({host->ctx().world != nullptr, host->ctx().net_spawn_suspended});
	};
	CHECK(host->boot_current(/*next_mission=*/false));
	// The head's pump alone ran ahead of the mission-start update, and its 0x81 left there.
	CHECK(host->drains_at_update == 1);
	CHECK(host->sent_at_update == 1);
	CHECK(!host->world_at_update);
	CHECK(host->socket.inbound.empty());
	// Ten pumps by the bring-up's tail, the gate held at each: the head's, the window's two and
	// the stages' three before the World stands, the bring-up's four over it.
	CHECK(host->drains_at_bringup == 10);
	CHECK(host->gate_at_bringup == 1);
	CHECK(host->sent_at_bringup == 1);
	CHECK(drains.size() == 10);
	for (size_t i = 0; i < drains.size(); ++i) {
		CHECK(drains[i].world == (i >= 6));
		CHECK(drains[i].gate == 1);
	}
	// Nothing pumped after the bring-up: the load end's 0x7B waits for the first tick.
	CHECK(host->socket.empty_drains == 10);
	CHECK(host->ctx().net_spawn_suspended == 0);
	CHECK(host->session.state() == inmatch::State::Running);
}

// A map change: the teardown's pump (the gate clear, the old World, ahead of the slot
// reset), the next load's first (the gate set, the World gone), the window's nine after
// the per-slot reset (the head's two and the three stages' before the objects, so before
// the next World stands, then the bring-up's four), none after the 0x7B; the gate is clear
// once the load ends.
// [orig: Game_TeardownMission @0x522588 (ahead of the reset @0x52269B); Game_StartMission
//  @0x524360 - the set @0x524398, Server_PumpNetworkTransport @0x5245E8, the per-slot reset
//  @0x5248BC, the pumps @0x524A63 .. @0x5253EA ahead of Entity_InitAllFromModels @0x52567F,
//  then @0x52589D .. @0x525AC5, the 0x7B @0x526267, the clear @0x52631B;
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
		// The head's two and the stages' three run before the next World stands, the
		// bring-up's four over it.
		CHECK(drains[i].world == (i >= 7));
		CHECK(drains[i].gate == 1);
		CHECK(drains[i].rtt_ms == 0);
	}
	CHECK(host->ctx().net_spawn_suspended == 0);
	CHECK(host->session.state() == inmatch::State::Running);
}

// --------------------------------------------------------------------------
// The send clock across a map change: the reset's 0x25 and an in-load admission reply.
// --------------------------------------------------------------------------

const char kServerScrk[] = "SERVERLOADPUMPSCRK123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789A";
const char kClientScrk[] = "CLIENTLOADPUMPSCRK123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789A";

// A remote past its connect, its socket transport owned by the host's peer table.
inmatch::NapiNPConnection &add_session_conn(Host &host, const PeerAddr &peer, uint32_t dcb,
		uint32_t server_sk) {
	inmatch::HostOwner &owner = host.role.state.host_owner;
	inmatch::PeerLink &link = owner.peers[peer];
	link.transport = std::make_unique<ns::UdpSessionTransport>(ns::UdpSessionTransport::Role::Host);
	inmatch::NapiNPConnection c;
	c.peer = peer;
	c.type = inmatch::NapiNPConnection::kTypeServerSide;
	c.connection_id = dcb;
	c.server_scrk = kServerScrk;
	c.client_scrk = kClientScrk;
	c.server_sk = server_sk;
	c.client_ck = 0x10203040u;
	c.link.transport = link.transport.get();
	c.link.mode = ns::TransportMode::Client;
	c.seq.outbound_message_limit = JO_SESSION_OUTBOUND_MESSAGE_MAX;
	host.ctx().np_protocol.connection_list.push_back(std::move(c));
	return host.ctx().np_protocol.connection_list.back();
}

// The S2C records of each session datagram sent to `peer`, with the index of the pump that
// sent it: drain k's mark is the count sent before pump k's send, so a datagram at index i
// left in the last pump whose mark is <= i.
struct SentPacket {
	size_t pump = 0;
	std::vector<ProtocolMessage> records;
};

std::vector<SentPacket> packets_to(const RecordingSocket &socket, const PeerAddr &peer,
		const std::vector<size_t> &marks) {
	std::vector<SentPacket> out;
	for (size_t i = 0; i < socket.sent.size(); ++i) {
		if (!(socket.sent_to[i] == peer)) continue;
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		if (!nw_decode_inbound(socket.sent[i].data(), socket.sent[i].size(), opcode, body) ||
				opcode != SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE)
			continue;
		ProtocolPacketHeader header;
		SentPacket p;
		if (!decode_protocol_packet_plaintext(body.data(), body.size(), kServerScrk, header,
					p.records))
			continue;
		for (size_t k = 0; k < marks.size(); ++k)
			if (marks[k] <= i) p.pump = k;
		out.push_back(std::move(p));
	}
	return out;
}

constexpr size_t kNoPump = static_cast<size_t>(-1);

// The pump that sent the first record tagged `tag`, or kNoPump.
size_t pump_of(const std::vector<SentPacket> &packets, uint8_t tag) {
	for (const SentPacket &p : packets)
		for (const ProtocolMessage &m : p.records)
			if (m.tag == tag) return p.pump;
	return kNoPump;
}

// Whether the first datagram carrying either tag carries both.
bool same_packet(const std::vector<SentPacket> &packets, uint8_t a, uint8_t b) {
	for (const SentPacket &p : packets) {
		bool has_a = false;
		bool has_b = false;
		for (const ProtocolMessage &m : p.records) {
			has_a = has_a || m.tag == a;
			has_b = has_b || m.tag == b;
		}
		if (has_a || has_b) return has_a && has_b;
	}
	return false;
}

// A synced remote kept across a map change, its send clock dictated (holdoff H) and its
// countdown at `countdown` when the round's teardown starts. Retail's send build waits for a
// spent countdown that each receive pump steps once, and queues the reset's 0x25 with class 0,
// no bypass: the teardown's pump steps it to r = countdown - 1 (a countdown of 1 builds there
// and reloads the holdoff, r = H), the 0x25s are queued behind it, and the datagram leaves at
// the load's r-th pump (the head for r = 1, then the window's, ahead of the 0x7B), or with the
// 0x7B at the first tick's send pump once r passes the load's ten pumps (a dedicated host).
// Pump indices: 0 the teardown's, 1 the load's head, 2..10 the window, 11 the first tick's.
// An admitting remote's C2S 0x01 that reached the socket during the load is answered inside
// it: its S2C 0x02 probe leaves at the window pump that read it.
// [orig: CNapiNPConnection_PumpFlags @0x629780 (the 0x10 step @0x62979A..0x6297AC, the 0x200
//  reload @0x6297F3..0x629802); CNapiNPConnection_PumpEnumeratorAndSend @0x6290C0 (the build's
//  gate @0x62927B); NapiNPServer_SendToConn @0x4C4F20 (`push 0` @0x4C4F4F);
//  Game_TeardownMission @0x522350 (the pump @0x522588 ahead of the reset @0x52269B);
//  Server_DisconnectAndResetAllPlayerSlots @0x516160 (the 0x25 sends @0x5161BB..0x516385);
//  Game_StartMission @0x524360 (the head pump @0x5245E8, @0x524A63 .. @0x525AC5, the 0x7B
//  @0x526267); Server_TickUpdate @0x51D7E0 (the send pump @0x51E487);
//  NapiNPServerMsg_HandleJoinRequest @0x512ED0 (state 6 @0x512FB3, the probe's call @0x512FBC)]
struct ResidualCase {
	uint32_t holdoff = 0;
	uint32_t countdown = 0;
	size_t reset_pump = 0;      // the pump the 0x25 leaves at
	bool rides_with_7b = false; // the 0x25 and the 0x7B share the datagram
};

void run_reset_residual(const ResidualCase &rc) {
	auto host = std::make_unique<Host>();
	host->role.set_socket(&host->socket);
	host->role.set_rotation(&host->rotation);
	inmatch::seed_rotation_from_host_screen(host->rotation, host->catalog, {0, 1}, {0, 0});
	CHECK(host->boot_current(/*next_mission=*/false));
	host->ctx().mission_exit_reason = inmatch::kMissionExitMapCycle;
	inmatch::FrameInput input;
	input.delta_seconds = kFrame;
	bool ended = false;
	for (int f = 0; f < 16 && !ended; ++f) ended = host->session.advance(input).terminal();
	CHECK(ended);
	// The kept remote, in match on the old mission.
	const PeerAddr kept_peer{0x0200007Fu, 23457};
	{
		inmatch::NapiNPConnection &kept =
				add_session_conn(*host, kept_peer, inmatch::kFirstJoinerDcb, 0x11223344u);
		kept.admission_stage = inmatch::GameAdmissionStage::Complete;
		kept.phase = inmatch::ConnectionPhase::InMatch;
		kept.slot_state = inmatch::PlayerSlotState::Joined;
		kept.s2c_send_holdoff_dictated = true;
		kept.s2c_send_holdoff_ticks = rc.holdoff;
		kept.s2c_send_holdoff_countdown = rc.countdown;
		kept.s2c_send_boundary_open = false;
	}
	// The admitting remote, its join request answered: its form post comes during the load.
	const PeerAddr joiner_peer = host->socket.peer;
	{
		inmatch::NapiNPConnection &joiner =
				add_session_conn(*host, joiner_peer, inmatch::kFirstJoinerDcb + 1, 0x55667788u);
		joiner.phase = inmatch::ConnectionPhase::Joined; // its 0x42 accepted
		joiner.admission_stage = inmatch::GameAdmissionStage::AwaitFormPost;
	}
	std::vector<size_t> marks;
	host->socket.on_drain = [&host, &marks]() {
		marks.push_back(host->socket.sent.size());
		// The load's head: the form post reaches the socket, read by the window's first pump.
		if (marks.size() == 2) {
			ProtocolPacketHeader header;
			header.session_id = 0x55667788u;
			header.seq_num = 1;
			std::vector<uint8_t> body;
			(void)encode_protocol_packet_plaintext(header,
					{make_protocol_message(c2s::JOIN_FORM_POST, {0x00})}, kClientScrk, body);
			host->socket.inbound.push_back(
					nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(body)));
		}
	};
	CHECK(inmatch::begin_host_map_change(host->role, host->catalog) ==
			inmatch::MapChangeStep::NextMission);
	CHECK(host->boot_current(/*next_mission=*/true));
	CHECK(marks.size() == 11); // the teardown's, the load's head, the window's nine
	const size_t load_pumps = marks.size();
	// Ticks until the kept remote's 0x7B has left.
	for (int f = 0; f < 16; ++f) (void)host->session.advance(input);
	const std::vector<SentPacket> to_kept = packets_to(host->socket, kept_peer, marks);
	CHECK(pump_of(to_kept, s2c::GAME_RESET) == rc.reset_pump);
	if (rc.rides_with_7b) {
		CHECK(pump_of(to_kept, s2c::FULL_PLAYER_INFO) == rc.reset_pump);
		CHECK(same_packet(to_kept, s2c::GAME_RESET, s2c::FULL_PLAYER_INFO));
	} else {
		// Ahead of the 0x7B, which waits for the load's end.
		const size_t info_pump = pump_of(to_kept, s2c::FULL_PLAYER_INFO);
		CHECK(info_pump != kNoPump && info_pump >= load_pumps);
	}
	// The form post read at the window's first pump is answered there, inside the load.
	const std::vector<SentPacket> to_joiner = packets_to(host->socket, joiner_peer, marks);
	CHECK(pump_of(to_joiner, s2c::JOIN_PADDING_PROBE) == 2);
}

void test_map_change_reset_leaves_at_the_residual_pump() {
	// Holdoff 3: residuals 1..3 put the 0x25 at the head, @0x524A63 and @0x524F76. Residual 1
	// and 2 come from countdowns 2 and 3; residual 3 (the holdoff) from countdown 1, which the
	// teardown's pump spends, builds at and reloads (the 0x200 reload @0x6297F3..0x629802).
	for (uint32_t r = 1; r <= 2; ++r) run_reset_residual({3, r + 1, r, false});
	run_reset_residual({3, 1, 3, false});
	// Holdoff 12, residual 11: past the load's ten pumps, the first tick's send carries the
	// 0x25 with the 0x7B.
	run_reset_residual({12, 12, 11, true});
}

// The server-info stream across a map change: the teardown empties it ahead of its pump and
// the next mission start rebuilds it after the load's first pump, so a C2S 0x33 read at the
// teardown's pump or the load's head gets the header alone, [the old token][0][0], and one read
// in the window gets the rebuilt stream under the next token.
// [orig: Game_TeardownMission - CDataStream_Resize(&stru_C9480C, 0) @0x5223D1, the pump
//  @0x522588; Game_StartMission - the head pump @0x5245E8, the rebuild @0x5247EB, the token
//  @0x5247F3, the window @0x524A63; NapiNPServerMsg_HandleReplayRequest @0x515230 ->
//  NetPacket_WriteReplayDataBlock @0x5071F0 (@0x50727F, @0x5072A2)]
void test_map_change_server_info_stream_is_empty_until_the_rebuild() {
	auto host = std::make_unique<Host>();
	host->role.set_socket(&host->socket);
	host->role.set_rotation(&host->rotation);
	inmatch::seed_rotation_from_host_screen(host->rotation, host->catalog, {0, 1}, {0, 0});
	CHECK(host->boot_current(/*next_mission=*/false));
	host->ctx().mission_exit_reason = inmatch::kMissionExitMapCycle;
	inmatch::FrameInput input;
	input.delta_seconds = kFrame;
	bool ended = false;
	for (int f = 0; f < 16 && !ended; ++f) ended = host->session.advance(input).terminal();
	CHECK(ended);
	const PeerAddr peer = host->socket.peer;
	{
		inmatch::NapiNPConnection &c =
				add_session_conn(*host, peer, inmatch::kFirstJoinerDcb, 0x55667788u);
		c.admission_stage = inmatch::GameAdmissionStage::Complete;
		c.phase = inmatch::ConnectionPhase::InMatch;
		c.slot_state = inmatch::PlayerSlotState::Joined;
	}
	const uint32_t token = host->ctx().server_info_transfer_id;
	uint32_t next_seq = 1;
	auto request = [&host, &next_seq, token]() {
		ProtocolPacketHeader header;
		header.session_id = 0x55667788u;
		header.seq_num = next_seq++;
		std::vector<uint8_t> body;
		std::vector<uint8_t> payload(8, 0);
		for (int i = 0; i < 4; ++i) payload[i] = static_cast<uint8_t>(token >> (8 * i));
		(void)encode_protocol_packet_plaintext(header,
				{make_protocol_message(c2s::FILE_CHUNK_REQUEST, std::move(payload))}, kClientScrk,
				body);
		host->socket.inbound.push_back(
				nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(body)));
	};
	std::vector<size_t> marks;
	host->socket.on_drain = [&host, &marks, &request]() {
		marks.push_back(host->socket.sent.size());
		if (marks.size() == 1 || marks.size() == 2) request(); // read at the head, the window
	};
	request(); // read at the teardown's pump
	CHECK(inmatch::begin_host_map_change(host->role, host->catalog) ==
			inmatch::MapChangeStep::NextMission);
	CHECK(host->boot_current(/*next_mission=*/true));
	std::vector<std::pair<size_t, std::vector<uint8_t>>> chunks;
	for (const SentPacket &p : packets_to(host->socket, peer, marks))
		for (const ProtocolMessage &m : p.records)
			if (m.tag == s2c::FILE_TRANSFER_CHUNK) chunks.emplace_back(p.pump, m.payload);
	CHECK(chunks.size() == 3);
	if (chunks.size() == 3) {
		const std::vector<uint8_t> header_only = {
				static_cast<uint8_t>(token), static_cast<uint8_t>(token >> 8),
				static_cast<uint8_t>(token >> 16), static_cast<uint8_t>(token >> 24),
				0, 0, 0, 0, 0, 0, 0, 0};
		CHECK(chunks[0].first == 0 && chunks[0].second == header_only); // the teardown's
		CHECK(chunks[1].first == 1 && chunks[1].second == header_only); // the load's head
		// The window's: the rebuilt stream under the next token, from offset 0.
		CHECK(chunks[2].first == 2);
		CHECK(chunks[2].second.size() > 12);
		if (chunks[2].second.size() > 12) {
			CHECK(chunks[2].second[0] == static_cast<uint8_t>(token + 1));
			CHECK(chunks[2].second[4] != 0 || chunks[2].second[5] != 0);
			CHECK(chunks[2].second[8] == 0);
		}
	}
	CHECK(!host->ctx().server_info_stream_emptied);
}

} // namespace

int main() {
	test_handlers_drop_under_the_load_gate();
	test_fresh_load_pumps_its_session();
	test_map_change_load_pumps_after_the_slot_reset();
	test_map_change_reset_leaves_at_the_residual_pump();
	test_map_change_server_info_stream_is_empty_until_the_rebuild();
	if (failures != 0) {
		std::printf("host_load_pump: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("host_load_pump: ok\n");
	return 0;
}
