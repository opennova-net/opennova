// inmatch::HostRole over the mission kernel (ADR 0043 d3), ungated: the
// synthetic mission mission_kernel_test boots, driven through the net half
// every host embedder shares -- the SP listen bring-up (the in-process
// loopback carrying the host's own dcb-2 client, the auto-spawned local
// player, the HostClient replica fold), N listen frames over a null socket
// (the logic clock advances once per frame, the 0x0A fan folds into the local
// ClientState, the viewport seam reaches the ctx), the local C2S gameplay
// drain (reload, mounted-slot, medic and squad requests reach the server
// dispatcher while movement stays queued for Server_TickUpdate; the own deploy pick
// reads the session's protocol), and the dedicated bring-up (no loopback client, no
// local player, no local fold).
#include <runtime/inmatch/host_role.h>
#include <net/protocol/protocol.h>
#include <runtime/inmatch/local_role.h>
#include <runtime/controls/controls.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/world/angle.h>
#include <runtime/world/local_player_view.h>
#include <runtime/world/player_loadout.h> // local_loadout_rebuild
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/role_feeds.h>
#include <runtime/inmatch/novaworld_link.h> // kNwuSessionRoleHosting
#include <base/gameprofile/game_type.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/squad_messages.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/mission/mission_text.h>
#include <formats/def/reserved_items.h>
#include <formats/playersav/player_sav.h>

#include <runtime/inmatch/null_datagram_socket.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/inmatch/mission_rotation.h> // HostRotation (the LastGame row)
#include <runtime/inmatch/admin_console.h> // AdminConsole (a Serve Only host's typed line)
#include <formats/gamecfg/game_cfg.h> // gamecfg::defaults (the console's cfg block)
#include <base/io/tick_rate.h> // kTicksPerSecondInt

#include "../common/boot_file_source.h"
#include "../common/synthetic_mission.h"

#include <cstdio>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// JO's title identity (net/protocol/protocol.h), the identity these tests speak.
static const opennova::Protocol &kJo = *opennova::title_protocol("jo");

using namespace opennova;
namespace ms = opennova::mission;
namespace w = opennova::world;

static int failures = 0;
#define CHECK(c)                                                                            \
	do {                                                                                    \
		if (!(c)) {                                                                         \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                        \
			++failures;                                                                     \
		}                                                                                   \
	} while (0)

