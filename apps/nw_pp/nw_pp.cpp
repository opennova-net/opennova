// nw_pp — NovaWorld in-game packet pretty-printer.
//
// Reads a pcap/pcapng OR a hexcap (the format `tools/net/pcap_to_hexcap.py`
// produces; the env-var input for `nw_ingame_histogram_test`) and emits one
// line per outer datagram plus one structured block per inner protocol
// message. Drives the SAME outer-decode pipeline as `nw_ingame_histogram_test`
// and `nw_ingame_pool_records_test` — envelope CRC → outer NWU → per-session
// SCRK → 0x43/0x83 → reassembly — so what it prints is the exact byte stream
// the shipping libs see, not a parallel re-implementation.
//
// Input format is auto-detected from the path suffix: `.pcap` / `.pcapng`
// are parsed natively (no Wireshark / tshark required) for the loopback-UDP
// subset of the link-layer space — Ethernet, BSD-loopback (NULL/LOOP),
// raw-IP, IPv4 only. IP fragmentation is not reassembled (loopback MTU is
// 65535 so we don't see it in practice; fragments are dropped with a
// stderr warning). Anything else is read as hexcap text.
//
// Tag-specific decoders live in `libs/novaworld/include/novaworld/ingame_decode.h`
// (shared with `nw_ingame_pool_records_test` and the future real handlers).
// As new tags get field maps in docs/net/novaworld-net-re.md, their decoders
// land there and a printer for them lands here.
//
// CLI:
//   nw_pp <capture-path>                # pcapng/pcap/hexcap all accepted
//   nw_pp <capture-path> 0x0d 0x20      # filter to listed S2C tags
//   NW_INGAME_HEXCAP=<hexcap> nw_pp     # env-driven, hexcap only (test contract)

#include <def/def.h>
#include <napi/envelope.h>
#include <napi/tlv.h>
#include <novacrypto/nwu.h>
#include <novaworld/ingame_decode.h>
#include <novaworld/serverlog_decode.h>
#include <novaworld/protocol_message.h>
#include <novaworld/session_hello.h>
#include <novaworld/session_keys.h>
#include <novaworld/wire_capture.h>
#include <novaworld/replay_timeline.h>
#include <scr/scr.h>

#include "pcap_reader.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
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

bool ends_with_icase(const std::string &s, const char *suffix) {
	const size_t sl = std::strlen(suffix);
	if (s.size() < sl) return false;
	for (size_t i = 0; i < sl; ++i) {
		const char a = std::tolower(static_cast<unsigned char>(s[s.size() - sl + i]));
		const char b = std::tolower(static_cast<unsigned char>(suffix[i]));
		if (a != b) return false;
	}
	return true;
}

bool is_pcap_path(const std::string &p) {
	return ends_with_icase(p, ".pcap") || ends_with_icase(p, ".pcapng");
}

bool is_sph_path(const std::string &p) { return ends_with_icase(p, ".sph"); }

// pcap/pcapng reading is shared with the test suite: apps/common/pcap_reader.h
// The outer-decode pipeline (envelope -> NWU -> SCRK -> 0x43/0x83 -> reassembly)
// is shared too: libs/novaworld/wire_capture.h decode_capture_to_messages.

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

// Pool taxonomy from docs/engine-primer.md + docs/world/world-wac-ai-re.md:
// 0=organics (player + dynamic spawned units), 1=items (vehicles, spawn
// points, props), 2=buildings/static, 3=markers (waypoints, nav, objectives).
const char *pool_label(unsigned p) {
	switch (p) {
		case 0: return "organics";
		case 1: return "items";
		case 2: return "buildings";
		case 3: return "markers";
		default: return "?";
	}
}

std::string handle_str(uint16_t h) {
	char buf[40];
	if (h == 0xFFFF) std::snprintf(buf, sizeof(buf), "0xFFFF=none");
	else std::snprintf(buf, sizeof(buf), "0x%04x p%u(%s)/s%u", h,
	                   unsigned(h >> 12), pool_label(unsigned(h >> 12)),
	                   unsigned(h & 0xFFF));
	return buf;
}

// Tag labels from docs/net/novaworld-net-re.md §4 dispatch tables. Short
// human names just to orient the reader of nw_pp output; not exhaustive,
// just the tags we've seen flow in real captures.
const char *tag_label(char dir, int tag) {
	if (dir == 'S') {
		switch (tag) {
			case 0x00: return "init";
			case 0x02: return "post-handshake";
			case 0x0A: return "per-frame-update";       // §5.9
			case 0x0B: return "BMS-header";              // §5.4
			case 0x0C: return "entity-spawn-batch";
			case 0x0D: return "pool-spawn";              // §5.11
			case 0x0F: return "world-state-load";
			case 0x10: return "static-entity-batch";    // §5.9
			case 0x16: return "player-list";
			case 0x1A: return "wait-for-game-start-ack";
			case 0x1D: return "spawn-success-gate";      // §5.2
			case 0x1E: return "game-event";
			case 0x20: return "pool3-sync";              // §5.12
			case 0x26: return "kill-sync";
			case 0x40: return "capture-zone-state";
			case 0x45: return "terrain-load";
			case 0x46: return "player-sync";
			case 0x4E: return "kill-by-slot";
			case 0x57: return "rtt-echo";
			case 0x5A: return "weapon-loadout";
			case 0x60: return "file-chunk";
			case 0x61: return "session-key";
			case 0x64: return "mission-chunk";
			case 0x6F: return "cinematic-camera";
			case 0x7B: return "full-player-info";
			default: return nullptr;
		}
	} else {
		switch (tag) {
			case 0x00: return "JOIN";
			case 0x06: return "fired-round";
			case 0x0C: return "entity-uplink";            // §5.10 (extended type-10)
			case 0x0D: return "replication-ack";
			case 0x0F: return "spawn-query";              // §5.9 (len-2 = spawn-point query)
			case 0x16: return "chat";
			case 0x21: return "checksum-reply";
			case 0x22: return "burst";
			case 0x23: return "burst";
			case 0x28: return "burst";
			case 0x29: return "burst";
			case 0x2C: return "rtt-consumed";
			case 0x47: return "ping";                     // observed len=0 header-only
			case 0x48: return "client-ack";               // observed 4 B
			case 0x4C: return "client-state-byte";        // observed 1 B (=0x01)
			default: return nullptr;
		}
	}
}

