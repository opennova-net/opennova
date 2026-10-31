// The mission start's Player_InitPlayer body leg on a playing host (L7-s1
// FOUND-1): with spawn zones the host's own player joins held, and the add hid
// its body. Every mission start then calls Player_InitPlayer after the round
// init, which names the body after the local profile and clears its hidden
// bit, and the mission start's own local-player block clears it again, builds
// the proximity lists and sets it again: the body leaves the mission start
// hidden, so no server tick's slot lists count it as a human before the own
// join: the receive pump that hands over the own client's C2S 0x0B (queued at
// the mission start's end, D-NET-884) runs the join, which clears the bit
// and whose own 0x0A writer hides the undeployed body again ahead of the slot
// lists, so that frame holds. The
// 0x0B reaches the server on the first frame: the own connection's period is
// one tick and the mission start's two client receives spend its countdown
// ahead of the 0x0B's send, so a held listen host runs no update before its
// own join's 0x0A (N = 0).
// [orig: NapiNPServer_UpdateHoldoffTicks - the local connection's 1
//  @0x4C5F53..0x4C5F69; CNapiNPConnection_PumpFlags - the step
//  @0x6297A9..0x6297AC; Game_StartMission - the receives @0x525784 /
//  @0x525BD9, the send @0x526255; NetClient_FlushAndSync's authority return
//  @0x424719; CNapiNPConnection_PumpEnumeratorAndSend's gate @0x62927B]
// [orig: Game_StartMission -- the round init @0x525BAF, the Player_InitPlayer
//  call @0x525BBC, the block @0x525C6B..0x525C9E (the re-hide @0x525C9E);
//  Player_InitPlayer @0x4E15F0 -- the Name @0x4E184F..0x4E186A, `Flags &= ~1` @0x4E19C2;
//  Server_BuildEntitySlotLists @0x4F97A0 -- the Player bit @0x4F9815, the
//  hidden bit @0x4F9820, +1 @0x4F98B1, called from Server_TickUpdate @0x51D89A;
//  Game_ProcessMainFrame -- the humans/ticks hold @0x52671C..0x52672A, the
//  Entity_UpdateAllEntities call @0x52674B; NetPacket_WritePlayerState --
//  (+89912 & 0x10) @0x4FF7B8]
#include <runtime/inmatch/host_role.h>
#include <net/protocol/protocol.h>
#include <base/gameprofile/game_type.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/loadout_submit.h> // seed_session_kit_from_profile (the shell seed)
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/null_datagram_socket.h>
#include <net/npwire/ingame_decode.h> // decode_loadout_submit (the own 0x2F pair)
#include <formats/mission/bms.h>
#include <formats/playersav/weapon_sav.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/ai.h>
#include <runtime/world/infantry.h> // IRootMotionSource (the warm-up pin)
#include <runtime/world/player_loadout.h> // local_loadout_kit (the restart's kit)
#include <runtime/world/world.h>

#include "../common/boot_file_source.h"
#include "../common/synthetic_mission.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../common/title_fixture.h"

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

// The shell's single player bring-up record (socketless, playing), over `process` when given.
inmatch::HostBringup single_player_bringup(uint32_t game_type,
		inmatch::ClientProcessState *process = nullptr) {
	inmatch::HostConfig cfg;
	test_titles::jo_config(cfg.config);
	cfg.protocol = &kJo;
	cfg.config.server_name = "SINGLEPLAYERGAME";
	cfg.config.max_players = 1;
	cfg.config.game_type = game_type;
	cfg.socket_mode = inmatch::SocketMode::Socketless;
	cfg.serve_and_play = true;
	cfg.client_process = process;
	inmatch::HostBringup bringup;
	bringup.host_cfg = cfg;
	return bringup;
}

// A Serve Only host's bring-up record (a LAN session, no local player), over `process`.
inmatch::HostBringup serve_only_bringup(uint32_t game_type, inmatch::ClientProcessState *process) {
	inmatch::HostConfig cfg;
	test_titles::jo_config(cfg.config);
	cfg.protocol = &kJo;
	cfg.config.server_name = "serve_only";
	cfg.config.max_players = 4;
	cfg.config.game_type = game_type;
	cfg.socket_mode = inmatch::SocketMode::Lan;
	cfg.serve_and_play = false;
	cfg.client_process = process;
	inmatch::HostBringup bringup;
	bringup.host_cfg = cfg;
	return bringup;
}

// A Serve Only host's mission start over `process`: the boot, the round init's row body (the
// host boot's step) and Player_InitPlayer's legs. Returns the row body's equipped byte (0xFF
// with no body) and the local table's slot count.
struct ServeOnlyStart {
	uint8_t equipped = 0xFF;
	size_t slots = 0;
};
// `shell_page`, when given, is the game shell's profile side page: the shell's boot offers it to
// the session seed (Simulation::seed_session_kit_from_profile), as a Godot-game Serve Only host does.
ServeOnlyStart run_serve_only_start(inmatch::ClientProcessState &process,
		const char *shell_page = nullptr) {
	ServeOnlyStart out;
	std::map<std::string, std::string> files;
	files["weapon.def"] = "weapon WPN_KNIFE\r\nend\r\nweapon WPN_M4AUTO\r\nend\r\n";
	auto kernel_storage = std::make_unique<ms::MissionKernel>();
	test_titles::jo_world(kernel_storage->world);
	ms::MissionKernel &kernel = *kernel_storage;
	auto role_storage = std::make_unique<inmatch::HostRole>(inmatch::RoleKind::DedicatedHost, nullptr);
	test_titles::jo_controls(*role_storage);
	inmatch::HostRole &role = *role_storage;
	role.bind(kernel);
	kernel.open_document(two_entity_mission(), "serve_only_buffer", source_over(&files));
	ms::KernelBootOptions options;
	options.playable = false;
	options.mp_session = true;
	options.game_type = mission_game_type(kernel.mission);
	options.bringup_net_session = [&] { role.bring_up(serve_only_bringup(options.game_type, &process)); };
	std::string error;
	CHECK(kernel.boot(options, error));
	if (shell_page != nullptr) {
		// The row's team 1 selects side A; its page names `shell_page`. A dedicated host is no
		// session peer, so the seed copies nothing and the buffer stands.
		playersav::Record profile;
		profile.blue.player_class = 8;
		profile.blue.pages[3].entries.push_back(playersav::KitEntry{shell_page, 1, 0, -1});
		int seeded_side = -1;
		const size_t before = process.spawn_loadout_buffer.entries.size();
		CHECK(!inmatch::seed_session_kit_from_profile(kernel.world, profile,
				*opennova::title_by_code("jo"),
				opennova::bms::MissionType::NormalMission, /*assigned_team=*/1,
				kernel.local.loadout, seeded_side));
		CHECK(seeded_side == -1);
		CHECK(process.spawn_loadout_buffer.entries.size() == before);
	}
	// The kernel boot's mission start ran the round init's row body and Player_InitPlayer
	// (MissionKernel::round_init), ahead of the shell's seed above.
	const w::Entity *body = kernel.world.registry.get(kernel.world.cached.local_player);
	CHECK(body != nullptr);
	if (body != nullptr) out.equipped = body->equipped_adm_index;
	for (int combo = 0; combo < w::weapon_combo::kSlotCount; ++combo) {
		const w::WeaponInventorySlot *slot = kernel.local.inventory.slot(combo);
		if (slot != nullptr && slot->adm_index >= 0) ++out.slots;
	}
	return out;
}