namespace {

using test_mission::two_entity_mission;
using test_boot::source_over;

uint32_t mission_game_type(const bms::File &mission) {
	return game_type::for_mission_mode(bms::selected_game_mode(
			static_cast<bms::AttribFlags>(mission.header.attrib_flags)));
}

int local_loopback_connections(const inmatch::ListenHostState &state) {
	int count = 0;
	for (const inmatch::NapiNPConnection &conn : state.host_owner.ctx.np_protocol.connection_list)
		if (conn.type == 2 && conn.link.transport == &state.host_loop) ++count;
	return count;
}

// One tick's input with the renderer viewport height a listen host wraps its
// S2C 0x68 cursor against.
inmatch::TickInput tick_input(uint32_t loaded_model_list_count) {
	inmatch::TickInput in;
	in.loaded_model_list_count = loaded_model_list_count;
	return in;
}

// The shell's single player takes the out-of-session rules word (D-NET-405).
// The game shell's SP comes up through the staged record (the SP config,
// mpattrib 0x3A06, socketless): out of a session the mission-data block's
// rules word is the literal 0x4204, never the config word; a LAN host with
// the same config is in a session and takes the config word. The derived
// bits follow the word: NoFriendlyFire 0x200 set, AutoScopeZero 0x10000
// clear, in both.
// [orig: CNapiGameSession_InitRandomSeedOrRequest @0x51E944..0x51E957]
// Its own frame: main's is near the stack limit with every block's locals.
void check_shell_single_player_rules_word(std::map<std::string, std::string> &files) {
	for (const inmatch::SocketMode mode :
			{inmatch::SocketMode::Socketless, inmatch::SocketMode::Lan}) {
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		auto &kernel = *kernel_storage;
		auto role_storage = std::make_unique<inmatch::HostRole>();
		inmatch::HostRole &role = *role_storage;
		role.bind(kernel);
		inmatch::ListenHostState &host = role.state;
		kernel.open_document(two_entity_mission(), "synth", source_over(&files));
		ms::KernelBootOptions options;
		options.game_type = mission_game_type(kernel.mission);
		options.mp_session = mode != inmatch::SocketMode::Socketless;
		options.bringup_net_session = [&] {
			inmatch::HostConfig cfg;
			cfg.protocol = &kJo;
			cfg.config = inmatch::singleplayer_game_config(*opennova::title_by_code("jo"), options.game_type, nullptr);
			cfg.config.allow_sniper_scope_zoom = true;
			cfg.socket_mode = mode;
			cfg.serve_and_play = true;
			inmatch::HostBringup bringup;
			bringup.host_cfg = cfg;
			role.bring_up(bringup);
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(error.empty());
		CHECK(host.host_owner.ctx.config.mp_attributes == 0x3A06u);
		if (mode == inmatch::SocketMode::Socketless) {
			CHECK(kernel.world.rules.mpattrib == 0x4204u);
			CHECK(!kernel.world.rules.mp_session);
		} else {
			CHECK(kernel.world.rules.mpattrib == 0x3A06u);
			CHECK(kernel.world.rules.mp_session);
		}
		CHECK(kernel.world.rules.no_friendly_fire);
		CHECK(!kernel.world.rules.auto_scope_zero);
		// The sniper-zoom rule is the config's in a session alone [orig:
		//  Game_ApplySessionSettingsToGlobals @0x552284 / @0x5522B9].
		CHECK(kernel.world.rules.allow_sniper_scope_zoom ==
				(mode != inmatch::SocketMode::Socketless));
		// The fill reads the config's own value out of a session, the rule in one
		// [orig: WeaponSlot_InitFromDef @0x53EF07..0x53EF25].
		CHECK(kernel.world.rules.sniper_zoom_fill);
	}
}

// The listen host's own 0x0F rewrites its location names (D-NET-657).
// The spawn copies each name whole into its 64-byte slot of the one
// g_LocationNames, so a 70-byte name reads on into the next name; the
// host's own client's 0x0F copies every name back into the same slots,
// cut to 63 bytes, so from then on the host's own list (a later joiner's
// 0x0F, the chat's labels) reads the cut names.
// [orig: Entity_SpawnFromBMSRecord @0x40F1D3..0x40F1DF; NapiNPClientMsg_0x00F
//  @0x42e51b..0x42e5ad, ahead of the ack burst's is_authority test @0x42e5b7]
// Its own frame, the kernel and role on the heap: main is near the stack limit.
void check_own_world_state_location_names(std::map<std::string, std::string> &files) {
	auto kernel_storage = std::make_unique<ms::MissionKernel>();
	ms::MissionKernel &kernel = *kernel_storage;
	auto role_storage = std::make_unique<inmatch::HostRole>();
	inmatch::HostRole &role = *role_storage;
	role.bind(kernel);
	inmatch::ListenHostState &host = role.state;
	bms::File mission = two_entity_mission();
	for (const int32_t name_index : {0, 1}) {
		bms::Entity marker{};
		marker.type = bms::ItemType::Marker;
		marker.type_id = def::DEF_TYPE_NAMED_LOCATION;
		marker.name_index = name_index;
		marker.x = (40 + name_index) << 16;
		marker.y = 20 << 16;
		mission.markers.push_back(marker);
	}
	kernel.open_document(mission, "location_names", source_over(&files));
	const std::string long_name = "North " + std::string(64, 'x'); // 70 bytes
	ms::MissionText text;
	text.loaded = true;
	text.table.sections.push_back({"Locations", 2});
	text.table.entries.push_back({"LOCATION000", long_name, {}, 0});
	text.table.entries.push_back({"LOCATION001", "Ridge", {}, 0});
	ms::KernelBootOptions options;
	options.mp_session = true;
	options.game_type = mission_game_type(kernel.mission);
	options.bringup_net_session = [&] {
		inmatch::HostConfig cfg;
		cfg.protocol = &kJo;
		cfg.config.server_name = "location_names";
		cfg.config.max_players = 4;
		cfg.config.game_type = options.game_type;
		cfg.socket_mode = inmatch::SocketMode::Lan;
		cfg.serve_and_play = true;
		inmatch::HostBringup bringup;
		bringup.host_cfg = cfg;
		bringup.mission_text = text;
		role.bring_up(bringup);
	};
	std::string error;
	CHECK(kernel.boot(options, error));
	opennova::inmatch::NullDatagramSocket socket;
	role.set_socket(&socket);
	const std::vector<std::string> spawned = {long_name.substr(0, 64) + "Ridge", "Ridge"};
	CHECK(host.host_owner.ctx.mission_location_names == spawned);
	CHECK(host.client_runtime != nullptr);
	if (host.client_runtime != nullptr) {
		int landed_at = -1;
		for (int frame = 1; frame <= 200 && landed_at < 0; ++frame) {
			role.run_tick(tick_input(0));
			if (host.client_runtime->state().world_state.revision != 0) landed_at = frame;
		}
		std::printf("host_role: the host's own 0x0F landed at frame %d\n", landed_at);
		const std::vector<std::string> cut = {long_name.substr(0, 63), "Ridge"};
		CHECK(landed_at > 0);
		CHECK(host.client_runtime->state().location_names == cut);
		CHECK(host.host_owner.ctx.mission_location_names == cut);
	}
}

} // namespace

// The permanent-death and spectator bits reach the host's own client in
// its looped-back S2C 0x08. A dead body's deploy press then queues the
// reliable 0x51 [i16 0xFFFF] when spectators are allowed, whose next-frame
// drain converts the host's own slot through the server's handler, and
// queues nothing when they are not; a live body's press is the 0x0E pick.
// [orig: Input_HandleActionBinding -- a6 @0x49AD76..0x49AD98, case 12
//  @0x49B024..0x49B03C, the convert @0x49B085..0x49B0B9;
//  NapiNPClientMsg_HandleSessionConfig @0x428386 / @0x42838C;
//  Server_ProcessClientRequestSpectatorRespawn @0x51C840]
void check_listen_host_deploy_convert(std::map<std::string, std::string> &files) {
	for (const int spectators : {2, 0}) {
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		auto &kernel = *kernel_storage;
		auto role_storage = std::make_unique<inmatch::HostRole>();
		auto &role = *role_storage;
		role.bind(kernel);
		inmatch::ListenHostState &host = role.state;
		kernel.open_document(two_entity_mission(), "deploy_convert", source_over(&files));
		ms::KernelBootOptions options;
		options.mp_session = true;
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] {
			inmatch::HostConfig cfg;
			cfg.protocol = &kJo;
			cfg.config.server_name = "deploy_convert";
			cfg.config.max_players = 4;
			cfg.config.game_type = options.game_type;
			cfg.config.respawn_timeout = 7;
			cfg.config.permanent_death = true;
			cfg.config.spectator_slots = spectators;
			cfg.socket_mode = inmatch::SocketMode::Lan;
			cfg.serve_and_play = true;
			inmatch::HostBringup bringup;
			bringup.host_cfg = cfg;
			role.bring_up(bringup);
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(host.client_runtime != nullptr && kernel.local.has_local_player());
		opennova::inmatch::NullDatagramSocket socket;
		role.set_socket(&socket);
		for (int i = 0; i < 4; ++i) role.run_tick(tick_input(0));
		inmatch::NapiNPConnection *own = nullptr;
		for (inmatch::NapiNPConnection &conn : host.host_owner.ctx.np_protocol.connection_list)
			if (conn.link.owned_entity == kernel.world.cached.local_player) own = &conn;
		CHECK(own != nullptr);
		if (own == nullptr) continue;
		const opennova::replication::ClientState &cs = host.client_runtime->state();
		CHECK(cs.permanent_death && cs.spectators_allowed == (spectators != 0));
		// A live body's press is the pick, on the host's own client.
		CHECK(!kernel.local.local_player_dead());
		CHECK(role.request_deployment_pick(0xFFFFu));
		role.run_tick(tick_input(0));
		role.run_tick(tick_input(0));
		CHECK(!own->link.spectator);
		// The Respawn key's 0x41 kills the body through the server's handler.
		inmatch::TickInput press = tick_input(0);
		(void)role.input_events().push(opennova::controls::InputEventRecord{55, 0, 0, 0, false});
		role.apply_input(press);
		role.run_tick(tick_input(0));
		role.run_tick(tick_input(0));
		CHECK(kernel.local.local_player_dead());
		const std::size_t queued = host.host_loop.c2s_pending();
		const bool sent = role.request_deployment_pick(0xFFFFu);
		CHECK(sent == (spectators != 0));
		role.run_tick(tick_input(0)); // the press frame's client frame flushes it
		CHECK(host.host_loop.c2s_pending() == queued + (spectators != 0 ? 1u : 0u));
		for (int i = 0; i < 3; ++i) role.run_tick(tick_input(0));
		CHECK(own->link.spectator == (spectators != 0));
	}
}

// The listen host's own deploy pick (C2S 0x0E) reaches the server's handler with the host
// context, as a joiner's does, so its deploy writes the state byte of the protocol the session
// speaks: a DFX2 host's sets the pre-round loadout latch at every deploy (DFX2-11), a JO host's
// only while the pre-round timer runs.
// [orig: Input_HandleActionBinding case 12 -> CNapiNetwork_QueueReliableMessage(0x0E)
//  @0x49b17b; Server_ProcessClientRequestRespawn @0x519AF0; Server_DeployPlayer
//  @0x517803/@0x517812] [orig: Server_DeployPlayer @0x514a55 (dfx2.exe)]
void check_listen_host_own_deploy_reads_its_protocol(std::map<std::string, std::string> &files) {
	for (const char *code : {"jo", "dfx2"}) {
		const Protocol *protocol = title_protocol(code);
		const bool is_dfx2 = std::string(code) == "dfx2";
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		auto &kernel = *kernel_storage;
		auto role_storage = std::make_unique<inmatch::HostRole>();
		auto &role = *role_storage;
		role.bind(kernel);
		inmatch::ListenHostState &host = role.state;
		kernel.open_document(two_entity_mission(), "own_deploy", source_over(&files));
		ms::KernelBootOptions options;
		options.mp_session = true;
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] {
			inmatch::HostConfig cfg;
			cfg.protocol = protocol;
			cfg.config.server_name = "own_deploy";
			cfg.config.max_players = 4;
			cfg.config.game_type = options.game_type;
			cfg.socket_mode = inmatch::SocketMode::Lan;
			cfg.serve_and_play = true;
			inmatch::HostBringup bringup;
			bringup.host_cfg = cfg;
			role.bring_up(bringup);
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(host.client_runtime != nullptr && kernel.local.has_local_player());
		CHECK(host.host_owner.ctx.np_protocol.identity == protocol);
		opennova::inmatch::NullDatagramSocket socket;
		role.set_socket(&socket);
		for (int i = 0; i < 4; ++i) role.run_tick(tick_input(0));
		inmatch::NapiNPConnection *own = nullptr;
		for (inmatch::NapiNPConnection &conn : host.host_owner.ctx.np_protocol.connection_list)
			if (conn.link.owned_entity == kernel.world.cached.local_player) own = &conn;
		CHECK(own != nullptr && own->burst.spawned);
		if (own == nullptr) continue;
		// The slot waits on its pick with no pre-round timer running, so only the protocol's
		// fixed state byte sets the latch.
		kernel.world.preround_delay_seconds = 0;
		own->link.respawn_pending = true;
		own->link.respawn_delay_seconds = 0;
		own->link.preround_loadout_latch = false;
		CHECK(role.request_deployment_pick(0xFFFFu));
		host.client_runtime->Client_ProcessNetworkFrame(); // the client frame sends the pick
		role.drain_host_client_gameplay_requests();        // the server's handler, own slot
		CHECK(!own->link.respawn_pending);
		CHECK(own->link.preround_loadout_latch == is_dfx2);
	}
}

// V-L2-s8 (D-NET-710 iii): on the authority the head's 0x400 operand is the own client's
// pre-round copy (g_ClientPreRoundTimer, written only by the client's 0x0A handler), not the
// server's countdown: the seat row (17, code 182, 0x0C000C01) passes while only the server's
// timer runs and drops while the own copy runs; the Knife row (28, code 201, 0x0C000801)
// carries no 0x400.
// [orig: Input_HandleActionBinding @0x49AD40 - `cmp g_ClientPreRoundTimer, 0` @0x49ADBB;
//  NapiNPClientMsg_0x00A @0x430064]
void check_head_preround_reads_the_own_clients_copy(std::map<std::string, std::string> &files) {
	auto kernel_storage = std::make_unique<ms::MissionKernel>();
	auto &kernel = *kernel_storage;
	auto role_storage = std::make_unique<inmatch::HostRole>();
	auto &role = *role_storage;
	role.bind(kernel);
	inmatch::ListenHostState &host = role.state;
	kernel.open_document(two_entity_mission(), "head_preround", source_over(&files));
	ms::KernelBootOptions options;
	options.mp_session = true;
	options.game_type = mission_game_type(kernel.mission);
	options.bringup_net_session = [&] {
		inmatch::HostConfig cfg;
		cfg.protocol = &kJo;
		cfg.config.server_name = "head_preround";
		cfg.config.max_players = 4;
		cfg.config.game_type = options.game_type;
		cfg.socket_mode = inmatch::SocketMode::Lan;
		cfg.serve_and_play = true;
		inmatch::HostBringup bringup;
		bringup.host_cfg = cfg;
		role.bring_up(bringup);
	};
	std::string error;
	CHECK(kernel.boot(options, error));
	CHECK(host.client_runtime != nullptr && kernel.local.has_local_player());
	if (host.client_runtime == nullptr) return;
	opennova::inmatch::NullDatagramSocket socket;
	role.set_socket(&socket);
	for (int i = 0; i < 4; ++i) role.run_tick(tick_input(0));
	host.client_runtime->state().deploy_overlay_active = false;
	host.client_runtime->state().spawn_success_gate = false;
	CHECK(!kernel.local.local_player_dead());
	kernel.world.preround_delay_seconds = 5;
	kernel.world.cached.client_preround_timer = 0;
	CHECK(role.action_head_admits(182));
	kernel.world.preround_delay_seconds = 0;
	kernel.world.cached.client_preround_timer = 5;
	CHECK(!role.action_head_admits(182));
	CHECK(role.action_head_admits(201));
}

// D-NET-710 (L2-s9 row 5, LX-s8's kick witness): a listen host's console never punts by
// itself. Its line goes to its own server as C2S 0x04, whose handler runs only NETLOG / STAT
// and echoes the rest to the sender as S2C 0x06; the echo runs the client's walk, where a
// bare PUNT ALL is no verb (nobody punted) and the SERVER verb sends the rest of the line
// back as C2S 0x24, which the server runs with the local slot's authority: PUNT ALL stages
// reason 33 "S.C:SPA" on every non-local slot (a joiner's 0x46 burst answers it).
// [orig: Client_ProcessConsoleCommand @0x520E80 - the peer's C2S 0x04 @0x520EFE..0x520F29;
//  NapiNPServerMsg_HandleConsoleLine @0x5199D0 - the NETLOG arm @0x519A7B, the S2C 0x06
//  @0x519AB5..0x519AC5; NapiNPClientMsg_HandleConsoleEcho @0x432BC0 ->
//  NapiClientCmd_ParseAndExecute @0x4329C0 (the call @0x432C12);
//  NapiClientCmd_HandleDebugCommand @0x42C4D0 - the
//  SERVER arm @0x42C85D, the match @0x42C867, the C2S 0x24 @0x42C904..0x42C922;
//  NapiNPServerMsg_0x024 @0x514DC0 -> Server_HandleBanPuntCommand @0x50B380 (the sender's
//  slot @0x514DE7, the local slot's authority @0x50B3C4, PUNT ALL @0x50B945..0x50B951)]
void check_listen_host_console_punts_only_through_server(
		std::map<std::string, std::string> &files, const char *line, bool punts) {
	auto kernel_storage = std::make_unique<ms::MissionKernel>();
	auto &kernel = *kernel_storage;
	auto role_storage = std::make_unique<inmatch::HostRole>();
	auto &role = *role_storage;
	role.bind(kernel);
	inmatch::ListenHostState &host = role.state;
	kernel.open_document(two_entity_mission(), "console_punt", source_over(&files));
	ms::KernelBootOptions options;
	options.mp_session = true;
	options.game_type = mission_game_type(kernel.mission);
	options.bringup_net_session = [&] {
		inmatch::HostConfig cfg;
		cfg.protocol = &kJo;
		cfg.config.server_name = "console_punt";
		cfg.config.max_players = 4;
		cfg.config.game_type = options.game_type;
		cfg.socket_mode = inmatch::SocketMode::Lan;
		cfg.serve_and_play = true;
		inmatch::HostBringup bringup;
		bringup.host_cfg = cfg;
		role.bring_up(bringup);
	};
	std::string error;
	CHECK(kernel.boot(options, error));
	CHECK(host.client_runtime != nullptr);
	if (host.client_runtime == nullptr) return;
	opennova::inmatch::NullDatagramSocket socket;
	role.set_socket(&socket);
	for (int i = 0; i < 70; ++i) role.run_tick(tick_input(0)); // past the console's first second
	// A remote player's slot (a joiner's server-side connection, added and in the match).
	auto transport = std::make_unique<replication::UdpSessionTransport>(
			replication::UdpSessionTransport::Role::Host);
	inmatch::NapiNPConnection remote;
	remote.peer = PeerAddr{0x0100007Fu, 34100};
	remote.type = inmatch::NapiNPConnection::kTypeServerSide;
	remote.phase = inmatch::ConnectionPhase::InMatch;
	remote.burst.spawned = true;
	remote.reply.player_slot = 1;
	remote.server_sk = 0x51525354u;
	remote.client_ck = 0x61626364u;
	remote.server_scrk = "CONSOLE-PUNT-SERVER-SCRK";
	remote.client_scrk = "CONSOLE-PUNT-CLIENT-SCRK";
	remote.link.mode = replication::TransportMode::Client;
	remote.link.transport = transport.get();
	host.host_owner.ctx.np_protocol.connection_list.push_back(std::move(remote));
	const auto remote_conn = [&]() -> inmatch::NapiNPConnection * {
		for (inmatch::NapiNPConnection &conn : host.host_owner.ctx.np_protocol.connection_list)
			if (conn.peer.port == 34100) return &conn;
		return nullptr;
	};
	CHECK(host.client_runtime->console_command(line));
	for (int i = 0; i < 12; ++i) role.run_tick(tick_input(0));
	const inmatch::NapiNPConnection *punted = remote_conn();
	CHECK(punted != nullptr && punted->host_disconnect_sent == punts);
}

// D-NET-1285 (LI-s25 row 0, LI-s24 FOUND 1): single player is connection mode 3, host and
// client, out of a session. Its typed line reaches the command entry through the host's own
// client: the local arms read the session word (clear), so the binding CRC arm queues the cheat
// row's record 123 at once, whose dispatch flips the AI-skips-the-local-player bit
// (D-NET-1284). A line the local arms leave rides the local client connection as C2S 0x04,
// the peer's leg: the client frame sends it, the next frame's server receive runs the
// console-line handler, which echoes it to the sender as S2C 0x06, and that frame's client
// receive runs the walk, whose binding arm queues the named record (the respawn row's 55).
// [orig: SinglePlayer_StartMission @0x561AF0 - 3 pushed @0x561BD9, the call @0x561BE0;
//  CNapiGameSession_CreateSession @0x4C97C0 - CNapiNPConnection_Create(.., 2) @0x4C9B9C;
//  Client_ProcessConsoleCommand @0x520E80 - Client_ProcessLocalCommand @0x520EEC, the peer
//  test @0x520EF8, the 0x04 @0x520F29; Client_ProcessLocalCommand @0x520BF0 - the session test
//  @0x520DE5, the CRC walk @0x520E17..0x520E30; Server_TickUpdate @0x51D7E0 -
//  CNapiNetwork_PumpServerProtocolRecv called @0x51D895 (no session test ahead of it);
//  NapiNPServerMsg_HandleConsoleLine @0x5199D0 - the S2C 0x06 @0x519AC5;
//  Client_ProcessNetworkFrame @0x42C180 - CNapiNetwork_PumpClientProtocolRecv @0x42C228;
//  NapiClientCmd_ParseAndExecute @0x4329C0 - the name walk @0x432a64..0x432a94]
void check_single_player_console_line_rides_the_loopback(
		std::map<std::string, std::string> &files) {
	auto kernel_storage = std::make_unique<ms::MissionKernel>();
	auto &kernel = *kernel_storage;
	auto role_storage = std::make_unique<inmatch::HostRole>(inmatch::RoleKind::SinglePlayer,
			nullptr);
	auto &role = *role_storage;
	role.bind(kernel);
	inmatch::ListenHostState &host = role.state;
	kernel.open_document(two_entity_mission(), "sp_console", source_over(&files));
	ms::KernelBootOptions options;
	options.game_type = mission_game_type(kernel.mission);
	options.bringup_net_session = [&] { role.bring_up_singleplayer(*opennova::title_by_code("jo")); };
	std::string error;
	CHECK(kernel.boot(options, error));
	CHECK(host.client_runtime != nullptr);
	if (host.client_runtime == nullptr) return;
	CHECK(host.host_owner.ctx.is_mp_session_peer != 0 && host.host_owner.ctx.is_authority != 0 &&
			!kernel.world.rules.mp_session);
	for (int i = 0; i < 70; ++i) role.run_tick(tick_input(0)); // past the console's first second
	controls::InputEventQueue &events = role.input_events();
	events.reset();
	// The cheat line: a test line whose CRC is row 116's token dword 0xFF623840.
	const std::string cheat = "LI-s24 \xA8\xB5\x97[";
	CHECK(host.client_runtime->console_command(cheat));
	CHECK(events.size() == 1 && events.at(0).code == 123);
	CHECK(!kernel.world.rules.ai_rules_skip_local_player);
	role.drain_input_events(nullptr, 0);
	CHECK(events.empty() && kernel.world.rules.ai_rules_skip_local_player);
	// The name line: no local arm takes it, so nothing is queued until the echo.
	const int32_t respawn = 55;
	CHECK(respawn == 55);
	CHECK(host.client_runtime->console_command("respawn"));
	CHECK(events.empty());
	role.run_tick(tick_input(0)); // the client frame sends the 0x04, after the server's receive
	CHECK(events.empty());
	role.run_tick(tick_input(0)); // the server's receive echoes it, the client's receive walks it
	CHECK(events.size() == 1 && events.at(0).code == respawn);
	// D-NET-1302 (LI-s26 row 0, LI-s25 FOUND 3): a typed token of a row whose arm is the
	// embedder's reaches that arm through the one dispatcher a key's record meets. "verbose"
	// (row 75, code 37) and "help" (row 106, code 8) are HUD rows, "exit" (row 77, code 3) the
	// sim's exit; "magazine" (row 47, code 211) is the role's own reload arm and goes no further.
	// The authority takes one console line a second from a slot, on the word the chat throttle
	// shares: a line its receive meets inside the second past the last one it took is dropped,
	// echo and all, so the second of two lines typed in one frame never queues a record. Each
	// line here waits out the second past the previous echo (the host's clock runs
	// kTicksPerSecondInt ticks a second, host_milliseconds_for_logic_tick).
	// [orig: NapiNPServerMsg_HandleConsoleLine @0x5199D0 - GetTickCount @0x519A1E, the slot's
	//  +100360 word @0x519A24, `!last || now >= last + 1000` @0x519A36, the store @0x519A3C]
	events.reset();
	struct Device final : inmatch::TickInputSource {
		std::vector<int32_t> codes;
		inmatch::InputPacket sample_tick(uint32_t) override { return {}; }
		void dispatch_record(const controls::InputEventRecord &record) override {
			codes.push_back(record.code);
		}
	};
	Device device;
	const auto typed_a_second_on = [&](std::initializer_list<const char *> lines) {
		for (int i = 0; i < io::kTicksPerSecondInt; ++i) role.run_tick(tick_input(0));
		for (const char *line : lines) CHECK(host.client_runtime->console_command(line));
		role.run_tick(tick_input(0)); // the client frame sends the 0x04s
		role.run_tick(tick_input(0)); // the server's receive echoes, the client's receive walks
	};
	typed_a_second_on({"verbose", "magazine"}); // one frame: the throttle drops "magazine"
	CHECK(events.size() == 1 && events.at(0).code == 37);
	role.drain_input_events(&device, 0);
	typed_a_second_on({"magazine"});
	CHECK(events.size() == 1 && events.at(0).code == 211);
	role.drain_input_events(&device, 0);
	typed_a_second_on({"help"});
	CHECK(events.size() == 1 && events.at(0).code == 8);
	role.drain_input_events(&device, 0);
	typed_a_second_on({"exit"});
	CHECK(events.size() == 1 && events.at(0).code == 3);
	role.drain_input_events(&device, 0);
	CHECK(events.empty() && device.codes == std::vector<int32_t>({37, 8, 3}));
}

// D-NET-1306 (LI-s28 row 0, LI-s26 FOUND 1): the dispatcher's server and process rows
// reach their cases through the one dispatcher, whatever queued the record. On a listen host
// in a LAN session (the authority, a session peer): savescores flips the process word and
// posts its literal SYSTEM line; netdelay sends S2C 0x1B and the chat line to the remote
// slot; LastGame flips the rotation's word and posts its Misc toast; tod, a non-peer
// authority's row, sets nothing here; Ban lists, punts and posts; resetgames ends the round
// and clears the tallies. On a Serve Only host (the authority, no peer) tod sets the clock
// word with its tickdown and todrate the advance per tick.
// [orig: Input_HandleActionBinding @0x49AD40 - case 38 @0x49B7B3, case 103 @0x49BB73, case 106
//  @0x49BB9A, case 107 @0x49BBA5, case 108 @0x49BBFE, case 438 @0x49BDAB..0x49BE43, case 497
//  @0x49BE5A..0x49BE8D]
void check_server_rows_reach_their_arms(std::map<std::string, std::string> &files) {
	{
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		auto &kernel = *kernel_storage;
		auto role_storage = std::make_unique<inmatch::HostRole>();
		auto &role = *role_storage;
		role.bind(kernel);
		inmatch::ListenHostState &host = role.state;
		kernel.open_document(two_entity_mission(), "server_rows", source_over(&files));
		ms::KernelBootOptions options;
		options.mp_session = true;
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] {
			inmatch::HostConfig cfg;
			cfg.protocol = &kJo;
			cfg.config.server_name = "server_rows";
			cfg.config.max_players = 4;
			cfg.config.game_type = options.game_type;
			cfg.socket_mode = inmatch::SocketMode::Lan;
			cfg.serve_and_play = true;
			inmatch::HostBringup bringup;
			bringup.host_cfg = cfg;
			role.bring_up(bringup);
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(host.client_runtime != nullptr && kernel.local.has_local_player());
		if (host.client_runtime == nullptr) return;
		opennova::inmatch::NullDatagramSocket socket;
		role.set_socket(&socket);
		for (int i = 0; i < 4; ++i) role.run_tick(tick_input(0));
		inmatch::NapiNPServerCtx &ctx = host.host_owner.ctx;
		CHECK(ctx.is_in_session != 0 && ctx.is_authority != 0 && ctx.is_mp_session_peer != 0);
		auto transport = std::make_unique<replication::UdpSessionTransport>(
				replication::UdpSessionTransport::Role::Host);
		inmatch::NapiNPConnection remote;
		remote.peer = PeerAddr{0x0100007Fu, 34100};
		remote.type = inmatch::NapiNPConnection::kTypeServerSide;
		remote.phase = inmatch::ConnectionPhase::InMatch;
		remote.burst.spawned = true;
		remote.reply.player_slot = 1;
		remote.reply.player_name = "R1";
		remote.link.mode = replication::TransportMode::Client;
		remote.link.transport = transport.get();
		ctx.np_protocol.connection_list.push_back(std::move(remote));
		inmatch::NapiNPConnection *joined = nullptr;
		for (inmatch::NapiNPConnection &conn : ctx.np_protocol.connection_list)
			if (conn.peer.port == 34100) joined = &conn;
		CHECK(joined != nullptr);
		if (joined == nullptr) return;
		std::vector<w::HudChatLine> &lines = kernel.world.out.hud_chat_lines;
		const auto dispatch = [&](int32_t code, int32_t p1) {
			lines.clear();
			(void)role.input_events().push(controls::InputEventRecord{code, p1, 0, 0, false});
			role.drain_input_events(nullptr, 0);
		};
		// savescores (row 88, code 38).
		dispatch(38, 0);
		CHECK(role.process_state() != nullptr && role.process_state()->save_scores);
		CHECK(lines.size() == 1 && lines[0].text == "Save Scores: On" && lines[0].system_ring &&
				lines[0].argb == 0xFFFFFFFFu);
		dispatch(38, 0);
		CHECK(!role.process_state()->save_scores && lines.size() == 1 &&
				lines[0].text == "Save Scores: Off");
		// PuntCRC (row 85, code 47): the process's eight counters in one SYSTEM line
		// (D-NET-1307) [orig: Debug_PrintScoreCounters @0x4FD6A0 - @0x4FD6F3, @0x4FD704].
		role.process_state()->punt_crc_counters = {0, 3, 1, 0, 0, 0, 0, 0};
		dispatch(47, 0);
		CHECK(lines.size() == 1 && lines[0].text == "1:0 2:3 3:1 4:0 5:0 6:0 7:0 8:0" &&
				lines[0].system_ring && lines[0].argb == 0xFFFFFFFFu);
		// scopescale (row 74, code 49): the session's scope-scale word steps (0 + 16 clamps to
		// 128) and the Misc toast takes it as $A; a typed argument sets it (D-NET-1318)
		// [orig: Input_HandleActionBinding_0 @0x4E0420 - case 49 @0x4E056D..0x4E05F2].
		kernel.local.look_settings.scope_scale = 0;
		dispatch(49, 0);
		CHECK(kernel.local.look_settings.scope_scale == 0x80 && lines.size() == 1 &&
				std::string(lines[0].key) == "STRMISC_SCOPESCALE" && lines[0].text == "128" &&
				lines[0].system_ring && lines[0].argb == 0xFFFFFFFFu);
		dispatch(49, 300);
		CHECK(kernel.local.look_settings.scope_scale == 300 && lines.size() == 1 &&
				lines[0].text == "300");
		// The movement and look rows as records (D-NET-1319): a typed forward row ORs its
		// input-flag bit for the frame's fold, an axis form (a first argument) sets none, and
		// Last_move queues the forward row's record into the same drain; the next tick's fold
		// carries the bit into the word its pack reports.
		// [orig: Input_HandleActionBinding_0 @0x4E0420 - case 152 @0x4E0CBE, the axis
		//  @0x4E0CB1, case 176 @0x4E0CD5, 425's Input_QueueEvent(152, 0, 0, 1) @0x4E13ED]
		host.client_runtime->state().deploy_overlay_active = false;
		host.client_runtime->state().spawn_success_gate = false;
		kernel.world.cached.client_preround_timer = 0;
		kernel.local.record_input_bits = 0;
		dispatch(152, 0);
		CHECK(kernel.local.record_input_bits == w::kInputFlagForward);
		role.run_tick(tick_input(0));
		CHECK(kernel.local.record_input_bits == 0 &&
				((kernel.local.input_flags.prev | kernel.local.input_flags.flags) &
						w::kInputFlagForward) != 0);
		dispatch(152, 300);
		CHECK(kernel.local.record_input_bits == 0);
		dispatch(425, 0);
		CHECK(kernel.local.record_input_bits == w::kInputFlagForward && role.input_events().empty());
		kernel.local.record_input_bits = 0;
		dispatch(176, 0);
		CHECK(kernel.local.record_input_bits == w::kInputFlagFreeLook);
		kernel.local.record_input_bits = 0;
		// netdelay (row 89, code 103): the remote slot takes S2C 0x1B [2], then the chat line.
		replication::Datagram sent;
		while (transport->pop_outbound(sent)) {}
		dispatch(103, 2);
		std::vector<uint8_t> tags;
		while (transport->pop_outbound(sent)) tags.push_back(sent.tag);
		CHECK(tags == std::vector<uint8_t>({s2c::NETWORK_DELAY, s2c::CHAT_BROADCAST}));
		// LastGame (row 94, code 108).
		inmatch::HostRotation rotation;
		ctx.rotation = &rotation;
		dispatch(108, 0);
		CHECK(rotation.last_game && lines.size() == 1 &&
				std::string(lines[0].key) == "STRMISC_LASTGAME_ON" && lines[0].system_ring);
		dispatch(108, 0);
		CHECK(!rotation.last_game && lines.size() == 1 &&
				std::string(lines[0].key) == "STRMISC_LASTGAME_OFF");
		ctx.rotation = nullptr;
		// tod (row 95, code 438): a session peer's authority sets nothing.
		const uint32_t tod = kernel.world.weather.tod_fixed24;
		const uint32_t peer_resets = kernel.world.weather.time_set_resets;
		dispatch(438, 1230);
		CHECK(kernel.world.weather.tod_fixed24 == tod &&
				kernel.world.weather.time_set_resets == peer_resets);
		// Ban (row 93, code 107): slot 1 is listed and punted, and the SYSTEM line posts.
		ctx.server_text.kicked = "Kicked:";
		dispatch(107, 1);
		CHECK(joined->host_disconnect_sent);
		CHECK(!ctx.bans.addresses.entries.empty() && ctx.bans.addresses.entries.back().name == "R1");
		CHECK(lines.size() == 1 && lines[0].text == "Kicked: R1" && lines[0].system_ring);
		// resetgames (row 92, code 106): the tallies of teams 1 and 2 and the rounds clear.
		ctx.round_wins = {2, 3, 4, 5};
		ctx.rounds_played = 6;
		dispatch(106, 0);
		CHECK(ctx.round_wins[0] == 0 && ctx.round_wins[1] == 0 && ctx.round_wins[2] == 4 &&
				ctx.round_wins[3] == 5 && ctx.rounds_played == 0);
	}
	{
		inmatch::ClientProcessState process;
		const auto kernel_owner = std::make_unique<ms::MissionKernel>();
		const auto role_owner = std::make_unique<inmatch::HostRole>();
		ms::MissionKernel &kernel = *kernel_owner;
		inmatch::HostRole &role = *role_owner;
		role.bind(kernel);
		kernel.open_document(two_entity_mission(), "server_rows_dedicated", source_over(&files));
		ms::KernelBootOptions options;
		options.playable = false;
		options.mp_session = true;
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] {
			inmatch::HostConfig cfg;
			cfg.protocol = &kJo;
			cfg.config.server_name = "server_rows_dedicated";
			cfg.config.max_players = 4;
			cfg.config.game_type = options.game_type;
			cfg.socket_mode = inmatch::SocketMode::Lan;
			cfg.serve_and_play = false;
			cfg.client_process = &process;
			inmatch::HostBringup bringup;
			bringup.host_cfg = cfg;
			role.bring_up(bringup);
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		opennova::inmatch::NullDatagramSocket socket;
		role.set_socket(&socket);
		role.run_tick(tick_input(0));
		const auto dispatch = [&](int32_t code, int32_t p1) {
			(void)role.input_events().push(controls::InputEventRecord{code, p1, 0, 0, false});
			role.drain_input_events(nullptr, 0);
		};
		// tod 1230: 12 << 24 | 30 * 0x44400, the tickdown 10, then one tile-cache and cube reset
		// (@0x49BE4B / @0x49BE50).
		kernel.world.weather.tod_epoch_tickdown = 0;
		const uint32_t resets = kernel.world.weather.time_set_resets;
		dispatch(438, 1230);
		CHECK(kernel.world.weather.tod_fixed24 == ((12u << 24) | (30u * 0x44400u)) &&
				kernel.world.weather.tod_epoch_tickdown == 10 &&
				kernel.world.weather.time_set_resets == resets + 1);
		// todrate 4: 0x18000000 / 248; todrate 0 keeps it.
		dispatch(497, 4);
		CHECK(kernel.world.weather.tod_advance_per_tick == 0x18000000u / 248u);
		dispatch(497, 0);
		CHECK(kernel.world.weather.tod_advance_per_tick == 0x18000000u / 248u);
		// A typed `tod 0630` on this host (its console's command walk, the remote admin's CMD)
		// queues the row's record with atol(0630) on the role's queue, which the drain then
		// dispatches (D-NET-1323).
		// [orig: Client_ProcessConsoleCommand @0x520E80 - NapiClientCmd_ParseAndExecute called
		//  @0x520F70; NapiClientCmd_ParseAndExecute @0x4329C0 - Input_QueueEvent called @0x432BA1]
		gamecfg::GameCfg block = gamecfg::defaults();
		inmatch::AdminConsole::Seams seams;
		seams.config_block = &block;
		seams.config_title = opennova::title_by_code("jo");
		seams.input_events = &role.input_events();
		inmatch::AdminConsole console(role.state.host_owner.ctx, std::move(seams));
		console.set_scene(inmatch::AdminScene::GameLoop);
		std::vector<std::string> replies;
		(void)console.dispatch(AdminSession{"boss", 0xFFFFFFFFu}, "CMD tod 0630", replies);
		CHECK(role.input_events().size() == 1 && role.input_events().at(0).code == 438 &&
				role.input_events().at(0).p1 == 630);
		role.drain_input_events(nullptr, 0);
		CHECK(role.input_events().empty() &&
				kernel.world.weather.tod_fixed24 == ((6u << 24) | (30u * 0x44400u)));
	}
}

// Its own frame: main's locals already fill the 1 MB test stack.
void check_quality_report_runs_on(std::map<std::string, std::string> &files) {
	// --- the 0x4C report's count is the process's --------------------------
	// Retail keeps the count in one global that only the report zeroes, so a
	// client remade mid-cycle (a map change's) reports on the same 311-frame
	// beat; the embedder's ClientProcessState carries it across sessions. Here
	// the count stands at 200 at frame 201 and the host's level lands at frame 312.
	// [orig: g_NetQualityReportTimer @0xA85B84 - Client_ProcessNetworkFrame
	//  @0x42C239 / @0x42C246, its only writers]
	{
		// Heap-held: a second kernel in this frame overflows the 1 MB test-main stack.
		const auto kernel_owner = std::make_unique<ms::MissionKernel>();
		const auto role_owner = std::make_unique<inmatch::HostRole>();
		ms::MissionKernel &kernel = *kernel_owner;
		inmatch::HostRole &role = *role_owner;
		role.bind(kernel);
		inmatch::ListenHostState &host = role.state;
		kernel.open_document(two_entity_mission(), "quality_report_runs_on", source_over(&files));
		ms::KernelBootOptions options;
		options.mp_session = true;
		options.game_type = mission_game_type(kernel.mission);
		inmatch::ClientProcessState process;
		options.bringup_net_session = [&] {
			inmatch::HostConfig cfg;
			cfg.protocol = &kJo;
			cfg.config.server_name = "quality_report_runs_on";
			cfg.config.max_players = 4;
			cfg.config.game_type = options.game_type;
			cfg.socket_mode = inmatch::SocketMode::Lan;
			cfg.serve_and_play = true;
			cfg.client_process = &process;
			inmatch::HostBringup bringup;
			bringup.host_cfg = cfg;
			role.bring_up(bringup);
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		opennova::inmatch::NullDatagramSocket socket;
		role.set_socket(&socket);
		role.observe_frame_rate(62);
		kernel.capture_baseline();
		int stored_at = -1;
		for (int frame = 1; frame <= 600 && stored_at < 0; ++frame) {
			if (frame == 201) CHECK(process.net_quality_report_timer == 200);
			role.run_tick(tick_input(0));
			for (const inmatch::NapiNPConnection &conn : host.host_owner.ctx.np_protocol.connection_list)
				if (conn.link.owned_entity == kernel.world.cached.local_player &&
						conn.reply.client_quality != 0)
					stored_at = frame;
		}
		std::printf("host_role: the remade client's 0x4C stored at frame %d\n", stored_at);
		CHECK(stored_at == 312);
	}
}

// A Serve Only host's g_NetQuality: retail's main frame stores the sample's level in it, steps
// its indicators in a session and raises the link-error flags with no client of its own, and
// the status page draws it (D-NET-971). In a NovaWorld session whose NWU session is in
// use but not yet ready the N sits at 255 (then fades 10 a frame once ready), the stored
// level 3 ramps its channel 4 a frame, an incoming link error raises its bit; the HUD facts
// read all of it off the context. A LAN host's N stays dark and the facts carry no N gate.
// [orig: Game_ProcessMainFrame @0x5263F0 - CNetQuality_SetLevel @0x52659B, `cmp is_in_session`
//  @0x526680 -> CNetQuality_UpdateIndicators @0x52668D, Server_TickUpdate @0x5266B6;
//  CNetQuality_UpdateIndicators @0x4C3090; CNetQuality_SetLinkErrorFlag @0x4C34F0;
//  CNetQuality_DrawIndicators @0x4C3200 (the N gate @0x4C33F0)]
void check_serve_only_net_quality_indicators(std::map<std::string, std::string> &files) {
	for (const bool novaworld : {true, false}) {
		inmatch::ClientProcessState process;
		const auto kernel_owner = std::make_unique<ms::MissionKernel>();
		const auto role_owner = std::make_unique<inmatch::HostRole>();
		ms::MissionKernel &kernel = *kernel_owner;
		inmatch::HostRole &role = *role_owner;
		role.bind(kernel);
		kernel.open_document(two_entity_mission(), "serve_only_indicators", source_over(&files));
		ms::KernelBootOptions options;
		options.playable = false;
		options.mp_session = true;
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] {
			inmatch::HostConfig cfg;
			cfg.protocol = &kJo;
			cfg.config.server_name = "serve_only_indicators";
			cfg.config.max_players = 4;
			cfg.config.game_type = options.game_type;
			cfg.socket_mode = inmatch::SocketMode::Lan;
			cfg.serve_and_play = false;
			cfg.client_process = &process;
			inmatch::HostBringup bringup;
			bringup.host_cfg = cfg;
			role.bring_up(bringup);
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(role.state.client_runtime == nullptr);
		opennova::inmatch::NullDatagramSocket socket;
		role.set_socket(&socket);
		inmatch::NapiNPServerCtx &ctx = role.state.host_owner.ctx;
		CHECK(ctx.is_in_session != 0);
		if (novaworld) ctx.transport_mode = inmatch::NetworkType::NovaWorld;
		ctx.nwu_in_use = novaworld;
		// Hosting, so the sample's NovaWorld exit stays clear [orig: Game_ProcessMainFrame
		//  @0x52656D..0x52657C (dword_B60108 2 / 3 skip the exit store)].
		ctx.nwu_session_role = inmatch::kNwuSessionRoleHosting;
		ctx.nwu_session_flags = 0;
		role.run_tick(tick_input(0)); // the first frame's sample opens the 62-frame block
		// A later sample's level 3 (off the block's frame, so no sample rewrites it) and this
		// tick's incoming link error.
		ctx.net_quality_level = 3;
		ctx.net_quality_link_errors = inmatch::kNetQualityLinkErrorIncoming;
		role.run_tick(tick_input(0));
		const hud::NetQualityIndicators &q = ctx.net_quality_indicators;
		CHECK(q.level == 3 && q.level3_alpha == 4); // one frame's ramp step
		CHECK(q.link_error_bits == static_cast<uint32_t>(hud::kNetLinkErrorIncoming) &&
				q.link_error_countdown == hud::kNetLinkErrorFrames);
		CHECK(q.novaworld_alpha == (novaworld ? 255 : 0));
		inmatch::RoleView view;
		view.kernel = &kernel;
		view.host = &ctx;
		const inmatch::HudRoleFacts facts = inmatch::hud_role_facts(view);
		CHECK(facts.net_quality.in_session && facts.net_quality.novaworld_icon == novaworld);
		CHECK(facts.net_quality.indicators.level == 3 &&
				facts.net_quality.indicators.novaworld_alpha == q.novaworld_alpha);
		if (novaworld) {
			// Bits 2 and 8: the NWU session is ready, so the N fades 10 a frame.
			ctx.nwu_session_flags = 0x0A;
			role.run_tick(tick_input(0));
			CHECK(ctx.net_quality_indicators.novaworld_alpha == 245);
		}
	}
}

// Its own frame, like check_quality_report_runs_on.
void check_serve_only_steps_quality_count(std::map<std::string, std::string> &files) {
	// --- a Serve Only host steps the process's 0x4C count ----------------------
	// Retail's main frame runs the client net frame in every role, so a Serve Only host steps
	// the count (and zeroes it past 310) with no report; a listen host the same process runs
	// next reports on the phase the dedicated frames left: after N of them, at (311 - N) + 1.
	// [orig: Game_ProcessMainFrame @0x526692; Client_ProcessNetworkFrame @0x42C239 / @0x42C246]
	// Few enough that the listen client's level is measured by its first report (a 0 level
	// stores nothing the test can see).
	constexpr int kDedicatedFrames = 10;
	inmatch::ClientProcessState process;
	{
		const auto kernel_owner = std::make_unique<ms::MissionKernel>();
		const auto role_owner = std::make_unique<inmatch::HostRole>();
		ms::MissionKernel &kernel = *kernel_owner;
		inmatch::HostRole &role = *role_owner;
		role.bind(kernel);
		kernel.open_document(two_entity_mission(), "serve_only_quality", source_over(&files));
		ms::KernelBootOptions options;
		options.playable = false;
		options.mp_session = true;
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] {
			inmatch::HostConfig cfg;
			cfg.protocol = &kJo;
			cfg.config.server_name = "serve_only_quality";
			cfg.config.max_players = 4;
			cfg.config.game_type = options.game_type;
			cfg.socket_mode = inmatch::SocketMode::Lan;
			cfg.serve_and_play = false;
			cfg.client_process = &process;
			inmatch::HostBringup bringup;
			bringup.host_cfg = cfg;
			role.bring_up(bringup);
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(role.state.client_runtime == nullptr);
		opennova::inmatch::NullDatagramSocket socket;
		role.set_socket(&socket);
		for (int frame = 0; frame < kDedicatedFrames; ++frame) role.run_tick(tick_input(0));
		CHECK(process.net_quality_report_timer == static_cast<uint32_t>(kDedicatedFrames));
	}
	{
		const auto kernel_owner = std::make_unique<ms::MissionKernel>();
		const auto role_owner = std::make_unique<inmatch::HostRole>();
		ms::MissionKernel &kernel = *kernel_owner;
		inmatch::HostRole &role = *role_owner;
		role.bind(kernel);
		inmatch::ListenHostState &host = role.state;
		kernel.open_document(two_entity_mission(), "serve_only_then_listen", source_over(&files));
		ms::KernelBootOptions options;
		options.mp_session = true;
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] {
			inmatch::HostConfig cfg;
			cfg.protocol = &kJo;
			cfg.config.server_name = "serve_only_then_listen";
			cfg.config.max_players = 4;
			cfg.config.game_type = options.game_type;
			cfg.socket_mode = inmatch::SocketMode::Lan;
			cfg.serve_and_play = true;
			cfg.client_process = &process;
			inmatch::HostBringup bringup;
			bringup.host_cfg = cfg;
			role.bring_up(bringup);
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		opennova::inmatch::NullDatagramSocket socket;
		role.set_socket(&socket);
		role.observe_frame_rate(62);
		kernel.capture_baseline();
		int stored_at = -1;
		for (int frame = 1; frame <= 600 && stored_at < 0; ++frame) {
			role.run_tick(tick_input(0));
			for (const inmatch::NapiNPConnection &conn : host.host_owner.ctx.np_protocol.connection_list)
				if (conn.link.owned_entity == kernel.world.cached.local_player &&
						conn.reply.client_quality != 0)
					stored_at = frame;
		}
		std::printf("host_role: after %d Serve Only frames the listen client's 0x4C stored at frame %d\n",
				kDedicatedFrames, stored_at);
		CHECK(stored_at == (311 - kDedicatedFrames) + 1);
	}
}

// The gun's clip spent with rounds left: the walk's RELOAD queues the request
// addressing the gun (an EWeap: 0xFFFF). A playing authority (single player) sends it
// through its own client, the C2S 0x25 bytes [gun handle][FF FF] on the loopback, and
// refills the gun as its S2C 0x49 echo does; a dedicated host has no client
// connection: nothing is sent and the gun is never refilled (D-NET-932). The
// bare local role refills as the echo would.
// [orig: WeaponAction_Reload @0x5430B0 (@0x5430FF); NetPacket_SendReloadRequest
//  @0x432930 (0xFFFF @0x432972..0x432974); CNapiNetwork_QueueReliableMessage
//  @0x4C4FA0 (@0x4C4FA6..0x4C4FA8); CNapiGameSession_CreateSession @0x4C97C0
//  (@0x4C9B6A..0x4C9B76, @0x4C9BA6); NapiNPClientMsg_WeaponReload_0x049 @0x42C0A0
//  (the refill @0x42C116)]
void check_gun_reload_relay(inmatch::Role &role, inmatch::HostRole *host, w::World &world,
		w::EntityHandle gun_handle, int frame_kind) {
	world.tables.weapons.entries.back().action_fsm.clip_capacity = 1;
	world.registry.get(gun_handle)->primary_weapon_slot.clip = 0;
	world.registry.get(gun_handle)->primary_weapon_slot.reserve = 5;
	std::vector<std::vector<uint8_t>> sent;
	for (int i = 0; i < 40; ++i) {
		role.run_tick(tick_input(0));
		if (host == nullptr) continue;
		replication::Datagram d;
		while (host->state.host_loop.host_recv(d))
			if (d.tag == c2s::WEAPON_RELOAD_REQUEST) sent.push_back(d.body);
	}
	const std::vector<uint8_t> expected = {uint8_t(gun_handle.packed & 0xFF),
			uint8_t(gun_handle.packed >> 8), 0xFF, 0xFF};
	if (frame_kind == 0) {
		CHECK(sent.size() == 1);
		CHECK(!sent.empty() && sent[0] == expected);
	} else {
		CHECK(sent.empty());
	}
	CHECK(world.registry.get(gun_handle)->primary_weapon_slot.clip == (frame_kind == 1 ? 0 : 1));
}

int main() {
	std::map<std::string, std::string> files;

	// --- the SP listen bring-up and the per-tick frame ----------------------
	{
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &kernel = *kernel_storage;
		auto role_storage = std::make_unique<inmatch::HostRole>();
		inmatch::HostRole &role = *role_storage;
		role.bind(kernel);
		inmatch::ListenHostState &host = role.state;
		kernel.open_document(two_entity_mission(), "synth", source_over(&files));
		ms::KernelBootOptions options;
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] { role.bring_up_singleplayer(*opennova::title_by_code("jo")); };
		// The synthetic source carries no items.def: the Player row it would give the
		// local body, which the 0x0C needs (D-NET-487).
		kernel.world.tables.player.has_item_def = true;
		kernel.world.tables.player.item_type_index = 1;
		kernel.world.tables.player.item_type = 3;
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(error.empty());


		// The bring-up residue: the np host owner over the kernel's world and
		// mission, serve-and-play over the loopback, the SINGLEPLAYERGAME
		// config, the HostClient runtime folding that loopback, and exactly
		// one dcb-2 connection (the host's own client) whose transport is the
		// loopback. start_host_session spawned the host's own player, so the
		// kernel's own spawn step found one already.
		CHECK(host.host_owner.serve_and_play);
		CHECK(host.host_owner.host_loopback == &host.host_loop);
		CHECK(host.host_owner.ctx.world == &kernel.world);
		CHECK(host.host_owner.ctx.mission == &kernel.mission);
		CHECK(host.host_owner.ctx.config.server_name == "SINGLEPLAYERGAME");
		CHECK(host.host_owner.ctx.config.max_players == 1u);
		// The SP launcher's literal attribute word, not the cfg default 0x3A02
		// [orig: SinglePlayer_StartMission @0x561bb7 -> @0x561cdb].
		CHECK(host.host_owner.ctx.config.mp_attributes == 0x3A06u);
		CHECK(host.host_owner.ctx.config.game_type == options.game_type);
		// The config's unlimited_vehicles word, stock 1, reaches the world:
		// destroyed hulls respawn. [orig: Config_SetDefaults @0x54D352;
		//  Client_BuildMissionDataRequestBlock @0x51E8C5..0x51E8CB]
		CHECK(host.host_owner.ctx.config.unlimited_vehicles);
		CHECK(kernel.world.rules.vehicle_respawns);
		CHECK(host.client_runtime != nullptr);
		if (host.client_runtime) {
			CHECK(host.client_runtime->role() == inmatch::ClientRuntime::Role::HostClient);
			CHECK(host.client_runtime->is_authority());
		}
		CHECK(local_loopback_connections(host) == 1);
		CHECK(kernel.local.has_local_player());
		CHECK(kernel.local.player_ai() != nullptr);

		// N listen frames over the null wire: one logic tick and one owner
		// tick per frame; the per-tick 0x0A fan reaches the local ClientState
		// through the loopback fold; a headless embedder leaves the viewport
		// seam at 0 and a windowed one stamps its height.
		opennova::inmatch::NullDatagramSocket socket;
		role.set_socket(&socket);
		const uint32_t tick0 = kernel.world.logic_tick;
		const uint32_t now0 = host.host_owner.now_tick;
		// Pin the single-owner invariant at one frame, then across a short run.
		role.run_tick(tick_input(0));
		CHECK(kernel.world.logic_tick == tick0 + 1);
		CHECK(host.host_owner.now_tick == now0 + 1);
		for (int i = 0; i < 7; ++i)
			role.run_tick(tick_input(0));
		CHECK(kernel.world.logic_tick == tick0 + 8);
		CHECK(host.host_owner.now_tick == now0 + 8);
		// The SP listen server folds a loopback replica, yet the view arbiter's
		// session word stays clear: the SP launch sets network type 0, so
		// retail's is_in_session reads 0 for the whole mission.
		// [orig: SinglePlayer_StartMission -> CNapiNetwork_SetNetworkType(0)
		//  @0x561bce; the +0x58 store @0x4c4a85]
		CHECK(!kernel.world.rules.mp_session);
		CHECK(!kernel.local.view_session_inputs.in_session);
		CHECK(host.host_owner.ctx.loaded_model_list_count == 0u);
		if (host.client_runtime) {
			CHECK(host.client_runtime->state().frames_applied > 0);
			CHECK(!host.client_runtime->state().entities.empty());
		}
		role.run_tick(tick_input(305));
		CHECK(host.host_owner.ctx.loaded_model_list_count == 305u);
		CHECK(kernel.world.logic_tick == tick0 + 9);

		// No input frame runs under a menu screen: the deferred ToSpecial queue
		// is not flushed there, and is once the frame runs again.
		// [orig: Game_ProcessMainFrame @0x52660D; Input_FlushDeferredEvents's one
		//  caller @0x49D591]
		{
			kernel.local.queue_deferred_event(kernel.local.kToSpecialCode);
			kernel.local.queue_deferred_event(kernel.local.kToSpecialCode);
			inmatch::TickInput menu = tick_input(0);
			menu.consume_one_shots = true;
			menu.player.held_action_bits = inmatch::HELD_MENU_OPEN;
			role.apply_input(menu);
			CHECK(kernel.local.deferred_events.size() == 2);
			inmatch::TickInput live = tick_input(0);
			live.consume_one_shots = true;
			role.apply_input(live);
			CHECK(kernel.local.deferred_events.size() < 2);
			kernel.local.deferred_events.clear();
		}

		// The local gameplay drain consumes reloads while preserving movement
		// for Server_TickUpdate's dedicated uplink decoder.
		const size_t s2c_before = host.host_loop.s2c_pending();
		host.host_loop.client_send(c2s::ENTITY_UPLINK, std::vector<uint8_t>{0});
		WeaponReload reload;
		reload.entity_handle = kernel.local.player()->handle.packed;
		reload.reload_param = 0;
		host.host_loop.client_send(c2s::WEAPON_RELOAD_REQUEST, encode_weapon_reload(reload));
		CHECK(host.host_loop.c2s_pending() == 2);
		role.drain_host_client_gameplay_requests();
		CHECK(host.host_loop.c2s_pending() == 1);
		replication::Datagram preserved;
		CHECK(host.host_loop.host_recv(preserved));
		CHECK(preserved.tag == c2s::ENTITY_UPLINK);
		CHECK(!host.host_loop.host_recv(preserved));
		std::printf("host_role: the reload drain staged %zu S2C reply datagram(s)\n",
				host.host_loop.s2c_pending() - s2c_before);
		// A drain with nothing queued is a no-op.
		role.drain_host_client_gameplay_requests();
		CHECK(host.host_loop.c2s_pending() == 0);
		// The host's own medic call (action 217 -> C2S 0x2E) rides the same
		// queue, sent by its client frame, and must reach the dispatcher, not
		// the movement-only drain. [orig: Input_HandleActionBinding @0x49B4B4..0x49B50C]
		CHECK(role.send_medic_request());
		CHECK(host.host_loop.c2s_pending() == 0);
		host.client_runtime->Client_ProcessNetworkFrame();
		CHECK(host.host_loop.c2s_pending() == 1);
		role.drain_host_client_gameplay_requests();
		CHECK(host.host_loop.c2s_pending() == 0);
		// The host player's command-map sends reach its own server's squad
		// handlers the same way: a C2S 0x45 naming its own slot lands the
		// fireteam there. [orig: NetPacket_SendFireteamAssign @0x42dceb (0x45)
		// -> CNapiNetwork_QueueReliableMessage @0x4c4fa0;
		// NapiNPServerMsg_0x045_HandleTeamAssignment @0x510c00]
		inmatch::NapiNPConnection *self = nullptr;
		for (inmatch::NapiNPConnection &conn : host.host_owner.ctx.np_protocol.connection_list)
			if (conn.type == 2 && conn.link.transport == &host.host_loop) self = &conn;
		CHECK(self != nullptr);
		if (self != nullptr) {
			FireteamAssign assign;
			assign.fireteam = 2;
			assign.members = {self->reply.player_slot};
			host.host_loop.client_send(c2s::FIRETEAM_ASSIGN, encode_fireteam_assign(assign));
			role.drain_host_client_gameplay_requests();
			CHECK(host.host_loop.c2s_pending() == 0);
			CHECK(self->fireteam == 2);
		}
		// The next frame's Server_TickUpdate drains what the local drain left.
		host.host_loop.client_send(c2s::ENTITY_UPLINK, std::vector<uint8_t>{0});
		role.run_tick(tick_input(0));
		CHECK(host.host_loop.c2s_pending() == 0);
		// A point award reaches the local replica's score feedback.
		const w::EntityHandle scorer = kernel.world.cached.local_player;
		CHECK(kernel.world.match.player(scorer) != nullptr);
		if (kernel.world.match.player(scorer) != nullptr) {
			kernel.world.match.player(scorer)->stats[w::MatchStats::kPoints] = 17;
			for (int tick = 0; tick < 62; ++tick) role.run_tick(tick_input(0));
			const auto &feedback = host.client_runtime->state().score_feedback;
			CHECK(feedback.updates == 1);
			CHECK(feedback.score == 17 && feedback.delta == 17);
		}
	}

	// Action 6 from the host's own player must traverse the same dispatcher
	// as a remote C2S 0x16, before the movement-only drain can discard it.
	// [orig: Input_HandleActionBinding_0 @ 0x4E0420 (action 6 @ 0x4E0492);
	// NapiNPServerMsg_HandleWeaponToggle @ 0x511A70]
	{
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		auto &kernel = *kernel_storage;
		auto role_storage = std::make_unique<inmatch::HostRole>();
		auto &role = *role_storage;
		role.bind(kernel);
		kernel.open_document(two_entity_mission(), "tank_slots", source_over(&files));
		ms::KernelBootOptions options;
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] { role.bring_up_singleplayer(*opennova::title_by_code("jo")); };
		std::string error;
		CHECK(kernel.boot(options, error));
		for (int i = 0; i < 8; ++i) role.run_tick(tick_input(0));

		auto &world = kernel.world;
		const auto add_weapon = [&](const char *name, int clip, int rounds) {
			w::WeaponTableEntry def;
			def.name = name;
			def.valid = true;
			def.category = 11;
			def.clipsize = clip;
			def.startrounds = rounds;
			world.tables.weapons.entries.push_back(def);
		};
		add_weapon("WPN_TANK_CANNON", 1, 40);
		add_weapon("WPN_TANK_COAX", 200, 600);
		w::Entity hull;
		hull.kind = w::EntityKind::Item;
		hull.has_item_def = true;
		hull.item_type = 1;
		hull.item_attrib = w::kItemAttribEweap;
		hull.primary_weapon = "WPN_TANK_COAX";
		const auto hull_handle = world.registry.spawn(1, hull);
		CHECK(hull_handle.valid());
		if (!hull_handle.valid()) return 1;
		w::Entity gun;
		gun.kind = w::EntityKind::Item;
		gun.has_item_def = true;
		gun.item_type = 6;
		gun.item_attrib = w::kItemAttribEweap;
		gun.primary_weapon = "WPN_TANK_CANNON";
		gun.emplacement_attachment_flags = 2;
		gun.emplacement_parent = hull_handle;
		gun.emplacement_parent_spawn_id = world.registry.get(hull_handle)->registry_spawn_id;
		gun.ground_target = hull_handle;
		w::Seat seat;
		seat.type = w::SeatType::Gunner;
		seat.bone_index = 1;
		seat.retail_slot = 9;
		gun.seats.push_back(seat);
		const auto gun_handle = world.registry.spawn(1, gun);
		CHECK(gun_handle.valid());
		if (!gun_handle.valid()) return 1;
		// Each class init binds its own slot at the spawn
		// [orig: WeaponSlot_InitFromEntityDef @0x5466C0].
		CHECK(world.vehicles.init_weapon_slot_from_entity_def(*world.registry.get(hull_handle)));
		CHECK(world.vehicles.init_weapon_slot_from_entity_def(*world.registry.get(gun_handle)));
		// Single player is out of a session, where the UseGun attach refuses a
		// player holding no EquippedSlot (D-NET-446): the gunner's spawn select
		// equips its rifle, as the boot's Player_InitPlayer leg does.
		// [orig: Entity_AttachToUseGunSlot @0x546BF6..0x546C0D;
		//  Player_InitPlayer -> Player_SelectWeaponSlot @0x4E1995]
		{
			w::WeaponTableEntry rifle;
			rifle.name = "WPN_M4AUTO";
			rifle.valid = true;
			rifle.category = 3;
			rifle.clipsize = 30;
			rifle.startrounds = 210;
			world.tables.weapons.entries.push_back(rifle);
			kernel.local.loadout.spawn_kit() = {w::WeaponKitEntry{"WPN_M4AUTO", -1, -1, -1}};
			w::local_loadout_rebuild(world, kernel.local.loadout, kernel.local.weapon,
					kernel.local.inventory, kernel.local.inventory_valid,
					/*select_spawn_default=*/true);
			CHECK(kernel.local.equipped_slot_held());
		}
		CHECK(world.vehicles.process_attach(world.cached.local_player, gun_handle, 1));
		auto &live_gun = *world.registry.get(gun_handle);
		auto &live_hull = *world.registry.get(hull_handle);
		live_gun.primary_weapon_slot.clip = 0;
		live_gun.primary_weapon_slot.reserve = 37;
		live_hull.primary_weapon_slot.clip = 123;
		live_hull.primary_weapon_slot.reserve = 321;
		// The toggle moves the route bit (EquippedSlot) only, never the AdmDef
		// byte [orig: NapiNPServerMsg_HandleWeaponToggle @0x511AFB..0x511B38].
		const auto adm_before = kernel.local.player()->equipped_adm_index;
		for (bool parent : {true, false, true, false}) {
			MountedWeaponSlotSelection request;
			request.use_parent_slot = parent;
			role.state.host_loop.client_send(c2s::MOUNTED_WEAPON_SLOT_SELECT,
					encode_mounted_weapon_slot_selection(request));
			role.drain_host_client_gameplay_requests();
			CHECK(role.state.host_loop.c2s_pending() == 0);
			CHECK(live_gun.primary_weapon_slot.redirect_to_parent_slot == parent);
			const auto *selected = world.vehicles.resolve_mounted_ammo_slot(live_gun);
			CHECK(selected == (parent ? &live_hull.primary_weapon_slot : &live_gun.primary_weapon_slot));
			CHECK(kernel.local.player()->equipped_adm_index == adm_before);
			CHECK(live_gun.primary_weapon_slot.clip == 0 && live_gun.primary_weapon_slot.reserve == 37);
			CHECK(live_hull.primary_weapon_slot.clip == 123 && live_hull.primary_weapon_slot.reserve == 321);
		}
	}

