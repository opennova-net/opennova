// The map change's load order on the host: the next mission start's server pumps spread
// through the load as retail's are, so the round reset's S2C 0x25 leaves ahead of the terrain
// and a kept joiner's reload legs are answered while the map loads, ahead of the load end's
// S2C 0x7B.
//
// Retail's Game_StartMission (a first load, the authority's .bms arm) pumps its server
// protocol (Server_PumpNetworkTransport @0x4FD960) at its head (@0x5245E8), after the stream
// rebuild, the per-slot reset and the chat clear (@0x524A63), around the terrain (@0x524F76,
// Game_LoadTerrainDuringConnect @0x524F7C, @0x524F8C), after the mission's document
// (Mission_LoadBMSFile @0x524FFA, @0x525256), after the avatars and the environment clock
// (@0x5253EA), then past the def loads and the objects (Entity_InitAllFromModels @0x52567F) five
// times (@0x52589D, @0x5258E5, @0x5259EC an own client's only, @0x525A5C, @0x525AC5); the load
// end's 0x7B (Server_BroadcastPlayerInfoToAll @0x526267) waits for the first tick's send pump.
// A joiner reloading in place answers the 0x25 with C2S 0x48 / 0x47 / 0x33, which a later
// in-load pump reads, so their S2C 0x75 and 0x60 queue ahead of the 0x7B.
//
// The oracle is the sanitized x7 round capture's retail-host order (round RR / RO): the 0x25
// leaves at an in-load pump (there with the linger's last 0x79, which this host never queued),
// the joiner's three legs come back while the host still loads, and the first packet after the
// load carries [0x75, 0x60, 0x7B].
//
// Pinned here, core-only (a synthetic mount, a recording socket, no retail data):
//   - a dedicated host's map change: the 0x25 leaves at the window's first pump, ahead of the
//     terrain; the reload legs read during the terrain are answered at the next stage pump,
//     before the next World stands; the first packet past the load is [0x75, 0x60, 0x7B]; the
//     load pumps nine times past its head's first (the render arm's @0x5259EC skipped);
//   - a playing listen host's: its head's pumps ahead of the terrain too, and ten window pumps;
//   - with the embedder's device stages deferred (the game's effect world and warm, which
//     stand for retail's render inits): the render arm's pumps (two dedicated, three listen)
//     run at those stages and at phase B's head, so a reload leg that lands while they run is
//     answered ahead of the 0x7B; the PreMission pass and the round init wait for the arm's
//     last pump (Game_StartMission's EventTrigger_UpdateAllWithFlag2 @0x525B86 and
//     Server_InitAllPlayerEntitiesForRound @0x525BAF past @0x525AC5), so a leg the arm's
//     pumps read meets the slots before their round init.
#include <base/gameprofile/game_type.h>
#include <base/gameprofile/gameprofile.h>
#include <formats/def/def.h>
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
#include <runtime/inmatch/server_admin_command.h>
#include <runtime/inmatch/server_console.h>
#include <runtime/inmatch/session.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/mission/mission_sidecars.h>
#include <runtime/world/player_spawn.h> // kPlayerInfantryTypeId
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
constexpr size_t kNoPump = static_cast<size_t>(-1);

// The index of a boot step in a kernel's boot_trace, or -1.
int step_index(const std::vector<std::string> &trace, const char *step) {
	for (size_t i = 0; i < trace.size(); ++i)
		if (trace[i] == step) return static_cast<int>(i);
	return -1;
}

const char kServerScrk[] = "SERVERLOADORDERSCRK23456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789A";
const char kClientScrk[] = "CLIENTLOADORDERSCRK23456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789A";

// A socket that records each receive drain: the drain loop calls recv_from until it returns 0,
// so each pump with nothing left to read makes one empty call. A queued datagram is handed
// over first, from `peer`.
struct RecordingSocket final : opennova::IDatagramSocket {
	std::deque<std::vector<uint8_t>> inbound;
	PeerAddr peer{0x0100007Fu, 23456};
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
		if (on_drain) on_drain();
		return 0;
	}
	void send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) override {
		sent.emplace_back(data, data + len);
		sent_to.push_back(to);
	}
};