// ItemDef.id → display_name resolution. Populated when --items <path> is
// given on the CLI. Resolves type_ids in 0x0D / 0x20 record dumps so the
// reader sees "type=0x04bd [d_5ton truck]" instead of just a hex id.
std::unordered_map<int, std::string> g_item_names;

// Per-item §5.10b dispatch class — selects which compact decoder runs on a
// tag==1 record inside S2C 0x0A's trailing event loop. The EntityClass enum and
// class_from_tag() now live in libs/novaworld/ingame_decode.h (shared with the
// decode_frame_update walker). This map is the wire_id → class table, populated
// from items.def in load_items_def; an unmapped id leaves the walker unable to
// size a record (fail closed). Keyed by wire_id (items.def id − 100000).
std::unordered_map<uint16_t, EntityClass> g_item_class;

const char *class_name(EntityClass c) {
	switch (c) {
		case EntityClass::Player:   return "Player";
		case EntityClass::Infantry: return "Infantry";
		case EntityClass::Vehicle:  return "Vehicle";
		case EntityClass::Guided:   return "Guided";
		default:                    return "Unknown";
	}
}

std::string type_str(uint16_t type) {
	// display_name field is 128 bytes in DefItemDef; a 192-byte stack
	// buffer comfortably holds the longest name + the "0xNNNN[...]" wrap.
	char buf[192];
	auto it = g_item_names.find(int(type));
	if (it == g_item_names.end()) {
		std::snprintf(buf, sizeof(buf), "0x%04x", type);
		return buf;
	}
	std::snprintf(buf, sizeof(buf), "0x%04x[%s]", type, it->second.c_str());
	return buf;
}

// Load items.def into g_item_names. Accepts plaintext or SCR-encrypted
// input — the libs/scr decryptor expects the SCR magic in the first 3
// bytes, otherwise we treat the file as plaintext .def. Returns count
// of items loaded, 0 on any failure.
size_t load_items_def(const char *path) {
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) {
		std::fprintf(stderr, "items: failed to open %s\n", path);
		return 0;
	}
	const std::streamsize n = f.tellg();
	if (n <= 0) return 0;
	std::vector<uint8_t> raw(static_cast<size_t>(n));
	f.seekg(0);
	if (!f.read(reinterpret_cast<char *>(raw.data()), n)) return 0;

	const uint8_t *plain = raw.data();
	size_t plain_size = raw.size();
	std::vector<uint8_t> decrypted;
	if (scr_is_scr(raw.data(), raw.size())) {
		decrypted.resize(raw.size());
		size_t out_size = decrypted.size();
		if (scr_decrypt_buf(raw.data(), raw.size(), decrypted.data(),
		                    &out_size, SCR_KEY_JO_DFX2) != 0) {
			std::fprintf(stderr, "items: SCR decrypt failed for %s\n", path);
			return 0;
		}
		decrypted.resize(out_size);
		plain = decrypted.data();
		plain_size = decrypted.size();
	}

	DefItemsFile items{};
	if (def_parse_items_memory(plain, plain_size, &items) != 0) {
		std::fprintf(stderr, "items: def_parse_items_memory failed\n");
		return 0;
	}
	// The wire `itemTypeId` is `items.def.id - 100000` (cross-witnessed
	// 2026-06-16: wire 0x050b=1291 ↔ items.def `id 101291` "Drivable Dune
	// Buggy", wire 0x14B9=5305 ↔ id 105305 "Player #1 (Multiplayer)",
	// wire 0x04b0=1200 ↔ id 101200 "Drivable Indonesian LCT"). Engine
	// loader presumably folds the 100000 offset out before storing the
	// runtime `gItemDefs[i].id` field that `ItemList_FindIndexByTypeId
	// @ 0x49E100` compares against.
	for (size_t i = 0; i < items.count; ++i) {
		const DefItemDef &it = items.entries[i];
		const int wire_id = it.id - 100000;
		if (wire_id >= 0 && wire_id < 0x10000) {
			g_item_names[wire_id] = it.display_name;
			// §5.10b: the engine reads ItemDef+356 to dispatch the per-entity
			// network-serialize callback; that field is seeded from one of the
			// 4 *_function class-tag directives at items.def load time. We
			// haven't IDA-witnessed which directive specifically — but the
			// player has `ai_function plyr` (matching the documented `plyr →
			// SerializePlayerState` callback), so ai_function is the primary
			// signal. Fall back to move_function for items that omit it.
			EntityClass cls = class_from_tag(it.ai_function);
			if (cls == EntityClass::Unknown)
				cls = class_from_tag(it.move_function);
			if (cls != EntityClass::Unknown)
				g_item_class[uint16_t(wire_id)] = cls;
		}
	}
	const size_t loaded = items.count;
	def_free_items(&items);
	return loaded;
}

