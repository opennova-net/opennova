// D-NET-NEW-LP-3: the S2C 0x0F's waypoint records outside the waypoint game types.
//
// Retail's writer puts the player route's records into the 0x0F of every team-1 body, whatever
// the game type; only the client's reader is gated on the waypoint game type, so a Deathmatch
// reader leaves the records unread and takes the first record's pool-3 index as the
// location-name count. A listen host's own client reads its own 0x0F into the one
// g_LocationNames the writer reads, so every later joiner's 0x0F carries that list: retail's
// 03TR Deathmatch 0x0F carries 4 records and then 26 names (its first record's index, 0x1A).
// This test boots a Deathmatch listen host over a mission of our own with the same route
// shape (pool-3 indices 26, 75, 102, 127; a slot no marker fills reads as the zeroed record,
// name 0, not done) and two location labels, and pins the three legs by retail's arithmetic:
// the host's own copy, the later joiner's body, and that joiner's read.
// [orig: NetPacket_WriteWorldStateLoad0x0F @0x502D10 - the team byte @0x502E3A..0x502E41, the
//  route scan @0x502E45..0x502E66, the records @0x502F28..0x502F7A, the names @0x502E83..0x502EDC;
//  NapiNPClientMsg_0x00F @0x42E200 - the count @0x42E359, the game-type gate @0x42E459, the name
//  count @0x42E50B..0x42E51B, the name copy and step @0x42E538..0x42E5A3]
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/null_datagram_socket.h>
#include <runtime/inmatch/server_initial_state.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/mission/mission_text.h>
#include <runtime/world/entity.h>
#include <base/gameprofile/game_type.h>
#include <base/gameprofile/gameprofile.h>
#include <formats/def/reserved_items.h>
#include <net/npwire/ingame_decode.h>
#include <net/title/title_identity.h>

#include "../common/boot_file_source.h"
#include "../common/synthetic_mission.h"

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace opennova;
namespace ms = opennova::mission;

static int failures = 0;
#define CHECK(c)                                                                            \
	do {                                                                                    \
		if (!(c)) {                                                                         \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                        \
			++failures;                                                                     \
		}                                                                                   \
	} while (0)

