// S13 A6: the finding codes (ADR 0046). Every finding is made from a row of a table: the editor's
// own (CoreFinding) or a document type's (DocumentType::findings), each table static_asserted
// into its enum's order. Here, over the tables as the registry holds them: every token is its own
// across every table; finding_row finds each row by its token and nothing else; a type's rows are
// its family's; the menu's render-check rows are the compiler notes' tokens; the columns say what
// the fixes, the file index and the Problems location decided by the code's spelling before (the
// same codes, now read from the rows); a finding made from a row carries its token; and a
// finding's subject is written with the keys the wire had.
#include <editor/documents/animation_document.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/catalog_validation.h>
#include <editor/documents/document_types.h>
#include <editor/documents/mns_document.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/model_document.h>
#include <editor/documents/strings_document.h>
#include <editor/session/finding_codes.h>
#include <editor/session/session_json.h>
#include <base/io/json.h>
#include <runtime/menu/menu_frame.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common/test_expect.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

// Every table as the registry holds it: the core's, then each type's, with whose it is.
struct Table {
	std::string owner;
	FindingTable rows;
};
std::vector<Table> tables() {
	std::vector<Table> out{ { "core", core_finding_codes() } };
	for (size_t i = 1; i <= kDocumentTypeCount; ++i) {
		const DocumentType *type = document_type(static_cast<DocumentTypeId>(i));
		if (type && type->findings) out.push_back({ type->name, type->findings() });
	}
	return out;
}

// The tokens of every row whose columns `keep` takes, sorted.
template <typename Keep>
std::vector<std::string> tokens_where(Keep keep) {
	std::vector<std::string> out;
	for (const Table &table : tables())
		for (const FindingCodeRow &row : table.rows)
			if (keep(row)) out.push_back(row.token);
	std::sort(out.begin(), out.end());
	return out;
}

std::vector<std::string> fixed_by(FindingFix fixes) {
	return tokens_where([fixes](const FindingCodeRow &row) { return row.fixes == fixes; });
}

} // namespace

// Every row has a dotted token, none shared across the tables (the core's and every type's); a
// Rewrite's words exactly on a Rewrite row, and no Rewrite on a row that blocks the save.
static int test_tokens_unique() {
	std::map<std::string, std::string> owners; // a token -> the table declaring it
	size_t rows = 0;
	for (const Table &table : tables()) {
		TEST_EXPECT(table.rows.count > 0);
		for (const FindingCodeRow &row : table.rows) {
			++rows;
			const std::string token = row.token ? row.token : "";
			TEST_EXPECT(!token.empty() && token.find('.') != std::string::npos &&
			            token.front() != '.' && token.back() != '.');
			const auto made = owners.emplace(token, table.owner);
			if (!made.second)
				std::fprintf(stderr, "%s is declared by %s and %s\n", token.c_str(),
				             made.first->second.c_str(), table.owner.c_str());
			TEST_EXPECT(made.second);
			TEST_EXPECT((row.fixes == FindingFix::Rewrite) == (row.rewrite_does != nullptr));
			TEST_EXPECT(!(row.blocks_save && row.fixes == FindingFix::Rewrite));
		}
	}
	TEST_EXPECT(tables().size() == 1 + kDocumentTypeCount);
	TEST_EXPECT(core_finding_codes().count == kCoreFindingCount);
	// finding_tables (what the editor MCP's catalog lists) is the same walk.
	const std::vector<Table> walked = tables();
	const std::vector<NamedFindingTable> named = finding_tables();
	TEST_EXPECT(named.size() == walked.size());
	for (size_t i = 0; i < named.size() && i < walked.size(); ++i)
		TEST_EXPECT(walked[i].owner == named[i].owner && walked[i].rows.rows == named[i].rows.rows &&
		            walked[i].rows.count == named[i].rows.count);
	// The columns' wire forms, each its own.
	std::set<std::string> fixes;
	for (const FindingFix fix : { FindingFix::None, FindingFix::Requirement, FindingFix::WrongKind, FindingFix::Rename,
	                              FindingFix::ResetRow, FindingFix::Reference, FindingFix::UnimportedTexture,
	                              FindingFix::Reload, FindingFix::Reimport, FindingFix::Rewrite })
		TEST_EXPECT(fixes.insert(finding_fix_token(fix)).second);
	TEST_EXPECT(std::string(finding_place_token(FindingPlace::Content)) == "content" &&
	            std::string(finding_place_token(FindingPlace::File)) == "file");
	std::printf("%zu finding codes in %zu tables (%zu the editor's own)\n", rows, tables().size(),
	            kCoreFindingCount);
	return 0;
}