void print_pool_spawn_record(int index, const PoolSpawnRecord &r) {
	std::printf("        record %d flags=0x%04x slot=%s type=%s "
	            "name=%-12s pos=(%.1f, %.1f, %.1f)",
	            index, r.spawn_flags, handle_str(r.slot_id).c_str(),
	            type_str(r.item_type_id).c_str(),
	            ("\"" + r.entity_name + "\"").c_str(),
	            fp16(r.pos_x), fp16(r.pos_y), fp16(r.pos_z));
	if (r.spawn_flags & 0x0020) std::printf(" entity36=0x%08x", r.entity_flags);
	if (r.spawn_flags & 0x0001) std::printf(" velX");
	if (r.spawn_flags & 0x0002) std::printf(" velY");
	if (r.spawn_flags & 0x0004) std::printf(" velZ");
	if (r.spawn_flags & 0x0008) std::printf(" sectionMask");
	if (r.spawn_flags & 0x0010) std::printf(" team=0x%02x", r.team_byte);
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
	std::printf(" bone=0x%02x", r.bone_byte);
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
	std::printf("        slot %u type=%s flags=0x%02x "
	            "pos=(%.1f, %.1f, %.1f)",
	            unsigned(slot_idx), type_str(r.item_type_id).c_str(),
	            r.flags_byte, fp16(r.pos_x), fp16(r.pos_y), fp16(r.pos_z));
	if (r.flags_byte & 0x01) std::printf(" movement=0x%08x", r.movement_val);
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

void print_organic_record(int index, const OrganicSpawnRecord &r) {
	if (!r.has_body) {
		std::printf("        record %d slot=%s (empty spawn)\n", index,
		            handle_str(r.slot_id).c_str());
		return;
	}
	std::printf("        record %d slot=%s type=%s name=%-12s pos=(%.1f, %.1f, %.1f) "
	            "yaw=%.2f\xc2\xb0(0x%08x) team=0x%02x parent=%s\n",
	            index, handle_str(r.slot_id).c_str(),
	            type_str(r.item_type_id).c_str(),
	            ("\"" + r.entity_name + "\"").c_str(), fp16(r.pos_x), fp16(r.pos_y),
	            fp16(r.pos_z),
	            double(uint32_t(r.orientation)) / 4294967296.0 * 360.0,
	            uint32_t(r.orientation), r.team, handle_str(r.parent_handle).c_str());
}

void print_tag_0c(const std::vector<uint8_t> &body) {
	OrganicSpawnBatch batch;
	const bool clean = decode_organic_spawn_batch(body.data(), body.size(), batch);
	std::printf("        [0x0C] entityCount=%d (body %zu B%s%s)\n",
	            int(batch.entity_count), body.size(),
	            batch.sentinel_ended_early ? ", sentinel-ended" : "",
	            clean ? "" : ", DECODE INCOMPLETE");
	for (size_t i = 0; i < batch.records.size(); ++i)
		print_organic_record(int(i), batch.records[i]);
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

void print_tag_40(const std::vector<uint8_t> &body) {
	CaptureZoneOverlayBatch batch;
	const bool clean = decode_capture_zone_overlay(body.data(), body.size(), batch);
	std::printf("        [0x40] count=%u (body %zu B%s)\n",
	            unsigned(batch.count), body.size(),
	            clean ? "" : ", DECODE INCOMPLETE");
	for (const auto &e : batch.entries) {
		const char *col = e.icon_color == 0x0c ? "neutral" :
		                  e.icon_color == 0x09 ? "Red" :
		                  e.icon_color == 0x0a ? "Blue" : "?";
		std::printf("        overlay handle=%s param=0x%02x icon=0x%02x(%s) "
		            "flags=0x%02x%s source=0x%02x\n",
		            handle_str(e.handle).c_str(), e.param, e.icon_color, col,
		            e.flags, (e.flags & 0x10) ? " [capture-zone]" : "", e.source);
	}
}

// Tiny bounds-checked cursor for the 0x0A header walk. Mirrors the Cursor in
// libs/novaworld/src/ingame_decode.cpp; kept local here so nw_pp doesn't drag
// the libs' internal cursor type into a public header.
struct PpCursor {
	const uint8_t *p;
	const uint8_t *end;
	bool ok = true;
	uint8_t  u8()  { if (!ok || p + 1 > end) { ok = false; return 0; }
	                 return *p++; }
	uint16_t u16() { if (!ok || p + 2 > end) { ok = false; return 0; }
	                 uint16_t v = uint16_t(p[0]) | uint16_t(p[1]) << 8;
	                 p += 2; return v; }
	int16_t  i16() { return int16_t(u16()); }
	uint32_t u32() { if (!ok || p + 4 > end) { ok = false; return 0; }
	                 uint32_t v = uint32_t(p[0]) | uint32_t(p[1]) << 8 |
	                              uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
	                 p += 4; return v; }
	int32_t  i32() { return int32_t(u32()); }
	void     skip(size_t n) { if (!ok || p + n > end) { ok = false; return; }
	                          p += n; }
};

void print_player_compact_record(const PlayerCompactRecord &r) {
	std::printf("            player: vehBone=%u seat=%u vehHdl=%s "
	            "pos=(0x%04x,0x%04x,0x%04x) yaw=0x%02x pitch=0x%02x "
	            "anim=%u state=0x%02x weapAnim=%u prio=%u animDef=%u health=0x%02x\n",
	            unsigned(r.vehicle_bone), unsigned(r.seat_type),
	            handle_str(r.vehicle_handle).c_str(),
	            unsigned(r.pos_x_compressed), unsigned(r.pos_y_compressed),
	            unsigned(r.pos_z_compressed),
	            unsigned(r.yaw_byte), unsigned(r.pitch_byte),
	            unsigned(r.anim_slot_low), unsigned(r.state_flags),
	            unsigned(r.weapon_anim_state), unsigned(r.priority),
	            unsigned(r.anim_def_index), unsigned(r.health_class_byte));
}

void print_vehicle_compact_record(const VehicleCompactRecord &r) {
	std::printf("            vehicle: parent=%s pos=(0x%04x,0x%04x,0x%04x) "
	            "yawHigh=%d flags=0x%02x %s",
	            handle_str(r.parent_slot_handle).c_str(),
	            unsigned(r.pos_x_compressed), unsigned(r.pos_y_compressed),
	            unsigned(r.pos_z_compressed), int(r.yaw_high),
	            unsigned(r.flags_byte), r.is_mounted ? "MOUNTED" : "unmounted");
	if (r.is_mounted) {
		std::printf(" secHdg=0x%04x", unsigned(r.secondary_heading));
	} else {
		std::printf(" weap=(x=0x%04x y=0x%04x z=0x%04x hdg=0x%04x)",
		            unsigned(r.weapon_x_compressed),
		            unsigned(r.weapon_y_raw),
		            unsigned(r.weapon_z_compressed),
		            unsigned(r.weapon_heading_compressed));
	}
	std::printf(" finalHdg=0x%04x\n", unsigned(r.final_heading));
}

void print_infantry_compact_record(const InfantryCompactRecord &r) {
	std::printf("            infantry: seatBone=%u vehHdl=%s "
	            "pos=(0x%04x,0x%04x,0x%04x) yaw=0x%02x flags=0x%02x "
	            "pitch=0x%02x aimYaw=0x%02x anim=0x%02x\n",
	            unsigned(r.seat_bone_idx),
	            handle_str(r.vehicle_slot_handle).c_str(),
	            unsigned(r.pos_x_compressed), unsigned(r.pos_y_compressed),
	            unsigned(r.pos_z_compressed),
	            unsigned(r.yaw_byte), unsigned(r.flags_byte),
	            unsigned(r.pitch_byte), unsigned(r.aim_yaw_byte),
	            unsigned(r.anim_byte));
}

void print_player_extended_uplink(const PlayerExtendedUplink &r) {
	std::printf("            extended: vehHdl=%s pos=(%.1f, %.1f, %.1f) "
	            "hdg=%d pitch=%d animLow=0x%02x flagsXor=0x%02x "
	            "animDef=(%u,%u,%u) stat=(0x%02x,0x%02x)\n",
	            handle_str(r.vehicle_handle).c_str(),
	            fp16(r.pos_x), fp16(r.pos_y), fp16(r.pos_z),
	            int(r.heading), int(r.pitch),
	            unsigned(r.anim_slot_low), unsigned(r.flags_xor),
	            unsigned(r.anim_def_1), unsigned(r.anim_def_2),
	            unsigned(r.anim_def_3),
	            unsigned(r.stat_byte_0), unsigned(r.stat_byte_1));
	std::printf("            weapons: "
	            "(id=0x%04x ctr=%u) (id=0x%04x ctr=%u) "
	            "(id=0x%04x ctr=%u) (id=0x%04x ctr=%u)\n",
	            unsigned(r.weapon_id_0), unsigned(r.fire_counter_0),
	            unsigned(r.weapon_id_1), unsigned(r.fire_counter_1),
	            unsigned(r.weapon_id_2), unsigned(r.fire_counter_2),
	            unsigned(r.weapon_id_3), unsigned(r.fire_counter_3));
}

// C2S 0x0C — joiner per-frame uplink. Parses the 5-byte sub-header and
// dispatches on `sub_op`: 0x0A → extended (player only, §5.10 case 3/4),
// 0x0B → compact (would be S2C-shaped; not expected on C2S). Other classes
// reject modes 3/4 with return −1 (§5.10b line 886), so on C2S we should only
// ever see player+extended in practice.
void print_tag_0c_c2s(const std::vector<uint8_t> &body) {
	EntityPacketSubHeader hdr;
	size_t consumed = 0;
	if (!decode_entity_packet_sub_header(body.data(), body.size(), hdr,
	                                     consumed)) {
		std::printf("        [0x0C C2S] sub-header decode failed (len=%zu)\n",
		            body.size());
		return;
	}
	std::printf("        [0x0C C2S] hdl=%s type=%s sub_op=0x%02x(%s) (%zu B body)\n",
	            handle_str(hdr.handle).c_str(),
	            type_str(hdr.item_type_id).c_str(),
	            unsigned(hdr.sub_op),
	            hdr.sub_op == 0x0A ? "extended" :
	            hdr.sub_op == 0x0B ? "compact" : "?",
	            body.size() - consumed);
	const uint8_t *rest = body.data() + consumed;
	const size_t   rest_len = body.size() - consumed;
	if (hdr.sub_op == 0x0A) {
		PlayerExtendedUplink r;
		size_t used = 0;
		if (decode_player_extended_uplink(rest, rest_len, r, used)) {
			print_player_extended_uplink(r);
			if (used < rest_len)
				std::printf("            trailing %zu B: %s\n",
				            rest_len - used,
				            to_hex_sample(rest + used, rest_len - used).c_str());
		} else {
			std::printf("            extended decode failed (consumed=%zu of %zu): %s\n",
			            used, rest_len,
			            to_hex_sample(rest, rest_len).c_str());
		}
	} else if (hdr.sub_op == 0x0B) {
		PlayerCompactRecord r;
		size_t used = 0;
		if (decode_player_compact_record(rest, rest_len, r, used)) {
			print_player_compact_record(r);
		} else {
			std::printf("            compact decode failed (consumed=%zu of %zu): %s\n",
			            used, rest_len,
			            to_hex_sample(rest, rest_len).c_str());
		}
	} else if (rest_len) {
		std::printf("            unknown sub_op, raw: %s\n",
		            to_hex_sample(rest, rest_len).c_str());
	}
}

void print_tag_06_c2s(const std::vector<uint8_t> &body) {
	ClientFiredRound r;
	size_t used = 0;
	if (!decode_client_fired_round(body.data(), body.size(), r, used)) {
		std::printf("        [0x06 C2S] decode failed (consumed=%zu of %zu): %s\n",
		            used, body.size(),
		            to_hex_sample(body.data(), body.size()).c_str());
		return;
	}
	std::printf("        [0x06 C2S] tick=%u shooter=%s flags=0x%02x adm=%u "
	            "pos=(%.1f, %.1f, %.1f) dir=(%.4f, %.4f) target=%s hit_part=%u "
	            "extras=(0x%02x,0x%02x,0x%02x) muzzle=(off=0x%04x x=0x%04x y=0x%04x z=0x%04x w=0x%04x)\n",
	            r.current_tick, handle_str(r.shooter_handle).c_str(),
	            unsigned(r.fire_flags), unsigned(r.adm_index),
	            fp16(r.pos_x), fp16(r.pos_y), fp16(r.pos_z),
	            fp16(r.dir_x), fp16(r.dir_y),
	            handle_str(r.target_handle).c_str(), unsigned(r.hit_part),
	            unsigned(r.extra_byte1), unsigned(r.extra_byte2),
	            unsigned(r.misc_byte),
	            unsigned(r.base_offset), unsigned(r.offset_x),
	            unsigned(r.offset_y), unsigned(r.offset_z),
	            unsigned(r.offset_w));
}

void print_tag_21_c2s(const std::vector<uint8_t> &body) {
	ClientChecksumReply r;
	size_t used = 0;
	if (!decode_client_checksum_reply(body.data(), body.size(), r, used)) {
		std::printf("        [0x21 C2S] decode failed (need 5 B got %zu)\n",
		            body.size());
		return;
	}
	std::printf("        [0x21 C2S] player=%u expected_crc=0x%08x",
	            unsigned(r.player_index), r.expected_crc);
	if (body.size() > used) {
		std::printf(" trailing %zu B: %s",
		            body.size() - used,
		            to_hex_sample(body.data() + used, body.size() - used).c_str());
	}
	std::printf("\n");
}

void print_weapon_hit_record(const WeaponHitRecord &r) {
	std::printf("            weapon-hit: flags=0x%02x adm=%u sub=%u target=%s "
	            "pos=(0x%04x,0x%04x,0x%04x) yaw=0x%04x pitch=0x%04x dmgExtra=0x%04x",
	            unsigned(r.flags), unsigned(r.adm_index),
	            unsigned(r.hit_subtype), handle_str(r.target_handle).c_str(),
	            unsigned(r.pos_x_compressed), unsigned(r.pos_y_compressed),
	            unsigned(r.pos_z_compressed),
	            unsigned(r.yaw_bam_high), unsigned(r.pitch_bam_high),
	            unsigned(r.damage_extra_raw));
	if (r.has_parent_byte())   std::printf(" parent=0x%02x", unsigned(r.parent_byte));
	if (r.has_weapon_handle()) std::printf(" weap=%s", handle_str(r.weapon_handle).c_str());
	std::printf("\n");
}

// Walk a S2C 0x0A body: fixed header per §5.9 plus trailing event loop.
// Event tags: 0=EOB, 1=per-entity compact record, 2=weapon-hit. Fails closed
// on any unknown tag — prints the offset and stops.
void print_tag_0a(const std::vector<uint8_t> &body) {
	PpCursor c{body.data(), body.data() + body.size(), true};

	// Fixed header per §5.9: 3× i32 refs (tick anchors), flags1, flags2 (low
	// 2 bits = sub-block selector, bit 3 = vehicle-passenger record gate),
	// variable sub-block (cases 0-3, all IDA-witnessed), then the 7 B fixed
	// tail (state_flag_byte u8 / mount u16 / health i16 / state_word i16)
	// and optional 6 B vehicle-passenger record.
	const int32_t ref0 = c.i32();
	const int32_t ref1 = c.i32();
	const int32_t ref2 = c.i32();
	const uint8_t flags1 = c.u8();
	const uint8_t flags2 = c.u8();
	const unsigned sub_idx = unsigned(flags2 & 0x03);

	std::printf("        [0x0A] refs=(0x%08x,0x%08x,0x%08x) flags1=0x%02x "
	            "flags2=0x%02x sub=%u\n",
	            uint32_t(ref0), uint32_t(ref1), uint32_t(ref2),
	            unsigned(flags1), unsigned(flags2), sub_idx);

	// All four sub-block widths IDA-witnessed in NapiNPClientMsg_0x00A
	// (2026-06-16 grill): case 0 = 11 B (6× u8 + u8 sentinel + i32),
	// case 1 = 6 B (4× u8 + i16), case 2 = 11 B (3× u16 + 5× u8 ENV),
	// case 3 = 16 B or 0 B gated by `g_GameType & 0x20000`. Sub=3's gate is
	// wire-invisible — skipping 0 B matches non-objective gametypes; on an
	// objective-bit-set capture, sub=3 frames will misalign and the walker
	// will fail closed on an unknown event tag.
	switch (sub_idx) {
		case 0:
			c.skip(11);  // 6× u8 + u8 sentinel + i32 [orig: 0x430054..0x43012E]
			break;
		case 1:
			c.skip(6);   // 4× u8 + i16 [orig: 0x430191..0x430210]
			break;
		case 2: {
			const uint16_t fog_dist     = c.u16();
			const uint16_t fog_accel    = c.u16();
			const uint16_t tod_fixed    = c.u16();
			const uint8_t  quake_ticks  = c.u8();
			const uint8_t  cloud_scroll = c.u8();
			const uint8_t  cloud_byte2  = c.u8();
			const uint8_t  overcast     = c.u8();
			const uint8_t  env_trail    = c.u8();
			std::printf("            env: fogDist=0x%04x fogAccel=0x%04x "
			            "todFixed=0x%04x quake=%u clouds=(0x%02x,0x%02x) "
			            "overcast=0x%02x trail=0x%02x\n",
			            unsigned(fog_dist), unsigned(fog_accel),
			            unsigned(tod_fixed), unsigned(quake_ticks),
			            unsigned(cloud_scroll), unsigned(cloud_byte2),
			            unsigned(overcast), unsigned(env_trail));
			break;
		}
		case 3:
			// Gated on `g_GameType & 0x20000`; default to skip 0 B (the
			// common case) and rely on the post-header event-loop scan
			// catching a stale alignment.
			break;
	}

	// §5.9 post-dispatch fixed tail — 7 B always, IDA-witnessed:
	//   state_flag_byte u8  [0x4303E5] bit 0→dword_B76484, bit 1→dword_B76480,
	//                                  bits 0/1→g_local_player_entity.pad7[12] bits 8/9
	//   mountHandle     u16 [0x430408] vehicle-mount handle (pool<<12|slot, 0xFFFF=none)
	//   health          i16 [0x430428] → g_local_player_entity->Health
	//   state_word      i16 [0x430442] → *(WORD*)g_local_player_entity->pad7
	// (The trailing i16 was previously read as two separate bytes — refuted
	// by the 0x430442 grill; bytes 6-7 are the high half of a single i16.)
	const uint8_t  state_flag_byte = c.u8();
	const uint16_t mount           = c.u16();
	const int16_t  health          = c.i16();
	const int16_t  state_word      = c.i16();
	if (!c.ok) {
		std::printf("            header: underrun in tail (state/mount/health/state_word)\n");
		return;
	}
	std::printf("            header: state=0x%02x mount=%s health=%d "
	            "state_word=0x%04x\n",
	            unsigned(state_flag_byte), handle_str(mount).c_str(),
	            int(health), unsigned(uint16_t(state_word)));

	// Conditional vehicle-passenger record [orig: 0x430459 —
	// `if ((flags2 & 0xF) != 8) goto skip`]. Only fires when sub-block was
	// case 0 AND bit 3 of flags2 is set (joiner is mounted as a passenger,
	// not driver). Wire layout: u16 passenger_handle [0x430474] (0xFFFF
	// early-skips seat_yaw/pitch), u16 seat_yaw [0x4304C3], u16 seat_pitch
	// [0x4304DC].
	if ((flags2 & 0xF) == 8) {
		const uint16_t passenger_handle = c.u16();
		if (!c.ok) {
			std::printf("            passenger: underrun before handle\n");
			return;
		}
		if (passenger_handle == 0xFFFF) {
			std::printf("            passenger: hdl=ffff (no seat yaw/pitch)\n");
		} else {
			const uint16_t seat_yaw   = c.u16();
			const uint16_t seat_pitch = c.u16();
			if (!c.ok) {
				std::printf("            passenger: underrun in seat yaw/pitch\n");
				return;
			}
			std::printf("            passenger: hdl=%s seat_yaw=0x%04x "
			            "seat_pitch=0x%04x\n",
			            handle_str(passenger_handle).c_str(),
			            unsigned(seat_yaw), unsigned(seat_pitch));
		}
	}

	// Event loop [orig: 0x4306A1..0x4307A2]. Tags hard-capped at {0,1,2} —
	// `cmp eax,2 / jg` at 0x4306DA treats tag ≥3 as silent terminator (same
	// exit as tag==0). For tag==1 the wire order is `[u8 tag][u16 handle]
	// [u16 typeId]<compact-record>` confirmed at 0x43070C / 0x43076B.
	int rec_idx = 0;
	while (c.ok && c.p < c.end) {
		const size_t tag_off = size_t(c.p - body.data());
		const uint8_t tag = c.u8();
		if (tag == 0) {
			std::printf("            [eob] %zu B leftover\n",
			            size_t(c.end - c.p));
			return;
		}
		if (tag == 2) {
			// §5.9.1 weapon-hit. Variable length 17-20 B by flags gate.
			// Event loop continues past hits (not a terminator).
			const size_t avail = size_t(c.end - c.p);
			WeaponHitRecord r;
			size_t consumed = 0;
			if (decode_weapon_hit_record(c.p, avail, r, consumed)) {
				std::printf("            tag=0x02 weapon-hit @+%zu (%zu B)\n",
				            tag_off, consumed);
				print_weapon_hit_record(r);
				c.p += consumed;
				continue;
			}
			std::printf("            tag=0x02 weapon-hit @+%zu DECODE FAILED "
			            "(consumed=%zu, avail=%zu) — halting, %s\n",
			            tag_off, consumed, avail,
			            to_hex_sample(c.p, avail).c_str());
			return;
		}
		if (tag == 1) {
			const uint16_t handle  = c.u16();
			const uint16_t type_id = c.u16();
			if (!c.ok) {
				std::printf("            tag=0x01 @+%zu underrun in record "
				            "header\n", tag_off);
				return;
			}
			auto it = g_item_class.find(type_id);
			const EntityClass cls = (it == g_item_class.end())
			                         ? EntityClass::Unknown : it->second;
			const size_t avail = size_t(c.end - c.p);
			std::printf("            rec %d @+%zu hdl=%s type=%s class=%s\n",
			            rec_idx, tag_off, handle_str(handle).c_str(),
			            type_str(type_id).c_str(), class_name(cls));
			rec_idx++;
			size_t consumed = 0;
			bool ok = false;
			switch (cls) {
				case EntityClass::Player: {
					PlayerCompactRecord r;
					ok = decode_player_compact_record(c.p, avail, r, consumed);
					if (ok) print_player_compact_record(r);
					break;
				}
				case EntityClass::Vehicle: {
					VehicleCompactRecord r;
					ok = decode_vehicle_compact_record(c.p, avail, r, consumed);
					if (ok) print_vehicle_compact_record(r);
					break;
				}
				case EntityClass::Infantry: {
					InfantryCompactRecord r;
					ok = decode_infantry_compact_record(c.p, avail, r, consumed);
					if (ok) print_infantry_compact_record(r);
					break;
				}
				case EntityClass::Guided:
				case EntityClass::Unknown:
				default:
					// Either guided (§5.15 deferred — variable-length delta
					// codec) or unknown class — both fail closed since we
					// don't know how many bytes to skip.
					std::printf("            (no fixed decoder, %zu B remain) "
					            "halting walker — %s\n", avail,
					            to_hex_sample(c.p, avail).c_str());
					return;
			}
			if (!ok) {
				std::printf("            DECODE FAILED (consumed=%zu, avail=%zu) — "
				            "halting walker\n", consumed, avail);
				return;
			}
			c.p += consumed;
			continue;
		}
		// Unknown event tag — fail closed.
		std::printf("            tag=0x%02x @+%zu UNKNOWN — halting walker, "
		            "%zu B remaining: %s\n", unsigned(tag), tag_off,
		            size_t(c.end - c.p),
		            to_hex_sample(c.p, size_t(c.end - c.p)).c_str());
		return;
	}
}

void print_payload(char dir, int frame, int tag,
                   const std::vector<uint8_t> &payload) {
	const char *label = tag_label(dir, tag);
	std::printf("[%c f=%-4d tag=0x%02x%s%s%s len=%zu]\n", dir, frame, tag,
	            label ? "[" : "", label ? label : "", label ? "]" : "",
	            payload.size());
	if (dir == 'S' && tag == 0x0A) print_tag_0a(payload);
	else if (dir == 'S' && tag == 0x0C) print_tag_0c(payload);
	else if (dir == 'S' && tag == 0x0D) print_tag_0d(payload);
	else if (dir == 'S' && tag == 0x20) print_tag_20(payload);
	else if (dir == 'S' && tag == 0x40) print_tag_40(payload);
	else if (dir == 'C' && tag == 0x0C) print_tag_0c_c2s(payload);
	else if (dir == 'C' && tag == 0x06) print_tag_06_c2s(payload);
	else if (dir == 'C' && tag == 0x21) print_tag_21_c2s(payload);
	else if (!payload.empty()) std::printf("        %s\n",
	                                       to_hex_sample(payload.data(),
	                                                     payload.size()).c_str());
}

// ---- /PROFILE .sph server-log mode -----------------------------------------

const char *serverlog_team_name(uint8_t team) {
	switch (team) {
	case 0: return "Neutral";
	case 1: return "Blue";
	case 2: return "Red";
	default: return "?";
	}
}

int run_server_log(const char *path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		std::fprintf(stderr, "FAILED to open %s\n", path);
		return 1;
	}
	std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
	                          std::istreambuf_iterator<char>());
	std::fprintf(stderr, "loaded %zu bytes from %s\n", data.size(), path);

	ServerLogDocument doc;
	const bool clean = decode_server_log(data.data(), data.size(), doc);

	std::printf("== /PROFILE .sph server-log ==\n");
	std::printf("mission=%s  version=%u  ended=%s  leftover=%zu (%s)\n",
	            doc.mission.c_str(), doc.version, doc.ended_clean ? "yes" : "NO",
	            doc.leftover_bytes, clean ? "clean" : "DECODE INCOMPLETE");

	std::printf("roster (%zu):\n", doc.roster.size());
	for (const auto &p : doc.roster)
		std::printf("  id=%u team=%u(%s) name=\"%s\"\n", p.net_id, p.team,
		            serverlog_team_name(p.team), p.name.c_str());

	std::printf("events (%zu):\n", doc.events.size());
	for (const auto &e : doc.events)
		std::printf("  @frame=%u %s id=%u\n", e.at_frame,
		            e.kind == ServerLogEventKind::Death ? "DEATH" : "DISCONNECT",
		            e.net_id);

	std::printf("frames (%zu):\n", doc.frames.size());
	if (doc.cdat_count)
		std::printf("  (CPSP entity-data blobs seen: %u)\n", doc.cdat_count);
	for (const auto &fr : doc.frames) {
		std::printf("  frame %u (%zu entities):\n", fr.frame_index,
		            fr.entities.size());
		for (const auto &e : fr.entities)
			std::printf("    id=%u pos=(%.3f, %.3f, %.3f) yaw=%.1f deg "
			            "(0x%08x) flags=0x%x veh=%u stat=%u\n",
			            e.net_id, serverlog_fp16(e.pos_x), serverlog_fp16(e.pos_y),
			            serverlog_fp16(e.pos_z), serverlog_bam32_deg(e.yaw_bam),
			            e.yaw_bam, e.flags, e.vehicle_flag, e.stat_byte);
	}
	return clean ? 0 : 2;
}

