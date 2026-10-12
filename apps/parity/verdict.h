#pragma once

// The verdict over one parity case: every subject topology (RO, OR, OO) compared
// to RR by every metric (metric.h), each judged against the A/A noise floor.
//
// The A/A rule (ADR 0050 R4): RR-aa is a second retail-vs-retail run of the same
// case. For a metric and key, the subject's distance from RR fails when it
// exceeds max(fixed floor, distance(RR-aa, RR)). The A/A distances themselves form
// the "aa" sanity section, judged against the fixed floors alone: a pass, or
// "widened" where retail's own run-to-run noise exceeds the retired tolerance (the
// floor then widens to it for every subject, and the report says so). A/A rows
// never fail the verdict; every run's evidence findings (run_facts.h) do.

#include "metric.h"
#include "run_facts.h"

#include <cstddef>
#include <string>
#include <vector>

namespace opennova::parity {

struct SubjectRun {
	std::string topology; // "RO", "OR" or "OO"
	const RunFacts *facts = nullptr;
};

struct VerdictRow {
	std::string topology; // the subject's, or "aa"
	MetricValue value;
	bool aa_present = false;
	bool aa_comparable = true;
	double aa_distance = 0.0;
	double threshold = 0.0;
	std::string status; // subject rows: pass, fail, skip; aa rows: pass, widened, skip, short
};

struct EvidenceRow {
	std::string run; // the run's label
	std::string code;
	std::string detail;
};

struct Verdict {
	std::vector<EvidenceRow> evidence;
	std::vector<VerdictRow> aa;
	std::vector<VerdictRow> rows;
	size_t failures = 0; // evidence findings plus failing subject rows
	bool pass() const { return failures == 0; }
};

// Judge every subject against RR with the RR-aa noise floor.
Verdict judge(const RunFacts &rr, const RunFacts &rr_aa, const std::vector<SubjectRun> &subjects);

// The opennova.parity-verdict.v1 document.
std::string verdict_json(const Verdict &verdict, const std::string &items_path, const RunFacts &rr,
		const RunFacts &rr_aa, const std::vector<SubjectRun> &subjects);

} // namespace opennova::parity
