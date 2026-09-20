#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <net/npwire/replication_model.h> // PlayerReplicationState, GameEntitySnapshot
#include <runtime/world/world.h>

#include <runtime/replication/connection.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/inmatch/session_transport.h>

namespace opennova::replication {

// In-match message tags (the inner-message tag, not the session opcode) are the
// direction-scoped constants in npwire/ingame_message_id.h (s2c::PER_FRAME_UPDATE etc.).

// The per-connection in-match replication primitives. A host owns a CONNECTION TABLE (the reimpl of
// the original's per-connection fan); both the legacy listen-server binding and inmatch's
// Server_TickUpdate own that table elsewhere (inmatch: NapiNPProtocol.connection_list, each node's
// embedded replication::Connection `link`) and call these two functions per node, so there is ONE
// drain/emit implementation (ADR 0011 / inmatch ROADMAP P4). Faithful frame order
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
void drain_connection_c2s(world::World &world, Connection &conn);

// The per-frame 0x0A byte cap, header included [orig: g_entity_send_budget
// @0xC8FC50, default 600, runtime-set by the BANDWIDTH server command clamped
// 100-1600]. A global like retail's; the host session applies GameConfig's
// value at bring-up. Lowering it rotates entities out of each frame exactly
// the way a populated retail host does (D-NET-154) — the honest subrate lever
// for tests.
void set_entity_send_budget(int bytes);
int entity_send_budget();

// The environment draw/view distance in world units feeding the 0x0A priority
// score's LOS gate and +200 inside-view bonus [orig: word_26C681E, env-written;
// reads @0x50eabb/@0x50eb62]. Zero (the fresh-image default) disables both
// terms exactly as an unwritten retail global does; the sim wires it from the
// same env value the occlusion camera uses. (D-NET-139)
void set_view_distance_units(int units);

// Serialize the live world into ONE S2C 0x0A frame for `conn`, anchored to its
// live owned entity, and host_send it onto that connection's transport. A
// missing, despawned, or lifetime-stale owner returns false WITHOUT advancing
// phase/age/cache/watermark state. `ents` is the world snapshot built ONCE by
// the caller [orig: NapiNPServer_SendToConn @0x4C4F20 per node]. Retail calls
// the writer only for a deployed player slot and derives its reference from
// that slot's live entity; there is no pre-spawn/map fallback frame.
// [orig: Server_SendEntityStateToPlayer @0x517BA0 state==6 gate, recipient eye
// reference @0x517BF5..0x517C13, ++playerSlot+100566 @0x517BE8]
// Drain the world's water-surface crossings into S2C 0x34 messages for one
// connection. Retail fans these to ALIVE players only (send_mask 128) the moment
// a hull or body crosses the plane, positioned at the water surface.
// [orig: Server_SendOverlayActionToAlive @0x50a1b0]
std::vector<std::vector<uint8_t>> build_water_cross_messages(
        const world::World &world);

// The fan's phases lap onto the SIM_REPLICATION_* rows of the world's profile
// (ADR 0043 d5); an inactive profile keeps selection and encoding free of
// clock reads.
bool emit_connection_s2c(const world::World &w, Connection &conn,
                         const std::vector<GameEntitySnapshot> &ents,
                         uint32_t game_type = 0,
                         std::size_t max_frame_body_bytes = 0);

} // namespace opennova::replication
