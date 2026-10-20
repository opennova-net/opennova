#pragma once

// The metrics one run is compared to the same case's RR run by (ADR 0050 R4). Each
// yields a distance from RR and the fixed floor the retired PowerShell verifier
// used as its tolerance; the verdict (verdict.h) widens the floor to the A/A
// distance (RR-aa vs RR) of the same metric and key, so only distance beyond the
// retail-vs-retail noise is a divergence.
//
// Distances by metric family:
//   presence, boolean shape checks       0 or 1, floor 0
//   exact sets                           Jaccard distance (1 - |A n B| / |A u B|), floor 0
//   counts and cadences                  |subject - RR rate-normalized|, floor max(1, ceil 20%)
//   rates (bandwidth, gameplay cadence)  |subject / RR - 1|, floor 0.20
//   share distributions                  max over keys of |share delta| / max(0.08, RR/2),
//                                        floor 1 (keys under a 2% share on both sides skip)
//   orders and sequences                 inversions / edit distance, floor 0
//   dynamic vectors                      |log2(subject / RR)|, floor 3 (the 1/8x..8x band)
//   spatial baselines                    the largest axis delta, floor 64 world units

#include "run_facts.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace opennova::parity {

struct MetricValue {
	char direction = 'B'; // 'C', 'S', or 'B' (the session as a whole)
	std::string metric;
	std::string key;
	std::string code; // the finding code a failure reports
	double distance = 0.0;
	double floor = 0.0;
	// False when a side has too few samples to compare. With RR short the metric
	// is skipped; with the compared run short it fails unless the A/A was too.
	bool comparable = true;
	bool reference_short = false;
	std::string subject; // the compared run's value, rendered
	std::string reference; // RR's value, rendered
	std::string detail;
};

struct CompareOptions {
	// The compared run's host is OpenNova (OR, OO; the A/A computes it too): its
	// integrity challenge rows must be RR's own.
	bool host_rows = false;
	// The compared run's host is retail (RR-aa, RO, an RR subject): a reply it did
	// not punt is witnessed even for a row RR never challenged.
	bool retail_host = false;
	// Another retail run of the same integrity profile (RR-aa) whose valid replies
	// join RR's row->CRC oracle.
	const RunFacts *oracle_pool = nullptr;
};

// Every metric of `other` against the reference `rr`, both directions.
std::vector<MetricValue> compare_runs(
		const RunFacts &rr, const RunFacts &other, const CompareOptions &options);

// The families (metric_wire.cpp, metric_initial.cpp, metric_state.cpp).
void compare_wire(const RunFacts &rr, const RunFacts &other, const CompareOptions &options,
		std::vector<MetricValue> &out);
// The initial reliable stream of one direction: its grouping, its bodies by
// position, as sets and as decoded records.
void compare_initial(
		const RunFacts &rr, const RunFacts &other, char dir, std::vector<MetricValue> &out);
void compare_state(const RunFacts &rr, const RunFacts &other, std::vector<MetricValue> &out);

// --- shared helpers (metric_math.cpp)

MetricValue make_metric(char dir, std::string name, std::string key, const char *code);

// A 9-bit message identity rendered "0x0a:0" (tag : settings bit).
std::string identity_key(uint16_t identity);
uint16_t event_identity(const wire::ParityEvent &event);
// The direction's gameplay identity: C2S 0x0C, S2C 0x0A.
uint16_t gameplay_identity(char dir);
// A stable body's comparison key: the registered prefix (S2C 0x0F's live tick)
// stripped; S2C 0x64's session-block random bytes and S2C 0x0D's bobbing-hull z zeroed
// (body.S.0x64.session_block, body.S.0x0d.hull_bob).
std::string stable_body_key(char dir, uint16_t identity, const std::string &raw);
// One physical record's framing key; a registered grouping length renders `len=*`.
std::string wire_key(char dir, const wire::ParityWireRecord &record);
// A packet's grouping key: its flags, identities and framing.
std::string group_key(const wire::ParityPacket &packet);
bool carries_low_tag(const wire::ParityPacket &packet, uint16_t identity);
bool carries_identity(const wire::ParityPacket &packet, uint16_t identity);
double jaccard_distance(const std::set<std::string> &a, const std::set<std::string> &b);
// Pairwise order inversions between the items `a` and `b` share.
size_t order_inversions(const std::vector<std::string> &a, const std::vector<std::string> &b);
size_t edit_distance(const std::vector<std::string> &a, const std::vector<std::string> &b);
std::string render_set(const std::set<std::string> &values, size_t cap = 240);
std::string render_list(const std::vector<std::string> &values, size_t cap = 240);
std::string render_number(double value);
std::string hex_of(const std::vector<uint8_t> &bytes, size_t cap = 0);
// |subject / reference - 1|, or 0/1 when the reference is zero.
double relative_distance(double subject, double reference);
// The count rule: RR's count rate-normalized to the compared run's duration when RR
// has at least 20, with the max(1, ceil 20%) floor.
void count_metric(
		MetricValue &value, double rr_count, double rr_ms, double other_count, double other_ms);
// The nearest-rank 90th percentile (the ceil(0.9 n)-th smallest): two samples give
// the larger, never the minimum. 0 for none.
double p90(std::vector<double> values);

} // namespace opennova::parity
