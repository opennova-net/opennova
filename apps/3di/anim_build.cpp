// opennova-3di anim build: mint a `.adm` table and every `.bad` clip it names
// (or one lone `.bad`) from the `.o3a` clip-set text a DCC exporter writes. The
// reader and the mint for the retail target are the engine's (formats/bad/bad_o3a_read.h,
// bad_build_mint_set); this command opens the file, prints the findings as
// `path:line: message` and writes every file whole, or none.

#include "anim_cli.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <base/io/strutil.h>
#include <formats/bad/bad_build.h>
#include <formats/bad/bad_o3a_read.h>
#include <runtime/anim/adm_clip_index.h>

#include "threedi_cli.h"

using namespace opennova::bad;

namespace opennova::threedi_cli {

int cmd_anim_build(const char *scene_path, const char *out_path) {
	std::ifstream file(scene_path);
	if (!file) {
		std::fprintf(stderr, "opennova-3di: cannot open %s\n", scene_path);
		return 1;
	}
	BadBuildSet set;
	std::vector<opennova::threedi::SceneFinding> findings;
	BadO3aLines lines;
	const bool read = bad_o3a_read(file, set, findings, &lines);
	if (!print_findings(scene_path, findings) || !read) return 1;

	const std::filesystem::path out = std::filesystem::path(out_path);
	const bool lone = opennova::strutil::iequals(out.extension().string(), ".bad");
	// `-o` names the file; an `adm` record that names another is only noted.
	if (!set.adm_name.empty() && (lone || !opennova::strutil::iequals(set.adm_name, out.filename().string())))
		std::fprintf(stderr, "opennova-3di: note: the set names its table '%s'; -o writes %s\n", set.adm_name.c_str(),
				out_path);

	// Every clip and the table are minted in memory for the retail target and
	// read back before any file is written: a set that fails anywhere writes
	// nothing, and every problem the target finds names its line (a bone's, a
	// clip's, a row's).
	std::vector<BadMintedFile> files;
	std::vector<BadBuildProblem> problems;
	if (!bad_build_mint_set(set, out.filename().string(), bad_retail_limits(), opennova::anim::adm_slot_index, files,
				problems)) {
		std::vector<opennova::threedi::SceneFinding> refused;
		bad_o3a_findings(lines, problems, refused);
		print_findings(scene_path, refused);
		return 1;
	}
	size_t total = 0;
	for (BadMintedFile &minted : files) {
		const bool table = !lone && &minted == &files.back();
		const std::string path = lone || table ? out.string() : (out.parent_path() / minted.name).string();
		if (!table) total += minted.bytes.size();
		// Each file lands whole or not at all (write_output): a full disk never
		// leaves a truncated clip where the last good one was.
		if (!write_output(path.c_str(), minted.bytes.data(), minted.bytes.size())) return 1;
	}
	if (!lone) {
		std::printf("wrote %s (%zu rows) and %zu clips (%zu bytes)\n", out_path, set.rows.size(), set.clips.size(),
				total);
		return 0;
	}
	std::printf("wrote %s (%zu bytes)\n", out_path, total);
	return 0;
}

} // namespace opennova::threedi_cli
