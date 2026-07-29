// Cross-validate the in-game decoders against the AUTHORED dvxc1 probe ("ON RE
// Probe COOP Dvxc1", probe 3) — the successor to nw_dvxi3_groundtruth_test (TDM)
// and nw_pool_groundtruth_test (dvxi5, A&S). This probe moved to a CO-OP /
// objective (waypoint) gametype (Header.attrib_flags 0x01000000 -> g_GameType
// 0x30020), which is the unique mode that unlocks BOTH deferred wire bodies the
// prior probes never carried:
//   - S2C 0x0F waypoint records (gated (g_GameType & 0xFFFDFFFF)==0x10020), and
//   - S2C 0x0A objective sub-block 3 (gated g_GameType & 0x20000).
// It also forces the S2C 0x60 multi-chunk session-var transfer (a >200 B host
// MOTD) + the C2S 0x33 re-request, and fills S2C 0x7B gameName ("jox01") + the
// `extra` dword (= g_GameType). On top of the new bodies it re-validates the
// already-decoded spawn batches (0x10 / 0x0D / 0x0C / 0x20) field-for-field.
//
// fixtures/novaworld/dvxc1_manifest.txt is the ground-truth oracle. The host
// (retail Jointops.exe + JOX expansion) serialized the mission onto the wire;
// this test decodes the matching loopback capture and asserts each decoded record
// reproduces the authored fact. Reads the pcap directly via the shared apps/common
// pcap reader; path from NW_DVXC1_PCAP else DEFAULT_DVXC1_PCAP. Skips cleanly when
// the capture is absent (.scratch is untracked) so CI stays green.

#include <npwire/ingame_decode.h>
#include <npwire/wire_capture.h>

#include <pcapio/pcap_reader.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace opennova;

