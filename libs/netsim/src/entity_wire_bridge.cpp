#include "netsim/entity_wire_bridge.h"

#include <cmath>      // std::lround

#include <npwire/ingame_decode.h> // network_transform_local_to_world (grounded uplink lift)
#include <world/ai.h>          // AiEntity / AiSystem (engine-frame mirror)
#include <world/geom.h>        // to_fixed / from_fixed
#include <world/spawn_select.h> // kSpawnMarkerStartTypes (the 60xx spawn-point family)
#include <world/zone_chain.h>   // zone_chain_zone_info_byte — the 0x0D zone byte (§5.11)

namespace opennova::netsim {

// The player infantry item template (§5.2a host-built player entity; the same id
// PlayerReplicationState::entity_type_id defaults to).
static constexpr uint16_t kPlayerInfantryTypeId = 0x14B9u;

EntityClass class_for_type_id(uint16_t type_id) {
	// Phase 1 minimal table. Phase 3 derives this from the item's *_function class
	// tag in items.def [orig: ItemDef+356]. Every replicated non-player type is
	// treated as AI infantry for now (org0/org1 — §5.14).
	if (type_id == kPlayerInfantryTypeId) return EntityClass::Player;
	return EntityClass::Infantry;
}

EntityClass entity_class_of(const world::Entity &e) {
	// The resolved items.def class wins: the host's item-traits sweep stamps
	// Entity::net_class_code from the item's *_function tag (ai_function, else move_function)
	// via class_from_tag — the same directive that drives the original's ItemDef+356 serialize
	// callback [orig: admission @0x50e6d3 / dispatch @0x50f2e2 in the 0x0A loop]. This is
	// load-bearing per-ITEM, not per-pool: an ewep emplacement is pool-1 but has its own
	// callback layout — serializing it with the vehicle compact record desynced the retail
	// client mid-frame on EVERY 0x0A (retail-join v13, 2026-07-02).
	if (e.net_class_code != 0xFFu) return static_cast<EntityClass>(e.net_class_code);
	// Unresolved world (no items.def fed — tests / bare CLI): the phase-1 minimal heuristic.
	// Pool-1 items stay Unknown (NOT vehicles) — only a resolved class tag may select the
	// vehicle record.
	if (e.item_id == kPlayerInfantryTypeId) return EntityClass::Player;
	if (e.kind == world::EntityKind::Organic) return EntityClass::Infantry;
	return EntityClass::Unknown;
}

namespace {

// playerClass (entity+0x294) for the wire: a player MUST advertise a valid soldier class
// (5..9) or the JOINER's client skips body-anim channel (+0x188) registration at round-load
// and then cannot move/crouch/prone — the body motor early-bails on a NULL anim channel. The
// client resolves the soldier model from playerClass at round-load, NOT from the wire
// avatar/anim_slot.
//
// CORRECTION 2026-07-27 — that second sentence is true but MISLEADING, and reading it as
// "playerClass picks the character" cost us a live bug. What playerClass resolves at
// round-load is a charattr `*_CAMMO` items.def id, and every MP class resolves to the
// same one, whose graphic is `us01`: that is retail's FALLBACK body, not the character.
// The character is a separate replicated identity — the renderer draws the TWO-PART
// `entity->CharacterEntity` (+0x3C, an Avatars.def combo instance) and falls back to the
// items.def graphic only when that instance or its first model is null
// [orig: Entity_RenderWithLODCallback @0x5d6ef0 @0x5d6fdf..0x5d701e]. We never build such
// an instance, so we sit permanently in the fallback branch and EVERY remote player
// renders as us01 — observed live against a retail host 2026-07-27. Tracked in
// D-PLAYERINFO-1.
// [orig: Game_ReloadEntityModelsAndCallbacks @0x522830 ->
// AnimMap_GetSlotPropertyInt(playerClass) @0x4127b0 -> ADM -> AnimMap_RegisterEntity @0x40bb60;
// class 0 -> slot 15 -> empty ADM -> registration skipped -> Entity_UpdateInfantryPlayerBody
// @0x4b40e0 bails @0x4b4135. re-grill 2026-06-28.] Carry the entity's loadout class; default a
// player to 8 (golden) until per-player loadout class is wired. The [5,9]-else-8 clamp is the
// EXACT retail rule: Server_PlayerAdd @0x51d102 forces player_slot+89820 AND entity+660 to 8
// when the requested class is outside [5,9] (grill 2026-07-01: byte-faithful).
uint8_t player_class_for_wire(const world::Entity &e) {
	if (e.item_id == kPlayerInfantryTypeId && (e.player_class < 5 || e.player_class > 9))
		return 8;
	return e.player_class;
}

} // namespace

uint8_t health_classification_byte(int32_t health, int32_t health_max, uint8_t player_class) {
	// The §5.10 field-17 pack: `(tier << 4) | (playerClass & 0xF)`, tier quantized from
	// Health/healthMax in 16.16 fixed point — tier 2 above 0.75 (49152), tier 1 above 0.4375
	// (28671), else tier 0. The client apply (Entity_SetHealthFromDifficultyByte @0x4AD580)
	// reconstructs the tier MIDPOINT (87.5% / 59.375% / 21.875% of healthMax) with the same
	// two constants, so this pack is its exact inverse. The original's null-entity/null-itemDef
	// paths return tier 2; our snapshot always carries both inputs, so only the healthMax==0
	// divide guard is reachable. [orig: Entity_GetHealthClassification @ 0x4AD4E0]
	const int32_t max = health_max != 0 ? health_max : 1; // [orig: healthMax ? healthMax : 1]
	const int64_t ratio = (static_cast<int64_t>(health) << 16) / max;
	const uint8_t cls = player_class & 0x0Fu;
	if (ratio > 49152) return static_cast<uint8_t>(0x20u | cls);          // tier 2 @0x4ad552
	return static_cast<uint8_t>((ratio > 28671 ? 0x10u : 0x00u) | cls);   // tier 1/0 @0x4ad56e
}

GameEntitySnapshot snapshot_of(const world::Entity &e) {
	GameEntitySnapshot s;
	s.pool = static_cast<uint8_t>(e.handle.pool());
	s.slot = static_cast<uint16_t>(e.handle.slot());
	s.wire_handle = e.handle.packed; // the authoritative registry handle (pool<<12 | slot)
	s.type_id = static_cast<uint16_t>(e.item_id);
	s.flags = 0;
	s.team = e.team;
	// World stores mission-space floats; the wire is i32 16.16 (world::to_fixed).
	s.x = world::to_fixed(e.position.x);
	s.y = world::to_fixed(e.position.y);
	s.z = world::to_fixed(e.position.z);
	// Entity+16 on the wire is a 32-bit engine-frame BAM heading = (90 - mission_yaw) *
	// kBamPerDegree (D-NET-86 / §5.23/§5.24 — the same convention promote.cpp:87 and ai.cpp
	// build from). Entity::yaw is mission yaw in DEGREES, so reconcile our two-store split
	// (Entity::yaw degrees vs AiEntity::heading BAM) at the wire boundary here: the prior
	// `yaw << 16` was both the wrong scale (deg*65536, ~182x off) and missing the (90 - yaw)
	// frame inversion, collapsing nearly every facing to ~yaw 90 -> NPCs faced the wrong way.
	// The encoder takes the rounded high byte and the present inverts (90 - bam/deg) back to
	// mission yaw, so listen-server NPCs now face the same way as the AI-pool present and the
	// local-player avatar (which bypasses this bridge). [orig: Entity_SpawnFromBMSRecord
	// @0x40e9f0; resolves open question Q1]
	constexpr int64_t kBamPerDegree = 11930464; // 2^32 / 360
	s.euler_z = static_cast<int32_t>(static_cast<int64_t>(90 - e.yaw) * kBamPerDegree);
	s.entity_class = entity_class_of(e);
	s.health = e.health; // §5.10 field-17 tier numerator — non-zero keeps the player alive
	// items.def-resolved healthMax when the item-traits sweep stamped it; else the struct's
	// class-8 player default (150) stands (see GameEntitySnapshot::health_max).
	if (e.health_max > 0) s.health_max = e.health_max;
	s.player_class = player_class_for_wire(e); // field-17 low nibble (entity+0x294)
	// Engine pitch BAM (entity+0x14): a pure degree widen — pitch has no (90-x) frame
	// inversion (that is yaw-only, D-NET-86). Entity::pitch is mission degrees.
	s.pitch_bam = static_cast<int32_t>(static_cast<int64_t>(e.pitch) * kBamPerDegree);
	// The +0x12C movement-INPUT byte the owning client uplinked (apply_player_intent ingests
	// it) — NOT the visual anim slot: remote players are motor-driven from replicated input
	// [orig: case-2 apply @0x4c11ec; witness 2026-07-02 corrected the anim_slot misnomer].
	s.move_input_byte = e.net_move_input;
	// The equipped-weapon adm index (entity+0x2B0) — the 0x0A off-16 echo source: uplink
	// ingest for peers, the WPN_M4AUTO spawn default for host-spawned players (D-NET-143).
	s.equipped_adm_index = e.equipped_adm_index;
	// Body-anim wire state (entity+0x2BC/+0x2B8 + the channel ratio), mirrored from the
	// infantry motor each tick (AiSystem::mirror_wire_anim; remote peers get the authority
	// selection pass) — the player record bytes 14/15 sources. (D-NET-159)
	s.anim_state_id = e.net_anim_state;
	s.anim_pending_id = e.net_anim_pending;
	s.anim_channel_ratio = e.net_anim_phase;
	s.veh_bone = e.mounted ? e.mount_bone : 0; // entity+0x157 [orig: mounted-only @0x4c0a1a]
	s.state_flags = static_cast<uint8_t>(e.flags & 0xFF); // entity+0x24 low byte, unmasked
	s.mount_handle = (e.mounted && e.mount_target.valid()) ? e.mount_target.packed : 0xFFFFu;
	// entity+0x28 groundEntity — the standing-on carrier the player record echoes when not
	// mounted [orig: op1 @0x4c0a08 reads +0x28 as the default carrier]. Mirrored from the
	// owner's uplink for read-applied peers (apply_player_intent; D-NET-151).
	s.ground_handle = e.ground_target.valid() ? e.ground_target.packed : 0xFFFFu;
	return s;
}

std::vector<GameEntitySnapshot> snapshot_world(const world::World &w) {
	std::vector<GameEntitySnapshot> out;
	w.registry.for_each([&](const world::Entity &e) {
		GameEntitySnapshot s = snapshot_of(e);
		if (s.entity_class == EntityClass::Unknown) return; // no 0x0A compact form
		// Retail's infantry compact writer reads entity+0x2EC (target heading)
		// and entity+0x2D0 (aim pitch). In the port those animation-owned fields
		// live on AiEntity, so lift them at the one world-to-wire snapshot seam.
		if (s.entity_class == EntityClass::Infantry && w.ai != nullptr) {
			if (const world::AiEntity *ai = w.ai->for_handle(e.handle)) {
				s.infantry_target_heading_bam = ai->inf.target_heading;
				s.infantry_aim_pitch_bam = ai->inf.aim_pitch;
			}
		}
		// Retail's PLAYER compact writer reads entity Pitch directly. The port
		// splits the registry record from AiEntity, and mounted pose keeps the
		// carrier pitch on Entity while preserving the gunner's live look on
		// AiEntity. Rejoin only that split here; snapshot_of's witnessed integer
		// yaw conversion deliberately retains its distinct truncation behavior.
		if (s.entity_class == EntityClass::Player && w.ai != nullptr) {
			if (const world::AiEntity *ai = w.ai->for_handle(e.handle)) {
				s.pitch_bam = ai->pitch;
			}
		}
		// Resolve the record carrier's pose here, where the registry is in reach — the
		// carrier is often a pool-2 STATIC (building) with no snapshot of its own in the
		// 0x0A list. Mount wins over ground [orig: op1 @0x4c0a08]; a stale handle simply
		// leaves the pose invalid and the record falls back to the free-standing form.
		const uint16_t carrier =
				s.mount_handle != 0xFFFFu ? s.mount_handle : s.ground_handle;
		if (carrier != 0xFFFFu) {
			if (const world::Entity *c =
			            w.registry.get(world::EntityHandle{carrier})) {
				constexpr int64_t kBamPerDegree = 11930464; // 2^32 / 360
				s.carrier_pose_valid = true;
				s.carrier_x = world::to_fixed(c->position.x);
				s.carrier_y = world::to_fixed(c->position.y);
				s.carrier_z = world::to_fixed(c->position.z);
				// Same engine-frame conventions as snapshot_of: yaw is (90 - mission)
				// framed, pitch a pure widen (D-NET-86).
				s.carrier_yaw_bam = static_cast<int32_t>(
						static_cast<int64_t>(90 - c->yaw) * kBamPerDegree);
				s.carrier_pitch_bam = static_cast<int32_t>(
						static_cast<int64_t>(c->pitch) * kBamPerDegree);
				s.carrier_roll_bam = static_cast<int32_t>(
						static_cast<int64_t>(c->roll) * kBamPerDegree);
			}
		}
		out.push_back(s);
	});
	return out;
}

namespace {

constexpr int64_t kBamPerDegree = 11930464; // 2^32 / 360 (matches snapshot_of)

// Engine-frame heading BAM the wire carries at entity+16 — (90 - mission_yaw) deg, the
// same convention snapshot_of writes and every spawn decoder reads (D-NET-86).
int32_t engine_heading_bam(int16_t mission_yaw) {
	return static_cast<int32_t>(static_cast<int64_t>(90 - mission_yaw) * kBamPerDegree);
}

// Pitch/roll are pure degree-to-BAM axes; only heading has the 90-degree frame inversion.
int32_t engine_axis_bam(int16_t degrees) {
	return static_cast<int32_t>(static_cast<int64_t>(degrees) * kBamPerDegree);
}

// entity+36 GamePlayerEntity Flags word for a PLAYER spawn record, written verbatim by the
// original serializers [orig: serialize_entity_states_to_buffer @0x5030a0 writes
// *(u16)(entity+36); serialize_object_to_buffer @0x504d10 likewise]. bit 0x100 =
// player/minimap-register (set for EVERY player so the client's handler re-resolves the model
// at round-load, NapiNPClientMsg_0x00C @0x42e91a). bit 0x01: WITNESSED to be PER-ENTITY host
// state, not per-recipient — Server_PlayerAdd @0x51cbc0 sets `entity+36 |= 1` once at add time
// (@0x51d0da, gated on add_event+108 = NapiNPPlayer+0x37, remote adds only; the host's LOCAL
// player takes the early-return path @0x51cc31 and never gets it), and the serializer copies
// entity+36 verbatim with no recipient-conditional logic, so retail cannot vary this bit per
// recipient. The same-map ASH_I5A capture (0x0101 on the joiner's record, 0x0100 on the host's)
// is fully explained by remote-vs-local add. Our per-recipient computation below is wire-
// identical for a host+1-joiner session but DIVERGES for >=3 players (retail would send 0x0101
// for OTHER remote players too) — kept until the NapiNPPlayer+0x37 gate semantics is witnessed;
// docs/net/novaworld-net-re.md (D-NET-136). Carry the movement/spawn gate (0x02) through while
// the entity is still spawning [orig: entity+36 bit 1].
uint16_t player_wire_flags(const world::Entity &e, world::EntityHandle recipient_own) {
	uint16_t flags = 0x0100u;
	if (recipient_own.valid() && e.handle == recipient_own) flags |= 0x01u;
	if ((e.flags & 0x2u) != 0) flags |= 0x2u;
	return flags;
}

// entity+348 (0x15C) — the wire "net_id" is the player's MINIMAP slot id, NOT the WAC SSN
// (e.net_id, which players keep at 0 to stay out of find_by_net_id; D-NET-112 conflated the
// two). The retail host allocates a per-team minimap id here [orig: Server_PlayerAdd @0x51cbc0
// fills player_slot+442 (team 1) / +444 (team 2) — seeded from the JOINING client's own JSP
// fields (jsp[56]/jsp[58], Server_BuildPlayerInfoAndAdd @0x51d560), validated by
// MinimapSlot_HasEntity @0x57b140 and reallocated via lookup_entity_slot_and_pack_entry
// @0x57ad40 when stale; serialized at serialize_entity_states_to_buffer @0x5030a0 name+21 =
// *(u16)(entity+348)]. The REAL packing (witnessed in the packer @0x57ae47 and its decoder
// MinimapSlot_FindByPackedId @0x57a270) is type(bits 0-4) | subtype(5-8) | index(9-14) |
// side(15) over the 288-byte minimap slot array — bit 15 is the nationality ALIGNMENT, not a
// liveness bit (net-re §5.59; game_world.gd's join packer writes it from the selected
// nationality's alignment): golden 0x0200 = index 1 on side A, 0x8207 = type 7 + index 1 on
// side B. It MUST be nonzero: a 0 net_id makes the JOINER's MinimapSlot_HasEntity(0)
// match the first zero-initialized slot, so its handler SKIPS minimap allocation and the remote
// player is left unregistered — the remote-only divergence behind the C2S 0x0F flood grill,
// while the joiner's OWN player is immune (its minimap slot is set by local deploy, not this
// wire record). Our formula below is an ENCODING SHIM, not the retail packing: its team==2
// 0x8000 does land in the right bit (side), but `slot` lands in the `type` field with
// subtype/index left unpopulated. It stays interop-safe because the client
// self-heals any UNMATCHED net_id — NapiNPClientMsg_0x00C @0x42eadb reallocates and overwrites
// entity->NetId when MinimapSlot_HasEntity fails. Faithful port = minimap slot-array alloc;
// docs/net/novaworld-net-re.md (D-NET-137). [golden diff + minimap grill 2026-07-01]
uint16_t player_minimap_net_id(const world::Entity &e) {
	return static_cast<uint16_t>((e.team == 2 ? 0x8000u : 0u) | 0x0200u |
	                             (e.handle.slot() & 0x1Fu));
}

// player_class_for_wire (the [5,9]-else-8 clamp) lives above snapshot_of, which shares it.

} // namespace

OrganicSpawnBatch build_pool0_organic_batch(const world::World &w, world::EntityHandle recipient_own) {
	OrganicSpawnBatch batch;
	w.registry.for_each([&](const world::Entity &e) {
		if (e.handle.pool() != 0) return;
		OrganicSpawnRecord rec;
		rec.slot_id = e.handle.packed;                 // the wire handle (pool<<12|slot)
		rec.has_body = true;
		rec.item_type_id = static_cast<uint16_t>(e.item_id);
		rec.entity_flags = e.owner_connection_id;      // entity+0x78: the owning connection's dcb,
		                                               // stamped at spawn (host loopback / joiner ack).
		                                               // [orig: Server_PlayerAdd @0x51cbc0; D-NET-92/101]
		rec.entity_name = e.name;
		// Player-record wire rules (flags/minimap net_id/playerClass) are shared with the
		// S2C 0x18 repair record — see the witness comments on the helpers above.
		rec.minimap_flags =
				(e.item_id == kPlayerInfantryTypeId) ? player_wire_flags(e, recipient_own) : 0;
		rec.pos_x = world::to_fixed(e.position.x);
		rec.pos_y = world::to_fixed(e.position.y);
		rec.pos_z = world::to_fixed(e.position.z);
		rec.orientation = engine_heading_bam(e.yaw);
		rec.team = e.team;
		// Field 13 = entity+0x374 animSlot, the character-model/anim-set selector — serialized RAW
		// [orig: serialize_entity_states_to_buffer @0x5030a0 reads +0x374 @0x5032b8]. For players the
		// spawn stamped it from the joiner's per-side VCA/VCB join var (golden joiner=4); NEVER the
		// body-anim clip — echoing Entity::body_anim_slot here was the DBuggy1-shadow bug (D-NET-146).
		rec.anim_slot = e.anim_slot;
		// Players: the per-team minimap/char-slot id picked at add (CI0/CI1 join vars) when present,
		// else the D-NET-137 encoding shim (host's own player / var-less peers).
		rec.net_id = (e.item_id == kPlayerInfantryTypeId)
				? (e.minimap_net_id != 0 ? e.minimap_net_id : player_minimap_net_id(e))
				: e.net_id;
		rec.player_class = player_class_for_wire(e);
		batch.records.push_back(std::move(rec));
	});
	batch.entity_count = static_cast<uint16_t>(batch.records.size());
	return batch;
}

FullEntitySpawnRecord build_full_entity_spawn(const world::Entity &e,
                                              world::EntityHandle recipient_own) {
	FullEntitySpawnRecord rec;
	rec.slot_id = e.handle.packed;
	// Both fields are dereferenced from entity+0x20 ItemDef. A null def writes zero for each,
	// which is load-bearing: item_type==0 makes the client stop after destroy+memset instead of
	// rebuilding the slot. [orig: serialize_object_to_buffer @0x504d79/@0x504dc8;
	// NapiNPClientMsg_FullEntitySpawn gate @0x433b5a]
	rec.item_type_id = e.has_item_def ? static_cast<uint16_t>(e.item_id) : 0;
	rec.item_type = e.has_item_def ? e.item_type : 0;
	rec.team = e.team;
	rec.minimap_flags =
			(e.item_id == kPlayerInfantryTypeId) ? player_wire_flags(e, recipient_own) : 0;
	rec.entity_flags = e.owner_connection_id;
	// The name rides only when the resolved ItemDef carries AIData. Use the raw attrib source,
	// rather than name presence or a pool heuristic, so a null/non-AI def emits the required
	// one-byte empty cstr. [orig: serialize_object_to_buffer @0x504e20..0x504e7c]
	if (e.has_item_def && (e.item_attrib & world::kItemAttribAIData) != 0) rec.entity_name = e.name;
	// The three live relationship pointers serialize independently; do not infer one from
	// mounted, because the original simply resolves each stored pointer to its pool handle.
	// [orig: serialize_object_to_buffer @0x504e8c..0x504fb4]
	if (e.primary_occupant.valid()) rec.parent_vehicle_handle = e.primary_occupant.packed;
	if (e.ground_target.valid()) rec.ground_entity_handle = e.ground_target.packed;
	if (e.mount_target.valid()) rec.parent_entity_handle = e.mount_target.packed;
	// A live initialized entity carries invalid handles in every empty slot. Slots 0..7
	// are the sparse passenger mask, slot 8 is ctrlx/drvrx, and slot 9 is UseGun.
	// Entity::seats stays dense for gameplay and carries the fixed retail slot explicitly.
	// [orig: itemDef+604/+605..+614; entity+400..+418]
	if (e.has_item_def) {
		rec.mount_handle_8 = 0xFFFFu;
		rec.mount_handle_9 = 0xFFFFu;
		for (const world::Seat &seat : e.seats) {
			const uint16_t occupant =
					seat.occupant.valid() ? seat.occupant.packed : 0xFFFFu;
			if (seat.retail_slot < 8) {
				rec.seat_mask |= static_cast<uint8_t>(1u << seat.retail_slot);
				rec.mount_handles[seat.retail_slot] = occupant;
			} else if (seat.retail_slot == 8) {
				rec.mount_handle_8 = occupant;
			} else if (seat.retail_slot == 9) {
				rec.mount_handle_9 = occupant;
			}
		}
	}
	rec.pos_x = world::to_fixed(e.position.x);
	rec.pos_y = world::to_fixed(e.position.y);
	rec.pos_z = world::to_fixed(e.position.z);
	// Yaw high word — the client restores Yaw = (i16)heading_hi << 16 (@0x433aa1), so this is
	// the engine-frame heading BAM's top half (same convention as the 0x0C orientation).
	rec.heading_hi = static_cast<uint16_t>(static_cast<uint32_t>(engine_heading_bam(e.yaw)) >> 16);
	rec.pitch_hi = static_cast<uint16_t>(static_cast<uint32_t>(engine_axis_bam(e.pitch)) >> 16);
	rec.ai_state = static_cast<uint8_t>(e.ai_state); // entity+692 low byte, serialized raw
	// Same field sources as the 0x0C organic record: entity+0x374 raw + the per-team minimap id
	// (see build_pool0_organic_batch; D-NET-146/137).
	rec.anim_slot = e.anim_slot;
	rec.net_id = (e.item_id == kPlayerInfantryTypeId)
			? (e.minimap_net_id != 0 ? e.minimap_net_id : player_minimap_net_id(e))
			: e.net_id;
	rec.player_class = player_class_for_wire(e);
	// The wire struct retains its early alert_level name, but the grilled source is refNum.
	// entity+340 remains the sole unmodeled live-record byte and therefore stays zero.
	rec.alert_level = e.ref_num;
	rec.sub_type = e.sub_type;
	return rec;
}

PoolSpawnBatch build_pool1_spawn_batch(const world::World &w) {
	PoolSpawnBatch batch;
	w.registry.for_each([&](const world::Entity &e) {
		if (e.handle.pool() != 1) return;
		PoolSpawnRecord rec;
		rec.slot_id = e.handle.packed;
		rec.item_type_id = static_cast<uint16_t>(e.item_id);
		rec.entity_name = e.name;
		rec.pos_x = world::to_fixed(e.position.x);
		rec.pos_y = world::to_fixed(e.position.y);
		rec.pos_z = world::to_fixed(e.position.z);
		rec.euler_z = engine_heading_bam(e.yaw);
		rec.euler_x = engine_axis_bam(e.pitch);
		rec.euler_y = engine_axis_bam(e.roll);
		rec.team_byte = e.team;
		// items.def addeweap children use retail's existing entity+368
		// relationship in the 0x0D spawn record. Positions remain absolute world
		// coordinates; the client derives the rigid child-to-parent transform only
		// after the complete batch has populated both rows.
		if (e.emplacement_parent.valid()) {
			const world::Entity *parent = w.registry.get(e.emplacement_parent);
			if (parent != nullptr && parent->registry_spawn_id ==
					e.emplacement_parent_spawn_id)
				rec.parent_handle = e.emplacement_parent.packed;
		}
		// Faithful 0x0800 AI-trailer gate: emit the trailer ONLY for AI-capable item defs
		// (items.def ItemDefAttrib & 0x100000 = AIData, resolved into Entity::is_ai_capable). This
		// matches the stock 0x0D decoder's own gate exactly (itemDef.attrib & 0x100000 @0x433327),
		// so it is BOTH byte-faithful (retail emits the trailer iff AI-capable) AND crash-safe (the
		// decoder strcpys the trailer name @0x433370 iff AI-capable, so an AI-capable record always
		// carries a valid in-packet NUL-terminated name). [orig: NapiNPClientMsg_0x00D @0x432c40; D-NET-97]
		if (e.is_ai_capable) {
			rec.ai_name = e.name;          // strcpy source @0x433370 (empty = one 0x00, still safe)
			rec.ai_profile_1 = rec.pos_x;  // retail mirrors pos into the opaque AI profiles (aiSlot+0x10/+0x14)
			rec.ai_profile_2 = rec.pos_y;
			if (rec.ai_name.empty() && !rec.ai_profile_1 && !rec.ai_profile_2)
				rec.ai_profile_1 = 1;      // guarantee the encoder's 0x0800 gate fires even at the world origin
		}
		// The §5.11 zone/trait fields, per the witnessed serializer gates [orig:
		// serialize_entity_pool_to_packet_0 @0x503940]: entity Flags dword (0x20 @0x503ae1),
		// subType (0x80 @0x503e58), refNum (0x40), and the ZONE block — a NUMBERED zone
		// (entity+538) emits 0x2000 + the packed (zoneNumber + 32*rank) byte + the u16 radius
		// (entity+350) [orig: ZoneSlotChain_GetZoneInfo @0x503eeb; @0x503ecc-0x503f08]; an
		// un-numbered SpawnPoint def (attrib 0x40000) emits 0x8000 + the radius alone
		// (@0x503f29). Golden ASH_I5A bunkers (type 0x054F): flags 0x20a1/0x20b1 = zone 2
		// rank 1, radius 70. Plain vehicles carry none of these (all-zero fields keep the
		// gates clear — the golden vehicle records). NOT health: the client spawns 0x0D
		// entities and lifts them to itemDef->healthMax at Game_StartMission's reload
		// (@0x522830) / via 0x18 (@0x433780); the pre-v14 code sent Entity::health here,
		// planting the health VALUE into every vehicle's zone-radius word.
		rec.entity_flags = e.engine_flags;
		rec.action_byte = e.sub_type; // entity+532 [orig: @0x503e58]
		rec.alert_byte = e.ref_num;   // entity+533 [orig: @0x503e3c]
		if (e.zone_number != 0) {
			rec.zone_number_rank = world::zone_chain_zone_info_byte(w.zone_chain, e);
			rec.zone_radius = e.zone_radius;
		} else if (e.is_spawn_point) {
			rec.zone_radius = e.zone_radius; // 0x8000 path (radius-0 defs stay absent —
			                                 // the value-derived flag gate, D-NET-97 note)
		}
		batch.records.push_back(std::move(rec));
	});
	batch.entity_count = static_cast<int16_t>(batch.records.size());
	return batch;
}

StaticEntityBatch build_pool2_static_batch(const world::World &w) {
	// The 0x10 record carries no slot id — the client's slot is start_index + iteration
	// index — so emit slot-aligned: start at 0, one record per slot 0..max_live_slot, with
	// empty-slot sentinels (item_type_id 0) for holes (faithful to the pool cursor walk).
	StaticEntityBatch batch;
	std::vector<const world::Entity *> by_slot;
	int max_slot = -1;
	w.registry.for_each([&](const world::Entity &e) {
		if (e.handle.pool() != 2) return;
		const int slot = e.handle.slot();
		if (slot > max_slot) max_slot = slot;
		if (static_cast<int>(by_slot.size()) <= slot) by_slot.resize(slot + 1, nullptr);
		by_slot[slot] = &e;
	});
	if (max_slot < 0) return batch; // empty pool -> empty batch
	batch.start_index = 0;
	for (int slot = 0; slot <= max_slot; ++slot) {
		StaticEntityRecord rec;
		const world::Entity *e = by_slot[static_cast<size_t>(slot)];
		if (e == nullptr) {
			rec.is_empty_slot = true; // bare [u16 0] sentinel
			batch.records.push_back(rec);
			continue;
		}
		rec.item_type_id = static_cast<uint16_t>(e->item_id);
		rec.pos_x = world::to_fixed(e->position.x);
		rec.pos_y = world::to_fixed(e->position.y);
		rec.pos_z = world::to_fixed(e->position.z);
		rec.euler_z = engine_heading_bam(e->yaw); // entity+16 heading (gates 0x0001 if non-zero)
		rec.euler_x = engine_axis_bam(e->pitch);   // entity+20 pitch (0x0002 when non-zero)
		rec.euler_y = engine_axis_bam(e->roll);    // entity+24 roll (0x0004 when non-zero)
		rec.team_byte = e->team;
		// The D-NET-147 building/armory fields: the composed entity Flags dword (entity+36,
		// gates 0x0020), the BMS ammo byte (entity+290, always present), refNum (entity+533,
		// gates 0x0040) and subType (entity+532, gates 0x0080 — 0xFF on indestructible defs).
		// Golden ASH_I5A buildings: flags 0x0A1, eflags 0x04020400, subType 0xFF, ammo 0xFF.
		// [orig: serialize_pool2_static_to_buffer @0x5042F0 field sources @0x5044e6/@0x504502/
		// @0x504519/@0x504535]
		rec.entity_flags = e->engine_flags;
		rec.ammo_count = e->ammo_count;
		rec.bone_a = e->ref_num;
		rec.bone_b = e->sub_type;
		batch.records.push_back(rec);
	}
	batch.entity_count = static_cast<int16_t>(batch.records.size());
	return batch;
}

static Pool3SyncRecord pool3_record_of(const world::Entity &e) {
	Pool3SyncRecord rec;
	rec.item_type_id = static_cast<uint16_t>(e.item_id);
	rec.net_handle = e.handle.packed;            // entitySlot+124 — the wire handle (always)
	rec.pos_x = world::to_fixed(e.position.x);   // entitySlot+4/8/12 (always)
	rec.pos_y = world::to_fixed(e.position.y);
	rec.pos_z = world::to_fixed(e.position.z);
	rec.movement_val = static_cast<uint32_t>(engine_heading_bam(e.yaw)); // entry+16 BAM (D-NET-59)
	rec.team_byte = e.team;
	return rec;
}

Pool3SyncBatch build_pool3_marker_batch(const world::World &w) {
	Pool3SyncBatch batch;
	w.registry.for_each([&](const world::Entity &e) {
		if (e.handle.pool() != 3) return;
		batch.records.push_back(pool3_record_of(e));
	});
	batch.entity_count = static_cast<int16_t>(batch.records.size());
	return batch;
}

Pool3SyncBatch build_pool3_spawn_marker_batch(const world::World &w) {
	Pool3SyncBatch batch;
	w.registry.for_each([&](const world::Entity &e) {
		if (e.handle.pool() != 3) return;
		// Only the 60xx start-marker family — the spawn points the client's spawn-select reads.
		bool is_spawn = false;
		for (size_t i = 0; i < world::kSpawnMarkerStartTypeCount; ++i) {
			if (e.item_id == world::kSpawnMarkerStartTypes[i]) { is_spawn = true; break; }
		}
		if (!is_spawn) return;
		batch.records.push_back(pool3_record_of(e));
	});
	batch.entity_count = static_cast<int16_t>(batch.records.size());
	return batch;
}

bool apply_player_intent(world::World &world, const PlayerIntent &intent) {
	// 1. Resolve the joiner's owned entity by its wire handle (pool<<12 | slot).
	//    [orig: dispatch_entity_packet_callback @0x4D6A80 resolves g_pool_list[h>>12] and
	//    verifies `entity == *owner_ctx` before invoking the +356 callback — the receive
	//    path has NO entity+286/entity+36 health gate; that gate is send-side only
	//    (Player_BuildTag0CInputBody @0x42A550).]
	world::Entity *ent =
			world.registry.get(world::EntityHandle{static_cast<uint16_t>(intent.entity_handle)});
	if (ent == nullptr) return false;

	// 2. Never read-apply the host's OWN player — it is motor-from-raw-input, never a
	//    self-applied pose (§5.38 / ADR-0012 amendment).
	if (world.cached.local_player.valid() && ent->handle == world.cached.local_player)
		return false;

	// 3. Movement/spawn gate: skip the apply while entity+0x24 bit1 is set (spawning).
	//    [orig: case 4 gate @0x4c2000 `test [edi+24h], 2; jnz skip`.] The host-session
	//    globals the original also gates on (g_spawn_success_gate==0, playerSlot+0x20==6
	//    in-game, dword_C8D824==0 @0x4c200a-0x4c2028) hold for an active in-game peer and
	//    are modeled implicitly here (the SP listen-server only drains C2S for joined peers).
	if ((ent->flags & 0x2u) != 0)
		return false;

	// Engine-frame conversions. Heading/pitch on the extended wire are an i16 sign-extended
	// and << 16 = a full 32-bit BAM — a PURE widen, NOT the (90 - yaw) mission framing the
	// FORWARD snapshot_of applies (the joiner serialized its live entity+0x10, already
	// engine-framed). [orig: case 4 @0x4c1da6 `movsx eax, ax; shl eax, 10h` / @0x4c1dca.]
	int32_t heading_bam = static_cast<int32_t>(intent.heading) << 16;
	const int32_t pitch_bam = static_cast<int32_t>(intent.pitch) << 16;

	// Grounded branch (D-NET-151): carrier_handle != 0xFFFF means the sender stands ON
	// another entity (building floor / vehicle deck — any pool) and pos/heading are
	// CARRIER-LOCAL. Resolve the carrier and lift local -> world with the carrier's pose;
	// the heading composes by plain BAM addition (the original transform's out[3] =
	// local[3] + carrier[3], pitch passes through) [orig: case 4 resolve @0x4c1d07-0x4c1d26,
	// Entity_TransformLocalToWorld call @0x4c1de1, heading add @0x43be7e]. An unresolvable
	// carrier applies the local values RAW — exactly the original's null-carrier leg (no
	// transform, no rejection); our modeled carrier pose is yaw+pitch (roll unmodeled = 0).
	int32_t wire_x = intent.pos_x, wire_y = intent.pos_y, wire_z = intent.pos_z;
	const bool grounded = intent.carrier_handle != 0xFFFFu;
	if (grounded) {
		if (const world::Entity *carrier = world.registry.get(
		            world::EntityHandle{static_cast<uint16_t>(intent.carrier_handle)})) {
			const int32_t carrier_yaw_bam = engine_heading_bam(carrier->yaw);
			const WorldPose w = network_transform_local_to_world(
					intent.pos_x, intent.pos_y, intent.pos_z,
					world::to_fixed(carrier->position.x),
					world::to_fixed(carrier->position.y),
					world::to_fixed(carrier->position.z),
					static_cast<uint32_t>(carrier_yaw_bam),
					static_cast<uint32_t>(static_cast<int64_t>(carrier->pitch) * 11930464),
					0u);
			wire_x = w.x;
			wire_y = w.y;
			wire_z = w.z;
			heading_bam += carrier_yaw_bam; // [orig: out[3] = ref[3] + local[3] @0x43be7e]
		}
	}

	// Mirror the uplinked ground link so the 0x0A echo re-emits it (the client stores its
	// own record's carrier back into groundEntity(+0x28) @0x4c1353 and DETACH-corrects on a
	// mismatch — echoing 0xFFFF at a grounded client is what snapped it, D-NET-151). Retail
	// derives +0x28 from the movement resolver's unconditional CB/terrain ground probe
	// [orig: Entity_RaycastGroundHeightAndObject @0x414370]. Our read-applied remote peers
	// are not re-simulated, so their owner's uplink is authoritative for this field
	// (divergence note in the D-NET-151 entry).
	ent->ground_target = grounded ? world::EntityHandle{static_cast<uint16_t>(
	                                        intent.carrier_handle)}
	                              : world::EntityHandle{};

	// 4. SNAP the registry Entity — the store snapshot_of reads and the S2C 0x0A frame
	//    re-broadcasts. The inverse of snapshot_of's two-store read at the wire boundary.
	//    [orig: case 4 live-pos snap @0x4c2084-0x4c208e + live orientation mirror
	//    @0x4c206a/@0x4c206d.] Free-standing wire position is ABSOLUTE world (no
	//    map-origin add on receive); the grounded branch above already lifted local ->
	//    world.
	ent->position.x = static_cast<float>(world::from_fixed(wire_x));
	ent->position.y = static_cast<float>(world::from_fixed(wire_y));
	ent->position.z = static_cast<float>(world::from_fixed(wire_z));
	// BAM32 -> mission yaw degrees: yaw = 90 - bam / kBamPerDegree (the exact inverse of
	// snapshot_of's `(90 - yaw) * kBamPerDegree`), normalized into [0, 360).
	constexpr double kBamPerDegree = 11930464.0; // 2^32 / 360 (matches snapshot_of)
	const long yaw_deg = std::lround(90.0 - static_cast<double>(heading_bam) / kBamPerDegree);
	ent->yaw = static_cast<int16_t>(((yaw_deg % 360) + 360) % 360);

	// Ingest the uplinked wire-state bytes the 0x0A echo re-broadcasts (the faithful
	// uplink -> entity -> 0x0A loop): the +0x12C movement-input byte [orig: case-4 store; the
	// case-2 remote apply @0x4c11ec motor-drives peers from it] and the state-flags byte —
	// the RAW entity+0x24 low byte whose bits 2-4 REPLACE ours (crouch/prone family)
	// [orig: case-4 apply @0x4c1e4d `flags ^= (flags ^ wire) & 0x1C` — the previous
	// xor-delta apply corrupted already-set stance bits; D-NET-151]. Without this the echo
	// re-emits zeros and remote observers see the peer frozen at idle.
	ent->net_move_input = intent.move_input;
	ent->flags ^= (ent->flags ^ static_cast<uint32_t>(intent.state_flags)) & 0x1Cu;
	// Analog control axes (entity+0x130..) — consumed by the vehicle motor when this
	// player holds a ctrl/drvr seat [orig: case-4 stores beside the move-order byte;
	// reader Entity_UpdateVehiclePhysics @0x48b783].
	ent->net_analog_x = intent.analog_x;
	ent->net_analog_y = intent.analog_y;
	ent->net_analog_z = intent.analog_z;

	// Equipped-weapon adm index (entity+0x2B0), the 0x0A off-16 echo source. Retail gates the
	// ingest by AdmDefs[idx].category < 11 [orig: case-4 store @0x4C20A3]; a table-less world
	// (unit paths — a live host always feeds weapon.def) accepts the byte verbatim, and an
	// index with no table entry (including the 0xFF none sentinel) is NOT stored, mirroring
	// the failed AdmDef_GetEntryByIndex leg. (D-NET-143)
	if (world.weapons.empty()) {
		ent->equipped_adm_index = intent.equipped_adm_index;
	} else if (const world::WeaponTableEntry *we =
	                   world.weapons.by_index(intent.equipped_adm_index)) {
		if (we->category < 11) ent->equipped_adm_index = intent.equipped_adm_index;
	}

	// 5. Mirror the engine-frame store (AiEntity) and stage the smooth-target the CLIENT
	//    interpolation consumes; mark the entity net-snapped so the infantry motor SKIPS it
	//    (the host does not re-simulate a read-applied peer). [orig: case 4 staging +0x234/
	//    240/244 @0x4c2042-0x4c205e, live +4/+0x10/+0x14 mirror, progress +0x27C=0 @0x4c20a9;
	//    motor skip @0x4b9a03.] No AiEntity (peer not AI-attached) -> registry snap stands alone.
	if (world.ai != nullptr) {
		if (world::AiEntity *ae = world.ai->for_handle(ent->handle)) {
			ae->net_is_remote_peer = true;
			ae->pos[0] = wire_x; // live +4/+8/+0xC (world — the grounded branch already lifted)
			ae->pos[1] = wire_y;
			ae->pos[2] = wire_z;
			ae->heading = heading_bam; // live +0x10 (carrier-composed when grounded)
			ae->pitch = pitch_bam;     // live +0x14
			ae->net_smooth_target[0] = wire_x; // +0x234
			ae->net_smooth_target[1] = wire_y; // +0x238
			ae->net_smooth_target[2] = wire_z; // +0x23C
			ae->net_smooth_heading = heading_bam;    // +0x240
			ae->net_smooth_pitch = pitch_bam;        // +0x244
			ae->net_interp_progress = 0;             // +0x27C reset
		}
	}
	return true;
}

PlayerExtendedUplink build_player_uplink(const world::Entity &e, const world::AiEntity &ae) {
	PlayerExtendedUplink up; // wire defaults: carrier_handle 0xFFFF, all counters 0
	// Relationship attach/detach is live, but this uplink still reports every local player as
	// free-standing. Retail sends groundEntity/carrier plus carrier-local pose both for standing
	// platforms [orig: @0x4b3291] and mounted players. Static emplacements mask the difference;
	// moving/rotated carriers need the D-NET-151 local-frame uplink follow-up.
	up.carrier_handle = 0xFFFFu;
	// Live engine-frame pose (the AiEntity store apply_player_intent SNAPs back on receive):
	// pos[] is already i32 16.16; heading/pitch are BAM32 whose HIGH half is the i16 wire field
	// (the exact inverse of apply_player_intent's `intent.heading << 16`). [orig: case 4
	// @0x4c1da6/@0x4c1dca + the live +4/+8/+0xC pos store.]
	up.pos_x = ae.pos[0];
	up.pos_y = ae.pos[1];
	up.pos_z = ae.pos[2];
	up.heading = static_cast<int16_t>(ae.heading >> 16);
	up.pitch = static_cast<int16_t>(ae.pitch >> 16);
	// The +0x12C movement-INPUT byte for our own player (the host ingests + echoes it in our
	// 0x0A record so OTHER clients motor-drive our avatar). Until the local input bitfield is
	// exported from the motor, carry the last known value (0 = idle). [witness 2026-07-02:
	// corrected from the anim_slot misnomer — this byte is locomotion input, not an anim slot.]
	up.move_input_byte = e.net_move_input;
	// The RAW entity+0x24 (Flags) low byte, written verbatim and unmasked — the byte the
	// original serializes straight after the movement-input byte and straight before the
	// analog triplet. The host REPLACES bits 2-4 of its copy from it
	// (`flags ^= (flags ^ wire) & 0x1C` [orig: @0x4c1e4d]), then re-broadcasts its copy raw
	// in every S2C 0x0A player compact record [orig: @0x4c0c7d], and each observer's own
	// body updater re-derives the third-person weapon-channel pose from bit 0x10 — the
	// scoped hold variants [orig: Entity_UpdateInfantryPlayerBody @0x4b5deb]. Leaving this
	// at 0 held the host's copy of our scope/NVG/binocular bits permanently clear, so no
	// other player ever saw this joiner aim down sights. Masking here would be wrong twice
	// over: the original does not mask on the write side, and the receiver already does.
	// [orig: NetPacket_SerializePlayerState case 3 @0x4c1b17 `mov cl, [edi+24h]`]
	up.state_flags_byte = static_cast<uint8_t>(e.flags & 0xFFu);
	// The equipped-weapon adm index for our own player — the host ingests it (category-gated)
	// and echoes it at our 0x0A off-16 so other clients resolve our weapon-anim def. Carries
	// the spawn default (WPN_M4AUTO) until joiner-side weapon switching exports a live value.
	// [orig: the client fills byte 24 from entity+0x2B0; case-4 store @0x4C20A3] (D-NET-143)
	up.equipped_adm_index = e.equipped_adm_index;
	return up;
}

} // namespace opennova::netsim
