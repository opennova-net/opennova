#include <runtime/replication/entity_wire_bridge.h>

#include <algorithm>  // std::min / std::sort (the uplink interest list)
#include <climits>    // INT32_MIN (the _ftol2_sse indefinite)
#include <cmath>      // std::lround

#include <net/npwire/ingame_decode.h> // network_transform_local_to_world (grounded uplink lift)
#include <net/npwire/wire_handle.h>   // the wire-side handle packing (pinned below)
#include <runtime/replication/client_state.h>  // the decoded rows the uplink interest list scores
#include <runtime/replication/connection_fan.h> // view_distance_units (word_26C681E)
#include <runtime/terrain_query/height_field.h>  // TerrainHeightField::valid
#include <runtime/world/ai.h>          // AiEntity / AiSystem (engine-frame mirror)
#include <runtime/world/angle.h>       // spawn_angle_bam (the placement angle)
#include <runtime/world/collision.h>   // terrain_clip_segment (the org2 ground-settle tail)
#include <runtime/world/entity.h>      // EntityHandle (pinned below)
#include <runtime/world/geom.h>        // to_fixed / from_fixed
#include <runtime/world/player_spawn.h> // kPlayerInfantryTypeId (pinned below)
#include <runtime/world/infantry.h>    // kInfantryAirborneGap / remote body state
#include <runtime/world/zone_chain.h>   // zone_chain_zone_info_byte — the 0x0D zone byte (§5.11)

