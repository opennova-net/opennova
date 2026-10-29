#pragma once

// The NAPI library's transport identity: the values every JO-family executable's NAPI port
// compiles in whatever the title, which the in-match protocol's identity block and the NovaWorld
// lobby session's both carry. The one source for every encoder and gate that names them.

namespace opennova {

// The NAPI library's protocol version string (NVS), the same text in every JO-family executable:
// the announce and the join write it and the host's 0x41 / 0x42 gates compare it
// (docs/titles/dfx2-vs-jo-re.md §6.1).
// [orig: the literal @0x7dfcf0, written NapiNPSession_SendAnnouncePacket @0x61fa92 and
//  CNapiNPConnection_SendClientJoin @0x61fea7, compared NapiNPProtocol_HandleClientHello
//  @0x6217cf and NapiNPProtocol_HandleClientJoin @0x62bcdc]
// [orig: the literal @0x7f9280, written @0x707bc2 / @0x707fd7, compared @0x7098ff / @0x713e0c
//  (dfx2.exe)]
inline constexpr const char *kNapiNpVersion =
		"NAPI NP Version 0.0.1 1/12/2004 - 2/20/2004 Milota Copyright 2004 NovaLogic";

// The company (CO) both network inits write, and the lobby session's.
// [orig: CNapiNetwork_Init @0x4ca72f; CNapiGameSession_InitNPConnection @0x4d3cae]
// [orig: @0x4ca89f, @0x4d3e9e (dfx2.exe)]
inline constexpr const char *kNovaLogicCompany = "NovaLogic Inc, Calabasas CA U.S.A.";

} // namespace opennova
