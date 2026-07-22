#pragma once

#include <vector>

#include <npwire/ingame_decode.h>   // EntityClass / PlayerExtendedUplink
#include <npwire/replication_model.h> // GameEntitySnapshot
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
// it with an items.def *_function class-tag resolver [orig: ItemDef+356]. From a bare
// type_id it CANNOT see vehicle classes (that knowledge is items.def-side), so
// decoders that must agree with entity_class_of for pool-1 vehicles layer a learned
// table over it — NetClientView records type->Vehicle from the 0x0D pool-1 spawn
// batch (see NetClientView::classify). The host's chosen compact encoder and the
// client's chosen compact decoder must agree or the record chain desyncs.
EntityClass class_for_type_id(uint16_t type_id);

// The §5.10b replication class a live World entity replicates as. EntityClass::Unknown
// means the entity has no 0x0A compact form and is not streamed (markers/buildings).
EntityClass entity_class_of(const world::Entity &e);

// Synthesize the server-owned wire snapshot of a live World entity — the input the
// §5.9 0x0A builder consumes. Position is the entity's mission-space float lifted to
// i32 16.16 (world::to_fixed). euler_z is the engine heading (entity+16, D-NET-86).
GameEntitySnapshot snapshot_of(const world::Entity &e);

// The §5.10 player compact-record field-17 byte: `(healthTier << 4) | (playerClass & 0xF)`,
// the tier quantized from health/health_max in 16.16 (boundaries 49152 = 0.75 and 28671 =
// 0.4375). Exact inverse of the client apply Entity_SetHealthFromDifficultyByte @0x4AD580,
// which writes the low nibble back to entity->playerClass and re-resolves itemDef from it —
// so a mis-packed byte re-breaks a remote entity every applied frame (the C2S 0x0F flood,
// D-NET-138). [orig: Entity_GetHealthClassification @ 0x4AD4E0]
uint8_t health_classification_byte(int32_t health, int32_t health_max, uint8_t player_class);

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
// `recipient_own` is the handle of THIS recipient's owned player entity: its 0x0C record gets minimap_flags
// bit 0x01 (the "recipient's own player" marker), every OTHER player gets 0x0100 — the per-recipient split a
// same-map retail capture confirmed (2026-07-01). Pass an invalid handle for a recipient-agnostic batch.
OrganicSpawnBatch build_pool0_organic_batch(const world::World &w, world::EntityHandle recipient_own = {});
// pool-1 destructibles / items / vehicles -> S2C 0x0D [orig: serialize_entity_pool_to_packet_0 @0x503940].
PoolSpawnBatch build_pool1_spawn_batch(const world::World &w);
// pool-2 static structures -> S2C 0x10 [orig: sub_5042F0]. Slot-aligned (start_index 0, empty-slot
// sentinels for holes) because the 0x10 record carries no slot id — the client's slot = start+index.
StaticEntityBatch build_pool2_static_batch(const world::World &w);
// pool-3 markers / waypoints / nav-nodes -> S2C 0x20 [orig: serialize_entity_pool_to_packet @0x503460].
Pool3SyncBatch build_pool3_marker_batch(const world::World &w);
// pool-3 SPAWN-POINT markers only (item_id in the kSpawnMarkerStartTypes 60xx family) -> S2C 0x20.
// The small networked subset the client's spawn-select reads via Entity_BuildMapPoiLists @0x42de40 —
// streaming the full pool-3 (incl. every nav waypoint) floods the client (D-NET-98), but the spawn-select
// screen STILL needs the spawn points or the joiner spams C2S 0x0f and never deploys (§5.38c).
Pool3SyncBatch build_pool3_spawn_marker_batch(const world::World &w);

// One S2C 0x18 FULL-ENTITY-SPAWN record (§5.46) for a live World entity — the host's
// reply body to a C2S 0x0F entity-info query, the client's self-heal request for a
// stale/mismatched entity (its 0x0A tail cross-check @0x4307c4 failed for this handle).
// The client DESTROYS + fully REBUILDS the entity from this record. Modeled fields
// come from their live retail counterparts: resolved item id/type, raw AIData name
// gate, entity links, fixed passenger/control/UseGun slots, pose BAM high words, and
// the modeled tail bytes. Remaining source and admission gaps are catalogued under
// docs/net/novaworld-net-re.md D-NET-133.
// [orig: NapiNPServerMsg_HandlePlayerInfoRequest @0x514180 →
// serialize_object_to_buffer @0x504d10]
FullEntitySpawnRecord build_full_entity_spawn(const world::Entity &e,
                                              world::EntityHandle recipient_own = {});

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
// `intent.heading << 16`). On-foot only (carrier_handle = 0xFFFF; the mounted vehicle-local
// transform is deferred). The anti-cheat weapon/fire counters are left 0 — the §5.38a
// receive path has NO counter gate, so the host read-apply ignores them. The 5-byte
// sub-header (handle = the host-assigned wire handle H, item_type_id = e.item_id, sub_op =
// 0x0A) is built by the caller. [orig: Player_BuildTag0CInputBody @0x42A550; inverse of
// NetPacket_SerializePlayerState case 4 @0x4c2042-0x4c20a9.]
PlayerExtendedUplink build_player_uplink(const world::Entity &e, const world::AiEntity &ae);

} // namespace opennova::netsim