	// --- the shell's single player takes the out-of-session rules word ------
	check_shell_single_player_rules_word(files);

	// --- the dedicated (HostOnly) bring-up -----------------------------------
	{
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &kernel = *kernel_storage;
		auto role_storage = std::make_unique<inmatch::HostRole>();
		inmatch::HostRole &role = *role_storage;
		role.bind(kernel);
		inmatch::ListenHostState &host = role.state;
		kernel.open_document(two_entity_mission(), "synth", source_over(&files));
		ms::KernelBootOptions options;
		options.playable = false;
        options.mp_session = true; // a dedicated host has no player of its own
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] {
			inmatch::HostConfig cfg;
			cfg.protocol = &kJo;
			cfg.config.server_name = "listen_host_test";
			cfg.config.max_players = 4;
			cfg.config.game_type = options.game_type;
			cfg.config.unlimited_vehicles = false; // game.cfg unlimited_vehicles = 0
			cfg.socket_mode = inmatch::SocketMode::Lan;
			cfg.serve_and_play = false; // SERVERTYPE serve-only: no local player
			inmatch::HostBringup bringup;
			bringup.host_cfg = cfg;
			role.bring_up(bringup);
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(error.empty());

		// A host config without unlimited vehicles removes destroyed hulls.
		CHECK(!host.host_owner.ctx.config.unlimited_vehicles);
		CHECK(!kernel.world.rules.vehicle_respawns);
		CHECK(!host.host_owner.serve_and_play);
		// The loopback channel exists but no local client registers on it.
		CHECK(host.host_owner.host_loopback == &host.host_loop);
		CHECK(host.client_runtime == nullptr);
		CHECK(host.host_owner.ctx.world == &kernel.world);
		CHECK(host.host_owner.ctx.mission == &kernel.mission);
		CHECK(host.host_owner.ctx.config.server_name == "listen_host_test");
		CHECK(host.host_owner.ctx.config.max_players == 4u);
		CHECK(local_loopback_connections(host) == 0);
		CHECK(host.host_owner.ctx.np_protocol.connection_list.empty());
		// Its mission start's round init spawned only its connectionless row's hidden body,
		// which Player_InitPlayer takes for the local player (D-NET-352, D-NET-1233).
		const auto row_is_local = [&] {
			const w::Entity *row = kernel.world.registry.get(kernel.world.cached.local_player);
			return host.host_owner.ctx.host_slot.body.valid() &&
					kernel.world.cached.local_player == host.host_owner.ctx.host_slot.body &&
					row != nullptr && (row->flags & w::kEntityFlagCarried) != 0;
		};
		CHECK(row_is_local());

		// The same frame drives a dedicated host: the logic clock and the
		// owner tick advance together, and the drain has no local connection
		// to serve.
		opennova::inmatch::NullDatagramSocket socket;
		const uint32_t tick0 = kernel.world.logic_tick;
		const uint32_t now0 = host.host_owner.now_tick;
		for (int i = 0; i < 4; ++i)
			role.run_tick(tick_input(0));
		CHECK(kernel.world.logic_tick == tick0 + 4);
		CHECK(host.host_owner.now_tick == now0 + 4);
		// A network host is in the session (the host launch passes type 1..3).
		// [orig: UI_HandleHostSessionStart -> CNapiNetwork_SetNetworkType(2)
		//  @0x556e83; the +0x58 store @0x4c4a85]
		CHECK(kernel.local.view_session_inputs.in_session);
		role.drain_host_client_gameplay_requests();
		CHECK(host.host_loop.c2s_pending() == 0);
		CHECK(row_is_local());

		// Its HUD rows pass the dispatcher's head with no session player of ours: the non-peer
		// authority's operand is the constant 2, read without the local entity, so help
		// (row 106, flags 0x05000800), lockgame (84) and playerlist_alt (63) pass and a
		// bit-0 row (dotsize, 38, 0x0C000C01) drops by the operand.
		// [orig: Input_HandleActionBinding @0x49AD40 - the 2 @0x49AD52..0x49AD74; bit 0
		//  with the operand @0x49AD95..0x49ADA0]
		CHECK(role.action_head_admits(8));
		CHECK(role.action_head_admits(119));
		CHECK(role.action_head_admits(102));
		CHECK(!role.action_head_admits(216));
	}

