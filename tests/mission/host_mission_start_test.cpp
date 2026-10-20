// The mission start's Player_InitPlayer body leg on a playing host (L7-s1
// FOUND-1): with spawn zones the host's own player joins held, and the add hid
// its body. Every mission start then calls Player_InitPlayer after the round
// init, which clears the local player's hidden bit, so the next server tick's
// slot lists count it as a human: the empty-world hold (humans 0 with a
// started WAC clock) lets that frame's entity update run. The frame's 0x0A
// writer hides the undeployed body again, so the next frame holds.
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
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/world.h>

#include "../common/boot_file_source.h"
#include "../common/synthetic_mission.h"

#include <cstdio>
#include <map>
#include <memory>
#include <string>

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
	// The held own body is hidden after the bring-up; the eager WAC load
	// leaves its clock at 1 [orig: WacScript_InitAndLoad @0x4F9665 /
	// @0x4F9770].
	CHECK(own->link.respawn_pending && hidden());
	kernel.world.cached.wac_ticks = 1;
	if (mission_start) {
		role.init_local_player_at_mission_start();
		CHECK(!hidden());
	}
	const uint32_t updates = kernel.world.entity_update_counter;
	role.run_tick(tick_input());
	if (mission_start) {
		// Counted as a human after the receive pump: the frame's update ran,
		// and its 0x0A writer hid the undeployed body again.
		CHECK(kernel.world.cached.humans == 1);
		CHECK(kernel.world.entity_update_counter == updates + 1);
	} else {
		CHECK(kernel.world.cached.humans == 0);
		CHECK(kernel.world.entity_update_counter == updates);
	}
	CHECK(hidden());
	role.run_tick(tick_input());
	CHECK(kernel.world.cached.humans == 0);
	CHECK(kernel.world.entity_update_counter == updates + (mission_start ? 1u : 0u));
}

} // namespace

int main() {
	run_held_host(/*mission_start=*/true);
	run_held_host(/*mission_start=*/false);
	if (failures == 0) std::printf("host_mission_start: all passed\n");
	return failures == 0 ? 0 : 1;
}
