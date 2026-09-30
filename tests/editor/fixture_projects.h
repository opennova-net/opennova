#pragma once
// D4's four fixture projects (S13 D4, tests/editor/project_validation_test.cpp), shared with the
// long operations' tests (S13 A3): every fixture a document type reads, the stylesheets' uses, the
// item ids, and the two together (the case with open documents); a finding as one line, every
// member of it.
#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <editor/graph/reference_kinds.h>
#include <editor/model/diagnostic.h>

#include "common/file_io.h"
#include "common/test_paths.h"

namespace fixture_projects {

namespace fs = std::filesystem;
using opennova::editor::Diagnostic;
using opennova::editor::diagnostic_severity_label;
using opennova::editor::reference_row;

inline std::string repo() {
	return std::string(test_paths_repo_root(__FILE__));
}

inline std::string text_of(const std::vector<uint8_t> &bytes) {
	return std::string(bytes.begin(), bytes.end());
}

// A finding as one line, every member of it (its reference kind by its token, which a new kind
// leaves as it is).
inline std::string row_of(const Diagnostic &d) {
	return std::string(diagnostic_severity_label(d.severity)) + "|" + d.code + "|" + d.asset + "|" +
			std::to_string(d.line) + "|" + d.record + "|" + d.field + "|" +
			std::to_string(d.row_id) + "|" + std::to_string(d.child_id) + "|" +
			std::to_string(d.record_kind) + "|" + d.role + "|" + d.target + "|" +
			reference_row(d.reference).token + "|" + d.scope + "|" + std::to_string(d.loader_arg) +
			"|" + d.message;
}

using Files = std::vector<std::pair<std::string, std::string>>;

// Every fixture a document type reads: the item table, the string tables, the menus, the
// stylesheet (as menu_style.mns, the one the game reads), the models, the clips and the tables.
inline Files fixture_files() {
	Files files;
	const std::string root = repo() + "/fixtures/";
	const auto add_dir = [&](const std::string &dir, const std::string &to, const char *extension,
								 bool as_style = false) {
		std::vector<fs::path> found;
		std::error_code ec;
		for (const auto &entry : fs::directory_iterator(root + dir, ec))
			if (entry.is_regular_file(ec) && entry.path().extension() == extension)
				found.push_back(entry.path());
		std::sort(found.begin(), found.end());
		for (const fs::path &path : found)
			files.push_back({ to +
							(as_style ? std::string("menu_style.mns")
									  : path.filename().generic_string()),
					text_of(test_io::read_file(path.generic_string())) });
	};
	add_dir("def", "defs/", ".def");
	add_dir("rtxt", "strings/", ".bin");
	add_dir("mnu", "menus/", ".mnu");
	add_dir("mns", "menus/", ".mns", true);
	add_dir("threedi/synth", "models/", ".3di");
	add_dir("anim", "anims/", ".bad");
	add_dir("anim", "anims/", ".adm");
	return files;
}

inline std::string window(const std::string &name, const std::string &body) {
	return "<WINDOW TYPE=\"STATIC\" NAME=\"" + name +
			"\">\r\n<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>90</RIGHT><BOTTOM>90</BOTTOM></"
			"POSITION>\r\n" +
			body + "</WINDOW>\r\n";
}

// The stylesheet's cross-file checks: the shell's two stylesheets and one the game never reads,
// and a menu naming their variables as colours, a font and an image.
inline Files style_files() {
	return {
		{ "menus/menu_style.mns",
				"// the shell\r\nCOL_OK FF00FF00\r\nCOL_BAD red\r\nFONT_A arial.fnt\r\nIMG_A "
				"logo.tga\r\n"
				"MIXED FF112233\r\nUNUSED_V 1\r\nDUP 1\r\nDUP 2\r\nXML <b>\r\nNESTED %COL_OK%\r\n"
				"SLASH a\\\\b\r\nBRANDED 11\r\nALSO FF000000\r\n" },
		{ "menus/brand.mns", "BRANDED FF222222\r\nNEWONE 5\r\n" },
		{ "menus/other.mns", "COL_OK 1\r\nLOST 2\r\n" },
		{ "menus/main.mnu",
				"<SCREEN>\r\n<NAME>MAIN</NAME>\r\n" +
						window("A",
								"<APPEARANCE STATE=\"DEFAULT\" "
								"TYPE=\"COLOR\">%COL_OK%</APPEARANCE>\r\n"
								"<FONT><NAME>%FONT_A%</NAME><DEFAULT_FG>%COL_BAD%</DEFAULT_FG></"
								"FONT>\r\n") +
						window("B",
								"<APPEARANCE STATE=\"DEFAULT\" "
								"TYPE=\"IMAGE\">%IMG_A%</APPEARANCE>\r\n"
								"<FONT><NAME>%MIXED%</NAME><DEFAULT_FG>%MIXED%</DEFAULT_FG></"
								"FONT>\r\n") +
						window("C",
								"<APPEARANCE STATE=\"DEFAULT\" "
								"TYPE=\"COLOR\">%BRANDED%</APPEARANCE>\r\n"
								"<FONT><DEFAULT_FG>%DUP%</DEFAULT_FG><DEFAULT_BG>%NEWONE%</"
								"DEFAULT_BG></FONT>\r\n") +
						window("D",
								"<APPEARANCE STATE=\"DEFAULT\" "
								"TYPE=\"COLOR\">%XML%</APPEARANCE>\r\n") +
						"</SCREEN>\r\n" },
		{ "fonts/arial.fnt", "fnt" },
		{ "art/logo.tga", "tga" },
	};
}

inline std::string item(const std::string &name, int id) {
	return "begin \"" + name + "\"\n  id " + std::to_string(id) +
			"\n  type marker\n  hp 0\nend\n\n";
}

// The item ids: repeated inside a table (0 among them), and a second table repeating the
// first's (no finding: the game reads one of the two); the weapon and ammo names repeated in
// their tables.
inline Files item_files() {
	return {
		{ "defs/items.def",
				item("Alpha", 100) + item("Bravo", 101) + item("Charlie", 100) + item("Delta", 0) +
						item("Echo", 0) + item("Foxtrot", 102) },
		{ "extra/items.def", item("Golf", 102) + item("Hotel", 103) + item("India", 101) },
		{ "defs/weapon.def",
				"weapon \"WPN_A\"\ncategory 3\nend\nweapon \"wpn_a\"\ncategory 4\nend\n" },
		{ "defs/ammo.def", "ammo AMMO_A\nvelocity 900\nend\nammo AMMO_A\nvelocity 800\nend\n" },
	};
}

// The stylesheets' and the item tables' files together (the case with open documents).
inline Files style_and_item_files() {
	Files files = style_files();
	for (auto &file : item_files())
		files.push_back(file);
	return files;
}


} // namespace fixture_projects
