#include <bms/parser.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace opennova {

namespace {

constexpr size_t BMS_HEADER_SIZE = 0x268;
constexpr size_t BMS_ENTITY_SIZE = 0xAC; // 172 bytes per record across all pools
constexpr uint8_t BMS_MIN_VERSION = 19;

inline uint16_t read_u16_le(const uint8_t *p) {
	return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline int16_t read_i16_le(const uint8_t *p) {
	return static_cast<int16_t>(read_u16_le(p));
}

inline uint32_t read_u32_le(const uint8_t *p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
			(static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline int32_t read_i32_le(const uint8_t *p) {
	return static_cast<int32_t>(read_u32_le(p));
}

bool walk_pool(const uint8_t *data, size_t len, size_t &cursor,
               uint32_t pool, uint32_t count, std::vector<BmsEntity> &out) {
	for (uint32_t i = 0; i < count; ++i) {
		if (cursor + BMS_ENTITY_SIZE > len) {
			return false;
		}
		const uint8_t *r = data + cursor;
		BmsEntity e{};
		e.type_id = read_u32_le(r + 0);
		e.flags = read_u32_le(r + 12);
		e.x = read_i32_le(r + 16);
		e.y = read_i32_le(r + 20);
		e.z = read_i32_le(r + 24);
		e.yaw = read_i16_le(r + 56);
		e.pitch = read_i16_le(r + 58);
		e.roll = read_i16_le(r + 60);
		e.team = r[73];
		e.pool = static_cast<uint16_t>(pool);
		e.slot = static_cast<uint16_t>(i);
		out.push_back(e);
		cursor += BMS_ENTITY_SIZE;
	}
	return true;
}

} // namespace

bool bms_parse(const uint8_t *data, size_t len, BmsMission &out) {
	if (!data || len < BMS_HEADER_SIZE) {
		return false;
	}
	if (data[0] != 'B' || data[1] != 'M' || data[2] != 'S') {
		return false;
	}
	out.version = data[3];
	if (out.version < BMS_MIN_VERSION) {
		return false;
	}
	out.game_type = read_u32_le(data + 0x88);

	const uint32_t pool1_count = read_u32_le(data + 0xA4);
	const uint32_t pool2_count = read_u32_le(data + 0xA8);
	const uint32_t pool3_count = read_u32_le(data + 0xAC);
	const uint32_t pool0_count = read_u32_le(data + 0xB0);
	const uint16_t loadout1_size = read_u16_le(data + 0x242);
	const uint16_t loadout2_size = read_u16_le(data + 0x246);

	// Mission_LoadBMSFile reads the loadout blocks into a stack buffer when
	// loading single-player, or fseek-skips them when loading multiplayer.
	// Either way, the bytes are present in the file at this position; we just
	// step over them to reach the entity pools.
	size_t cursor = BMS_HEADER_SIZE + loadout1_size + loadout2_size;
	if (cursor > len) {
		return false;
	}

	out.entities.clear();
	out.entities.reserve(static_cast<size_t>(pool1_count) +
			pool2_count + pool3_count + pool0_count);

	// Engine read order in Mission_LoadBMSFile: pool 1 → pool 2 → pool 3 → pool 0.
	if (!walk_pool(data, len, cursor, 1, pool1_count, out.entities)) return false;
	if (!walk_pool(data, len, cursor, 2, pool2_count, out.entities)) return false;
	if (!walk_pool(data, len, cursor, 3, pool3_count, out.entities)) return false;
	if (!walk_pool(data, len, cursor, 0, pool0_count, out.entities)) return false;

	// Trailing waypoints / groups / layers / events / nav-zones are not
	// required for tag=0x0E spawn lookup; ignore.
	return true;
}

bool bms_load_file(const std::string &path, BmsMission &out) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	const std::string buf = ss.str();
	return bms_parse(reinterpret_cast<const uint8_t *>(buf.data()), buf.size(), out);
}

bool bms_pick_safe_spawn(const BmsMission &mission, BmsEntity &out) {
	// Prefer pool 1 first (AI / organics — entities the map designer placed
	// to STAND on the terrain at human eye level). Pool 2 entries can be
	// buildings whose Position is at building origin (not necessarily ground
	// level) or vehicles that float at deck height; using one of those puts
	// the player mid-air on top of an object.
	//
	// Filter rules (applied to each pool in turn):
	//   - type_id != 0  (real entity, not an empty slot)
	//   - position not at the origin  (uninitialized record)
	//   - |X| < 1 000 000 000  (reject extreme placeholders observed in the
	//     dvxi5 BMS — pool1[6] had pos=(-1.39e9, -7.88e8, 0) which is far
	//     outside any sensible engine-coord range)
	auto plausible = [](const BmsEntity &e) -> bool {
		if (e.type_id == 0) return false;
		if (e.x == 0 && e.y == 0 && e.z == 0) return false;
		if (e.x < -1'000'000'000 || e.x > 1'000'000'000) return false;
		if (e.y < -1'000'000'000 || e.y > 1'000'000'000) return false;
		return true;
	};

	for (uint16_t preferred_pool : {static_cast<uint16_t>(1),
			static_cast<uint16_t>(3),
			static_cast<uint16_t>(0),
			static_cast<uint16_t>(2)}) {
		std::vector<const BmsEntity *> candidates;
		for (const auto &e : mission.entities) {
			if (e.pool != preferred_pool) continue;
			if (!plausible(e)) continue;
			candidates.push_back(&e);
		}
		if (candidates.empty()) continue;
		// Within the chosen pool, sort by Z and take the median entry —
		// closer to ground than the highest, but above any below-terrain
		// noise on the lowest end.
		std::sort(candidates.begin(), candidates.end(),
				[](const BmsEntity *a, const BmsEntity *b) { return a->z < b->z; });
		out = *candidates[candidates.size() / 2];
		return true;
	}
	return false;
}

} // namespace opennova
