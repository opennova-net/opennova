// Cross-validate the in-game pool decoders (S2C 0x0D pool-entity spawn §5.11,
// S2C 0x20 pool-3 sync §5.12) against the AUTHORED dvxi5 mission — the sibling
// of the /profile .sph check (D-NET-61), extended from pool-0 players to the
// AI/vehicle/marker pools the .sph cannot witness.
//
// The authored mission ("ON RE Probe AS dvxi5", a deliberate probe built in
// ONED) is the ground-truth oracle: fixtures/novaworld/dvxi5_manifest.txt lists
// every entity's type_id / position / team / heading. The host (retail
// Jointops.exe) serialized that mission onto the wire; this test decodes the
// matching loopback capture and asserts, per entity, that the decoded record
// reproduces the authored fact — not merely that the body was consumed (the
// weaker invariant nw_ingame_pool_records_test already covers).
//
// Reads the real pcap DIRECTLY via the shared apps/common pcap reader (no
// hexcap intermediate). Path comes from NW_DVXI5_PCAP, else the in-tree
// .scratch capture (DEFAULT_DVXI5_PCAP). Skips cleanly when the capture is
// absent (it is a local-only artifact — .scratch is untracked), so CI stays
// green; the decoder regression coverage that runs without the capture lives in
// the inline-pcap unit tests (nw_pool_decode_unit_test).

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
#ifndef DEFAULT_DVXI5_PCAP
#define DEFAULT_DVXI5_PCAP ""
#endif

// One authored entity from the manifest (a row of dvxi5_manifest.txt).
struct Expected {
	int wire_tag = 0;     // 0x0D / 0x20 / 0x0C
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
		// strip leading whitespace
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

// Wire BAM heading the engine derives from the authored .mis facing: the
// documented "90 - yaw" convention (the IDA-verified heading fix). facing 0 ->
// 90 deg (0x40000000); facing 90 -> 0 deg; facing 180 -> 270 deg (0xC0000000);
// facing 270 -> 180 deg (0x80000000). The dvxi5 AI (facing 0/90/180/270) pin
// the sign: the 0x20 start markers alone (facing 0/180) cannot distinguish
// "90 - yaw" from "yaw + 90" since they agree there.
uint32_t expected_bam(int facing_deg) {
	int deg = ((90 - facing_deg) % 360 + 360) % 360;
	return uint32_t((uint64_t(deg) << 32) / 360);
}

std::vector<PoolSpawnRecord> g_spawn_0d;     // collected S2C 0x0D records
std::vector<Pool3SyncRecord> g_sync_20;      // collected S2C 0x20 records
std::vector<OrganicSpawnRecord> g_spawn_0c;  // collected S2C 0x0C records (has_body)
int g_0c_batches = 0, g_0c_clean = 0;        // byte-exact-consume witness for 0x0C

// Collect the pool records carried by one decoded S2C message. The outer stack
// (envelope -> NWU -> SCRK -> reassembly) is handled by the shared
// decode_capture_to_messages; this is just the per-tag fan-out.
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
	// --- locate inputs --------------------------------------------------------
	std::string pcap_path;
	if (const char *env = std::getenv("NW_DVXI5_PCAP"); env && *env)
		pcap_path = env;
	else
		pcap_path = DEFAULT_DVXI5_PCAP;

	std::vector<net::PcapDatagram> pkts;
	if (pcap_path.empty() || !net::read_pcap_udp_file(pcap_path, pkts)) {
		std::printf("[skip] dvxi5 capture not found (set NW_DVXI5_PCAP to "
		            "...mission_probe.pcapng) — '%s'\n",
		            pcap_path.c_str());
		return 0;
	}

	std::vector<Expected> manifest;
	const std::string manifest_path = std::string(FIXTURE_DIR) + "/dvxi5_manifest.txt";
	if (!load_manifest(manifest_path, manifest)) {
		std::printf("FAILED to read manifest %s\n", manifest_path.c_str());
		return 1;
	}
	std::printf("loaded %zu authored entities from %s\n", manifest.size(),
	            manifest_path.c_str());
	std::printf("loaded %zu datagrams from %s\n", pkts.size(), pcap_path.c_str());

	// --- decode via the shared pipeline, then collect S2C 0x0D / 0x20 / 0x0C ---
	std::vector<CaptureDatagram> caps;
	caps.reserve(pkts.size());
	for (auto &pk : pkts) caps.push_back({pk.frame_index, pk.srcport, pk.dstport, std::move(pk.payload)});
	for (const auto &m : decode_capture_to_messages(caps))
		if (m.dir == 'S' && !m.settings_update) collect(int(m.tag), m.payload);
	std::printf("collected %zu x 0x0D, %zu x 0x20, %zu x 0x0C records "
	            "(%d/%d 0x0C batches consumed exactly)\n",
	            g_spawn_0d.size(), g_sync_20.size(), g_spawn_0c.size(),
	            g_0c_clean, g_0c_batches);

	// Vehicles re-ground onto terrain, so the height (z) can shift sub-unit from
	// the authored value; x and y (the mission plane) are lossless i32 16.16 and
	// must match exactly. (Markers keep their authored z.)
	constexpr int32_t kZTol = 1 * 0x10000; // 1.0 world unit

