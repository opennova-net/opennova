#pragma once

#include <cstdint>
#include <vector>

#include <net/npwire/protocol_message.h>

#include <runtime/inmatch/server_message_dispatch.h> // GameConfig, ServerDispatchInputs

namespace opennova::world {
class World;
}

// The respawn and deploy legs of the host's C2S dispatch (split off
// server_message_dispatch.cpp): the C2S 0x0E deploy-map pick, the 0x41 Respawn
// key, the 0x4D team change and the 0x51 spectator respawn. dispatch_session_replies
// keeps the switch and calls each body from its case label; the definitions carry
// the witnesses. Server_ReleasePlayerDeployment and build_spawn_wave_status_body,
// declared in server_message_dispatch.h, are defined in server_dispatch_respawn.cpp.
namespace opennova::inmatch {

void dispatch_team_change_request(NapiNPConnection &conn, uint32_t now_tick,
		world::World *world, const ServerDispatchInputs &inputs);             // C2S 0x4D
void dispatch_death_timeout_reset(NapiNPConnection &conn, world::World *world,
		const ServerDispatchInputs &inputs);                                  // C2S 0x41
void dispatch_respawn_request(const GameConfig &config, NapiNPConnection &conn,
		const ProtocolMessage &msg, world::World *world, const ServerDispatchInputs &inputs,
		std::vector<ProtocolMessage> &replies);                               // C2S 0x0E
void dispatch_spectator_respawn(const GameConfig &config, NapiNPConnection &conn,
		const ProtocolMessage &msg, const ServerDispatchInputs &inputs);      // C2S 0x51

} // namespace opennova::inmatch
