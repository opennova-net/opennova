// Cross-validate the in-game pool decoders against the AUTHORED dvxi3 probe
// ("ON RE Probe TDM Dvxi3", probe 2) — the successor to nw_pool_groundtruth_test
// (dvxi5, Advance & Secure). Moved to Team Deathmatch on a coastal island, this
// probe additionally forces the S2C 0x10 pool-2 STATIC entity batch (armory, oil
// pump, oil-field decorations) onto the wire, so it cross-validates the new
// decode_static_entity_batch (§5.9) alongside 0x0D / 0x20 / 0x0C.
//
// fixtures/novaworld/dvxi3_manifest.txt is the ground-truth oracle: every
// authored entity's wire_tag / type_id / 16.16 position / team / heading. The
// host (retail Jointops.exe) serialized that mission onto the wire; this test
// decodes the matching loopback capture and asserts each decoded record
// reproduces the authored fact. Reads the pcap directly via the shared
// apps/common pcap reader; path from NW_DVXI3_PCAP else the in-tree .scratch
// capture (DEFAULT_DVXI3_PCAP). Skips cleanly when the capture is absent (it is a
// local-only artifact — .scratch is untracked), so CI stays green.

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
#ifndef DEFAULT_DVXI3_PCAP
#define DEFAULT_DVXI3_PCAP ""
#endif

// One authored entity from the manifest (a row of dvxi3_manifest.txt).
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

// Wire BAM heading the engine derives from the authored facing: the documented
// "90 - yaw" convention. The dvxi3 entities use facing 0/90/180/270 to pin sign.
uint32_t expected_bam(int facing_deg) {
	int deg = ((90 - facing_deg) % 360 + 360) % 360;
	return uint32_t((uint64_t(deg) << 32) / 360);
}

std::vector<PoolSpawnRecord> g_spawn_0d;      // S2C 0x0D records
std::vector<Pool3SyncRecord> g_sync_20;       // S2C 0x20 records
std::vector<OrganicSpawnRecord> g_spawn_0c;   // S2C 0x0C records (has_body)
std::vector<StaticEntityRecord> g_static_10;  // S2C 0x10 records (non-empty)
int g_0c_batches = 0, g_0c_clean = 0;
int g_10_batches = 0, g_10_clean = 0;

