// Multi-client lifecycle + protocol-volume witness for the `probe3_again` capture
// — the richer re-run of the dvxc1 Co-op probe (same authored mission, so the
// entity manifest is unchanged and re-validated by nw_dvxc1_groundtruth_test).
// This test asserts the NEW dimensions probe3_again adds: 3 human players, a
// weapon-heavy session (260 C2S 0x06 fires across 15 adm indices, 2 shooters),
// the high-volume transport channels (RTT 0x57/0x2C ~10.7k each + the periodic
// request trio 0x68/0x43/0x39 -> C2S 0x3D/0x08/0x1C), the deployed-item channel
// (0x59), the multi-client death/leave lifecycle (0x26+0x13 deaths, 0x5D clean
// despawn), AND the honest CONFIRMED NEGATIVES (no guided 0x0C sub_ops, AI static
// so mid-game 0x20 is the load batch only, Co-op roster teams==0).
//
// The distinguishing assertion cross-checks the wire against the host /PROFILE
// .sph value-oracle (decode_server_log): the .sph DEATH/DISCONNECT events and the
// 3-player roster must line up with the wire lifecycle tags.
//
// Reads the pcap directly via the shared apps/common pcap reader; path from
// NW_PROBE3AGAIN_PCAP else DEFAULT_PROBE3AGAIN_PCAP. The host .sph from
// NW_PROBE3AGAIN_HOST_SPH else DEFAULT_PROBE3AGAIN_HOST_SPH. Skips cleanly when
// the capture is absent (.scratch is untracked) so CI stays green; the .sph
// cross-check is skipped independently if its file is absent.

#include <npwire/ingame_decode.h>
#include <npwire/serverlog_decode.h>
#include <npwire/wire_capture.h>

#include <pcapio/pcap_reader.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace opennova;

