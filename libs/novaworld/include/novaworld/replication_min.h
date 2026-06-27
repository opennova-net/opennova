#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <novaworld/ingame_decode.h> // EntityClass (the §5.10b replication class)

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
};

// Mission entity data shared by all game-server frontends. The CLI can fill this from parsed .bms
// pool-2 records; Godot scenes can author the same records directly. Runtime replication code treats
// this as the source of server-owned world entities and never reaches back into a transport layer.
struct GameEntitySnapshot {
	uint8_t pool = 2;
	uint16_t slot = 0;
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
};

} // namespace opennova
