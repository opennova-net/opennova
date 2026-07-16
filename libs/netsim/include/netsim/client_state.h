#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <npwire/ingame_decode.h> // EntityClass

namespace opennova::netsim {

// One entity as the local client has DECODED it off the wire. Per ADR 0011 the
// present pass reads THIS, not the authoritative sim directly — so single-player
// renders exactly the state a networked peer would see.
struct ClientEntityState {
	uint16_t handle = 0;                          // (pool<<12)|slot
	uint16_t type_id = 0;
	EntityClass cls = EntityClass::Unknown;
	int32_t x = 0;                                // world i32 16.16 (decompressed
	int32_t y = 0;                                // compact position + the frame anchor)
	int32_t z = 0;
	uint8_t yaw_byte = 0;                         // coarse heading (compact high byte)
	bool seen_this_frame = false;
};

// The decoded world the client holds after pumping the loopback. Positions are
// post-compression (lossy, ~|v|>>11 quantization) — exactly what the original
// client renders for its decoded peers. Callers must NOT "correct" them toward the
// authoritative value.
struct ClientState {
	int32_t anchor_x = 0;                         // latest 0x0A frame anchor
	int32_t anchor_y = 0;
	int32_t anchor_z = 0;
	int16_t local_health = 0;
	std::vector<ClientEntityState> entities;
	std::uint32_t frames_applied = 0;

	ClientEntityState *find(uint16_t handle);
	ClientEntityState &upsert(uint16_t handle);

private:
	// The wire fold addresses entities by handle for every record in every frame.
	// Retaining vector indexes keeps that lookup O(1) without changing `entities`
	// insertion order or upsert's existing vector-reference semantics.
	std::unordered_map<uint16_t, std::size_t> entity_index_;
};

} // namespace opennova::netsim