void collect(int tag, const std::vector<uint8_t> &assembled) {
	if (tag == 0x0D) {
		PoolSpawnBatch batch;
		decode_pool_spawn_batch(assembled.data(), assembled.size(), batch);
		for (auto &r : batch.records) g_spawn_0d.push_back(r);
	} else if (tag == 0x20) {
		Pool3SyncBatch batch;
		decode_pool3_sync_batch(assembled.data(), assembled.size(), batch);
		for (auto &r : batch.records)
			if (!r.is_empty_slot) g_sync_20.push_back(r);
	} else if (tag == 0x0C) {
		OrganicSpawnBatch batch;
		const bool clean =
		    decode_organic_spawn_batch(assembled.data(), assembled.size(), batch);
		g_0c_batches++;
		if (clean) g_0c_clean++;
		for (auto &r : batch.records)
			if (r.has_body) g_spawn_0c.push_back(r);
	} else if (tag == 0x10) {
		StaticEntityBatch batch;
		const bool clean =
		    decode_static_entity_batch(assembled.data(), assembled.size(), batch);
		g_10_batches++;
		if (clean) g_10_clean++;
		for (auto &r : batch.records)
			if (!r.is_empty_slot) g_static_10.push_back(r);
	}
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
	if (const char *env = std::getenv("NW_DVXI3_PCAP"); env && *env)
		pcap_path = env;
	else
		pcap_path = DEFAULT_DVXI3_PCAP;

	std::vector<net::PcapDatagram> pkts;
	if (pcap_path.empty() || !net::read_pcap_udp_file(pcap_path, pkts)) {
		std::printf("[skip] dvxi3 capture not found (set NW_DVXI3_PCAP to "
		            "...probe2.pcapng) — '%s'\n",
		            pcap_path.c_str());
		return 0;
	}

	std::vector<Expected> manifest;
	const std::string manifest_path = std::string(FIXTURE_DIR) + "/dvxi3_manifest.txt";
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
	for (const auto &m : decode_capture_to_messages(caps))
		if (m.dir == 'S' && !m.settings_update) collect(int(m.tag), m.payload);
	std::printf("collected 0x0D=%zu, 0x20=%zu, 0x0C=%zu, 0x10=%zu records "
	            "(0x0C %d/%d, 0x10 %d/%d batches consumed exactly)\n",
	            g_spawn_0d.size(), g_sync_20.size(), g_spawn_0c.size(),
	            g_static_10.size(), g_0c_clean, g_0c_batches, g_10_clean,
	            g_10_batches);

	// Re-grounded entities can shift z sub-unit; x/y (mission plane) are lossless.
	constexpr int32_t kZTol = 1 * 0x10000; // 1.0 world unit

	// --- 0x0D pool-entity spawn (vehicles + destructible objects) -------------
	// Vehicles re-ground at spawn: ground vehicles hold, but boats snap to water
	// and aircraft take a hover/landing pose, so x/y/z drift sub-unit from the
	// authored grounded pose (x/y is no longer lossless like statics/markers).
	// Match the nearest authored 0x0D of the same type within a generous tol;
	// auto-spawned vehicle weapons (emplaced guns) have no manifest row → skipped.
	// Heading is not carried on the 0x0D wire.
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
	}

	// --- 0x20 pool-3 sync (start markers) -------------------------------------
	for (const auto &r : g_sync_20) {
		Expected *m = nullptr;
		for (auto &e : manifest)
			if (e.wire_tag == 0x20 && e.type_id == r.item_type_id &&
			    e.pos_x == r.pos_x && e.pos_y == r.pos_y) { m = &e; break; }
		char buf[256];
		std::snprintf(buf, sizeof(buf),
		              "0x20 record type=0x%04x pos=(%d,%d) matches an authored entity",
		              r.item_type_id, r.pos_x, r.pos_y);
		check(m != nullptr, buf);
		if (!m) continue;
		m->matched = true;
		// team: neutral (0) markers clear the gate; teamed markers carry team_byte.
		const bool team_gate = (r.flags_byte & 0x08) != 0;
		if (m->team != 0) {
			std::snprintf(buf, sizeof(buf), "%s: team gate set + team_byte==%d (got %d)",
			              m->role.c_str(), m->team, r.team_byte);
			check(team_gate && r.team_byte == m->team, buf);
		} else {
			std::snprintf(buf, sizeof(buf), "%s: team gate CLEAR (team 0)", m->role.c_str());
			check(!team_gate, buf);
		}
		// movement BAM (flag 0x01): the wire OMITS the field when the heading is 0
		// (facing 90 -> BAM 0); present otherwise and must equal the 90-facing BAM.
		const bool move_gate = (r.flags_byte & 0x01) != 0;
		const uint32_t exp = expected_bam(m->facing_deg);
		if (exp == 0) {
			std::snprintf(buf, sizeof(buf), "%s: zero-heading movement omitted/zero (gate=%d)",
			              m->role.c_str(), move_gate);
			check(!move_gate || r.movement_val == 0, buf);
		} else {
			std::snprintf(buf, sizeof(buf),
			              "%s: movement BAM == 0x%08x for facing %d (got 0x%08x)",
			              m->role.c_str(), exp, m->facing_deg, r.movement_val);
			check(move_gate && r.movement_val == exp, buf);
		}
		std::snprintf(buf, sizeof(buf), "%s: net_handle == authored bms_id %d (got %d)",
		              m->role.c_str(), m->bms_id, r.net_handle);
		check(r.net_handle == m->bms_id, buf);
	}

	// --- 0x0C pool-0 organic spawn (AI infantry; players ride 0x0C too) -------
	check(g_0c_batches > 0, "at least one 0x0C organic spawn batch present");
	check(g_0c_clean == g_0c_batches, "every 0x0C batch consumed exactly");
	for (const auto &r : g_spawn_0c) {
		Expected *m = nullptr;
		for (auto &e : manifest)
			if (e.wire_tag == 0x0C && e.type_id == r.item_type_id &&
			    e.pos_x == r.pos_x && e.pos_y == r.pos_y) { m = &e; break; }
		if (!m) continue; // players (type 0x14B9) ride 0x0C; not in the manifest
		m->matched = true;
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

	// --- 0x10 pool-2 static-entity batch (armory, oil pump, static decor) -----
	// The headline new path: pure statics replicate here (NOT 0x0D). Every batch
	// must consume exactly (byte-witness of the §5.9 field map) and each authored
	// static must appear with matching type/pos and team-gate.
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
	check(!g_static_10.empty(), "at least one 0x10 static record present");

	std::printf("\nexpected 0x0D=%d, 0x20=%d, 0x0C=%d, 0x10=%d authored entities\n",
	            expect_0d, expect_20, expect_0c, expect_10);
	if (g_failures) {
		std::printf("\n%d assertion(s) failed\n", g_failures);
		return 1;
	}
	std::printf("\nPASS: every authored dvxi3 entity reproduced on the wire — "
	            "vehicles (0x0D), markers (0x20), AI (0x0C), and STATICS (0x10) — "
	            "type_id + position + team + heading.\n");
	return 0;
}
