// Golden coverage diff — compare OUR freshly captured retail-join session against
// a golden retail<->retail capture, by message coverage per (direction, wire tag).
//
// This is the CI-gated counterpart to scripts/net/diff_vs_golden.ps1: it asserts
// that the set of in-match message TYPES our server emits/receives matches what
// retail does, so a regression that drops (or spuriously adds) a tag fails the
// build. It is a COVERAGE diff, not a byte diff — per-frame contents (positions,
// ticks, SCRK, seq) legitimately differ between two different sessions, so we
// compare which (dir,tag) flow, not their bytes. Byte/field correctness is held
// by the existing groundtruth tests (nw_dvxc1_groundtruth, nw_ingame_pool_records,
// npruntime_golden_*).
//
// Both captures decode through the SAME shared pipeline the client uses
// (decode_capture_to_messages: envelope CRC -> NWU -> per-session SCRK ->
// 0x43/0x83 -> reassembly), so the comparison is ground truth.
//
// Gating:
//   NW_GOLDEN_OURS      our capture (.pcap/.pcapng) — REQUIRED; skip clean if unset
//                       (there is no committed "ours" oracle; it is produced live
//                       by the capture harness, then this test re-validated against it)
//   NW_GOLDEN_GAMEPLAY  the golden (defaults to .scratch/golden/retail-gameplay-session.pcapng)
//
// A GAP (retail emits a tag above the noise floor, we never do) or a SPURIOUS S2C
// tag (we emit a tag retail never does) outside the documented allowlists fails.
// The allowlists are the explicit, reviewable record of what is knowingly
// deferred — they are NOT a way to paper over a real regression.

#include <npwire/wire_capture.h> // CaptureDatagram / InGameMessage / decode_capture_to_messages
#include <npwire/ingame_message_catalog.h> // ingame_message_name

#include <pcapio/pcap_reader.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifndef DEFAULT_GAMEPLAY_PCAP
#define DEFAULT_GAMEPLAY_PCAP ""
#endif

using namespace opennova;

namespace {

using Key = std::pair<char, uint8_t>; // (dir, low-byte tag)

// A golden tag seen fewer than this many times is noise (rare one-off control
// messages) and not required of our capture.
constexpr int kNoiseFloor = 1;

// Tags retail emits that our server knowingly does not yet — each carries its
// tracked D-NET reference so the diff names the deferral by ID (populated
// 2026-07-05 from the attested v35 baseline: game-server retail_join_v35_game
// vs the retail gameplay golden). Kept here (not silently skipped) so the
// deferral is reviewable and a NEW gap fails the diff. Nearly all belong to
// D-NET-163 — the dev golden-harness host (`nw_server`, ADR 0013, never
// shipped) is a minimal in-match harness, and a join-scope capture never
// exercises the periodic anti-cheat challenges, the gameplay-event traffic
// (kills/scores/sounds), or the low-frequency session/roster control tags that
// a full retail<->retail session carries. Format: {dir, tag} -> "D-NET-nnn".
const std::map<Key, const char *> kDeferredGaps = {
    // Periodic integrity / anti-cheat challenge-response pairs (harness drives none).
    {{'C', 0x08}, "D-NET-163"}, // time-sync-reply (no S2C 0x43 ping to answer)
    {{'S', 0x43}, "D-NET-163"}, // time-sync-ping
    {{'C', 0x1c}, "D-NET-163"}, // charattr-crc-reply (no S2C 0x39 challenge)
    {{'S', 0x39}, "D-NET-163"}, // charattr-crc-challenge
    {{'C', 0x20}, "D-NET-163"}, // entity-checksum-reply (no S2C 0x30 request)
    {{'S', 0x30}, "D-NET-163"}, // entity-checksum-req
    {{'C', 0x21}, "D-NET-163"}, // loadout-checksum-reply (no S2C 0x31 request)
    {{'S', 0x31}, "D-NET-163"}, // loadout-crc-req
    {{'C', 0x3d}, "D-NET-163"}, // loaded-model page (no S2C 0x68 request)
    {{'S', 0x68}, "D-NET-163"}, // loaded-model page request
    // Gameplay-event traffic a join-only capture never produces (no kills/scores).
    {{'S', 0x26}, "D-NET-163"}, // kill-sync
    {{'S', 0x4e}, "D-NET-163"}, // kill-by-slot
    {{'S', 0x34}, "D-NET-163"}, // play-sound
    {{'S', 0x81}, "D-NET-163"}, // score-delta-sound
    // Low-frequency session / roster / control tags the harness does not model.
    {{'S', 0x4c}, "D-NET-163"}, // target-assignment
    {{'S', 0x50}, "D-NET-163"}, // team-assign (Server_AssignPlayerTeam MATCHING, not driven here)
    {{'S', 0x58}, "D-NET-163"}, // session-status
    {{'S', 0x59}, "D-NET-163"}, // deployed-item
    {{'S', 0x5d}, "D-NET-163"}, // destroy-list (0x5D _DestroyEntityList @ 0x429730)
    {{'S', 0x79}, "D-NET-163"}, // spectator-flag
    {{'S', 0x7e}, "D-NET-163"}, // server-config-strings
};

// Tags our server emits that retail does not in this golden — opennova<->opennova
// bookkeeping wire-legal but absent from the retail<->retail reference, each with
// its tracked D-NET reference.
const std::map<Key, const char *> kAllowedSpurious = {
    // Our host emits the 0x18 repair record where this retail reference session
    // did not; the extra repair-path emission is the tracked residual.
    {{'S', 0x18}, "D-NET-133"}, // full-entity-spawn (repair-path residual)
};

std::map<Key, long> histogram(const std::vector<net::PcapDatagram> &pkts) {
	std::vector<CaptureDatagram> caps;
	caps.reserve(pkts.size());
	for (const net::PcapDatagram &p : pkts) {
		CaptureDatagram c;
		c.frame_index = p.frame_index;
		c.src_port = p.srcport;
		c.dst_port = p.dstport;
		c.payload = p.payload;
		caps.push_back(std::move(c));
	}
	std::map<Key, long> h;
	for (const InGameMessage &m : decode_capture_to_messages(caps))
		h[{m.dir, static_cast<uint8_t>(m.tag & 0xFF)}] += 1;
	return h;
}

const char *name_of(Key k) {
	const char *n = ingame_message_name(k.first, k.second);
	return n ? n : "?";
}

} // namespace

