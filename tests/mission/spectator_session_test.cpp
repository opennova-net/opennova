// The death screen's spectator over the role/kernel: the in-process listen
// host turns its own player into a spectator (the F3 path), its own client
// folds the death screen from the loopback 0x75 / 0x0A, and from then on the
// role's input pass and the free-fly motor own the local entity — the free
// sub-mode flies it from the movement keys, the chase sub-mode follows the
// spectate target and steers the orbit instead, and the cycle's wrap back to
// free places the entity on the composed view pose. The replica track legs
// build the map POI list outside the waypoint gametypes.
// [orig: Entity_UpdateInfantryPlayerBody @0x4b40f8..0x4b411a;
//  Camera_UpdateFreeFly @0x4b2980; sub_52AD50 @0x52ad50; sub_52AFF0
//  @0x52b082..0x52b0ee; NapiNPClientMsg_0x00A @0x42ffb8..0x42ffc9;
//  Entity_BuildMapPoiLists @0x42de40]
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/null_datagram_socket.h>
#include <runtime/inmatch/replica_track.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/spectator_session.h>
#include <base/gameprofile/game_type.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include "../common/boot_file_source.h"
#include "../common/synthetic_mission.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

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

using test_boot::source_over;
using test_mission::two_entity_mission;

uint32_t mission_game_type(const bms::File &mission) {
	return game_type::for_mission_mode(bms::selected_game_mode(
			static_cast<bms::AttribFlags>(mission.header.attrib_flags)));
}

inmatch::NapiNPConnection *own_connection(inmatch::ListenHostState &host) {
	for (inmatch::NapiNPConnection &conn : host.host_owner.ctx.np_protocol.connection_list)
		if (conn.type == inmatch::NapiNPConnection::kTypeClientSide) return &conn;
	return nullptr;
}

// One logic tick through the role as the session runs it: the input pass,
// then the tick.
void frame(inmatch::HostRole &role, const inmatch::TickInput &in) {
	role.apply_input(in);
	role.run_tick(in);
}

inmatch::TickInput keys(bool forward = false, bool back = false) {
	inmatch::TickInput in;
	in.player.movement.forward = forward;
	in.player.movement.back = back;
	return in;
}

} // namespace

