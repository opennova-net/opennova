#pragma once

#include <cstdint>
#include <vector>

#include <npwire/replication_model.h> // PlayerReplicationState, GameEntitySnapshot
#include <world/world.h>

#include "netsim/connection.h"
#include "netsim/entity_wire_bridge.h"
#include "netsim/session_transport.h"

namespace opennova::netsim {

// In-match message tags (the inner-message tag, not the session opcode) are the
// direction-scoped constants in npwire/ingame_message_id.h (s2c::PER_FRAME_UPDATE etc.).

// The per-connection in-match replication primitives. A host owns a CONNECTION TABLE (the reimpl of
// the original's per-connection fan); both the legacy listen-server binding and npruntime's
// Server_TickUpdate own that table elsewhere (npruntime: NapiNPProtocol.connection_list, each node's
// embedded netsim::Connection `link`) and call these two functions per node, so there is ONE
// drain/emit implementation (ADR 0011 / npruntime ROADMAP P4). Faithful frame order
// [orig: Game_ProcessMainFrame @ 0x5263f0]:
//
//   input -> drain_connection_c2s (C2S) -> World::run_logic_tick (WAC/BMS/AI)
//         -> emit_connection_s2c (S2C) -> present
//
// The host's own client is a transport-mode-1 LoopbackChannel connection; a remote LAN peer is a
// UdpSessionTransport connection of the same shape.

// Drain + read-apply the queued C2S 0x0C player uplinks on `conn`'s transport (the SNAP), gated on
// the connection's owned entity [D-NET-119; orig: dispatch_entity_packet_callback @0x4D6A80 verifies
// `entity == *owner_ctx` before NetPacket_SerializePlayerState]. Null transport is a no-op (symmetric
// with emit_connection_s2c). Only sub_op 0x0A (extended) this increment; 0x0B compact is deferred.
// Only the host (authority) receives C2S — a joiner never drains one
// [orig: dispatch_entity_packet_callback @0x4D6A80 gates on g_napi_np_ctx.is_authority].
void drain_connection_c2s(world::World &world, const Connection &conn);

// Serialize the live world into ONE S2C 0x0A frame for `conn`, anchored to its owned entity (or
// `fallback_anchor` when it has none), and host_send it onto that connection's transport. `ents`
// is the world snapshot built ONCE by the caller [orig: NapiNPServer_SendToConn @0x4c4f20 per node].
// `conn` is non-const because each send ADVANCES the connection's 0x0A sub-block phase counter
// [orig: ++playerSlot+100566 in Server_SendEntityStateToPlayer @0x517be8].
void emit_connection_s2c(const world::World &w, Connection &conn,
                         const std::vector<GameEntitySnapshot> &ents,
                         const PlayerReplicationState &fallback_anchor,
                         uint32_t game_type = 0);

} // namespace opennova::netsim