// finding_row finds each row by its token (the row itself), and none for a token no table
// declares; a code's row is the one its enumerator names.
static int test_lookup() {
	for (const Table &table : tables())
		for (const FindingCodeRow &row : table.rows)
			TEST_EXPECT(finding_row(row.token) == &row);
	TEST_EXPECT(!finding_row("") && !finding_row("reference") && !finding_row("reference.missing.") &&
	            !finding_row("no.such_code"));
	TEST_EXPECT(finding_row("reference.missing") == &finding_code(CoreFinding::ReferenceMissing));
	TEST_EXPECT(std::string(finding_code(CoreFinding::AssetKindUnknown).token) == "asset.kind.unknown");
	TEST_EXPECT(std::string(finding_code(CoreFinding::UnsavedNone).token) == "unsaved.none");
	TEST_EXPECT(std::string(finding_code(CoreFinding::DocumentUnserializable).token) == "document.unserializable");
	TEST_EXPECT(std::string(finding_code(CatalogFinding::ItemType).token) == "catalog.item_type");
	TEST_EXPECT(std::string(finding_code(StringsFinding::Regrouped).token) == "strings.regrouped");
	TEST_EXPECT(std::string(finding_code(MenuFinding::RenderMapping).token) == "menu.render.mapping");
	TEST_EXPECT(std::string(finding_code(StyleFinding::LineEnding).token) == "style.line_ending");
	TEST_EXPECT(std::string(finding_code(StyleFinding::LoneBackslash).token) == "style.lone_backslash");
	TEST_EXPECT(std::string(finding_code(StyleFinding::Stops).token) == "style.stops");
	TEST_EXPECT(std::string(finding_code(StyleFinding::MixedUse).token) == "style.mixed_use");
	TEST_EXPECT(std::string(finding_code(ModelFinding::FrameMissing).token) == "model.frame_missing");
	TEST_EXPECT(std::string(finding_code(AnimationFinding::TriggerUnknown).token) == "animation.trigger_unknown");
	TEST_EXPECT(std::string(finding_code(AnimationMapFinding::SlotRepeated).token) == "animation_map.slot_repeated");
	return 0;
}

// A type's codes are one family, its own (the Problems group they fall in); the menu's render
// check has a row per compiler note, its token menu.render. and the note's, found by the note.
static int test_type_tables() {
	for (size_t i = 1; i <= kDocumentTypeCount; ++i) {
		const DocumentType *type = document_type(static_cast<DocumentTypeId>(i));
		TEST_EXPECT(type && type->findings);
		if (!type || !type->findings) continue;
		const FindingTable table = type->findings();
		const std::string first = table.rows[0].token;
		const std::string family = first.substr(0, first.find('.') + 1);
		for (const FindingCodeRow &row : table)
			TEST_EXPECT(std::string(row.token).compare(0, family.size(), family) == 0);
	}
	for (int i = 0; i < menu::kMenuFrameNoteCodeCount; ++i) {
		const auto note = static_cast<menu::MenuFrameNoteCode>(i);
		const FindingCodeRow &row = finding_code(note);
		TEST_EXPECT(std::string(row.token) == std::string("menu.render.") + menu::menu_frame_note_token(note));
		TEST_EXPECT(finding_row(row.token) == &row && row.fixes == FindingFix::None && !row.blocks_save);
	}
	TEST_EXPECT(menu_finding_codes().count ==
	            static_cast<size_t>(MenuFinding::kCount) + size_t(menu::kMenuFrameNoteCodeCount));
	return 0;
}

