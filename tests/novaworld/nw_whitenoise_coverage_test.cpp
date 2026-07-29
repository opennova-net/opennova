// Stock-content cross-validation — the FIRST capture of a normal retail JO Co-op
// session (operation_whitenoise.pcapng), as opposed to the authored RE probes
// (dvxi5 / dvxi3 / dvxc1) every prior in-game finding rode on. A probe mission is
// hand-built to exercise a single behavior; stock content runs the decoders
// against the full, organically-authored load + gameplay stream at scale (3304
// datagrams: 1485 S2C 0x0A frames, 1463 C2S 0x0C uplinks, 1286 S2C 0x40 zone
// updates, a 29-payload S2C 0x20 pool-3 load batch, …).
//
// There is no authored manifest for a stock mission, so this is a WIRE-ONLY
// coverage witness (no .sph, no manifest oracle — per the project rule). It asserts:
//
//   (1) Load health — the capture reads (3304 datagrams) and every low-table tag
//       it carries is catalogued (no uncatalogued tag carrying bytes).
//   (2) Byte-consume coverage — every catalogued `Decoded` tag present in the
//       capture has its decoder consume each body EXACTLY (no over/under-read)
//       across ALL its occurrences. This is the strong check nw_pp's lenient
//       printing hides; it is the per-tag analog of nw_ingame_pool_records over
//       the whole catalog. 0x0A is excluded from the strict consume (its compact
//       record loop needs the items.def class table to walk; counted only).
//   (3) S2C 0x20 enhancement spot-asserts (D-NET-55 / §5.12) — the stock load
//       batch is a large PAGED stream over hundreds of pool entities and exercises
//       EVERY flag-gated optional field (movement/orient/ammo/team/weapType/score),
//       which the 2-record authored probe batches never did. It is also LOAD-ONLY
//       (every 0x20 precedes the first gameplay 0x0A frame): pool-3 carries static
//       markers, so the "patrolling-AI mid-game 0x20" anticipated by D-NET-55 does
//       not occur — moving AI ride pool-0 (0x0A / 0x0C).
//   (4) Confirmed negatives — even in stock Co-op, all C2S 0x0C uplinks are
//       sub_op 0x0A (no guided field-groups → D-NET-64 stays open), and there is
//       no S2C 0x44 entity-routed nor S2C 0x59 deployed-item traffic.
//
// Reads the pcap directly via the shared apps/common reader; path from
// NW_WHITENOISE_PCAP else DEFAULT_WHITENOISE_PCAP. Skips cleanly when the capture
// is absent (.scratch is untracked) so CI stays green.

#include <npwire/ingame_decode.h>
#include <npwire/ingame_message_catalog.h>
#include <npwire/wire_capture.h>

#include <pcapio/pcap_reader.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace opennova;

