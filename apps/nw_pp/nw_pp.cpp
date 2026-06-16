// nw_pp — NovaWorld in-game packet pretty-printer.
//
// Reads a hexcap (the format `tools/net/pcap_to_hexcap.py` produces, also
// the env-var input for `nw_ingame_histogram_test`) and emits one line per
// outer datagram plus one structured block per inner protocol message. Drives
// the SAME outer-decode pipeline as `nw_ingame_histogram_test` and
// `nw_ingame_pool_records_test` — envelope CRC → outer NWU → per-session
// SCRK → 0x43/0x83 → reassembly — so what it prints is the exact byte stream
// the shipping libs see, not a parallel re-implementation.
//
// Tag-specific decoders live in `libs/novaworld/include/novaworld/ingame_decode.h`
// (shared with `nw_ingame_pool_records_test` and the future real handlers).
// As new tags get field maps in docs/net/novaworld-net-re.md, their decoders
// land there and a printer for them lands here.
//
// CLI:
//   nw_pp <hexcap-path>                 # all frames
//   nw_pp <hexcap-path> 0x0d 0x20       # filter to listed S2C tags
//   NW_INGAME_HEXCAP=<path> nw_pp       # env-driven (matches test convention)

#include <napi/envelope.h>
#include <napi/tlv.h>
#include <novacrypto/nwu.h>
#include <novaworld/ingame_decode.h>
#include <novaworld/protocol_message.h>
#include <novaworld/session_hello.h>
#include <novaworld/session_keys.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace opennova;

