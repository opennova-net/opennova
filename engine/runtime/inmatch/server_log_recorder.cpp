// The /PROFILE server log recorder (server_log_recorder.h) — the CServerLog
// object [orig: g_ServerLog @0xb79448; the writer cluster @0x4e1a10..0x4e1ec0].
#include <runtime/inmatch/server_log_recorder.h>

#include <net/npwire/serverlog_encode.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/world.h>

#include <base/io/crt_ftol.h>

#include <cstdio>
#include <cstdlib>

namespace opennova::inmatch {

namespace {

// Path_ReplaceOrAppendExtension: everything after the FIRST '.' becomes the
// extension; no '.' appends one [orig: @0x53c780 — the forward scan
// @0x53c7bd..0x53c7cb].
std::string replace_or_append_extension(std::string path, const char *ext) {
	const size_t dot = path.find('.');
	if (dot == std::string::npos) return path + "." + ext;
	path.resize(dot + 1);
	return path + ext;
}

bool is_player(const world::Entity &e) {
	return ((e.flags | e.engine_flags) & world::kEntityFlagPlayer) != 0;
}

// The entity's Flags dword (entity+36): the port keeps it in two coherent
// views, as the player 0x100 / mounted 0x40 consumers read it.
uint32_t entity_flags(const world::Entity &e) { return e.flags | e.engine_flags; }

// Entity_ValidatePtr: the active player slot whose entity is `e`, else none
// [orig: @0x500910 — `slot+4 && *slot == entity` over g_PlayerSlots].
const NapiNPConnection *slot_of(const NapiNPServerCtx &ctx, const world::Entity &e) {
	for (const NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (player_slot_active(c) && c.link.owned_entity == e.handle) return &c;
	}
	return nullptr;
}

} // namespace

std::string server_log_file_name(ServerFileSink &files, const std::string &profile_path) {
	std::string path = replace_or_append_extension(profile_path, "sph");
	bool wrapped = false;
	while (files.exists(path)) {
		// Cut at the last '.', then back over the digits before it; a name
		// that is all digits stops at its first byte (retail reads on past
		// the buffer's head there) [orig: strrchr @0x4e1f4e, the scan
		// @0x4e1f62..0x4e1f71].
		size_t dot = path.rfind('.');
		if (dot == std::string::npos) dot = path.size();
		size_t digits = dot;
		while (digits > 0 && path[digits - 1] >= '0' && path[digits - 1] <= '9') --digits;
		// The CRT atol + 1 [orig: _atol @0x4e1f75] (io::retail_atol; D-NET-384), the add
		// wrapping in 32 bits.
		int next = static_cast<int32_t>(static_cast<uint32_t>(
				io::retail_atol(path.substr(digits, dot - digits).c_str())) + 1u);
		if (next > 10) {
			next = 1;
			wrapped = true;
		}
		path = replace_or_append_extension(path.substr(0, digits) + std::to_string(next), "sph");
		// Once wrapped, every candidate is deleted, so the next test finds it
		// gone [orig: @0x4e1ff2..0x4e1ff9].
		if (wrapped) files.remove(path);
	}
	return path;
}

void server_logs_deploy(ServerLogRecorder *profile, const world::Entity &player) {
	if (profile != nullptr) profile->write_death(player);
}

uint32_t server_log_entity_id(const world::Entity &e) {
	return is_player(e) ? e.owner_connection_id : static_cast<uint32_t>(e.net_id);
}

ServerLogRecorder::ServerLogRecorder(ServerFileSink &files, std::string profile_path)
	: files_(files), profile_path_(std::move(profile_path)) {
	roster_.fill(0xFFFFFFFFu);
}

ServerLogRecorder::~ServerLogRecorder() { close(); }

bool ServerLogRecorder::open(const std::string &map_file_name) {
	// [orig: ServerLog_OpenForWrite @0x4e1ec0 — the close @0x4e1eda, the
	//  cleared header and the -1 roster table @0x4e1eef..0x4e1efe]
	close();
	buffer_.clear();
	roster_.fill(0xFFFFFFFFu);
	roster_count_ = 0;
	file_ = server_log_file_name(files_, profile_path_);
	// fopen(.., "wb") [orig: @0x4e201e]; a failure leaves the recorder closed
	// [orig: @0x4e202b].
	if (!files_.write(file_, std::string_view(), /*append=*/false)) {
		open_ = false;
		return false;
	}
	open_ = true;
	buffer_.reserve(kBufferBytes);
	append_server_log_begin(buffer_, map_file_name); // [orig: @0x4e2056..0x4e206f]
	return true;
}

void ServerLogRecorder::close() {
	// [orig: CServerLog_CloseAndFree @0x4e1a10 — the .END only when the
	//  buffer holds something @0x4e1a1e, its flush rule `2 * len + 8`
	//  @0x4e1a2a, the final fwrite @0x4e1a68, fclose @0x4e1a73]
	if (open_ && !buffer_.empty()) {
		flush_if(2 * buffer_.size() + 8 > kBufferBytes);
		append_server_log_end(buffer_);
		flush();
	}
	open_ = false;
	buffer_.clear();
	buffer_.shrink_to_fit();
}

void ServerLogRecorder::flush() {
	if (!buffer_.empty()) {
		files_.write(file_,
				std::string_view(reinterpret_cast<const char *>(buffer_.data()), buffer_.size()),
				/*append=*/true);
	}
	buffer_.clear();
}

void ServerLogRecorder::flush_if(bool full) {
	if (full) flush();
}

bool ServerLogRecorder::roster_has(uint32_t id) const {
	for (const uint32_t stored : roster_)
		if (stored == id) return true;
	return false;
}

void ServerLogRecorder::write_player(const world::Entity &e) {
	// [orig: CServerLog_WritePlayerNameRecord @0x4e1cc0 — the 128-entry walk
	//  @0x4e1cf8..0x4e1d16; a new id is stored at the count's index and the
	//  count bumped @0x4e1d26 / @0x4e1d3b / @0x4e1d69]
	if (!open_) return;
	const uint32_t id = server_log_entity_id(e);
	if (roster_has(id)) return;
	// Retail stores the 129th distinct id over its own count word and walks on
	// past the table; the port keeps the table bounded and still writes the
	// record (D-NET-355).
	if (roster_count_ < kRosterIds) roster_[static_cast<size_t>(roster_count_)] = id;
	++roster_count_;
	// A player's name is its entity Name (entity+0xF4); anyone else's is
	// "#<DcbId>" [orig: @0x4e1d2a; sprintf(off_7CD428 "#%d") @0x4e1d4d].
	std::string name;
	if (is_player(e)) {
		name = e.display_name;
	} else {
		char buf[28];
		std::snprintf(buf, sizeof(buf), "#%d", static_cast<int>(e.net_id));
		name = buf;
	}
	const size_t name_len = name.size() + 1;
	flush_if(buffer_.size() + name_len + 20 > kBufferBytes); // [orig: @0x4e1d82]
	append_server_log_player(buffer_, id, static_cast<int8_t>(e.team), name);
}

void ServerLogRecorder::write_mission_roster(const NapiNPServerCtx &ctx,
		const world::World &world) {
	// [orig: Game_StartMission @0x526135..0x52617b — every g_PoolList[0] row
	//  with Flags 0x100, or every row when not in a session and the authority]
	if (!open_) return;
	const bool every_row = ctx.is_in_session == 0 && ctx.is_authority == 1;
	world.registry.for_each_in_pool(0, [&](const world::Entity &e) {
		if (is_player(e) || every_row) write_player(e);
	});
}

void ServerLogRecorder::write_frame(const NapiNPServerCtx &ctx, const world::World &world,
		uint32_t tick) {
	// [orig: Game_ProcessMainFrame @0x526879..0x5268e7 — `(g_CurrentTick & 7)
	//  == 7` @0x52688b, FBEG(tick >> 3) @0x526899, the pool-0 walk with the same
	//  row rule as the roster pass @0x5268b5..0x5268db]
	if (!open_ || (tick & 7u) != 7u) return;
	flush_if(2 * buffer_.size() + 12 > kBufferBytes); // [orig: @0x4e1ab6]
	append_server_log_frame(buffer_, static_cast<uint32_t>(static_cast<int32_t>(tick) >> 3));
	const bool every_row = ctx.is_in_session == 0 && ctx.is_authority == 1;
	world.registry.for_each_in_pool(0, [&](const world::Entity &e) {
		if (!is_player(e) && !every_row) return;
		// [orig: CServerLog_WritePositionRecord @0x4e1b00]
		flush_if(buffer_.size() + 44 > kBufferBytes); // [orig: @0x4e1b1c]
		// The position and attitude the wire carries for the entity
		// (replication's world<->wire seam): entity+4/+8/+12, the +16 yaw
		// and +24 roll BAMs.
		const auto s = replication::snapshot_of(e);
		ServerLogEntity rec;
		rec.net_id = server_log_entity_id(e);
		rec.pos_x = s.x;
		rec.pos_y = s.y;
		rec.pos_z = s.z;
		rec.yaw_bam = static_cast<uint32_t>(s.euler_z);
		rec.angle2 = static_cast<uint32_t>(s.roll_bam);
		rec.flags = entity_flags(e);
		// 1 while the entity has a parent (a seat) [orig: @0x4e1baa..0x4e1bb3].
		rec.vehicle_flag = e.mounted && e.mount_target.valid() ? 1 : 0;
		// The owning slot's frame-rate statistic, 0 without a slot
		// [orig: Entity_ValidatePtr @0x4e1b61, parent[0x15F78] @0x4e1b6d].
		const NapiNPConnection *slot = slot_of(ctx, e);
		rec.stat_byte = slot != nullptr ? slot->link.uplink_frame_rate : uint16_t{0};
		append_server_log_entity(buffer_, rec);
	});
}

void ServerLogRecorder::write_death(const world::Entity &e) {
	// [orig: CServerLog_WriteDeathMarker @0x4e1e00 — the roster-table walk
	//  returns on a miss @0x4e1e3e; the flush rule @0x4e1e54]
	if (!open_) return;
	const uint32_t id = server_log_entity_id(e);
	if (!roster_has(id)) return;
	flush_if(2 * buffer_.size() + 12 > kBufferBytes);
	append_server_log_event(buffer_, ServerLogEventKind::Death, id);
}

void ServerLogRecorder::write_disconnect(const world::Entity &e) {
	// [orig: CServerLog_WriteDisconnectMarker @0x4e1c50 — no table test; the
	//  flush rule @0x4e1c65]
	if (!open_) return;
	flush_if(buffer_.size() + 12 > kBufferBytes);
	append_server_log_event(buffer_, ServerLogEventKind::Disconnect, server_log_entity_id(e));
}

} // namespace opennova::inmatch
