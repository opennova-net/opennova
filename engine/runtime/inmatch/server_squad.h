#pragma once

// THE HOST SIDE OF THE COMMAND MAP: the squad chain (a member's leader slot
// and fireteam, per player slot), the relays of the squad and user-waypoint
// C2S messages, the break-up of a player's squad, and the punt vote.
// Witness record: docs/interface/hud-re.md "The command map" (D-HUD-19) and
// docs/net/novaworld-net-re.md §4.

#include <cstdint>
#include <vector>

#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>

namespace opennova::world {
class World;
}

namespace opennova::inmatch {

// Route one squad / waypoint / punt C2S (0x17, 0x3F, 0x43, 0x44, 0x45, 0x46,
// 0x4B, 0x4F) from `sender`; false for any other tag. Every handler is the
// authority's, from a slot with a live player that is not a spectator (the
// punt vote reads no spectator byte). Recipients are the in-match slots the
// retail send filter admits, each staged on its own transport.
// [orig: NapiNPServerMsg_HandleChatOrWhisper @0x514850 (0x17),
//  NapiNPServerMsg_VoteKick @0x518f10 (0x3F), Server_HandleEntitySync
//  @0x510990 (0x43), NapiNPServerMsg_HandleChatBroadcast @0x510ae0 (0x44),
//  NapiNPServerMsg_0x045_HandleTeamAssignment @0x510c00 (0x45),
//  NapiNPServerMsg_HandleVoteKick @0x510d20 (0x46),
//  NapiNPServer_BroadcastPlayerProfileUpdate @0x510dc0 (0x4B),
//  NapiNPServerMsg_0x04F @0x514a40 (0x4F) — every IDB name but the 0x3F one
//  a misnomer]
bool Server_HandleSquadMessage(NapiNPServerCtx &ctx, NapiNPConnection &sender, uint8_t tag,
		const std::vector<uint8_t> &body, world::World &world);

// NetPacket_WritePlayerChainLink: link `member` to `leader`. Leader 0xFF or
// the member's own slot clears the link; otherwise the leader slot must be in
// range, the walk up the leader's chain must not reach the member (a cycle)
// or leave the slot range, and the member's team must equal the leader's.
// True with the stored link when accepted (the S2C 0x71 goes out), false for
// the silent rejections.
// [orig: NetPacket_WritePlayerChainLink @0x5106d0]
bool Server_LinkSquadMember(NapiNPServerCtx &ctx, NapiNPConnection &member, uint8_t leader,
		uint8_t &stored);

// The break-up of `player`'s squad: its link cleared (0xFF, fireteam 0), S2C
// 0x71 [0xFF][slot] to the in-match slots of its team, S2C 0x72 [0][""] and
// [1][""] to it; then the same for every in-match slot it led.
// [orig: Server_SendPlayerStateAndSquad @0x518b40 (a misnomer); callers
//  Server_ChangeEntityTeam @0x518e92 and Server_HandlePlayerDisconnect
//  @0x51b837]
void Server_DissolveSquadOf(NapiNPServerCtx &ctx, NapiNPConnection &player);

// The punt tally: with voting on and at least the minimum active slots, a
// target whose votes reach round(active x percent) is punted (the reason-40
// "VOTEDOFF" disconnect) each time a vote brings it there, then every voter
// for a punted target is reset to 0xFF.
// [orig: Server_ProcessVoteKickResults @0x511400; the punt
//  Server_SendValidatedChatToPlayer @0x50a210 -> CNapiNPConnection_SendChatMessage
//  @0x4c7ef0 (a misnomer: the disconnect event)]
void Server_ProcessVoteKickResults(NapiNPServerCtx &ctx);

} // namespace opennova::inmatch