// ---- --replay-json export mode ---------------------------------------------
// Build the replay timeline (libs/novaworld) from the capture and emit a single
// self-contained JSON document the standalone 2D viewer (tools/net/
// replay_viewer.html) loads directly. Positions are world meters; heading is
// degrees. The viewer anchors uplink tracks to each entity's spawn for display.

std::string json_escape(const std::string &s) {
	std::string o;
	for (char c : s) {
		switch (c) {
		case '"': o += "\\\""; break;
		case '\\': o += "\\\\"; break;
		case '\n': o += "\\n"; break;
		case '\r': o += "\\r"; break;
		case '\t': o += "\\t"; break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				char b[8];
				std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned char>(c));
				o += b;
			} else {
				o += c;
			}
		}
	}
	return o;
}

// Coarse display category: prefer the §5.10b entity class (when items.def was
// loaded), else fall back to the pool taxonomy. Drives the viewer's shape/legend.
const char *category_for(uint16_t type_id, uint8_t pool) {
	auto it = g_item_class.find(type_id);
	if (it != g_item_class.end()) {
		switch (it->second) {
		case EntityClass::Player:   return "player";
		case EntityClass::Infantry: return "infantry";
		case EntityClass::Vehicle:  return "vehicle";
		case EntityClass::Guided:   return "guided";
		default: break;
		}
	}
	switch (pool) {
	case 0: return "organic";
	case 1: return "item";
	case 2: return "building";
	case 3: return "marker";
	default: return "unknown";
	}
}