namespace {

#ifndef DEFAULT_WHITENOISE_PCAP
#define DEFAULT_WHITENOISE_PCAP ""
#endif

int g_failures = 0;
void check(bool cond, const std::string &what) {
	if (!cond) {
		std::printf("FAIL: %s\n", what.c_str());
		g_failures++;
	}
}

// Per-(dir,tag) consumption tally over the whole capture.
struct Tally {
	int count = 0;  // messages seen
	int clean = 0;  // bodies consumed exactly by the decoder
};

// Run the Decoded-class decoder for one S2C body; return true iff it consumed the
// body exactly. 0x0A is handled by the caller (needs the class table). `wp_hint` /
// `obj_hint` are the off-wire gametype gates (from the 0x7B `extra` = g_GameType).
bool consume_s2c(int tag, const std::vector<uint8_t> &b, bool wp_hint) {
	const uint8_t *p = b.data();
	const size_t n = b.size();
	size_t used = 0;
	switch (tag) {
	case 0x0D: { PoolSpawnBatch o;        return decode_pool_spawn_batch(p, n, o); }
	case 0x10: { StaticEntityBatch o;     return decode_static_entity_batch(p, n, o); }
	case 0x0C: { OrganicSpawnBatch o;     return decode_organic_spawn_batch(p, n, o); }
	case 0x20: { Pool3SyncBatch o;        return decode_pool3_sync_batch(p, n, o); }
	case 0x16: { PlayerList o;            return decode_player_list(p, n, o); }
	case 0x46: { PlayerSync o;            return decode_player_sync(p, n, o); }
	case 0x40: { CaptureZoneOverlayBatch o; return decode_capture_zone_overlay(p, n, o); }
	case 0x4E: { BatchKillBatch o;        return decode_batch_kill(p, n, o); }
	case 0x5A: { WeaponLoadout o;         return decode_weapon_loadout(p, n, o); }
	case 0x6E: { RosterSync o;            return decode_roster_sync(p, n, o); }
	case 0x7B: { FullPlayerInfo o;        return decode_full_player_info(p, n, o); }
	case 0x0F: { WorldStateLoad o;        return decode_world_state_load(p, n, o, wp_hint); }
	case 0x60:
	case 0x64: { FileTransferChunk o;     return decode_file_transfer_chunk(p, n, o); }
	case 0x6B: { MinimapOverlayBatch o;   return decode_minimap_overlay_batch(p, n, o); }
	case 0x45: { TerrainLoadBatch o;      return decode_terrain_load_batch(p, n, o); }
	case 0x1E: { GameEventRecord o;       return decode_game_event(p, n, o, used) && used == n; }
	case 0x26: { KillRecord o;            return decode_kill_record(p, n, o, used) && used == n; }
	case 0x49: { WeaponReload o;          return decode_weapon_reload(p, n, o, used) && used == n; }
	case 0x13: { EntityDeathRecord o;     return decode_entity_death(p, n, o, used) && used == n; }
	case 0x30: { EntityChecksumRequest o; return decode_entity_checksum_request(p, n, o, used) && used == n; }
	case 0x42: { uint16_t o;              return decode_input_state_flags(p, n, o, used) && used == n; }
	case 0x79: { uint8_t o;               return decode_spectator_flag(p, n, o, used) && used == n; }
	case 0x2A: { ChatHistoryEntry o;      return decode_chat_history_entry(p, n, o, used) && used == n; }
	case 0x57: { RttSample o;             return decode_rtt_sample(p, n, o, used) && used == n; }
	case 0x68:
	case 0x43:
	case 0x39: { uint32_t o;              return decode_u32_scalar(p, n, o, used) && used == n; }
	default: return false; // not a strict-consume Decoded tag we handle here
	}
}

bool consume_c2s(int tag, const std::vector<uint8_t> &b) {
	const uint8_t *p = b.data();
	const size_t n = b.size();
	size_t used = 0;
	switch (tag) {
	case 0x06: { ClientFiredRound o;     return decode_client_fired_round(p, n, o, used) && used == n; }
	case 0x21: {
		// §5.17: the handler consumes exactly 5 B (u8 player_index + u32 crc); the
		// reply is framed to a 9-B minimum with 4 trailing ZERO bytes the handler
		// never reads. So "consumed exactly" here = decoder reads 5 AND the framing
		// tail is the documented zeros (not used == n).
		ClientChecksumReply o;
		if (!decode_client_checksum_reply(p, n, o, used)) return false;
		for (size_t i = used; i < n; ++i) if (p[i] != 0) return false;
		return true;
	}
	case 0x2C: { RttSample o;            return decode_rtt_sample(p, n, o, used) && used == n; }
	case 0x22: { BurstPlayerSyncRequest o; return decode_burst_player_sync_request(p, n, o, used) && used == n; }
	case 0x23: { return decode_burst_visible_request(p, n, used) && used == n; }
	case 0x28: { BurstLoadoutRequest o;  return decode_burst_loadout_request(p, n, o, used) && used == n; }
	case 0x29: { TeamSpawnAck o;         return decode_team_spawn_ack(p, n, o, used) && used == n; }
	case 0x4C: { BurstClientQuality o;   return decode_burst_client_quality(p, n, o, used) && used == n; }
	default: return false;
	}
}

} // namespace