mission_catalog::Row catalog_row(const char *file, bms::AttribFlags mode) {
	mission_catalog::Row r;
	r.file = file;
	r.title = file;
	r.game_mode = static_cast<uint32_t>(mode);
	return r;
}

std::vector<uint8_t> le32(uint32_t v) {
	return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
			static_cast<uint8_t>(v >> 24)};
}

// The S2C records of each session datagram sent to `peer`, with the index of the pump that
// sent it: drain k's mark is the count sent before pump k's send, so a datagram at index i
// left in the last pump whose mark is <= i.
struct SentPacket {
	size_t pump = 0;
	std::vector<uint8_t> tags;
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
		std::vector<ProtocolMessage> records;
		if (!decode_protocol_packet_plaintext(body.data(), body.size(), kServerScrk, header,
					records))
			continue;
		SentPacket p;
		for (size_t k = 0; k < marks.size(); ++k)
			if (marks[k] <= i) p.pump = k;
		for (const ProtocolMessage &m : records) p.tags.push_back(m.tag);
		out.push_back(std::move(p));
	}
	return out;
}

// --------------------------------------------------------------------------
// A dedicated host and a kept joiner.
// --------------------------------------------------------------------------

struct DedicatedHost {
	std::map<std::string, std::string> files;
	std::vector<mission_catalog::Row> catalog{
			catalog_row("MAPA.BMS", bms::AttribFlags::TeamDeathmatch),
			catalog_row("MAPB.BMS", bms::AttribFlags::TeamDeathmatch)};
	inmatch::HostRotation rotation;
	inmatch::HostRole role{inmatch::RoleKind::DedicatedHost};
	inmatch::Session session{role};
	inmatch::HostBoot boot;
	std::unique_ptr<mission::MissionKernel> kernel;
	RecordingSocket socket;
	std::function<void()> at_terrain;
	// The embedder's device stages between the phases (HostBootRequest::render_arm_deferred),
	// in the game's order: phase A, the stages, phase B, the load end.
	bool render_arm_deferred = false;
	std::function<void()> device_stages;

	inmatch::NapiNPServerCtx &ctx() { return role.state.host_owner.ctx; }

	// The rotation's current map's session config.
	inmatch::HostConfig current_host_cfg() const {
		inmatch::HostConfig cfg;
		cfg.protocol = &kJo;
		cfg.config.server_name = "Load Order";
		cfg.config.game_type = rotation.list.map_game_type;
		cfg.config.mission_file = rotation.list.map_file;
		cfg.config.max_players = 9;
		cfg.config.num_teams = 2;
		cfg.socket_mode = inmatch::SocketMode::Lan;
		cfg.serve_and_play = false;
		return cfg;
	}

	bool boot_current(bool next_mission) {
		const std::string map_file = rotation.list.map_file;
		inmatch::HostBootRequest r;
		r.mission = test_mission::two_entity_mission();
		r.mission.header.attrib_flags = bms::AttribFlags::TeamDeathmatch;
		std::snprintf(r.mission.header.mission_name, sizeof(r.mission.header.mission_name), "%s",
				map_file == "MAPA.BMS" ? "Map A" : "Map B");
		mission::sync_counts(r.mission);
		r.mission_basename = mission::mission_base_name(map_file);
		r.files = test_boot::source_over(&files);
		r.session = &session;
		r.role = &role;
		r.host = &role;
		r.host_cfg = current_host_cfg();
		r.host_cfg.config.mission_name = r.mission.get_mission_name();
		r.next_mission = next_mission;
		r.render_arm_deferred = render_arm_deferred;
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
		// The terrain's step: the embedder's terrain seam, where the mission's terrain loads.
		r.before_terrain = [this](mission::MissionKernel &) {
			if (at_terrain) at_terrain();
		};
		std::string error;
		if (!inmatch::boot_host_mission(std::move(r), boot, error)) {
			std::printf("boot: %s\n", error.c_str());
			return false;
		}
		if (device_stages) device_stages();
		if (!inmatch::start_host_mission(boot, inmatch::HostStartDevice{}, error)) {
			std::printf("boot: %s\n", error.c_str());
			return false;
		}
		inmatch::end_host_mission_load(boot);
		return true;
	}
};