std::string basename_of(const std::string &p) {
	const size_t s = p.find_last_of("/\\");
	return s == std::string::npos ? p : p.substr(s + 1);
}

void write_sample_json(std::ostream &o, const ReplaySample &s, bool with_src) {
	o << "{\"f\":" << s.frame_index << ",\"x\":" << fp16(s.x) << ",\"y\":"
	  << fp16(s.y) << ",\"z\":" << fp16(s.z) << ",\"h\":";
	if (s.has_heading) o << s.heading_deg; else o << "null";
	if (with_src) {
		const char *src =
		    s.source == ReplaySampleSource::ClientUplink ? "uplink"
		    : s.source == ReplaySampleSource::FrameUpdate ? "frameupdate"
		                                                  : "spawn";
		o << ",\"src\":\"" << src << "\"";
	}
	o << "}";
}

int run_replay_json(const std::vector<CaptureDatagram> &caps,
                    const std::string &capture_path, const char *out_path) {
	// Resolve a wire type_id to its §5.10b compact class so the S2C 0x0A
	// per-frame motion can be walked (needs items.def — pass --items).
	auto class_of = [](uint16_t t) -> EntityClass {
		auto it = g_item_class.find(t);
		return it == g_item_class.end() ? EntityClass::Unknown : it->second;
	};
	const ReplayTimeline tl =
	    build_replay_timeline(decode_capture_to_messages(caps), class_of);
	std::ofstream o(out_path);
	if (!o) {
		std::fprintf(stderr, "FAILED to open %s for write\n", out_path);
		return 1;
	}
	o.setf(std::ios::fixed);
	o.precision(3);
	o << "{\n  \"meta\": {\"capture\":\"" << json_escape(basename_of(capture_path))
	  << "\",\"first_frame\":" << tl.first_frame << ",\"last_frame\":"
	  << tl.last_frame << ",\"entity_count\":" << tl.entities.size()
	  << ",\"note\":\"positions=world meters; heading=degrees; uplink samples are "
	     "world+origin (viewer anchors them to spawn for display)\"},\n";
	// type_id -> display name (populated only when --items gave an items.def).
	{
		std::set<uint16_t> types;
		for (const auto &e : tl.entities) types.insert(e.type_id);
		o << "  \"items\": {";
		bool first = true;
		for (uint16_t t : types) {
			if (!first) o << ",";
			first = false;
			auto it = g_item_names.find(int(t));
			o << "\"" << t << "\":\""
			  << (it != g_item_names.end() ? json_escape(it->second) : "") << "\"";
		}
		o << "},\n";
	}
	o << "  \"entities\": [\n";
	for (size_t i = 0; i < tl.entities.size(); ++i) {
		const ReplayEntity &e = tl.entities[i];
		char tagbuf[8];
		std::snprintf(tagbuf, sizeof(tagbuf), "0x%02x",
		              static_cast<unsigned char>(e.spawn_tag));
		o << "    {\"handle\":" << e.handle << ",\"pool\":" << unsigned(e.pool)
		  << ",\"type_id\":" << e.type_id << ",\"name\":\"" << json_escape(e.name)
		  << "\",\"category\":\"" << category_for(e.type_id, e.pool)
		  << "\",\"team\":";
		if (e.team_known) o << e.team; else o << "null";
		o << ",\"net_id\":" << e.net_id << ",\"spawn_tag\":\"" << tagbuf
		  << "\",\"spawn\":";
		if (e.has_spawn) write_sample_json(o, e.spawn, false); else o << "null";
		o << ",\"track\":[";
		for (size_t j = 0; j < e.track.size(); ++j) {
			if (j) o << ",";
			write_sample_json(o, e.track[j], true);
		}
		o << "]}";
		if (i + 1 < tl.entities.size()) o << ",";
		o << "\n";
	}
	o << "  ]\n}\n";
	std::fprintf(stderr, "wrote %zu entities (frames %d..%d) to %s\n",
	             tl.entities.size(), tl.first_frame, tl.last_frame, out_path);
	return 0;
}

} // namespace

