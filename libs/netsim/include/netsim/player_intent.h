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
	uint16_t vehicle_handle = 0xFFFF;  // mounted vehicle, 0xFFFF = on foot
	int32_t  pos_x = 0;                // i32 16.16 world (authoritative, uncompressed)
	int32_t  pos_y = 0;
	int32_t  pos_z = 0;
	int16_t  heading = 0;              // entity+16 yaw intent
	int16_t  pitch = 0;               // entity+0x14 pitch intent
	uint8_t  move_input = 0;          // entity+0x12C movement-input byte (the wire off-12 echo
	                                  // source; renamed from the `anim` misnomer, witness 2026-07-02)
	uint8_t  flags_xor = 0;           // uplink flags-xor byte — bits 2-4 XOR into entity+0x24
	                                  // [orig: case-4 apply; §5.10 extended uplink field map]
	uint32_t buttons = 0;             // fire / action bitmask
};

} // namespace opennova::netsim
