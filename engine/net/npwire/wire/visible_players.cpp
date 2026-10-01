#include <net/npwire/visible_players.h>

#include <base/io/byte_reader.h>
#include <base/io/le.h>

namespace opennova {

void decode_visible_players(const uint8_t *body, size_t len, VisiblePlayers &out,
		bool *clean) {
	out = VisiblePlayers{};
	io::ByteReader r(body, len);
	// [orig: NapiNPClientMsg_0x04C @0x4285ad..0x4285b7]
	const uint8_t count = r.read_u8();
	out.entries.reserve(count);
	for (uint8_t i = 0; i < count; ++i) {
		VisiblePlayers::Entry e;
		e.slot = r.read_u8();          // @0x428609..0x42860f
		e.entity_handle = r.read_u16(); // @0x42861d..0x428626
		out.entries.push_back(e);
	}
	if (clean != nullptr) *clean = r.ok() && r.remaining() == 0;
}

std::vector<uint8_t> encode_visible_players(const VisiblePlayers &players) {
	// The count byte, then {slot+0x14, packed entity handle} per staged entry
	// [orig: NetPacket_SerializeVisiblePlayersSnapshot @0x5064be..0x5064f4].
	std::vector<uint8_t> out;
	io::append_u8(out, static_cast<uint8_t>(players.entries.size()));
	for (const VisiblePlayers::Entry &e : players.entries) {
		io::append_u8(out, e.slot);
		io::append_u16_le(out, e.entity_handle);
	}
	return out;
}

void decode_spawn_slot_notice(const uint8_t *body, size_t len, SpawnSlotNotice &out) {
	out = SpawnSlotNotice{};
	io::ByteReader r(body, len);
	out.slot = r.read_u8(); // [orig: @0x4317c0..0x4317c6]
}

std::vector<uint8_t> encode_spawn_slot_notice(const SpawnSlotNotice &notice) {
	// [orig: Server_OnPlayerJoin — `mov dl, [esi+14h]` @0x51a946, the one
	//  payload byte @0x51a960, length 1 @0x51a966]
	return {notice.slot};
}

} // namespace opennova