namespace opennova::replication {

// npwire's wire_handle packing and world's EntityHandle are the same witnessed
// layout [orig: EntityPool_FindByNetId @ 0x4f0a20]; world stays net-agnostic and
// npwire sim-agnostic, so replication — the one lib that sees both — pins them here.
static_assert(wire_handle::kInvalid == world::EntityHandle::kInvalid);
static_assert(wire_handle::kPoolCount == world::kEntityPoolCount);
static_assert(wire_handle::make(3, 5) == world::EntityHandle::make(3, 5).packed);
static_assert(wire_handle::pool(0x2123) == world::EntityHandle{0x2123}.pool() &&
              wire_handle::slot(0x2123) == world::EntityHandle{0x2123}.slot());
static_assert(kPlayerPersonTypeId == world::kPlayerInfantryTypeId);


EntityClass class_for_type_id(uint16_t type_id) {
	// A bare type id cannot reveal ItemDef+356. Keep the one exact built-in
	// witness and fail closed for everything that requires the item catalog.
	if (type_id == kPlayerPersonTypeId) return EntityClass::Player;
	return EntityClass::Unknown;
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
	// An unresolved World can identify only the exact built-in player Person.
	// EntityKind and pool membership do not determine compact width.
	if (e.item_id == kPlayerPersonTypeId) return EntityClass::Player;
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
	if (e.item_id == kPlayerPersonTypeId && (e.player_class < 5 || e.player_class > 9))
		return 8;
	return e.player_class;
}

// Engine-frame heading BAM the wire carries at entity+16 — (90 - mission_yaw) deg, the
// same convention snapshot_of writes and every spawn decoder reads (D-NET-86). The
// whole-degree mirror is the BMS placement angle, so the dword is the spawn angle
// retail keeps there, low 16 bits zero (world::spawn_angle_bam).
int32_t engine_heading_bam(int16_t mission_yaw) {
	return world::spawn_angle_bam(90 - mission_yaw);
}

// Pitch/roll are pure degree-to-BAM axes; only heading has the 90-degree frame inversion.
int32_t engine_axis_bam(int16_t degrees) {
	return world::spawn_angle_bam(degrees);
}

// The carrier frame a mounted seat-local position is measured against: the one
// BAM32 entity euler retail keeps on both sides of Entity_TransformWorldToLocal.
// A seeded carrier (every vehicle past its first motor tick) poses its riders in
// its live BAM frame (vehicle_mount.cpp entity_local_point_world), so the wire
// undoes that same frame — through the whole-degree mirrors the seat-local bytes
// would drift by up to sin(0.5 deg) x the seat lever arm with every sub-degree of
// carrier attitude. The mirrors remain the unseeded (flat placer) frame.
int32_t carrier_heading_bam(const world::Entity &c) {
	return c.veh.yaw_seeded ? c.veh.yaw_bam : engine_heading_bam(c.yaw);
}
int32_t carrier_pitch_bam(const world::Entity &c) {
	return c.veh.yaw_seeded ? c.veh.air_pitch_bam : engine_axis_bam(c.pitch);
}
int32_t carrier_roll_bam(const world::Entity &c) {
	return c.veh.yaw_seeded ? c.veh.air_roll_bam : engine_axis_bam(c.roll);
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
	// Entity+16 on the wire is a 32-bit engine-frame BAM heading = the spawn angle of
	// (90 - mission_yaw) (D-NET-86 / §5.23/§5.24 — the same convention promote.cpp and ai.cpp
	// build from). Entity::yaw is mission yaw in DEGREES, so reconcile our two-store split
	// (Entity::yaw degrees vs AiEntity::heading BAM) at the wire boundary here: the prior
	// `yaw << 16` was both the wrong scale (deg*65536, ~182x off) and missing the (90 - yaw)
	// frame inversion, collapsing nearly every facing to ~yaw 90 -> NPCs faced the wrong way.
	// The encoder takes the rounded high byte and the present inverts (90 - bam/deg) back to
	// mission yaw, so listen-server NPCs now face the same way as the AI-pool present and the
	// local-player avatar (which bypasses this bridge). [orig: Entity_SpawnFromBMSRecord
	// @0x40e9f0; resolves open question Q1]. A seeded mover keeps the dword itself, so
	// its whole-degree mirror never reaches the wire: the vehicle compact rounds the
	// high half of the full BAM [orig: Entity_SerializeVehicleState @0x460CEC..0x460D0A].
	s.euler_z = carrier_heading_bam(e);
	s.entity_class = entity_class_of(e);
	s.health = e.health; // §5.10 field-17 tier numerator — non-zero keeps the player alive
	// items.def-resolved healthMax when the item-traits sweep stamped it; else the struct's
	// class-8 player default (150) stands (see GameEntitySnapshot::health_max).
	if (e.health_max > 0) s.health_max = e.health_max;
	s.player_class = player_class_for_wire(e); // field-17 low nibble (entity+0x294)
	// Engine pitch/roll BAM (entity+0x14/+0x18): a pure degree widen — neither has the
	// (90-x) frame inversion (that is yaw-only, D-NET-86); a seeded mover's own BAM wins
	// like the heading. The wreck's dead-pose compact carries both
	// [orig: Entity_SerializeVehicleState @0x460D31 / @0x460D52].
	s.pitch_bam = carrier_pitch_bam(e);
	s.roll_bam = carrier_roll_bam(e);
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
	// The live vehicle compact's prediction tail is sourced from the authority
	// motor's command registers, not rendered turret state. Ground families
	// normally leave lateral speed at zero; air families use the same seam.
	s.vehicle_forward_speed_reg = e.veh.cmd_speed;          // vehicleData[136]
	s.vehicle_lateral_speed_reg = e.veh.cmd_lateral_speed; // vehicleData[135]
	s.vehicle_steer_target_bam = e.veh.steer_target_bam;   // vehicleData[132]
	// entity+0xA0 slideDecay — the vertical velocity the joiner's family prediction
	// integrates [orig: the live compact's off-11 source @0x460d5a].
	s.vehicle_vertical_velocity = e.veh.slide_z;
	s.mount_handle = (e.mounted && e.mount_target.valid()) ? e.mount_target.packed : wire_handle::kInvalid;
	// entity+0x28 groundEntity — the standing-on carrier the player record echoes when not
	// mounted [orig: op1 @0x4c0a08 reads +0x28 as the default carrier]. Mirrored from the
	// owner's uplink for read-applied peers (apply_player_intent; D-NET-151).
	s.ground_handle = e.ground_target.valid() ? e.ground_target.packed : wire_handle::kInvalid;
	return s;
}

std::vector<GameEntitySnapshot> snapshot_world(const world::World &w) {
	std::vector<GameEntitySnapshot> out;
	w.registry.for_each([&](const world::Entity &e) {
		// The dismemberment clone is an ordinary pool-0 slot on the wire: the
		// original's 0x0A priority walk admits every live slot with a def
		// (no dead/connection filter) [orig: Server_BuildEntityPriorityList
		// admission @0x50e6cb-0x50e6da], so the clone streams compacts like
		// any NPC corpse. Its section mask never rides the wire — no organic
		// channel carries entity+0x134 (world-wac-ai-re §19.2).
		GameEntitySnapshot s = snapshot_of(e);
		if (s.entity_class == EntityClass::Unknown) return; // no 0x0A compact form
		// Retail's infantry compact writer reads entity+0x2EC (target heading)
		// and entity+0x2D0 (aim pitch). In the port those animation-owned fields
		// live on AiEntity, so lift them at the one world-to-wire snapshot seam.
		if (s.entity_class == EntityClass::Infantry) {
			if (const world::AiEntity *ai = w.ai.for_handle(e.handle)) {
				s.infantry_target_heading_bam = ai->inf.target_heading;
				s.infantry_aim_pitch_bam = ai->inf.aim_pitch;
			}
		}
		// Retail's PLAYER compact writer reads entity Pitch directly. The port
		// splits the registry record from AiEntity, and mounted pose keeps the
		// carrier pitch on Entity while preserving the gunner's live look on
		// AiEntity. Rejoin only that split here; snapshot_of's witnessed integer
		// yaw conversion deliberately retains its distinct truncation behavior.
		if (s.entity_class == EntityClass::Player) {
			if (const world::AiEntity *ai = w.ai.for_handle(e.handle)) {
				s.pitch_bam = ai->pitch;
			}
		}
		// Priority-feed fills (D-NET-139 full terms; the World-aware seam).
		if (s.entity_class == EntityClass::Vehicle) {
			// The per-tick displacement metric off the authority motor's own
			// integration step (vel_x/vel_y/slide_z ARE this tick's deltas):
			// sqrt(dx^2 + dy^2 + (dz/2)^2) >> 6, clamp 255
			// [orig: the build metric @0x50e9e5..0x50ea5a].
			const double mx = static_cast<double>(e.veh.vel_x);
			const double my = static_cast<double>(e.veh.vel_y);
			const double mz = static_cast<double>(e.veh.slide_z >> 1);
			const int32_t mag =
					static_cast<int32_t>(std::sqrt(mx * mx + my * my + mz * mz)) >> 6;
			s.tick_speed_q6 = static_cast<uint8_t>(mag > 255 ? 255 : (mag < 0 ? 0 : mag));
			// A live controller occupies the hull [orig: entity+0x170 @0x50efc9].
			s.occupied = e.primary_occupant.valid();
		}
		if ((s.entity_class == EntityClass::Player ||
		     s.entity_class == EntityClass::Infantry) &&
				e.mounted && e.mount_target.valid()) {
			// The carrier pointer is non-null — the dead-recipient score's
			// 600-point term [orig: entity+0x16C @0x50eb28..0x50eb3f].
			s.mounted = true;
			// Riders lose the +100 standing term unless the carrier is an EWEAP
			// [orig: mountDef+0x54 bit5 @0x50eb08..0x50eb15].
			if (const world::Entity *mount = w.registry.get(e.mount_target)) {
				s.mounted_non_eweap =
						(mount->item_attrib & world::kItemAttribEweap) == 0u;
			}
		}
		// A mountable carried G EWeap overloads the compact player's seat_type
		// byte as the authoritative selected-slot echo. Derive it from the live
		// target and mutable MountSlot bit, never a map/game constant.
		// [orig: carrier +0x326 bit2 / MountSlot+0x5e bit8 @0x4c0a39]
		if (s.entity_class == EntityClass::Player && e.mounted &&
				e.mount_target.valid()) {
			if (const world::Entity *mount = w.registry.get(e.mount_target)) {
				if (mount->has_item_def && mount->item_type != 1u &&
						(mount->item_attrib & world::kItemAttribEweap) != 0u &&
						(mount->emplacement_attachment_flags & 0x02u) != 0u) {
					s.mounted_weapon_seat_type =
							mount->primary_weapon_slot.redirect_to_parent_slot
							? 2u : 1u;
				}
			}
		}
		// Resolve the record carrier's pose here, where the registry is in reach — the
		// carrier is often a pool-2 STATIC (building) with no snapshot of its own in the
		// 0x0A list. The organic records prefer the mount over the ground entity [orig:
		// op1 @0x4c0a08]; the vehicle record reads only its groundEntity (+0x28) [orig:
		// Entity_SerializeVehicleState @0x460b4d]. A stale handle simply leaves the pose
		// invalid and the record falls back to the free-standing form.
		const uint16_t carrier = s.entity_class == EntityClass::Vehicle
				? s.ground_handle
				: (s.mount_handle != wire_handle::kInvalid ? s.mount_handle
				                                            : s.ground_handle);
		if (carrier != wire_handle::kInvalid) {
			if (const world::Entity *c =
			            w.registry.get(world::EntityHandle{carrier})) {
				s.carrier_pose_valid = true;
				s.carrier_x = world::to_fixed(c->position.x);
				s.carrier_y = world::to_fixed(c->position.y);
				s.carrier_z = world::to_fixed(c->position.z);
				// Same engine-frame conventions as snapshot_of: yaw is (90 - mission)
				// framed, pitch a pure widen (D-NET-86) — read from the frame the
				// rider was posed in (carrier_heading_bam).
				s.carrier_yaw_bam = carrier_heading_bam(*c);
				s.carrier_pitch_bam = carrier_pitch_bam(*c);
				s.carrier_roll_bam = carrier_roll_bam(*c);
			}
		}
		out.push_back(s);
	});
	return out;
}

namespace {

// entity+36 GamePlayerEntity Flags word for a PLAYER spawn record, written verbatim by the
// original serializers [orig: NetPacket_SerializeEntityStatesToBuffer @0x5030a0 writes
// *(u16)(entity+36); NetPacket_SerializeObjectToBuffer @0x504d10 likewise]. bit 0x100 =
// player/minimap-register (set for EVERY player so the client's handler re-resolves the model
// at round-load, NapiNPClientMsg_0x00C @0x42e91a). bit 0x01 is PER-ENTITY host state, the same
// for every recipient (the serializer copies entity+36 verbatim): it marks a player still on its
// deploy screen or spectating. Every player is born 0x101 [orig: Entity_SpawnFromAnimSlotProperty
// @0x43c433/@0x43c508]; the join clears bit0 for a non-spectator [orig: Server_OnPlayerJoin
// @0x51a7da] and a spectator add sets it [orig: Server_PlayerAdd @0x51d0da, gated on
// NapiNPPlayer+0x37 = the joiner's JSR join var, the PRE_GAME_MENU SPECTATE box:
// UI_PreGameMenuStateMachine @0x568bf0 -> UI_JoinSelectedSession @0x569c77, parsed
// @0x4c7604, latched @0x512e60 / @0x4c81ff; the host's own player binds through the
// local branch @0x51cc31 and never reaches the stamp]; and every 0x0A the host writes for a player clears
// that player's own bit0, then sets it again while it spectates or its deploy screen holds
// [orig: NetPacket_WritePlayerState @0x4ff6d0, @0x4ff7a1, @0x4ff7b8]. The host keeps that bit on
// the entity (server_spawn.cpp), so the record reads it from there (D-NET-136). Carry the
// movement/spawn gate (0x02) through while the entity is still spawning [orig: entity+36 bit 1].
uint16_t player_wire_flags(const world::Entity &e) {
	return static_cast<uint16_t>(0x0100u |
			((e.flags | e.engine_flags) & world::kEntityFlagParachute) | (e.flags & 0x3u));
}

// The entity's one retail Flags dword as the load-stream serializers read it,
// raw: the runtime word and the spawn-composed word the port splits it into,
// plus the REFLECTABLE bit every vehicle's init sets, which the port keeps as
// the ItemDefType-1 trait. [orig: Entity_InitFromModel @0x40e204..0x40e20a]
uint32_t load_stream_flags_dword(const world::Entity &e) {
	return e.flags | e.engine_flags | (e.item_type == 1 ? world::kEntityFlagReflective : 0u);
}

// The 0x0C / 0x18 record's u16 flags word: entity+36's low half for EVERY entity
// [orig: NetPacket_SerializeEntityStatesToBuffer @0x50324c; NetPacket_SerializeObjectToBuffer
// @0x504df0..0x504e00] - the player composition above for a player, the load-stream dword
// for every other body (an AI corpse streams its dead bit, a vehicle its REFLECTABLE bit;
// D-NET-133).
uint16_t record_flags_word(const world::Entity &e) {
	return e.item_id == kPlayerPersonTypeId
			? player_wire_flags(e)
			: static_cast<uint16_t>(load_stream_flags_dword(e) & 0xFFFFu);
}

// entity+348 (0x15C) — the wire "net_id" is the player's MINIMAP slot id, NOT the WAC SSN
// (e.net_id, which players keep at 0 to stay out of find_by_net_id; D-NET-112 conflated the
// two). The retail host allocates a per-team minimap id here [orig: Server_PlayerAdd @0x51cbc0
// fills player_slot+442 (team 1) / +444 (team 2) — seeded from the JOINING client's own JSP
// fields (jsp[56]/jsp[58], Server_BuildPlayerInfoAndAdd @0x51d560), validated by
// MinimapSlot_HasEntity @0x57b140 and reallocated via EntitySlot_LookupAndPackEntry
// @0x57ad40 when stale; serialized at NetPacket_SerializeEntityStatesToBuffer @0x5030a0 name+21 =
// *(u16)(entity+348)]. The REAL packing (witnessed in the packer @0x57ae47 and its decoder
// MinimapSlot_FindByPackedId @0x57a270) is type(bits 0-4) | subtype(5-8) | index(9-14) |
// side(15) over the 288-byte minimap slot array — bit 15 is the nationality ALIGNMENT, not a
// liveness bit (net-re §5.59; the join packer writes it from the selected
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
} // namespace

static uint16_t player_minimap_net_id(const world::Entity &e) {
	return static_cast<uint16_t>((e.team == 2 ? 0x8000u : 0u) | 0x0200u |
	                             (e.handle.slot() & 0x1Fu));
}

// player_class_for_wire (the [5,9]-else-8 clamp) lives above snapshot_of, which shares it.

uint16_t player_wire_net_id(const world::Entity &e) {
	return e.minimap_net_id != 0 ? e.minimap_net_id : player_minimap_net_id(e);
}

OrganicSpawnBatch build_pool0_organic_batch(const world::World &w) {
	OrganicSpawnBatch batch;
	w.registry.for_each([&](const world::Entity &e) {
		if (e.handle.pool() != 0) return;
		// The dismemberment clone is included: the original's join download
		// serializes every pool-0 slot (single caller
		// Server_SendInitialGameStateToPlayer @0x51bd24), and the 0x0C record
		// carries no section mask — a late joiner materializes the clone as a
		// whole-body corpse, exactly like retail (world-wac-ai-re §19.2).
		OrganicSpawnRecord rec;
		rec.slot_id = e.handle.packed;                 // the wire handle (pool<<12|slot)
		rec.has_body = true;
		rec.item_type_id = static_cast<uint16_t>(e.item_id);
		rec.owner_connection_id = e.owner_connection_id; // entity+0x78: the owning connection's dcb,
		                                               // stamped at spawn (host loopback / joiner ack).
		                                               // [orig: Server_PlayerAdd @0x51cbc0; D-NET-92/101]
		// The entity Name (entity+0xF4), for every organic: a player's own name, an
		// AI's authored one. [orig: NetPacket_SerializeEntityStatesToBuffer
		//  @0x5031FF..0x50323A]
		rec.entity_name = e.display_name;
		// Player-record wire rules (flags/minimap net_id/playerClass) are shared with the
		// S2C 0x18 repair record — see the witness comments on the helpers above.
		rec.minimap_flags = record_flags_word(e);
		rec.pos_x = world::to_fixed(e.position.x);
		rec.pos_y = world::to_fixed(e.position.y);
		rec.pos_z = world::to_fixed(e.position.z);
		rec.orientation = engine_heading_bam(e.yaw);
		rec.team = e.team;
		// Field 13 = entity+0x374 animSlot, the character-model/anim-set selector — serialized RAW
		// [orig: NetPacket_SerializeEntityStatesToBuffer @0x5030a0 reads +0x374 @0x5032b8]. For players the
		// spawn stamped it from the joiner's per-side VCA/VCB join var (golden joiner=4); NEVER the
		// body-anim clip — echoing Entity::body_anim_slot here was the DBuggy1-shadow bug (D-NET-146).
		rec.anim_slot = e.anim_slot;
		// Players: the per-team minimap/char-slot id picked at add (CI0/CI1 join vars) when present,
		// else the D-NET-137 encoding shim (host's own player / var-less peers).
		rec.net_id = (e.item_id == kPlayerPersonTypeId)
				? player_wire_net_id(e)
				: e.net_id;
		rec.player_class = player_class_for_wire(e);
		rec.player_slot_id = e.player_slot_id; // entity+0x154 [orig: @0x503316..0x503327]
		batch.records.push_back(std::move(rec));
	});
	batch.entity_count = static_cast<uint16_t>(batch.records.size());
	return batch;
}

FullEntitySpawnRecord build_full_entity_spawn(const world::Entity &e) {
	FullEntitySpawnRecord rec;
	rec.slot_id = e.handle.packed;
	// Both fields are dereferenced from entity+0x20 ItemDef. A null def writes zero for each,
	// which is load-bearing: item_type==0 makes the client stop after destroy+memset instead of
	// rebuilding the slot. [orig: NetPacket_SerializeObjectToBuffer @0x504d79/@0x504dc8;
	// NapiNPClientMsg_FullEntitySpawn gate @0x433b5a]
	rec.item_type_id = e.has_item_def ? static_cast<uint16_t>(e.item_id) : 0;
	rec.item_type = e.has_item_def ? e.item_type : 0;
	rec.team = e.team;
	rec.minimap_flags = record_flags_word(e);
	rec.entity_flags = e.owner_connection_id;
	// The name rides only when the resolved ItemDef carries AIData. Use the raw attrib source,
	// rather than name presence or a pool heuristic, so a null/non-AI def emits the required
	// one-byte empty cstr. The name is the entity Name, entity+0xF4.
	// [orig: NetPacket_SerializeObjectToBuffer @0x504e20..0x504e7c (`lea edi,[ebp+0F4h]`
	//  @0x504E24)]
	if (e.has_item_def && (e.item_attrib & world::kItemAttribAIData) != 0)
		rec.entity_name = e.display_name;
	// The three live relationship pointers serialize independently; do not infer one from
	// mounted, because the original simply resolves each stored pointer to its pool handle.
	// [orig: NetPacket_SerializeObjectToBuffer @0x504e8c..0x504fb4]
	if (e.primary_occupant.valid()) rec.parent_vehicle_handle = e.primary_occupant.packed;
	if (e.ground_target.valid()) rec.ground_entity_handle = e.ground_target.packed;
	if (e.mount_target.valid()) rec.parent_entity_handle = e.mount_target.packed;
	// A live initialized entity carries invalid handles in every empty slot. Slots 0..7
	// are the sparse passenger mask, slot 8 is ctrlx/drvrx, and slot 9 is UseGun.
	// Entity::seats stays dense for gameplay and carries the fixed retail slot explicitly.
	// [orig: itemDef+604/+605..+614; entity+400..+418]
	if (e.has_item_def) {
		rec.mount_handle_8 = wire_handle::kInvalid;
		rec.mount_handle_9 = wire_handle::kInvalid;
		for (const world::Seat &seat : e.seats) {
			const uint16_t occupant =
					seat.occupant.valid() ? seat.occupant.packed : wire_handle::kInvalid;
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
	// the engine-frame heading BAM's top half (same convention as the 0x0C orientation). The
	// writer truncates the live dwords, so a seeded mover sends its own BAM
	// [orig: NetPacket_SerializeObjectToBuffer movzx word [ebx+12h] @0x505166, [ebx+16h] @0x505179].
	rec.heading_hi = static_cast<uint16_t>(static_cast<uint32_t>(carrier_heading_bam(e)) >> 16);
	rec.pitch_hi = static_cast<uint16_t>(static_cast<uint32_t>(carrier_pitch_bam(e)) >> 16);
	rec.ai_state = static_cast<uint8_t>(e.ai_state); // entity+692 low byte, serialized raw
	// Same field sources as the 0x0C organic record: entity+0x374 raw + the per-team minimap id
	// (see build_pool0_organic_batch; D-NET-146/137).
	rec.anim_slot = e.anim_slot;
	rec.net_id = (e.item_id == kPlayerPersonTypeId)
			? player_wire_net_id(e)
			: e.net_id;
	rec.player_class = player_class_for_wire(e);
	// The wire struct retains its early alert_level name, but the grilled source is refNum.
	rec.player_slot_id = e.player_slot_id; // entity+340 [orig: @0x5051f0..0x5051fd]
	rec.alert_level = e.ref_num;
	rec.sub_type = e.sub_type;
	return rec;
}


// The vehicle brain's +0x318 state byte the 0x0D record streams under field
// 0x1000, rebuilt from the latches the port keeps on the motor state (bit 0
// the claimant engine edge, 1 the movement direction, 2 the lights edge or an
// aircraft's flare latch, 3 skid, 4 collision, 5 the tank pivot, 6/7 the tank
// tumble cues), plus 0x80 while a type-1 profile's part spin runs.
// [orig: NetPacket_SerializeEntityPoolToPacket_0 @0x503E85..0x503EC0, the rate read
//  sub_48F090 @0x48F09A (entity+0x468)]
static uint8_t vehicle_sound_latch_byte(const world::Entity &e, const world::AiEntity &ae) {
	const auto &m = e.veh;
	uint8_t b = 0;
	if (m.engine_sound_latched) b |= 0x01u;
	if (m.reverse_sound_latched) b |= 0x02u;
	if (m.light_sound_latched || m.flare_latched) b |= 0x04u;
	if (m.skid_sound_latched) b |= 0x08u;
	if (m.collision_sound_latched) b |= 0x10u;
	if (m.pivot_sound_latched) b |= 0x20u;
	if (m.tumble_hard_latched) b |= 0x40u;
	if (m.tumble_med_latched) b |= 0x80u;
	if (ae.profile.type == 1 && m.part_spin.rate != 0) b |= 0x80u;
	return b;
}

PoolSpawnBatch build_pool1_spawn_batch(const world::World &w) {
	PoolSpawnBatch batch;
	w.registry.for_each([&](const world::Entity &e) {
		if (e.handle.pool() != 1) return;
		PoolSpawnRecord rec;
		rec.slot_id = e.handle.packed;
		rec.item_type_id = static_cast<uint16_t>(e.item_id);
		// The entity's Name (entity+0xF4, Entity::display_name) rides only for an
		// AIData def (the AI trailer's own gate below); every other record carries
		// the one-byte empty string.
		// [orig: NetPacket_SerializeEntityPoolToPacket_0 @0x503A64..0x503ADF]
		if (e.is_ai_capable) rec.entity_name = e.display_name;
		rec.pos_x = world::to_fixed(e.position.x);
		rec.pos_y = world::to_fixed(e.position.y);
		rec.pos_z = world::to_fixed(e.position.z);
		// The entity+16/+20/+24 dwords, full width: a seeded mover's own BAM, not its
		// whole-degree mirror [orig: NetPacket_SerializeEntityPoolToPacket_0 @0x503B37,
		// @0x503B53, @0x503B6F].
		rec.euler_z = carrier_heading_bam(e);
		rec.euler_x = carrier_pitch_bam(e);
		rec.euler_y = carrier_roll_bam(e);
		rec.team_byte = e.team;
		// The PARENT field is the occupantEntity (+0x170) back-reference: a
		// vehicle's driver, a gun's gunner. An addeweap child's carrier is never
		// written here; it rides the TARGET below (its groundEntity), which the
		// client follows. [orig: NetPacket_SerializeEntityPoolToPacket_0
		// `mov eax, [ebp+170h]` @0x503BC9, flag 0x100 @0x503BD3, the pool walk
		// @0x503BDB..0x503C07]
		if (e.primary_occupant.valid() && w.registry.get(e.primary_occupant) != nullptr)
			rec.parent_handle = e.primary_occupant.packed;
		// The separate flag-0x0200 TARGET field carries the structural carrier
		// (groundEntity/+40) the mounted child rides — retail serializes it
		// from the stored pointer, independent of parent. The joiner's
		// materializer authors its ground_target from THIS field only.
		// The field rides the stored pointer alone: the handle is computed from
		// its pool row whether or not an entity still lives there, so a child
		// whose carrier row was destroyed streams that freed row's handle.
		// [orig: NetPacket_SerializeEntityPoolToPacket_0 target write (entity+40)
		//  @0x503C22..0x503C49, the pool-range walk with no occupancy test;
		//  handler resolve @0x4332bc, store @0x4332d7]
		if (e.ground_target.valid()) rec.target_handle = e.ground_target.packed;
		// Retail's 0x0400 block serializes the fixed mountHandles slots, not
		// the dense gameplay seat-vector order: itemDef+604 supplies the mask
		// for slots 0..7 and entity+416/+418 are slots 8/9. Offered empty
		// passenger seats retain their mask bit and carry 0xFFFF.
		for (const world::Seat &seat : e.seats) {
			const uint16_t occupant = seat.occupant.valid()
					? seat.occupant.packed
					: wire_handle::kInvalid;
			if (seat.retail_slot < 8) {
				rec.seat_mask |= static_cast<uint8_t>(1u << seat.retail_slot);
				rec.mount_handles[seat.retail_slot] = occupant;
			} else if (seat.retail_slot == 8) {
				rec.mount_handle_8 = occupant;
			} else if (seat.retail_slot == 9) {
				rec.mount_handle_9 = occupant;
			}
		}
		// Faithful 0x0800 AI-trailer gate: emit the trailer ONLY for AI-capable item defs
		// (items.def ItemDefAttrib & 0x100000 = AIData, resolved into Entity::is_ai_capable). This
		// matches the stock 0x0D decoder's own gate exactly (itemDef.attrib & 0x100000 @0x433327),
		// so it is BOTH byte-faithful (retail emits the trailer iff AI-capable) AND crash-safe (the
		// decoder strcpys the trailer name @0x433370 iff AI-capable, so an AI-capable record always
		// carries a valid in-packet NUL-terminated name). [orig: NapiNPClientMsg_0x00D @0x432c40; D-NET-97]
		// The trailer rides every AIData def's AI slot (entity+0x68), whatever its
		// values. The two dwords are the slot's +0x10/+0x14, which the entity's
		// model init stamps from its position then and nothing rewrites: the spawn
		// x/y, so a vehicle that drove off (or a wreck parked off the map) still
		// streams where it spawned (the retail load stream shows both). The name
		// is the slot's +156, the record's raw 8-byte ai_textfile.
		// [orig: NetPacket_SerializeEntityPoolToPacket_0 @0x503D3D..0x503DAB;
		//  Entity_InitFromModel @0x40E10C..0x40E11E; Entity_SpawnFromBMSRecord
		//  @0x40ED80/@0x40ED8C]
		if (e.is_ai_capable) {
			rec.has_ai_trailer = true;
			rec.ai_name = e.ai_text_file;  // strcpy source @0x433370 (empty = one 0x00, still safe)
			rec.ai_profile_1 = world::to_fixed(e.spawn_position.x);
			rec.ai_profile_2 = world::to_fixed(e.spawn_position.y);
		}
		// Every row carries its entity+290 byte (the BMS byte-81 ammo count).
		// [orig: NetPacket_SerializeEntityPoolToPacket_0 @0x503D27..0x503D38]
		rec.bone_byte = e.ammo_count;
		// Field 0x1000 rides every row with a vehicle brain (entity+0x64): the
		// brain's +0x318 state byte, 0x00 on a parked hull.
		// [orig: NetPacket_SerializeEntityPoolToPacket_0 @0x503E7F..0x503EC0]
		if (const world::AiEntity *brain = w.ai.for_handle(e.handle)) {
			rec.has_sound_latch_byte = true;
			rec.sound_latch_byte = vehicle_sound_latch_byte(e, *brain);
		}
		// The §5.11 zone/trait fields, per the witnessed serializer gates [orig:
		// NetPacket_SerializeEntityPoolToPacket_0 @0x503940]: entity Flags dword (0x20 @0x503ae1),
		// subType (0x80 @0x503e58), refNum (0x40), and the ZONE block — a NUMBERED zone
		// (entity+538) emits 0x2000 + the packed (zoneNumber + 32*rank) byte + the u16 radius
		// (entity+350) [orig: ZoneSlotChain_GetZoneInfo @0x503eeb; @0x503ecc-0x503f08]; an
		// un-numbered SpawnPoint def (attrib 0x40000) emits 0x8000 + the radius alone
		// (@0x503f29). Golden ASH_I5A bunkers (type 0x054F): flags 0x20a1/0x20b1 = zone 2
		// rank 1, radius 70. Plain vehicles leave the subType, refNum and zone gates
		// clear, but never the Flags one. NOT health: the client spawns 0x0D
		// entities and lifts them to itemDef->healthMax at Game_StartMission's reload
		// (@0x522830) / via 0x18 (@0x433780); the pre-v14 code sent Entity::health here,
		// planting the health VALUE into every vehicle's zone-radius word.
		// The Flags field is the entity's live dword, raw: the runtime word and the
		// spawn-composed word the port splits it into, plus the REFLECTABLE bit every
		// vehicle's init sets, which the port keeps as the ItemDefType-1 trait. So every
		// vehicle emits field 0x20: the retail load stream in
		// fixtures/novaworld/run_20260426_120859/server_load_packets.nwmsg carries 0x20400
		// for each live vehicle (0x20000 is its motor's per-tick bit) and 0x406 for each
		// wreck. [orig: NetPacket_SerializeEntityPoolToPacket_0 `mov edx,[ebp+24h]; test edx,edx`
		//  @0x503ae1..0x503aed; Entity_UpdateVehiclePhysics @0x48d451]
		rec.entity_flags = load_stream_flags_dword(e);
		rec.action_byte = e.sub_type; // entity+532 [orig: @0x503e58]
		rec.alert_byte = e.ref_num;   // entity+533 [orig: @0x503e3c]
		// The zone block rides the zone number byte, whatever its packed info byte,
		// and else a SpawnPoint def, whatever its radius: a radius-0 SpawnPoint still
		// emits 0x8000 and its zero word. [orig: `cmp byte [ebp+21Ah],0`
		//  @0x503ECC..0x503ED3; `test dword [def+54h],40000h` @0x503F1F..0x503F43]
		if (e.zone_number != 0) {
			rec.has_zone_number_rank = true;
			rec.zone_number_rank = world::zone_chain_zone_info_byte(w.zones.chain, e);
			rec.zone_radius = e.zone_radius;
		} else if (e.is_spawn_point) {
			rec.has_zone_radius_alt = true;
			rec.zone_radius = e.zone_radius;
		}
		// A palm def (its damage callback) or a psec mover streams the low byte of
		// entity+0x270, the palm's standing/falling state or a piece's type,
		// whatever its value. [orig: NetPacket_SerializeEntityPoolToPacket_0
		//  @0x503F4C..0x503F80]
		if (e.palm_state_streamed) {
			rec.has_difficulty_byte = true;
			rec.difficulty_byte = static_cast<uint8_t>(e.palm_state);
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
		// A door def's section word: entity+308 with bits 1..count re-read from
		// its door rows, carried whenever the def's signed door byte (the low
		// byte of def+0x890) is nonzero. [orig: NetPacket_SerializePool2StaticToBuffer
		// @0x504432..0x5044B3 — the attrib-byte sign test @0x50443F]
		if ((e->item_attrib & 0x80u) != 0) {
			const int count = static_cast<int8_t>(e->deathtime_ticks & 0xFF);
			rec.section_mask = static_cast<int32_t>(
					w.doors.wire_section_mask(*e, e->section_mask, count));
			rec.has_section_mask = count != 0;
		} else if ((e->item_attrib2 & 0x4000u) != 0) {
			// Any other def carries entity+308 only as a Landmine def (its
			// triggered mines), and only a nonzero word.
			// [orig: @0x5044A6..0x5044B1 — `test [def+58h], 4000h`, `test ebx, ebx`]
			rec.section_mask = static_cast<int32_t>(e->section_mask);
		}
		// The D-NET-147 building/armory fields: the composed entity Flags dword (entity+36,
		// gates 0x0020), the BMS ammo byte (entity+290, always present), refNum (entity+533,
		// gates 0x0040) and subType (entity+532, gates 0x0080 — 0xFF on indestructible defs).
		// Golden ASH_I5A buildings: flags 0x0A1, eflags 0x04020400, subType 0xFF, ammo 0xFF.
		// The Flags field is the live dword, raw, exactly as the 0x0D record's.
		// [orig: NetPacket_SerializePool2StaticToBuffer @0x5042F0 field sources @0x5044e6/@0x504502/
		// @0x504519/@0x504535]
		rec.entity_flags = load_stream_flags_dword(*e);
		rec.ammo_count = e->ammo_count;
		rec.bone_a = e->ref_num;
		rec.bone_b = e->sub_type;
		// The same entity+0x270 byte as the 0x0D record's, behind the same
		// callback test. [orig: NetPacket_SerializePool2StaticToBuffer @0x504554..0x504588]
		rec.has_score_flag = e->palm_state_streamed;
		rec.score_flag = static_cast<uint8_t>(e->palm_state);
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
	// Pool-3 flag 0x02 is the raw entity+0 dword. For marker types whose
	// BMS waypoint distance overrides that field (notably KOTH's 6006), this
	// is the authored Q16 radius rather than an Euler component.
	// [orig: NetPacket_SerializeEntityPoolToPacket @0x503593/@0x5035A9]
	rec.orientation_val = static_cast<uint32_t>(
			world::to_fixed(e.bound_radius));
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

bool apply_player_intent(world::World &world, const PlayerIntent &intent) {
	// 1. Resolve the joiner's owned entity by its wire handle (pool<<12 | slot).
	//    [orig: NetPacket_DispatchEntityPacketCallback @0x4D6A80 resolves g_PoolList[h>>12] and
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
	//    globals the original also gates on (g_SpawnSuccessGate==0, playerSlot+0x20==6
	//    in-game, dword_C8D824==0 @0x4c200a-0x4c2028) hold for an active in-game peer and
	//    are modeled implicitly here (the SP listen-server only drains C2S for joined peers).
	if ((ent->flags & 0x2u) != 0)
		return false;

	// Engine-frame conversions. Heading/pitch on the extended wire are an i16 sign-extended
	// and << 16 = a full 32-bit BAM — a PURE widen, NOT the (90 - yaw) mission framing the
	// FORWARD snapshot_of applies (the joiner serialized its live entity+0x10, already
	// engine-framed). [orig: case 4 @0x4c1da6 `movsx eax, ax; shl eax, 10h` / @0x4c1dca.]
	int32_t heading_bam = static_cast<int32_t>(
			static_cast<uint32_t>(static_cast<uint16_t>(intent.heading)) << 16);
	const int32_t pitch_bam = static_cast<int32_t>(
			static_cast<uint32_t>(static_cast<uint16_t>(intent.pitch)) << 16);

	// Grounded branch (D-NET-151): carrier_handle != 0xFFFF means the sender stands ON
	// another entity (building floor / vehicle deck — any pool) and pos/heading are
	// CARRIER-LOCAL. Resolve the carrier and lift local -> world with the carrier's pose;
	// the heading composes by plain BAM addition (the original transform's out[3] =
	// local[3] + carrier[3], pitch passes through) [orig: case 4 resolve @0x4c1d07-0x4c1d26,
	// Entity_TransformLocalToWorld call @0x4c1de1, heading add @0x43be7e]. An unresolvable
	// carrier applies the local values RAW — exactly the original's null-carrier leg (no
	// transform, no rejection). The resolved path uses the carrier's complete modeled Euler.
	int32_t wire_x = intent.pos_x, wire_y = intent.pos_y, wire_z = intent.pos_z;
	const bool grounded = intent.carrier_handle != wire_handle::kInvalid;
	if (grounded) {
		if (const world::Entity *carrier = world.registry.get(
		            world::EntityHandle{static_cast<uint16_t>(intent.carrier_handle)})) {
			const int32_t carrier_yaw_bam = carrier_heading_bam(*carrier);
			const WorldPose w = network_transform_local_to_world(
					intent.pos_x, intent.pos_y, intent.pos_z,
					world::to_fixed(carrier->position.x),
					world::to_fixed(carrier->position.y),
					world::to_fixed(carrier->position.z),
					static_cast<uint32_t>(carrier_yaw_bam),
					static_cast<uint32_t>(carrier_pitch_bam(*carrier)),
					static_cast<uint32_t>(carrier_roll_bam(*carrier)));
			wire_x = w.x;
			wire_y = w.y;
			wire_z = w.z;
			heading_bam = static_cast<int32_t>(static_cast<uint32_t>(heading_bam) +
			                                      static_cast<uint32_t>(carrier_yaw_bam));
			// [orig: out[3] = ref[3] + local[3] @0x43be7e]
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
	// BAM32 -> mission yaw degrees: yaw = 90 - bam / kBamPerDegree, rounded (the inverse of
	// snapshot_of's spawn-angle heading), normalized into [0, 360).
	constexpr double kBamPerDegree = 11930464.0; // 2^32 / 360
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
	// ingest by g_AdmDefs[idx].category < 11 [orig: case-4 store @0x4C20A3]; a table-less world
	// (unit paths — a live host always feeds weapon.def) accepts the byte verbatim, and an
	// index with no table entry (including the 0xFF none sentinel) is NOT stored, mirroring
	// the failed AdmDef_GetEntryByIndex leg. (D-NET-143)
	if (world.tables.weapons.empty()) {
		ent->equipped_adm_index = intent.equipped_adm_index;
	} else if (const world::WeaponTableEntry *we =
	                   world.tables.weapons.by_index(intent.equipped_adm_index)) {
		if (we->category < 11) ent->equipped_adm_index = intent.equipped_adm_index;
	}

	// 5. Mirror the engine-frame store (AiEntity) and stage the smooth-target the CLIENT
	//    interpolation consumes; mark the entity net-snapped so the infantry motor SKIPS it
	//    (the host does not re-simulate a read-applied peer). [orig: case 4 staging +0x234/
	//    240/244 @0x4c2042-0x4c205e, live +4/+0x10/+0x14 mirror, progress +0x27C=0 @0x4c20a9;
	//    motor skip @0x4b9a03.] No AiEntity (peer not AI-attached) -> registry snap stands alone.
	if (world::AiEntity *ae = world.ai.for_handle(ent->handle)) {
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

		// The 0x0C state byte contains only the entity Flags LOW byte; the
		// in-air bit (0x2000) is not a hidden high-bit wire field. Reconstruct
		// that authority state from the pose every uplink, using the same
		// capsule-bottom clearance and hysteresis as the local player motor.
		// This makes a held jump from a falling peer fail the retail gate and
		// makes the next grounded sample produce the real landing edge. Water
		// transitions remain authority-world state (D-INF-3), not C2S flags.
		// The ground is the org2 resolver's ground-settle tail over the terrain
		// alone: Z raised to the 6144 grid, a 2.0 u column clipped by the
		// heightfield (the vertical column writes the terrain height whether or
		// not it reaches). [orig: Entity_UpdateInfantryPlayerBody resolver call
		// @0x4B7CE0..0x4B7CF4 -> Entity_MovementCollisionResolver tail
		// @0x4B3D6E..0x4B3DA9]
		bool clear_airborne = grounded || ent->mounted;
		bool set_airborne = false;
		if (!clear_airborne && world.tables.terrain != nullptr && world.tables.terrain->valid()) {
			const int32_t start[3] = {ae->pos[0], ae->pos[1], (ae->pos[2] + 6143) & ~0x17FF};
			int32_t end[3] = {start[0], start[1], start[2] - 0x20000};
			(void)world::terrain_clip_segment(*world.tables.terrain, start, end, end);
			const int32_t ground = end[2];
			const int64_t foot_clearance = static_cast<int64_t>(wire_z) -
					static_cast<int64_t>(ae->inf.prev_capsule_bottom) - ground;
			set_airborne = foot_clearance > world::kInfantryAirborneGap;
			clear_airborne = foot_clearance <= 0;
		}
		if (set_airborne) {
			ae->inf.airborne = true;
			ent->flags |= world::kEntityFlagInAir;
			ent->engine_flags |= world::kEntityFlagInAir;
		} else if (clear_airborne) {
			ae->inf.airborne = false;
			ent->flags &= ~world::kEntityFlagInAir;
			ent->engine_flags &= ~world::kEntityFlagInAir;
		}
	}
	return true;
}

namespace {

// _ftol2_sse: the x87 top is stored as a double and converted with cvttsd2si,
// a truncation whose out-of-range result is the integer indefinite 0x80000000.
int32_t retail_ftol(double v) {
	if (!(v > -2147483649.0 && v < 2147483648.0)) return INT32_MIN;
	return static_cast<int32_t>(v);
}

// The `cdq; xor; sub` absolute value: abs(INT32_MIN) stays INT32_MIN.
int32_t retail_abs32(int32_t v) {
	const uint32_t sign = static_cast<uint32_t>(v >> 31);
	return static_cast<int32_t>((static_cast<uint32_t>(v) ^ sign) - sign);
}

struct InterestPair {
	int32_t key = 0;
	uint16_t handle = 0xFFFF;
};

// [orig: CPairList_ShellSortByValue @0x526CF0 -- the Knuth gap sequence and the
//  signed `cmp; jge` that stops the insertion at an equal key, so the sort is
//  descending and NOT stable]
void retail_shell_sort_descending(std::vector<InterestPair> &rows) {
	size_t gap = 1;
	while (gap <= rows.size() / 9)
		gap = 3 * gap + 1;
	do {
		for (size_t i = gap; i < rows.size(); ++i) {
			const InterestPair insert = rows[i];
			size_t j = i;
			while (j >= gap && rows[j - gap].key < insert.key) {
				rows[j] = rows[j - gap];
				j -= gap;
			}
			rows[j] = insert;
		}
		gap /= 3;
	} while (gap != 0);
}

// The client's own top-N interest list, scored against the uplinking player.
// Pool 0 then pool 1, each in slot order, admitting a live entity with an
// items.def network callback, Flags bit 0 clear, that is not the player.
// [orig: Server_BuildEntityPriorityListForPlayer @0x50DF20]
void build_uplink_interest_pairs(world::World &world, const world::Entity &e,
		const world::AiEntity &ae, const UplinkClientInputs &interest,
		uint16_t out_handles[4], uint16_t out_scores[4]) {
	// The caller's pre-fill and the builder's tail: an unused pair is
	// (0xFFFF, 0). [orig: @0x4C1BC7..0x4C1BD8; @0x50E546..0x50E563]
	for (int i = 0; i < 4; ++i) {
		out_handles[i] = 0xFFFF;
		out_scores[i] = 0;
	}
	if (interest.replica == nullptr) return;
	// The player's carrier: groundEntity (+0x28), overridden by parentEntity
	// (+0x16C). [orig: @0x50DF88..0x50DF99]
	const world::EntityHandle player_target =
			e.mounted && e.mount_target.valid() ? e.mount_target : e.ground_target;
	const int32_t player_yaw = ae.heading;   // +0x10 [orig: @0x50DFA6]
	const int32_t player_pitch = ae.pitch;   // +0x14 [orig: @0x50DF9D]
	const int view_distance = static_cast<int16_t>(view_distance_units()); // movsx word_26C681E
	constexpr double kMaxDistance = 2147418112.0;        // flt_7C19E0
	constexpr double kRadiansToBam = -683565275.5764316; // dbl_7C57B8 (-2^31 / pi)

	// The walk order: pool 0 then pool 1, each by slot. [orig: @0x50DFAF / @0x50E231]
	std::vector<const ClientEntityState *> rows;
	rows.reserve(interest.replica->entities.size());
	for (const ClientEntityState &row : interest.replica->entities) {
		const int pool = row.handle >> 12;
		if (pool == 0 || pool == 1) rows.push_back(&row);
	}
	std::sort(rows.begin(), rows.end(),
			[](const ClientEntityState *a, const ClientEntityState *b) {
				return a->handle < b->handle;
			});

	std::vector<InterestPair> list;
	list.reserve(rows.size());
	for (const ClientEntityState *row : rows) {
		const int pool = row->handle >> 12;
		// The admission: a callback-bearing items.def class, Flags bit 0
		// clear (a carried/attached body), not the player itself.
		// [orig: @0x50DFED ItemTypeIndex, @0x50DFF7 Flags & 1, @0x50DFFD
		//  itemDef, @0x50E004 entity != player, @0x50E008 itemDef+0x164]
		if (row->cls != EntityClass::Player && row->cls != EntityClass::Infantry &&
				row->cls != EntityClass::Vehicle && row->cls != EntityClass::Guided)
			continue;
		const uint32_t flags = row->state_flags_known ? row->state_flags : row->spawn_entity_flags;
		if ((flags & 1u) != 0) continue;
		if (row->handle == interest.self_wire_handle) continue;
		// A pool-1 row is materialized at its own wire handle; a pool-0
		// person lives only in the replica.
		const world::Entity *native =
				pool == 1 ? world.registry.get(world::EntityHandle{row->handle}) : nullptr;

		// Distance in tiles: |d| with z halved, less the entity's boundRadius.
		// [orig: @0x50E033..0x50E0B3]
		const int32_t dx = static_cast<int32_t>(static_cast<uint32_t>(row->x) -
				static_cast<uint32_t>(ae.pos[0]));
		const int32_t dy = static_cast<int32_t>(static_cast<uint32_t>(row->y) -
				static_cast<uint32_t>(ae.pos[1]));
		const int32_t dz = static_cast<int32_t>(static_cast<uint32_t>(row->z) -
				static_cast<uint32_t>(ae.pos[2]));
		const int32_t dz_half = dz >> 1;
		const double dxd = dx, dyd = dy, dzhd = dz_half;
		const double dist3 = std::min(std::sqrt(dzhd * dzhd + dxd * dxd + dyd * dyd), kMaxDistance);
		// entity+0 boundRadius: a pool-1 row's materialized twin carries it; a
		// pool-0 person is its decoded collision proxy's (the same model bound
		// the retail client's own pool-0 entity holds).
		const int32_t bound_q16 = native != nullptr ? world::to_fixed(native->bound_radius)
				: (pool == 0 && world.collision != nullptr
						? world.collision->wire_person_bound_radius_q16(row->handle) : 0);
		int32_t distance = static_cast<int32_t>(static_cast<uint32_t>(retail_ftol(dist3)) -
				static_cast<uint32_t>(bound_q16)) >> 16;
		if (distance > 2048) continue;
		if (distance < 0) distance = 0;
		int32_t distance_score = 1124 - distance;
		if (distance_score < 0) distance_score = 0;

		// The view angle: bearing and elevation against Yaw/Pitch.
		// [orig: @0x50E0BB..0x50E13A]
		const double planar = std::min(std::sqrt(dyd * dyd + dxd * dxd), kMaxDistance);
		const int32_t planar_int = retail_ftol(planar);
		const int32_t bearing = retail_ftol(std::atan2(dyd, dxd) * kRadiansToBam);
		int32_t yaw_term = retail_abs32(static_cast<int32_t>(
				0u - static_cast<uint32_t>(player_yaw) - static_cast<uint32_t>(bearing))) >> 24;
		if (yaw_term > 64) yaw_term += 64;
		const int32_t elevation = retail_ftol(
				std::atan2(static_cast<double>(dz), static_cast<double>(planar_int)) * kRadiansToBam);
		const int32_t pitch_term = retail_abs32(static_cast<int32_t>(
				0u - static_cast<uint32_t>(player_pitch) - static_cast<uint32_t>(elevation))) >> 25;
		const int32_t angle = 256 - pitch_term - yaw_term;

		const uint8_t team = row->team_known ? row->team : 0;
		const int32_t enemy = team != 0 && team != e.team ? 1 : 0;
		int32_t los = 0;
		if (angle > 128 && distance < view_distance) {
			// [orig: Entity_CheckLineOfSightTerrainAndEntities(player, entity,
			//  player+4, entity+4, 0, 0) @0x50E179]
			const int32_t end[3] = {row->x, row->y, row->z};
			if (world.collision == nullptr) {
				los = 1;
			} else if (pool == 0) {
				// A decoded person is the endpoint entity through its row: the
				// Flags indoors bit and the parent slot (its seat mount).
				world::CollisionWorld::LosWireEndpoint person;
				person.indoors = (row->rm_entity_flags & world::kEntityFlagIndoors) != 0;
				if (row->carrier_handle != world::EntityHandle::kInvalid && row->mount_bone != 0)
					person.parent = world::EntityHandle{row->carrier_handle};
				los = world.collision->wire_person_los_clear(world, e.handle, person, ae.pos, end, 0,
						false) ? 1 : 0;
			} else {
				los = world.collision->entity_los_clear(world, e.handle,
						native != nullptr ? native->handle : world::EntityHandle{},
						ae.pos, end, 0, false) ? 1 : 0;
			}
		}
		const int32_t target = row->handle == player_target.packed ? 1 : 0;
		const int32_t cursor = row->handle == interest.hud_target_wire_handle ? 1 : 0;
		int32_t key = 0;
		if (pool == 0) {
			// Standing or riding an EWeap: parentEntity null, else the parent
			// def's attrib bit 0x20 (no def: 0). [orig: @0x50E187..0x50E1B0]
			int32_t visible = 1;
			if (row->carrier_handle != world::EntityHandle::kInvalid && row->mount_bone != 0) {
				const world::Entity *parent =
						world.registry.get(world::EntityHandle{row->carrier_handle});
				visible = parent != nullptr && parent->has_item_def &&
						(parent->item_attrib & world::kItemAttribEweap) != 0 ? 1 : 0;
			}
			// The player-class bit, Flags 0x100 -- the Player class row
			// (present_rows.cpp's same mapping). [orig: @0x50E1CF]
			const int32_t is_player = row->cls == EntityClass::Player ? 1 : 0;
			key = distance_score + 2 * (angle + 25 * (enemy + 15 * cursor +
					2 * (los + visible + 10 * target) + is_player));
		} else {
			// occupantEntity (+0x170). [orig: @0x50E40A]
			const int32_t occupied =
					native != nullptr && native->primary_occupant.valid() ? 1 : 0;
			key = distance_score + 2 * (angle + 25 * (enemy + 15 * cursor +
					2 * (los + 2 * (target + occupied + 4 * target))));
		}
		if (distance < view_distance) key += 200; // [orig: @0x50E1F9 / @0x50E457]
		if ((flags & 1u) != 0) key >>= 4;         // [orig: @0x50E208] (unreachable past the filter)
		list.push_back({key, row->handle});
	}
	retail_shell_sort_descending(list);
	// The top entries: a key above 0xFFFF saturates. [orig: @0x50E4C0..0x50E53C]
	const size_t count = std::min<size_t>(list.size(), 4);
	for (size_t i = 0; i < count; ++i) {
		out_handles[i] = list[i].handle;
		out_scores[i] = list[i].key > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(list[i].key);
	}
}

} // namespace

uint16_t uplink_hud_target_wire_handle(const world::World &world, const world::AiEntity &ae,
		uint16_t self_wire_handle, uint16_t aim_wire_person) {
	const world::EntityHandle cursor =
			ae.inf.combat_target.valid() ? ae.inf.combat_target : ae.inf.head_look_target;
	if (cursor == world.cached.local_player) return self_wire_handle;
	if (cursor.valid()) return cursor.pool() >= 1 && cursor.pool() <= 3 ? cursor.packed : 0xFFFF;
	// The head-look store named a decoded remote person: its wire row.
	return aim_wire_person;
}

PlayerExtendedUplink build_player_uplink(world::World &world,
                                         const world::Entity &e,
                                         const world::AiEntity &ae,
                                         const UplinkClientInputs &interest) {
	PlayerExtendedUplink up; // wire defaults include the no-carrier handle 0xFFFF
	// Live engine-frame pose (the AiEntity store apply_player_intent SNAPs back on receive):
	// pos[] is already i32 16.16; heading/pitch are BAM32 whose HIGH half is the i16 wire field
	// (the exact inverse of apply_player_intent's `intent.heading << 16`). [orig: case 4
	// @0x4c1da6/@0x4c1dca + the live +4/+8/+0xC pos store.]
	up.pos_x = ae.pos[0];
	up.pos_y = ae.pos[1];
	up.pos_z = ae.pos[2];
	up.heading = static_cast<int16_t>(ae.heading >> 16);
	up.pitch = static_cast<int16_t>(ae.pitch >> 16);
	// Retail's op-3 builder reads groundEntity (+0x28) alone, which the player
	// body points at parentEntity on every update while mounted; the native
	// player body keeps no such store, so the mount is taken first here -- the
	// same entity in steady state. A resolved carrier changes both position and
	// heading into its local frame; pitch passes through unchanged. A stale
	// relationship cannot exist as a raw pointer in retail, so the handle port
	// safely falls back to FFFF/world pose.
	// [orig: NetPacket_SerializePlayerState case 3 `mov ecx, [edi+28h]` @0x4C141D;
	//  Entity_UpdateInfantryPlayerBody @0x4B41A2..0x4B41B4 `groundEntity =
	//  parentEntity`; Entity_TransformWorldToLocal @0x43BB50; heading
	//  subtraction @0x43bb7b]
	world::EntityHandle carrier_handle;
	// A receive that folded the player's own record ahead of this send block
	// re-pointed the link first: the record's seat leaves the seat's own
	// ground link (a vehicle on open ground has none). The seat resolves to
	// its materialized row; a record whose seat has no row was dropped before
	// that store, so the link stands. The standing record's carrier store
	// (@0x4C1358) is not modeled: an unmounted player keeps its own link.
	// [orig: NetPacket_SerializePlayerState case 2 -- the carrier resolve and
	//  its no-itemDef drop @0x4C105E..0x4C10C7, Entity_TryAttachOrDetach
	//  @0x4C1329, `parentEntity->groundEntity` @0x4C1346..0x4C1353]
	const world::Entity *echo_seat = nullptr;
	if (interest.self_echo != nullptr && interest.self_echo->mount_bone != 0 &&
			interest.self_echo->carrier_handle != wire_handle::kInvalid)
		echo_seat = world.registry.get(world::EntityHandle{interest.self_echo->carrier_handle});
	if (echo_seat != nullptr)
		carrier_handle = echo_seat->ground_target;
	else if (e.mounted && e.mount_target.valid())
		carrier_handle = e.mount_target;
	else if (e.ground_target.valid())
		carrier_handle = e.ground_target;
	if (const world::Entity *carrier = world.registry.get(carrier_handle)) {
		const int32_t carrier_yaw_bam = carrier_heading_bam(*carrier);
		const WorldPose local = network_transform_world_to_local(
				ae.pos[0], ae.pos[1], ae.pos[2],
				world::to_fixed(carrier->position.x),
				world::to_fixed(carrier->position.y),
				world::to_fixed(carrier->position.z),
				static_cast<uint32_t>(carrier_yaw_bam),
				static_cast<uint32_t>(carrier_pitch_bam(*carrier)),
				static_cast<uint32_t>(carrier_roll_bam(*carrier)));
		up.carrier_handle = carrier_handle.packed;
		up.pos_x = local.x;
		up.pos_y = local.y;
		up.pos_z = local.z;
		const int32_t local_heading = static_cast<int32_t>(
				static_cast<uint32_t>(ae.heading) -
				static_cast<uint32_t>(carrier_yaw_bam));
		up.heading = static_cast<int16_t>(local_heading >> 16);
	}
	// The +0x12C movement-INPUT byte for our own player (the host ingests + echoes it in our
	// 0x0A record so OTHER clients motor-drive our avatar). The local player mirrors
	// this byte from its last pack's MoveOrder, which under a send holdoff packs at
	// the uplink boundary from every key held during the window [orig:
	// Client_ProcessNetworkFrame @0x42C3E9 precedes @0x42C482]. [witness 2026-07-02:
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
	// The self-check byte ahead of the movement-input byte starts from the
	// scope bit (Flags 0x10) moved to 0x80; the rotating debugger / hook /
	// movement probes the same byte ORs in stay clear here. A retail host
	// skips the byte on read. [orig: NetPacket_SerializePlayerState case 3
	//  `movsx ebx, cl; and ebx, 10h` + three `add ebx, ebx` @0x4C1432..0x4C1447,
	//  the probe switch @0x4C145E.., stored @0x4C1AF0; case 4 advances past it]
	up.anticheat_flags = static_cast<uint8_t>((e.flags & 0x10u) << 3);
	// Preserve the signed control bytes as wire bit patterns. The authority
	// consumes these for analog throttle and steering. Dropping them loses
	// controls that may already have affected local vehicle prediction.
	// [orig: NetPacket_SerializePlayerState @0x4C09C0, case 3 @0x4C1B2E..0x4C1B74;
	// entity+0x130/131/132; case 4 @0x4C1E6A..0x4C1EA4]
	up.analog_x = static_cast<uint8_t>(e.net_analog_x);
	up.analog_y = static_cast<uint8_t>(e.net_analog_y);
	up.analog_z = static_cast<uint8_t>(e.net_analog_z);
	// The equipped-weapon adm index for our own player — the host ingests it (category-gated)
	// and echoes it at our 0x0A off-16 so other clients resolve our weapon-anim def.
	// [orig: the client fills byte 24 from entity+0x2B0; case-4 store @0x4C20A3] (D-NET-143)
	up.equipped_adm_index = e.equipped_adm_index;
	// The low bytes of the main loop's FR-counter frame rate and its window's
	// CPU share [orig: case 3 `mov dl, byte ptr g_StatsAvgFps` @0x4C1BA2,
	// `mov dl, byte ptr g_StatsCpuPercent` @0x4C1BBC].
	up.stat_byte_0 = static_cast<uint8_t>(interest.avg_fps);
	up.stat_byte_1 = static_cast<uint8_t>(interest.cpu_percent);
	// The four interest pairs: the client's own top-4 list, handle then score
	// per pair [orig: case 3 @0x4C1BC7..0x4C1C9B -- Server_BuildEntityPriorityListForPlayer
	// (entity, handles, scores, 4) @0x4C1BE9].
	uint16_t handles[4], scores[4];
	build_uplink_interest_pairs(world, e, ae, interest, handles, scores);
	up.priority_handle_0 = handles[0];
	up.priority_score_0 = scores[0];
	up.priority_handle_1 = handles[1];
	up.priority_score_1 = scores[1];
	up.priority_handle_2 = handles[2];
	up.priority_score_2 = scores[2];
	up.priority_handle_3 = handles[3];
	up.priority_score_3 = scores[3];
	return up;
}

} // namespace opennova::replication