    // The shared deployment transaction resets local view state immediately,
    // even when the host never presents an intervening dead frame.
    {
        auto kernel_storage = std::make_unique<ms::MissionKernel>();
        ms::MissionKernel &kernel = *kernel_storage;
        auto role_storage = std::make_unique<inmatch::HostRole>();
        inmatch::HostRole &role = *role_storage;
        role.bind(kernel);
        kernel.open_document(two_entity_mission(), "respawn", source_over(&files));
        ms::KernelBootOptions options;
        options.game_type = mission_game_type(kernel.mission);
        options.bringup_net_session = [&] { role.bring_up_singleplayer(*opennova::title_by_code("jo")); };
        std::string error;
        CHECK(kernel.boot(options, error));
        auto &lp = kernel.local;
        auto &owner = role.state.host_owner;
        auto &conn = owner.ctx.np_protocol.connection_list.front();
        CHECK(lp.request_stance(2));
        lp.input.look_pitch = 12345;
        lp.input.forward = true;
        lp.view.binoculars_requested = lp.view.binoculars_raised = true;
        lp.view.binoculars_view_active = true;
        lp.view.scope_engaged = true;
        lp.view.scope_settled = true;
        lp.view.nvg_active = true;
        lp.view.nvg_gain = 7;
        lp.view.shake = {30, 111, 222, 333};
        lp.view.camera_mode = 4;
        lp.view.lookahead_q16[0] = 700;
        lp.hud_map_control.mode = 3;
        lp.hud_map_control.zoom_q16 = 32768;
        lp.hud_map_control.big_zoom_q16 = 262144;
        lp.weapon.power_throw_start_tick = 99;
        kernel.world.weather.core.hit_dim.arm(true, false);
        kernel.world.script.waypoints.current = 3;
        role.state.client_runtime->state().local_medic_reviving = true;
        const auto revision = lp.round_reset_revision;
        const auto resets = kernel.world.out.effects.count("local_round_reset");
        lp.player()->alive = false;
        lp.player()->health = 0;
        lp.player()->flags |= w::kEntityFlagDead;
        const auto replies = inmatch::Server_ReleasePlayerDeployment(
                owner.ctx.config, conn, kernel.world, {}, 0xFFFF, nullptr,
                owner.ctx.np_protocol.identity);
        CHECK(!replies.empty());
        CHECK(lp.player()->alive && lp.player()->health > 0);
        CHECK(lp.round_reset_revision == revision + 1);
        CHECK(kernel.world.out.effects.count("local_round_reset") == resets + 1);
        CHECK(!lp.view.binoculars_requested && !lp.view.binoculars_raised &&
                !lp.view.binoculars_view_active && !lp.view.scope_engaged &&
                !lp.view.scope_settled);
        CHECK(lp.stance_latch() == 0 && !lp.input.prone && !lp.input.crouch);
        // The look yaw is the player's +0x10 word the deploy placed (D-NET-376)
        // [orig: Game_InitNewRound @0x4227E3].
        CHECK(lp.input.look_heading ==
                kernel.world.ai.for_handle(lp.player()->handle)->heading);
        CHECK(lp.input.look_pitch == 12345 && lp.input.forward);
        CHECK(lp.view.shake.counter == 0 && lp.view.shake.roll == 111 &&
                lp.view.shake.pitch == 222 && lp.view.shake.yaw == 333);
        CHECK(lp.view.nvg_active && lp.view.nvg_gain == 7);
        CHECK(lp.view.camera_mode == 0 && lp.view.tp_anchor_valid && lp.view.lookahead_q16[0] == 0);
        CHECK(lp.hud_map_control.mode == 0 && lp.hud_map_control.zoom_q16 == 32768 &&
                lp.hud_map_control.big_zoom_q16 == 262144);
        CHECK(lp.weapon.power_throw_start_tick == 0);
        CHECK(kernel.world.weather.core.hit_dim.intensity == 0 &&
                kernel.world.weather.core.hit_dim.fade_rate == 0);
        CHECK(kernel.world.script.waypoints.current == -1);
        role.run_tick(tick_input(0));
        CHECK(!role.state.client_runtime->state().local_medic_reviving);
        CHECK(lp.round_reset_revision == revision + 1);

        // A remote player's deployment cannot reset the local camera or map.
        inmatch::NapiNPConnection remote;
        remote.link.owned_entity = w::EntityHandle::make(0, 0);
        CHECK(remote.link.owned_entity != kernel.world.cached.local_player);
        lp.hud_map_control.mode = 2;
        inmatch::Server_ReleasePlayerDeployment(owner.ctx.config, remote, kernel.world, {}, 0xFFFF,
                nullptr, owner.ctx.np_protocol.identity);
        CHECK(lp.hud_map_control.mode == 2 && lp.round_reset_revision == revision + 1);
    }

