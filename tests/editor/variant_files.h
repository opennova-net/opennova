#pragma once
// The files a document type's core leg opens over a format's variants: every file of the repository's
// directories it names (a format's fixtures/<format>/, the base game's assets/) whose extension is one of a
// list (without case), in name order, and the paths the format's own rows commit, which must be there: a
// missing one is a failure, never a skip. The paths are the repository root's. A file of our own game (assets/,
// expansions/) that Git LFS keeps and the checkout has not pulled (CI's test jobs pull fixtures/ alone) is its
// pointer: skipped, said, never swept as a file of its format; the minted variants under fixtures/ carry the forms.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <base/io/os_path.h>
#include <base/io/strutil.h>

#include "common/file_io.h"
#include "common/test_paths.h"

namespace editor_test {

struct VariantFiles {
	std::vector<std::string> files;   // repository-relative, in name order, each once
	std::vector<std::string> missing; // the expected paths that are not there
	std::vector<std::string> skipped; // the game's files that are unpulled LFS pointers
};

// Whether a directory is our own game's (assets/, expansions/...), whose LFS files a checkout may not have pulled.
inline bool game_dir(const std::string &dir) {
	return dir.rfind("assets", 0) == 0 || dir.rfind("expansions", 0) == 0;
}

inline std::string repo_path(const std::string &relative) {
	return std::string(test_paths_repo_root(__FILE__)) + "/" + relative;
}

inline std::vector<uint8_t> repo_bytes(const std::string &relative) { return test_io::read_file(repo_path(relative)); }

// `dirs` and `expected` repository-relative; `extensions` with their dot (".aip").
inline VariantFiles variant_files(const std::vector<std::string> &dirs, const std::vector<std::string> &extensions,
                                  const std::vector<std::string> &expected = {}) {
	VariantFiles out;
	const auto wanted = [&](const std::string &name) {
		return std::any_of(extensions.begin(), extensions.end(),
		                   [&](const std::string &ext) { return opennova::strutil::ends_with_icase(name, ext); });
	};
	for (const std::string &dir : dirs) {
		std::error_code ec;
		for (const auto &entry : std::filesystem::directory_iterator(opennova::io::os_path(repo_path(dir)), ec)) {
			if (!entry.is_regular_file(ec)) continue;
			const std::string name = opennova::io::utf8_path(entry.path().filename());
			if (!wanted(name)) continue;
			if (game_dir(dir) && test_io::is_lfs_pointer(repo_bytes(dir + "/" + name))) {
				std::printf("[skip] %s/%s is an unpulled LFS pointer\n", dir.c_str(), name.c_str());
				out.skipped.push_back(dir + "/" + name);
				continue;
			}
			out.files.push_back(dir + "/" + name);
		}
	}
	for (const std::string &path : expected) {
		std::error_code ec;
		if (!std::filesystem::is_regular_file(opennova::io::os_path(repo_path(path)), ec)) {
			out.missing.push_back(path);
			continue;
		}
		out.files.push_back(path);
	}
	std::sort(out.files.begin(), out.files.end());
	out.files.erase(std::unique(out.files.begin(), out.files.end()), out.files.end());
	return out;
}

// Each missing expected path said; their count.
inline int report_missing(const VariantFiles &variants) {
	for (const std::string &path : variants.missing)
		std::fprintf(stderr, "FAIL %s: the variant fixture its format's row commits is not there\n", path.c_str());
	return int(variants.missing.size());
}

} // namespace editor_test
