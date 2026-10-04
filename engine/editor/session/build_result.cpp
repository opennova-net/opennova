#include <editor/session/build_result.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <editor/graph/reference_kinds.h>
#include <editor/model/field_text.h>
#include <editor/project_build/build_plan.h>

namespace opennova::editor {

std::string size_words(uint64_t bytes) {
	char text[32];
	const double b = static_cast<double>(bytes);
	if (bytes < 1024) std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
	else if (bytes < (uint64_t(1) << 20)) std::snprintf(text, sizeof(text), "%.1f KB", b / 1024.0);
	else if (bytes < (uint64_t(1) << 30)) std::snprintf(text, sizeof(text), "%.1f MB", b / double(uint64_t(1) << 20));
	else std::snprintf(text, sizeof(text), "%.1f GB", b / double(uint64_t(1) << 30));
	return text;
}

namespace {

// "37 s", "2 min 5 s", "under a second".
std::string duration_words(double seconds) {
	if (seconds < 1.0) return "under a second";
	const long total = std::lround(seconds);
	if (total < 60) return std::to_string(total) + " s";
	return std::to_string(total / 60) + " min " + std::to_string(total % 60) + " s";
}

} // namespace

BuildResult build_result(const BuildReport &report, bool in_project) {
	BuildResult out;
	if (report.refused) {
		out.outcome = BuildResult::Outcome::Refused;
		std::vector<Diagnostic> blockers;
		for (const Diagnostic &d : report.diagnostics)
			if (blocks_build(d) && d.row() != &finding_code(CoreFinding::BuildBlocked)) blockers.push_back(d);
		// Why, by the refusals' class: the game's own (a required file, a reference the game cannot go on
		// without), the editor's (what it cannot read, write or store), or both.
		const size_t games = size_t(std::count_if(blockers.begin(), blockers.end(), blocker_is_the_games));
		const std::string why = games == blockers.size() ? " it, as the game would stop."
		                        : games == 0 ? " it: the editor does not pack what it cannot vouch for."
		                                     : " it: the game would stop for some, and the editor does not pack the others.";
		out.headline = "Build refused: " + counted(blockers.size(), "problem") + (blockers.size() == 1 ? " stops" : " stop") + why;
		for (const Diagnostic &d : blockers) out.refusals.push_back(blocker_words(d));
		return out;
	}
	if (!report.ok) {
		out.outcome = BuildResult::Outcome::Failed;
		out.headline = "Build failed.";
		for (const Diagnostic &d : report.diagnostics)
			if (d.severity == DiagnosticSeverity::Error) {
				out.failure = d.message;
				break;
			}
		if (out.failure.empty()) out.failure = "It was cancelled: nothing it made is kept.";
		return out;
	}
	out.outcome = report.reused_existing ? BuildResult::Outcome::Unchanged : BuildResult::Outcome::Built;
	out.headline = report.reused_existing ? std::string("Build unchanged: the files are as the last build packed them.")
	                                      : "Built in " + duration_words(report.seconds) + ".";
	out.where = report.build_dir;
	for (const BuiltFile &file : report.built) {
		std::string words = size_words(file.bytes);
		if (file.archive) {
			words += ", " + grouped(file.files) + (file.files == 1 ? " file" : " files");
			if (!report.reused_existing) words += file.reused ? ", kept from the last build" : ", written";
		}
		out.files.push_back({file.name, words});
	}
	// How a player plays it: the game reads its archives from its own folder [orig: PFF_OpenAllArchives @
	// 0x4a4310, over the name table @ 0x829f90], so these take the place of the game's own.
	if (!in_project)
		out.players = "To play it, copy these files into a copy of the game's folder, over the files of these names. "
		              "The game then reads only what this build packs, so the project must hold everything the "
		              "game needs (File > Import the whole game install).";
	// A folder several projects build into: theirs are never pruned, replaced or taken.
	if (!report.others.empty())
		out.others = "The folder holds " + counted(report.others.size(), "build") + " of other projects, left as " +
		             (report.others.size() == 1 ? "it is." : "they are.");
	return out;
}

std::string refused_words(const BuildReport &report) {
	if (!report.refused) return std::string();
	const std::vector<std::string> refusals = build_result(report, true).refusals;
	if (refusals.empty()) return "The build was refused.";
	return "Refused: " + refusals.front() +
	       (refusals.size() > 1 ? " (and " + std::to_string(refusals.size() - 1) + " more)" : std::string()) + ".";
}

std::string build_result_line(const BuildResult &result) {
	std::string line = result.headline;
	if (!line.empty() && line.back() == '.') line.pop_back();
	std::string files;
	for (size_t i = 0; i < result.files.size(); ++i) {
		const std::string &words = result.files[i].words;
		files += (i == 0 ? "" : i + 1 == result.files.size() ? " and " : ", ") + result.files[i].name + " " +
		         words.substr(0, words.find(','));
	}
	if (!files.empty()) line += ": " + files;
	return line + (result.where.empty() ? std::string(".") : ", in " + result.where + ".");
}

} // namespace opennova::editor
