#pragma once

#include <cstdint>
#include <vector>

#include <net/npwire/protocol_message.h>

#include <runtime/inmatch/server_message_dispatch.h> // GameConfig, ServerDispatchInputs

namespace opennova::world {
class World;
}

// The admission and join legs of the host's C2S dispatch (split off
// server_message_dispatch.cpp): the C2S 0x00 -> 0x01 -> 0x02 admission FSM and
// its S2C 0x00 / 0x02 / metadata replies, the post-admission 0x00 / 0x01 /
// 0x02 cases, and the 0x33 server-info / 0x37 mission-data chunk services.
// dispatch_session_replies keeps the switch and calls each body from its case
// label; the definitions carry the witnesses. The session-info builders and the
// spawn pump's join boundary declared in server_message_dispatch.h are defined
// in server_dispatch_join.cpp too.
namespace opennova::inmatch {

// The admission FSM for a connection whose admission is not yet complete: the
// replies for this datagram (none on a violation, which rejects the
// connection). Every call returns; the in-match switch never runs for it.
std::vector<ProtocolMessage> dispatch_admission(const GameConfig &config,
		NapiNPConnection &conn, const std::vector<ProtocolMessage> &messages,
		uint32_t now_tick, std::vector<NapiNPConnection> &roster, world::World *world,
		const ServerDispatchInputs &inputs);

// The case bodies of the in-match switch.
void dispatch_join(NapiNPConnection &conn, const ProtocolMessage &msg,
		std::vector<ProtocolMessage> &replies);                            // C2S 0x00
void dispatch_join_form_post(uint32_t now_tick, std::vector<ProtocolMessage> &replies); // C2S 0x01
void dispatch_join_padding_echo(const GameConfig &config, NapiNPConnection &conn,
		uint32_t now_tick, std::vector<NapiNPConnection> &roster, world::World *world,
		const ServerDispatchInputs &inputs, std::vector<ProtocolMessage> &replies); // C2S 0x02
void dispatch_file_chunk_request(const GameConfig &config, const ProtocolMessage &msg,
		const ServerDispatchInputs &inputs, std::vector<ProtocolMessage> &replies); // C2S 0x33
void dispatch_mission_chunk_request(const GameConfig &config, const ProtocolMessage &msg,
		const ServerDispatchInputs &inputs, std::vector<ProtocolMessage> &replies); // C2S 0x37

// C2S 0x09, the joiner's player-sync request (its mission load completed): an added slot's
// net player enters game state 9 and the slot the player-sync track (PlayerSlotState::PlayerSync),
// which Server_SendInitialGameStateToPlayer then runs. No reply. The host's own client sends it
// in-process: the producer applies it for the type-2 loopback.
void dispatch_player_sync_request(NapiNPConnection &conn);

} // namespace opennova::inmatch