	// --- 0x0D pool-entity spawn (vehicles + objective markers) ----------------
	for (const auto &r : g_spawn_0d) {
		if (r.slot_id == 0xFFFF) continue;
		Expected *m = nullptr;
		for (auto &e : manifest)
			if (e.wire_tag == 0x0D && e.type_id == r.item_type_id &&
			    e.pos_x == r.pos_x && e.pos_y == r.pos_y) {
				m = &e;
				break;
			}
		char buf[256];
		std::snprintf(buf, sizeof(buf),
		              "0x0D record type=0x%04x pos=(%d,%d) matches an authored entity",
		              r.item_type_id, r.pos_x, r.pos_y);
		check(m != nullptr, buf);
		if (!m) continue;
		m->matched = true;
		int32_t dz = r.pos_z - m->pos_z;
		if (dz < 0) dz = -dz;
		std::snprintf(buf, sizeof(buf), "%s: z within tol (delta=%.3f)",
		              m->role.c_str(), double(r.pos_z - m->pos_z) / 65536.0);
		check(dz <= kZTol, buf);
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
			    e.pos_x == r.pos_x && e.pos_y == r.pos_y) {
				m = &e;
				break;
			}
		char buf[256];
		std::snprintf(buf, sizeof(buf),
		              "0x20 record type=0x%04x pos=(%d,%d) matches an authored entity",
		              r.item_type_id, r.pos_x, r.pos_y);
		check(m != nullptr, buf);
		if (!m) continue;
		m->matched = true;
		int32_t dz = r.pos_z - m->pos_z;
		if (dz < 0) dz = -dz;
		std::snprintf(buf, sizeof(buf), "%s: z within tol (delta=%.3f)",
		              m->role.c_str(), double(r.pos_z - m->pos_z) / 65536.0);
		check(dz <= kZTol, buf);
		// team (flag 0x08)
		const bool team_gate = (r.flags_byte & 0x08) != 0;
		std::snprintf(buf, sizeof(buf), "%s: team gate set + team_byte==%d (got %d)",
		              m->role.c_str(), m->team, r.team_byte);
		check(team_gate && r.team_byte == m->team, buf);
		// movement_val == BAM(facing + 90) (flag 0x01)
		const bool move_gate = (r.flags_byte & 0x01) != 0;
		const uint32_t exp = expected_bam(m->facing_deg);
		std::snprintf(buf, sizeof(buf),
		              "%s: movement BAM == 0x%08x for facing %d deg (got 0x%08x)",
		              m->role.c_str(), exp, m->facing_deg, r.movement_val);
		check(move_gate && r.movement_val == exp, buf);
		// net_handle carries the marker's authored BMS id
		std::snprintf(buf, sizeof(buf), "%s: net_handle == authored bms_id %d (got %d)",
		              m->role.c_str(), m->bms_id, r.net_handle);
		check(r.net_handle == m->bms_id, buf);
	}

	// --- 0x0C pool-0 organic spawn (the 4 AI infantry, type 0x0816) -----------
	// Every batch must consume exactly (byte-exact witness of the §5.23 field
	// map), and each authored AI must appear with matching type/pos/team/heading.
	check(g_0c_batches > 0, "at least one 0x0C organic spawn batch present");
	check(g_0c_clean == g_0c_batches, "every 0x0C batch consumed exactly (no leftover)");
	for (const auto &r : g_spawn_0c) {
		Expected *m = nullptr;
		for (auto &e : manifest)
			if (e.wire_tag == 0x0C && e.type_id == r.item_type_id &&
			    e.pos_x == r.pos_x && e.pos_y == r.pos_y) {
				m = &e;
				break;
			}
		if (!m) continue; // players (type 0x14B9) ride 0x0C too; not in the manifest
		m->matched = true;
		char buf[256];
		int32_t dz = r.pos_z - m->pos_z;
		if (dz < 0) dz = -dz;
		std::snprintf(buf, sizeof(buf), "%s: z within tol (delta=%.3f)",
		              m->role.c_str(), double(r.pos_z - m->pos_z) / 65536.0);
		check(dz <= kZTol, buf);
		std::snprintf(buf, sizeof(buf), "%s: team == %d (got %d)", m->role.c_str(),
		              m->team, r.team);
		check(r.team == m->team, buf);
		const uint32_t exp = expected_bam(m->facing_deg);
		std::snprintf(buf, sizeof(buf),
		              "%s: orientation BAM == 0x%08x for facing %d deg (got 0x%08x)",
		              m->role.c_str(), exp, m->facing_deg, uint32_t(r.orientation));
		check(uint32_t(r.orientation) == exp, buf);
	}

	// --- every authored entity must have been witnessed -----------------------
	int expect_0d = 0, expect_20 = 0, expect_0c = 0;
	for (const auto &e : manifest) {
		if (e.wire_tag == 0x0D) {
			expect_0d++;
			check(e.matched, "authored 0x0D entity '" + e.role + "' witnessed on the wire");
		} else if (e.wire_tag == 0x20) {
			expect_20++;
			check(e.matched, "authored 0x20 entity '" + e.role + "' witnessed on the wire");
		} else if (e.wire_tag == 0x0C) {
			expect_0c++;
			check(e.matched, "authored 0x0C entity '" + e.role + "' witnessed on the wire");
		}
	}
	check(!g_spawn_0d.empty(), "at least one 0x0D spawn batch present");
	check(!g_sync_20.empty(), "at least one 0x20 sync batch present");

	std::printf("\nexpected 0x0D=%d, 0x20=%d, 0x0C=%d authored entities\n",
	            expect_0d, expect_20, expect_0c);
	if (g_failures) {
		std::printf("\n%d assertion(s) failed\n", g_failures);
		return 1;
	}
	std::printf("\nPASS: every authored dvxi5 vehicle (0x0D) / marker (0x20) / AI "
	            "infantry (0x0C) reproduced on the wire — type_id + position + team "
	            "+ heading.\n");
	return 0;
}