int main(int argc, char *argv[]) {
	const char *path = nullptr;
	const char *items_path = nullptr;
	const char *replay_json_out = nullptr;
	std::set<int> tag_filter;
	for (int i = 1; i < argc; ++i) {
		const char *a = argv[i];
		if (std::strcmp(a, "--items") == 0 && i + 1 < argc) {
			items_path = argv[++i];
		} else if (std::strcmp(a, "--replay-json") == 0 && i + 1 < argc) {
			replay_json_out = argv[++i];
		} else if (a[0] == '0' && (a[1] == 'x' || a[1] == 'X')) {
			tag_filter.insert(int(std::strtol(a, nullptr, 16)));
		} else if (!path) {
			path = a;
		}
	}
	if (!path) path = std::getenv("NW_INGAME_HEXCAP");
	if (!items_path) items_path = std::getenv("NW_PP_ITEMS");
	if (!path || !*path) {
		std::fprintf(stderr,
		             "usage: nw_pp <capture-path> [--items <items.def>] [0xNN ...]\n"
		             "       path is a .pcap / .pcapng (parsed natively)\n"
		             "       or a hexcap text file (one '<srcport> <frame> "
		             "<hex>' per line)\n"
		             "       NW_INGAME_HEXCAP env supplies a hexcap path\n"
		             "       --items / NW_PP_ITEMS gives a JO items.def "
		             "(plaintext or SCR-encrypted with the JO/DFX2 key)\n"
		             "       so type_ids in 0x0D/0x20 records show as names\n"
		             "       --replay-json <out> writes a replay-timeline JSON "
		             "for tools/net/replay_viewer.html instead of printing\n");
		return 1;
	}
	if (is_sph_path(path)) return run_server_log(path);

	if (items_path && *items_path) {
		const size_t n = load_items_def(items_path);
		std::fprintf(stderr, "loaded %zu item names from %s\n", n, items_path);
	}

	std::vector<Datagram> dgrams;
	if (is_pcap_path(path)) {
		std::vector<net::PcapDatagram> pkts;
		if (!net::read_pcap_udp_file(path, pkts)) {
			std::fprintf(stderr, "FAILED to read pcap %s\n", path);
			return 1;
		}
		for (auto &pk : pkts) {
			Datagram d;
			d.srcport = pk.srcport;
			d.frame = pk.frame_index;
			d.bytes = std::move(pk.payload);
			dgrams.push_back(std::move(d));
		}
	} else {
		std::ifstream file(path);
		if (!file) {
			std::fprintf(stderr, "FAILED to open %s\n", path);
			return 1;
		}
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
	}
	std::fprintf(stderr, "loaded %zu datagrams from %s\n", dgrams.size(), path);
	if (!tag_filter.empty()) {
		std::fprintf(stderr, "tag filter:");
		for (int t : tag_filter) std::fprintf(stderr, " 0x%02x", t);
		std::fprintf(stderr, "\n");
	}

	// Drive the shared outer-decode pipeline, then print each reassembled
	// message (the per-tag dispatch lives in print_payload). Display tag keeps
	// the historical 0x1000 settings-update bit; the filter matches the low byte.
	std::vector<CaptureDatagram> caps;
	caps.reserve(dgrams.size());
	for (auto &d : dgrams) caps.push_back({d.frame, std::move(d.bytes)});

	// --replay-json: build the timeline and emit the viewer document, then exit.
	if (replay_json_out && *replay_json_out)
		return run_replay_json(caps, path, replay_json_out);

	for (const auto &m : decode_capture_to_messages(caps)) {
		if (!tag_filter.empty() && !tag_filter.count(m.tag & 0xFF)) continue;
		const int display_tag = int(m.tag) | (m.settings_update ? 0x1000 : 0);
		print_payload(m.dir, m.frame_index, display_tag, m.payload);
	}
	return 0;
}
