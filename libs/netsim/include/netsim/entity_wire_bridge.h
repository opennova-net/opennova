#pragma once

#include <vector>

#include <novaworld/ingame_decode.h>   // EntityClass / PlayerExtendedUplink
#include <novaworld/replication_min.h> // GameEntitySnapshot
#include <world/ai.h>                  // AiEntity (engine-frame live pose)
#include <world/entity.h>
#include <world/world.h>

#include "netsim/player_intent.h" // PlayerIntent

namespace opennova::netsim {

// The single deliberate bridge between the libs/world runtime entity model
// (world::Entity / EntityRegistry) and the libs/novaworld wire model
// (GameEntitySnapshot / the §5.x compact records). This is the ONLY place the two
// representations meet — keeping libs/world net-agnostic and libs/novaworld
// sim-agnostic (ADR 0009/0011).

// Map a wire type_id to its §5.10b replication class. Phase 1 uses a minimal table
// (the player infantry template vs everything-else-is-infantry); Phase 3 replaces
// it with an items.def *_function class-tag resolver [orig: ItemDef+356]. It MUST
// agree with entity_class_of for any entity that is actually replicated, so the
// host's chosen compact encoder matches the client's chosen compact decoder.
EntityClass class_for_type_id(uint16_t type_id);

// The §5.10b replication class a live World entity replicates as. EntityClass::Unknown
// means the entity has no 0x0A compact form and is not streamed (markers/buildings).
EntityClass entity_class_of(const world::Entity &e);

// Synthesize the server-owned wire snapshot of a live World entity — the input the
// §5.9 0x0A builder consumes. Position is the entity's mission-space float lifted to
// i32 16.16 (world::to_fixed). euler_z is the engine heading (entity+16, D-NET-86).
GameEntitySnapshot snapshot_of(const world::Entity &e);

// Walk the live registry into the replicated entity set. Entities with no 0x0A
// compact form (EntityClass::Unknown) are skipped.
std::vector<GameEntitySnapshot> snapshot_world(const world::World &w);

// ---------------------------------------------------------------------------
// Per-pool LOAD-TIME spawn batches — the full world the host streams to a joiner
// during its world-load sequence [orig: Server_SendInitialGameStateToPlayer @0x51bba0,
// phases 0x10 -> 0x0D -> 0x0C -> 0x20]. SEPARATE from the per-frame 0x0A motion path
// (snapshot_world): these carry IDENTITY/TYPE/SPAWN-POSE for every pool, including the
// statics/markers that have no compact 0x0A form. Routing is by handle.pool(), which
// pool_for_kind already assigns (Organic->0, Item->1, Building->2, Marker->3).
//
// Each extractor reads only the fields the libs/world Entity models; the wire fields a
// freshly-promoted static/spawn does not carry (ammo, weapon block, AI trailer, ...) stay
// zero — faithful for a load-time spawn record, and the flag word each encoder derives
// (encode_*_batch) gates them out. The orientation field is the engine-frame heading BAM
// (90 - yaw)*kBamPerDegree, the same convention snapshot_of / decode_* use (D-NET-86).

// pool-0 organics (AI infantry + players) -> S2C 0x0C [orig: serialize_entity_states_to_buffer @0x5030a0].
OrganicSpawnBatch build_pool0_organic_batch(const world::World &w);
// pool-1 destructibles / items / vehicles -> S2C 0x0D [orig: serialize_entity_pool_to_packet_0 @0x503940].
PoolSpawnBatch build_pool1_spawn_batch(const world::World &w);
// pool-2 static structures -> S2C 0x10 [orig: sub_5042F0]. Slot-aligned (start_index 0, empty-slot
// sentinels for holes) because the 0x10 record carries no slot id — the client's slot = start+index.
StaticEntityBatch build_pool2_static_batch(const world::World &w);
// pool-3 markers / waypoints / nav-nodes -> S2C 0x20 [orig: serialize_entity_pool_to_packet @0x503460].
Pool3SyncBatch build_pool3_marker_batch(const world::World &w);
// pool-3 SPAWN-POINT markers only (item_id in the kSpawnMarkerStartTypes 60xx family) -> S2C 0x20.
// The small networked subset the client's spawn-select reads via Entity_BuildSpawnPointList @0x42de40 —
// streaming the full pool-3 (incl. every nav waypoint) floods the client (D-NET-98), but the spawn-select
// screen STILL needs the spawn points or the joiner spams C2S 0x0f and never deploys (§5.38c).
Pool3SyncBatch build_pool3_spawn_marker_batch(const world::World &w);

// Host-side receive-apply of a decoded C2S 0x0C extended (type-10) player uplink to a
// REMOTE PEER entity — the host-side mover [orig: NetPacket_SerializePlayerState case 4
// @0x4c2042-0x4c20a9; docs/net/novaworld-net-re.md §5.38a/§5.10]. The inverse of
// snapshot_of's two-store read at the wire
// boundary: it SNAPS the registry Entity pose (the store snapshot_of re-broadcasts) and
// mirrors the engine-frame AiEntity (live pose + heading/pitch) + stages the smooth-target
// the CLIENT interpolation consumes, marking the entity net-snapped so the motor skips it
// [orig: Entity_UpdateInfantryAI @0x4b9a03]. Per §5.38 / ADR-0012 this is ONLY ever called
// for a remote peer — the host NEVER read-applies its own player (motor-from-input). Returns
// false if the handle is unresolved, it is the local player, or the movement/spawn gate
// (Entity.flags bit1) is set.
bool apply_player_intent(world::World &world, const PlayerIntent &intent);

// The JOINER-side inverse of apply_player_intent: synthesize the C2S 0x0C extended
// (type-10) player-uplink BODY (the 43-byte PlayerExtendedUplink) from the joiner's own
// live local-player state, sent each frame so the HOST SNAPs it via apply_player_intent.
// Position is the live engine-frame AiEntity.pos[] (i32 16.16 — the exact store the host
// writes back); heading/pitch are the BAM32 high half (the inverse of apply_player_intent's
// `intent.heading << 16`). On-foot only (vehicle_handle = 0xFFFF; the mounted vehicle-local
// transform is deferred). The anti-cheat weapon/fire counters are left 0 — the §5.38a
// receive path has NO counter gate, so the host read-apply ignores them. The 5-byte
// sub-header (handle = the host-assigned wire handle H, item_type_id = e.item_id, sub_op =
// 0x0A) is built by the caller. [orig: Player_BuildTag0CInputBody @0x42A550; inverse of
// NetPacket_SerializePlayerState case 4 @0x4c2042-0x4c20a9.]
PlayerExtendedUplink build_player_uplink(const world::Entity &e, const world::AiEntity &ae);

} // namespace opennova::netsim