namespace {

#ifndef DEFAULT_PROBE3AGAIN_PCAP
#define DEFAULT_PROBE3AGAIN_PCAP ""
#endif
#ifndef DEFAULT_PROBE3AGAIN_HOST_SPH
#define DEFAULT_PROBE3AGAIN_HOST_SPH ""
#endif

// --- wire collectors ---------------------------------------------------------
int n_s57 = 0, n_c2c = 0;                 // RTT ping/pong
int n_s68 = 0, n_c3d = 0;                 // loaded-model page req / reply
int n_s43 = 0, n_c08 = 0;                 // time-sync ping / reply
int n_s39 = 0, n_c1c = 0;                 // charattr-row challenge / reply
int n_06 = 0;                             // C2S fired-round
int n_5d = 0;                             // S2C destroy-list (clean despawn)
int n_59 = 0;                             // S2C deployed-item
int n_s20 = 0;                            // S2C pool-3 sync batches
int n_26 = 0, n_13 = 0;                   // death paths
int n_16 = 0, n_46 = 0, n_6e = 0;         // lifecycle/identity
int guided_sub_ops = 0;                   // C2S 0x0C with sub_op != 0x0A (guided)
int total_c0c = 0;                        // C2S 0x0C total
size_t max_roster_teams = 0;              // largest teamCount seen on 0x6E
std::set<int> adm_set;                    // distinct weapon adm indices (0x06)
std::set<uint16_t> shooter_set;           // distinct shooter handles (0x06)
std::set<std::string> names_7b;           // distinct player names (0x7B)

void collect(char dir, int tag, const std::vector<uint8_t> &a) {
	if (dir == 'S') {
		switch (tag) {
		case 0x57: n_s57++; break;
		case 0x68: n_s68++; break;
		case 0x43: n_s43++; break;
		case 0x39: n_s39++; break;
		case 0x5D: n_5d++; break;
		case 0x59: n_59++; break;
		case 0x20: n_s20++; break;
		case 0x26: n_26++; break;
		case 0x13: n_13++; break;
		case 0x16: n_16++; break;
		case 0x46: n_46++; break;
		case 0x6E: {
			n_6e++;
			RosterSync rs;
			if (decode_roster_sync(a.data(), a.size(), rs) &&
			    rs.teams.size() > max_roster_teams)
				max_roster_teams = rs.teams.size();
			break;
		}
		case 0x7B: {
			FullPlayerInfo fi;
			if (decode_full_player_info(a.data(), a.size(), fi) && !fi.player_name.empty())
				names_7b.insert(fi.player_name);
			break;
		}
		default: break;
		}
	} else { // 'C'
		switch (tag) {
		case 0x2C: n_c2c++; break;
		case 0x3D: n_c3d++; break;
		case 0x08: n_c08++; break;
		case 0x1C: n_c1c++; break;
		case 0x06: {
			n_06++;
			ClientFiredRound fr;
			size_t used = 0;
			if (decode_client_fired_round(a.data(), a.size(), fr, used)) {
				adm_set.insert(fr.adm_index);
				shooter_set.insert(fr.shooter_handle);
			}
			break;
		}
		case 0x0C: {
			total_c0c++;
			EntityPacketSubHeader sh;
			size_t used = 0;
			if (decode_entity_packet_sub_header(a.data(), a.size(), sh, used) &&
			    sh.sub_op != 0x0A)
				guided_sub_ops++;
			break;
		}
		default: break;
		}
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
	if (const char *env = std::getenv("NW_PROBE3AGAIN_PCAP"); env && *env)
		pcap_path = env;
	else
		pcap_path = DEFAULT_PROBE3AGAIN_PCAP;

	std::vector<net::PcapDatagram> pkts;
	if (pcap_path.empty() || !net::read_pcap_udp_file(pcap_path, pkts)) {
		std::printf("[skip] probe3_again capture not found (set NW_PROBE3AGAIN_PCAP "
		            "to ...probe3_again.pcapng) — '%s'\n", pcap_path.c_str());
		return 0;
	}
	std::printf("loaded %zu datagrams from %s\n", pkts.size(), pcap_path.c_str());

	std::vector<CaptureDatagram> caps;
	caps.reserve(pkts.size());
	for (auto &pk : pkts)
		caps.push_back({pk.frame_index, pk.srcport, pk.dstport, std::move(pk.payload)});
	for (const auto &m : decode_capture_to_messages(caps)) {
		if (m.settings_update) continue;
		collect(m.dir, int(m.tag), m.payload);
	}

	std::printf("RTT: 0x57=%d 0x2C=%d | trio: 0x68=%d/0x3D=%d 0x43=%d/0x08=%d "
	            "0x39=%d/0x1C=%d\n", n_s57, n_c2c, n_s68, n_c3d, n_s43, n_c08,
	            n_s39, n_c1c);
	std::printf("lifecycle: deaths 0x26=%d 0x13=%d | despawn 0x5D=%d | players(0x7B)=%zu "
	            "0x16=%d 0x46=%d 0x6E=%d (max teams=%zu)\n", n_26, n_13, n_5d,
	            names_7b.size(), n_16, n_46, n_6e, max_roster_teams);
	std::printf("weapons: 0x06=%d fires, %zu distinct adm, %zu shooters | 0x59 deployed=%d\n",
	            n_06, adm_set.size(), shooter_set.size(), n_59);
	std::printf("negatives: C2S 0x0C=%d (guided sub_ops=%d) | S2C 0x20=%d batches\n",
	            total_c0c, guided_sub_ops, n_s20);

	// --- high-volume transport channels (Wave 1) -----------------------------
	check(n_s57 > 5000 && n_c2c > 5000, "RTT ping/pong is high-volume (>5000 each)");
	int rtt_diff = n_s57 - n_c2c; if (rtt_diff < 0) rtt_diff = -rtt_diff;
	check(rtt_diff <= 4, "RTT 0x57 and C2S 0x2C counts pair (within 4)");
	check(n_s68 > 0 && n_c3d > 0, "loaded-model req 0x68 <-> page C2S 0x3D co-present");
	check(n_s43 > 0 && n_c08 > 0, "time-sync ping 0x43 <-> reply C2S 0x08 co-present");
	check(n_s39 > 0 && n_c1c > 0, "charattr-row challenge 0x39 <-> reply C2S 0x1C co-present");

	// --- multi-client identity + lifecycle (Wave 4) --------------------------
	check(names_7b.size() >= 2, "at least 2 distinct players named via S2C 0x7B");
	check(n_16 > 0 && n_46 > 0, "player-list 0x16 + player-sync 0x46 present");
	check((n_26 + n_13) > 0, "death events present (0x26 kill-sync and/or 0x13)");
	check(n_5d > 0, "S2C 0x5D clean-despawn (destroy-list) present");

	// --- weapon variety (Wave 5 data) ----------------------------------------
	check(n_06 > 100, "weapon-heavy session: >100 C2S 0x06 fired-round events");
	check(adm_set.size() >= 5, "at least 5 distinct weapon adm indices fired");
	check(shooter_set.count(0x0006) && shooter_set.count(0x0007),
	      "both human shooters (handles 0x0006 + 0x0007) fired");
	check(n_59 >= 1, "deployed-item 0x59 present");

	// --- CONFIRMED NEGATIVES (honest scope; D-NET-55 / D-NET-64 / Co-op) ------
	check(total_c0c > 0, "C2S 0x0C entity uploads present");
	check(guided_sub_ops == 0,
	      "NO guided traffic: every C2S 0x0C sub_op is 0x0A (D-NET-64 still open)");
	check(n_s20 <= 4,
	      "mid-game S2C 0x20 is the load batch only — AI static (D-NET-55 still open)");
	check(max_roster_teams == 0, "Co-op roster 0x6E teams==0 (single team vs AI)");

	// === .sph value-oracle cross-check =======================================
	std::string sph_path;
	if (const char *env = std::getenv("NW_PROBE3AGAIN_HOST_SPH"); env && *env)
		sph_path = env;
	else
		sph_path = DEFAULT_PROBE3AGAIN_HOST_SPH;
	std::ifstream sf(sph_path, std::ios::binary);
	if (!sf) {
		std::printf("[note] host .sph not found (%s) — skipping the value-oracle "
		            "cross-check\n", sph_path.c_str());
	} else {
		std::vector<uint8_t> data((std::istreambuf_iterator<char>(sf)),
		                          std::istreambuf_iterator<char>());
		ServerLogDocument doc;
		decode_server_log(data.data(), data.size(), doc);
		int sph_deaths = 0, sph_disconnects = 0;
		for (const auto &e : doc.events) {
			if (e.kind == ServerLogEventKind::Death) sph_deaths++;
			else if (e.kind == ServerLogEventKind::Disconnect) sph_disconnects++;
		}
		bool all_blue = !doc.roster.empty();
		for (const auto &p : doc.roster) if (p.team != 1) all_blue = false;
		std::printf(".sph oracle: roster=%zu deaths=%d disconnects=%d (mission=%s)\n",
		            doc.roster.size(), sph_deaths, sph_disconnects, doc.mission.c_str());

		check(doc.roster.size() == 3, ".sph host roster has 3 players");
		check(all_blue, ".sph roster all Blue (Co-op single team)");
		check(sph_deaths >= 5, ".sph records the session's deaths (>=5)");
		check(sph_disconnects == 2, ".sph records 2 clean disconnects");
		// the distinguishing cross-check: clean disconnects <-> wire 0x5D despawn.
		check(n_5d == sph_disconnects,
		      "wire 0x5D despawn count matches the .sph disconnect count");
		// the wire carries at least as many death notifications as the .sph logs.
		check((n_26 + n_13) >= sph_deaths,
		      "wire death tags (0x26+0x13) cover the .sph death count");
	}

	if (g_failures) {
		std::printf("\n%d assertion(s) failed\n", g_failures);
		return 1;
	}
	std::printf("\nPASS: probe3_again multi-client lifecycle + protocol volume witnessed — "
	            "RTT/trio channels, 3 players, weapon variety, 0x59 deployed-item, "
	            "death/despawn lifecycle cross-validated vs the .sph oracle; guided + "
	            "mid-game 0x20 confirmed absent.\n");
	return 0;
}