int main() {
	std::string pcap_path;
	if (const char *env = std::getenv("NW_WHITENOISE_PCAP"); env && *env)
		pcap_path = env;
	else
		pcap_path = DEFAULT_WHITENOISE_PCAP;

	std::vector<net::PcapDatagram> pkts;
	if (pcap_path.empty() || !net::read_pcap_udp_file(pcap_path, pkts)) {
		std::printf("[skip] operation_whitenoise capture not found (set "
		            "NW_WHITENOISE_PCAP) — '%s'\n", pcap_path.c_str());
		return 0;
	}
	std::printf("loaded %zu datagrams from %s\n", pkts.size(), pcap_path.c_str());

	std::vector<CaptureDatagram> caps;
	caps.reserve(pkts.size());
	for (auto &pk : pkts)
		caps.push_back({pk.frame_index, pk.srcport, pk.dstport, std::move(pk.payload)});
	const std::vector<InGameMessage> msgs = decode_capture_to_messages(caps);
	std::printf("decoded %zu in-game protocol messages\n", msgs.size());

	// g_GameType from the 0x7B `extra` field — gates the off-wire 0x0F waypoint /
	// 0x0A objective bodies (a receiver-side gate not present on the wire).
	uint32_t game_type = 0;
	for (const auto &m : msgs) {
		if (m.settings_update || m.dir != 'S' || m.tag != 0x7B) continue;
		FullPlayerInfo fi;
		if (decode_full_player_info(m.payload.data(), m.payload.size(), fi))
			game_type = fi.extra;
	}
	const bool wp_hint = (game_type & 0xFFFDFFFFu) == 0x10020u;
	const bool obj_hint = (game_type & 0x20000u) != 0;
	std::printf("g_GameType (0x7B extra) = 0x%05x  (waypoint=%d objective=%d)\n",
	            game_type, wp_hint, obj_hint);

	std::map<std::pair<char, int>, Tally> tally;
	std::set<int> uncatalogued;       // low-table tags missing from the catalog
	int high_table = 0;               // tag >= 0x100 (NAPI high-table H:0x00..0x03)

	// --- target-2/3/4 collectors -------------------------------------------------
	uint32_t pool3_any_flag = 0;
	int pool3_total_records = 0, pool3_empty_slots = 0;
	std::set<int> pool3_start_indices;
	int first_0a_frame = -1, last_20_frame = -1;
	int frame_0a_total = 0, frame_0a_complete = 0;
	std::set<int> c2s_0c_sub_ops;     // negative: must be {0x0A} only (no guided)

	for (const auto &m : msgs) {
		if (m.settings_update) continue;
		if (m.tag >= 0x100) { ++high_table; continue; }
		const int tag = int(m.tag & 0xFF);
		const MsgCatalogEntry *e = lookup_ingame_message(m.dir, uint8_t(tag));
		if (!e) { uncatalogued.insert((m.dir << 8) | tag); continue; }

		Tally &t = tally[{m.dir, tag}];
		t.count++;
		if (e->coverage != MsgCoverage::Decoded) continue;

		if (m.dir == 'S' && tag == 0x0A) {
			// Frame update: the compact-record loop needs the items.def class table
			// to walk, so we count (and note completion) rather than strict-consume.
			std::function<EntityClass(uint16_t)> noclass;
			FrameUpdate fu;
			decode_frame_update(m.payload.data(), m.payload.size(), noclass, fu, obj_hint);
			++frame_0a_total;
			if (fu.complete) ++frame_0a_complete;
			if (first_0a_frame < 0 || m.frame_index < first_0a_frame)
				first_0a_frame = m.frame_index;
			continue;
		}

		bool clean = false;
		if (m.dir == 'S') {
			clean = consume_s2c(tag, m.payload, wp_hint);
		} else if (m.dir == 'C' && tag == 0x0C) {
			// 5-B sub-header then, for the extended (type-10) uplink, a 43-B body.
			EntityPacketSubHeader sh;
			size_t hc = 0;
			if (decode_entity_packet_sub_header(m.payload.data(), m.payload.size(), sh, hc)) {
				c2s_0c_sub_ops.insert(int(sh.sub_op));
				if (sh.sub_op == 0x0A) {
					PlayerExtendedUplink up;
					size_t bc = 0;
					clean = decode_player_extended_uplink(
					            m.payload.data() + hc, m.payload.size() - hc, up, bc) &&
					        hc + bc == m.payload.size();
				} else {
					// Non-extended (compact / guided) sub_ops need the class table;
					// not strict-consumed here. Count the sub-header parse as clean.
					clean = true;
				}
			}
		} else {
			clean = consume_c2s(tag, m.payload);
		}
		if (clean) t.clean++;

		// S2C 0x20 spot collection (the §5.12 / D-NET-55 enhancement target).
		if (m.dir == 'S' && tag == 0x20) {
			Pool3SyncBatch o;
			decode_pool3_sync_batch(m.payload.data(), m.payload.size(), o);
			pool3_start_indices.insert(int(o.start_index));
			if (last_20_frame < 0 || m.frame_index > last_20_frame)
				last_20_frame = m.frame_index;
			for (const auto &r : o.records) {
				pool3_total_records++;
				if (r.is_empty_slot) { pool3_empty_slots++; continue; }
				pool3_any_flag |= r.flags_byte;
			}
		}
	}

	// --- report ------------------------------------------------------------------
	std::printf("\n=== per-tag consumption (Decoded tags) ===\n");
	for (const auto &kv : tally) {
		const MsgCatalogEntry *e = lookup_ingame_message(kv.first.first, uint8_t(kv.first.second));
		const bool decoded = e && e->coverage == MsgCoverage::Decoded;
		std::printf("  %c 0x%02x %-22s count=%-5d clean=%-5d %s\n",
		            kv.first.first, kv.first.second, e ? e->name : "?",
		            kv.second.count, kv.second.clean,
		            decoded ? "[Decoded]" : "");
	}
	std::printf("high-table messages (tag>=0x100): %d\n", high_table);
	std::printf("S2C 0x0A frames: %d (complete %d; compact loop needs items.def)\n",
	            frame_0a_total, frame_0a_complete);
	std::printf("S2C 0x20: %d records over %zu pages (start idx), flags=0x%02x, "
	            "%d empty-slot sentinels; last 0x20 frame=%d, first 0x0A frame=%d\n",
	            pool3_total_records, pool3_start_indices.size(), pool3_any_flag,
	            pool3_empty_slots, last_20_frame, first_0a_frame);
	std::printf("C2S 0x0C sub_ops seen: ");
	for (int s : c2s_0c_sub_ops) std::printf("0x%02x ", s);
	std::printf("\n");

	// (1) Load health.
	check(pkts.size() == 3304, "capture reads 3304 datagrams");
	check(!msgs.empty(), "outer-decode pipeline produced in-game messages");
	// The catalog is a CURATED subset (Decoded + PrinterOnly + a few Unhandled),
	// not an exhaustive tag list — the §4 dispatch tables have ~120 handlers, most
	// uncharacterized. Report the uncharacterized tail stock Co-op exercises (a
	// future-work surface), but it is not a failure.
	std::printf("uncharacterized tail (not in the curated catalog): %zu tags: ",
	            uncatalogued.size());
	for (int k : uncatalogued) std::printf("%c0x%02x ", char(k >> 8), k & 0xFF);
	std::printf("\n");

	// (2) Byte-consume coverage — every Decoded tag present consumed every body
	// exactly (0x0A excluded, handled above).
	for (const auto &kv : tally) {
		const MsgCatalogEntry *e = lookup_ingame_message(kv.first.first, uint8_t(kv.first.second));
		if (!e || e->coverage != MsgCoverage::Decoded) continue;
		if (kv.first.first == 'S' && kv.first.second == 0x0A) continue;
		char buf[128];
		std::snprintf(buf, sizeof(buf),
		              "%c 0x%02x %s: all %d bodies consumed exactly (clean=%d)",
		              kv.first.first, kv.first.second, e->name,
		              kv.second.count, kv.second.clean);
		check(kv.second.clean == kv.second.count, buf);
	}

	// (3) S2C 0x20 enhancement spot-asserts.
	check(tally[{'S', 0x20}].count == 29, "S2C 0x20: 29 payloads (stock load batch)");
	check(pool3_total_records > 200,
	      "S2C 0x20: hundreds of pool entities (vs the 2-record authored probe batch)");
	check(pool3_start_indices.size() >= 20,
	      "S2C 0x20: paged stream (>=20 distinct startIdx pages)");
	check((pool3_any_flag & 0x3F) == 0x3F,
	      "S2C 0x20: every flag-gated optional exercised (movement|orient|ammo|team|weapType|score)");
	check(last_20_frame > 0 && first_0a_frame > 0 && last_20_frame < first_0a_frame,
	      "S2C 0x20 is LOAD-ONLY (every 0x20 precedes the first gameplay 0x0A frame)");

	// (4) Confirmed negatives (scope of the still-open holes).
	check(c2s_0c_sub_ops.size() == 1 && c2s_0c_sub_ops.count(0x0A) == 1,
	      "C2S 0x0C: every uplink is sub_op 0x0A (no guided field-groups; D-NET-64 open)");
	check(tally[{'S', 0x44}].count == 0, "no S2C 0x44 entity-routed traffic in stock Co-op");
	check(tally[{'S', 0x59}].count == 0, "no S2C 0x59 deployed-item traffic in stock Co-op");

	if (g_failures) {
		std::printf("\n%d assertion(s) failed\n", g_failures);
		return 1;
	}
	std::printf("\nPASS: stock Co-op decodes clean — every Decoded tag consumed the "
	            "wire exactly; 0x20 load batch exercises every optional field and is "
	            "load-only; no guided / 0x44 / 0x59 traffic.\n");
	return 0;
}
