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
// Tag-specific decoders live in `libs/npwire/include/npwire/ingame_decode.h`
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
#include <npwire/ingame_decode.h>
#include <npwire/ingame_message_catalog.h>
#include <npwire/ingame_message_id.h>
#include <npwire/serverlog_decode.h>
#include <npwire/protocol_message.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>
#include <npwire/wire_capture.h>
#include <scr/scr.h>

#include <pcapio/pcap_reader.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <io/strutil.h>

using namespace opennova;

namespace {

struct Datagram {
	int srcport = 0;
	int dstport = 0;
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

using opennova::strutil::ends_with_icase;

bool is_pcap_path(const std::string &p) {
	return ends_with_icase(p, ".pcap") || ends_with_icase(p, ".pcapng");
}

bool is_sph_path(const std::string &p) { return ends_with_icase(p, ".sph"); }

// pcap/pcapng reading is shared with the test suite: apps/common/pcap_reader.h
// The outer-decode pipeline (envelope -> NWU -> SCRK -> 0x43/0x83 -> reassembly)
// is shared too: libs/npwire/wire_capture.h decode_capture_to_messages.

std::string to_hex_sample(const uint8_t *p, size_t n, size_t cap = 48) {
	// NW_PP_HEXCAP_MAX overrides the per-dump byte cap (witness sessions need whole payloads).
	if (const char *env = std::getenv("NW_PP_HEXCAP_MAX"); env != nullptr && env[0] != '\0') {
		const long v = std::strtol(env, nullptr, 10);
		if (v > 0) cap = static_cast<size_t>(v);
	}
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

// Tag labels come from the shared in-game message catalog
// (libs/npwire/.../ingame_message_catalog.h) — the single source of truth
// shared with the nw_message_coverage CI gate, so names can't drift between the
// printer and the coverage test. nullptr (uncatalogued) renders as a bare hex tag.
const char *tag_label(char dir, int tag) {
	return ingame_message_name(dir, uint8_t(tag));
}

// ItemDef.id → display_name resolution. Populated when --items <path> is
// given on the CLI. Resolves type_ids in 0x0D / 0x20 record dumps so the
// reader sees "type=0x04bd [d_5ton truck]" instead of just a hex id.
std::unordered_map<int, std::string> g_item_names;

// Per-item §5.10b dispatch class — selects which compact decoder runs on a
// tag==1 record inside S2C 0x0A's trailing event loop. The EntityClass enum and
// class_from_tag() now live in libs/npwire/ingame_decode.h (shared with the
// decode_frame_update walker). This map is the wire_id → class table, populated
// from items.def in load_items_def; an unmapped id leaves the walker unable to
// size a record (fail closed). Keyed by wire_id (items.def id − 100000).
std::unordered_map<uint16_t, EntityClass> g_item_class;

// The session game type (g_GameType @ 0x24D2128), learned from the S2C 0x7B
// `extra` field (the host puts g_GameType there). Two in-match sub-bodies are
// gated on it but the gate is NOT on their own wire, so the printer threads it
// into the gated decoders: 0x0F waypoint records ((g & 0xFFFDFFFF)==0x10020) and
// 0x0A objective sub-block 3 (g & 0x20000). 0 until the first 0x7B is seen.
uint32_t g_game_type = 0;
inline bool gt_is_waypoint() { return (g_game_type & 0xFFFDFFFFu) == 0x10020u; }
inline bool gt_is_objective() { return (g_game_type & 0x20000u) != 0; }

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
		std::printf(" zone=%u rank=%u radius=%u", r.zone_number_rank & 0x1F,
		            r.zone_number_rank >> 5, r.zone_radius);
	else if (r.spawn_flags & 0x8000)
		std::printf(" zoneRadius=%u", r.zone_radius);
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
	            "yaw=%.2f\xc2\xb0(0x%08x) team=0x%02x parent=%s eFlags(+0x78)=0x%08x "
	            "miniFlags(+36)=0x%04x%s net=0x%04x anim_slot=%u(+0x374) player_class=%u(+0x294) ai_state=%u\n",
	            index, handle_str(r.slot_id).c_str(),
	            type_str(r.item_type_id).c_str(),
	            ("\"" + r.entity_name + "\"").c_str(), fp16(r.pos_x), fp16(r.pos_y),
	            fp16(r.pos_z),
	            double(uint32_t(r.orientation)) / 4294967296.0 * 360.0,
	            uint32_t(r.orientation), r.team, handle_str(r.parent_handle).c_str(),
	            r.entity_flags, r.minimap_flags,
	            (r.minimap_flags & 0x100) ? " [LOCAL0x100]" : "", r.net_id,
	            unsigned(r.anim_slot), unsigned(r.player_class), unsigned(r.ai_state));
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

// S2C 0x18 FULL-ENTITY-SPAWN (§5.46) — the reply to a C2S 0x0F entity-info query;
// the client destroys + fully rebuilds the entity from this record.
void print_tag_18(const std::vector<uint8_t> &body) {
	FullEntitySpawnRecord r;
	const bool clean = decode_full_entity_spawn(body.data(), body.size(), r);
	std::printf("        [0x18] slot=%s type=%s defType=%u name=%s team=0x%02x "
	            "flags(+36)=0x%04x owner(+0x78)=0x%08x pos=(%.1f, %.1f, %.1f) yawHi=0x%04x "
	            "net=0x%04x player_class=%u anim_slot=%u links=(%s,%s,%s) seatMask=0x%02x%s\n",
	            handle_str(r.slot_id).c_str(), type_str(r.item_type_id).c_str(),
	            unsigned(r.item_type), ("\"" + r.entity_name + "\"").c_str(), r.team,
	            r.minimap_flags, r.entity_flags, fp16(r.pos_x), fp16(r.pos_y), fp16(r.pos_z),
	            r.heading_hi, r.net_id, unsigned(r.player_class), unsigned(r.anim_slot),
	            handle_str(r.parent_vehicle_handle).c_str(),
	            handle_str(r.ground_entity_handle).c_str(),
	            handle_str(r.parent_entity_handle).c_str(), r.seat_mask,
	            clean ? "" : " DECODE INCOMPLETE");
}

// S2C 0x58 SESSION-STATUS (§5.48) — server/mission names + up-time + scoring rules.
void print_tag_58(const std::vector<uint8_t> &body) {
	SessionStatusBlock s;
	const bool clean = decode_session_status(body.data(), body.size(), s);
	std::printf("        [0x58] server=\"%s\" mission=\"%s\" bytes=(%u,%u,%u) uptime=%ums",
	            s.server_name.c_str(), s.mission_name.c_str(),
	            s.byte0, s.byte1, s.byte2, s.uptime_ms);
	int nonzero = 0;
	for (int i = 0; i < 39; ++i) if (s.stat_values[i] != 0) ++nonzero;
	std::printf(" kvCount=%u statVars(nonzero)=%d:", s.kv_count, nonzero);
	for (int i = 0; i < 39; ++i)
		if (s.stat_values[i] != 0) std::printf(" [%02d]=%d", i, s.stat_values[i]);
	for (const SessionStatusKV &kv : s.kv) std::printf(" kv%u=%u", kv.key, kv.value);
	if (s.trailing_bytes > 0) std::printf(" +%zuB unread tail", s.trailing_bytes);
	std::printf("%s\n", clean ? "" : " DECODE INCOMPLETE");
}

// S2C 0x6F ZONE-TIMER VALUE (§5.49) — capture/takeover HUD meter, seconds on the wire.
void print_tag_6f(const std::vector<uint8_t> &body) {
	ZoneTimerValue z;
	size_t consumed = 0;
	const bool clean = decode_zone_timer_value(body.data(), body.size(), z, consumed)
	                   && consumed == body.size();
	std::printf("        [0x6f] zone=%s mode=%u value=%.2fs limit=%.2fs rate=%d "
	            "b544=0x%02x b545=0x%02x%s\n",
	            handle_str(z.zone_handle).c_str(), z.mode, fp16(z.value_s),
	            fp16(z.limit_s), z.rate, z.byte544, z.byte545,
	            clean ? "" : " DECODE INCOMPLETE");
}

// S2C 0x53 ZONE-TIMER WINDOW (§5.49).
void print_tag_53(const std::vector<uint8_t> &body) {
	ZoneTimerWindow z;
	size_t consumed = 0;
	const bool clean = decode_zone_timer_window(body.data(), body.size(), z, consumed)
	                   && consumed == body.size();
	std::printf("        [0x53] zone=%s modeA=%u modeB=%u window=[%us..%us] rate=%u%s\n",
	            handle_str(z.zone_handle).c_str(), z.mode_a, z.mode_b,
	            z.start_s, z.end_s, z.rate, clean ? "" : " DECODE INCOMPLETE");
}

// S2C 0x34 PLAY-SOUND (§5.50).
void print_tag_34(const std::vector<uint8_t> &body) {
	PlaySoundCommand s;
	const bool clean = decode_play_sound(body.data(), body.size(), s);
	std::printf("        [0x34] flag=%u sound=\"%s\"", s.flag, s.sound_name.c_str());
	if (s.has_pos)
		std::printf(" pos=(%d, %d, %d)", s.pos_x, s.pos_y, s.pos_z);
	std::printf("%s\n", clean ? "" : " DECODE INCOMPLETE");
}

// S2C 0x2C SESSION + MISSION-FILE NAMES (§5.51).
void print_tag_2c_s2c(const std::vector<uint8_t> &body) {
	MissionMapNames n;
	const bool clean = decode_mission_map_names(body.data(), body.size(), n);
	std::printf("        [0x2c] session=\"%s\" bms=\"%s\"%s\n",
	            n.session_name.c_str(), n.map_file_name.c_str(),
	            clean ? "" : " DECODE INCOMPLETE");
}

// S2C 0x14 CHAT broadcast (§5.52).
void print_tag_14(const std::vector<uint8_t> &body) {
	ChatBroadcast m;
	const bool clean = decode_chat_broadcast(body.data(), body.size(), m);
	std::printf("        [0x14] slot=%u chan=%u text=\"%s\"%s\n",
	            m.sender_slot, m.channel, m.text.c_str(),
	            clean ? "" : " DECODE INCOMPLETE");
}

// C2S 0x0D CHAT uplink (§5.52).
void print_tag_0d_c2s(const std::vector<uint8_t> &body) {
	ChatUplink m;
	const bool clean = decode_chat_uplink(body.data(), body.size(), m);
	std::printf("        [C 0x0d] chan=%u text=\"%s\"%s\n",
	            m.channel, m.text.c_str(), clean ? "" : " DECODE INCOMPLETE");
}

// C2S 0x2F LOADOUT SUBMIT (§5.56) — spawn-menu accept: team/class + ADM slots.
void print_tag_2f_c2s(const std::vector<uint8_t> &body) {
	LoadoutSubmit l;
	const bool clean = decode_loadout_submit(body.data(), body.size(), l);
	std::printf("        [C 0x2f] team=%u class=%u weaponSlot=%u entries=%zu:",
	            l.team, l.player_class, l.weapon_slot_index, l.entries.size());
	for (const LoadoutSubmitEntry &e : l.entries)
		std::printf(" {adm=%u ammo=%u/%u var=%u}", e.adm_index, e.ammo_primary,
		            e.ammo_secondary, e.variant);
	std::printf("%s\n", clean ? "" : " DECODE INCOMPLETE");
}

// C2S 0x0F entity-info query (§5.46) — [u16 handle], the self-heal request.
void print_tag_0f_c2s(const std::vector<uint8_t> &body) {
	const uint16_t handle = body.size() >= 2
	        ? uint16_t(body[0] | (uint16_t(body[1]) << 8)) : 0;
	std::printf("        [C 0x0f] query=%s%s\n", handle_str(handle).c_str(),
	            body.size() == 2 ? "" : " (short body -> handle 0)");
}

// S2C 0x04 SESSION SLOT CONFIG (§5.53).
void print_tag_04(const std::vector<uint8_t> &body) {
	SessionSlotConfig s;
	const bool clean = decode_session_slot_config(body.data(), body.size(), s);
	std::printf("        [0x04] cfg=%u mySlot=%u maxPlayers=%u trailing=%u "
	            "skipped=(0x%08x,0x%08x,0x%08x,0x%08x,0x%08x)%s\n",
	            s.session_config, s.local_player_slot, s.max_players, s.trailing,
	            s.skipped[0], s.skipped[1], s.skipped[2], s.skipped[3], s.skipped4,
	            clean ? "" : " DECODE INCOMPLETE");
}

// S2C 0x08 SESSION CONFIG (§5.54).
void print_tag_08_s2c(const std::vector<uint8_t> &body) {
	SessionConfig s;
	const bool clean = decode_session_config(body.data(), body.size(), s);
	std::printf("        [0x08] gameType=%d fields=(", s.fields[3]);
	for (int i = 0; i < 10; ++i)
		std::printf("%s%d", i ? "," : "", s.fields[i]);
	std::printf(") bytes=(");
	for (int i = 0; i < 7; ++i)
		std::printf("%s%u", i ? "," : "", s.bytes[i]);
	std::printf(") flags=0x%08x%s\n", s.bitflags, clean ? "" : " DECODE INCOMPLETE");
}

// S2C 0x02 JOIN POSITION-ACK + PADDING PROBE (§5.55).
void print_tag_02_s2c(const std::vector<uint8_t> &body) {
	JoinPaddingProbe p;
	const bool clean = decode_join_padding_probe(body.data(), body.size(), p);
	std::printf("        [0x02] pos=(%.1f, %.1f) paddingLen=%u filler=%zuB%s\n",
	            fp16(p.pos_x), fp16(p.pos_y), p.padding_len, p.filler_bytes,
	            clean ? "" : " DECODE INCOMPLETE");
}

// S2C 0x19 — [i32] -> dword_A82360 sync var.
void print_tag_19(const std::vector<uint8_t> &body) {
	uint32_t v = 0;
	size_t consumed = 0;
	const bool clean = decode_u32_scalar(body.data(), body.size(), v, consumed)
	                   && consumed == body.size();
	std::printf("        [0x19] value=0x%08x%s\n", v, clean ? "" : " DECODE INCOMPLETE");
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

void print_static_entity_record(uint16_t slot, const StaticEntityRecord &r) {
	const uint16_t handle = uint16_t((2u << 12) | (slot & 0x0FFF));
	if (r.is_empty_slot) {
		std::printf("        slot %s (empty)\n", handle_str(handle).c_str());
		return;
	}
	std::printf("        slot %s type=%s flags=0x%03x pos=(%.1f, %.1f, %.1f)",
	            handle_str(handle).c_str(), type_str(r.item_type_id).c_str(),
	            unsigned(r.field_flags), fp16(r.pos_x), fp16(r.pos_y), fp16(r.pos_z));
	if (r.field_flags & 0x0008) std::printf(" sect=0x%08x", unsigned(r.section_mask));
	if (r.field_flags & 0x0010) std::printf(" team=0x%02x", r.team_byte);
	// The D-NET-147 building/armory fields (entity+36 Flags / +533 / +532 / +624 / +350).
	if (r.field_flags & 0x0020) std::printf(" eflags=0x%08x", unsigned(r.entity_flags));
	if (r.field_flags & 0x0040) std::printf(" refNum=0x%02x", r.bone_a);
	if (r.field_flags & 0x0080) std::printf(" subType=0x%02x", r.bone_b);
	if (r.field_flags & 0x0100) std::printf(" score=0x%02x", r.score_flag);
	std::printf(" ammo=0x%02x weap=0x%02x", r.ammo_count, r.weapon_byte);
	if (r.weapon_byte != 0 || (r.field_flags & 0x0200))
		std::printf(" attach=%s", handle_str(r.attach_ref).c_str());
	std::printf("\n");
}

void print_tag_10(const std::vector<uint8_t> &body) {
	StaticEntityBatch batch;
	const bool clean = decode_static_entity_batch(body.data(), body.size(), batch);
	std::printf("        [0x10] startIdx=%u entityCount=%d (body %zu B%s)\n",
	            unsigned(batch.start_index), int(batch.entity_count),
	            body.size(), clean ? "" : ", DECODE INCOMPLETE");
	for (size_t i = 0; i < batch.records.size(); ++i)
		print_static_entity_record(uint16_t(batch.start_index + i), batch.records[i]);
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

void print_player_compact_record(const PlayerCompactRecord &r) {
	// carrier != none => pos is CARRIER-LOCAL compressed + yaw is carrier-relative
	// (mount if bone/seat set, else the standing-on ground entity; D-NET-151).
	std::printf("            player: vehBone=%u seat=%u carrier=%s "
	            "pos=(0x%04x,0x%04x,0x%04x) yaw=0x%02x pitch=0x%02x "
	            "input=%u state=0x%02x animState=%u animRatio=%u animDef=%u health=0x%02x\n",
	            unsigned(r.vehicle_bone), unsigned(r.seat_type),
	            handle_str(r.carrier_handle).c_str(),
	            unsigned(r.pos_x_compressed), unsigned(r.pos_y_compressed),
	            unsigned(r.pos_z_compressed),
	            unsigned(r.yaw_byte), unsigned(r.pitch_byte),
	            unsigned(r.move_input_byte), unsigned(r.state_flags),
	            unsigned(r.anim_state_id), unsigned(r.anim_channel_ratio),
	            unsigned(r.anim_def_index), unsigned(r.health_class_byte));
}

void print_vehicle_compact_record(const VehicleCompactRecord &r) {
	std::printf("            vehicle: parent=%s pos=(0x%04x,0x%04x,0x%04x) "
	            "eulerZ=%d flags=0x%02x %s",
	            handle_str(r.parent_slot_handle).c_str(),
	            unsigned(r.pos_x_compressed), unsigned(r.pos_y_compressed),
	            unsigned(r.pos_z_compressed), int(r.euler_z),
	            unsigned(r.flags_byte), r.is_dead_pose ? "DEAD-POSE" : "live");
	if (r.is_dead_pose) {
		std::printf(" euler=(x=%d y=%d)\n", int(r.euler_x), int(r.euler_y));
	} else {
		std::printf(" health=%u weap=(x=0x%04x aimY=0x%04x aimZ=0x%04x hdgBAM=%d)\n",
		            unsigned(r.health_word), unsigned(r.weapon_x),
		            unsigned(r.weapon_aim_y), unsigned(r.weapon_aim_z),
		            int(r.weapon_heading_bam));
	}
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
	// carrier != none => pos/hdg are CARRIER-LOCAL (ground entity — any pool; D-NET-151).
	std::printf("            extended: carrier=%s pos=(%.1f, %.1f, %.1f) "
	            "hdg=%d pitch=%d ac=0x%02x input=0x%02x flags=0x%02x "
	            "analog=(%u,%u,%u) adm=0x%02x fps=(%u,%u)\n",
	            handle_str(r.carrier_handle).c_str(),
	            fp16(r.pos_x), fp16(r.pos_y), fp16(r.pos_z),
	            int(r.heading), int(r.pitch),
	            unsigned(r.anticheat_flags),
	            unsigned(r.move_input_byte), unsigned(r.state_flags_byte),
	            unsigned(r.analog_x), unsigned(r.analog_y),
	            unsigned(r.analog_z),
	            unsigned(r.equipped_adm_index),
	            unsigned(r.stat_byte_0), unsigned(r.stat_byte_1));
	// The client's top-4 entity-INTEREST pairs (Server_BuildEntityPriorityListForPlayer,
	// op3 @0x4c1be9) — the old "weapons (id,ctr)" labels were a decode-era misread.
	std::printf("            prio: "
	            "(hdl=0x%04x score=%u) (hdl=0x%04x score=%u) "
	            "(hdl=0x%04x score=%u) (hdl=0x%04x score=%u)\n",
	            unsigned(r.priority_handle_0), unsigned(r.priority_score_0),
	            unsigned(r.priority_handle_1), unsigned(r.priority_score_1),
	            unsigned(r.priority_handle_2), unsigned(r.priority_score_2),
	            unsigned(r.priority_handle_3), unsigned(r.priority_score_3));
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
	            hdr.sub_op == ENTITY_SUB_OP_EXTENDED ? "extended" :
	            hdr.sub_op == ENTITY_SUB_OP_COMPACT ? "compact" : "?",
	            body.size() - consumed);
	const uint8_t *rest = body.data() + consumed;
	const size_t   rest_len = body.size() - consumed;
	if (hdr.sub_op == ENTITY_SUB_OP_EXTENDED) {
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
	} else if (hdr.sub_op == ENTITY_SUB_OP_COMPACT) {
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
	            "pos=(%.1f, %.1f, %.1f) dir=(%.4f, %.4f) target=%s shot_seq=%u "
	            "extras=(0x%02x,0x%02x,0x%02x) "
	            "pose_delta=(x=0x%04x y=0x%04x z=0x%04x yaw=0x%04x pitch=0x%04x)\n",
	            r.current_tick, handle_str(r.shooter_handle).c_str(),
	            unsigned(r.fire_flags), unsigned(r.adm_index),
	            fp16(r.pos_x), fp16(r.pos_y), fp16(r.pos_z),
	            fp16(r.dir_x), fp16(r.dir_y),
	            handle_str(r.target_handle).c_str(), unsigned(r.hit_part),
	            unsigned(r.extra_byte1), unsigned(r.extra_byte2),
	            unsigned(r.misc_byte),
	            unsigned(r.delta_x), unsigned(r.delta_y),
	            unsigned(r.delta_z), unsigned(r.delta_yaw),
	            unsigned(r.delta_pitch));
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

void print_round_event_record(const RoundEventRecord &r) {
	std::printf("            round-event: flags=0x%02x adm=%u sub=%u shooter=%s "
	            "origin=(0x%04x,0x%04x,0x%04x) yaw=0x%04x pitch=0x%04x shotSeq=0x%04x",
	            unsigned(r.flags), unsigned(r.adm_index),
	            unsigned(r.subtype), handle_str(r.shooter_handle).c_str(),
	            unsigned(r.pos_x_compressed), unsigned(r.pos_y_compressed),
	            unsigned(r.pos_z_compressed),
	            unsigned(r.yaw_bam_high), unsigned(r.pitch_bam_high),
	            unsigned(r.shot_seq));
	if (r.has_slot_byte())     std::printf(" slot=0x%02x", unsigned(r.slot_byte));
	if (r.has_target_handle()) std::printf(" target=%s", handle_str(r.target_handle).c_str());
	std::printf("\n");
}

// Print a S2C 0x0A frame update. Thin renderer over the single libs walker
// (decode_frame_update) — the byte layout lives there, this just formats the
// resulting FrameUpdate (header anchor + flags, env snapshot, local-player tail,
// passenger record, per-entity compact records, weapon-hit records). On a short
// read decode_frame_update fills what it walked and flags it; we print that plus
// the halt point.
void print_tag_0a(const std::vector<uint8_t> &body) {
	auto class_of = [](uint16_t t) -> EntityClass {
		auto it = g_item_class.find(t);
		if (it != g_item_class.end()) return it->second;
		// Fallback for the universal player template (0x14B9) so the 0x0A trailer's
		// player compact records decode even without --items (the items.def class
		// map). Other types still need --items; an Unknown there halts the walker,
		// with the "pass --items" hint printed below — so a missing classifier never
		// masquerades as a malformed/short stream.
		if (t == 0x14B9) return EntityClass::Player;
		return EntityClass::Unknown;
	};
	FrameUpdate fu;
	const bool ok = decode_frame_update(body.data(), body.size(), class_of, fu,
	                                    gt_is_objective());

	std::printf("        [0x0A] refs=(0x%08x,0x%08x,0x%08x) flags1=0x%02x "
	            "flags2=0x%02x sub=%u%s\n",
	            uint32_t(fu.anchor_x), uint32_t(fu.anchor_y), uint32_t(fu.anchor_z),
	            unsigned(fu.flags1), unsigned(fu.flags2), unsigned(fu.sub_block),
	            ok ? "" : " (DECODE INCOMPLETE)");

	if (fu.weapon.present)
		std::printf("            wpn: preround=%u slots=(%u,%u,%u,%u,%u) reload=%s uniformMask=0x%08x\n",
		            unsigned(fu.weapon.preround_timer), unsigned(fu.weapon.slot_state360),
		            unsigned(fu.weapon.slot_state368), unsigned(fu.weapon.slot_state364),
		            unsigned(fu.weapon.slot_state356), unsigned(fu.weapon.slot_state460),
		            fu.weapon.reload_seconds == 0xFF ? "belt"
		                : (std::to_string(fu.weapon.reload_seconds) + "s").c_str(),
		            uint32_t(fu.weapon.uniform_team_mask));
	if (fu.timer.present)
		std::printf("            timer: state=(%u,%u,%u,%u) seconds=%d\n",
		            unsigned(fu.timer.state0), unsigned(fu.timer.state1),
		            unsigned(fu.timer.state2), unsigned(fu.timer.state3),
		            int(fu.timer.timer_seconds));
	if (fu.env.present)
		std::printf("            env: fogDist=0x%04x fogAccel=0x%04x todFixed=0x%04x "
		            "quake=%u clouds=(0x%02x,0x%02x) overcast=0x%02x param=0x%02x\n",
		            unsigned(fu.env.fog_dist), unsigned(fu.env.fog_accel),
		            unsigned(fu.env.tod_fixed), unsigned(fu.env.quake_ticks),
		            unsigned(fu.env.cloud_scroll), unsigned(fu.env.cloud_param2),
		            unsigned(fu.env.overcast), unsigned(fu.env.env_param));
	if (fu.objective.present)
		std::printf("            objective: %d %d %d %d\n",
		            fu.objective.state[0], fu.objective.state[1],
		            fu.objective.state[2], fu.objective.state[3]);

	std::printf("            header: state=0x%02x mount=%s health=%d state_word=0x%04x\n",
	            unsigned(fu.state_flag_byte), handle_str(fu.mount_handle).c_str(),
	            int(fu.health), unsigned(uint16_t(fu.state_word)));

	if (fu.passenger.present) {
		if (fu.passenger.has_seat)
			std::printf("            passenger: hdl=%s seat_yaw=0x%04x seat_pitch=0x%04x\n",
			            handle_str(fu.passenger.handle).c_str(),
			            unsigned(fu.passenger.seat_yaw), unsigned(fu.passenger.seat_pitch));
		else
			std::printf("            passenger: hdl=ffff (no seat yaw/pitch)\n");
	}

	for (size_t i = 0; i < fu.records.size(); ++i) {
		const FrameUpdateRecord &r = fu.records[i];
		std::printf("            rec %zu hdl=%s type=%s class=%s\n", i,
		            handle_str(r.handle).c_str(), type_str(r.type_id).c_str(),
		            class_name(r.cls));
		switch (r.cls) {
			case EntityClass::Player:   print_player_compact_record(r.player); break;
			case EntityClass::Vehicle:  print_vehicle_compact_record(r.vehicle); break;
			case EntityClass::Infantry: print_infantry_compact_record(r.infantry); break;
			default: break;
		}
	}
	for (const RoundEventRecord &re : fu.round_events) {
		std::printf("            tag=0x02 round-event\n");
		print_round_event_record(re);
	}
	if (!ok)
		std::printf("            (decode halted after %zu B of %zu%s)\n",
		            fu.consumed, body.size(),
		            g_item_class.empty()
		                    ? " — likely an unclassifiable trailer record; pass --items <ITEMS.def>"
		                    : "");
}

// S2C 0x1E game event — kill feed + objectives + zone control.
void print_tag_1e(const std::vector<uint8_t> &body) {
	GameEventRecord r;
	size_t consumed = 0;
	if (!decode_game_event(body.data(), body.size(), r, consumed)) {
		std::printf("        [0x1E] game-event decode failed (len=%zu): %s\n",
		            body.size(), to_hex_sample(body.data(), body.size()).c_str());
		return;
	}
	const GameEventKind k = game_event_kind(r.event_type);
	const char *kind = k == GameEventKind::Kill ? "KILL"
	                 : k == GameEventKind::Objective ? "OBJECTIVE" : "event";
	const char *key = game_event_strcnd_key(r.event_type);
	auto idx = [](uint8_t i) {
		return i == 0xFF ? std::string("none") : ("p0/s" + std::to_string(unsigned(i)));
	};
	std::printf("        [0x1E] %s type=%u attacker=%s victim=%s aux=%s pos=(%d, %d)%s%s\n",
	            kind, unsigned(r.event_type), idx(r.attacker_index).c_str(),
	            idx(r.victim_index).c_str(), idx(r.aux_index).c_str(),
	            int(r.pos_x), int(r.pos_y), key ? " key=" : "", key ? key : "");
}

// S2C 0x26 entity kill replication.
void print_tag_26(const std::vector<uint8_t> &body) {
	KillRecord r;
	size_t consumed = 0;
	if (!decode_kill_record(body.data(), body.size(), r, consumed)) {
		std::printf("        [0x26] kill decode failed (len=%zu)\n", body.size());
		return;
	}
	std::printf("        [0x26] KILL victim=%s attacker=%s\n",
	            handle_str(r.victim_slot).c_str(), handle_str(r.attacker).c_str());
}

// S2C 0x4E batch despawn/kill.
void print_tag_4e(const std::vector<uint8_t> &body) {
	BatchKillBatch b;
	const bool clean = decode_batch_kill(body.data(), body.size(), b);
	std::printf("        [0x4E] batch-despawn count=%u slots=%zu%s\n",
	            unsigned(b.count), b.slots.size(), clean ? "" : " (DECODE INCOMPLETE)");
	for (uint16_t s : b.slots)
		std::printf("            slot %s\n", handle_str(s).c_str());
}

void print_tag_16(const std::vector<uint8_t> &body) {
	PlayerList pl;
	const bool clean = decode_player_list(body.data(), body.size(), pl);
	std::printf("        [0x16] flags=0x%02x rows=%u teams=%u inGame=%u spect=%u%s\n",
	            pl.flags, pl.player_count, pl.team_count, pl.in_game_count,
	            pl.spectator_count, clean ? "" : " DECODE INCOMPLETE");
	for (const auto &r : pl.players)
		std::printf("        player slot=0x%02x ping=%u score=%u/%u flags=0x%02x (team=%u spect=%u)\n",
		            r.slot_id, r.ping, r.score1, r.score2, r.flags,
		            r.flags >> 1, r.flags & 1);
}

void print_tag_46(const std::vector<uint8_t> &body) {
	PlayerSync ps;
	const bool clean = decode_player_sync(body.data(), body.size(), ps);
	if (ps.removal) {
		std::printf("        [0x46] slot=0x%02x REMOVAL (bitmask=0x%04x)%s\n",
		            ps.slot_id, ps.field_bitmask, clean ? "" : " INCOMPLETE");
		return;
	}
	std::printf("        [0x46] slot=0x%02x bitmask=0x%04x entity=0x%04x team=%u "
	            "name=\"%s\"%s%s\n",
	            ps.slot_id, ps.field_bitmask, unsigned(ps.entity_slot_id), ps.team,
	            ps.name.c_str(), ps.queue_ack ? " +ack" : "",
	            clean ? "" : " INCOMPLETE");
}

// S2C 0x5A weapon-loadout. typeId is an AdmDef (weapon/action) index, not an
// items.def type — printed raw, not via type_str.
void print_tag_5a(const std::vector<uint8_t> &body) {
	WeaponLoadout wl;
	const bool clean = decode_weapon_loadout(body.data(), body.size(), wl);
	std::printf("        [0x5A] weapon-loadout avatarClass=%u slots=%zu%s\n",
	            unsigned(wl.avatar_class), wl.slots.size(),
	            clean ? "" : " (DECODE INCOMPLETE)");
	for (const auto &s : wl.slots)
		std::printf("            slot admIdx=%u ammo=(%u,%u,%u)\n",
		            unsigned(s.type_id), unsigned(s.ammo_primary),
		            unsigned(s.ammo_secondary), unsigned(s.ammo_alt));
}

// S2C 0x6E team/squad roster sync.
void print_tag_6e(const std::vector<uint8_t> &body) {
	RosterSync rs;
	const bool clean = decode_roster_sync(body.data(), body.size(), rs);
	std::printf("        [0x6E] roster teams=%u%s\n",
	            unsigned(rs.team_count), clean ? "" : " (DECODE INCOMPLETE)");
	for (const auto &t : rs.teams) {
		std::printf("            team ent=%s slotIdx=%u members=%u slotHdl=%s:",
		            handle_str(t.team_entity_handle).c_str(),
		            unsigned(t.team_slot_index), unsigned(t.member_count),
		            handle_str(t.team_slot_handle).c_str());
		for (uint16_t m : t.members) std::printf(" %s", handle_str(m).c_str());
		std::printf("\n");
	}
}

// S2C 0x7B full player/session info. Field roles witnessed from the landing
// globals + PunkBuster cvar map (NOT the Hex-Rays auto-comment) — `id` is the
// NovaWorld player/account ID, not a clan tag.
void print_tag_7b(const std::vector<uint8_t> &body) {
	FullPlayerInfo fi;
	const bool clean = decode_full_player_info(body.data(), body.size(), fi);
	// The 0x7B `extra` dword is the host's g_GameType (witnessed: probe3 Co-op =
	// 0x30020). Track it so the gametype-gated 0x0F / 0x0A sub-bodies decode.
	g_game_type = fi.extra;
	std::printf("        [0x7B] full-player-info name=\"%s\" id=\"%s\" server=\"%s\" "
	            "mission=\"%s\" map=\"%s\" extra=0x%08x%s\n",
	            fi.player_name.c_str(), fi.player_id.c_str(), fi.server_name.c_str(),
	            fi.mission_name.c_str(), fi.map_file.c_str(), fi.extra,
	            clean ? "" : " (DECODE INCOMPLETE)");
	if (!fi.motd.empty())      std::printf("            motd=\"%s\"\n", fi.motd.c_str());
	if (!fi.game_name.empty()) std::printf("            game=\"%s\"\n", fi.game_name.c_str());
}

// S2C 0x0F world-state-load. The 128-entry score table is summarized (non-zero
// count); the spawn pose + waypoint/team-name counts + team names are shown.
void print_tag_0f(const std::vector<uint8_t> &body) {
	WorldStateLoad ws;
	// Waypoint records ride the 0x0F wire only for a waypoint gametype (the host
	// gate (g_GameType & 0xFFFDFFFF)==0x10020, off-wire) — pass the hint learned
	// from the 0x7B `extra` field, else the records misparse as team-name data.
	const bool clean = decode_world_state_load(body.data(), body.size(), ws,
	                                           gt_is_waypoint());
	int nonzero = 0;
	for (int32_t s : ws.team_scores) if (s) ++nonzero;
	std::printf("        [0x0F] world-state tick=%u spawn=(%.1f, %.1f, %.1f) "
	            "yaw=%d pitch=%d roll=%d flags=0x%02x scores[%d nz] waypoints=%u "
	            "teamNames=%u%s\n",
	            ws.session_tick, fp16(ws.pos_x), fp16(ws.pos_y), fp16(ws.pos_z),
	            int(ws.yaw), int(ws.pitch), int(ws.roll), unsigned(ws.game_flags),
	            nonzero, unsigned(ws.waypoint_count), unsigned(ws.team_name_count),
	            clean ? "" : " (DECODE INCOMPLETE)");
	for (const auto &w : ws.waypoints)
		std::printf("            waypoint slot=%s nameId=%u (STRWPNAME%03u) pad=%u\n",
		            handle_str(uint16_t((3u << 12) | (w.slot_id & 0x0FFF))).c_str(),
		            unsigned(w.name_id), unsigned(w.name_id), unsigned(w.pad));
	for (const auto &n : ws.team_names)
		std::printf("            team-name \"%s\"\n", n.c_str());
}

// S2C 0x60 / 0x64 chunked file transfer (shared printer).
void print_tag_file_xfer(int tag, const std::vector<uint8_t> &body) {
	FileTransferChunk ft;
	if (!decode_file_transfer_chunk(body.data(), body.size(), ft)) {
		std::printf("        [0x%02X] file-transfer header decode failed (len=%zu)\n",
		            tag, body.size());
		return;
	}
	std::printf("        [0x%02X] file-transfer id=%u total=%u offset=%u chunk=%zu B %s "
	            "(re-request C2S 0x%02X if incomplete): %s\n",
	            tag, ft.transfer_id, ft.total_size, ft.chunk_offset, ft.chunk_size,
	            ft.is_final() ? "[FINAL]" : "[more]",
	            tag == s2c::FILE_TRANSFER_CHUNK ? c2s::FILE_CHUNK_REQUEST
	                                            : c2s::MISSION_CHUNK_REQUEST,
	            to_hex_sample(ft.chunk_data, ft.chunk_size).c_str());
}

// C2S 0x22 player-sync request.
void print_tag_22_c2s(const std::vector<uint8_t> &body) {
	BurstPlayerSyncRequest r;
	size_t used = 0;
	if (!decode_burst_player_sync_request(body.data(), body.size(), r, used)) {
		std::printf("        [0x22 C2S] decode failed (need 3 B got %zu)\n", body.size());
		return;
	}
	std::printf("        [0x22 C2S] player-sync-request slot=0x%02x fieldFlags=0x%04x\n",
	            unsigned(r.slot), unsigned(r.field_flags));
}

// C2S 0x23 visible-players request (empty).
void print_tag_23_c2s(const std::vector<uint8_t> &body) {
	size_t used = 0;
	const bool ok = decode_burst_visible_request(body.data(), body.size(), used);
	std::printf("        [0x23 C2S] visible-players-request (empty)%s\n",
	            ok ? "" : " UNEXPECTED PAYLOAD");
}

// C2S 0x28 weapon-loadout request.
void print_tag_28_c2s(const std::vector<uint8_t> &body) {
	BurstLoadoutRequest r;
	size_t used = 0;
	if (!decode_burst_loadout_request(body.data(), body.size(), r, used)) {
		std::printf("        [0x28 C2S] decode failed (need 10 B got %zu)\n", body.size());
		return;
	}
	std::printf("        [0x28 C2S] loadout-request filter=0x%08x flags=0x%08x extra=0x%04x\n",
	            r.loadout_filter, r.flags, unsigned(r.extra));
}

// C2S 0x29 team/spawn ack (client 0x51 apply @0x431c99 sends team_index+1; the server
// reads it as a g_team_change_entity_list index @0x514f7c — D-NET-148).
void print_tag_29_c2s(const std::vector<uint8_t> &body) {
	TeamSpawnAck r;
	size_t used = 0;
	if (!decode_team_spawn_ack(body.data(), body.size(), r, used)) {
		std::printf("        [0x29 C2S] decode failed (need 2 B got %zu)\n", body.size());
		return;
	}
	std::printf("        [0x29 C2S] team-spawn-ack teamChangeIndex=%u\n",
	            unsigned(r.team_change_index));
}

// C2S 0x25 weapon-reload request (§5.58) — same 4-B body as the S2C 0x49 relay.
void print_tag_25_c2s(const std::vector<uint8_t> &body) {
	WeaponReload r;
	size_t used = 0;
	if (!decode_weapon_reload(body.data(), body.size(), r, used)) {
		std::printf("        [0x25 C2S] weapon-reload decode failed (need 4 B got %zu)\n",
		            body.size());
		return;
	}
	std::printf("        [0x25 C2S] weapon-reload-request handle=%s slotCombo=%u\n",
	            handle_str(r.entity_handle).c_str(), unsigned(r.reload_param));
}

// C2S 0x4C client quality/state byte.
void print_tag_4c_c2s(const std::vector<uint8_t> &body) {
	BurstClientQuality r;
	size_t used = 0;
	if (!decode_burst_client_quality(body.data(), body.size(), r, used)) {
		std::printf("        [0x4C C2S] decode failed (need 1 B got %zu)\n", body.size());
		return;
	}
	std::printf("        [0x4C C2S] client-quality=%u\n", unsigned(r.value));
}

// S2C 0x57 RTT ping/pong echo (mirror of C2S 0x2C).
void print_tag_57(const std::vector<uint8_t> &body) {
	RttSample r;
	size_t used = 0;
	if (!decode_rtt_sample(body.data(), body.size(), r, used)) {
		std::printf("        [0x57] rtt-echo decode failed (need 5 B got %zu)\n", body.size());
		return;
	}
	std::printf("        [0x57] rtt-echo ts=0x%08x flag=%u (%s)\n",
	            r.timestamp, unsigned(r.echo_flag),
	            r.echo_flag ? "ping->C2S 0x2C" : "measure rtt");
}

// C2S 0x2C RTT ping/pong consumed (mirror of S2C 0x57).
void print_tag_2c_c2s(const std::vector<uint8_t> &body) {
	RttSample r;
	size_t used = 0;
	if (!decode_rtt_sample(body.data(), body.size(), r, used)) {
		std::printf("        [0x2C C2S] rtt-consumed decode failed (need 5 B got %zu)\n", body.size());
		return;
	}
	std::printf("        [0x2C C2S] rtt-consumed ts=0x%08x flag=%u (%s)\n",
	            r.timestamp, unsigned(r.echo_flag),
	            r.echo_flag ? "ping->S2C 0x57" : "measure rtt");
}

// S2C 0x68 loaded-model-page request -> reply C2S 0x3D. Older notes called
// this an entity-index list; the producer is the frozen renderer model cache.
void print_tag_68(const std::vector<uint8_t> &body) {
	uint32_t v = 0; size_t used = 0;
	if (!decode_u32_scalar(body.data(), body.size(), v, used)) {
		std::printf("        [0x68] loaded-model-page request decode failed (need 4 B got %zu)\n", body.size());
		return;
	}
	std::printf("        [0x68] loaded-model-page request startIdx=%u -> reply C2S 0x3D\n", v);
}

// S2C 0x43 time-sync ping -> reply C2S 0x08.
void print_tag_43(const std::vector<uint8_t> &body) {
	uint32_t v = 0; size_t used = 0;
	if (!decode_u32_scalar(body.data(), body.size(), v, used)) {
		std::printf("        [0x43] time-sync-ping decode failed (need 4 B got %zu)\n", body.size());
		return;
	}
	std::printf("        [0x43] time-sync-ping serverTs=0x%08x -> reply C2S 0x08\n", v);
}

// S2C 0x39 charattr CHARACTER-row CRC challenge -> reply C2S 0x1C.
void print_tag_39(const std::vector<uint8_t> &body) {
	uint32_t v = 0; size_t used = 0;
	if (!decode_u32_scalar(body.data(), body.size(), v, used)) {
		std::printf("        [0x39] charattr-crc-challenge decode failed (need 4 B got %zu)\n", body.size());
		return;
	}
	std::printf("        [0x39] charattr-crc-challenge seed=0x%08x -> reply C2S 0x1C\n", v);
}

// S2C 0x6B minimap overlay batch.
void print_tag_6b(const std::vector<uint8_t> &body) {
	MinimapOverlayBatch b;
	if (!decode_minimap_overlay_batch(body.data(), body.size(), b)) {
		std::printf("        [0x6B] minimap-overlay decode failed (len %zu)\n", body.size());
		return;
	}
	std::printf("        [0x6B] minimap-overlay count=%zu\n", b.entries.size());
	for (const auto &e : b.entries)
		std::printf("            blip handle=%s\n", handle_str(e.handle).c_str());
}

// S2C 0x49 weapon-reload notification.
void print_tag_49(const std::vector<uint8_t> &body) {
	WeaponReload r;
	size_t used = 0;
	if (!decode_weapon_reload(body.data(), body.size(), r, used)) {
		std::printf("        [0x49] weapon-reload decode failed (need 4 B got %zu)\n", body.size());
		return;
	}
	std::printf("        [0x49] weapon-reload handle=%s reloadParam=%u\n",
	            handle_str(r.entity_handle).c_str(), unsigned(r.reload_param));
}

// S2C 0x13 entity death (second death path).
void print_tag_13(const std::vector<uint8_t> &body) {
	EntityDeathRecord d;
	size_t used = 0;
	if (!decode_entity_death(body.data(), body.size(), d, used)) {
		std::printf("        [0x13] entity-death decode failed (need 4 B got %zu)\n", body.size());
		return;
	}
	std::printf("        [0x13] entity-death handle=%s killerSource=%d\n",
	            handle_str(d.entity_handle).c_str(), int(d.killer_source));
}

// S2C 0x30 entity-checksum request -> reply C2S 0x20.
void print_tag_30(const std::vector<uint8_t> &body) {
	EntityChecksumRequest r;
	size_t used = 0;
	if (!decode_entity_checksum_request(body.data(), body.size(), r, used)) {
		std::printf("        [0x30] entity-checksum-req decode failed (need 3 B got %zu)\n", body.size());
		return;
	}
	std::printf("        [0x30] entity-checksum-req entityId=0x%02x checksum=0x%04x -> reply C2S 0x20\n",
	            unsigned(r.entity_id), unsigned(r.checksum));
}

// S2C 0x31 loadout/ammo CRC request -> reply C2S 0x21.
void print_tag_31(const std::vector<uint8_t> &body) {
	LoadoutCrcRequest r;
	size_t used = 0;
	if (!decode_loadout_crc_request(body.data(), body.size(), r, used)) {
		std::printf("        [0x31] loadout-crc-req decode failed (need 3 B got %zu)\n", body.size());
		return;
	}
	std::printf("        [0x31] loadout-crc-req ammoIndex=0x%02x xorKey=0x%04x -> reply C2S 0x21\n",
	            unsigned(r.ammo_index), unsigned(r.xor_key));
}

// S2C 0x42 input/state-flags push.
void print_tag_42(const std::vector<uint8_t> &body) {
	uint16_t flags = 0; size_t used = 0;
	if (!decode_input_state_flags(body.data(), body.size(), flags, used)) {
		std::printf("        [0x42] input-state-flags decode failed (need 2 B got %zu)\n", body.size());
		return;
	}
	std::printf("        [0x42] input-state-flags=0x%04x\n", unsigned(flags));
}

// S2C 0x79 spectator-mode flag.
void print_tag_79(const std::vector<uint8_t> &body) {
	uint8_t flag = 0; size_t used = 0;
	if (!decode_spectator_flag(body.data(), body.size(), flag, used)) {
		std::printf("        [0x79] spectator-flag decode failed (need 1 B got %zu)\n", body.size());
		return;
	}
	std::printf("        [0x79] spectator-flag=%u\n", unsigned(flag));
}

// S2C 0x2A chat-history entry.
void print_tag_2a(const std::vector<uint8_t> &body) {
	ChatHistoryEntry e;
	size_t used = 0;
	if (!decode_chat_history_entry(body.data(), body.size(), e, used)) {
		std::printf("        [0x2A] chat-history decode failed (need 10 B got %zu)\n", body.size());
		return;
	}
	std::printf("        [0x2A] chat-history a=0x%08x b=0x%08x c=%d\n",
	            unsigned(e.field_a), unsigned(e.field_b), int(e.field_c));
}

// S2C 0x59 deployed-item / weapon-overlay spawn.
void print_tag_59(const std::vector<uint8_t> &body) {
	DeployedItemSpawn d;
	size_t used = 0;
	if (!decode_deployed_item_spawn(body.data(), body.size(), d, used)) {
		std::printf("        [0x59] deployed-item decode failed (need 32 B got %zu)\n", body.size());
		return;
	}
	std::printf("        [0x59] deployed-item slot=%s owner=%s type=%s (fr=%s/en=%s) parent=%s\n",
	            handle_str(d.slot_handle).c_str(), handle_str(d.owner_handle).c_str(),
	            type_str(d.item_id).c_str(), type_str(d.friendly_item_id).c_str(),
	            type_str(d.enemy_item_id).c_str(), handle_str(d.parent_handle).c_str());
	std::printf("            pos=(%.1f, %.1f, %.1f) ang=(0x%04x,0x%04x,0x%04x)\n",
	            fp16(d.pos_x), fp16(d.pos_y), fp16(d.pos_z),
	            unsigned(d.angle_x), unsigned(d.angle_y), unsigned(d.angle_z));
}

// S2C 0x45 terrain-tile load batch (paged; §5.37).
void print_tag_45(const std::vector<uint8_t> &body) {
	TerrainLoadBatch b;
	if (!decode_terrain_load_batch(body.data(), body.size(), b)) {
		std::printf("        [0x45] terrain-load decode failed (len %zu)\n", body.size());
		return;
	}
	if (b.has_header)
		std::printf("        [0x45] terrain-load HEADER magic=0x%08x tile_count=%u "
		            "tiles[0,%u) (%zu entries)\n",
		            unsigned(b.magic), unsigned(b.tile_count), unsigned(b.end_index),
		            b.tiles.size());
	else
		std::printf("        [0x45] terrain-load page tiles[%u,%u) (%zu entries)\n",
		            unsigned(b.start_index), unsigned(b.end_index), b.tiles.size());
}

// S2C 0x44 entity-routed sub-packet (sub-header decoded; body class-dependent).
void print_tag_44(const std::vector<uint8_t> &body) {
	EntityRoutedPacket p;
	if (!decode_entity_routed_packet(body.data(), body.size(), p)) {
		std::printf("        [0x44] entity-routed decode failed (need 5 B got %zu)\n", body.size());
		return;
	}
	std::printf("        [0x44] entity-routed field0=0x%04x netId=%d subtype=%u (class body %zu B)\n",
	            unsigned(p.field0), int(p.net_id), unsigned(p.subtype), p.body_size);
}

// Render one NWU NapiMessage tree (lobby KV statement) indented.
void print_napi_message(const NapiMessage &m, int depth) {
	std::string pad(8 + depth * 2, ' ');
	std::printf("%s<%s>\n", pad.c_str(), m.name.c_str());
	for (const auto &f : m.fields) {
		bool printable = !f.data.empty();
		for (size_t i = 0; i + 1 < f.data.size(); ++i) // allow trailing NUL
			if (f.data[i] < 0x20 || f.data[i] > 0x7e) { printable = false; break; }
		if (printable) {
			std::string v(f.data.begin(), f.data.end());
			while (!v.empty() && v.back() == '\0') v.pop_back();
			std::printf("%s  %s = \"%s\"\n", pad.c_str(), f.name.c_str(), v.c_str());
		} else {
			std::printf("%s  %s = ", pad.c_str(), f.name.c_str());
			for (uint8_t b : f.data) std::printf("%02x ", b);
			std::printf("(%zu B)\n", f.data.size());
		}
	}
	for (const auto &c : m.children) print_napi_message(c, depth + 1);
}

// S2C/C2S tag 0x00 — the NWU lobby key-value statement stream (ClientConnected,
// ServerVerifyResult, ClientPlayerEnterRequest/ServerPlayerEnterResult with
// ConnectionId, ClientHostPlayerAdded/PlayerNumber, ...). Only when the payload
// is a 0x02-framed container; otherwise (e.g. the in-match VERSIONCRCSTRING JOIN)
// fall back to the hex sample the default case prints.
bool print_tag_00_kv(const std::vector<uint8_t> &body) {
	if (body.empty() || body[0] != 0x02) return false;
	std::vector<NapiMessage> msgs;
	size_t consumed = 0;
	if (napi_stream_decode(body.data(), body.size(), msgs, &consumed) != 0 || msgs.empty())
		return false;
	for (const auto &m : msgs) print_napi_message(m, 0);
	return true;
}

// --- histogram mode (machine-readable coverage; --histogram) ----------------
//
// Tally per (dir, tag) message count + total inner-payload bytes, decoupled from
// the pretty-printer's per-tag layout so the golden-diff harness
// (scripts/net/diff_vs_golden.ps1) and CI parse a stable contract regardless of
// how the human renderer evolves. One line per (dir, tag):
//
//   HIST <dir> 0x<tag> count=<n> bytes=<total> name=<catalog-name-or-?>
//
// followed by a TOTAL line. Tags are the low byte (the 0x1000 settings-update
// display bit is masked off — a settings update is still the same wire tag).
struct HistCell { long count = 0; long bytes = 0; };
std::map<std::pair<char, int>, HistCell> g_hist;

void hist_tally(char dir, int tag, size_t payload_len) {
	HistCell &c = g_hist[{dir, tag & 0xFF}];
	c.count += 1;
	c.bytes += long(payload_len);
}

void hist_emit() {
	long total_msgs = 0, total_bytes = 0;
	for (const auto &kv : g_hist) {
		const char dir = kv.first.first;
		const int tag = kv.first.second;
		const char *name = tag_label(dir, tag);
		std::printf("HIST %c 0x%02x count=%ld bytes=%ld name=%s\n", dir, tag,
		            kv.second.count, kv.second.bytes, name ? name : "?");
		total_msgs += kv.second.count;
		total_bytes += kv.second.bytes;
	}
	std::printf("TOTAL msgs=%ld bytes=%ld tags=%zu\n", total_msgs, total_bytes,
	            g_hist.size());
}

// --coverage: join the histogram with the shared catalog's coverage class and
// rank the DECODE BACKLOG — every (dir,tag) the capture carries that has no
// structured decoder yet (PrinterOnly / Unhandled / uncatalogued), ordered by
// volume, so "what should we field-map next?" is one command, not a manual cross
// of the histogram against docs. The catalog (ingame_message_catalog.h) is the
// single source of truth shared with nw_message_coverage; this just reports it.
void coverage_emit() {
	const char *cov_name[] = {"Decoded", "PrinterOnly", "Unhandled"};
	// Backlog = seen tags whose coverage is not Decoded, sorted by count desc.
	std::vector<std::pair<long, std::string>> backlog;
	long covered = 0, partial = 0;
	for (const auto &kv : g_hist) {
		const char dir = kv.first.first;
		const uint8_t tag = uint8_t(kv.first.second);
		const MsgCatalogEntry *e = lookup_ingame_message(dir, tag);
		const bool decoded = e && e->coverage == MsgCoverage::Decoded;
		if (decoded) { covered += kv.second.count; continue; }
		partial += kv.second.count;
		const char *cls = e ? cov_name[int(e->coverage)] : "UNCATALOGUED";
		const char *name = e ? e->name : "?";
		char buf[160];
		std::snprintf(buf, sizeof(buf), "%c 0x%02x %-22s %-12s count=%ld%s%s", dir,
		              tag, name, cls, kv.second.count, e && e->note ? "  " : "",
		              e && e->note ? e->note : "");
		backlog.push_back({kv.second.count, buf});
	}
	std::sort(backlog.begin(), backlog.end(),
	          [](const auto &a, const auto &b) { return a.first > b.first; });
	std::printf("=== decode backlog (tags present with no structured decoder, by volume) ===\n");
	for (const auto &b : backlog) std::printf("BACKLOG %s\n", b.second.c_str());
	std::printf("COVERAGE decoded_msgs=%ld undecoded_msgs=%ld backlog_tags=%zu\n",
	            covered, partial, backlog.size());
}

bool g_hexdump_mode = false; // --hexdump: raw payload hex for EVERY message (byte-diff two captures)

void print_payload(char dir, int frame, int tag,
                   const std::vector<uint8_t> &payload, int session = 0) {
	const char *label = tag_label(dir, tag);
	std::printf("[%c f=%-4d s=%-5d tag=0x%02x%s%s%s len=%zu]\n", dir, frame, session, tag,
	            label ? "[" : "", label ? label : "", label ? "]" : "",
	            payload.size());
	if (g_hexdump_mode) {
		// One line per 32 payload bytes — decoded views can hide bytes; this never does.
		for (size_t off = 0; off < payload.size(); off += 32) {
			const size_t n = payload.size() - off < 32 ? payload.size() - off : 32;
			std::printf("        raw+%04zx %s\n", off, to_hex_sample(payload.data() + off, n, 32).c_str());
		}
		return; // raw view replaces the structured decode (keeps diffs purely byte-level)
	}
	if (dir == 'S' && tag == s2c::PER_FRAME_UPDATE) print_tag_0a(payload);
	else if (dir == 'S' && tag == s2c::ENTITY_SPAWN_BATCH) print_tag_0c(payload);
	else if (dir == 'S' && tag == s2c::POOL_SPAWN) print_tag_0d(payload);
	else if (dir == 'S' && tag == s2c::POOL3_SYNC) print_tag_20(payload);
	else if (dir == 'S' && tag == s2c::STATIC_ENTITY_BATCH) print_tag_10(payload);
	else if (dir == 'S' && tag == s2c::CAPTURE_ZONE_STATE) print_tag_40(payload);
	else if (dir == 'S' && tag == s2c::PLAYER_LIST) print_tag_16(payload);
	else if (dir == 'S' && tag == s2c::FULL_ENTITY_SPAWN) print_tag_18(payload);
	else if (dir == 'S' && tag == s2c::PLAYER_SYNC) print_tag_46(payload);
	else if (dir == 'S' && tag == s2c::GAME_EVENT) print_tag_1e(payload);
	else if (dir == 'S' && tag == s2c::KILL_SYNC) print_tag_26(payload);
	else if (dir == 'S' && tag == s2c::KILL_BY_SLOT) print_tag_4e(payload);
	else if (dir == 'S' && tag == s2c::WORLD_STATE_LOAD) print_tag_0f(payload);
	else if (dir == 'S' && tag == s2c::WEAPON_LOADOUT) print_tag_5a(payload);
	else if (dir == 'S' && tag == s2c::ROSTER_SYNC) print_tag_6e(payload);
	else if (dir == 'S' && tag == s2c::FULL_PLAYER_INFO) print_tag_7b(payload);
	else if (dir == 'S' && (tag == s2c::FILE_TRANSFER_CHUNK || tag == s2c::MISSION_DATA_CHUNK))
		print_tag_file_xfer(tag, payload);
	else if (dir == 'S' && tag == s2c::RTT_ECHO) print_tag_57(payload);
	else if (dir == 'S' && tag == s2c::LOADED_MODEL_PAGE_REQUEST) print_tag_68(payload);
	else if (dir == 'S' && tag == s2c::TIME_SYNC_PING) print_tag_43(payload);
	else if (dir == 'S' && tag == s2c::CHARATTR_CRC_CHALLENGE) print_tag_39(payload);
	else if (dir == 'S' && tag == s2c::MINIMAP_OVERLAY) print_tag_6b(payload);
	else if (dir == 'S' && tag == s2c::WEAPON_RELOAD) print_tag_49(payload);
	else if (dir == 'S' && tag == s2c::ENTITY_DEATH) print_tag_13(payload);
	else if (dir == 'S' && tag == s2c::ENTITY_CHECKSUM_REQ) print_tag_30(payload);
	else if (dir == 'S' && tag == s2c::LOADOUT_CRC_REQ) print_tag_31(payload);
	else if (dir == 'S' && tag == s2c::INPUT_STATE_FLAGS) print_tag_42(payload);
	else if (dir == 'S' && tag == s2c::SPECTATOR_FLAG) print_tag_79(payload);
	else if (dir == 'S' && tag == s2c::CHAT_HISTORY) print_tag_2a(payload);
	else if (dir == 'S' && tag == s2c::DEPLOYED_ITEM) print_tag_59(payload);
	else if (dir == 'S' && tag == s2c::TERRAIN_LOAD) print_tag_45(payload);
	else if (dir == 'S' && tag == s2c::ENTITY_ROUTED) print_tag_44(payload);
	else if (dir == 'S' && tag == s2c::SESSION_STATUS) print_tag_58(payload);
	else if (dir == 'S' && tag == s2c::ZONE_TIMER_VALUE) print_tag_6f(payload);
	else if (dir == 'S' && tag == s2c::ZONE_TIMER_WINDOW) print_tag_53(payload);
	else if (dir == 'S' && tag == s2c::PLAY_SOUND) print_tag_34(payload);
	else if (dir == 'S' && tag == s2c::MISSION_MAP_NAMES) print_tag_2c_s2c(payload);
	else if (dir == 'S' && tag == s2c::CHAT_BROADCAST) print_tag_14(payload);
	else if (dir == 'S' && tag == s2c::SESSION_SLOT_CONFIG) print_tag_04(payload);
	else if (dir == 'S' && tag == s2c::SESSION_CONFIG) print_tag_08_s2c(payload);
	else if (dir == 'S' && tag == s2c::JOIN_PADDING_PROBE) print_tag_02_s2c(payload);
	else if (dir == 'S' && tag == s2c::SPAWN_ACK_TIMESTAMP) print_tag_19(payload);
	else if (dir == 'C' && tag == c2s::CHAT_MESSAGE) print_tag_0d_c2s(payload);
	else if (dir == 'C' && tag == c2s::ENTITY_INFO_QUERY) print_tag_0f_c2s(payload);
	else if (dir == 'C' && tag == c2s::LOADOUT_SUBMIT) print_tag_2f_c2s(payload);
	else if (dir == 'C' && tag == c2s::RTT_CONSUMED) print_tag_2c_c2s(payload);
	else if (dir == 'C' && tag == c2s::ENTITY_UPLINK) print_tag_0c_c2s(payload);
	else if (dir == 'C' && tag == c2s::FIRED_ROUND) print_tag_06_c2s(payload);
	else if (dir == 'C' && tag == c2s::CHECKSUM_REPLY) print_tag_21_c2s(payload);
	else if (dir == 'C' && tag == c2s::PLAYER_SYNC_REQUEST) print_tag_22_c2s(payload);
	else if (dir == 'C' && tag == c2s::VISIBLE_PLAYERS_REQUEST) print_tag_23_c2s(payload);
	else if (dir == 'C' && tag == c2s::LOADOUT_REQUEST) print_tag_28_c2s(payload);
	else if (dir == 'C' && tag == c2s::TEAM_SPAWN_ACK) print_tag_29_c2s(payload);
	else if (dir == 'C' && tag == c2s::WEAPON_RELOAD_REQUEST) print_tag_25_c2s(payload);
	else if (dir == 'C' && tag == c2s::CLIENT_QUALITY) print_tag_4c_c2s(payload);
	else if (tag == 0x00 && print_tag_00_kv(payload)) { /* NWU KV rendered */ }
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

// --handshake: print the OUTER session handshake datagrams (0x41 ClientHello /
// 0x42 ClientAuth / 0x81 ServerHello / 0x82 ServerAuth) that the in-game stream
// pipeline consumes silently (wire_capture recovers SCRK from them and moves
// on). The 0x42 dump expands the CU chunks — the joiner's character/profile
// vars (CI0/CI1/TR/CTA/CTB/VCA/VCB ...) the host feeds into the player add
// [orig: NapiNetConfig_LoadFromConnTags @0x4c7260 <- NapiNPProtocol_HandleClientJoin
// @0x62b750 CU loop; client builder CNapiServerInfo_SerializeToSession @0x4c3650].
int run_handshake(const char *path) {
	long seen = 0, shown = 0;
	auto on_dg = [&](const net::PcapDatagram &pk) -> bool {
		++seen;
		std::vector<uint8_t> stripped(pk.payload.size());
		size_t out_len = 0;
		if (napi_envelope_decode(pk.payload.data(), pk.payload.size(), stripped.data(),
		                         stripped.size(), &out_len) != 0)
			return true;
		stripped.resize(out_len);
		if (stripped.empty()) return true;
		const uint8_t opcode = stripped[0];
		if (opcode != SESSION_OPCODE_CLIENT_HELLO && opcode != SESSION_OPCODE_CLIENT_AUTH && opcode != SESSION_OPCODE_SERVER_HELLO && opcode != SESSION_OPCODE_SERVER_AUTH) return true;
		std::vector<uint8_t> body(stripped.begin() + 1, stripped.end());
		// Outer NWU transform: decrypt-on-receive is nwu_encrypt (names swapped).
		if (!body.empty()) nwu_encrypt(body.data(), body.size(), SESSION_NWU_KEY);
		++shown;
		std::printf("[f=%d %d->%d op=0x%02x len=%zu]\n", pk.frame_index, pk.srcport,
		            pk.dstport, unsigned(opcode), body.size());
		if (opcode == SESSION_OPCODE_CLIENT_AUTH) {
			ClientAuth auth;
			if (!parse_client_auth(body.data(), body.size(), auth)) {
				std::printf("        ClientAuth: PARSE FAILED  raw=%s\n",
				            to_hex_sample(body.data(), body.size()).c_str());
				return true;
			}
			std::printf("        ClientAuth co=\"%s\" na=\"%s\" ci=%u hk=0x%08x ck=0x%08x "
			            "cu_chunks=%zu\n",
			            auth.co.c_str(), auth.na.c_str(), auth.ci, auth.hk, auth.ck,
			            auth.cu.size());
			// Each CU chunk: [type:1B][name + NUL][LE16 data_len][value + NUL] — the
			// wire shape of NapiNPChunk_Create @0x624720 (parse_client_cu_chunk).
			for (const auto &cu : auth.cu) {
				uint8_t ctype = 0;
				std::string name, value;
				if (parse_client_cu_chunk(cu.data(), cu.size(), ctype, name, value)) {
					std::printf("          CU type=%u %s = \"%s\"\n", unsigned(ctype),
					            name.c_str(), value.c_str());
				} else {
					std::printf("          CU (undecoded) raw=%s\n",
					            to_hex_sample(cu.data(), cu.size()).c_str());
				}
			}
		} else if (opcode == SESSION_OPCODE_CLIENT_HELLO) {
			ClientHello hello;
			if (parse_client_hello(body.data(), body.size(), hello))
				std::printf("        ClientHello co=\"%s\" pn=\"%s\"\n", hello.co.c_str(),
				            hello.pn.c_str());
		}
		return true;
	};
	if (!net::stream_pcap_udp_file(path, on_dg)) {
		std::fprintf(stderr, "FAILED to stream pcap %s\n", path);
		return 1;
	}
	std::fprintf(stderr, "scanned %ld datagrams, %ld handshake datagrams shown\n", seen, shown);
	return 0;
}

} // namespace

int main(int argc, char *argv[]) {
	const char *path = nullptr;
	const char *items_path = nullptr;
	std::set<int> tag_filter;
	bool stream_mode = false;
	bool handshake_mode = false;
	bool histogram_mode = false;
	bool coverage_mode = false;
	long max_frames = 0; // 0 = unlimited
	long skip_frames = 0;
	for (int i = 1; i < argc; ++i) {
		const char *a = argv[i];
		if (std::strcmp(a, "--items") == 0 && i + 1 < argc) {
			items_path = argv[++i];
		} else if (std::strcmp(a, "--histogram") == 0) {
			histogram_mode = true;
		} else if (std::strcmp(a, "--coverage") == 0) {
			coverage_mode = true;
			histogram_mode = true; // coverage joins the histogram tally with the catalog
		} else if (std::strcmp(a, "--stream") == 0) {
			stream_mode = true;
		} else if (std::strcmp(a, "--handshake") == 0) {
			handshake_mode = true;
		} else if (std::strcmp(a, "--hexdump") == 0) {
			g_hexdump_mode = true;
		} else if (std::strcmp(a, "--max-frames") == 0 && i + 1 < argc) {
			max_frames = std::strtol(argv[++i], nullptr, 10);
			stream_mode = true; // a frame budget only makes sense streaming
		} else if (std::strcmp(a, "--skip") == 0 && i + 1 < argc) {
			skip_frames = std::strtol(argv[++i], nullptr, 10);
			stream_mode = true;
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
		             "usage: nw_pp <capture-path> [--items <items.def>] [--stream] "
		             "[--histogram] [--max-frames N] [--skip N] [0xNN ...]\n"
		             "       path is a .pcap / .pcapng (parsed natively)\n"
		             "       --histogram emits one machine-readable 'HIST <dir> "
		             "0x<tag> count=.. bytes=.. name=..' line per (dir,tag) + a "
		             "TOTAL line (for scripts/net/diff_vs_golden.ps1 + CI)\n"
		             "       --coverage ranks the DECODE BACKLOG: tags present in "
		             "the capture with no structured decoder yet, by volume\n"
		             "       --stream decodes lazily (flat memory) for multi-GB "
		             "captures; --skip N starts after N datagrams; --max-frames N "
		             "stops after N (implies --stream)\n"
		             "       or a hexcap text file (one '<srcport> <frame> "
		             "<hex>' per line)\n"
		             "       NW_INGAME_HEXCAP env supplies a hexcap path\n"
		             "       --items / NW_PP_ITEMS gives a JO items.def "
		             "(plaintext or SCR-encrypted with the JO/DFX2 key)\n"
		             "       so type_ids in 0x0D/0x20 records show as names\n");
		return 1;
	}
	if (is_sph_path(path)) return run_server_log(path);
	if (handshake_mode) {
		if (!is_pcap_path(path)) {
			std::fprintf(stderr, "--handshake needs a pcap/pcapng input\n");
			return 1;
		}
		return run_handshake(path);
	}

	if (items_path && *items_path) {
		const size_t n = load_items_def(items_path);
		std::fprintf(stderr, "loaded %zu item names from %s\n", n, items_path);
	}

	// Streaming path: for multi-GB captures the whole-file load below OOMs. Feed
	// each datagram straight into a resumable CaptureDecoder and print messages as
	// they complete — flat memory regardless of file size (wire_capture.h). Only
	// the pcap reader differs; the per-message print is identical to the batch path.
	if (stream_mode && is_pcap_path(path)) {
		opennova::CaptureDecoder decoder;
		long seen = 0, printed_through = 0;
		bool stopped_early = false;
		auto on_dg = [&](const net::PcapDatagram &pk) -> bool {
			++seen;
			if (skip_frames > 0 && seen <= skip_frames) return true;
			opennova::CaptureDatagram cd;
			cd.frame_index = pk.frame_index;
			cd.src_port = pk.srcport;
			cd.dst_port = pk.dstport;
			cd.payload = pk.payload; // copy: reference only valid during the callback
			for (const auto &m : decoder.push(cd)) {
				if (!tag_filter.empty() && !tag_filter.count(m.tag & 0xFF)) continue;
				if (histogram_mode) { hist_tally(m.dir, m.tag, m.payload.size()); continue; }
				const int display_tag = int(m.tag) | (m.settings_update ? 0x1000 : 0);
				print_payload(m.dir, m.frame_index, display_tag, m.payload, m.session);
			}
			printed_through = pk.frame_index;
			if (max_frames > 0 && (seen - skip_frames) >= max_frames) {
				stopped_early = true;
				return false; // budget reached — stop streaming
			}
			return true;
		};
		if (!net::stream_pcap_udp_file(path, on_dg)) {
			if (!stopped_early) {
				std::fprintf(stderr, "FAILED to stream pcap %s\n", path);
				return 1;
			}
		}
		std::fprintf(stderr,
		             "streamed %ld datagrams from %s%s (through frame %ld)\n",
		             seen, path, stopped_early ? " [budget reached]" : "",
		             printed_through);
		if (coverage_mode) coverage_emit();
		else if (histogram_mode) hist_emit();
		return 0;
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
			d.dstport = pk.dstport;
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
	for (auto &d : dgrams) caps.push_back({d.frame, d.srcport, d.dstport, std::move(d.bytes)});

	for (const auto &m : decode_capture_to_messages(caps)) {
		if (!tag_filter.empty() && !tag_filter.count(m.tag & 0xFF)) continue;
		if (histogram_mode) { hist_tally(m.dir, m.tag, m.payload.size()); continue; }
		const int display_tag = int(m.tag) | (m.settings_update ? 0x1000 : 0);
		print_payload(m.dir, m.frame_index, display_tag, m.payload, m.session);
	}
	if (coverage_mode) coverage_emit();
	else if (histogram_mode) hist_emit();
	return 0;
}
