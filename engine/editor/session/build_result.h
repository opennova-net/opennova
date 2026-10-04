#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/project_build/build_run.h>

namespace opennova::editor {

// What the last build came to, in a modder's words (the UX round's problems lane): the build panel's
// model, which the panel draws, Output's line says in short and the tests read. A build that landed: how long it took, where (the
// project's own build folder, or the folder Build to folder chose, with a line on how a player installs
// what it holds), and each file it published with its size (an archive with how many files it packs,
// written or kept from the last build); one that changed nothing says so; one refused names what refuses
// it (the gate's refusals, each with why); one that failed on the way says why.
struct BuildResult {
	enum class Outcome { None, Built, Unchanged, Refused, Failed };
	Outcome outcome = Outcome::None;
	std::string headline; // "Built in 37 s."
	std::string where;    // the folder it landed in ("" when it landed nowhere)
	struct File {
		std::string name;
		std::string words; // "220.4 MB, 2,301 files, written"
	};
	std::vector<File> files;
	std::string players;               // a folder build's: how a player installs it
	std::string others;                // the folder's builds of other projects, left as they are ("" for none)
	std::vector<std::string> refusals; // a refused build's, each in words, the first few
	std::string failure;               // a failed build's: why
};

// The result of `report`; `in_project` whether it landed under the project's own build folder (Build), or
// in a folder of the modder's choosing (Build to folder).
BuildResult build_result(const BuildReport &report, bool in_project);
// A build that landed in one line for Output: "Built in 37 s: resource.pff 220.4 MB, localres.pff 3.1 MB,
// language.pff 12.0 KB and 1 loose file, in <where>."
std::string build_result_line(const BuildResult &result);

// What refused a build, from its own report (the menu bar's "Build refused" says it whatever the status line
// has moved on to): "Refused: <its first refusal> (and N more)."; "" for a build not refused.
std::string refused_words(const BuildReport &report);

// A size in a modder's units: "812 B", "4.5 KB", "220.4 MB", "1.2 GB".
std::string size_words(uint64_t bytes);

} // namespace opennova::editor
