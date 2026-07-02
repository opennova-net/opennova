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

#include <novaworld/wire_capture.h> // CaptureDatagram / InGameMessage / decode_capture_to_messages
#include <novaworld/ingame_message_catalog.h> // ingame_message_name

#include "pcap_reader.h"

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

// Tags retail emits that our server knowingly does not yet — each is tracked as a
// D-NET divergence and implemented in a later pass. Empty until the first baseline
// capture populates it from the real diff; kept here (not silently skipped) so the
// deferral is reviewable. Format: {dir, tag}.
const std::set<Key> kDeferredGaps = {
    // e.g. {'S', 0x45}, // terrain-load — headless host has no terrain stream (D-NET-81)
};

// Tags our server emits that retail does not in this golden — e.g. opennova<->opennova
// bookkeeping that is wire-legal but absent from the retail<->retail reference. Empty
// until a baseline shows a justified case.
const std::set<Key> kAllowedSpurious = {};

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
		const char *status = "OK";
		if (oc == 0 && gc >= kNoiseFloor) {
			if (kDeferredGaps.count(k)) {
				status = "GAP(deferred)";
			} else {
				status = "GAP";
				++gaps;
			}
		} else if (oc == 0 && gc > 0) {
			status = "gap(noise)";
		} else if (gc == 0 && oc > 0) {
			// Only S2C spurious tags are a hard failure: a stray C2S tag is the
			// retail client's choice, not our server's. S2C is what WE emit.
			if (k.first == 'S' && !kAllowedSpurious.count(k)) {
				status = "SPURIOUS";
				++spurious;
			} else {
				status = "extra";
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