int main() {
	const char *ours_path = std::getenv("NW_GOLDEN_OURS");
	if (!ours_path || !*ours_path) {
		std::printf("[skip] set NW_GOLDEN_OURS to a freshly captured retail-join "
		            ".pcapng to run the golden coverage diff\n");
		return 0; // skip clean — ours is produced live, never committed
	}
	std::string golden_path;
	if (const char *env = std::getenv("NW_GOLDEN_GAMEPLAY"); env && *env)
		golden_path = env;
	else
		golden_path = DEFAULT_GAMEPLAY_PCAP;

	std::vector<net::PcapDatagram> ours_pkts, gold_pkts;
	if (!net::read_pcap_udp_file(ours_path, ours_pkts)) {
		std::printf("FAILED to read NW_GOLDEN_OURS=%s\n", ours_path);
		return 1;
	}
	if (golden_path.empty() || !net::read_pcap_udp_file(golden_path, gold_pkts)) {
		std::printf("[skip] golden capture not found (set NW_GOLDEN_GAMEPLAY) — '%s'\n",
		            golden_path.c_str());
		return 0; // skip clean — gitignored golden absent
	}

	const std::map<Key, long> ours = histogram(ours_pkts);
	const std::map<Key, long> gold = histogram(gold_pkts);
	if (ours.empty()) {
		std::printf("FAIL: our capture decoded to zero messages (empty or handshake "
		            "missing — SCRK unrecoverable)\n");
		return 1;
	}

	// Union of keys for the printed table.
	std::set<Key> keys;
	for (const auto &kv : ours) keys.insert(kv.first);
	for (const auto &kv : gold) keys.insert(kv.first);

	std::printf("%-3s %-6s %-22s %8s %8s  %s\n", "dir", "tag", "name", "ours",
	            "golden", "status");
	int gaps = 0, spurious = 0;
	for (Key k : keys) {
		const long oc = ours.count(k) ? ours.at(k) : 0;
		const long gc = gold.count(k) ? gold.at(k) : 0;
		char status[48] = "OK";
		if (oc == 0 && gc >= kNoiseFloor) {
			if (auto it = kDeferredGaps.find(k); it != kDeferredGaps.end()) {
				std::snprintf(status, sizeof(status), "GAP(deferred %s)", it->second);
			} else {
				std::snprintf(status, sizeof(status), "GAP");
				++gaps;
			}
		} else if (oc == 0 && gc > 0) {
			std::snprintf(status, sizeof(status), "gap(noise)");
		} else if (gc == 0 && oc > 0) {
			// Only S2C spurious tags are a hard failure: a stray C2S tag is the
			// retail client's choice, not our server's. S2C is what WE emit.
			if (auto it = kAllowedSpurious.find(k); k.first == 'S' && it == kAllowedSpurious.end()) {
				std::snprintf(status, sizeof(status), "SPURIOUS");
				++spurious;
			} else if (k.first == 'S') {
				std::snprintf(status, sizeof(status), "extra(allowed %s)", it->second);
			} else {
				std::snprintf(status, sizeof(status), "extra");
			}
		}
		char tagbuf[8];
		std::snprintf(tagbuf, sizeof(tagbuf), "0x%02x", k.second);
		std::printf("%-3c %-6s %-22s %8ld %8ld  %s\n", k.first, tagbuf, name_of(k),
		            oc, gc, status);
	}

	std::printf("\nGAPS (retail emits, we don't, non-deferred): %d\n", gaps);
	std::printf("SPURIOUS (we emit S2C, retail doesn't, not allowed): %d\n", spurious);

	if (gaps > 0 || spurious > 0) {
		std::printf("FAIL: coverage diverges from golden — see GAP/SPURIOUS rows "
		            "above. Implement the gap (and clear it from the worklist), or, "
		            "if intentionally deferred, add it to kDeferredGaps with a "
		            "D-NET reference.\n");
		return 1;
	}
	std::printf("OK\n");
	return 0;
}