// A remote in the match on the old mission, its send clock dictated (holdoff 12) and its
// countdown at `countdown` when the round's teardown starts.
void add_kept_joiner(DedicatedHost &host, uint32_t countdown) {
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
	c.s2c_send_holdoff_countdown = countdown;
	c.s2c_send_boundary_open = false;
	host.ctx().np_protocol.connection_list.push_back(std::move(c));
}

// The joiner's answer to the 0x25, one packet: its client ack (its own dcb), the 0x47 and the
// server-info request at offset 0 under the host's current token.
std::vector<uint8_t> reload_legs(uint32_t token) {
	ProtocolPacketHeader header;
	header.session_id = 0x55667788u;
	header.seq_num = 1;
	std::vector<uint8_t> chunk = le32(token);
	const std::vector<uint8_t> offset = le32(0);
	chunk.insert(chunk.end(), offset.begin(), offset.end());
	std::vector<uint8_t> body;
	(void)encode_protocol_packet_plaintext(header,
			{make_protocol_message(c2s::CLIENT_ACK, le32(inmatch::kFirstJoinerDcb)),
					make_protocol_message(c2s::PING, {}),
					make_protocol_message(c2s::FILE_CHUNK_REQUEST, std::move(chunk))},
			kClientScrk, body);
	return nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(body));
}

// Pump indices from the map change on: 0 the teardown's, 1 the load's head @0x5245E8, 2 and 3
// the window's first two (@0x524A63, @0x524F76), 4..6 the stages' (@0x524F8C, @0x525256,
// @0x5253EA), 7..10 the bring-up's (@0x52589D, @0x5258E5, @0x525A5C, @0x525AC5), 11 on the
// ticks'. The kept joiner's countdown of 3 sends the 0x25 at pump 2 and reloads the holdoff;
// its reload legs reach the socket while the terrain loads and are read at pump 4.
void test_dedicated_map_change_spreads_its_pumps_through_the_load() {
	auto host = std::make_unique<DedicatedHost>();
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
	add_kept_joiner(*host, /*countdown=*/3);

	std::vector<size_t> marks;
	size_t pumps_at_terrain = kNoPump;
	size_t read_at = kNoPump;
	bool world_at_read = true;
	bool legs_queued = false;
	host->socket.on_drain = [&]() {
		marks.push_back(host->socket.sent.size());
		// The drain that read the reload legs: the first after they reached the socket.
		if (legs_queued && read_at == kNoPump) {
			read_at = marks.size() - 1;
			world_at_read = host->ctx().world != nullptr;
		}
	};
	host->at_terrain = [&]() {
		pumps_at_terrain = marks.size();
		host->socket.inbound.push_back(reload_legs(host->ctx().server_info_transfer_id));
		legs_queued = true;
	};
	CHECK(inmatch::begin_host_map_change(host->role, host->catalog) ==
			inmatch::MapChangeStep::NextMission);
	CHECK(host->boot_current(/*next_mission=*/true));
	const size_t load_pumps = marks.size();
	// The teardown's, the load's head, the window's nine (a dedicated host's).
	CHECK(load_pumps == 11);
	// The head's pumps ran ahead of the terrain: the teardown's, the head's and two more.
	CHECK(pumps_at_terrain == 4);
	// The legs were read at the stage pump after the terrain, before the next World stood.
	CHECK(read_at == 4);
	CHECK(!world_at_read);
	// Ticks until the kept joiner's boundary has opened past the load.
	for (int f = 0; f < 16; ++f) (void)host->session.advance(input);
	const std::vector<SentPacket> to_kept = packets_to(host->socket, host->socket.peer, marks);
	CHECK(to_kept.size() >= 2);
	if (to_kept.size() >= 2) {
		// The 0x25, all its queue held, at the window's first pump.
		CHECK(to_kept[0].pump == 2);
		CHECK(to_kept[0].tags == std::vector<uint8_t>{s2c::GAME_RESET});
		// The first packet past the load: the legs' replies, then the load end's 0x7B.
		CHECK(to_kept[1].pump >= load_pumps);
		CHECK((to_kept[1].tags == std::vector<uint8_t>{s2c::SPECTATOR_FLAGS,
				s2c::FILE_TRANSFER_CHUNK, s2c::FULL_PLAYER_INFO}));
		if (to_kept[1].tags.size() != 3) {
			std::printf("first packet past the load:");
			for (const uint8_t t : to_kept[1].tags) std::printf(" %02X", t);
			std::printf("\n");
		}
	}
	CHECK(host->ctx().net_spawn_suspended == 0);
	CHECK(host->session.state() == inmatch::State::Running);
}

