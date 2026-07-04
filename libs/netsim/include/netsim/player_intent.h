#pragma once

#include <cstdint>

namespace opennova::netsim {

// Wire-shaped player input — the field map of the C2S 0x0C extended uplink (§5.10,
// [orig: Player_BuildTag0CInputBody @ 0x42A550]). ONE struct serves both transports
// (ADR 0012 Decision 2): under SP it feeds the owned pool-0 entity through the
// in-process loopback; under MP the identical fields ARE the C2S uplink a remote
// client sends. Phase 1 only defines the seam; EntityWireBridge::apply_player_intent
// (Phase 2) applies it, gated by the witnessed entity+286 (healthMax) / entity+36
// bit-1 checks.
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
	                                    // from the extended uplink gated AdmDefs[idx].category
	                                    // < 11, echoed at 0x0A off-16 [orig: @0x4C20A3]
	                                    // (D-NET-143). 0xFF = none.
	int8_t   analog_x = 0;            // entity+0x130..+0x132 analog control axes (uplink
	int8_t   analog_y = 0;            // off-21..23) — the vehicle motor consumes the
	int8_t   analog_z = 0;            // controlling occupant's axes [orig: @0x48b783]
	uint32_t buttons = 0;             // fire / action bitmask
};

} // namespace opennova::netsim
