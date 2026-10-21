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

// The admission records of a datagram for a connection whose admission is not yet complete,
// each through its handler's gate in order (none on a rejection, which releases or punts the
// connection). `consumed` receives the records walked: those after the record that completed
// the admission are the in-match switch's.
std::vector<ProtocolMessage> dispatch_admission(const GameConfig &config,
		NapiNPConnection &conn, const std::vector<ProtocolMessage> &messages,
		uint32_t now_tick, std::vector<NapiNPConnection> &roster,
		const ServerDispatchInputs &inputs, size_t *consumed = nullptr);

// The S2C 0x02 padding probe (CNetPlayer_SendFileTransferChunk(0)): its 512-byte body over the
// host context (the host's GetTickCount, the process's file-transfer session id, the padding
// length, then 500 CRT draws), the two words the C2S 0x02 echo must return kept on the
// connection, which then awaits that echo. The C2S 0x01 sends it at once, or under the
// join-ticket gate the enter result's success does. [orig: CNetPlayer_SendFileTransferChunk
//  @0x4C9440; NapiNPServerMsg_HandleJoinRequest @0x512ED0 (the call @0x512FBC);
//  CNapiGameSession_HandlePlayEnterResponse @0x4D1940 (the call @0x4D1C19)]
ProtocolMessage admission_padding_probe(NapiNPConnection &conn, NapiNPServerCtx *ctx);

// The case bodies of the in-match switch.
void dispatch_join_padding_echo(const GameConfig &config, NapiNPConnection &conn,
		uint32_t now_tick, const ServerDispatchInputs &inputs,
		std::vector<ProtocolMessage> &replies);                            // C2S 0x02
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