// --------------------------------------------------------------------------
// A playing listen host.
// --------------------------------------------------------------------------

struct ListenHost {
	std::map<std::string, std::string> files;
	std::vector<mission_catalog::Row> catalog{
			catalog_row("ADMAP.BMS", bms::AttribFlags::AttackAndDefend)};
	inmatch::HostRotation rotation;
	inmatch::HostRole role{inmatch::RoleKind::ListenHost};
	inmatch::Session session{role};
	inmatch::HostBoot boot;
	std::unique_ptr<mission::MissionKernel> kernel;
	std::vector<def::DefItemDef> rows;
	def::DefItemsFile items{};
	RecordingSocket socket;
	std::function<void()> at_terrain;
	bool render_arm_deferred = false;
	std::function<void()> device_stages;

	ListenHost() {
		role.set_socket(&socket);
		files["SndProf.def"] =
				"begin \"default\"\r\nend\r\n"
				"begin \"SP_Host\"\r\n     SSLFootGND     T_DIRT_L\r\nend\r\n";
		rows.resize(2);
		rows[0].id = static_cast<int>(world::kPlayerInfantryTypeId) +
				static_cast<int>(mission::kItemIdOffset);
		rows[0].hp = 150;
		std::snprintf(rows[0].sound_profile, sizeof(rows[0].sound_profile), "SP_Host");
		rows[1].id = 164 + static_cast<int>(mission::kItemIdOffset);
		rows[1].hp = 100;
		rows[1].attrib = 0x8000u;
		items.entries = rows.data();
		items.count = rows.size();
		role.set_rotation(&rotation);
		// Its Switch cell set: the first half's end replays it.
		inmatch::seed_rotation_from_host_screen(rotation, catalog, {0}, {1});
	}

	bool boot_map(bool next_mission) {
		inmatch::HostBootRequest r;
		r.host_cfg.protocol = &kJo;
		r.mission = test_mission::two_entity_mission();
		r.mission.header.attrib_flags = bms::AttribFlags::AttackAndDefend;
		mission::sync_counts(r.mission);
		r.mission_basename = "ADMAP";
		r.files = test_boot::source_over(&files);
		r.items = &items;
		r.session = &session;
		r.role = &role;
		r.host = &role;
		r.host_cfg.config.server_name = "Load Order Listen";
		r.host_cfg.config.game_type = game_type::kAttackDefend;
		r.host_cfg.config.mission_file = "ADMAP.BMS";
		r.host_cfg.config.max_players = 9;
		r.host_cfg.config.num_teams = 2;
		r.host_cfg.socket_mode = inmatch::SocketMode::Lan;
		r.host_cfg.serve_and_play = true;
		r.next_mission = next_mission;
		r.render_arm_deferred = render_arm_deferred;
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
		r.before_terrain = [this](mission::MissionKernel &) {
			if (at_terrain) at_terrain();
		};
		std::string error;
		if (!inmatch::boot_host_mission(std::move(r), boot, error)) {
			std::printf("listen boot: %s\n", error.c_str());
			return false;
		}
		if (device_stages) device_stages();
		if (!inmatch::start_host_mission(boot, inmatch::HostStartDevice{}, error)) {
			std::printf("listen boot: %s\n", error.c_str());
			return false;
		}
		inmatch::end_host_mission_load(boot);
		return true;
	}
};