int main() {
	std::map<std::string, std::string> files;
	ms::MissionKernel kernel;
	inmatch::HostRole role;
	role.bind(kernel);
	inmatch::ListenHostState &host = role.state;
	kernel.open_document(two_entity_mission(), "synth", source_over(&files));
	ms::KernelBootOptions options;
	options.game_type = mission_game_type(kernel.mission);
	options.bringup_net_session = [&] { role.bring_up_singleplayer(); };
	std::string error;
	CHECK(kernel.boot(options, error));
	inmatch::NullDatagramSocket socket;
	role.set_socket(&socket);
	CHECK(host.client_runtime != nullptr);
	if (host.client_runtime == nullptr) return 1;
	inmatch::ClientRuntime &client = *host.client_runtime;
	w::World &world = kernel.world;
	for (int i = 0; i < 4; ++i) frame(role, keys());
	CHECK(!world.spectator.death_screen);

	// --- the spectator transition reaches the own client's death screen ----
	inmatch::NapiNPConnection *self = own_connection(host);
	CHECK(self != nullptr);
	if (self == nullptr) return 1;
	CHECK(inmatch::Server_SetPlayerSpectator(host.host_owner.ctx, *self, world, true));
	for (int i = 0; i < 8 && !client.state().death_screen_active; ++i) frame(role, keys());
	CHECK(client.state().death_screen_active);
	frame(role, keys());
	CHECK(world.spectator.death_screen);
	// The rising edge seats the eye the free-fly camera composes from.
	CHECK(kernel.local.player()->eye_offset_z == 0xD000);
	CHECK(kernel.local.player()->eye_offset_x == 0 && kernel.local.player()->eye_offset_y == 0);

	// --- the free sub-mode flies the local entity from the keys -------------
	{
		w::AiEntity *body = kernel.local.player_ai();
		CHECK(body != nullptr);
		body->pitch = 0;
		kernel.local.input.look_pitch = 0;
		frame(role, keys()); // the pitch mirror settles
		const int32_t x0 = body->pos[0], y0 = body->pos[1], z0 = body->pos[2];
		frame(role, keys(/*forward=*/true));
		const double dx = static_cast<double>(body->pos[0] - x0);
		const double dy = static_cast<double>(body->pos[1] - y0);
		const double moved = std::sqrt(dx * dx + dy * dy);
		CHECK(std::fabs(moved - 10570.0) <= 2.0);
		CHECK(body->pos[2] == z0);
		// The registry mirror follows (the camera, the radar, the uplink).
		CHECK(std::abs(w::to_fixed(kernel.local.player()->position.x) - body->pos[0]) <= 2);
		// No fire leaves the death screen.
		CHECK(!kernel.local.weapon.fire_held);
	}

	// --- the chase sub-mode follows the target and steers the orbit --------
	const w::EntityHandle target = w::spawn_remote_player(world, [] {
		w::PlayerSpawn s;
		s.position = {40, 50, 3};
		s.net_id = 0xFFF1;
		return s;
	}());
	CHECK(target.valid());
	{
		// The own client's walk runs over its replica rows; give the new
		// player its row now rather than waiting on the next records.
		replication::ClientEntityState &row = client.view().state().upsert(target.packed);
		row.type_id = 0x14B9;
		row.cls = EntityClass::Player;
		row.state_flags_known = true;
		row.state_flags = 0;
	}
	inmatch::spectate_action(kernel, client, replication::ClientReplicaPipeline::kSpectateActionCycleMode);
	CHECK(client.state().death_screen_submode == 1);
	CHECK(client.state().spectate_target == target.packed);
	frame(role, keys());
	{
		const w::AiEntity *tb = world.ai.for_handle(target);
		const w::AiEntity *body = kernel.local.player_ai();
		CHECK(tb != nullptr && body != nullptr);
		if (tb != nullptr && body != nullptr) {
			// The local slot runs ahead of the target's: within one tick's motion.
			CHECK(std::abs(body->pos[0] - tb->pos[0]) <= 0x10000);
			CHECK(std::abs(body->pos[1] - tb->pos[1]) <= 0x10000);
			CHECK(body->heading == tb->heading);
		}
		// The forward key zooms the chase in instead of moving.
		const int32_t before = kernel.local.view.chase_distance_q16;
		frame(role, keys(/*forward=*/true));
		CHECK(kernel.local.view.chase_distance_q16 < before);
		CHECK((world.script.input_action_bits & 0x80u) != 0);
	}

	// --- first person, then the wrap back to free places the entity --------
	inmatch::spectate_action(kernel, client, replication::ClientReplicaPipeline::kSpectateActionCycleMode);
	CHECK(client.state().death_screen_submode == 2);
	frame(role, keys());
	{
		const w::AiEntity *tb = world.ai.for_handle(target);
		// First person tracks the target: the tracked entity changed.
		CHECK(kernel.local.view.camera_tracked ==
				(w::kCameraTrackedTargetKey | target.packed));
		CHECK(kernel.local.view.chase_distance_q16 == 0x30000);
		inmatch::spectate_action(kernel, client,
				replication::ClientReplicaPipeline::kSpectateActionCycleMode);
		CHECK(client.state().death_screen_submode == 0);
		const w::AiEntity *body = kernel.local.player_ai();
		if (tb != nullptr && body != nullptr) {
			// The composed first-person view sits at the copied pose, so the
			// placement lands within the eye's pull-back of it.
			CHECK(std::abs(body->pos[0] - tb->pos[0]) <= 0x8000);
			CHECK(std::abs(body->pos[1] - tb->pos[1]) <= 0x8000);
			CHECK(std::abs(body->pos[2] - tb->pos[2]) <= 0x8000);
			CHECK(kernel.local.input.look_heading == body->heading);
		}
	}

	// --- the stamp: a joiner carries its target row's pose -----------------
	{
		replication::ClientEntityState &row = client.view().state().upsert(target.packed);
		row.x = 0x123456;
		row.y = 0x654321;
		row.z = 0x10000;
		row.heading_bam = 0x22222222;
		client.view().state().death_screen_submode = 1;
		client.view().state().spectate_target = target.packed;
		inmatch::stamp_spectator_motor(kernel, &client, /*joiner=*/true);
		CHECK(world.spectator.follows_target());
		CHECK(!world.spectator.target_entity.valid());
		CHECK(world.spectator.target_pos[0] == 0x123456 && world.spectator.target_yaw == 0x22222222);
		inmatch::stamp_spectator_motor(kernel, &client, /*joiner=*/false);
		CHECK(world.spectator.target_entity == target);
		inmatch::stamp_spectator_motor(kernel, nullptr, false);
		CHECK(!world.spectator.death_screen && !world.spectator.has_target);
	}

	// --- the replica track legs ---------------------------------------------
	{
		w::World poi_world;
		poi_world.registry.configure_pool(1, 4);
		w::Entity flag;
		flag.has_item_def = true;
		flag.item_id = 4091;
		CHECK(poi_world.registry.spawn_at(w::EntityHandle::make(1, 2), flag).valid());
		replication::ClientState state;
		inmatch::ReplicaTrackSeen seen;
		state.world_state.revision = 1;
		state.world_state.waypoints_set = false; // a non-waypoint 0x0F
		inmatch::apply_replica_track(poi_world, state, 0x10000, true, seen);
		CHECK(poi_world.script.waypoints.entries.size() == 1);
		CHECK(poi_world.script.waypoints.entries[0].handle() == w::EntityHandle::make(1, 2));
		// The local 0x50 rebuilds it outside the waypoint gametypes only.
		w::Entity bay;
		bay.has_item_def = true;
		bay.item_id = 4100;
		CHECK(poi_world.registry.spawn_at(w::EntityHandle::make(1, 3), bay).valid());
		state.local_team_assigns = 1;
		inmatch::apply_replica_track(poi_world, state, 0x30020, true, seen);
		CHECK(poi_world.script.waypoints.entries.size() == 1);
		state.local_team_assigns = 2;
		inmatch::apply_replica_track(poi_world, state, 0x10000, true, seen);
		CHECK(poi_world.script.waypoints.entries.size() == 2);
		// Nothing new: nothing rebuilt.
		poi_world.script.waypoints.entries.clear();
		inmatch::apply_replica_track(poi_world, state, 0x10000, true, seen);
		CHECK(poi_world.script.waypoints.entries.empty());
	}

	if (failures != 0) {
		std::printf("spectator_session: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("spectator_session: ok\n");
	return 0;
}
