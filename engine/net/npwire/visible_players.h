#pragma once

// The visible-players wire: the S2C 0x4C snapshot a client folds into its
// player-slot pointer table (g_PlayerSlotPtrTable / g_PlayerSlotPtrCount), and
// the S2C 0x4D slot notice every in-game client receives when a player joins,
// which makes each OTHER client re-request the 0x46 row and the 0x4C snapshot.
// See docs/net/novaworld-net-re.md and docs/interface/hud-re.md "The MP legs".

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova {

// S2C 0x4C: [u8 count] then count x {u8 player slot, u16 entity handle}. Every
// field reads the retail way: a read past the end yields 0 and does not
// advance, and the entry loop still runs `count` times (the client's table
// gets zeroed entries for a short body).
// [orig: NapiNPClientMsg_0x04C @0x428570 — count @0x4285b3, slot @0x42860f,
//  handle @0x428623; the builder NetPacket_SerializeVisiblePlayersSnapshot
//  @0x506320 writes the count @0x5064be and each entry @0x5064d5/@0x5064f1]
struct VisiblePlayers {
	struct Entry {
		uint8_t slot = 0;
		uint16_t entity_handle = 0;
	};
	std::vector<Entry> entries;
};
// Never fails (retail has no reject path); `clean` reports a body that held
// every declared entry exactly.
void decode_visible_players(const uint8_t *body, size_t len, VisiblePlayers &out,
		bool *clean = nullptr);
std::vector<uint8_t> encode_visible_players(const VisiblePlayers &players);

// S2C 0x4D: [u8 player slot]. A short body reads slot 0.
// [orig: NapiNPClientMsg_HandleSpawnSlot @0x4317B0 — the read @0x4317c6;
//  the sender Server_OnPlayerJoin @0x51a946..0x51a97a writes slot+0x14]
struct SpawnSlotNotice {
	uint8_t slot = 0;
};
void decode_spawn_slot_notice(const uint8_t *body, size_t len, SpawnSlotNotice &out);
std::vector<uint8_t> encode_spawn_slot_notice(const SpawnSlotNotice &notice);

// The C2S 0x22 field mask the 0x4D re-request asks for (every roster field),
// and the team-only mask the 0x50 re-request asks for.
// [orig: `*(_WORD *)(&g_NetMsgPayload + 1) = 7415` @0x43180a;
//  `= 4` @0x431ad1]
inline constexpr uint16_t kSpawnSlotSyncFields = 0x1CF7;
inline constexpr uint16_t kTeamAssignSyncFields = 0x0004;

} // namespace opennova
