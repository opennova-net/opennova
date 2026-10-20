// The mission start's Player_InitPlayer body leg on a playing host (L7-s1
// FOUND-1): with spawn zones the host's own player joins held, and the add hid
// its body. Every mission start then calls Player_InitPlayer after the round
// init, which clears the local player's hidden bit, so a server tick's slot
// lists count it as a human (the empty-world hold, humans 0 with a started WAC
// clock, lets that frame's entity update run) until the own join: the receive
// pump that hands over the own client's C2S 0x0B (queued at the mission
// start's end, D-NET-NEW-L4-5) runs the join, whose own 0x0A writer hides the
// undeployed body again ahead of the slot lists, so that frame holds. Ours
// hands the 0x0B over on the first frame (the own client's send countdown is
// D-NET-NEW-L1-17).
// [orig: Game_StartMission -- the round init @0x525BAF, the Player_InitPlayer
//  call @0x525BBC; Player_InitPlayer @0x4E15F0 -- `Flags &= ~1` @0x4E19C2;
//  Server_BuildEntitySlotLists @0x4F97A0 -- the Player bit @0x4F9815, the
//  hidden bit @0x4F9820, +1 @0x4F98B1, called from Server_TickUpdate @0x51D89A;
//  Game_ProcessMainFrame -- the humans/ticks hold @0x52671C..0x52672A, the
//  Entity_UpdateAllEntities call @0x52674B; NetPacket_WritePlayerState --
//  (+89912 & 0x10) @0x4FF7B8]
#include <runtime/inmatch/host_role.h>
#include <net/title/title_identity.h>
#include <base/gameprofile/gameprofile.h>
#include <base/gameprofile/game_type.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/null_datagram_socket.h>
#include <formats/mission/bms.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/ai.h>
#include <runtime/world/infantry.h> // IRootMotionSource (the warm-up pin)
#include <runtime/world/world.h>

#include "../common/boot_file_source.h"
#include "../common/synthetic_mission.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

static const opennova::TitleIdentity &kJo = *opennova::title_identity(opennova::gameprofile::GAME_JO);

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

inmatch::TickInput tick_input() {
	inmatch::TickInput in;
	in.loaded_model_list_count = 0;
	return in;
}

// A LAN listen host over the two-entity mission whose placed item stands in
// for an authored spawn zone, so the host's own player joins held.
// `mission_start` runs the mission start's Player_InitPlayer body leg.
void run_held_host(bool mission_start) {
	std::map<std::string, std::string> files;
	auto kernel_storage = std::make_unique<ms::MissionKernel>();
	ms::MissionKernel &kernel = *kernel_storage;
	auto role_storage = std::make_unique<inmatch::HostRole>();
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
		cfg.title = &kJo;
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
	// The held own body is hidden after the bring-up, whose burst left the own
	// slot in state 5; the mission start's end queued the own client's C2S
	// 0x0B, which the first frame's receive pump hands to its receiver
	// (D-NET-NEW-L4-5). The eager WAC load leaves its clock at 1 [orig:
	// WacScript_InitAndLoad @0x4F9665 / @0x4F9770].
	CHECK(own->slot_state == inmatch::PlayerSlotState::AwaitJoin && !own->burst.spawned);
	CHECK(!own->link.respawn_pending && hidden());
	CHECK(host.host_loop.c2s_pending() == 1);
	kernel.world.cached.wac_ticks = 1;
	if (mission_start) {
		role.init_local_player_at_mission_start();
		CHECK(!hidden());
	}
	const uint32_t updates = kernel.world.entity_update_counter;
	role.run_tick(tick_input());
	// The receive pump's join holds the undeployed slot; it clears the body's
	// hidden bit and its own 0x0A's writer sets it again, both inside the
	// receive pump, ahead of the slot lists: the frame counts no human and its
	// update holds, the mission start's unhide or not. (The unhide is counted on
	// the frames the own 0x0B has not yet reached the server: D-NET-NEW-L1-17,
	// the own client's send countdown.)
	// [orig: Server_OnPlayerJoin - the clear @0x51A7DA, its 0x0A @0x51A82D ->
	//  NetPacket_WritePlayerState (+89912 & 0x10) @0x4FF7B8; Server_TickUpdate -
	//  the receive pump @0x51D895, Server_BuildEntitySlotLists @0x51D89A]
	CHECK(own->burst.spawned && own->slot_state == inmatch::PlayerSlotState::Joined);
	CHECK(own->link.respawn_pending);
	CHECK(kernel.world.cached.humans == 0);
	CHECK(kernel.world.entity_update_counter == updates);
	CHECK(hidden());
	role.run_tick(tick_input());
	CHECK(kernel.world.cached.humans == 0);
	CHECK(kernel.world.entity_update_counter == updates);
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

// D-NET-NEW-L4-5: the listen host's own join runs from its C2S 0x0B, queued at
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
	ms::MissionKernel &kernel = *kernel_storage;
	auto role_storage = std::make_unique<inmatch::HostRole>();
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
		cfg.title = &kJo;
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
	CHECK(host.host_loop.c2s_pending() == 1);
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

} // namespace

int main() {
	run_held_host(/*mission_start=*/true);
	run_held_host(/*mission_start=*/false);
	run_own_join_warms_the_wired_body();
	if (failures == 0) std::printf("host_mission_start: all passed\n");
	return failures == 0 ? 0 : 1;
}
