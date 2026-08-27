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

#include <net/npwire/wire_capture.h> // CaptureDatagram / InGameMessage / decode_capture_to_messages
#include <net/npwire/ingame_message_catalog.h> // ingame_message_name

#include <base/pcapio/pcap_reader.h>

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

// Event/state-conditioned messages cannot be required from a passive join
// capture: absence says only that the corresponding death, expiry, deploy,
// target, sound, team-change, or placeable event did not happen in that run.
// Keep this list separate from implementation deferrals. The steady-state
// session/control channels are deliberately NOT here: run 12 proves the full
// integrity trio plus 0x58/0x5D/0x79/0x7E, so losing one is a hard GAP.
// Each conditional wire path still needs its own deterministic regression or
// an induced retail witness; this coverage test must never manufacture events.
const std::map<Key, const char *> kStateConditionedGaps = {
    {{'C', 0x0f}, "entity-self-repair"},  // missing/inconsistent entity query
	{{'S', 0x12}, "entity-expiry"},       // Server_RemoveEntityAndNotify
	{{'S', 0x13}, "entity-death"},        // damage/death routing; unit-pinned by round sim
	{{'S', 0x18}, "entity-self-repair"},  // full entity reply to C2S 0x0F
    {{'S', 0x1e}, "game-event"},         // objective / kill-feed event
    {{'S', 0x26}, "entity-kill"},        // non-player entity kill record
	{{'S', 0x34}, "sound-trigger"},      // mission/gameplay sound
	{{'S', 0x49}, "weapon-reload"},      // player reload request/relay
	// 0x4C and 0x4E are NOT event-conditioned: they answer the C2S 0x23/0x28
	// join-burst requests on every join (§5.33), and the host now ports both
	// replies — losing either is a hard GAP.
    {{'S', 0x50}, "team-change"},        // team assign/change, not plain join
    {{'S', 0x52}, "player-death-stats"}, // two kill-stat reports per player death
    {{'S', 0x59}, "deployed-item"},      // placeable creation/update
    {{'S', 0x6e}, "dead-or-deploying"},  // spawn-wave status; unit-pinned at 1 Hz
    {{'S', 0x81}, "score-delta"},        // positive score feedback
};

// Tags our server emits that retail does not in this golden — opennova<->opennova
// bookkeeping wire-legal but absent from the retail<->retail reference, each with
// its tracked D-NET reference.
const std::map<Key, const char *> kAllowedSpurious = {
    // Our host emits the 0x18 repair record where this retail reference session
    // did not; the extra repair-path emission is the tracked residual.
	{{'S', 0x18}, "D-NET-133"}, // full-entity-spawn (repair-path residual)
	// Run 05 contained a real reload that the shorter retail golden did not.
	// The request/relay path is pinned independently by npruntime_reload_relay.
	{{'S', 0x49}, "weapon-reload"},
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
			if (auto it = kStateConditionedGaps.find(k); it != kStateConditionedGaps.end()) {
				std::snprintf(status, sizeof(status), "absent(state:%s)", it->second);
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

	std::printf("\nGAPS (retail steady-state emits, we don't): %d\n", gaps);
	std::printf("SPURIOUS (we emit S2C, retail doesn't, not allowed): %d\n", spurious);

	if (gaps > 0 || spurious > 0) {
		std::printf("FAIL: coverage diverges from golden — see GAP/SPURIOUS rows "
		            "above. A scenario-only tag belongs in kStateConditionedGaps "
		            "only with a separate event witness/regression.\n");
		return 1;
	}
	std::printf("OK\n");
	return 0;
}