// The columns decide what the codes' spelling decided before (S13 A6 moved the rules into the
// rows): the fixes of each family (problem_fixes' collect), the Rewrite's words (its
// rewrite_does), the files that do not serialize (ProblemFixIndex's suffix test: .unserializable,
// .invalid_input), and the findings about the file as a whole (the Problems location's
// asset.name.*, build.name_unstorable, build.archive_in_project).
static int test_columns() {
	using Tokens = std::vector<std::string>;
	TEST_EXPECT(fixed_by(FindingFix::Requirement) ==
	            Tokens({ "play.boot_missing", "requirement.missing", "requirement.optional_missing" }));
	TEST_EXPECT(fixed_by(FindingFix::WrongKind) == Tokens({ "requirement.wrong_kind" }));
	TEST_EXPECT(fixed_by(FindingFix::Rename) ==
	            Tokens({ "asset.name.duplicate", "asset.name.too_long", "build.name_unstorable" }));
	TEST_EXPECT(fixed_by(FindingFix::ResetRow) == Tokens({ "animation_map.no_reset" }));
	TEST_EXPECT(fixed_by(FindingFix::Reference) == Tokens({ "reference.missing" }));
	TEST_EXPECT(fixed_by(FindingFix::UnimportedTexture) == Tokens({ "import.texture_not_imported" }));
	TEST_EXPECT(fixed_by(FindingFix::Reload) == Tokens({ "document.conflict" }));
	TEST_EXPECT(fixed_by(FindingFix::Reimport) == Tokens({ "import.output_missing" }));
	TEST_EXPECT(fixed_by(FindingFix::Rewrite) ==
	            Tokens({ "animation_map.ignored_input", "catalog.ignored_input", "menu.ignored_input",
	                     "strings.regrouped", "style.line_ending" }));
	const std::map<std::string, std::string> rewrites = {
		{ "animation_map.ignored_input", "without the input the game ignores" },
		{ "catalog.ignored_input", "without the input the game ignores" },
		{ "menu.ignored_input", "without the input the game ignores" },
		{ "strings.regrouped", "with its strings grouped by section the way the game reads them" },
		{ "style.line_ending", "with every line ending CR LF" },
	};
	for (const auto &[token, does] : rewrites) {
		const FindingCodeRow *row = finding_row(token);
		TEST_EXPECT(row && row->rewrite_does && does == row->rewrite_does);
	}
	TEST_EXPECT(tokens_where([](const FindingCodeRow &row) { return row.blocks_save; }) ==
	            Tokens({ "animation_map.invalid_input", "catalog.invalid_input", "catalog.unserializable",
	                     "document.unserializable", "menu.invalid_input", "menu.unserializable",
	                     "strings.invalid_input" }));
	TEST_EXPECT(tokens_where([](const FindingCodeRow &row) { return row.place == FindingPlace::File; }) ==
	            Tokens({ "asset.name.duplicate", "asset.name.empty", "asset.name.too_long", "build.archive_in_project",
	                     "build.name_unstorable" }));
	return 0;
}