namespace {

const TitleIdentity &kJo = *title_identity(gameprofile::GAME_JO);

// A Deathmatch mission with the 03TR route shape and two location labels.
bms::File deathmatch_route_mission() {
	bms::File mission = test_mission::two_entity_mission();
	mission.header.attrib_flags = bms::AttribFlags::Deathmatch;
	for (const int32_t name_index : {0, 1}) {
		bms::Entity marker{};
		marker.type = bms::ItemType::Marker;
		marker.type_id = def::DEF_TYPE_NAMED_LOCATION;
		marker.name_index = name_index;
		marker.x = (40 + name_index) << 16;
		marker.y = 20 << 16;
		mission.markers.push_back(marker);
	}
	bms::WaypointRecord route{};
	route.flags = bms::WaypointFlags::PlayerRoute;
	route.marker_count = 4;
	route.waypoint_numbers = {26, 75, 102, 127};
	mission.waypoint_records.push_back(route);
	return mission;
}

// The four records the writer emits: the raw pool-3 index, name 0, not done.
std::vector<uint8_t> route_records() {
	std::vector<uint8_t> out;
	for (const int index : {26, 75, 102, 127})
		out.insert(out.end(), {static_cast<uint8_t>(index), 0, 0, 0, 0});
	return out;
}

// The host's own client reads its own 0x0F (count 4, the records, 2, "Ridge", "Pass") as a
// Deathmatch reader: the name count is the first record's index (26), the names start inside
// the records, and the eight past the body's end are empty.
std::vector<std::string> own_copy_names() {
	std::vector<std::string> names = {"", "", "", "K", "", "", "", "f", "", "", "", "\x7f", "", "", "",
			"\x02", "Ridge", "Pass"};
	names.resize(26);
	return names;
}

void run(std::map<std::string, std::string> &files) {
	auto kernel_storage = std::make_unique<ms::MissionKernel>();
	ms::MissionKernel &kernel = *kernel_storage;
	auto role_storage = std::make_unique<inmatch::HostRole>();
	inmatch::HostRole &role = *role_storage;
	role.bind(kernel);
	inmatch::ListenHostState &host = role.state;
	bms::File mission = deathmatch_route_mission();
	kernel.open_document(mission, "dm_route", test_boot::source_over(&files));
	ms::MissionText text;
	text.loaded = true;
	text.table.sections.push_back({"Locations", 2});
	text.table.entries.push_back({"LOCATION000", "Ridge", {}, 0});
	text.table.entries.push_back({"LOCATION001", "Pass", {}, 0});
	ms::KernelBootOptions options;
	options.mp_session = true;
	options.game_type = game_type::for_mission_mode(bms::selected_game_mode(
			static_cast<bms::AttribFlags>(kernel.mission.header.attrib_flags)));
	CHECK(options.game_type == game_type::kDeathmatch);
	options.bringup_net_session = [&] {
		inmatch::HostConfig cfg;
		cfg.title = &kJo;
		cfg.config.server_name = "dm_route";
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
	inmatch::NullDatagramSocket socket;
	role.set_socket(&socket);
	inmatch::NapiNPServerCtx &ctx = host.host_owner.ctx;
	CHECK(ctx.mission_location_names == (std::vector<std::string>{"Ridge", "Pass"}));
	CHECK(kernel.world.script.waypoints.entries.size() == 4);
	CHECK(host.client_runtime != nullptr);
	if (host.client_runtime == nullptr) return;
	int landed_at = -1;
	for (int frame = 1; frame <= 200 && landed_at < 0; ++frame) {
		inmatch::TickInput in;
		role.run_tick(in);
		if (host.client_runtime->state().world_state.revision != 0) landed_at = frame;
	}
	CHECK(landed_at > 0);

	// The host's own body is team 1, so its own 0x0F carried the records; its own client's
	// Deathmatch read replaced the host's list.
	const inmatch::NapiNPConnection *own = nullptr;
	for (const inmatch::NapiNPConnection &conn : ctx.np_protocol.connection_list)
		if (conn.type == inmatch::NapiNPConnection::kTypeClientSide) own = &conn;
	CHECK(own != nullptr);
	if (own == nullptr) return;
	const world::Entity *body = kernel.world.registry.get(own->link.owned_entity);
	CHECK(body != nullptr && body->team == 1);
	CHECK(host.client_runtime->state().location_names == own_copy_names());
	CHECK(ctx.mission_location_names == own_copy_names());

	// A later joiner's 0x0F: the 535 fixed bytes, count 4, the records, then the host's 26 names.
	const std::vector<uint8_t> later = inmatch::serialize_world_state_load(ctx, *own, 1);
	std::vector<uint8_t> tail = {4, 0};
	const std::vector<uint8_t> records = route_records();
	tail.insert(tail.end(), records.begin(), records.end());
	tail.insert(tail.end(), {26, 0});
	for (const std::string &name : own_copy_names()) {
		tail.insert(tail.end(), name.begin(), name.end());
		tail.push_back(0);
	}
	CHECK(later.size() == 535 + tail.size());
	CHECK(later.size() >= 535 && std::vector<uint8_t>(later.begin() + 535, later.end()) == tail);

	// That joiner reads it as a Deathmatch reader: 26 names from the records on (the records'
	// own 15, the writer's count word 0x1A, then the first ten of the host's list); the rest of
	// the list stays unread.
	WorldStateLoad read;
	CHECK(decode_world_state_load(later.data(), later.size(), read, /*is_waypoint_gametype=*/false));
	std::vector<std::string> joiner = {"", "", "", "K", "", "", "", "f", "", "", "", "\x7f", "", "", "",
			"\x1a"};
	const std::vector<std::string> host_list = own_copy_names();
	joiner.insert(joiner.end(), host_list.begin(), host_list.begin() + 10);
	CHECK(read.waypoint_count == 4 && read.waypoints.empty());
	CHECK(read.team_name_count == 26 && read.team_names == joiner);
	std::size_t rest = 0;
	for (std::size_t i = 10; i < host_list.size(); ++i) rest += host_list[i].size() + 1;
	CHECK(read.unread_bytes == rest);

	// A waypoint-game-type reader takes the records and the names whole.
	WorldStateLoad coop;
	CHECK(decode_world_state_load(later.data(), later.size(), coop, /*is_waypoint_gametype=*/true));
	CHECK(coop.waypoints.size() == 4 && coop.waypoints[0].slot_id == 26 &&
			coop.waypoints[3].slot_id == 127 && coop.team_names == host_list && coop.unread_bytes == 0);
}

} // namespace

int main() {
	std::map<std::string, std::string> files;
	run(files);
	if (failures != 0) {
		std::printf("world_state_waypoints: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("world_state_waypoints: ok\n");
	return 0;
}
