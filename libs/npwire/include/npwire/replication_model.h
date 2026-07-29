#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <npwire/entity_class.h> // EntityClass (the §5.10b replication class)

namespace opennova {

// Transport-neutral POD inputs shared by the in-match replication code: the netsim world<->wire bridge
// (entity_wire_bridge / connection_fan), the npruntime per-frame 0x0A fan + Server_TickUpdate, and the
// reactive §5.1 reply dispatcher (server_message_dispatch). The CLI server, Godot server scene, and
// tests share this one game-state model; the runtime never reaches back into a transport layer.
//
// The §5.1/§5.2a reply BUILDERS that used to live here were retired with game_session.cpp (P8, net-re
// §5.45 / D-NET-127): the reactive reply bodies moved to libs/npruntime/server_message_dispatch.cpp,
// and the per-frame S2C 0x0A frame builder (build_0a_frame) into libs/netsim/connection_fan.cpp. Only the
// shared POD structs remain here.

struct PlayerReplicationState {
	// Player identity for the joining client.
	std::string player_name = "DevUser"; // ≤32 chars; the roster (tag=0x16) / sync (tag=0x46) name.
	std::string clan_tag = "";           // ≤16 chars; mostly cosmetic.
	uint8_t player_slot = 0;             // Slot index in `dword_A87048` player table.
	uint16_t entity_handle = 0;          // Packed `(pool << 12) | slot` for the player's pool entity.
	uint8_t team = 1;                    // entity+354 / tag=0x46 team field. 0 = "no team / spectator"
	                                     // (player can shoot but not move). 1 = blue, 2 = red. Default
	                                     // 1 = playable.
	uint32_t mi = 0x3CDEu;               // Server-side MI from ServerAuth — `sub_4E0090@0x4E0090`
	                                     // matches it against entity[120].
	// Spawn coordinates for the player on the joining map (dvxi5 by default). Engine units are signed
	// 32-bit ints; the values below land near the dvxi5 map center (the retail capture's tag=0x0C spawn).
	uint32_t spawn_x = 0xfe56f854u;
	uint32_t spawn_y = 0x0049f5f0u;
	uint32_t spawn_z = 0x003a5e6au;
	// Yaw / pitch / roll as raw u16 (client shifts each by 16 to fixed-point).
	uint16_t yaw = 0;
	uint16_t pitch = 0;
	uint16_t roll = 0;
	// Player entity item-template id. `0x14B9` (5305) = retail's "default infantry player" template.
	uint16_t entity_type_id = 0x14B9u;
	// Spawn-point/menu labels for tag=0x0F. Empty preserves the retail ASH_I5A witness tail; configured
	// sessions set this from their selected mission so a non-ASH host does not advertise the ASH names.
	std::vector<std::string> spawn_names;
	// The 0x0F variant-0 / 0x0A phase-0 u32 owned-zone mask: bit (1 << zone_no) set iff every zone
	// entity of that number belongs to this player's team — the deploy map's spawnable-zone
	// advertising. Default 0x8 = the golden ASH_I5A steady value (zone 3 wholly owned), kept for
	// World-less hosts; a zone-chain host computes it per recipient. [orig:
	// ZoneSlotChain_GetOwnedZoneMask @0x4a2620 written @0x4ff9a3; net-re §5.61]
	uint32_t uniform_team_mask = 0x8;
};

// Mission entity data shared by all game-server frontends. The CLI can fill this from parsed .bms
// pool-2 records; Godot scenes can author the same records directly. Runtime replication code treats
// this as the source of server-owned world entities and never reaches back into a transport layer.
struct GameEntitySnapshot {
	uint8_t pool = 2;
	uint16_t slot = 0;
	// The packed wire handle (pool<<12 | slot) carried straight off the registry Entity
	// (world::Entity::handle.packed) so the per-frame S2C 0x0A builder emits the authoritative
	// handle rather than re-deriving the bit-packing at emit time. Filled by snapshot_of (the
	// sole producer feeding the 0x0A fan); pool/slot stay for consumers that key on the split
	// fields. Value equals (pool<<12 | slot) by construction, so the wire bytes are unchanged.
	uint16_t wire_handle = 0;
	uint16_t type_id = 0;
	uint16_t flags = 0;
	uint8_t team = 0;
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;
	// Engine heading (entity+16, 32-bit BAM) — the yaw the spawn pose is built from (D-NET-86: entity+16
	// is yaw, NOT velocity). The §5.9 0x0A compact records carry only its high byte (Player/Infantry
	// yaw_byte (v+0x800000)>>24; Vehicle euler_z (v+0x8000)>>16), so replicated orientation is coarse on
	// the wire. The full engine heading is (90 - bms_yaw) deg; the bridge from a World entity
	// (Entity::yaw) fills this. Default 0 keeps existing positional inits unchanged.
	int32_t euler_z = 0;
	// §5.10b replication class for the S2C 0x0A event loop (D-NET-50): selects the compact encoder.
	// Unknown / NoNetworkCallback / Guided are not emitted by the production 0x0A frame builder;
	// authoring/runtime code sets this from the item's *_function class tag.
	EntityClass entity_class = EntityClass::Unknown;
	// Engine pitch (entity+0x14, BAM32). The player compact record's pitch byte is its rounded
	// high byte `(v + 0x800000) >> 24` [orig: @0x4c0c77]. Pure degree->BAM widen (no (90-x)
	// frame inversion — that is yaw-only).
	int32_t pitch_bam = 0;
	// entity+0x12C low byte — the player compact record's movement-input byte [orig: @0x4c0c9c].
	uint8_t move_input_byte = 0;
	// entity+0x2B0 — the equipped-weapon AdmDef index the player record's off-16 anim_def_index
	// echoes (uplink ingest @0x4C20A3 / the WPN_M4AUTO spawn default @0x4B1116). 0xFF = none
	// (the client apply skips it; 0 is a valid index). (D-NET-143)
	uint8_t equipped_adm_index = 0xFF;
	// Body-anim wire state for the player record bytes 14/15: the anim-state id the emit
	// selects as pending ?: current [orig: @0x4c0cc7 reads +0x2B8 ?: +0x2BC] and the anim
	// channel's elapsed-ticks-in-loop clamped to 255 [orig: @0x4c0cf2]. Mirrored from the
	// infantry motor (AiSystem::mirror_wire_anim). Defaults keep snapshot-only tests on the
	// retail spawn/idle bytes (44/0). (D-NET-159)
	uint8_t anim_state_id = 44;
	uint8_t anim_pending_id = 0;
	uint8_t anim_channel_ratio = 0;
	// InfantryCompactRecord's two witnessed orientation sources. These stay full
	// BAM32 in the transport-neutral snapshot; connection_fan performs the retail
	// byte packing from entity+0x2EC/+0x2D0. They add no wire fields.
	int32_t infantry_target_heading_bam = 0; // entity+748: clamp around heading, rounded high byte
	int32_t infantry_aim_pitch_bam = 0;      // entity+720: rounded high byte (wire aim_yaw_byte)
	// entity+0x157 attachBoneId — the RAW wire seat bone this player mounted by; the mounted
	// player record's byte 0 [orig: @0x4c0a1a reads +0x157 when mounted]. 0 when unmounted.
	uint8_t veh_bone = 0;
	// entity+0x24 (Flags) low byte, written UNMASKED to the player compact record's state byte
	// [orig: @0x4c0c7d — masking is read-side only: local 0xE1 / remote 0xFD]. Also the vehicle
	// compact record's flags byte [orig: @0x460d22].
	uint8_t state_flags = 0;
	// The RIDDEN vehicle (entity+0x16C) when mounted, else 0xFFFF. The player record's carrier
	// select prefers this over ground_handle [orig: op1 @0x4c0a08 — mount wins].
	uint16_t mount_handle = 0xFFFF;
	// The standing-on carrier (entity+0x28 groundEntity — building floor / vehicle deck, any
	// pool), else 0xFFFF. When either handle is live the player record's position is
	// CARRIER-LOCAL (Entity_TransformWorldToLocal @0x43BB50) with a carrier-relative yaw byte,
	// and the client mirrors the echoed carrier back into its own groundEntity (@0x4c1353) —
	// echoing 0xFFFF at a grounded client detaches and hard-snaps it (D-NET-151). Filled for
	// read-applied peers from their §5.10 uplink. Retail derives it from the movement
	// resolver's unconditional CB/terrain ground probe @0x414370; our remote-peer
	// read-apply path does not re-simulate that probe.
	uint16_t ground_handle = 0xFFFF;
	// Resolved carrier POSE for the record builder (the carrier may be a pool-2 static, which
	// has no snapshot of its own in the 0x0A entity list — the World-aware snapshot pass
	// resolves the pose from the registry instead). Valid only when carrier_pose_valid; the
	// pose is the carrier's entity+4..+0x18 sextet our world models (yaw+pitch; roll
	// unmodeled = 0). Selection mirrors op1: mount_handle wins over ground_handle
	// [orig: @0x4c0a08].
	bool carrier_pose_valid = false;
	int32_t carrier_x = 0, carrier_y = 0, carrier_z = 0; // 16.16 world
	int32_t carrier_yaw_bam = 0;
	int32_t carrier_pitch_bam = 0;
	int32_t carrier_roll_bam = 0;
	// Entity Health (entity+286). The §5.10 player compact record's "health classification" byte
	// (field 17) is quantized from this against health_max — see health_classification_byte
	// [orig: Entity_GetHealthClassification @ 0x4AD4E0]. A living entity MUST replicate a non-zero
	// byte or the client marks its own player dead and the C2S 0x0C move uplink
	// (Player_BuildTag0CInputBody @0x42a59d, gated on entity->Health != 0) never fires. Default
	// 100 keeps positional-only inits alive.
	int32_t health = 100;
	// itemDef->healthMax (itemDef+0x17C), the field-17 tier denominator. world::Entity does not
	// carry the resolved item def yet, so the default is the class-8 player healthMax (150) — the
	// same stopgap the 0x0A tail health uses; resolving per-item healthMax from items.def is a
	// tracked follow-up.
	int32_t health_max = 150;
	// playerClass (entity+0x294) — the field-17 low nibble the client apply writes back to
	// entity->playerClass and re-resolves the soldier model/itemDef from (@0x4AD580 / @0x4c1248).
	// Filled by snapshot_of via player_class_for_wire (retail [5,9]-else-8 clamp for players).
	uint8_t player_class = 0;
};

} // namespace opennova
