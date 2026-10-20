#pragma once
// The inputs the menu and sound document sweeps read: every file of an extension in the folders a sweep
// covers (the formats' minted variants under fixtures/<format>/, OpenNova's own base game in assets/, its
// expansion projects under expansions/), and the variants a sweep requires by name, each a failure when it
// is missing, never a skip. The JO corpus these sweeps once read from a retail install is closed, so each of
// its variants is minted once (tests/fixtures/*_gen.cpp, the format's lane) and read here.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <base/io/os_path.h>
#include <base/io/strutil.h>

#include "common/test_paths.h"

namespace variant_fixtures {

inline std::string repo() { return test_paths_repo_root(__FILE__); }

// A swept file: its path under the repo (what a failure names) and its path on disk.
struct Input {
	std::string label;
	std::string path;
};

// Every file named with `extension` (compared without case) in each of `folders` (under the repo, not walked
// down), each folder's sorted by name, then each of `required` (paths under the repo) not already among them.
// False, naming each, when a required file is missing.
inline bool inputs(const std::vector<std::string> &folders, const std::string &extension,
                   const std::vector<std::string> &required, std::vector<Input> &out) {
	using opennova::strutil::iequals;
	const std::string root = repo();
	out.clear();
	for (const std::string &folder : folders) {
		std::vector<std::string> names;
		std::error_code ec;
		for (const auto &entry : std::filesystem::directory_iterator(opennova::io::os_path(root + "/" + folder), ec)) {
			if (!entry.is_regular_file(ec)) continue;
			const std::string name = opennova::io::utf8_path(entry.path().filename());
			if (name.size() > extension.size() && iequals(name.substr(name.size() - extension.size()), extension))
				names.push_back(name);
		}
		std::sort(names.begin(), names.end());
		for (const std::string &name : names) out.push_back({folder + "/" + name, root + "/" + folder + "/" + name});
	}
	bool all = true;
	for (const std::string &label : required) {
		if (std::any_of(out.begin(), out.end(), [&](const Input &in) { return iequals(in.label, label); })) continue;
		std::error_code ec;
		if (!std::filesystem::is_regular_file(opennova::io::os_path(root + "/" + label), ec)) {
			std::printf("  FAIL the variant fixture %s is missing\n", label.c_str());
			all = false;
			continue;
		}
		out.push_back({label, root + "/" + label});
	}
	return all;
}

// The menu variants the format's lane authors (tests/mnu/repo_menus.h): shipped_forms.mnu, every element,
// attribute and lexical form the shipped menus use, written in their byte form (CR LF, tab indents, comments,
// bare flags, unquoted numbers, the slips their quotes make), and render_cases.mnu, the drawing cases the shipped
// screens hold and our base game's do not.
inline const std::vector<std::string> &menu_variants() {
	static const std::vector<std::string> variants = {"fixtures/mnu/shipped_forms.mnu", "fixtures/mnu/render_cases.mnu"};
	return variants;
}

// The menus the menu rows sweep (editor_menus, editor_menu_table, editor_menu_render): fixtures/mnu/'s, the
// base game's (assets/, the screens the game shows, modelled on the original's) and the variants above.
inline bool menus(std::vector<Input> &out) { return inputs({"fixtures/mnu", "assets"}, ".mnu", menu_variants(), out); }

} // namespace variant_fixtures
