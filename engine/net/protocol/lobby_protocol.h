#pragma once

// The NovaWorld lobby service's protocol (NOVAWORLDUDP): the identity block the client's lobby
// connection carries and the service's own, the same in every JO-family executable whatever the
// title. The one source for the lobby client, the hello builder and the session classifier.

namespace opennova {

// The lobby/browser service's own protocol name (docs/net/novaworld-net-re.md §3), which a
// client's lobby connection names and the service's 0x81 answers with.
// [orig: CNapiGameSession_InitNPConnection @0x4d3be0 - the protocol it creates with PN
//  "NOVAWORLDUDP"]
inline constexpr const char *kLobbyProtocolName = "NOVAWORLDUDP";

// The lobby protocol's PV1, which the service's gate compares, JO's and DFX2's alike.
// [orig: CNapiGameSession_InitNPConnection @0x4d3d51, the literal @0x7cc73c]
// [orig: @0x4d3f41 in the twin of CNapiGameSession_InitNPConnection @0x4d3dd0 (dfx2.exe)]
inline constexpr const char *kLobbyPv1 = "0.0.0 2/10/2004 EM";

// The lobby protocol's PV2, which the service's is_server branch compares
// [orig: NapiNPProtocol_HandleClientJoin @0x62b750 - PV2 == proto+364].
inline constexpr const char *kLobbyPv2 = "1";

} // namespace opennova
