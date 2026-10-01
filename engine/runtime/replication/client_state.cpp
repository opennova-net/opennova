// The ClientState lookups, the per-row pulse drain and the session-status /
// briefing-string folds, split out of client_replica_pipeline.cpp (the size
// ratchet); the state's own witness map
// is in client_state.h. The handle lookup is the (pool << 12) | slot resolve
// every S2C entity handler runs before touching a row
// [orig: e.g. NapiNPClientMsg_EntityDeath @0x42eb50 — pool nibble < 5, slot <
//  the pool's capacity, base + slot * stride].
#include <runtime/replication/client_state.h>
#include <cstddef>
#include <cstdint>

namespace opennova::replication {

// ---- ClientState lookup -----------------------------------------------------

ClientEntityState *ClientState::find(uint16_t handle) {
	// `entities` is intentionally public decoded state. Callers may clear,
	// reorder, append, or edit it directly, so a separate handle-to-index cache
	// cannot remain valid without changing that API. Keep lookup derived from
	// the authoritative vector.
	for (ClientEntityState &entity : entities) {
		if (entity.handle == handle) return &entity;
	}
	return nullptr;
}

const ClientEntityState *ClientState::find(uint16_t handle) const {
	for (const ClientEntityState &entity : entities) {
		if (entity.handle == handle) return &entity;
	}
	return nullptr;
}

ClientEntityState &ClientState::upsert(uint16_t handle) {
	if (ClientEntityState *e = find(handle)) return *e;
	ClientEntityState e;
	e.handle = handle;
	entities.push_back(e);
	mark_topology_changed();
	return entities.back();
}

void ClientState::clear_anim_pulses() {
	for (ClientEntityState &e : entities) e.anim_state_pulse = -1;
}

// ---- the session status (S2C 0x58) and the briefing strings (S2C 0x7E) -----

ClientSessionStatus fold_session_status(const SessionStatusBlock &block, uint32_t now_ms) {
	// The decode is the handler's lenient cursor: a short body leaves the
	// fields it could not read 0 and no option pairs, and the record is
	// still stamped valid [orig: SessionStatus_ParseFromBuffer @0x530ed0 — the
	// memset @0x530ee6, every read `cursor + n <= end ? read : 0`].
	ClientSessionStatus out;
	out.server_name = block.server_name.substr(0, 31);   // the 32-byte field
	out.mission_name = block.mission_name.substr(0, 63); // the 64-byte field
	out.game_type_byte = block.byte0;
	out.score_table = block.byte1;
	out.max_players = block.byte2;
	out.uptime_ms = block.uptime_ms;
	for (size_t i = 0; i < out.stats.size(); ++i) out.stats[i] = block.stat_values[i];
	for (const SessionStatusKV &kv : block.kv) {
		// [orig: `count < 8 && key <= 9` @0x53107f]
		if (out.options.size() < 8 && kv.key <= 9) out.options.push_back({kv.key, kv.value});
	}
	out.stamp_ms = now_ms; // +0x74 = GetTickCount()
	out.valid = true;      // @0x5310aa
	return out;
}

uint32_t session_status_elapsed_ms(const ClientSessionStatus &status, uint32_t now_ms) {
	// [orig: SessionStatus_GetElapsedMS @0x52d5f0 — `GetTickCount() + (+0x70 -
	//  +0x74)`]
	if (!status.valid) return 0;
	return now_ms + (status.uptime_ms - status.stamp_ms);
}

void fold_server_config_strings(const std::vector<uint8_t> &body, ClientServerConfigStrings &out) {
	// The first string runs to its NUL; the second starts just past it, the
	// body end clamping both [orig: NapiNPClientMsg_ServerConfigStrings
	// @0x425e20 — strlen, `if (second > end) second = end` @0x425e41].
	size_t first_end = 0;
	while (first_end < body.size() && body[first_end] != 0) ++first_end;
	out.first.assign(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(first_end));
	const size_t second_start = first_end + 1 < body.size() ? first_end + 1 : body.size();
	size_t second_end = second_start;
	while (second_end < body.size() && body[second_end] != 0) ++second_end;
	out.second.assign(body.begin() + static_cast<std::ptrdiff_t>(second_start),
			body.begin() + static_cast<std::ptrdiff_t>(second_end));
}

// strncpy(dst, src, 0x400): a source of 1024 or more characters leaves the
// buffer unterminated.
static constexpr size_t kServerConfigBuffer = 0x400;

std::string server_config_first_text(const ClientServerConfigStrings &strings) {
	return strings.first.substr(0, kServerConfigBuffer);
}

std::string server_config_second_text(const ClientServerConfigStrings &strings) {
	// byte_A86120 + 0x400 is byte_A86520: an unterminated second buffer reads
	// on into the first.
	if (strings.second.size() < kServerConfigBuffer) return strings.second;
	return strings.second.substr(0, kServerConfigBuffer) + server_config_first_text(strings);
}

} // namespace opennova::replication