inmatch::TickInput tick_input() {
	inmatch::TickInput in;
	in.loaded_model_list_count = 0;
	return in;
}

// A LAN listen host over the two-entity mission whose placed item stands in
// for an authored spawn zone, so the host's own player joins held. The kernel
// boot's mission start runs the round init and Player_InitPlayer's body leg
// (MissionKernel::round_init, D-NET-1233).
void run_held_host() {
	std::map<std::string, std::string> files;
	auto kernel_storage = std::make_unique<ms::MissionKernel>();
	test_titles::jo_world(kernel_storage->world);
	ms::MissionKernel &kernel = *kernel_storage;
	auto role_storage = std::make_unique<inmatch::HostRole>();
	test_titles::jo_controls(*role_storage);
	inmatch::HostRole &role = *role_storage;
	role.bind(kernel);
	inmatch::ListenHostState &host = role.state;
	kernel.open_document(two_entity_mission(), "mission_start", source_over(&files));
	ms::KernelBootOptions options;
	options.mp_session = true;
	options.game_type = mission_game_type(kernel.mission);
	options.bringup_net_session = [&] {
		w::EntityHandle placed;
		kernel.world.registry.for_each([&](const w::Entity &e) {
			if (e.net_id == 21) placed = e.handle;
		});
		if (w::Entity *zone = kernel.world.registry.get(placed)) {
			zone->has_item_def = true;
			zone->item_attrib |= w::kItemAttribSpawnPoint;
			zone->is_spawn_point = true;
		}
		inmatch::HostConfig cfg;
		test_titles::jo_config(cfg.config);
		cfg.protocol = &kJo;
		cfg.config.server_name = "mission_start";
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
	CHECK(kernel.world.zones.has_spawn_zone());
	opennova::inmatch::NullDatagramSocket socket;
	role.set_socket(&socket);
	inmatch::NapiNPConnection *own = nullptr;
	for (inmatch::NapiNPConnection &conn : host.host_owner.ctx.np_protocol.connection_list)
		if (conn.link.owned_entity == kernel.world.cached.local_player) own = &conn;
	CHECK(own != nullptr);
	if (own == nullptr) return;
	const w::EntityHandle body = own->link.owned_entity;
	auto hidden = [&] {
		const w::Entity *e = kernel.world.registry.get(body);
		return e != nullptr && (e->flags & w::kEntityFlagCarried) != 0;
	};
	// The held own body leaves the mission start hidden, named after the local profile
	// (the session config's player name), and the round init's burst left the own slot
	// in state 5; the mission start's end queued the own client's C2S 0x0B, which the
	// first frame's receive pump hands to its receiver (D-NET-884). The eager WAC
	// load leaves its clock at 1 [orig: WacScript_InitAndLoad @0x4F9665 / @0x4F9770].
	CHECK(own->slot_state == inmatch::PlayerSlotState::AwaitJoin && !own->burst.spawned);
	CHECK(!own->link.respawn_pending && hidden());
	{
		const w::Entity *e = kernel.world.registry.get(body);
		CHECK(e != nullptr && e->name == host.host_owner.ctx.config.player_name);
	}
	// The own client's mission start queued its two 0x2F and its 0x0B (D-NET-695).
	CHECK(host.host_loop.c2s_pending() == 3);
	kernel.world.cached.wac_ticks = 1;
	const uint32_t updates = kernel.world.entity_update_counter;
	role.run_tick(tick_input());
	// The receive pump's join holds the undeployed slot; it clears the body's
	// hidden bit and its own 0x0A's writer sets it again, both inside the
	// receive pump, ahead of the slot lists: the frame counts no human and its
	// update holds: no frame between the load and the own join runs an update
	// (N = 0, the header's arithmetic).
	// [orig: Server_OnPlayerJoin - the clear @0x51A7DA, its 0x0A @0x51A82D ->
	//  NetPacket_WritePlayerState (+89912 & 0x10) @0x4FF7B8; Server_TickUpdate -
	//  the receive pump @0x51D895, Server_BuildEntitySlotLists @0x51D89A]
	CHECK(own->burst.spawned && own->slot_state == inmatch::PlayerSlotState::Joined);
	CHECK(own->link.respawn_pending);
	CHECK(kernel.world.cached.humans == 0);
	CHECK(kernel.world.entity_update_counter == updates);
	// The join's 0x0F reaches the host's own client after the frame's server tick (retail:
	// at the head of the next frame, ahead of its Server_TickUpdate), and its local-player
	// block, which has no authority test, clears the shared body's hidden bit: the death
	// screen is down.
	// [orig: Server_OnPlayerJoin's 0x0F @0x51A869; NapiNPClientMsg_0x00F @0x42E3F5..0x42E403;
	//  Game_ProcessMainFrame - Client_ProcessNetworkFrame @0x526692, Server_TickUpdate @0x5266B6]
	CHECK(role.client_runtime() != nullptr && role.client_runtime()->state().world_state.revision == 1 &&
			!role.client_runtime()->state().death_screen_active);
	CHECK(!hidden());
	// So the second frame's slot lists count the held body as a human and its entity update
	// runs once, at the frame's tick 2 (an even key tick: the org1 block's gravity, water and
	// landing legs run), before that frame's 0x0A writer hides the undeployed body again; the
	// third frame holds. Retail's CP04 host streams its AI records exactly this one update past
	// the mission start (the x4 cp04 RR records against OR's).
	// [orig: Server_BuildEntitySlotLists @0x4F97A0 (+1 @0x4F98B1) from @0x51D89A; the gate
	//  @0x52671C..0x52672A; Entity_UpdateAllEntities @0x52674B (frameTickCount g_CurrentTick,
	//  stepped @0x5265B4 from Game_StartMission @0x524360's zero @0x525B9F); NetPacket_WritePlayerState
	//  @0x4FF6B0 (@0x4FF6D7 / @0x4FF7B8); Entity_UpdateInfantryAI's key parity @0x4BF146 / @0x4BF6A5..0x4BF6B2]
	CHECK(kernel.world.logic_tick == 2);
	role.run_tick(tick_input());
	CHECK(kernel.world.cached.humans == 1);
	CHECK(kernel.world.entity_update_counter == updates + 1);
	CHECK(hidden());
	role.run_tick(tick_input());
	CHECK(kernel.world.cached.humans == 0);
	CHECK(kernel.world.entity_update_counter == updates + 1);
	CHECK(hidden());
}

// A root-motion source whose every clip lifts the root and stands its capsule
// one unit over the origin (player_placement_test's).
struct Source final : w::IRootMotionSource {
	bool has_clip(int, int) const override { return true; }
	int32_t clip_length_ticks(int, int, int) const override { return 10000; }
	bool advance(int, int, int32_t &phase, w::RootMotionFrame &out) override {
		++phase;
		out = {};
		out.dz = 100;
		out.capsule_bottom = 65536;
		out.capsule_top = 2 * 65536;
		return true;
	}
};

// D-NET-884: the listen host's own join runs from its C2S 0x0B, queued at
// the mission start's end and handled by the next frame's receive pump, so its
// placement's warm-up (a Deathmatch host's scored arm) runs on a body whose
// animation channel the mission start wired, and the join's own 0x0A follows
// it: the body is lifted and its clip advanced before that 0x0A (the ASH
// z / animRatio keys). The bring-up's join ran ahead of that wiring.
// [orig: Game_StartMission @0x52624B / @0x526255; NapiNPServerMsg_PlayerJoinRequest
//  @0x51AB10 -> Server_OnPlayerJoin; Server_PositionPlayerForSpawn - the scored
//  arm's Entity_WarmUpOrganicAnimation @0x50D572; Server_OnPlayerJoin's 0x0A
//  @0x51A82D]
void run_own_join_warms_the_wired_body() {
	std::map<std::string, std::string> files;
	auto kernel_storage = std::make_unique<ms::MissionKernel>();
	test_titles::jo_world(kernel_storage->world);
	ms::MissionKernel &kernel = *kernel_storage;
	auto role_storage = std::make_unique<inmatch::HostRole>();
	test_titles::jo_controls(*role_storage);
	inmatch::HostRole &role = *role_storage;
	role.bind(kernel);
	inmatch::ListenHostState &host = role.state;
	bms::File mission = two_entity_mission();
	bms::AttribFlags attrib = mission.header.attrib_flags;
	CHECK(bms::set_game_mode(attrib, static_cast<uint32_t>(bms::AttribFlags::Deathmatch)));
	mission.header.attrib_flags = attrib;
	kernel.open_document(mission, "own_join_warm_up", source_over(&files));
	ms::KernelBootOptions options;
	options.mp_session = true;
	options.game_type = mission_game_type(kernel.mission);
	CHECK(options.game_type == game_type::kDeathmatch);
	options.bringup_net_session = [&] {
		inmatch::HostConfig cfg;
		test_titles::jo_config(cfg.config);
		cfg.protocol = &kJo;
		cfg.config.server_name = "own_join_warm_up";
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
	inmatch::NapiNPConnection *own = nullptr;
	for (inmatch::NapiNPConnection &conn : host.host_owner.ctx.np_protocol.connection_list)
		if (conn.link.owned_entity == kernel.world.cached.local_player) own = &conn;
	CHECK(own != nullptr);
	if (own == nullptr) return;
	w::Entity *body = kernel.world.registry.get(own->link.owned_entity);
	w::AiEntity *motor = kernel.world.ai.for_handle(own->link.owned_entity);
	CHECK(body != nullptr && motor != nullptr);
	if (body == nullptr || motor == nullptr) return;
	// Not joined at the bring-up: the own 0x0B waits on the loopback.
	CHECK(own->slot_state == inmatch::PlayerSlotState::AwaitJoin && !own->burst.spawned);
	// The own client's mission start queued its two 0x2F and its 0x0B (D-NET-695).
	CHECK(host.host_loop.c2s_pending() == 3);
	// The body's animation channel, which the mission start wires
	// (MissionKernel::resolve_new_infantry_adm_ids over the infantry ADM).
	Source source;
	kernel.world.ai.root_motion = &source;
	motor->inf.adm_id = 0;
	const int32_t z0 = body->position.z.q16;
	const int32_t phase0 = motor->inf.clip_phase;
	opennova::replication::Datagram drained;
	while (host.host_loop.client_recv(drained)) {
	}
	// The receive pump's own-client leg: the 0x0B's receiver runs the join.
	role.drain_host_client_gameplay_requests();
	CHECK(own->burst.spawned && own->slot_state == inmatch::PlayerSlotState::Joined);
	CHECK(motor->inf.clip_phase > phase0);
	CHECK(body->position.z.q16 > z0);
	// The join's bundle on the loopback, its 0x0A ahead of the 0x0F.
	std::vector<uint8_t> tags;
	opennova::replication::Datagram dg;
	while (host.host_loop.client_recv(dg)) tags.push_back(dg.tag);
	const auto a = std::find(tags.begin(), tags.end(), uint8_t{0x0A});
	const auto f = std::find(tags.begin(), tags.end(), uint8_t{0x0F});
	CHECK(a != tags.end() && f != tags.end() && a < f);
	kernel.world.ai.root_motion = nullptr;
}

// D-NET-695: in a session the listen host's own client submits its profile's kit at its
// mission start as a joiner's does: the side block its team names (the fresh profile's
// side A, class 8), first with the fixed slot 195, then with g_CurrentWeaponSlot (no
// local table here: the raw 195), both ahead of its 0x0B in queue order; the server's
// next receive accepts them for the own slot, whose class and PlayerType become the
// accepted class and whose loadout buffer the accept fills.
// [orig: Game_StartMission - is_in_session @0x525767, is_mp_session_peer @0x525773, the
//  side block @0x525793..0x5257C6, NetPacket_SendLoadoutSubmit(.., 195) @0x525836; the
//  second @0x525BC4..0x525C2E; the 0x0B @0x52624B, the pump @0x526255;
//  NapiNPServerMsg_HandlePlayerLoadout @0x515790 - the slot class @0x515AA8, the list
//  @0x515F95 (Server_SendWeaponSlotListToPlayer's PlayerType @0x5025A8)]
void run_own_mission_start_submits_its_kit() {
	std::map<std::string, std::string> files;
	auto kernel_storage = std::make_unique<ms::MissionKernel>();
	test_titles::jo_world(kernel_storage->world);
	ms::MissionKernel &kernel = *kernel_storage;
	auto role_storage = std::make_unique<inmatch::HostRole>();
	test_titles::jo_controls(*role_storage);
	inmatch::HostRole &role = *role_storage;
	role.bind(kernel);
	inmatch::ListenHostState &host = role.state;
	kernel.open_document(two_entity_mission(), "own_kit_pair", source_over(&files));
	ms::KernelBootOptions options;
	options.mp_session = true;
	options.game_type = mission_game_type(kernel.mission);
	options.bringup_net_session = [&] {
		inmatch::HostConfig cfg;
		test_titles::jo_config(cfg.config);
		cfg.protocol = &kJo;
		cfg.config.server_name = "own_kit_pair";
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
	inmatch::NapiNPConnection *own = nullptr;
	for (inmatch::NapiNPConnection &conn : host.host_owner.ctx.np_protocol.connection_list)
		if (conn.link.owned_entity == kernel.world.cached.local_player) own = &conn;
	CHECK(own != nullptr);
	if (own == nullptr) return;
	std::vector<opennova::replication::Datagram> queued;
	opennova::replication::Datagram dg;
	while (host.host_loop.host_recv(dg)) queued.push_back(dg);
	CHECK(queued.size() == 3);
	if (queued.size() != 3) return;
	CHECK(queued[0].tag == 0x2F && queued[1].tag == 0x2F && queued[2].tag == 0x0B);
	LoadoutSubmit first;
	LoadoutSubmit second;
	CHECK(decode_loadout_submit(queued[0].body.data(), queued[0].body.size(), first));
	CHECK(decode_loadout_submit(queued[1].body.data(), queued[1].body.size(), second));
	CHECK(first.team == own->assigned_team && first.player_class == 8 &&
			first.weapon_slot_index == 195u);
	CHECK(second.team == own->assigned_team && second.player_class == 8 &&
			second.weapon_slot_index == 195u);
	// The create stored side B's class (the side requirement -1); the accept stores 8.
	own->slot_class = 6;
	for (opennova::replication::Datagram &q : queued)
		host.host_loop.deliver_c2s(q.tag, std::move(q.body));
	role.drain_host_client_gameplay_requests();
	CHECK(own->slot_class == 8 && own->player_list_type == 8);
	CHECK(own->reply.loadout_buffer_set && own->reply.loadout_buffer.player_class == 8);
}

// The single player's mission start submits no kit: SinglePlayer_StartMission creates its
// session with the network type 0, which leaves is_in_session clear, so both submits' session
// tests fail and its own client queues the 0x0B alone (its slot is never accepted, its local
// table stays the one the load and the shell built). Our session create sets the context's
// is_in_session word for the single player too; the test is the rules' mp_session, the
// socketless session's false.
// [orig: SinglePlayer_StartMission @0x561AF0 - CNapiNetwork_SetNetworkType(0) @0x561BD4;
//  CNapiNetwork_SetNetworkType - is_in_session = (type in 1..3) @0x4C4A85;
//  Game_StartMission - is_in_session @0x525767 / @0x525BC4 ahead of the submits
//  @0x525836 / @0x525C2E; the 0x0B @0x52624B]
void run_single_player_mission_start_submits_no_kit() {
	std::map<std::string, std::string> files;
	auto kernel_storage = std::make_unique<ms::MissionKernel>();
	test_titles::jo_world(kernel_storage->world);
	ms::MissionKernel &kernel = *kernel_storage;
	auto role_storage = std::make_unique<inmatch::HostRole>(
			inmatch::RoleKind::SinglePlayer, nullptr);
	test_titles::jo_controls(*role_storage);
	inmatch::HostRole &role = *role_storage;
	role.bind(kernel);
	inmatch::ListenHostState &host = role.state;
	kernel.open_document(two_entity_mission(), "sp_no_kit_pair", source_over(&files));
	ms::KernelBootOptions options;
	options.mp_session = false;
	options.game_type = mission_game_type(kernel.mission);
	options.bringup_net_session = [&] {
		inmatch::HostConfig cfg;
		test_titles::jo_config(cfg.config);
		cfg.protocol = &kJo;
		cfg.config.server_name = "SINGLEPLAYERGAME";
		cfg.config.max_players = 1;
		cfg.config.game_type = options.game_type;
		cfg.socket_mode = inmatch::SocketMode::Socketless;
		cfg.serve_and_play = true;
		inmatch::HostBringup bringup;
		bringup.host_cfg = cfg;
		role.bring_up(bringup);
	};
	std::string error;
	CHECK(kernel.boot(options, error));
	CHECK(!kernel.world.rules.mp_session);
	CHECK(kernel.world.cached.local_player.valid());
	std::vector<opennova::replication::Datagram> queued;
	opennova::replication::Datagram dg;
	while (host.host_loop.host_recv(dg)) queued.push_back(dg);
	CHECK(queued.size() == 1);
	if (queued.size() == 1) CHECK(queued[0].tag == 0x0B);
}

// The first mission start's round init seeds the own slot's loadout buffer from the spawn
// buffer as it stands: a playing host's mission start copied its side's class page into it
// first (the shell's seed_session_kit_from_profile; here the title's default kit stands for
// it, the two of its weapons the mission's weapon.def carries, in the kit's order); the
// session's own add, inside the load ahead of the weapon table, seeds nothing. The kernel
// boot's mission start runs the seed at its round init (HostRole::run_round_init); this rig
// fills the spawn buffer just ahead of it, as the shell's seed does.
// [orig: Game_StartMission @0x525BAF -> Server_InitAllPlayerEntitiesForRound
//  @0x516BC4..0x516BE3; the peer's side page @0x525767..0x525818]
void run_first_round_init_seeds_the_own_buffer() {
	std::map<std::string, std::string> files;
	files["weapon.def"] = "weapon WPN_KNIFE\r\nend\r\nweapon WPN_M4AUTO\r\nend\r\n";
	auto kernel_storage = std::make_unique<ms::MissionKernel>();
	test_titles::jo_world(kernel_storage->world);
	ms::MissionKernel &kernel = *kernel_storage;
	auto role_storage = std::make_unique<inmatch::HostRole>();
	test_titles::jo_controls(*role_storage);
	inmatch::HostRole &role = *role_storage;
	role.bind(kernel);
	inmatch::ListenHostState &host = role.state;
	kernel.open_document(two_entity_mission(), "own_buffer_seed", source_over(&files));
	bool add_seeded_nothing = false;
	ms::KernelBootOptions options;
	options.mp_session = true;
	options.game_type = mission_game_type(kernel.mission);
	options.bringup_net_session = [&] {
		inmatch::HostConfig cfg;
		test_titles::jo_config(cfg.config);
		cfg.protocol = &kJo;
		cfg.config.server_name = "own_buffer_seed";
		cfg.config.max_players = 4;
		cfg.config.game_type = options.game_type;
		cfg.socket_mode = inmatch::SocketMode::Lan;
		cfg.serve_and_play = true;
		inmatch::HostBringup bringup;
		bringup.host_cfg = cfg;
		role.bring_up(bringup);
		// The add, inside the load ahead of the weapon table, seeded nothing; the round init
		// past the PreMission pass seeds the buffer from the spawn buffer as it then stands.
		const std::function<void()> round_init = kernel.round_init;
		kernel.round_init = [&, round_init] {
			for (const inmatch::NapiNPConnection &conn :
					host.host_owner.ctx.np_protocol.connection_list)
				if (conn.type == inmatch::NapiNPConnection::kTypeClientSide)
					add_seeded_nothing = conn.reply.loadout_buffer.entries.empty();
			kernel.local.loadout.spawn_kit() = w::weapon_kit_default(kernel.world.rules.title());
			round_init();
		};
	};
	std::string error;
	CHECK(kernel.boot(options, error));
	CHECK(add_seeded_nothing);
	// Entry 0 is the engine's "null" def; the file's two follow it.
	CHECK(kernel.world.tables.weapons.index_of("WPN_KNIFE") == 1 &&
			kernel.world.tables.weapons.index_of("WPN_M4AUTO") == 2);
	const inmatch::NapiNPConnection *own = nullptr;
	for (const inmatch::NapiNPConnection &conn : host.host_owner.ctx.np_protocol.connection_list)
		if (conn.type == inmatch::NapiNPConnection::kTypeClientSide) own = &conn;
	CHECK(own != nullptr);
	if (own == nullptr) return;
	const std::vector<LoadoutSubmitEntry> &entries = own->reply.loadout_buffer.entries;
	CHECK(own->reply.loadout_buffer_set && entries.size() == 2);
	if (entries.size() == 2) CHECK(entries[0].adm_index == 2 && entries[1].adm_index == 1);
}

// The single player's restart is a mission start whose armory table and class mask come from
// the mission entry's template (Game_StartMission(1), from Game_RestartRoundSP): the name-list
// result over the mission's chunks, which ours promotes at every load; ours boots a fresh
// kernel for the restart (D-PWR-8), whose table load promotes the same chunks again, so the
// restart's kit and availability equal the fresh launch's (NOT-A-DIV of FOUND-L7-s5-1's
// restart arm).
// [orig: Game_ProcessMainFrame @0x52682A..0x52682C -> Game_RestartRoundSP, its push 1
//  @0x5263D9 and call @0x5263DB -> Game_StartMission
//  - the table from the entry's +0xD1C template @0x5246CD..0x5246E8, the mask from +0x1118
//  @0x5246ED..0x524706; Mission_LoadBMSFile @0x40F4E0 - the SP-vs-net gate @0x40F694]
void run_single_player_restart_repromotes_the_mission_kit() {
	std::vector<world::WeaponKitEntry> kits[2];
	int32_t knife[2] = {0, 0};
	int32_t m4[2] = {0, 0};
	for (int pass = 0; pass < 2; ++pass) {
		std::map<std::string, std::string> files;
		files["weapon.def"] = "weapon WPN_KNIFE\r\nend\r\nweapon WPN_M4AUTO\r\nend\r\n";
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		test_titles::jo_world(kernel_storage->world);
		ms::MissionKernel &kernel = *kernel_storage;
		bms::File mission = two_entity_mission();
		bms::WeaponLoadoutRecord row;
		row.name = "WPN_KNIFE";
		row.ammo_primary = "1";
		row.ammo_secondary = "0";
		mission.loadout.entries.push_back(row);
		bms::ItemAvailabilityEntry banned;
		banned.name = "WPN_M4AUTO";
		banned.status = 0;
		mission.item_availability.push_back(banned);
		kernel.open_document(std::move(mission), "sp_restart", source_over(&files));
		ms::KernelBootOptions options;
		options.mp_session = false;
		options.restart = pass == 1;
		options.game_type = mission_game_type(kernel.mission);
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(kernel.restart_boot() == (pass == 1));
		kits[pass] = world::local_loadout_kit(kernel.world, kernel.local.loadout);
		knife[pass] = kernel.local.loadout.availability.value_for(
				kernel.world.tables.weapons.index_of("WPN_KNIFE"));
		m4[pass] = kernel.local.loadout.availability.value_for(
				kernel.world.tables.weapons.index_of("WPN_M4AUTO"));
	}
	CHECK(kits[0].size() == kits[1].size());
	for (size_t i = 0; i < kits[0].size() && i < kits[1].size(); ++i)
		CHECK(kits[0][i].name == kits[1][i].name &&
				kits[0][i].ammo_primary == kits[1][i].ammo_primary &&
				kits[0][i].ammo_secondary == kits[1][i].ammo_secondary);
	CHECK(!kits[0].empty() && kits[0][0].name == "WPN_KNIFE");
	CHECK(knife[0] == knife[1] && m4[0] == m4[1]);
	// A mission with no kit row: the fresh start's and the restart's buffer are the literal
	// default page, WPN_M4AUTO first (with no profile page of the shell's)
	// [orig: Game_ApplySessionSettingsToGlobals @0x5519A2..0x5519E4 (out of a session);
	//  Game_StartMission @0x524683..0x5246C8 (the restart's copy), "WPN_M4AUTO" @0x5246BE].
	// The fill is the single player start's (HostRole's bring-up writes the static arm).
	std::vector<world::WeaponKitEntry> literal[2];
	for (int pass = 0; pass < 2; ++pass) {
		std::map<std::string, std::string> files;
		files["weapon.def"] = "weapon WPN_KNIFE\r\nend\r\nweapon WPN_M4AUTO\r\nend\r\n";
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		test_titles::jo_world(kernel_storage->world);
		ms::MissionKernel &kernel = *kernel_storage;
		auto role_storage = std::make_unique<inmatch::HostRole>(
				inmatch::RoleKind::SinglePlayer, nullptr);
		test_titles::jo_controls(*role_storage);
		inmatch::HostRole &role = *role_storage;
		role.bind(kernel);
		kernel.open_document(two_entity_mission(), "sp_restart_literal", source_over(&files));
		ms::KernelBootOptions options;
		options.mp_session = false;
		options.restart = pass == 1;
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] { role.bring_up(single_player_bringup(options.game_type)); };
		std::string error;
		CHECK(kernel.boot(options, error));
		literal[pass] = world::local_loadout_kit(kernel.world, kernel.local.loadout);
	}
	CHECK(literal[0].size() == literal[1].size());
	CHECK(!literal[0].empty() && literal[0][0].name == "WPN_M4AUTO");
	CHECK(!literal[1].empty() && literal[1][0].name == "WPN_M4AUTO");
}

// Out of a session the side-A class word is the mission's type byte when it is 1, 2 or 3, else 1,
// and the mission start's Player_InitPlayer re-stamps the single player's team-1 body from it
// (D-PLAYERINFO-33): never the profile's class.
// [orig: Game_ApplySessionSettingsToGlobals @0x551500 (@0x551707..0x551725);
//  MissionList_ScanAndBuildFromFiles @0x563170 (optionA_1134 @0x563365); Player_InitPlayer
//  @0x4E15F0 - the team 1 / 3 test @0x4E1809..0x4E1817, g_CharClassTeam1 @0x4E182C..0x4E1832]
void run_single_player_class_is_the_missions_type() {
	struct Case {
		uint8_t type;
		uint8_t cls;
	};
	for (const Case c : {Case{1, 1}, Case{2, 2}, Case{3, 3}, Case{0, 1}, Case{7, 1}}) {
		std::map<std::string, std::string> files;
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		test_titles::jo_world(kernel_storage->world);
		ms::MissionKernel &kernel = *kernel_storage;
		auto role_storage = std::make_unique<inmatch::HostRole>(
				inmatch::RoleKind::SinglePlayer, nullptr);
		test_titles::jo_controls(*role_storage);
		inmatch::HostRole &role = *role_storage;
		role.bind(kernel);
		bms::File mission = two_entity_mission();
		mission.header.mission_type = static_cast<bms::MissionType>(c.type);
		kernel.open_document(std::move(mission), "sp_class", source_over(&files));
		ms::KernelBootOptions options;
		options.mp_session = false;
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] {
			inmatch::HostConfig cfg;
			test_titles::jo_config(cfg.config);
			cfg.protocol = &kJo;
			cfg.config.server_name = "SINGLEPLAYERGAME";
			cfg.config.max_players = 1;
			cfg.config.game_type = options.game_type;
			cfg.socket_mode = inmatch::SocketMode::Socketless;
			cfg.serve_and_play = true;
			// The shell's profile projection: class 6 on both sides.
			inmatch::CharacterJoinVars vars;
			vars.char_class[0] = 6;
			vars.char_class[1] = 6;
			cfg.local_character_vars = vars;
			inmatch::HostBringup bringup;
			bringup.host_cfg = cfg;
			role.bring_up(bringup);
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		const inmatch::NapiNPConnection *own = nullptr;
		for (const inmatch::NapiNPConnection &conn : role.state.host_owner.ctx.np_protocol.connection_list)
			if (conn.type == inmatch::NapiNPConnection::kTypeClientSide) own = &conn;
		CHECK(own != nullptr);
		if (own == nullptr) continue;
		CHECK(own->char_vars.char_class[0] == c.cls);
		CHECK(own->char_vars.char_class[1] == 6);
		// The kernel boot's round init ran Player_InitPlayer's body leg.
		const w::Entity *body = kernel.world.registry.get(kernel.world.cached.local_player);
		CHECK(body != nullptr);
		if (body != nullptr) CHECK(body->team == 1 && body->player_class == c.cls);
	}
}

// A single player's restart keeps the class word the start stored: Game_StartMission(1) skips the
// session apply, runs no round init, and no other writer of g_CharClassTeam1 runs out of a session
// (the 0x5A handler is a joiner's, the slot-list send answers a 0x2F or a deploy), so the restart's
// own row and Player_InitPlayer's re-stamp read the start's mission type; a profile class changed
// between the two boots is never read (D-PLAYERINFO-33). The two boots share the process words.
// [orig: Game_RestartRoundSP @0x5263A0 - the push 1 @0x5263D9; Game_StartMission @0x524360 - the
//  `cmp esi, ebx` / jz @0x52465E..0x524660 over the apply @0x524662, the own row's class
//  @0x524EE3; Game_ApplySessionSettingsToGlobals @0x551500 (@0x551707..0x551725); Player_InitPlayer
//  @0x4E15F0 (@0x4E182C..0x4E1832)]
void run_single_player_restart_keeps_the_class_word() {
	auto process = std::make_unique<inmatch::ClientProcessState>();
	const bool restarts[2] = {false, true};
	const uint8_t profile_class[2] = {6, 7};
	for (int pass = 0; pass < 2; ++pass) {
		std::map<std::string, std::string> files;
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		test_titles::jo_world(kernel_storage->world);
		ms::MissionKernel &kernel = *kernel_storage;
		auto role_storage = std::make_unique<inmatch::HostRole>(
				inmatch::RoleKind::SinglePlayer, nullptr);
		test_titles::jo_controls(*role_storage);
		inmatch::HostRole &role = *role_storage;
		role.bind(kernel);
		bms::File mission = two_entity_mission();
		mission.header.mission_type = static_cast<bms::MissionType>(2);
		kernel.open_document(std::move(mission), "sp_restart_class", source_over(&files));
		ms::KernelBootOptions options;
		options.mp_session = false;
		options.restart = restarts[pass];
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] {
			inmatch::HostBringup bringup = single_player_bringup(options.game_type, process.get());
			inmatch::CharacterJoinVars vars;
			vars.char_class[0] = profile_class[pass];
			vars.char_class[1] = profile_class[pass];
			bringup.host_cfg.local_character_vars = vars;
			role.bring_up(bringup);
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		const inmatch::NapiNPConnection *own = nullptr;
		for (const inmatch::NapiNPConnection &conn : role.state.host_owner.ctx.np_protocol.connection_list)
			if (conn.type == inmatch::NapiNPConnection::kTypeClientSide) own = &conn;
		CHECK(own != nullptr);
		if (own == nullptr) continue;
		CHECK(own->char_vars.char_class[0] == 2);
		// The kernel boot's round init ran Player_InitPlayer's body leg.
		const w::Entity *body = kernel.world.registry.get(kernel.world.cached.local_player);
		CHECK(body != nullptr);
		if (body != nullptr) CHECK(body->team == 1 && body->player_class == 2);
	}
}

// A single player's restart runs no round-state init (Game_RestartRoundSP -> Game_StartMission(1)),
// so its status UP-TIME keeps counting from the first start's stamp, where a fresh start's create
// stamps its own; and the restart's spawn raises read the word the previous start left, the
// mission start's (D-PWR-8). The two boots share the process words, as the shell's do.
// [orig: Server_InitNewRoundState - GetTickCount @0x51CB93, the store @0x51CBA0;
//  Server_BuildStatusReport @0x530B84..0x530B8C; Game_RestartRoundSP @0x5263A0 - the push 1
//  @0x5263D9; Game_StartMission - the 0 @0x525CDD]
void run_single_player_restart_keeps_the_uptime_stamp() {
	auto process = std::make_unique<inmatch::ClientProcessState>();
	uint32_t stamps[3] = {0, 0, 0};
	uint32_t uptimes[3] = {0, 0, 0};
	int32_t difficulty[3] = {9, 9, 9};
	const uint32_t start_ticks[3] = {1000, 5000, 9000};
	const bool restarts[3] = {false, true, false};
	for (int pass = 0; pass < 3; ++pass) {
		std::map<std::string, std::string> files;
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		test_titles::jo_world(kernel_storage->world);
		ms::MissionKernel &kernel = *kernel_storage;
		auto role_storage = std::make_unique<inmatch::HostRole>(
				inmatch::RoleKind::SinglePlayer, nullptr);
		test_titles::jo_controls(*role_storage);
		inmatch::HostRole &role = *role_storage;
		role.bind(kernel);
		inmatch::ListenHostState &host = role.state;
		kernel.open_document(two_entity_mission(), "sp_uptime", source_over(&files));
		ms::KernelBootOptions options;
		options.mp_session = false;
		options.restart = restarts[pass];
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] {
			inmatch::HostConfig cfg;
			test_titles::jo_config(cfg.config);
			cfg.protocol = &kJo;
			cfg.config.server_name = "SINGLEPLAYERGAME";
			cfg.config.max_players = 1;
			cfg.config.game_type = options.game_type;
			cfg.config.profile_difficulty = -1;
			cfg.socket_mode = inmatch::SocketMode::Socketless;
			cfg.serve_and_play = true;
			cfg.host_start_tick = start_ticks[pass];
			cfg.client_process = process.get();
			inmatch::HostBringup bringup;
			bringup.host_cfg = cfg;
			role.bring_up(bringup);
			// The word the spawn raises read, ahead of the mission start's reset.
			difficulty[pass] = kernel.world.rules.difficulty;
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		const inmatch::NapiNPServerCtx &ctx = host.host_owner.ctx;
		stamps[pass] = ctx.round_state_init_tick;
		uptimes[pass] = inmatch::session_uptime_ms(ctx);
	}
	// The fresh start stamps its own start tick; the restart keeps it, its UP-TIME the clock
	// since the first start; the next fresh start stamps its own again.
	CHECK(stamps[0] == 1000 && stamps[1] == 1000 && stamps[2] == 9000);
	CHECK(uptimes[1] == uptimes[0] + 4000u && uptimes[2] == uptimes[0]);
	CHECK(process->round_state_init_tick == 9000);
	// A fresh start seeds the profile's word; the restart reads the previous start's.
	CHECK(difficulty[0] == -1 && difficulty[2] == -1);
	CHECK(difficulty[1] == w::mission_start_difficulty(mission_game_type(two_entity_mission())));
}

// A fresh process's spawn buffer is the image's zero: a dedicated start fills nothing (no single
// player fill, no peer's side page), so the Serve Only row's Player_InitPlayer builds an empty
// table, its select of 195 finds no slot and stores nothing, and the row body keeps its spawn's
// equipped byte, the loadout name list's first weapon (D-NET-352).
// [orig: g_SpawnLoadoutBuffer @0x24D4E00 (zero in the image); Game_ApplySessionSettingsToGlobals
//  @0x551500 - the dedicated start's jz @0x5517A7; Game_StartMission - is_mp_session_peer
//  @0x525773 skips the side page; Player_InitPlayer @0x4E15F0 - the display list
//  @0x4E1872..0x4E1882, Player_SelectWeaponSlot @0x4E1995 -> the no-slot return
//  @0x4DD794..0x4DD79C; PlayerClass_InitEntity @0x4B1100..0x4B1116]
void run_a_fresh_dedicated_process_runs_on_an_empty_buffer() {
	auto process = std::make_unique<inmatch::ClientProcessState>();
	const ServeOnlyStart start = run_serve_only_start(*process);
	CHECK(process->spawn_loadout_buffer.entries.empty());
	CHECK(start.slots == 0);
	CHECK(start.equipped == 2); // WPN_M4AUTO: the name list initializer's first name
}

// A Godot-game Serve Only host's shell offers its profile's side page to the session seed; the
// mission start copies a side page only on a session peer, so the row keeps the process buffer:
// a fresh process's empty one (the spawn's WPN_M4AUTO), one that ran a single player mission
// that mission's kit (D-NET-143, D-NET-352; V-L4-s11).
// [orig: Game_StartMission @0x524360 - `cmp is_in_session` @0x525767, `cmp is_mp_session_peer`
//  @0x525773 / jz @0x525779 past the copy call @0x525818; Player_InitPlayer @0x4E15F0
//  (@0x4E1872..0x4E1882, @0x4E1995)]
void run_a_game_serve_only_row_copies_no_side_page() {
	{
		auto process = std::make_unique<inmatch::ClientProcessState>();
		const ServeOnlyStart start = run_serve_only_start(*process, "WPN_KNIFE");
		CHECK(process->spawn_loadout_buffer.entries.empty());
		CHECK(start.slots == 0);
		CHECK(start.equipped == 2); // WPN_M4AUTO, not the side page's knife
	}
	auto process = std::make_unique<inmatch::ClientProcessState>();
	process->spawn_loadout_buffer.entries.push_back(w::WeaponKitEntry{"WPN_KNIFE", 1, 0, -1});
	const ServeOnlyStart start = run_serve_only_start(*process, "WPN_M4AUTO");
	CHECK(process->spawn_loadout_buffer.entries.size() == 1 &&
			process->spawn_loadout_buffer.entries[0].name == "WPN_KNIFE");
	CHECK(start.slots == 1);
	CHECK(start.equipped == 1); // the single player's kit, not the side page's WPN_M4AUTO
}

// The buffer lives as long as the process: a single player mission's fill (its kit chunk here)
// is still the buffer when the same process hosts Serve Only, whose start fills nothing, so the
// row's Player_InitPlayer builds its table from it and its select of 195 equips the kit's knife
// over the spawn's WPN_M4AUTO, which every joiner's 0x0A off-16 then carries (D-NET-352).
// [orig: g_SpawnLoadoutBuffer @0x24D4E00; Mission_LoadBMSFile @0x40F4E0 (the write @0x40F961);
//  Game_ApplySessionSettingsToGlobals @0x551500 (@0x5517A7); Player_InitPlayer @0x4E15F0
//  (@0x4E1872..0x4E1882, @0x4E1995); Player_SelectWeaponSlot @0x4DD680 (the store @0x4DD727)]
void run_a_single_player_fill_carries_into_a_serve_only_row() {
	auto process = std::make_unique<inmatch::ClientProcessState>();
	{
		std::map<std::string, std::string> files;
		files["weapon.def"] = "weapon WPN_KNIFE\r\nend\r\nweapon WPN_M4AUTO\r\nend\r\n";
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		test_titles::jo_world(kernel_storage->world);
		ms::MissionKernel &kernel = *kernel_storage;
		auto role_storage = std::make_unique<inmatch::HostRole>(
				inmatch::RoleKind::SinglePlayer, nullptr);
		test_titles::jo_controls(*role_storage);
		inmatch::HostRole &role = *role_storage;
		role.bind(kernel);
		bms::File mission = two_entity_mission();
		bms::WeaponLoadoutRecord row;
		row.name = "WPN_KNIFE";
		row.ammo_primary = "1";
		row.ammo_secondary = "0";
		mission.loadout.entries.push_back(row);
		kernel.open_document(std::move(mission), "sp_fill", source_over(&files));
		ms::KernelBootOptions options;
		options.mp_session = false;
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] {
			role.bring_up(single_player_bringup(options.game_type, process.get()));
		};
		std::string error;
		CHECK(kernel.boot(options, error));
	}
	const std::vector<w::WeaponKitEntry> &buffer = process->spawn_loadout_buffer.entries;
	CHECK(buffer.size() == 1 && buffer[0].name == "WPN_KNIFE");
	const ServeOnlyStart start = run_serve_only_start(*process);
	CHECK(buffer.size() == 1 && buffer[0].name == "WPN_KNIFE");
	CHECK(start.slots == 1);
	CHECK(start.equipped == 1); // WPN_KNIFE
}

} // namespace

int main() {
	run_held_host();
	run_own_join_warms_the_wired_body();
	run_own_mission_start_submits_its_kit();
	run_single_player_mission_start_submits_no_kit();
	run_first_round_init_seeds_the_own_buffer();
	run_single_player_restart_repromotes_the_mission_kit();
	run_single_player_restart_keeps_the_uptime_stamp();
	run_single_player_class_is_the_missions_type();
	run_single_player_restart_keeps_the_class_word();
	run_a_fresh_dedicated_process_runs_on_an_empty_buffer();
	run_a_single_player_fill_carries_into_a_serve_only_row();
	run_a_game_serve_only_row_copies_no_side_page();
	if (failures == 0) std::printf("host_mission_start: all passed\n");
	return failures == 0 ? 0 : 1;
}