	// --- the frame's weapon-action walk pumps the NPC gunners ----------------
	// Every role frame runs the walk after its weather and view legs, so an NPC
	// gunner's queued FIRE on its emplacement leaves as a round within the
	// frame: on the listen host, on a dedicated host (no local player) and in
	// the bare local role alike. The entity pass itself no longer pumps.
	// [orig: Game_ProcessMainFrame -- Camera_ComputeThirdPersonView @0x526781,
	//  then the WeaponAction_ProcessAllEntities call @0x526786;
	//  WeaponAction_ProcessAllEntities @0x5426A6..0x5426C9]
	for (const int frame_kind : {0, 1, 2}) {
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		auto &kernel = *kernel_storage;
		auto host_storage = std::make_unique<inmatch::HostRole>();
		auto local_storage = std::make_unique<inmatch::LocalRole>();
		inmatch::Role &role = frame_kind == 2
				? static_cast<inmatch::Role &>(*local_storage)
				: static_cast<inmatch::Role &>(*host_storage);
		role.bind(kernel);
		kernel.open_document(two_entity_mission(), "gunner_walk", source_over(&files));
		ms::KernelBootOptions options;
		options.game_type = mission_game_type(kernel.mission);
		if (frame_kind == 0) {
			options.bringup_net_session = [&] { host_storage->bring_up_singleplayer(*opennova::title_by_code("jo")); };
		} else if (frame_kind == 1) {
			options.playable = false;
			options.mp_session = true;
			options.bringup_net_session = [&] {
				inmatch::HostConfig cfg;
				cfg.protocol = &kJo;
				cfg.config.server_name = "gunner_walk";
				cfg.config.max_players = 4;
				cfg.config.game_type = options.game_type;
				cfg.socket_mode = inmatch::SocketMode::Lan;
				inmatch::HostBringup bringup;
				bringup.host_cfg = cfg;
				host_storage->bring_up(bringup);
			};
		}
		std::string error;
		CHECK(kernel.boot(options, error));
		auto &world = kernel.world;
		w::EntityHandle npc;
		for (size_t slot = 0; slot < world.registry.pool_capacity(0) && !npc.valid(); ++slot) {
			const w::Entity *row =
					world.registry.get(w::EntityHandle::make(0, static_cast<int>(slot)));
			if (row != nullptr && row->handle != world.cached.local_player &&
					world.ai.for_handle(row->handle) != nullptr)
				npc = row->handle;
		}
		CHECK(npc.valid());
		if (!npc.valid()) return 1;
		world.tables.ammo.entries.resize(2);
		world.tables.ammo.entries[1].velocity = 800;
		world.tables.ammo.entries[1].max_age_ticks = 124;
		world.tables.ammo.entries[1].min_damage = 10;
		world.tables.ammo.entries[1].max_damage = 40;
		world.tables.ammo.entries[1].valid = true;
		w::WeaponTableEntry def;
		def.name = "WPN_WALK_TEST";
		def.valid = true;
		def.ammo_index = 1;
		def.clipsize = -1;
		def.action_fsm.clip_capacity = -1;
		for (int action = 0; action < w::weapon_action::kCount; ++action)
			def.action_fsm.actions[action].id = action;
		world.tables.weapons.entries.push_back(def);
		w::Entity gun;
		gun.kind = w::EntityKind::Item;
		gun.has_item_def = true;
		gun.item_type = 6;
		gun.item_attrib = w::kItemAttribEweap;
		gun.primary_weapon = "WPN_WALK_TEST";
		w::Seat seat;
		seat.type = w::SeatType::Gunner;
		seat.bone_index = 1;
		gun.seats.push_back(seat);
		const auto gun_handle = world.registry.spawn(1, gun);
		CHECK(gun_handle.valid());
		if (!gun_handle.valid()) return 1;
		// The gun's class init binds its slot at the spawn
		// [orig: WeaponSlot_InitFromEntityDef @0x5466C0].
		CHECK(world.vehicles.init_weapon_slot_from_entity_def(*world.registry.get(gun_handle)));
		CHECK(world.vehicles.process_attach(npc, gun_handle, 1));
		// The walk visits only rows with a resolved def row (ItemTypeIndex != 0); the
		// synthetic mission's NPC has none of its own.
		// [orig: WeaponAction_ProcessAllEntities @0x542690 (@0x5426B9)]
		world.registry.get(npc)->item_type_index = 1;
		w::Entity &live_gun = *world.registry.get(gun_handle);
		CHECK(live_gun.primary_weapon_owner == npc);
		live_gun.primary_weapon_slot.next = w::weapon_action::kFire;
		const int rounds_before = world.out.rounds.count;
		role.run_tick(tick_input(0));
		CHECK(live_gun.primary_weapon_slot.current == w::weapon_action::kFire);
		CHECK(world.out.rounds.count == rounds_before + 1);
		const int32_t last = (world.out.rounds.cursor + w::RoundRing::kCapacity - 1) %
				w::RoundRing::kCapacity;
		CHECK(world.out.rounds.records[static_cast<size_t>(last)].shooter_handle == npc.packed);

		check_gun_reload_relay(role, frame_kind == 2 ? nullptr : host_storage.get(), world,
				gun_handle, frame_kind);
	}

