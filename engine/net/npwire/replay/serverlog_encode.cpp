#include <net/npwire/serverlog_encode.h>

#include <base/io/le.h>
#include <formats/sph/sph.h>

#include <cstring>

// `/PROFILE` .sph record writers (serverlog_encode.h) — the CServerLog_*
// writer cluster [orig: @0x4e1a10..0x4e1e00, ServerLog_OpenForWrite @0x4e1ec0].

namespace opennova {

namespace {

void put_u32(std::vector<uint8_t> &p, uint32_t v) { io::append_u32_le(p, v); }
void put_u16(std::vector<uint8_t> &p, uint16_t v) { io::append_u16_le(p, v); }

} // namespace

void append_server_log_begin(std::vector<uint8_t> &out, const std::string &map_file_name) {
	std::vector<uint8_t> p;
	put_u32(p, kServerLogVersion); // [orig: @0x4e2060]
	// strncpy(.., 15): up to 15 name bytes, zero-filled past a shorter name;
	// the 16th byte is never stored [orig: @0x4e2067; the stray NUL @0x4e206c
	// lands at +28, the next chunk's first byte].
	char name[16] = {};
	const size_t n = std::strlen(map_file_name.c_str());
	std::memcpy(name, map_file_name.c_str(), n < 15 ? n : 15);
	p.insert(p.end(), name, name + 16);
	sph::append_chunk(out, "NGEB", p.data(), p.size()); // 'BEGN' LE [orig: @0x4e2056]
}

void append_server_log_player(std::vector<uint8_t> &out, uint32_t net_id, int8_t team,
		const std::string &name) {
	std::vector<uint8_t> p;
	const std::string stored = name.c_str(); // strlen stops at the first NUL
	put_u32(p, net_id);                                       // [orig: @0x4e1dbf / @0x4e1dc7]
	put_u32(p, static_cast<uint32_t>(static_cast<int32_t>(team))); // movsx [orig: @0x4e1dd2]
	put_u32(p, static_cast<uint32_t>(stored.size() + 1));     // [orig: @0x4e1db0]
	p.insert(p.end(), stored.begin(), stored.end());
	p.push_back(0);                                           // memcpy nameLen [orig: @0x4e1dda]
	sph::append_chunk(out, "FEDP", p.data(), p.size()); // 'PDEF' LE [orig: @0x4e1da6]
}

void append_server_log_frame(std::vector<uint8_t> &out, uint32_t frame_index) {
	std::vector<uint8_t> p;
	put_u32(p, frame_index); // [orig: @0x4e1ae8]
	sph::append_chunk(out, "GEBF", p.data(), p.size()); // 'FBEG' LE [orig: @0x4e1ade]
}

void append_server_log_entity(std::vector<uint8_t> &out, const ServerLogEntity &e) {
	std::vector<uint8_t> p;
	put_u32(p, e.net_id);                                     // +8  [orig: @0x4e1b55 / @0x4e1b5d]
	put_u32(p, static_cast<uint32_t>(-e.pos_y));              // +12 -Y [orig: @0x4e1b7f]
	put_u32(p, static_cast<uint32_t>(e.pos_z));               // +16 Z [orig: @0x4e1b85]
	put_u32(p, static_cast<uint32_t>(e.pos_x));               // +20 X [orig: @0x4e1b8b]
	// +24 takes Pitch and then Yaw; Yaw stands [orig: @0x4e1b91 / @0x4e1b9d].
	put_u32(p, e.yaw_bam);
	put_u32(p, e.angle2);                                     // +28 Roll [orig: @0x4e1b97]
	put_u32(p, 0);                                            // +32 never stored
	put_u32(p, e.flags);                                      // +36 [orig: @0x4e1b77]
	put_u16(p, e.vehicle_flag);                               // +40 [orig: @0x4e1ba4 / @0x4e1bb3]
	put_u16(p, e.stat_byte);                                  // +42 [orig: @0x4e1ba0]
	sph::append_chunk(out, "TADP", p.data(), p.size()); // strcpy "TADP," [orig: @0x4e1b3d]
}

void append_server_log_event(std::vector<uint8_t> &out, ServerLogEventKind kind,
		uint32_t net_id) {
	std::vector<uint8_t> p;
	put_u32(p, net_id);
	// 'PBRK' / 'PREM' LE [orig: @0x4e1e78 / @0x4e1c8d]
	sph::append_chunk(out, kind == ServerLogEventKind::Death ? "KRBP" : "MERP", p.data(),
			p.size());
}

void append_server_log_end(std::vector<uint8_t> &out) {
	sph::append_chunk(out, "DNE.", nullptr, 0); // [orig: @0x4e1a4e]
}

} // namespace opennova