// A listen host's window keeps its render arm's pump (@0x5259EC): ten past the head's first,
// two of them ahead of the terrain.
void test_listen_map_change_pumps_ten_through_the_load() {
	auto host = std::make_unique<ListenHost>();
	CHECK(host->boot_map(/*next_mission=*/false));
	if (host->kernel == nullptr) return;
	inmatch::NapiNPServerCtx &ctx = host->role.state.host_owner.ctx;
	CHECK(inmatch::Server_ExecuteServerCommand(ctx, &host->kernel->world, "Cycle", "", {}).handled);
	bool ended = false;
	for (int f = 0; f < 4000 && !ended; ++f) {
		inmatch::FrameInput input;
		input.delta_seconds = kFrame;
		ended = host->session.advance(input).terminal();
		host->kernel->world.out.discard_presentation();
	}
	CHECK(ended);
	size_t drains = 0;
	size_t drains_at_terrain = kNoPump;
	host->socket.on_drain = [&drains]() { ++drains; };
	host->at_terrain = [&drains, &drains_at_terrain]() { drains_at_terrain = drains; };
	CHECK(inmatch::begin_host_map_change(host->role, host->catalog) ==
			inmatch::MapChangeStep::NextMission);
	CHECK(host->boot_map(/*next_mission=*/true));
	// The teardown's, the load's head, the window's ten.
	CHECK(drains == 12);
	CHECK(drains_at_terrain == 4);
}

// --------------------------------------------------------------------------
// The render arm at the embedder's device stages (the game's effect world and warm).
// --------------------------------------------------------------------------

