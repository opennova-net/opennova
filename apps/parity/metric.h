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
};

// Every metric of `other` against the reference `rr`, both directions.
std::vector<MetricValue> compare_runs(
		const RunFacts &rr, const RunFacts &other, const CompareOptions &options);

// The two families (metric_wire.cpp, metric_state.cpp).
void compare_wire(const RunFacts &rr, const RunFacts &other, const CompareOptions &options,
		std::vector<MetricValue> &out);
void compare_state(const RunFacts &rr, const RunFacts &other, std::vector<MetricValue> &out);

// --- shared helpers (metric_math.cpp)

// A 9-bit message identity rendered "0x0a:0" (tag : settings bit).
std::string identity_key(uint16_t identity);
uint16_t event_identity(const wire::ParityEvent &event);
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

} // namespace opennova::parity