namespace {

struct Datagram {
	int srcport = 0;
	int frame = 0;
	std::vector<uint8_t> bytes;
};

bool hex_to_bytes(const std::string &hex, std::vector<uint8_t> &out) {
	if (hex.size() % 2 != 0) return false;
	out.clear();
	auto nib = [](char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	};
	for (size_t i = 0; i < hex.size(); i += 2) {
		const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
		if (hi < 0 || lo < 0) return false;
		out.push_back(static_cast<uint8_t>((hi << 4) | lo));
	}
	return true;
}

bool decode_outer(const std::vector<uint8_t> &raw, uint8_t &opcode,
                  std::vector<uint8_t> &body) {
	std::vector<uint8_t> stripped(raw.size());
	size_t out = 0;
	if (napi_envelope_decode(raw.data(), raw.size(), stripped.data(),
	                         stripped.size(), &out) != 0)
		return false;
	stripped.resize(out);
	if (stripped.empty()) return false;
	opcode = stripped[0];
	body.assign(stripped.begin() + 1, stripped.end());
	if (!body.empty()) nwu_encrypt(body.data(), body.size(), SESSION_NWU_KEY);
	return true;
}

std::string to_hex_sample(const uint8_t *p, size_t n, size_t cap = 48) {
	std::string s;
	char buf[4];
	for (size_t i = 0; i < n && i < cap; ++i) {
		std::snprintf(buf, sizeof(buf), "%02x", p[i]);
		s += buf;
		if (i + 1 < n && i + 1 < cap) s += ' ';
	}
	if (n > cap) s += " ..";
	return s;
}

double fp16(int32_t v) { return double(v) / 65536.0; }

std::string handle_str(uint16_t h) {
	char buf[24];
	if (h == 0xFFFF) std::snprintf(buf, sizeof(buf), "0xFFFF=none");
	else std::snprintf(buf, sizeof(buf), "0x%04x p%u/s%u", h,
	                   unsigned(h >> 12), unsigned(h & 0xFFF));
	return buf;
}

void print_pool_spawn_record(int index, const PoolSpawnRecord &r) {
	std::printf("        record %d flags=0x%04x slot=%s type=0x%04x "
	            "name=%-12s pos=(%.1f, %.1f, %.1f)",
	            index, r.spawn_flags, handle_str(r.slot_id).c_str(),
	            r.item_type_id, ("\"" + r.entity_name + "\"").c_str(),
	            fp16(r.pos_x), fp16(r.pos_y), fp16(r.pos_z));
	if (r.spawn_flags & 0x0020) std::printf(" entity36=0x%08x", r.entity_flags);
	if (r.spawn_flags & 0x0001) std::printf(" velX");
	if (r.spawn_flags & 0x0002) std::printf(" velY");
	if (r.spawn_flags & 0x0004) std::printf(" velZ");
	if (r.spawn_flags & 0x0008) std::printf(" sectionMask");
	if (r.spawn_flags & 0x0010) std::printf(" orient=0x%02x", r.orient_byte);
	if (r.spawn_flags & 0x0100)
		std::printf(" parent=%s", handle_str(r.parent_handle).c_str());
	if (r.spawn_flags & 0x0200)
		std::printf(" target=%s", handle_str(r.target_handle).c_str());
	if (r.spawn_flags & 0x0400) {
		std::printf(" weapMask=0x%02x", r.weapon_mask);
		if (r.weapon_mask) {
			int wcount = 0;
			for (int b = 0; b < 8; ++b) if (r.weapon_mask & (1u << b)) wcount++;
			std::printf(" weapons=%d+2extra", wcount);
		}
	}
	std::printf(" team=0x%02x", r.team_byte);
	if (r.spawn_flags & 0x0800)
		std::printf(" AItrailer{p1=0x%08x p2=0x%08x name=\"%s\"}",
		            r.ai_profile_1, r.ai_profile_2, r.ai_name.c_str());
	if (r.spawn_flags & 0x0040) std::printf(" alert=0x%02x", r.alert_byte);
	if (r.spawn_flags & 0x0080) std::printf(" action=0x%02x", r.action_byte);
	if (r.spawn_flags & 0x1000)
		std::printf(" weapType=0x%02x", r.weapon_type_byte);
	if (r.spawn_flags & 0x2000)
		std::printf(" health=0x%02x/0x%04x", r.health_byte, r.health_short);
	else if (r.spawn_flags & 0x8000)
		std::printf(" healthShort=0x%04x", r.health_short);
	if (r.spawn_flags & 0x4000) std::printf(" diff=0x%02x", r.difficulty_byte);
	std::printf("\n");
}

void print_pool3_sync_record(uint16_t slot_idx, const Pool3SyncRecord &r) {
	if (r.is_empty_slot) {
		std::printf("        slot %u type=0 (empty-slot sentinel)\n",
		            unsigned(slot_idx));
		return;
	}
	std::printf("        slot %u type=0x%04x flags=0x%02x "
	            "pos=(%.1f, %.1f, %.1f)",
	            unsigned(slot_idx), r.item_type_id, r.flags_byte,
	            fp16(r.pos_x), fp16(r.pos_y), fp16(r.pos_z));
	if (r.flags_byte & 0x01) std::printf(" parent=0x%08x", r.parent_handle);
	if (r.flags_byte & 0x02) std::printf(" orient=0x%08x", r.orientation_val);
	if (r.flags_byte & 0x04) std::printf(" ammo=%u", unsigned(r.ammo_count));
	std::printf(" net=%s", handle_str(r.net_handle).c_str());
	if (r.flags_byte & 0x08) std::printf(" team=0x%02x", r.team_byte);
	if (r.flags_byte & 0x10) std::printf(" weapType=0x%04x", r.weapon_type);
	if (r.flags_byte & 0x20) std::printf(" score=0x%02x", r.score_byte);
	std::printf("\n");
}

void print_tag_0d(const std::vector<uint8_t> &body) {
	PoolSpawnBatch batch;
	const bool clean = decode_pool_spawn_batch(body.data(), body.size(), batch);
	std::printf("        [0x0D] entityCount=%d (body %zu B%s%s)\n",
	            int(batch.entity_count), body.size(),
	            batch.sentinel_ended_early ? ", sentinel-ended" : "",
	            clean ? "" : ", DECODE INCOMPLETE");
	for (size_t i = 0; i < batch.records.size(); ++i)
		print_pool_spawn_record(int(i), batch.records[i]);
}

void print_tag_20(const std::vector<uint8_t> &body) {
	Pool3SyncBatch batch;
	const bool clean = decode_pool3_sync_batch(body.data(), body.size(), batch);
	std::printf("        [0x20] startIdx=%u entityCount=%d (body %zu B%s)\n",
	            unsigned(batch.start_index), int(batch.entity_count),
	            body.size(), clean ? "" : ", DECODE INCOMPLETE");
	for (size_t i = 0; i < batch.records.size(); ++i)
		print_pool3_sync_record(uint16_t(batch.start_index + i),
		                        batch.records[i]);
}

struct DirState {
	ProtocolReassemblyState rs;
	bool have_pending = false;
	int pending_tag = 0;
	int pending_first_frame = 0;
};

void print_payload(char dir, int frame, int tag,
                   const std::vector<uint8_t> &payload) {
	std::printf("[%c f=%-4d tag=0x%02x len=%zu]\n", dir, frame, tag,
	            payload.size());
	if (dir == 'S' && tag == 0x0D) print_tag_0d(payload);
	else if (dir == 'S' && tag == 0x20) print_tag_20(payload);
	else if (!payload.empty()) std::printf("        %s\n",
	                                       to_hex_sample(payload.data(),
	                                                     payload.size()).c_str());
}

void process_protocol(const std::vector<uint8_t> &body, const std::string &scrk,
                      char dir, DirState &st, int frame,
                      const std::set<int> &tag_filter) {
	if (scrk.empty()) return;
	ProtocolPacketHeader hdr;
	std::vector<ProtocolMessage> msgs;
	if (!decode_protocol_packet_plaintext(body.data(), body.size(), scrk, hdr,
	                                      msgs))
		return;
	for (const auto &pm : msgs) {
		int tag = int(pm.full_tag);
		if (pm.flags.settings_update) tag |= 0x1000;
		if (!st.have_pending) {
			st.pending_tag = tag;
			st.pending_first_frame = frame;
			st.have_pending = true;
		}
		std::vector<uint8_t> assembled;
		if (!reassemble_protocol_payload(st.rs, pm, assembled)) continue;
		if (tag_filter.empty() ||
		    tag_filter.count(st.pending_tag & 0xFF))
			print_payload(dir, st.pending_first_frame, st.pending_tag,
			              assembled);
		st.have_pending = false;
	}
}

} // namespace