// Retail's last load pumps run between its render device's mission inits, the 0x7B after
// them: Render_InitRenderingSubsystems @0x5259D1, @0x5259EC (an own client's only),
// Render_InitTerrainSubsystems @0x5259FD, @0x525A5C, Render_InitMissionTextures @0x525A6E,
// HUD_InitAllFonts @0x525A9A, @0x525AC5, then Server_BroadcastPlayerInfoToAll @0x526267. A
// dedicated host's arm is the two after its skipped inits (the test @0x5259B4). With the
// embedder's stages deferred, phase A leaves those two, and the PreMission pass and the round
// init wait for the arm's last pump, which phase B runs first: a kept joiner's reload legs
// that reach the socket while the stages run (between the phases) are read at the arm's first
// pump, over the new World and ahead of the slots' round init, and their replies still lead
// the 0x7B [orig: Game_StartMission - @0x525AC5, then EventTrigger_UpdateAllWithFlag2
// @0x525B86 and Server_InitAllPlayerEntitiesForRound @0x525BAF].
void test_dedicated_render_arm_answers_a_leg_during_the_device_stages() {
	auto host = std::make_unique<DedicatedHost>();
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
	add_kept_joiner(*host, /*countdown=*/3);

	std::vector<size_t> marks;
	size_t read_at = kNoPump;
	bool world_at_read = false;
	int round_init_at_read = 0;
	int premission_at_last_pump = 0;
	bool legs_queued = false;
	host->socket.on_drain = [&]() {
		marks.push_back(host->socket.sent.size());
		if (legs_queued && read_at == kNoPump) {
			read_at = marks.size() - 1;
			world_at_read = host->ctx().world != nullptr;
			round_init_at_read = step_index(host->kernel->boot_trace, "round_init");
		}
		// The arm's last pump (@0x525AC5), phase B's first leg.
		if (legs_queued && marks.size() - 1 == 10)
			premission_at_last_pump = step_index(host->kernel->boot_trace, "premission");
	};
	size_t pumps_at_stages = kNoPump;
	int owed_at_stages = -1;
	int owed_past_effects = -1;
	uint8_t gate_at_stages = 0;
	int premission_at_stages = 0;
	host->render_arm_deferred = true;
	host->device_stages = [&]() {
		pumps_at_stages = marks.size();
		owed_at_stages = host->role.render_arm_pumps_owed();
		gate_at_stages = host->ctx().net_spawn_suspended;
		premission_at_stages = step_index(host->kernel->boot_trace, "premission");
		host->socket.inbound.push_back(reload_legs(host->ctx().server_info_transfer_id));
		legs_queued = true;
		host->role.pump_render_arm_ahead_of_textures();
		owed_past_effects = host->role.render_arm_pumps_owed();
	};
	CHECK(inmatch::begin_host_map_change(host->role, host->catalog) ==
			inmatch::MapChangeStep::NextMission);
	CHECK(host->boot_current(/*next_mission=*/true));
	const size_t load_pumps = marks.size();
	// Phase A ran the teardown's, the head's and seven of the window's nine; the stages
	// ran the arm's first (@0x525A5C) and phase B its last (@0x525AC5).
	CHECK(pumps_at_stages == 9);
	CHECK(owed_at_stages == 2);
	CHECK(owed_past_effects == 1);
	CHECK(load_pumps == 11);
	CHECK(host->role.render_arm_pumps_owed() == 0);
	// The spawn gate held through the stages, until the load end.
	CHECK(gate_at_stages == 1);
	CHECK(host->ctx().net_spawn_suspended == 0);
	CHECK(host->session.state() == inmatch::State::Running);
	// The legs were read at the arm's first pump, the next World standing, its slots not yet
	// in their round init.
	CHECK(read_at == 9);
	CHECK(world_at_read);
	CHECK(round_init_at_read == -1);
	// The PreMission pass waits for the arm's last pump, then the round init follows it.
	CHECK(premission_at_stages == -1);
	CHECK(premission_at_last_pump == -1);
	if (host->kernel != nullptr) {
		const int premission = step_index(host->kernel->boot_trace, "premission");
		const int round_init = step_index(host->kernel->boot_trace, "round_init");
		CHECK(premission >= 0);
		CHECK(round_init > premission);
	}
	for (int f = 0; f < 16; ++f) (void)host->session.advance(input);
	const std::vector<SentPacket> to_kept = packets_to(host->socket, host->socket.peer, marks);
	CHECK(to_kept.size() >= 2);
	if (to_kept.size() >= 2) {
		CHECK(to_kept[0].pump == 2);
		CHECK(to_kept[0].tags == std::vector<uint8_t>{s2c::GAME_RESET});
		// The legs' replies ahead of the load end's 0x7B.
		CHECK(to_kept[1].pump >= load_pumps);
		CHECK((to_kept[1].tags == std::vector<uint8_t>{s2c::SPECTATOR_FLAGS,
				s2c::FILE_TRANSFER_CHUNK, s2c::FULL_PLAYER_INFO}));
		if (to_kept[1].tags.size() != 3) {
			std::printf("first packet past the deferred load:");
			for (const uint8_t t : to_kept[1].tags) std::printf(" %02X", t);
			std::printf("\n");
		}
	}
}

