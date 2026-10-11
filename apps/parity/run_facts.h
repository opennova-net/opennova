#pragma once

// One parity run as the comparator sees it: the run directory a
// scripts/net/run_parity_topology.ps1 cell wrote (run-summary.json beside its
// captures), its evidence capture decoded into the canonical trace
// (apps/wire/parity_trace.h) and bound to the one gameplay session, and the
// windows every metric slices:
//   steady window      [steady_started_utc, + steady_seconds]: the declared slice
//   comparison interval the session's first packet .. the steady window's end
//   initial prefix     each direction's packets through its first gameplay packet

#include "parity_trace.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::parity {

// Which capture of a run is its evidence: the summary's own evidence_capture
// (the retail endpoint's capture for RR/RO/OR, the external one for OO), or the
// external.pcapng every cell records.
enum class EvidenceSource { Summary, External };

struct RunInfo {
	std::string label; // "rr", "rr_aa", "ro", "or", "oo"
	std::string run_dir;
	std::string topology; // the summary's own
	std::string run_id;
	std::string mission;
	std::string game_type;
	std::string integrity_profile; // the corpus's integrity row profile ("" unknown)
	int port = 0;
	std::string capture; // the resolved evidence capture
	std::string external_capture; // the run's external.pcapng ("" when it has none)
	std::string perspective;
	double steady_seconds = 0.0;
	uint64_t steady_start_ns = 0;
	uint64_t steady_end_ns = 0; // start + steady_seconds: the slice end
	uint64_t steady_completed_ns = 0; // when the runner closed the window
};

// Strict ISO-8601 UTC ("2026-10-11T12:34:56.1234567Z", 0..9 fraction digits)
// to nanoseconds since the epoch. False on anything else.
bool parse_utc_ns(std::string_view text, uint64_t &out);

// Read `<run_dir>/run-summary.json`. False with `error` when the summary or its
// evidence capture cannot be read, or a field is missing or malformed.
bool read_run_info(const std::string &run_dir, const std::string &label, EvidenceSource source,
		RunInfo &out, std::string &error);

// A problem with one run's evidence; any fails the verdict.
struct EvidenceFinding {
	std::string code;
	std::string detail;
};

// One message of the run's external capture: wire time, not read time. An onHook
// capture stamps a datagram when the process reads it, so through a blocking load
// its stamps bunch (normalization.h `trace.read_time`); wire timing reads these.
struct WireTimedEvent {
	char dir = '?';
	uint16_t identity = 0; // the 9-bit message identity
	uint64_t ts_ns = 0;
};

struct RunFacts {
	RunInfo info;
	wire::ParityTrace trace; // the bound gameplay session's records only
	int session = 0; // its client UDP port
	uint64_t first_packet_ns = 0;
	double comparison_ms = 0.0; // the comparison interval's length
	std::vector<EvidenceFinding> evidence;
	// The external capture's messages of its own bound gameplay session, through
	// the steady window's end; `external_loaded` false when the run has none.
	bool external_loaded = false;
	std::vector<WireTimedEvent> external_events;

	bool in_comparison(uint64_t ts_ns) const { return ts_ns <= info.steady_end_ns; }
	bool in_steady(uint64_t ts_ns) const {
		return ts_ns >= info.steady_start_ns && ts_ns <= info.steady_end_ns;
	}
};

// Bind `trace` to its one gameplay session, measure the comparison interval and
// check the evidence (the steady window's gameplay coverage, the datagram
// accounting). Never fails: what is wrong lands in `evidence`.
RunFacts make_run_facts(RunInfo info, wire::ParityTrace trace);

// Bind the run's external capture `external` to its own gameplay session and keep
// its message times (`external_events`).
void attach_external(RunFacts &facts, const wire::ParityTrace &external);

// A datagram's direction: its class's, or by port when it has none.
char datagram_direction(const wire::ParityDatagram &datagram, int session);

// Decode `info.capture` and make its facts, then attach `info.external_capture`
// (the same trace when it is the evidence). False with `error` when the evidence
// capture cannot be read; an unreadable external capture leaves the wire-time
// metrics without a sample instead.
bool load_run_facts(const RunInfo &info, const wire::ItemClassMap *item_classes, RunFacts &out,
		std::string &error);

} // namespace opennova::parity
