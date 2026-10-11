#include "verdict.h"

#include "normalization.h"

#include <base/io/json.h>

#include <algorithm>
#include <map>
#include <string>
#include <utility>

namespace opennova::parity {

namespace {

std::string row_key(const MetricValue &value) {
	return std::string(1, value.direction) + "|" + value.metric + "|" + value.key;
}

bool beyond(double distance, double threshold) {
	return distance > threshold + 1e-9 * std::max(1.0, threshold);
}

// The same case on every run: a subject made from another case is not evidence.
void check_case(const RunFacts &rr, const RunFacts &run, std::vector<EvidenceRow> &out) {
	auto differs = [&](const char *what, const std::string &a, const std::string &b) {
		if (a != b)
			out.push_back({ run.info.label, "CASE_MISMATCH",
					std::string(what) + " '" + b + "' differs from RR's '" + a + "'" });
	};
	differs("mission", rr.info.mission, run.info.mission);
	differs("game_type", rr.info.game_type, run.info.game_type);
	differs("port", std::to_string(rr.info.port), std::to_string(run.info.port));
}

void collect_evidence(const RunFacts &run, std::vector<EvidenceRow> &out) {
	for (const EvidenceFinding &finding : run.evidence)
		out.push_back({ run.info.label, finding.code, finding.detail });
}

io::JsonValue number_or_null(bool present, double value) {
	return present ? io::json_number(value) : io::JsonValue::make_null();
}

io::JsonValue row_json(const VerdictRow &row) {
	io::JsonValue json = io::JsonValue::make_object();
	if (row.topology != "aa")
		json.set("topology", io::json_string(row.topology));
	json.set("direction", io::json_string(std::string(1, row.value.direction)));
	json.set("metric", io::json_string(row.value.metric));
	json.set("key", io::json_string(row.value.key));
	json.set("code", io::json_string(row.value.code));
	json.set("status", io::json_string(row.status));
	json.set("distance", number_or_null(row.value.comparable, row.value.distance));
	json.set("floor", io::json_number(row.value.floor));
	json.set("subject", io::json_string(row.value.subject));
	json.set("rr", io::json_string(row.value.reference));
	if (!row.value.detail.empty())
		json.set("detail", io::json_string(row.value.detail));
	if (row.topology != "aa") {
		json.set("aa_distance",
				number_or_null(row.aa_present && row.aa_comparable, row.aa_distance));
		json.set("threshold", io::json_number(row.threshold));
		json.set("widened", io::JsonValue::make_bool(row.threshold > row.value.floor));
	}
	return json;
}

io::JsonValue run_json(const RunFacts &run) {
	io::JsonValue json = io::JsonValue::make_object();
	json.set("run_dir", io::json_string(run.info.run_dir));
	json.set("run_id", io::json_string(run.info.run_id));
	json.set("topology", io::json_string(run.info.topology));
	json.set("mission", io::json_string(run.info.mission));
	json.set("game_type", io::json_string(run.info.game_type));
	json.set("port", io::json_number(run.info.port));
	json.set("capture", io::json_string(run.info.capture));
	json.set("perspective", io::json_string(run.info.perspective));
	json.set("session", io::json_number(run.session));
	// Epoch nanoseconds pass 2^53: a string keeps every digit.
	json.set("steady_start_ns", io::json_string(std::to_string(run.info.steady_start_ns)));
	json.set("steady_seconds", io::json_number(run.info.steady_seconds));
	json.set("comparison_ms", io::json_number(run.comparison_ms));
	io::JsonValue counts = io::JsonValue::make_object();
	counts.set("handshakes", io::json_number(double(run.trace.handshakes.size())));
	counts.set("datagrams", io::json_number(double(run.trace.datagrams.size())));
	counts.set("packets", io::json_number(double(run.trace.packets.size())));
	counts.set("events", io::json_number(double(run.trace.events.size())));
	counts.set("uplinks", io::json_number(double(run.trace.uplinks.size())));
	counts.set("frames", io::json_number(double(run.trace.frames.size())));
	counts.set("entities", io::json_number(double(run.trace.entities.size())));
	json.set("records", std::move(counts));
	return json;
}

} // namespace

const char *mask_kind_name(MaskKind kind) {
	switch (kind) {
		case MaskKind::HandshakeField:
			return "handshake_field";
		case MaskKind::HandshakeCu:
			return "handshake_cu";
		case MaskKind::InitialBody:
			return "initial_body";
		case MaskKind::BodyPrefix:
			return "body_prefix";
		case MaskKind::FirstOrder:
			return "first_order";
		case MaskKind::Field:
			return "field";
	}
	return "field";
}

Verdict judge(const RunFacts &rr, const RunFacts &rr_aa, const std::vector<SubjectRun> &subjects) {
	Verdict verdict;
	collect_evidence(rr, verdict.evidence);
	collect_evidence(rr_aa, verdict.evidence);
	check_case(rr, rr_aa, verdict.evidence);
	for (const SubjectRun &subject : subjects) {
		collect_evidence(*subject.facts, verdict.evidence);
		check_case(rr, *subject.facts, verdict.evidence);
	}

	CompareOptions aa_options;
	aa_options.host_rows = true;
	std::map<std::string, const MetricValue *> aa_index;
	const std::vector<MetricValue> aa_values = compare_runs(rr, rr_aa, aa_options);
	for (const MetricValue &value : aa_values) {
		VerdictRow row;
		row.topology = "aa";
		row.value = value;
		row.threshold = value.floor;
		if (!value.comparable)
			row.status = value.reference_short ? "skip" : "short";
		else
			row.status = beyond(value.distance, value.floor) ? "widened" : "pass";
		verdict.aa.push_back(std::move(row));
	}
	for (const MetricValue &value : aa_values)
		aa_index.emplace(row_key(value), &value);

	for (const SubjectRun &subject : subjects) {
		CompareOptions options;
		options.host_rows = subject.topology == "OR" || subject.topology == "OO";
		for (MetricValue &value : compare_runs(rr, *subject.facts, options)) {
			VerdictRow row;
			row.topology = subject.topology;
			const auto aa = aa_index.find(row_key(value));
			if (aa != aa_index.end()) {
				row.aa_present = true;
				row.aa_comparable = aa->second->comparable;
				row.aa_distance = aa->second->distance;
			}
			row.threshold = value.floor;
			if (row.aa_present && row.aa_comparable)
				row.threshold = std::max(value.floor, row.aa_distance);
			if (!value.comparable) {
				if (value.reference_short)
					row.status = "skip";
				else
					row.status = row.aa_present && !row.aa_comparable ? "pass" : "fail";
			} else {
				row.status = beyond(value.distance, row.threshold) ? "fail" : "pass";
			}
			row.value = std::move(value);
			if (row.status == "fail")
				++verdict.failures;
			verdict.rows.push_back(std::move(row));
		}
	}
	verdict.failures += verdict.evidence.size();
	return verdict;
}

std::string verdict_json(const Verdict &verdict, const std::string &items_path, const RunFacts &rr,
		const RunFacts &rr_aa, const std::vector<SubjectRun> &subjects) {
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema", io::json_string("opennova.parity-verdict.v1"));
	json.set("status", io::json_string(verdict.pass() ? "PASS" : "FAIL"));
	json.set("failures", io::json_number(double(verdict.failures)));
	json.set("items", io::json_string(items_path));
	json.set("rule",
			io::json_string("a subject's distance from RR fails beyond max(fixed "
							"floor, the RR-aa distance from RR)"));

	io::JsonValue runs = io::JsonValue::make_object();
	runs.set("rr", run_json(rr));
	runs.set("rr_aa", run_json(rr_aa));
	for (const SubjectRun &subject : subjects) {
		std::string label = subject.topology;
		std::transform(label.begin(), label.end(), label.begin(),
				[](char c) { return char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
		runs.set(label, run_json(*subject.facts));
	}
	json.set("runs", std::move(runs));

	io::JsonValue registry = io::JsonValue::make_array();
	for (const Normalization &entry : kNormalizations) {
		io::JsonValue item = io::JsonValue::make_object();
		item.set("id", io::json_string(entry.id));
		item.set("kind", io::json_string(mask_kind_name(entry.kind)));
		item.set("field", io::json_string(entry.name));
		item.set("reason", io::json_string(entry.reason));
		registry.push(std::move(item));
	}
	json.set("normalizations", std::move(registry));

	io::JsonValue evidence = io::JsonValue::make_array();
	for (const EvidenceRow &row : verdict.evidence) {
		io::JsonValue item = io::JsonValue::make_object();
		item.set("run", io::json_string(row.run));
		item.set("code", io::json_string(row.code));
		item.set("detail", io::json_string(row.detail));
		evidence.push(std::move(item));
	}
	json.set("evidence", std::move(evidence));

	io::JsonValue aa = io::JsonValue::make_array();
	size_t widened = 0;
	for (const VerdictRow &row : verdict.aa) {
		widened += row.status == "widened" ? 1 : 0;
		aa.push(row_json(row));
	}
	json.set("aa", std::move(aa));
	json.set("aa_widened", io::json_number(double(widened)));

	io::JsonValue rows = io::JsonValue::make_array();
	std::map<std::string, std::pair<size_t, size_t>> per_topology; // rows, failures
	for (const VerdictRow &row : verdict.rows) {
		auto &tally = per_topology[row.topology];
		++tally.first;
		tally.second += row.status == "fail" ? 1 : 0;
		rows.push(row_json(row));
	}
	json.set("rows", std::move(rows));
	io::JsonValue summary = io::JsonValue::make_object();
	for (const auto &entry : per_topology) {
		io::JsonValue item = io::JsonValue::make_object();
		item.set("rows", io::json_number(double(entry.second.first)));
		item.set("failures", io::json_number(double(entry.second.second)));
		summary.set(entry.first, std::move(item));
	}
	json.set("summary", std::move(summary));
	return io::json_write(json);
}

} // namespace opennova::parity
