#pragma once

#include <cstdint>
#include <string>

#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>

namespace opennova::world {
class World;
}

namespace opennova::inmatch {

// The slot-table team difference: every active player slot on team 1 counts
// +1, on team 2 -1 (the slot's team byte, not the entity's); 0 outside a
// team game type. [orig: Server_CalcTeamImbalance @0x4FCAE0 — the team bit
//  @0x4FCAEC, the walk @0x4FCB07..0x4FCB36 over slot+4 (active) / slot+416]
int32_t Server_CalcTeamImbalance(const NapiNPServerCtx &ctx);

// Whether the teams want balancing: only on an in-session authority in a team
// game that is not co-op; then always while the previous mission was set up
// under a non-team or co-op type (the rotation's previous-mode word; the live
// type when the context has no rotation), else only with autobalance on and a
// difference above one that reaches both thresholds.
// [orig: Server_ShouldAutoBalance @0x4FCB40 — the session/type gates
//  @0x4FCB40..0x4FCB66, dword_24D212C @0x4FCB68..0x4FCB79, g_AutoBalanceEnabled
//  @0x4FCB81, the thresholds @0x4FCB8A..0x4FCBA4; dword_24D212C's writers
//  ServerConfig_ApplyHostSetting @0x4A658D, Server_InitAllPlayerEntitiesForRound
//  @0x516AD0, UI_HandleHostSessionStart @0x556E1F, HostDialog_StartSession
//  @0x5588AE]
bool Server_ShouldAutoBalance(const NapiNPServerCtx &ctx);

// The round init's team balance: every active slot but the host's own, the
// longest connected first, moves from the larger team to the smaller while
// the difference is at least two and the minimum threshold; a moved slot
// takes the new team (its side's avatar and kit follow the team at its next
// spawn), and a live entity changes team and joins the team-change list.
// [orig: Server_AutoBalanceTeams @0x4FCC30 -- the pair list of (time in the
//  server, slot) over active non-local slots @0x4FCC72..0x4FCC95, the
//  descending shell sort CPairList_ShellSort @0x526C00 (the call @0x4FCCAC),
//  the walk @0x4FCCDC..0x4FCDD8: team 1 -> 2 @0x4FCD0E, team 2 -> 1
//  @0x4FCD8F, the entity's team and avatar @0x4FCD4F..0x4FCD5B /
//  @0x4FCD9F..0x4FCDAC, CBufferList_AddOrFind @0x4FCD67 / @0x4FCDB8]
void Server_AutoBalanceTeams(NapiNPServerCtx &ctx, world::World *world);

// A system line: S2C 0x14 [channel 7][sender 255][text], reliable (class 1,
// 310-flush retention), to every in-match slot (mask 0x80, the listen host
// included); an empty text sends nothing. Authority-only.
// [orig: Server_BroadcastSystemMessage @0x508260 — the gates @0x508270 /
//  @0x508275, NetPacket_WriteTwoBytesAndCString(.., 255, 7, msg) @0x5082BE
//  (byte2 stored first @0x5047C1), SendFiltered(0x14, 1, 310) @0x5082C3]
void Server_BroadcastSystemMessage(NapiNPServerCtx &ctx, const std::string &message);

// The death screen's team change (C2S 0x4D, no body), on the authority. The
// sender's player slot must exist and not be a spectator; a request inside
// change_team_interval_seconds of the last one is refused; TeamChoose
// (mp_attributes 0x4) must be set; with autobalance on the switch must not
// unbalance the teams. Then a team-2 player goes to team 1 (and 1 to 2):
// Server_ChangeEntityTeam, the C2Blue / C2Red system line with the slot's
// name, health and the last attacker cleared, the player-death transaction
// run inline, the dead bit, and the respawn holds (+360 / +364) overwritten
// with change_team_penalty_seconds and the revive window (+368) cleared. A
// slot on another team only stamps. The stamp is taken whenever the gates
// pass. Teams other than 1/2 never switch.
// [orig: NapiNPServerMsg_0x04D_ChangeTeam @0x518F70 — authority @0x518F85,
//  the slot @0x518F91..0x518FA8, spectator @0x518FAE, the interval
//  @0x518FBA..0x518FDF, TeamChoose @0x518FE5, autobalance @0x518FF2..0x519001,
//  team 2 @0x519010 / team 1 @0x519080; Server_ChangeEntityTeam @0x519018 /
//  @0x519086, the line @0x51902E..0x519046 / @0x51909C..0x5190B4, +0x11E /
//  +0x178 @0x51904C..0x519053 / @0x5190BA..0x5190C1, GameEvent_PlayerDeath
//  @0x519059 / @0x5190C7, Flags |= 2 @0x519061 / @0x5190CF, +0x168 / +0x16C
//  @0x51906B..0x519076 / @0x5190D9..0x5190E5, +368 @0x5190EB, the stamp
//  @0x5190F3]
void Server_HandleTeamChangeRequest(NapiNPServerCtx &ctx, NapiNPConnection &conn,
		world::World &world, uint32_t now_ms);

} // namespace opennova::inmatch
