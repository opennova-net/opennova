// See slot_staging.h.
#include <runtime/inmatch/slot_staging.h>

#include <runtime/devtools/scripted_input.h> // script_pose_from_mission: the driver's 16.16 conversion
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/world/world.h>

namespace opennova::inmatch {

SlotStagingResult stage_slot_position(NapiNPServerCtx &ctx, int slot, double x, double y,
		double z) {
	SlotStagingResult out;
	if (ctx.is_authority == 0 || ctx.world == nullptr) {
		out.error = "no host session to stage on";
		return out;
	}
	// The slot's row: an added player in that slot (the in-use byte +4).
	// [orig: Server_NotifyPlayerKicked @0x508304..0x508332]
	const NapiNPConnection *holder = nullptr;
	for (const NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (player_slot_active(conn) && static_cast<int>(conn.reply.player_slot) == slot) {
			holder = &conn;
			break;
		}
	}
	if (holder == nullptr) {
		out.status = SlotStagingStatus::NoPlayer;
		out.error = "slot " + std::to_string(slot) + " holds no player";
		return out;
	}
	// The Person at +0. [orig: Server_PositionPlayerForSpawn @0x50CF76 / @0x50CF7C]
	world::Entity *body = ctx.world->registry.get(holder->link.owned_entity);
	if (body == nullptr) {
		out.status = SlotStagingStatus::NoBody;
		out.error = "slot " + std::to_string(slot) + "'s player holds no body";
		return out;
	}
	devtools::ScriptPose pose;
	if (!devtools::script_pose_from_mission(x, y, z, 0.0, 0.0, pose, out.error)) {
		out.status = SlotStagingStatus::OutOfRange;
		return out;
	}
	// The three Position words, nothing else. [orig: Server_PositionPlayerForSpawn
	//  @0x50CFBE / @0x50CFCA / @0x50CFD0]
	body->position.set_q16({pose.position_q16[0], pose.position_q16[1], pose.position_q16[2]});
	out.status = SlotStagingStatus::Staged;
	out.handle = body->handle.packed;
	for (int axis = 0; axis < 3; ++axis) out.position_q16[axis] = pose.position_q16[axis];
	return out;
}

} // namespace opennova::inmatch
