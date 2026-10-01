// The host side of the visible-players table -- see server_visible_players.h.

#include <runtime/inmatch/server_visible_players.h>

#include <algorithm>

#include <net/npwire/ingame_message_id.h>
#include <net/npwire/visible_players.h>
#include <runtime/inmatch/session_transport.h>
#include <runtime/world/world.h>

namespace opennova::inmatch {

namespace {

// A roster slot that holds a player: added, not leaving, bound to an entity
// [orig: the slot +4 active byte and the +0 entity pointer @0x5063e0..0x5063ea].
bool slot_live(const NapiNPConnection &c) {
	return c.phase >= ConnectionPhase::PlayerAdded && c.phase < ConnectionPhase::Goodbye &&
			c.link.owned_entity.valid();
}

// The slot's team byte (slot+0x1A0), read through the bound entity like every
// roster reply (D-NET-132); a World-less path has no team.
uint8_t slot_team(const NapiNPConnection &c, const world::World *world) {
	if (world == nullptr) return 0;
	const world::Entity *e = world->registry.get(c.link.owned_entity);
	return e != nullptr ? e->team : uint8_t{0};
}

} // namespace

std::vector<uint8_t> build_visible_players_snapshot(const NapiNPConnection &requester,
		const std::vector<NapiNPConnection> &roster, const world::World *world,
		uint32_t game_type, bool mp_session_peer) {
	VisiblePlayers out;
	// An inactive requester writes count 0 [orig: @0x50635a..0x5063ac].
	if (!slot_live(requester)) return encode_visible_players(out);
	// Outside the team types the requester's player carries the AI-record
	// 0x200 bit from its spawn, which reads as team 255: no other slot matches.
	// [orig: @0x506372..0x50638c; the set @0x43c546..0x43c54f]
	const uint8_t requester_team =
			(game_type & 0x10000u) == 0 ? uint8_t{0xFF} : slot_team(requester, world);
	// The slot table walks in slot order [orig: g_PlayerSlots stride 100584
	// @0x5063c0..0x50649b].
	std::vector<const NapiNPConnection *> slots;
	for (const NapiNPConnection &c : roster) slots.push_back(&c);
	std::stable_sort(slots.begin(), slots.end(),
			[](const NapiNPConnection *a, const NapiNPConnection *b) {
				return a->reply.player_slot < b->reply.player_slot;
			});
	for (const NapiNPConnection *c : slots) {
		if (!slot_live(*c)) continue; // +4 active, +0 entity
		// The host's own slot (+5) only when the host plays [orig: @0x5063f4].
		const bool host_slot = c->link.mode == replication::TransportMode::Loopback;
		if (host_slot && !mp_session_peer) continue;
		// The spectator latch +100567 [orig: @0x506410]; the +96481/+97536
		// pair is always admitted (+96481 has no writer).
		if (c->link.spectator) continue;
		const bool self = c == &requester;
		if (!self && slot_team(*c, world) != requester_team) continue; // @0x506440
		VisiblePlayers::Entry e;
		e.slot = c->reply.player_slot;                  // slot+0x14 @0x506445
		e.entity_handle = c->link.owned_entity.packed; // the packed handle @0x506485
		out.entries.push_back(e);
	}
	return encode_visible_players(out);
}

void fan_spawn_slot_notice(std::vector<NapiNPConnection> &roster, const NapiNPConnection &joined) {
	SpawnSlotNotice notice;
	notice.slot = joined.reply.player_slot;
	const std::vector<uint8_t> body = encode_spawn_slot_notice(notice);
	for (NapiNPConnection &c : roster) {
		if (&c == &joined || !active_player_recipient(c)) continue;
		c.link.transport->host_send(s2c::SPAWN_SLOT_NOTICE, body);
	}
}

} // namespace opennova::inmatch