// A finding made from a row carries its token, at its place; by a table's enumerator the same as
// by its row; and it is about nothing more until its producer says what.
static int test_make_finding() {
	const Diagnostic by_row = make_finding(finding_code(StyleFinding::Unused), DiagnosticSeverity::Info,
	                                       "No menu uses it.", "menus/menu_style.mns", "value");
	const Diagnostic by_code = make_finding(StyleFinding::Unused, DiagnosticSeverity::Info, "No menu uses it.",
	                                        "menus/menu_style.mns", "value");
	TEST_EXPECT(by_row == by_code && by_row.code == "style.unused" && by_row.severity == DiagnosticSeverity::Info &&
	            by_row.message == "No menu uses it." && by_row.asset == "menus/menu_style.mns" &&
	            by_row.field == "value" && std::holds_alternative<std::monostate>(by_row.subject) &&
	            !requirement_subject(by_row) && !reference_subject(by_row) && subject_target(by_row).empty());
	const Diagnostic core = make_finding(CoreFinding::GraphUnreadable, DiagnosticSeverity::Warning, "Unread.");
	TEST_EXPECT(core.code == "graph.unreadable" && core.asset.empty() && core.field.empty());
	// Two findings alike but for what they are about differ.
	Diagnostic required = make_finding(CoreFinding::RequirementMissing, DiagnosticSeverity::Error, "Missing.");
	Diagnostic other = required;
	required.subject = RequirementSubject{ "gametext", "gametext.bin" };
	other.subject = RequirementSubject{ "gametext", "gameerr.bin" };
	TEST_EXPECT(required != other && subject_target(required) == "gametext.bin" &&
	            requirement_subject(required)->role == "gametext" && !reference_subject(required));
	other.subject = ReferenceSubject{ ReferenceKind::TextTable, "gametext.bin" };
	TEST_EXPECT(required != other && subject_target(other) == "gametext.bin" && !requirement_subject(other));
	return 0;
}

// What a finding is about goes on the wire as it did (S13 A6 moved the members into its
// subject, the keys unchanged): a required file's role and target; a reference's target, kind,
// scope and loader's argument, each only when it holds something; nothing more for neither.
static int test_subject_keys() {
	const auto keys = [](const Diagnostic &d) {
		std::vector<std::string> out;
		for (const io::JsonMember &member : diagnostic_to_json(d).object) out.push_back(member.key);
		return out;
	};
	using Keys = std::vector<std::string>;
	Diagnostic plain = make_finding(CoreFinding::GraphUnreadable, DiagnosticSeverity::Warning, "Unread.", "a.mnu");
	TEST_EXPECT(keys(plain) == Keys({ "severity", "code", "message", "asset" }));
	Diagnostic required = make_finding(CoreFinding::RequirementMissing, DiagnosticSeverity::Error, "Missing.");
	required.subject = RequirementSubject{ "gametext", "gametext.bin" };
	TEST_EXPECT(keys(required) == Keys({ "severity", "code", "message", "role", "target" }));
	const io::JsonValue json = diagnostic_to_json(required);
	TEST_EXPECT(json.get_string("role", "") == "gametext" && json.get_string("target", "") == "gametext.bin");
	required.subject = RequirementSubject{ "", "mystery.bin" };
	TEST_EXPECT(keys(required) == Keys({ "severity", "code", "message", "target" }));
	Diagnostic missing = make_finding(CoreFinding::ReferenceMissing, DiagnosticSeverity::Error, "Missing.",
	                                  "models/tank.3di", "name");
	missing.subject = ReferenceSubject{ ReferenceKind::Texture, "skin.tga", std::string(), 0 };
	TEST_EXPECT(keys(missing) == Keys({ "severity", "code", "message", "asset", "field", "target", "reference",
	                                    "loader_arg" }));
	const io::JsonValue texture = diagnostic_to_json(missing);
	TEST_EXPECT(texture.get_string("reference", "") == "texture" && texture.get_number("loader_arg", -1.0) == 0.0);
	missing.subject = ReferenceSubject{ ReferenceKind::TextId, "NO_ID", "GAMETEXT.BIN/menu" };
	TEST_EXPECT(keys(missing) == Keys({ "severity", "code", "message", "asset", "field", "target", "reference",
	                                    "scope" }));
	TEST_EXPECT(diagnostic_to_json(missing).get_string("scope", "") == "GAMETEXT.BIN/menu");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_tokens_unique();
	failures += test_lookup();
	failures += test_type_tables();
	failures += test_columns();
	failures += test_make_finding();
	failures += test_subject_keys();
	return failures == 0 ? 0 : 1;
}
