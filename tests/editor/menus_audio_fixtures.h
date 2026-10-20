#pragma once
// The inputs the menu and sound document sweeps read: every file of an extension in the folders a sweep
// covers (the formats' minted variants under fixtures/<format>/, OpenNova's own base game in assets/, its
// expansion projects under expansions/), and the variants a sweep requires by name, each a failure when it
// is missing, never a skip. The JO corpus these sweeps once read from a retail install is closed, so each of
// its variants is minted once (tests/fixtures/*_gen.cpp, the format's lane) and read here.
#include <cstdio>
#include <string>
#include <vector>

#include "editor/variant_files.h"

namespace variant_fixtures {

inline std::string repo() { return test_paths_repo_root(__FILE__); }

// A swept file: its path under the repo (what a failure names) and its path on disk.
struct Input {
	std::string label;
	std::string path;
};

// Every file named with `extension` (compared without case) in each of `folders` (under the repo, not walked
// down) and each of `required` (paths under the repo), in name order, each once (editor/variant_files.h's
// sweep). False, naming each, when a required file is missing.
inline bool inputs(const std::vector<std::string> &folders, const std::string &extension,
                   const std::vector<std::string> &required, std::vector<Input> &out) {
	const editor_test::VariantFiles found = editor_test::variant_files(folders, {extension}, required);
	out.clear();
	for (const std::string &label : found.files) out.push_back({label, editor_test::repo_path(label)});
	for (const std::string &label : found.missing) std::printf("  FAIL the variant fixture %s is missing\n", label.c_str());
	return found.missing.empty();
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

// The expansion's own main.mnu for the menu rows' install legs (minted_install.h mint_with): the base game's with its
// STARTUP screen's MUSICVAR 2, so the expansion's mount serves a menu of a base game's name with other bytes (swept
// again) beside the ones it serves unchanged (counted once). False where assets/main.mnu holds no MUSICVAR 1.
inline bool expansion_main_menu(std::vector<uint8_t> &out) {
	const std::vector<uint8_t> base = editor_test::repo_bytes("assets/main.mnu");
	std::string text(base.begin(), base.end());
	const std::string from = "<MUSICVAR>1</MUSICVAR>";
	const size_t at = text.find(from);
	if (at == std::string::npos) return false;
	text.replace(at, from.size(), "<MUSICVAR>2</MUSICVAR>");
	out.assign(text.begin(), text.end());
	return true;
}

} // namespace variant_fixtures
