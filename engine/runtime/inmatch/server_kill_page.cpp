// The host's join-window kill page -- see server_kill_page.h.

#include <runtime/inmatch/server_kill_page.h>

#include <net/npwire/ingame_message_id.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

namespace opennova::inmatch {

namespace {

// Pool_GetUsedCount: the pool's live high-water slot + 1.
uint32_t pool_used_count(const world::World &world, uint32_t pool) {
	uint32_t used = 0;
	world.registry.for_each([&](const world::Entity &e) {
		if (e.handle.pool() == pool && e.handle.slot() + 1u > used)
			used = e.handle.slot() + 1u;
	});
	return used;
}

// The 16-byte iterator the two walkers share: the current handle, its pool,
// that pool's used count and the handle it ends at.
// [orig: WeaponLoadout_IteratorBegin @0x501680 / ItemPoolIterator_Advance @0x501740]
struct PoolWalk {
	uint16_t current = 0xFFFF;
	uint32_t pool = 0;
	uint16_t end = 0;
};

void enter_pool(const world::World &world, PoolWalk &walk) {
	walk.pool = walk.current >> 12;
	const uint32_t used = pool_used_count(world, walk.pool);
	walk.end = static_cast<uint16_t>((walk.pool << 12) | (used & 0xFFFu));
}

// The next pool's first handle, or the end of the walk past pool 2.
bool hand_over(PoolWalk &walk) {
	if (walk.pool == 0)
		walk.current = 0x1000;
	else if (walk.pool == 1)
		walk.current = 0x2000;
	else {
		walk.current = 0xFFFF;
		return false;
	}
	return true;
}

// [orig: WeaponLoadout_IteratorBegin @0x501680 — pool > 2 @0x501694, the
//  bound @0x5016CD, the hand-over @0x5016D4..0x501704 (the next pool is
//  entered without testing it for rows)]
uint16_t walk_begin(const world::World &world, PoolWalk &walk, uint16_t start) {
	walk.current = start;
	walk.pool = start >> 12;
	if (walk.pool > 2) return walk.current = 0xFFFF;
	enter_pool(world, walk);
	if (!(walk.current < walk.end)) {
		if (!hand_over(walk)) return 0xFFFF;
		enter_pool(world, walk);
	}
	return walk.current;
}

// [orig: ItemPoolIterator_Advance @0x501740 — the end test @0x50174F, the
//  hand-over loop @0x501758..0x501792 over every empty pool]
uint16_t walk_advance(const world::World &world, PoolWalk &walk) {
	if (walk.current == 0xFFFF) return 0xFFFF;
	walk.current = static_cast<uint16_t>(walk.current + 1u);
	if (walk.current == walk.end) {
		do {
			if (!hand_over(walk)) return 0xFFFF;
			enter_pool(world, walk);
		} while (walk.current == walk.end);
	}
	return walk.current;
}

void put_u16(std::vector<uint8_t> &out, size_t at, uint16_t v) {
	out[at] = static_cast<uint8_t>(v & 0xFFu);
	out[at + 1] = static_cast<uint8_t>(v >> 8);
}

} // namespace

bool kill_page_entity_eligible(const world::World &world, uint32_t window_min,
		uint32_t window_max, uint16_t handle) {
	if (handle == 0xFFFF || (handle & 0xF000u) >= 0x5000u) return false;
	// A free or out-of-range slot reads as a zeroed row, which the def test
	// below refuses.
	const world::Entity *e = world.registry.get(world::EntityHandle{handle});
	if (e == nullptr) return false;
	const uint32_t stamp = e->last_state_sent_ms;
	if (stamp < window_min || stamp > window_max) return false;
	if (e->item_type_index == 0 || !e->has_item_def) return false;
	const uint32_t flags = e->flags | e->engine_flags; // the one Flags dword
	if ((flags & 0x100u) != 0) return false;
	if ((flags & 0x2u) != 0 || (flags & 0x4u) != 0) return true;
	return e->health <= 0;
}

std::vector<uint8_t> Server_CollectKillPage(const world::World &world, bool spawns_held,
		uint32_t window_min, uint32_t window_max, uint16_t start) {
	std::vector<uint8_t> out;
	if (spawns_held) return out;
	PoolWalk walk;
	uint16_t current = walk_begin(world, walk, start);
	if (current == 0xFFFF) return out;
	out.resize(2);
	int count = 0;
	do {
		if (kill_page_entity_eligible(world, window_min, window_max, current)) {
			out.push_back(static_cast<uint8_t>(current & 0xFFu));
			out.push_back(static_cast<uint8_t>(current >> 8));
			++count;
		}
		current = walk_advance(world, walk);
	} while (current != 0xFFFF && count <= 32);
	put_u16(out, 0, current);
	return out;
}

std::vector<ProtocolMessage> Server_HandleKillPageRequest(const NapiNPServerCtx *ctx,
		const NapiNPConnection &conn, const std::vector<uint8_t> &payload,
		const world::World *world) {
	std::vector<ProtocolMessage> replies;
	if (ctx == nullptr || ctx->is_authority == 0 || world == nullptr) return replies;
	if (!player_slot_active(conn)) return replies;
	// One cursor: a read that does not fit yields 0 and leaves it in place.
	size_t at = 0;
	const auto read = [&payload, &at](size_t width) -> uint32_t {
		if (at + width > payload.size()) return 0;
		uint32_t v = 0;
		for (size_t i = 0; i < width; ++i) v |= uint32_t(payload[at + i]) << (8 * i);
		at += width;
		return v;
	};
	const uint32_t window_min = read(4);
	const uint32_t window_max = read(4);
	const uint16_t start = static_cast<uint16_t>(read(2));
	std::vector<uint8_t> page = Server_CollectKillPage(*world,
			world->match.outcome().ended, window_min, window_max, start);
	if (page.empty() || !is_in_match(conn)) return replies;
	replies.push_back(make_protocol_message(s2c::KILL_BY_SLOT, std::move(page)));
	return replies;
}

} // namespace opennova::inmatch
