#include "cli.h"

#include "item_tables.h"
#include "run_facts.h"
#include "verdict.h"

#include <base/io/file_io.h>

#include <map>
#include <memory>
#include <string>
#include <utility>

namespace opennova::parity {

namespace {

constexpr const char *kUsage =
		"usage: opennova-parity --items <items.def> --rr <runDir> --rr-aa <runDir>\n"
		"                       [--ro <runDir>] [--or <runDir>] [--oo <runDir>]\n"
		"                       [--out <verdict.json>] [--evidence summary|external]\n"
		"  Each runDir is a scripts/net/run_parity_topology.ps1 cell (run-summary.json\n"
		"  beside its captures). Every subject topology is compared to RR, both\n"
		"  directions; a distance fails beyond max(fixed floor, the RR-aa distance).\n"
		"  A subject slot also takes an RR run (the A/A sanity: --rr <rr-aa>\n"
		"  --rr-aa <rr> --ro <rr> must pass).\n"
		"  --evidence external compares every run's external.pcapng instead of the\n"
		"  summary's evidence capture. Exit 0 PASS, 1 FAIL, 2 usage or input error.\n";

constexpr size_t kPrintedFailures = 40;

int usage(std::FILE *err, const std::string &problem) {
	if (!problem.empty())
		std::fprintf(err, "opennova-parity: %s\n", problem.c_str());
	std::fputs(kUsage, err);
	return kExitUsage;
}

} // namespace

int run_cli(const std::vector<std::string> &args, std::FILE *out, std::FILE *err) {
	std::map<std::string, std::string> values;
	static const char *const kValued[] = { "--items", "--rr", "--rr-aa", "--ro", "--or", "--oo",
		"--out", "--evidence" };
	for (size_t i = 0; i < args.size(); ++i) {
		const std::string &arg = args[i];
		if (arg == "--help" || arg == "-h") {
			std::fputs(kUsage, out);
			return kExitPass;
		}
		bool known = false;
		for (const char *flag : kValued)
			known = known || arg == flag;
		if (!known)
			return usage(err, "unknown argument '" + arg + "'");
		if (i + 1 >= args.size() || args[i + 1].empty())
			return usage(err, arg + " needs a value");
		if (values.count(arg))
			return usage(err, arg + " given twice");
		values[arg] = args[++i];
	}
	for (const char *required : { "--items", "--rr", "--rr-aa" })
		if (!values.count(required))
			return usage(err, std::string(required) + " is required");
	if (!values.count("--ro") && !values.count("--or") && !values.count("--oo"))
		return usage(err, "give at least one subject (--ro, --or, --oo)");
	EvidenceSource source = EvidenceSource::Summary;
	if (values.count("--evidence")) {
		if (values["--evidence"] == "external")
			source = EvidenceSource::External;
		else if (values["--evidence"] != "summary")
			return usage(err, "--evidence is summary or external");
	}
	const std::string out_path =
			values.count("--out") ? values["--out"] : std::string("parity-verdict.json");

	wire::ItemTables items;
	if (wire::load_items_def(values["--items"].c_str(), items) == 0) {
		std::fprintf(err, "opennova-parity: cannot load items.def %s\n", values["--items"].c_str());
		return kExitUsage;
	}

	struct Slot {
		const char *flag;
		const char *label;
		const char *topology;
	};
	static const Slot kSlots[] = { { "--rr", "rr", "RR" }, { "--rr-aa", "rr_aa", "RR" },
		{ "--ro", "ro", "RO" }, { "--or", "or", "OR" }, { "--oo", "oo", "OO" } };
	std::map<std::string, std::unique_ptr<RunFacts>> facts;
	for (const Slot &slot : kSlots) {
		if (!values.count(slot.flag))
			continue;
		RunInfo info;
		std::string error;
		if (!read_run_info(values[slot.flag], slot.label, source, info, error)) {
			std::fprintf(err, "opennova-parity: %s\n", error.c_str());
			return kExitUsage;
		}
		// A subject slot also takes an RR run: retail judged as a subject is the A/A
		// sanity (`--rr <rr-aa> --rr-aa <rr> --ro <rr>` must pass).
		const bool subject_slot = std::string(slot.topology) != "RR";
		if (info.topology != slot.topology && !(subject_slot && info.topology == "RR")) {
			std::fprintf(err, "opennova-parity: %s %s is a %s run, not %s\n", slot.flag,
					values[slot.flag].c_str(), info.topology.c_str(), slot.topology);
			return kExitUsage;
		}
		auto run = std::make_unique<RunFacts>();
		if (!load_run_facts(info, &items.classes, *run, error)) {
			std::fprintf(err, "opennova-parity: %s\n", error.c_str());
			return kExitUsage;
		}
		facts[slot.label] = std::move(run);
	}
	if (facts["rr"]->info.capture == facts["rr_aa"]->info.capture) {
		std::fprintf(err,
				"opennova-parity: --rr and --rr-aa are the same capture; the A/A "
				"floor needs two independent retail runs\n");
		return kExitUsage;
	}

	std::vector<SubjectRun> subjects;
	for (const char *label : { "ro", "or", "oo" }) {
		const auto found = facts.find(label);
		if (found == facts.end() || !found->second)
			continue;
		subjects.push_back({ found->second->info.topology, found->second.get() });
	}
	const Verdict verdict = judge(*facts["rr"], *facts["rr_aa"], subjects);
	const std::string json =
			verdict_json(verdict, values["--items"], *facts["rr"], *facts["rr_aa"], subjects);
	std::string error;
	if (!io::write_file_atomic(out_path, json, error)) {
		std::fprintf(
				err, "opennova-parity: cannot write %s: %s\n", out_path.c_str(), error.c_str());
		return kExitUsage;
	}

	size_t printed = 0;
	for (const EvidenceRow &row : verdict.evidence)
		if (printed++ < kPrintedFailures)
			std::fprintf(out, "PARITY_FAIL run=%s code=%s detail=%s\n", row.run.c_str(),
					row.code.c_str(), row.detail.c_str());
	for (const VerdictRow &row : verdict.rows) {
		if (row.status != "fail" || printed++ >= kPrintedFailures)
			continue;
		char distance[32] = "unexercised";
		if (row.value.comparable)
			std::snprintf(distance, sizeof(distance), "%g", row.value.distance);
		std::fprintf(out,
				"PARITY_FAIL topology=%s dir=%c metric=%s key=%s code=%s distance=%s "
				"threshold=%g subject=%s rr=%s\n",
				row.topology.c_str(), row.value.direction, row.value.metric.c_str(),
				row.value.key.empty() ? "-" : row.value.key.c_str(), row.value.code.c_str(),
				distance, row.threshold, row.value.subject.substr(0, 80).c_str(),
				row.value.reference.substr(0, 80).c_str());
	}
	size_t widened = 0;
	for (const VerdictRow &row : verdict.aa)
		widened += row.status == "widened" ? 1 : 0;
	std::fprintf(out, "PARITY_AA_WIDENED=%zu\n", widened);
	std::fprintf(out, "PARITY_STATUS=%s\n", verdict.pass() ? "PASS" : "FAIL");
	std::fprintf(out, "PARITY_FAILURES=%zu\n", verdict.failures);
	std::fprintf(out, "PARITY_REPORT=%s\n", out_path.c_str());
	return verdict.pass() ? kExitPass : kExitFail;
}

} // namespace opennova::parity