int main(int argc, char *argv[]) {
	const char *path = nullptr;
	std::set<int> tag_filter;
	for (int i = 1; i < argc; ++i) {
		const char *a = argv[i];
		if (a[0] == '0' && (a[1] == 'x' || a[1] == 'X')) {
			tag_filter.insert(int(std::strtol(a, nullptr, 16)));
		} else if (!path) {
			path = a;
		}
	}
	if (!path) path = std::getenv("NW_INGAME_HEXCAP");
	if (!path || !*path) {
		std::fprintf(stderr,
		             "usage: nw_pp <hexcap> [0xNN ...]\n"
		             "       or set NW_INGAME_HEXCAP\n");
		return 1;
	}
	std::ifstream file(path);
	if (!file) {
		std::fprintf(stderr, "FAILED to open %s\n", path);
		return 1;
	}

	std::vector<Datagram> dgrams;
	std::string line;
	while (std::getline(file, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty() || line[0] == '#') continue;
		std::istringstream ls(line);
		Datagram d;
		std::string hex;
		if (!(ls >> d.srcport >> d.frame >> hex)) continue;
		if (!hex_to_bytes(hex, d.bytes)) continue;
		dgrams.push_back(std::move(d));
	}
	std::fprintf(stderr, "loaded %zu datagrams from %s\n", dgrams.size(), path);
	if (!tag_filter.empty()) {
		std::fprintf(stderr, "tag filter:");
		for (int t : tag_filter) std::fprintf(stderr, " 0x%02x", t);
		std::fprintf(stderr, "\n");
	}

	std::string client_scrk, server_scrk;
	DirState cstate, sstate;
	for (const auto &d : dgrams) {
		uint8_t op = 0;
		std::vector<uint8_t> body;
		if (!decode_outer(d.bytes, op, body)) continue;
		switch (op) {
		case SESSION_OPCODE_CLIENT_AUTH: {
			ClientAuth a;
			if (parse_client_auth(body.data(), body.size(), a))
				client_scrk = a.scrk;
			break;
		}
		case SESSION_OPCODE_SERVER_AUTH: {
			ServerAuth a;
			if (parse_server_auth(body.data(), body.size(), a))
				server_scrk = a.scrk;
			break;
		}
		case SESSION_OPCODE_PROTOCOL_MESSAGE:
			process_protocol(body, client_scrk, 'C', cstate, d.frame,
			                 tag_filter);
			break;
		case SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE:
			process_protocol(body, server_scrk, 'S', sstate, d.frame,
			                 tag_filter);
			break;
		default:
			break;
		}
	}
	return 0;
}