namespace {

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "."
#endif
#ifndef DEFAULT_DVXC1_PCAP
#define DEFAULT_DVXC1_PCAP ""
#endif

// One authored entity from the manifest (a row of dvxc1_manifest.txt).
struct Expected {
	int wire_tag = 0;     // 0x0D / 0x20 / 0x0C / 0x10
	uint16_t type_id = 0;
	int bms_id = 0;
	int32_t pos_x = 0, pos_y = 0, pos_z = 0;
	int team = 0;
	int facing_deg = 0;
	std::string role;
	bool matched = false;
};

bool load_manifest(const std::string &path, std::vector<Expected> &out) {
	std::ifstream f(path);
	if (!f) return false;
	std::string line;
	while (std::getline(f, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		size_t s = line.find_first_not_of(" \t");
		if (s == std::string::npos) continue;
		if (line[s] == '#') continue;
		std::istringstream ls(line.substr(s));
		std::string tag;
		Expected e;
		if (!(ls >> tag)) continue;
		e.wire_tag = int(std::strtol(tag.c_str(), nullptr, 0));
		std::string type;
		if (!(ls >> type)) continue;
		e.type_id = uint16_t(std::strtol(type.c_str(), nullptr, 0));
		if (!(ls >> e.bms_id >> e.pos_x >> e.pos_y >> e.pos_z >> e.team >>
		      e.facing_deg)) {
			continue;
		}
		int group = 0;
		ls >> group; // optional column, unused here
		ls >> e.role;
		out.push_back(std::move(e));
	}
	return true;
}

// Wire BAM heading the engine derives from the authored facing ("90 - yaw").
uint32_t expected_bam(int facing_deg) {
	int deg = ((90 - facing_deg) % 360 + 360) % 360;
	return uint32_t((uint64_t(deg) << 32) / 360);
}

// --- spawn-batch collectors (the re-validation oracle, as in dvxi3) ----------
std::vector<PoolSpawnRecord> g_spawn_0d;      // S2C 0x0D records
std::vector<Pool3SyncRecord> g_sync_20;       // S2C 0x20 records
std::vector<OrganicSpawnRecord> g_spawn_0c;   // S2C 0x0C records (has_body)
std::vector<StaticEntityRecord> g_static_10;  // S2C 0x10 records (non-empty)
int g_0c_batches = 0, g_0c_clean = 0;
int g_10_batches = 0, g_10_clean = 0;

// --- deferred-tag witnesses (the headline new bodies this probe surfaces) -----
uint32_t g_game_type = 0;                     // learned from S2C 0x7B `extra`
std::vector<WorldStateLoad> g_world_0f;       // S2C 0x0F (waypoint records)
int g_0f_clean = 0;
std::vector<FullPlayerInfo> g_info_7b;        // S2C 0x7B (gameName + extra)
std::vector<FileTransferChunk> g_xfer_60;     // S2C 0x60 (multi-chunk VarList)
int g_c2s_33 = 0;                             // C2S 0x33 next-chunk re-requests
int g_0a_sub3 = 0;                            // S2C 0x0A frames with sub_block==3
int g_obj_0a_present = 0;                     // ...of which carried the objective body
// presence counters (soft re-validation of the lifecycle tags)
int g_n16 = 0, g_n46 = 0, g_n5a = 0, g_n6e = 0, g_n40 = 0, g_n1e = 0, g_n26 = 0;

void collect_s2c(int tag, const std::vector<uint8_t> &a) {
	if (tag == 0x0D) {
		PoolSpawnBatch batch;
		decode_pool_spawn_batch(a.data(), a.size(), batch);
		for (auto &r : batch.records) g_spawn_0d.push_back(r);
	} else if (tag == 0x20) {
		Pool3SyncBatch batch;
		decode_pool3_sync_batch(a.data(), a.size(), batch);
		for (auto &r : batch.records)
			if (!r.is_empty_slot) g_sync_20.push_back(r);
	} else if (tag == 0x0C) {
		OrganicSpawnBatch batch;
		const bool clean = decode_organic_spawn_batch(a.data(), a.size(), batch);
		g_0c_batches++;
		if (clean) g_0c_clean++;
		for (auto &r : batch.records)
			if (r.has_body) g_spawn_0c.push_back(r);
	} else if (tag == 0x10) {
		StaticEntityBatch batch;
		const bool clean = decode_static_entity_batch(a.data(), a.size(), batch);
		g_10_batches++;
		if (clean) g_10_clean++;
		for (auto &r : batch.records)
			if (!r.is_empty_slot) g_static_10.push_back(r);
	} else if (tag == 0x7B) {
		FullPlayerInfo fi;
		decode_full_player_info(a.data(), a.size(), fi);
		g_info_7b.push_back(fi);
		g_game_type = fi.extra;   // the host puts g_GameType in `extra` (probe3: 0x30020)
	} else if (tag == 0x0F) {
		WorldStateLoad ws;
		// Co-op (0x30020) is a waypoint gametype, so the records ride the wire.
		const bool clean = decode_world_state_load(
		    a.data(), a.size(), ws, (g_game_type & 0xFFFDFFFFu) == 0x10020u);
		if (clean) g_0f_clean++;
		g_world_0f.push_back(std::move(ws));
	} else if (tag == 0x60) {
		FileTransferChunk ch;
		if (decode_file_transfer_chunk(a.data(), a.size(), ch))
			g_xfer_60.push_back(ch);
	} else if (tag == 0x0A) {
		FrameUpdate fu;
		// Only the header/sub-block matter here; an Unknown class fails the record
		// loop closed AFTER the objective body, so present is still set.
		decode_frame_update(a.data(), a.size(),
		                    [](uint16_t) { return EntityClass::Unknown; }, fu,
		                    (g_game_type & 0x20000u) != 0);
		if (fu.sub_block == 3) {
			g_0a_sub3++;
			if (fu.objective.present) g_obj_0a_present++;
		}
	} else if (tag == 0x16) g_n16++;
	else if (tag == 0x46) g_n46++;
	else if (tag == 0x5A) g_n5a++;
	else if (tag == 0x6E) g_n6e++;
	else if (tag == 0x40) g_n40++;
	else if (tag == 0x1E) g_n1e++;
	else if (tag == 0x26) g_n26++;
}

int g_failures = 0;
void check(bool cond, const std::string &what) {
	if (!cond) {
		std::printf("FAIL: %s\n", what.c_str());
		g_failures++;
	}
}

} // namespace

int main() {
	std::string pcap_path;
	if (const char *env = std::getenv("NW_DVXC1_PCAP"); env && *env)
		pcap_path = env;
	else
		pcap_path = DEFAULT_DVXC1_PCAP;

	std::vector<net::PcapDatagram> pkts;
	if (pcap_path.empty() || !net::read_pcap_udp_file(pcap_path, pkts)) {
		std::printf("[skip] dvxc1 capture not found (set NW_DVXC1_PCAP to "
		            "...probe3.pcapng) — '%s'\n",
		            pcap_path.c_str());
		return 0;
	}

	std::vector<Expected> manifest;
	const std::string manifest_path = std::string(FIXTURE_DIR) + "/dvxc1_manifest.txt";
	if (!load_manifest(manifest_path, manifest)) {
		std::printf("FAILED to read manifest %s\n", manifest_path.c_str());
		return 1;
	}
	std::printf("loaded %zu authored entities from %s\n", manifest.size(),
	            manifest_path.c_str());
	std::printf("loaded %zu datagrams from %s\n", pkts.size(), pcap_path.c_str());

	std::vector<CaptureDatagram> caps;
	caps.reserve(pkts.size());
	for (auto &pk : pkts)
		caps.push_back({pk.frame_index, pk.srcport, pk.dstport, std::move(pk.payload)});
	for (const auto &m : decode_capture_to_messages(caps)) {
		if (m.settings_update) continue;
		if (m.dir == 'S') collect_s2c(int(m.tag), m.payload);
		else if (m.dir == 'C' && m.tag == 0x33) g_c2s_33++;
	}
	std::printf("collected 0x0D=%zu, 0x20=%zu, 0x0C=%zu, 0x10=%zu records "
	            "(0x0C %d/%d, 0x10 %d/%d batches consumed exactly)\n",
	            g_spawn_0d.size(), g_sync_20.size(), g_spawn_0c.size(),
	            g_static_10.size(), g_0c_clean, g_0c_batches, g_10_clean,
	            g_10_batches);
	std::printf("deferred: g_GameType=0x%05x  0x7B=%zu  0x0F=%zu(clean %d)  "
	            "0x60=%zu chunks  C2S 0x33=%d  0x0A sub3=%d(obj %d)\n",
	            g_game_type, g_info_7b.size(), g_world_0f.size(), g_0f_clean,
	            g_xfer_60.size(), g_c2s_33, g_0a_sub3, g_obj_0a_present);

	// Re-grounded entities can shift z sub-unit; x/y (mission plane) are lossless.
	constexpr int32_t kZTol = 1 * 0x10000; // 1.0 world unit

	// --- 0x0D pool-entity spawn (vehicles + spawn-volume objects) -------------
	// Match the nearest authored 0x0D of the same type within a generous tol;
	// auto-spawned vehicle weapons (emplaced guns) have no manifest row -> skipped.
	constexpr int64_t kVehTol = 4 * int64_t(0x10000); // 4 world units (re-ground drift)
	for (const auto &r : g_spawn_0d) {
		if (r.slot_id == 0xFFFF) continue;
		Expected *m = nullptr;
		int64_t best = -1;
		for (auto &e : manifest) {
			if (e.wire_tag != 0x0D || e.type_id != r.item_type_id || e.matched) continue;
			int64_t dx = int64_t(e.pos_x) - r.pos_x; if (dx < 0) dx = -dx;
			int64_t dy = int64_t(e.pos_y) - r.pos_y; if (dy < 0) dy = -dy;
			if (best < 0 || dx + dy < best) { best = dx + dy; m = &e; }
		}
		if (!m || best > kVehTol) continue; // auto-spawned weapon / no authored match
		m->matched = true;
		char buf[256];
		std::snprintf(buf, sizeof(buf), "%s: 0x0D position within vehicle tol (dx+dy=%.3f)",
		              m->role.c_str(), double(best) / 65536.0);
		check(best <= kVehTol, buf);
		const bool team_gate = (r.spawn_flags & 0x0010) != 0;
		if (m->team != 0) {
			std::snprintf(buf, sizeof(buf), "%s: team gate set + team_byte==%d (got %d)",
			              m->role.c_str(), m->team, r.team_byte);
			check(team_gate && r.team_byte == m->team, buf);
		} else {
			std::snprintf(buf, sizeof(buf), "%s: team gate CLEAR (team 0)", m->role.c_str());
			check(!team_gate, buf);
		}
		// Orientation: euler_z (entity+16, 0x01-gated) = (90 - authored facing) BAM
		// (D-NET-86), same engine-heading frame the statics use. Gate clear == the
		// engine heading is 0 (authored facing 90, e.g. the blue littlebird).
		const uint32_t exp_bam = expected_bam(m->facing_deg);
		const bool yaw_gate = (r.spawn_flags & 0x0001) != 0;
		const uint32_t got_bam = yaw_gate ? uint32_t(r.euler_z) : 0u;
		std::snprintf(buf, sizeof(buf),
		              "%s: 0x0D yaw BAM == 0x%08x for facing %d (got 0x%08x, gate %d)",
		              m->role.c_str(), exp_bam, m->facing_deg, got_bam, int(yaw_gate));
		check(got_bam == exp_bam, buf);
	}

	// --- 0x20 pool-3 sync (player starts + air-spawn + waypoint markers) ------
	for (const auto &r : g_sync_20) {
		Expected *m = nullptr;
		for (auto &e : manifest)
			if (e.wire_tag == 0x20 && e.type_id == r.item_type_id &&
			    e.pos_x == r.pos_x && e.pos_y == r.pos_y && !e.matched) { m = &e; break; }
		char buf[256];
		std::snprintf(buf, sizeof(buf),
		              "0x20 record type=0x%04x pos=(%d,%d) matches an authored entity",
		              r.item_type_id, r.pos_x, r.pos_y);
		check(m != nullptr, buf);
		if (!m) continue;
		m->matched = true;
		const bool team_gate = (r.flags_byte & 0x08) != 0;
		if (m->team != 0) {
			std::snprintf(buf, sizeof(buf), "%s: team gate set + team_byte==%d (got %d)",
			              m->role.c_str(), m->team, r.team_byte);
			check(team_gate && r.team_byte == m->team, buf);
		} else {
			std::snprintf(buf, sizeof(buf), "%s: team gate CLEAR (team 0)", m->role.c_str());
			check(!team_gate, buf);
		}
		std::snprintf(buf, sizeof(buf), "%s: net_handle == authored bms_id %d (got %d)",
		              m->role.c_str(), m->bms_id, r.net_handle);
		check(r.net_handle == m->bms_id, buf);
	}

	// --- 0x0C pool-0 organic spawn (AI infantry; players ride 0x0C too) -------
	check(g_0c_batches > 0, "at least one 0x0C organic spawn batch present");
	check(g_0c_clean == g_0c_batches, "every 0x0C batch consumed exactly");
	// Organics re-ground onto the navmesh on spawn, so the host's serialized xy
	// drifts sub-unit from the authored grounded pose (the manifest comes from the
	// editor model) — match the nearest authored AI of the same type within tol,
	// exactly as the 0x0D vehicle path does. AI types are unique + ≥15 u apart, so
	// no cross-match. Players (type 0x14B9) ride 0x0C with no manifest row → skipped.
	constexpr int64_t kOrgTol = 2 * int64_t(0x10000); // 2 world units (re-ground drift)
	for (const auto &r : g_spawn_0c) {
		Expected *m = nullptr;
		int64_t best = -1;
		for (auto &e : manifest) {
			if (e.wire_tag != 0x0C || e.type_id != r.item_type_id || e.matched) continue;
			int64_t dx = int64_t(e.pos_x) - r.pos_x; if (dx < 0) dx = -dx;
			int64_t dy = int64_t(e.pos_y) - r.pos_y; if (dy < 0) dy = -dy;
			if (best < 0 || dx + dy < best) { best = dx + dy; m = &e; }
		}
		if (!m || best > kOrgTol) continue;
		m->matched = true;
		{
			char buf[256];
			std::snprintf(buf, sizeof(buf), "%s: 0x0C position within organic tol (dx+dy=%.4f)",
			              m->role.c_str(), double(best) / 65536.0);
			check(best <= kOrgTol, buf);
		}
		char buf[256];
		std::snprintf(buf, sizeof(buf), "%s: team == %d (got %d)", m->role.c_str(),
		              m->team, r.team);
		check(r.team == m->team, buf);
		const uint32_t exp = expected_bam(m->facing_deg);
		std::snprintf(buf, sizeof(buf),
		              "%s: orientation BAM == 0x%08x for facing %d (got 0x%08x)",
		              m->role.c_str(), exp, m->facing_deg, uint32_t(r.orientation));
		check(uint32_t(r.orientation) == exp, buf);
	}

	// --- 0x10 pool-2 static-entity batch (armory, oil pump/tower) -------------
	check(g_10_batches > 0, "at least one 0x10 static-entity batch present");
	check(g_10_clean == g_10_batches, "every 0x10 batch consumed exactly");
	for (const auto &r : g_static_10) {
		Expected *m = nullptr;
		for (auto &e : manifest)
			if (e.wire_tag == 0x10 && e.type_id == r.item_type_id &&
			    e.pos_x == r.pos_x && e.pos_y == r.pos_y) { m = &e; break; }
		char buf[256];
		std::snprintf(buf, sizeof(buf),
		              "0x10 record type=0x%04x pos=(%d,%d) matches an authored entity",
		              r.item_type_id, r.pos_x, r.pos_y);
		check(m != nullptr, buf);
		if (!m) continue;
		m->matched = true;
		int32_t dz = r.pos_z - m->pos_z; if (dz < 0) dz = -dz;
		std::snprintf(buf, sizeof(buf), "%s: z within tol (delta=%.3f)",
		              m->role.c_str(), double(r.pos_z - m->pos_z) / 65536.0);
		check(dz <= kZTol, buf);
		const bool team_gate = (r.field_flags & 0x0010) != 0;
		if (m->team != 0) {
			std::snprintf(buf, sizeof(buf), "%s: team gate set + team_byte==%d (got %d)",
			              m->role.c_str(), m->team, r.team_byte);
			check(team_gate && r.team_byte == m->team, buf);
		} else {
			std::snprintf(buf, sizeof(buf), "%s: team gate CLEAR (team 0)", m->role.c_str());
			check(!team_gate, buf);
		}
		// Orientation: euler_z (entity+16, 0x01-gated) is the engine yaw heading =
		// (90 - authored facing) BAM (D-NET-86) — the field that was mislabeled as
		// velocity and dropped, leaving every static facing east in the spectator.
		// The host serializes it only when non-zero, so a cleared gate == engine
		// heading 0 == authored facing 90 (the blue armory pair).
		const uint32_t exp_bam = expected_bam(m->facing_deg);
		const bool yaw_gate = (r.field_flags & 0x0001) != 0;
		const uint32_t got_bam = yaw_gate ? uint32_t(r.euler_z) : 0u;
		std::snprintf(buf, sizeof(buf),
		              "%s: static yaw BAM == 0x%08x for facing %d (got 0x%08x, gate %d)",
		              m->role.c_str(), exp_bam, m->facing_deg, got_bam, int(yaw_gate));
		check(got_bam == exp_bam, buf);
	}

	// --- every authored entity must have been witnessed -----------------------
	int expect_0d = 0, expect_20 = 0, expect_0c = 0, expect_10 = 0;
	for (const auto &e : manifest) {
		const std::string w = "authored entity '" + e.role + "' witnessed on the wire";
		if (e.wire_tag == 0x0D) { expect_0d++; check(e.matched, w); }
		else if (e.wire_tag == 0x20) { expect_20++; check(e.matched, w); }
		else if (e.wire_tag == 0x0C) { expect_0c++; check(e.matched, w); }
		else if (e.wire_tag == 0x10) { expect_10++; check(e.matched, w); }
	}

	// === DEFERRED-TAG witnesses (the reason this probe exists) =================
	// g_GameType derived on the wire from the 0x7B `extra` field — confirms the
	// IDA-derived Co-op -> 0x30020 mapping (and that extra carries g_GameType).
	check(g_game_type == 0x30020u, "0x7B extra == g_GameType 0x30020 (Co-op on the wire)");

	// 0x7B: non-empty gameName ("jox01" = the JOX expansion id) + extra.
	bool found_jox = false, found_server = false;
	for (const auto &fi : g_info_7b) {
		if (fi.game_name == "jox01" && fi.extra == 0x30020u) found_jox = true;
		if (!fi.server_name.empty()) found_server = true;
	}
	check(!g_info_7b.empty(), "at least one S2C 0x7B full-player-info present");
	check(found_jox, "0x7B gameName=\"jox01\" with extra=0x30020 witnessed");
	check(found_server, "0x7B carries a non-empty server name");

	// 0x0F: waypoint records populated (first wire witness) + clean consume.
	check(!g_world_0f.empty(), "at least one S2C 0x0F world-state-load present");
	check(g_0f_clean == int(g_world_0f.size()),
	      "every 0x0F consumed exactly (waypoint tail parsed, not misread)");
	bool found_wp = false;
	for (const auto &ws : g_world_0f)
		if (ws.waypoint_count > 0 && ws.waypoints.size() == ws.waypoint_count)
			found_wp = true;
	check(found_wp, "0x0F carries waypoint records (waypointCount>0) — first witness");

	// 0x0A objective sub-block 3: present + body read on every sub-block-3 frame.
	check(g_0a_sub3 > 0, "at least one S2C 0x0A objective sub-block 3 (flags2&3==3)");
	check(g_obj_0a_present == g_0a_sub3,
	      "every 0x0A sub-block-3 frame carried the 16-B objective body");

	// 0x60 multi-chunk session-var transfer + C2S 0x33 re-request (D-NET-74).
	bool has_more = false, has_tail = false;
	for (const auto &ch : g_xfer_60) {
		if (ch.chunk_offset == 0 && ch.total_size > ch.chunk_size) has_more = true;
		if (ch.chunk_offset > 0) has_tail = true;
	}
	check(has_more, "S2C 0x60 first chunk is non-final (total > chunk) — multi-chunk");
	check(has_tail, "S2C 0x60 tail chunk at offset>0 present");
	check(g_c2s_33 > 0, "C2S 0x33 next-chunk re-request fired (first multi-chunk witness)");

	// soft re-validation of the lifecycle/scoreboard tags (all present in probe3).
	check(g_n16 > 0, "S2C 0x16 player-list present");
	check(g_n46 > 0, "S2C 0x46 player-sync present");
	check(g_n5a > 0, "S2C 0x5A weapon-loadout present");
	check(g_n6e > 0, "S2C 0x6E roster-sync present");
	check(g_n40 > 0, "S2C 0x40 capture-zone present");
	check(g_n1e > 0, "S2C 0x1E game-event present");
	check(g_n26 > 0, "S2C 0x26 kill-sync present");

	std::printf("\nexpected 0x0D=%d, 0x20=%d, 0x0C=%d, 0x10=%d authored entities\n",
	            expect_0d, expect_20, expect_0c, expect_10);
	if (g_failures) {
		std::printf("\n%d assertion(s) failed\n", g_failures);
		return 1;
	}
	std::printf("\nPASS: every authored dvxc1 entity reproduced on the wire, AND the "
	            "Co-op deferred bodies witnessed — 0x0F waypoint records, 0x0A objective "
	            "sub-block 3, 0x60 multi-chunk transfer + C2S 0x33, 0x7B gameName + "
	            "g_GameType.\n");
	return 0;
}