// A listen host's arm is three: two ahead of the textures (@0x5259EC, @0x525A5C), one after
// (@0x525AC5); the window keeps its ten.
void test_listen_render_arm_pumps_three_around_the_textures() {
	auto host = std::make_unique<ListenHost>();
	CHECK(host->boot_map(/*next_mission=*/false));
	if (host->kernel == nullptr) return;
	inmatch::NapiNPServerCtx &ctx = host->role.state.host_owner.ctx;
	CHECK(inmatch::Server_ExecuteServerCommand(ctx, &host->kernel->world, "Cycle", "", {}).handled);
	bool ended = false;
	for (int f = 0; f < 4000 && !ended; ++f) {
		inmatch::FrameInput input;
		input.delta_seconds = kFrame;
		ended = host->session.advance(input).terminal();
		host->kernel->world.out.discard_presentation();
	}
	CHECK(ended);
	size_t drains = 0;
	size_t drains_at_stages = kNoPump;
	size_t drains_past_effects = kNoPump;
	int owed_at_stages = -1;
	int premission_at_stages = 0;
	int round_init_at_stages = 0;
	host->socket.on_drain = [&drains]() { ++drains; };
	host->render_arm_deferred = true;
	host->device_stages = [&]() {
		drains_at_stages = drains;
		owed_at_stages = host->role.render_arm_pumps_owed();
		premission_at_stages = step_index(host->kernel->boot_trace, "premission");
		round_init_at_stages = step_index(host->kernel->boot_trace, "round_init");
		host->role.pump_render_arm_ahead_of_textures();
		drains_past_effects = drains;
	};
	CHECK(inmatch::begin_host_map_change(host->role, host->catalog) ==
			inmatch::MapChangeStep::NextMission);
	CHECK(host->boot_map(/*next_mission=*/true));
	CHECK(drains_at_stages == 9);
	CHECK(owed_at_stages == 3);
	CHECK(drains_past_effects == 11);
	CHECK(drains == 12);
	CHECK(ctx.net_spawn_suspended == 0);
	// The mission start's head waited for the stages: the PreMission pass, then the round
	// init over the kept slots, the own host's among them.
	CHECK(premission_at_stages == -1);
	CHECK(round_init_at_stages == -1);
	if (host->kernel != nullptr) {
		const int premission = step_index(host->kernel->boot_trace, "premission");
		const int round_init = step_index(host->kernel->boot_trace, "round_init");
		CHECK(premission >= 0);
		CHECK(round_init > premission);
	}
}

// The map change's head as the game's world load runs it, ahead of phase A: the head through
// the per-slot reset pumps nothing and clears no console line, so the embedder's effect system
// loads between the reset and the window, as retail's does; the window then clears the console
// and pumps twice, ahead of the terrain; with the embedder's three stage pumps the load pumps as
// a headless host's does (eleven, the teardown's and the router's included).
// [orig: Game_StartMission @0x524360 - Server_PumpNetworkTransport @0x5245E8,
//  Server_ResetAllPlayerSlotFrameState @0x5248BC, CEffectSystem_Init @0x524980,
//  Chat_ClearAllChannels @0x524A10, Server_PumpNetworkTransport @0x524A63 and @0x524F76]
void test_the_embedder_head_leaves_its_window_to_the_effect_system() {
	auto host = std::make_unique<DedicatedHost>();
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
	add_kept_joiner(*host, /*countdown=*/3);
	size_t drains = 0;
	host->socket.on_drain = [&drains]() { ++drains; };
	CHECK(inmatch::begin_host_map_change(host->role, host->catalog) ==
			inmatch::MapChangeStep::NextMission);
	// The teardown's pump and the router's (@0x5245E8).
	CHECK(drains == 2);
	inmatch::server_console_add(host->ctx(), "a line of the last map", 0xFFFFFFFFu);
	inmatch::begin_next_host_mission_load(host->role, test_boot::source_over(&host->files),
			mission::mission_base_name(host->rotation.list.map_file), {},
			host->current_host_cfg(), /*session_score_ini=*/false);
	CHECK(host->role.mission_load_begun());
	CHECK(drains == 2);
	CHECK(!host->ctx().console_chat.empty());
	host->role.open_mission_load_window();
	CHECK(drains == 4);
	CHECK(host->ctx().console_chat.empty());
	for (int stage = 0; stage < 3; ++stage) host->role.pump_load_stage();
	CHECK(host->boot_current(/*next_mission=*/true));
	CHECK(drains == 11);
	CHECK(host->ctx().net_spawn_suspended == 0);
	CHECK(host->session.state() == inmatch::State::Running);
}

} // namespace

int main() {
	test_dedicated_map_change_spreads_its_pumps_through_the_load();
	test_the_embedder_head_leaves_its_window_to_the_effect_system();
	test_listen_map_change_pumps_ten_through_the_load();
	test_dedicated_render_arm_answers_a_leg_during_the_device_stages();
	test_listen_render_arm_pumps_three_around_the_textures();
	if (failures != 0) {
		std::printf("map_change_load_order: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("map_change_load_order: ok\n");
	return 0;
}
