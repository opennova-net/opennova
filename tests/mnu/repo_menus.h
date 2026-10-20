// The menus the tree holds, which the menu format and runtime sweeps read in place of an
// installed game's: the authored set under fixtures/mnu (widgets.mnu, all_widgets.mnu, the
// render and keyboard cases, and shipped_forms.mnu, every element, attribute and lexical form
// the shipped JO menus use, written in their byte form) and the base game's own screens in
// assets/ (ADR 0048; the editor's writer form). No retail byte: the shipped menus were
// surveyed once for the constructs they hold, and each is authored here (fixtures/README.md).
#pragma once

#include "common/test_paths.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace repo_menus {

inline std::string lower(std::string s) {
	for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

inline std::string root() { return test_paths_repo_root(__FILE__); }

// The files of `<root>/<dir>` whose extension is `ext` (compared without case), sorted.
inline std::vector<std::string> files(const std::string &dir, const char *ext) {
	std::vector<std::string> out;
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(root() + "/" + dir, ec)) {
		if (!entry.is_regular_file(ec)) continue;
		if (lower(entry.path().extension().string()) == ext) out.push_back(entry.path().generic_string());
	}
	std::sort(out.begin(), out.end());
	return out;
}

// The authored menus (fixtures/mnu) then the base game's (assets/).
inline std::vector<std::string> menus() {
	std::vector<std::string> out = files("fixtures/mnu", ".mnu");
	const std::vector<std::string> base = files("assets", ".mnu");
	out.insert(out.end(), base.begin(), base.end());
	return out;
}

// The stylesheets: the authored ones (fixtures/mns) then the base game's (assets/).
inline std::vector<std::string> stylesheets() {
	std::vector<std::string> out = files("fixtures/mns", ".mns");
	const std::vector<std::string> base = files("assets", ".mns");
	out.insert(out.end(), base.begin(), base.end());
	return out;
}

// `<root>/<rel>`.
inline std::string path(const std::string &rel) { return root() + "/" + rel; }

// The file name of a path.
inline std::string name(const std::string &path) { return std::filesystem::path(path).filename().string(); }

// A sweep must see the variant fixture the shipped forms live in.
inline bool holds(const std::vector<std::string> &paths, const char *file) {
	for (const std::string &p : paths)
		if (lower(name(p)) == lower(file)) return true;
	return false;
}

} // namespace repo_menus