	// --- the authority-local fire gate on an MP listen host ------------------
	// The host's own client takes its looped-back tick seed, and its own shot
	// reaches the server only while its own slot is active against that clock:
	// refused on the frame the seed lands (tick == floor), then admitted; a
	// disarm opens the 3-period grace, then refuses; a zero seed parks the
	// clock and refuses. Single player has no gate.
	// [orig: NapiNPClientMsg_HandleTickSeed @0x4297c0;
	//  Entity_FireWeaponAndSendPacket @0x42be12..0x42be44 -> PlayerSlot_IsActive
	//  @0x4FC760; Server_SendRandomSeedToPlayer @0x5101A0]
	{
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &kernel = *kernel_storage;
		auto role_storage = std::make_unique<inmatch::HostRole>();
		inmatch::HostRole &role = *role_storage;
		role.bind(kernel);
		inmatch::ListenHostState &host = role.state;
		kernel.open_document(two_entity_mission(), "fire_gate", source_over(&files));
		ms::KernelBootOptions options;
		options.mp_session = true;
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] {
			inmatch::HostConfig cfg;
			cfg.protocol = &kJo;
			cfg.config.server_name = "fire_gate";
			cfg.config.max_players = 4;
			cfg.config.game_type = options.game_type;
			cfg.socket_mode = inmatch::SocketMode::Lan;
			cfg.serve_and_play = true; // a listen host: HostClient + its own player
			inmatch::HostBringup bringup;
			bringup.host_cfg = cfg;
			role.bring_up(bringup);
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(host.host_owner.serve_and_play && host.client_runtime != nullptr);
		CHECK(kernel.local.has_local_player());
		opennova::inmatch::NullDatagramSocket socket;
		role.set_socket(&socket);
		inmatch::NapiNPConnection *own = nullptr;
		for (inmatch::NapiNPConnection &conn : host.host_owner.ctx.np_protocol.connection_list)
			if (conn.link.owned_entity == kernel.world.cached.local_player) own = &conn;
		CHECK(own != nullptr);
		if (own != nullptr && host.client_runtime != nullptr) {
			// The own client's C2S 0x0B, queued at the mission start's end, joins it in
			// the first frame's receive pump, whose seed rode the loopback to the host's
			// own client (D-NET-884).
			CHECK(!own->fire_tick_mode);
			role.run_tick(tick_input(0));
			CHECK(own->fire_tick_mode && own->tick_seed != 0);
			CHECK(host.client_runtime->current_tick() != 0);
			for (int i = 0; i < 3; ++i) role.run_tick(tick_input(0));
			CHECK(host.client_runtime->current_tick() > own->fire_tick_floor);
			CHECK(role.local_fire_admitted());
			CHECK(kernel.local.authority_fire_admitted);
			// The tick on the floor is refused.
			const uint32_t floor = own->fire_tick_floor;
			own->fire_tick_floor = host.client_runtime->current_tick();
			CHECK(!role.local_fire_admitted());
			own->fire_tick_floor = floor;
			// A disarm: the grace admits, then refuses past three periods.
			// The grace runs on the server frame clock.
			(void)inmatch::Server_DisarmPlayerTickSeed(*own, kernel.world.out.rounds.frame_clock);
			CHECK(role.local_fire_admitted());
			const uint32_t holdoff = host.host_owner.ctx.config.effective_send_holdoff_ticks();
			kernel.world.out.rounds.frame_clock += 3 * holdoff;
			CHECK(!role.local_fire_admitted());
			role.run_tick(tick_input(0));
			CHECK(!kernel.local.authority_fire_admitted);
			// A re-arm admits again; the disarm's zero seed then parks the
			// host client's clock and refuses.
			(void)inmatch::Server_RerollPlayerTickSeed(*own, kernel.world.out.rounds.frame_clock);
			own->fire_tick_floor = 1;
			CHECK(role.local_fire_admitted());
			host.host_loop.host_send(s2c::TICK_SEED, {0, 0, 0, 0});
			role.run_tick(tick_input(0));
			CHECK(host.client_runtime->current_tick() == 0);
			CHECK(!role.local_fire_admitted());
		}
	}

	// --- the listen host's own Respawn key rides its looped-back C2S 0x41 ----
	// A session peer's key queues the bodiless 0x41 on its own connection: the
	// press frame's client frame flushes it, the next frame's drain hands it to
	// the server's handler for the host's own slot (its body's Health zeroed,
	// the respawn holds stored). Out of a session the key queues nothing.
	// [orig: Input_HandleActionBinding case 55 @0x49B20B..0x49B22A ->
	//  NetPacket_SendRequestRespawn @0x42DBA0; NapiNPServerMsg_DeathTimeoutReset
	//  @0x510540]
	for (const bool mp : {true, false}) {
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &kernel = *kernel_storage;
		auto role_storage = std::make_unique<inmatch::HostRole>();
		inmatch::HostRole &role = *role_storage;
		role.bind(kernel);
		inmatch::ListenHostState &host = role.state;
		kernel.open_document(two_entity_mission(), "respawn_key", source_over(&files));
		ms::KernelBootOptions options;
		options.mp_session = mp;
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] {
			if (!mp) {
				role.bring_up_singleplayer(*opennova::title_by_code("jo"));
				return;
			}
			inmatch::HostConfig cfg;
			cfg.protocol = &kJo;
			cfg.config.server_name = "respawn_key";
			cfg.config.max_players = 4;
			cfg.config.game_type = options.game_type;
			cfg.config.respawn_timeout = 7;
			cfg.socket_mode = inmatch::SocketMode::Lan;
			cfg.serve_and_play = true;
			inmatch::HostBringup bringup;
			bringup.host_cfg = cfg;
			role.bring_up(bringup);
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(host.client_runtime != nullptr && kernel.local.has_local_player());
		opennova::inmatch::NullDatagramSocket socket;
		role.set_socket(&socket);
		for (int i = 0; i < 4; ++i) role.run_tick(tick_input(0));
		inmatch::NapiNPConnection *own = nullptr;
		for (inmatch::NapiNPConnection &conn : host.host_owner.ctx.np_protocol.connection_list)
			if (conn.link.owned_entity == kernel.world.cached.local_player) own = &conn;
		w::Entity *body = kernel.local.player();
		CHECK(own != nullptr && body != nullptr);
		if (own == nullptr || body == nullptr) continue;
		CHECK(body->health > 0 && !kernel.local.local_player_dead());
		inmatch::TickInput press = tick_input(0);
		(void)role.input_events().push(opennova::controls::InputEventRecord{55, 0, 0, 0, false});
		role.apply_input(press);
		role.run_tick(tick_input(0));
		CHECK(body->health > 0); // the press frame only flushed it
		role.run_tick(tick_input(0));
		if (mp) {
			CHECK(body->health <= 0 && kernel.local.local_player_dead());
			CHECK(own->link.respawn_delay_seconds > 0);
		} else {
			CHECK(body->health > 0 && !kernel.local.local_player_dead());
		}
	}

