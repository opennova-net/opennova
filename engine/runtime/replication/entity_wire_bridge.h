#pragma once

#include <cstdint>
#include <vector>

#include <net/npwire/ingame_decode.h>   // EntityClass / PlayerExtendedUplink
#include <net/npwire/replication_model.h> // GameEntitySnapshot
#include <runtime/world/ai.h>                  // AiEntity (engine-frame live pose)
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

namespace opennova::replication {

struct ClientState; // runtime/replication/client_state.h
struct ClientEntityState;

// Wire-shaped player input — the field map of the C2S 0x0C extended uplink (§5.10,
// [orig: Player_BuildTag0CInputBody @ 0x42A550]). ONE struct serves both transports
// (ADR 0012 Decision 2): under SP it feeds the owned pool-0 entity through the
// in-process loopback; under MP the identical fields ARE the C2S uplink a remote
// client sends. apply_player_intent (below) applies it, gated by the witnessed
// read-apply gates (the dead bit, the round-over latch, the slot state, the pre-round).
struct PlayerIntent {
	uint16_t entity_handle = 0;        // owned entity (pool<<12)|slot
	uint16_t item_type_id = 0;
	uint16_t carrier_handle = 0xFFFF;  // GROUND entity the sender stands on (building floor /
	                                   // vehicle deck — any pool 0-4), 0xFFFF = free-standing.
	                                   // When set, pos/heading below are CARRIER-LOCAL
	                                   // (renamed from the vehicle_handle misnomer; witness
	                                   // 2026-07-03, D-NET-151)
	int32_t  pos_x = 0;                // i32 16.16 — world when free, carrier-local when grounded
	int32_t  pos_y = 0;
	int32_t  pos_z = 0;
	int16_t  heading = 0;              // entity+16 yaw intent (carrier-relative when grounded)
	int16_t  pitch = 0;               // entity+0x14 pitch intent (never localized)
	uint8_t  move_input = 0;          // entity+0x12C movement-input byte (the wire off-12 echo
	                                  // source; renamed from the `anim` misnomer, witness 2026-07-02)
	uint8_t  state_flags = 0;         // the RAW uplinked entity+0x24 low byte; the apply REPLACES
	                                  // bits 2-4 (crouch/prone family) with it [orig: @0x4c1e4d
	                                  // `flags ^= (flags ^ wire) & 0x1C`; the old flags_xor
	                                  // xor-delta reading was wrong — D-NET-151]
	uint8_t  equipped_adm_index = 0xFF; // entity+0x2B0 equipped-weapon AdmDef index — ingested
	                                    // from the extended uplink gated g_AdmDefs[idx].category
	                                    // < 11, echoed at 0x0A off-16 [orig: @0x4C20A3]
	                                    // (D-NET-143). 0xFF = none.
	int8_t   analog_x = 0;            // entity+0x130..+0x132 analog control axes (uplink
	int8_t   analog_y = 0;            // off-21..23) — the vehicle motor consumes the
	int8_t   analog_z = 0;            // controlling occupant's axes [orig: @0x48b783]
	uint32_t buttons = 0;             // fire / action bitmask
};

// The single deliberate bridge between the engine/runtime/world runtime entity model
// (world::Entity / EntityRegistry) and the engine/net/novaworld wire model
// (GameEntitySnapshot / the §5.x compact records). This is the ONLY place the two
// representations meet — keeping engine/runtime/world net-agnostic and engine/net/novaworld
// sim-agnostic (ADR 0009/0011).

// Bare wire ids carry no codec discriminator. This recognizes only the one
// built-in player-Person type whose callback is independently witnessed and
// returns Unknown for every other id. Production decode must use an
// ItemReplicationCatalog; Unknown is a fail-closed stop, never "organic Person".
EntityClass class_for_type_id(uint16_t type_id);

// The §5.10b replication class a live World entity replicates as. EntityClass::Unknown
// means the entity has no 0x0A compact form and is not streamed (markers/buildings).
EntityClass entity_class_of(const world::Entity &e);

// Synthesize the server-owned wire snapshot of a live World entity — the input the
// §5.9 0x0A builder consumes. Position is the entity's 16.16 words, raw
// (D-NET-507). euler_z is the engine heading (entity+16, D-NET-86).
GameEntitySnapshot snapshot_of(const world::Entity &e);

// The wire "net_id" of a player row is its character id (entity+0x15C): the
// assigned side's id the add validated against the host's Avatars.def (0 with no
// table, D-NET-137). The host's own present reads the same identity the 0x0C/0x0A
// writers emit.
uint16_t player_wire_net_id(const world::Entity &e);

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
// Each extractor reads only the fields the engine/runtime/world Entity models; the wire fields a
// freshly-promoted static/spawn does not carry (ammo, mount-occupancy block, AI trailer, ...) stay
// zero — faithful for a load-time spawn record, and the flag word each encoder derives
// (encode_*_batch) gates them out. The orientation field is the engine-frame heading BAM
// (90 - yaw)*kBamPerDegree, the same convention snapshot_of / decode_* use (D-NET-86).

// pool-0 organics (AI infantry + players) -> S2C 0x0C [orig: NetPacket_SerializeEntityStatesToBuffer @0x5030a0].
// The batch is the same for every recipient: a player's flags word carries its own entity's bit0
// (on the deploy screen or spectating), never a per-recipient marker (D-NET-136).
OrganicSpawnBatch build_pool0_organic_batch(const world::World &w);
// pool-1 destructibles / items / vehicles -> S2C 0x0D [orig: NetPacket_SerializeEntityPoolToPacket_0 @0x503940].
PoolSpawnBatch build_pool1_spawn_batch(const world::World &w);
// pool-2 static structures -> S2C 0x10 [orig: NetPacket_SerializePool2StaticToBuffer @0x5042f0]. Slot-aligned (start_index 0, an
// empty-slot sentinel for each free slot below the pool's used count, EntityRegistry::pool_used) because the 0x10 record
// carries no slot id: the client's slot = start+index.
// Its zone blocks are the whole pool's as one serializer call's (stream_pool2_zone_blocks).
StaticEntityBatch build_pool2_static_batch(const world::World &w);
// The zone blocks of one serializer call, its page: each record's static asks the zone chain
// (ZoneSlotChain_GetZoneInfo), whose two out bytes are the call's stack locals, written only by a
// hit, so a static the chain does not hold streams the last hit's byte from the same call, then its
// own radius. Before the call's first hit the bytes are uninitialized stack, read here as zero
// (D-NET-394). Each S2C 0x10 page is one call [orig: Server_SendInitialGameStateToPlayer @0x51bba0,
// the call @0x51bc50 per page].
void stream_pool2_zone_blocks(const world::World &w, StaticEntityBatch &page);
// The 0x0D zone bytes of one serializer call, its page: each numbered entity asks the zone chain,
// the outs carried from the call's last hit; before its first hit the rank is the low byte of the
// call's buffer-size argument (0x1000: 0), the zone byte uninitialized stack, read here as the
// entity's own zone number (D-NET-394). [orig: NetPacket_SerializeEntityPoolToPacket_0 @0x503940,
// the call @0x51bcba per page]
void stream_pool1_zone_bytes(const world::World &w, PoolSpawnBatch &page);
// pool-3 markers / waypoints / nav-nodes -> S2C 0x20 [orig: NetPacket_SerializeEntityPoolToPacket @0x503460].
// Slot-aligned like 0x10 (start_index 0, a zero word for each free slot below the pool's used count): the joiner's
// slot = start+index.
Pool3SyncBatch build_pool3_marker_batch(const world::World &w);

// One S2C 0x18 FULL-ENTITY-SPAWN record (§5.46) for a live World entity — the host's
// reply body to a C2S 0x0F entity-info query, the client's self-heal request for a
// stale/mismatched entity (its 0x0A tail cross-check @0x4307c4 failed for this handle).
// The client DESTROYS + fully REBUILDS the entity from this record. Modeled fields
// come from their live retail counterparts: resolved item id/type, raw AIData name
// gate, entity links, fixed passenger/control/UseGun slots, pose BAM high words, and
// the modeled tail bytes. Remaining source and admission gaps are catalogued under
// docs/net/novaworld-net-re.md D-NET-133.
// [orig: NapiNPServerMsg_HandlePlayerInfoRequest @0x514180 →
// NetPacket_SerializeObjectToBuffer @0x504d10]
FullEntitySpawnRecord build_full_entity_spawn(const world::Entity &e);

// Host-side receive-apply of a decoded C2S 0x0C extended (type-10) player uplink to a
// REMOTE PEER entity [orig: NetPacket_SerializePlayerState case 4 @0x4C09C0 -
// @0x4C1D07..0x4C20A9; docs/net/novaworld-net-re.md §5.38a/§5.10], in retail's order:
// the movement-input byte, the state-flag bits 2-4 and the analog axes land whatever
// the body's state; then, unless the body is dead, the round is over, the sender's slot
// is not in game (`slot_in_game`, playerSlot+0x20 == 6) or the pre-round countdown runs,
// it stages the smooth target the body update chases (AiSystem::tick_infantry), stores
// the live yaw/pitch, writes the live position only for a hidden body (Flags bit 0),
// stores the equipped adm index and restarts the chase (progress 0). Per §5.38 /
// ADR-0012 the host never read-applies its own player. Returns false if the handle is
// unresolved, it is the local player, or a pose gate held.
bool apply_player_intent(world::World &world, const PlayerIntent &intent, bool slot_in_game);

// What the client-side interest list reads beyond the uplinking player itself: the
// decoded pool-0/1 entities (a joiner's remote persons live only there; its pool-1
// rows are also materialized into the World at the same handle), the uplinking
// player's own wire handle (its row is skipped like retail's `entity != player`
// test), and the HUD target cursor entity (g_HUDTargetCursorEntity) as a wire
// handle. No replica = no candidates: the list is retail's empty form.
// [orig: Server_BuildEntityPriorityListForPlayer @0x50DF20]
struct UplinkClientInputs {
	const ClientState *replica = nullptr;
	uint16_t self_wire_handle = 0xFFFF;
	uint16_t hud_target_wire_handle = 0xFFFF;
	// The main loop's frame statistics the two stat bytes carry, as their low
	// bytes: the FR counter's average frame rate and its window's CPU share
	// (a retail host logs them per player). [orig: g_StatsAvgFps /
	// g_StatsCpuPercent, written @0x4C1BA2 / @0x4C1BBC; host store
	// playerSlot+0x15F78 / +0x15F79 @0x4c1edd / @0x4c1efa, read by
	// CServerLog_WritePositionRecord @0x4e1b6d]
	int32_t avg_fps = 0;
	int32_t cpu_percent = 0;
	// The uplinking player's own compact record, when this frame's receive
	// folded one ahead of the send block (null otherwise). Its apply re-points
	// the player's ground link, which the writer reads: a record that seats
	// the player names the seat's own ground link.
	// [orig: NetPacket_SerializePlayerState case 2 -- Entity_TryAttachOrDetach
	//  @0x4C1329, then `groundEntity = parentEntity ? parentEntity->groundEntity
	//  : carrier` @0x4C1346..0x4C1358; case 3 reads +0x28 @0x4C141D]
	const ClientEntityState *self_echo = nullptr;
};

// The JOINER-side inverse of apply_player_intent: synthesize the C2S 0x0C extended
// (type-10) player-uplink BODY (the 43-byte PlayerExtendedUplink) from the joiner's own
// live local-player state, sent each frame so the HOST SNAPs it via apply_player_intent.
// Position is the local player's own live Position words (i32 16.16, read through the
// AI slot's entity, the store the host writes back); heading/pitch are the BAM32 high half (the inverse of apply_player_intent's
// `intent.heading << 16`). A live mount_target wins over ground_target; when the selected
// carrier resolves in `world`, the handle plus carrier-local position/heading are emitted.
// Otherwise the existing FFFF/world-pose form is retained. The trailing four
// (handle, score) pairs are the client's own top-4 interest list over `interest`
// (Server_BuildEntityPriorityListForPlayer, called from the serializer's case 3);
// a retail host floors those rows' 0x0A scores and sends them past its distance gate.
// The 5-byte sub-header (handle = the host-assigned wire handle H, item_type_id =
// e.item_id, sub_op = 0x0A) is built by the caller. [orig: Player_BuildTag0CInputBody
// @0x42A550 -> NetPacket_SerializePlayerState case 3 @0x4C1413..0x4C1C9B, the pair
// list call @0x4C1BE9; inverse of case 4 @0x4c2042-0x4c20a9.]
PlayerExtendedUplink build_player_uplink(world::World &world,
                                         const world::Entity &e,
                                         const world::AiEntity &ae,
                                         const UplinkClientInputs &interest = {});

// The HUD target cursor (g_HUDTargetCursorEntity, HUD+0x168) as the uplink's
// interest list reads it, as a wire handle: the aim runtime's lock, else the
// body's head-look target. L answers to its self handle and a materialized
// pool-1..3 row to its own; a decoded remote person, which has no registry
// entity, answers to the aim acquisition's wire row (`aim_wire_person`).
// [orig: HUD_BuildEntityInfo @0x4B87ED..0x4B8825 -- the lock (aiRuntime+0xC)
//  @0x4B87F0, else headLookTarget (+0x344) @0x4B881F; read @0x50E1C6 / @0x50E42C]
uint16_t uplink_hud_target_wire_handle(const world::World &world, const world::AiEntity &ae,
		uint16_t self_wire_handle, uint16_t aim_wire_person);

} // namespace opennova::replication