	check_listen_host_deploy_convert(files);
	check_listen_host_own_deploy_reads_its_protocol(files);
	check_head_preround_reads_the_own_clients_copy(files);
	check_listen_host_console_punts_only_through_server(files, "PUNT ALL", /*punts=*/false);
	check_listen_host_console_punts_only_through_server(files, "SERVER PUNT ALL", /*punts=*/true);
	check_single_player_console_line_rides_the_loopback(files);
	check_server_rows_reach_their_arms(files);

	// --- the authority's own stance rides its looped-back C2S 0x1D ----------
	// The server handler latches the host's own player from the 0x1D its own
	// client sent: the datagram the press frame's client frame flushed is read
	// at the next frame's head, so the stance lands in that frame's server
	// tick, ahead of its body update.
	// [orig: Game_ProcessMainFrame -- CNapiNetwork_PumpManagerReceive @0x526528,
	//  Client_ProcessNetworkFrame @0x526692, Server_TickUpdate @0x5266b6 ->
	//  CNapiNetwork_PumpServerProtocolRecv @0x51d895;
	//  NapiNPServerMsg_HandleStanceChange @0x501c60, latches @0x501d1b / @0x501d2d]
	{
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &kernel = *kernel_storage;
		auto role_storage = std::make_unique<inmatch::HostRole>();
		inmatch::HostRole &role = *role_storage;
		role.bind(kernel);
		inmatch::ListenHostState &host = role.state;
		kernel.open_document(two_entity_mission(), "stance", source_over(&files));
		ms::KernelBootOptions options;
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] { role.bring_up_singleplayer(*opennova::title_by_code("jo")); };
		std::string error;
		CHECK(kernel.boot(options, error));
		opennova::inmatch::NullDatagramSocket socket;
		role.set_socket(&socket);
		for (int i = 0; i < 4; ++i) role.run_tick(tick_input(0));
		auto &lp = kernel.local;
		const auto body_stance = [&] {
			const w::AiEntity *body = lp.player_ai();
			return body != nullptr ? body->inf.stance : w::InfantryState::Stance::kStand;
		};
		CHECK(lp.has_local_player() && lp.stance_latch() == 0);
		// A looped-back crouch (action 169) reaches the dispatcher and latches.
		host.host_loop.client_send(c2s::STANCE_CHANGE, std::vector<uint8_t>{0xA9, 0x00});
		role.run_tick(tick_input(0));
		CHECK(lp.stance_latch() == 1 && lp.input.crouch && !lp.input.prone);
		CHECK(body_stance() == w::InfantryState::Stance::kCrouch);
		CHECK(lp.player() != nullptr && lp.player()->net_stance_bits == 2);
		// The key only queues the 0x1D (action 170, prone) on the host's own
		// client: the press frame's body stays crouched, its client frame
		// flushes the datagram, and the next frame's server tick latches it.
		// [orig: Input_HandleActionBinding_0 case 170 @0x4e0df3 ->
		//  CNapiNetwork_QueueReliableMessage, no latch write]
		CHECK(role.request_stance(2));
		CHECK(lp.stance_latch() == 1);
		CHECK(host.host_loop.c2s_pending() == 0);
		role.run_tick(tick_input(0));
		CHECK(lp.stance_latch() == 1 && body_stance() == w::InfantryState::Stance::kCrouch);
		CHECK(host.host_loop.c2s_pending() == 1);
		role.run_tick(tick_input(0));
		CHECK(lp.stance_latch() == 2 && lp.input.prone && !lp.input.crouch);
		CHECK(body_stance() == w::InfantryState::Stance::kProne);
		CHECK(host.host_loop.c2s_pending() == 0);
		// Every press sends, the selected stance included; the stand (172)
		// then clears both bits the same way.
		CHECK(role.request_stance(2));
		role.run_tick(tick_input(0));
		role.run_tick(tick_input(0));
		CHECK(lp.stance_latch() == 2);
		CHECK(role.request_stance(0));
		role.run_tick(tick_input(0));
		CHECK(lp.stance_latch() == 2);
		role.run_tick(tick_input(0));
		CHECK(lp.stance_latch() == 0 && !lp.input.prone && !lp.input.crouch);
		CHECK(body_stance() == w::InfantryState::Stance::kStand);
		// A stance request the sim raised (a ForceCrouch def's mount request: the
		// crouch's 0xA9) rides the same path, queued on the host's own client within the
		// frame that raised it, past none of the key's gates.
		// [orig: Player_MountWeaponSlot @0x4DFA40 - CNapiNetwork_QueueReliableMessage(0x1D,
		//  1, 0) @0x4DFC30]
		kernel.world.out.stance_requests.push_back(0xA9);
		role.run_tick(tick_input(0));
		CHECK(kernel.world.out.stance_requests.empty());
		CHECK(lp.stance_latch() == 0);
		CHECK(host.host_loop.c2s_pending() == 1);
		role.run_tick(tick_input(0));
		CHECK(lp.stance_latch() == 1 && lp.input.crouch && !lp.input.prone);
		CHECK(body_stance() == w::InfantryState::Stance::kCrouch);
		CHECK(role.request_stance(0));
		role.run_tick(tick_input(0));
		role.run_tick(tick_input(0));
		CHECK(lp.stance_latch() == 0 && body_stance() == w::InfantryState::Stance::kStand);
		// The host's chat line, emote and radio call take the same path as
		// its 0x1D: queued on its own client connection at the press, sent by
		// that frame's client frame, dispatched by the next frame's server
		// tick. [orig: Chat_SendGlobalMessage QueueReliableMessage(0xD) @0x49a9b4;
		//  NetPacket_SendEmoteRequest @0x42c147 (0x14); NetPacket_SendRadioCallRequest
		//  @0x42c177 (0x13); the manager FIFO drained @0x526528]
		inmatch::ClientRuntime &own = *host.client_runtime;
		std::string line = "on me";
		CHECK(own.queue_chat_message(1, line, 0) == hud::ChatSendResult::Sent);
		CHECK(own.queue_voice_menu_pick(c2s::EMOTE_REQUEST, 3));
		CHECK(own.queue_voice_menu_pick(c2s::RADIO_CALL_REQUEST, 5));
		CHECK(host.host_loop.c2s_pending() == 0);
		role.run_tick(tick_input(0));
		CHECK(host.host_loop.c2s_pending() == 3);
		role.run_tick(tick_input(0));
		CHECK(host.host_loop.c2s_pending() == 0);
		// So do its medic call, its team change and its command-map squad
		// sends: each sender queues on the local connection with no authority
		// branch. [orig: action 217 -> QueueReliableMessage(0x2E) @0x49b50c;
		//  DeathScreen_OnSwapTeams @0x5535ba -> 0x4D @0x42ddac; the squad
		//  senders 0x17 @0x42ddf2, 0x43 @0x42dc37, 0x44 @0x42dcac, 0x45 @0x42dcf7,
		//  0x46 @0x42dd2f, 0x4B @0x42dd7f, 0x4F @0x42de2d, 0x3F @0x5488b9]
		CHECK(role.send_medic_request());
		CHECK(own.queue_team_change_request());
		FireteamAssign assign;
		assign.fireteam = 1;
		CHECK(own.queue_squad_message(c2s::FIRETEAM_ASSIGN, encode_fireteam_assign(assign)));
		CHECK(host.host_loop.c2s_pending() == 0);
		role.run_tick(tick_input(0));
		CHECK(host.host_loop.c2s_pending() == 3);
		role.run_tick(tick_input(0));
		// Dispatched; what remains is the handlers' own follow-up traffic.
		replication::Datagram left;
		while (host.host_loop.host_recv(left))
			CHECK(left.tag != c2s::MEDIC_REQUEST && left.tag != c2s::TEAM_CHANGE_REQUEST &&
					left.tag != c2s::FIRETEAM_ASSIGN);
	}

	// --- the listen host's own player holds at the join and deploys by its pick
	// With spawn zones, the host's own player joins RESPAWN-PENDING and hidden
	// like any joiner: its own client folds the per-frame 0x0A overlay bit, the
	// frame loop's DEATH trigger reads it, and its Default Spawn pick rides its
	// loopback as the C2S 0x0E the server's respawn handler releases. The single
	// player holds the same way: the tick's tail slot walk writes its own 0x0A
	// out of a session too. (D-NET-339)
	// [orig: Game_StartMission @0x52624B (C2S 0x0B) -> Server_OnPlayerJoin
	//  @0x51a6f2; Server_TickUpdate @0x51E385..0x51E46D (the tail walk, outside
	//  the session blocks); Input_HandleActionBinding case 12 @0x49b17b;
	//  Render_ProcessMainSceneFrame @0x5CAC81]
	for (const bool mp_session : {true, false}) {
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &kernel = *kernel_storage;
		auto role_storage = std::make_unique<inmatch::HostRole>();
		inmatch::HostRole &role = *role_storage;
		role.bind(kernel);
		inmatch::ListenHostState &host = role.state;
		kernel.open_document(two_entity_mission(), "deploy_hold", source_over(&files));
		ms::KernelBootOptions options;
		options.mp_session = mp_session;
		options.game_type = mission_game_type(kernel.mission);
		bool zone_marked = false;
		options.bringup_net_session = [&] {
			// The placed item stands in for an authored spawn zone (a def with
			// attrib 0x40000), present before the host's own player joins.
			w::EntityHandle placed;
			kernel.world.registry.for_each([&](const w::Entity &e) {
				if (e.net_id == 21) placed = e.handle;
			});
			if (w::Entity *zone = kernel.world.registry.get(placed)) {
				zone->has_item_def = true;
				zone->item_attrib |= w::kItemAttribSpawnPoint;
				zone->is_spawn_point = true;
				zone_marked = true;
			}
			if (!mp_session) {
				role.bring_up_singleplayer(*opennova::title_by_code("jo"));
				return;
			}
			inmatch::HostConfig cfg;
			cfg.protocol = &kJo;
			cfg.config.server_name = "deploy_hold";
			cfg.config.max_players = 4;
			cfg.config.game_type = options.game_type;
			cfg.socket_mode = inmatch::SocketMode::Lan;
			cfg.serve_and_play = true;
			inmatch::HostBringup bringup;
			bringup.host_cfg = cfg;
			role.bring_up(bringup);
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(zone_marked && kernel.world.zones.has_spawn_zone());
		opennova::inmatch::NullDatagramSocket socket;
		role.set_socket(&socket);
		inmatch::NapiNPConnection *own = nullptr;
		for (inmatch::NapiNPConnection &conn : host.host_owner.ctx.np_protocol.connection_list)
			if (conn.link.owned_entity == kernel.world.cached.local_player) own = &conn;
		CHECK(own != nullptr && host.client_runtime != nullptr);
		if (own == nullptr || host.client_runtime == nullptr) continue;
		const w::Entity *player = kernel.world.registry.get(own->link.owned_entity);
		CHECK(player != nullptr);
		if (player == nullptr) continue;
		inmatch::RoleView view;
		view.kernel = &kernel;
		view.runtime = host.client_runtime.get();
		// The trigger's dead-bit arm: in a session, or out of one only under
		// the single-player respawn attribute [orig: @0x5CAB39..0x5CAB58].
		auto dead_triggers = [&] {
			w::Entity *body = kernel.world.registry.get(own->link.owned_entity);
			const uint32_t flags = body->flags;
			body->flags |= w::kEntityFlagDead;
			const bool triggered = inmatch::death_menu_triggered(view);
			body->flags = flags;
			return triggered;
		};
		// The add hid the held body; the own client's C2S 0x0B joins it held in the first
		// frame's receive pump (D-NET-884), the single player's as a listen host's: its own
		// 0x0A carries the overlay bit and re-sets the hide out of a session too.
		// [orig: Entity_SpawnFromAnimSlotProperty @0x43C508; Game_StartMission @0x525C9E;
		//  NetPacket_WritePlayerState @0x4FF7B8..0x4FF7BC]
		CHECK(!own->link.respawn_pending && (player->flags & 1u) != 0);
		for (int i = 0; i < 3; ++i) role.run_tick(tick_input(0));
		CHECK(own->link.respawn_pending && (player->flags & 1u) != 0);
		CHECK(host.client_runtime->state().deploy_overlay_active);
		CHECK(inmatch::death_menu_triggered(view));
		CHECK(host.client_runtime->state().take_death_menu_open(
				inmatch::death_menu_triggered(view), /*menu_open=*/false));
		// The pick queues on the host's own client, leaves with its client
		// frame, and the next server tick releases the hold.
		CHECK(host.client_runtime->queue_deployment_pick(0xFFFFu));
		for (int i = 0; i < 3 && own->link.respawn_pending; ++i) role.run_tick(tick_input(0));
		player = kernel.world.registry.get(own->link.owned_entity);
		CHECK(!own->link.respawn_pending && player != nullptr && (player->flags & 1u) == 0);
		role.run_tick(tick_input(0));
		CHECK(!host.client_runtime->state().deploy_overlay_active);
		CHECK(!inmatch::death_menu_triggered(view));
		CHECK(!host.client_runtime->state().take_death_menu_open(false, false));
		CHECK(!host.client_runtime->state().death_menu_open_latch);
		if (mp_session) {
			CHECK(dead_triggers());
			continue;
		}
		CHECK(!dead_triggers());
		kernel.world.tables.mission_attrib_flags |=
				w::MissionTables::kMissionAttribSinglePlayerRespawn;
		CHECK(dead_triggers());
		// The single player's own death under the attribute: no auto-lose,
		// the DEATH trigger, a SPAWNPOINTS_LIST over the zone its own client
		// banked from the loopback, and the zone pick on its own client
		// redeploys the body. [orig: Server_CheckWinConditions @0x51AD68;
		//  Render_ProcessMainSceneFrame @0x5CAB42..0x5CAB8B;
		//  UI_UpdateDeathScreenContent @0x553b53..0x553c4e;
		//  Input_HandleActionBinding case 12 @0x49b17b;
		//  Server_ProcessClientRequestRespawn @0x519AF0]
		w::EntityHandle zone_handle;
		kernel.world.registry.for_each([&](const w::Entity &e) {
			if (e.net_id == 21) zone_handle = e.handle;
		});
		w::Entity *zone_entity = kernel.world.registry.get(zone_handle);
		CHECK(zone_entity != nullptr);
		if (zone_entity == nullptr) continue;
		// An unnumbered zone (control 0) no capture event touches: it has no
		// timer entry, so it lists [orig: UI_UpdateDeathScreenContent
		// @0x553bd3..0x553bd5].
		zone_entity->team = player->team;
		w::Entity *body = kernel.world.registry.get(own->link.owned_entity);
		body->alive = false;
		body->health = 0;
		body->flags |= w::kEntityFlagDead;
		for (int i = 0; i < 16; ++i) role.run_tick(tick_input(0));
		CHECK(kernel.local.local_player_dead());
		CHECK(!kernel.world.match.outcome().ended);
		CHECK(inmatch::death_menu_triggered(view));
		const w::SpawnZoneRegistry zones = kernel.world.zones.build_spawn_zone_list();
		std::vector<w::DeployZoneRow> rows;
		CHECK(inmatch::deploy_zone_rows(view, zones, rows));
		CHECK(rows.size() == 1 && rows[0].index == 0 && rows[0].letter == 'A' &&
				rows[0].secured);
		CHECK(host.client_runtime->queue_deployment_pick(zone_handle.packed));
		for (int i = 0; i < 4 && kernel.local.local_player_dead(); ++i)
			role.run_tick(tick_input(0));
		CHECK(!kernel.local.local_player_dead());
	}

	// --- a DFX2 single player holds at its mission start with no spawn zone ---
	// dfx2.exe's join writes the state byte 0x13 whatever the mission holds, so a
	// single player's own join is RESPAWN-PENDING and its own 0x0A raises the
	// deploy overlay every frame: the DEATH screen opens at the mission start and
	// stays until the Default Spawn pick deploys the body.
	// [orig: Server_OnPlayerJoin @0x517717 (19); NetPacket_WritePlayerState
	//  @0x4fc88a..0x4fc89c; Server_TickUpdate @0x51a810 - the walk's call @0x51b324;
	//  NapiNPClientMsg_0x00A @0x431993 (dfx2.exe)]
	{
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &kernel = *kernel_storage;
		auto role_storage = std::make_unique<inmatch::HostRole>();
		inmatch::HostRole &role = *role_storage;
		role.bind(kernel);
		inmatch::ListenHostState &host = role.state;
		kernel.open_document(two_entity_mission(), "dfx2_sp_hold", source_over(&files));
		ms::KernelBootOptions options;
		options.title_rules = &w::title_rules("dfx2");
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] {
			role.bring_up_singleplayer(*opennova::title_by_code("dfx2"));
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(!kernel.world.zones.has_spawn_zone());
		opennova::inmatch::NullDatagramSocket socket;
		role.set_socket(&socket);
		inmatch::NapiNPConnection *own = nullptr;
		for (inmatch::NapiNPConnection &conn : host.host_owner.ctx.np_protocol.connection_list)
			if (conn.link.owned_entity == kernel.world.cached.local_player) own = &conn;
		CHECK(own != nullptr && host.client_runtime != nullptr);
		if (own != nullptr && host.client_runtime != nullptr) {
			inmatch::RoleView view;
			view.kernel = &kernel;
			view.runtime = host.client_runtime.get();
			for (int i = 0; i < 3; ++i) role.run_tick(tick_input(0));
			CHECK(own->link.respawn_pending);
			// Held across frames, not a one-frame raise the next 0x0A drops.
			for (int i = 0; i < 4; ++i) role.run_tick(tick_input(0));
			CHECK(own->link.respawn_pending);
			CHECK(host.client_runtime->state().deploy_overlay_active);
			CHECK(inmatch::death_menu_triggered(view));
			CHECK(host.client_runtime->queue_deployment_pick(0xFFFFu));
			for (int i = 0; i < 3 && own->link.respawn_pending; ++i) role.run_tick(tick_input(0));
			CHECK(!own->link.respawn_pending);
			role.run_tick(tick_input(0));
			CHECK(!host.client_runtime->state().deploy_overlay_active);
			CHECK(!inmatch::death_menu_triggered(view));
		}
	}

	// --- the objective Co-op mission start out of a session (D-PWR-2) ---------
	// The difficulty word starts every SP mission at 0; the objective Co-op
	// game type 0x30020 (an ATTRIB_COOP mission) also takes the single-player
	// respawn attribute, the word -1 and the local player's health at its
	// doubled ceiling. A training mission (stock Co-op 0x10020) keeps the def
	// hp and no respawn. The SP config's difficulty byte is the word the start
	// leaves. [orig: Game_StartMission @0x525CC2..0x525D12;
	//  Entity_GetMaxHealthWithDifficulty @0x43B8A0]
	for (const bool objective : {true, false}) {
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &kernel = *kernel_storage;
		auto role_storage = std::make_unique<inmatch::HostRole>();
		inmatch::HostRole &role = *role_storage;
		role.bind(kernel);
		bms::File mission = two_entity_mission();
		mission.header.attrib_flags = objective ? bms::AttribFlags::Coop : bms::AttribFlags::None;
		kernel.open_document(std::move(mission), "coop_start", source_over(&files));
		ms::KernelBootOptions options;
		options.game_type = mission_game_type(kernel.mission);
		CHECK(options.game_type == (objective ? game_type::kObjectiveCoop : game_type::kCoop));
		kernel.world.rules.difficulty = 1; // a previous word, never carried
		options.bringup_net_session = [&] {
			kernel.world.tables.player.item_hp = 100; // the Player def's hp
			role.bring_up_singleplayer(*opennova::title_by_code("jo"));
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		const w::Entity *local = kernel.world.registry.get(kernel.world.cached.local_player);
		CHECK(local != nullptr);
		if (local == nullptr) continue;
		CHECK(local->health_max == 100);
		CHECK(kernel.world.rules.difficulty == (objective ? -1 : 0));
		CHECK(((kernel.world.tables.mission_attrib_flags &
		        w::MissionTables::kMissionAttribSinglePlayerRespawn) != 0) == objective);
		CHECK(local->health == (objective ? 200 : 100));
		CHECK(w::local_player_max_health(kernel.world) == (objective ? 200 : 100));
		CHECK(role.state.host_owner.ctx.config.config_bytes[6] == (objective ? 0xFF : 0x00));
	}

	// --- a fresh SP start spawns at the profile's ceiling (D-PWR-6) ---------
	// The session settings copy the profile's +0x564 into the difficulty word
	// ahead of the round init, whose class init and spawn raise the zeroed
	// local player to its ceiling: a training mission (stock Co-op 0x10020)
	// starts at twice the def hp at -1 and half at 1, and keeps it after the
	// mission start sets the word 0. The SP restart's start skips the copy.
	// [orig: Game_ApplySessionSettingsToGlobals @0x551F6F..0x551F75 (called
	//  @0x524662, skipped on the restart @0x52465E); PlayerClass_InitEntity
	//  @0x4B112B; Entity_SpawnFromAnimSlotProperty @0x43C557; Game_StartMission
	//  @0x525CDD]
	for (const int32_t profile_word : {-1, 1}) {
		for (const bool restart : {false, true}) {
			auto kernel_storage = std::make_unique<ms::MissionKernel>();
			ms::MissionKernel &kernel = *kernel_storage;
			auto role_storage = std::make_unique<inmatch::HostRole>();
			inmatch::HostRole &role = *role_storage;
			role.bind(kernel);
			bms::File mission = two_entity_mission();
			mission.header.attrib_flags = bms::AttribFlags::None;
			kernel.open_document(std::move(mission), "profile_start", source_over(&files));
			ms::KernelBootOptions options;
			options.game_type = mission_game_type(kernel.mission);
			CHECK(options.game_type == game_type::kCoop);
			options.restart = restart;
			playersav::ProfileRecord profile;
			profile.sp_difficulty = profile_word;
			int32_t spawn_word = 0;
			options.bringup_net_session = [&] {
				kernel.world.tables.player.item_hp = 100; // the Player def's hp
				role.bring_up_singleplayer(*opennova::title_by_code("jo"), &profile);
				spawn_word = kernel.world.rules.difficulty;
			};
			std::string error;
			CHECK(kernel.boot(options, error));
			const w::Entity *local = kernel.world.registry.get(kernel.world.cached.local_player);
			CHECK(local != nullptr);
			if (local == nullptr) continue;
			CHECK(spawn_word == (restart ? 0 : profile_word));
			CHECK(kernel.world.rules.difficulty == 0);
			CHECK(local->health == (restart ? 100 : profile_word == -1 ? 200 : 50));
			CHECK(w::local_player_max_health(kernel.world) == 100);
		}
	}

	// --- dfx2.exe's mission start sets no difficulty (DFX2-28) ---------------
	// dfx2.exe's start runs no difficulty block: the word stays the profile's
	// copy, on a restart too, so an objective Co-op mission takes neither the -1
	// nor the single-player respawn attribute nor the doubled health, and the SP
	// config's byte is the profile's word.
	// [orig: Game_StartMission @0x521530 - nothing between @0x522EA9 and
	//  @0x522EAE; Game_ApplySessionSettingsToGlobals @0x54F1BF..0x54F1C5 (dfx2.exe)]
	for (const int32_t profile_word : {0, 1}) {
		for (const bool restart : {false, true}) {
			auto kernel_storage = std::make_unique<ms::MissionKernel>();
			ms::MissionKernel &kernel = *kernel_storage;
			auto role_storage = std::make_unique<inmatch::HostRole>();
			inmatch::HostRole &role = *role_storage;
			role.bind(kernel);
			bms::File mission = two_entity_mission();
			mission.header.attrib_flags = bms::AttribFlags::Coop;
			kernel.open_document(std::move(mission), "dfx2_coop_start", source_over(&files));
			ms::KernelBootOptions options;
			options.title_rules = &w::title_rules("dfx2");
			options.game_type = mission_game_type(kernel.mission);
			CHECK(options.game_type == game_type::kObjectiveCoop);
			options.restart = restart;
			playersav::ProfileRecord profile;
			profile.sp_difficulty = profile_word;
			int32_t spawn_word = -2;
			options.bringup_net_session = [&] {
				kernel.world.tables.player.item_hp = 100; // the Player def's hp
				role.bring_up_singleplayer(*opennova::title_by_code("dfx2"), &profile);
				spawn_word = kernel.world.rules.difficulty;
			};
			std::string error;
			CHECK(kernel.boot(options, error));
			const w::Entity *local = kernel.world.registry.get(kernel.world.cached.local_player);
			CHECK(local != nullptr);
			if (local == nullptr) continue;
			CHECK(spawn_word == profile_word);
			CHECK(kernel.world.rules.difficulty == profile_word);
			CHECK((kernel.world.tables.mission_attrib_flags &
			       w::MissionTables::kMissionAttribSinglePlayerRespawn) == 0);
			CHECK(local->health == (profile_word == 1 ? 50 : 100));
			CHECK(w::local_player_max_health(kernel.world) == (profile_word == 1 ? 50 : 100));
			CHECK(role.state.host_owner.ctx.config.config_bytes[6] ==
			      static_cast<uint8_t>(profile_word));
		}
	}

	// --- the listen host's own 0x4C quality report reaches its own slot ------
	// The host's client queues the report on its local connection on its 311th
	// client frame; the next frame's head drain hands it to the server's
	// handler, which stores the host's level on the host's own slot and dirties
	// it, and the 1 Hz walk of a later frame resends that slot's 5-byte 0x46,
	// which the host's own client folds into its roster the same frame. At 62
	// frames a second with no joiner the host's level is 1.
	// [orig: Client_ProcessNetworkFrame @0x42C23E..0x42C279; NapiNPServerMsg_0x04C
	//  @0x5111B0 -> PlayerSlot_SetClientQuality @0x5006E0; Server_TickUpdate
	//  @0x51DEC6..0x51DF10]
	{
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &kernel = *kernel_storage;
		auto role_storage = std::make_unique<inmatch::HostRole>();
		inmatch::HostRole &role = *role_storage;
		role.bind(kernel);
		inmatch::ListenHostState &host = role.state;
		kernel.open_document(two_entity_mission(), "quality_report", source_over(&files));
		ms::KernelBootOptions options;
		options.mp_session = true;
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] {
			inmatch::HostConfig cfg;
			cfg.protocol = &kJo;
			cfg.config.server_name = "quality_report";
			cfg.config.max_players = 4;
			cfg.config.game_type = options.game_type;
			cfg.socket_mode = inmatch::SocketMode::Lan;
			cfg.serve_and_play = true;
			inmatch::HostBringup bringup;
			bringup.host_cfg = cfg;
			role.bring_up(bringup);
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		opennova::inmatch::NullDatagramSocket socket;
		role.set_socket(&socket);
		role.observe_frame_rate(62);
		inmatch::NapiNPConnection *own = nullptr;
		for (inmatch::NapiNPConnection &conn : host.host_owner.ctx.np_protocol.connection_list)
			if (conn.link.owned_entity == kernel.world.cached.local_player) own = &conn;
		CHECK(own != nullptr && host.client_runtime != nullptr);
		if (own != nullptr && host.client_runtime != nullptr) {
			CHECK(own->reply.client_quality == 0 && !own->reply.client_quality_dirty);
			const uint8_t slot = static_cast<uint8_t>(own->reply.player_slot);
			int stored_at = -1;
			int resent_at = -1;
			bool dirty_seen = false;
			bool roster_early = false;
			for (int frame = 1; frame <= 500 && resent_at < 0; ++frame) {
				role.run_tick(tick_input(0));
				const uint8_t roster_quality = host.client_runtime->state().roster[slot].quality;
				if (stored_at < 0 && own->reply.client_quality != 0) stored_at = frame;
				if (stored_at > 0 && own->reply.client_quality_dirty) dirty_seen = true;
				if (stored_at > 0 && !own->reply.client_quality_dirty) resent_at = frame;
				if (roster_quality != 0 && resent_at < 0) roster_early = true;
			}
			std::printf("host_role: the host's 0x4C stored at frame %d, resent at frame %d\n",
					stored_at, resent_at);
			CHECK(stored_at == 312);
			CHECK(resent_at > stored_at && resent_at - stored_at <= 62 && dirty_seen);
			CHECK(!roster_early);
			CHECK(host.host_owner.ctx.net_quality_level == 1);
			CHECK(own->reply.client_quality == host.host_owner.ctx.net_quality_level);
			CHECK(host.client_runtime->state().roster[slot].quality == 1);
		}
	}

	check_quality_report_runs_on(files);
	check_serve_only_steps_quality_count(files);
	check_serve_only_net_quality_indicators(files);

	check_own_world_state_location_names(files);

	// The window's CPU share reaches the server context before the drain, where
	// every tick's 0x0A server-status block copies it (capped in the fan)
	// [orig: Game_MainLoop @0x52B948, the drain @0x52BA08; Server_TickUpdate
	//  @0x51D7EA..0x51D7FB].
	{
		auto role_storage = std::make_unique<inmatch::HostRole>();
		inmatch::HostRole &role = *role_storage;
		role.observe_cpu_share(37);
		CHECK(role.state.host_owner.ctx.stats_cpu_percent == 37);
		role.observe_cpu_share(300);
		CHECK(role.state.host_owner.ctx.stats_cpu_percent == 300);
	}

	if (failures == 0) std::printf("host_role: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
