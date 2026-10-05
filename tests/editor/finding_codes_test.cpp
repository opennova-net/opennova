// S13 A6: the finding codes (ADR 0046). Every finding is made from a row of a table, the editor's
// own (CoreFinding, model/finding_code_row) or a document type's (DocumentType::findings), each
// table static_asserted into its enum's order, and keeps it (Diagnostic::row). Here, over the
// tables as the registry holds them: every token is its own across every table; finding_row finds
// each row by its token and nothing else, a test's stand-in hiding none; a type's rows are one
// family, its group's; the menu's render-check rows are the compiler notes' and the stylesheet's
// reader rows the stylesheet reader's codes; the columns say what the fixes, the file index and
// the Problems location decided by the code's spelling before, and the spelling still agrees
// (a naming oracle); a finding keeps its row; and a finding's subject is written with the keys
// the wire had.
#include <editor/documents/animation_document.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/catalog_validation.h>
#include <editor/documents/document_types.h>
#include <editor/documents/mns_document.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/model_document.h>
#include <editor/documents/strings_document.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/diagnostic.h>
#include <editor/preview/menu_render_check.h>
#include <editor/session/finding_codes.h>
#include <editor/session/session_json.h>
#include <base/io/json.h>
#include <formats/mns/mns_document.h>
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

// Every table as the registry holds it: the core's, then each registered type's, with whose it is.
struct Table {
	std::string owner;
	FindingTable rows;
};
std::vector<Table> tables() {
	std::vector<Table> out{ { "core", core_finding_codes() } };
	for (size_t i = 1; i <= kDocumentTypeCount; ++i) {
		const DocumentType *type = registered_document_type(static_cast<DocumentTypeId>(i));
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

bool starts_with(const std::string &text, const std::string &head) {
	return text.compare(0, head.size(), head) == 0;
}
bool ends_with(const std::string &text, const std::string &tail) {
	return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

} // namespace

// Every row has a dotted token, none shared across the tables (the core's and every type's); a
// Rewrite's words exactly on a Rewrite row, and no Rewrite on a row that blocks the save;
// finding_tables is the registry's walk; the columns' wire forms are each their own.
static int test_tokens_unique() {
	std::map<std::string, std::string> owners; // a token -> the table declaring it
	size_t rows = 0;
	for (const Table &table : tables()) {
		// Every table has rows but the text type's (S13 D9): the game reads its files through readers
		// the editor does not model, so it makes no finding of its own.
		TEST_EXPECT(table.rows.count > 0 || table.owner == "text");
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
	const std::vector<Table> walked = tables();
	const std::vector<NamedFindingTable> named = finding_tables();
	TEST_EXPECT(named.size() == walked.size());
	for (size_t i = 0; i < named.size() && i < walked.size(); ++i) {
		TEST_EXPECT(walked[i].owner == named[i].owner && walked[i].rows.rows == named[i].rows.rows &&
		            walked[i].rows.count == named[i].rows.count);
		for (const FindingCodeRow &row : named[i].rows)
			TEST_EXPECT(finding_owner(&row) && std::string(finding_owner(&row)) == named[i].owner);
	}
	const FindingCodeRow stray{ "stray.code" };
	TEST_EXPECT(!finding_owner(&stray) && !finding_owner(nullptr));
	std::set<std::string> fixes;
	for (const FindingFix fix : { FindingFix::None, FindingFix::Requirement, FindingFix::WrongKind, FindingFix::Rename,
	                              FindingFix::ResetRow, FindingFix::Reference, FindingFix::UnimportedTexture,
	                              FindingFix::Reload, FindingFix::Reimport, FindingFix::Rewrite, FindingFix::TextureRows,
	                              FindingFix::ImportFitsUse })
		TEST_EXPECT(fixes.insert(finding_fix_token(fix)).second && !std::string(finding_fix_token(fix)).empty());
	TEST_EXPECT(std::string(finding_place_token(FindingPlace::Content)) == "content" &&
	            std::string(finding_place_token(FindingPlace::File)) == "file");
	TEST_EXPECT(std::string(finding_problem_token(FindingProblem::None)) == "none" &&
	            std::string(finding_problem_token(FindingProblem::Info)) == "info" &&
	            std::string(finding_problem_token(FindingProblem::Warning)) == "warning");
	std::printf("%zu finding codes in %zu tables (%zu the editor's own)\n", rows, tables().size(),
	            kCoreFindingCount);
	return 0;
}

// finding_row finds each row by its token (the row itself), none for a token no table declares; a
// code's row is the one its enumerator names.
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

// A test's stand-in in a registered type's place (S13 D6), with no finding codes of its own,
// hides none of that type's: the lookup and the tables read the registry as registered.
static int test_stand_in_hides_nothing() {
	const FindingCodeRow *unused = finding_row("style.unused");
	TEST_EXPECT(unused == &finding_code(StyleFinding::Unused));
	DocumentType blank_styles;
	blank_styles.id = DocumentTypeId::Styles;
	blank_styles.name = "blank";
	blank_styles.make = registered_document_type(DocumentTypeId::Styles)->make;
	blank_styles.validate_file = registered_document_type(DocumentTypeId::Styles)->validate_file;
	{
		DocumentTypeStandIn stand_in(blank_styles);
		TEST_EXPECT(document_type(DocumentTypeId::Styles) == &blank_styles);
		TEST_EXPECT(finding_row("style.unused") == unused && finding_row("style.line_ending"));
		bool styles = false;
		for (const NamedFindingTable &table : finding_tables())
			styles = styles || (std::string(table.owner) == "styles" && table.rows.holds(unused));
		TEST_EXPECT(styles);
	}
	return 0;
}

// A type's codes are one family, its table's group: every token starts with its group's key; the
// menu's render check has a row per compiler note, its token menu.render. and the note's, found by
// the note; the stylesheet's reader rows are the stylesheet reader's codes, style. and the code's
// token with '_' for '-' (its line-ending the validator's line_ending).
static int test_type_tables() {
	for (size_t i = 1; i <= kDocumentTypeCount; ++i) {
		const DocumentType *type = registered_document_type(static_cast<DocumentTypeId>(i));
		TEST_EXPECT(type && type->findings);
		if (!type || !type->findings) continue;
		const FindingTable table = type->findings();
		if (table.count == 0) continue; // the text type's
		const FindingGroup group = table.rows[0].group;
		for (const FindingCodeRow &row : table)
			TEST_EXPECT(row.group == group && starts_with(row.token, std::string(finding_group_key(group)) + "."));
	}
	for (int i = 0; i < menu::kMenuFrameNoteCodeCount; ++i) {
		const auto note = static_cast<menu::MenuFrameNoteCode>(i);
		const FindingCodeRow &row = finding_code(note);
		TEST_EXPECT(std::string(row.token) == std::string("menu.render.") + menu::menu_frame_note_token(note));
		TEST_EXPECT(finding_row(row.token) == &row && menu_finding_codes().holds(&row) &&
		            row.source == FindingSource::RenderCheck && row.group == FindingGroup::Menus);
	}
	TEST_EXPECT(menu_finding_codes().count ==
	            static_cast<size_t>(MenuFinding::kCount) + size_t(menu::kMenuFrameNoteCodeCount));
	// Whether a note is a Problems row, and at what severity, is its row's (the list the render
	// check's switch held): menu_note_problem reads it.
	for (int i = 0; i < menu::kMenuFrameNoteCodeCount; ++i) {
		const auto note = static_cast<menu::MenuFrameNoteCode>(i);
		const FindingProblem problem = finding_code(note).problem;
		DiagnosticSeverity severity = DiagnosticSeverity::Error;
		const bool listed = menu_note_problem(note, &severity);
		TEST_EXPECT(listed == (problem != FindingProblem::None));
		TEST_EXPECT(!listed || severity == (problem == FindingProblem::Info ? DiagnosticSeverity::Info
		                                                                    : DiagnosticSeverity::Warning));
	}
	using Tokens = std::vector<std::string>;
	const auto noted = [](FindingProblem problem) {
		return tokens_where([problem](const FindingCodeRow &row) {
			return row.source == FindingSource::RenderCheck && row.problem == problem &&
			       std::string(row.token) != "menu.render.mapping";
		});
	};
	TEST_EXPECT(noted(FindingProblem::None) ==
	            Tokens({ "menu.render.appearance_custom", "menu.render.font_missing",
	                     "menu.render.frame_stencil_unloaded", "menu.render.marquee_runtime_content",
	                     "menu.render.scroll_extent_default", "menu.render.state_fallback",
	                     "menu.render.style_var_unresolved", "menu.render.table_cells_custom",
	                     "menu.render.table_no_columns",
	                     "menu.render.text_id_missing", "menu.render.text_table_missing",
	                     "menu.render.texture_missing" }));
	TEST_EXPECT(noted(FindingProblem::Info) ==
	            Tokens({ "menu.render.image_height_shared", "menu.render.item_kind_not_drawn",
	                     "menu.render.list_rows_clipped", "menu.render.table_cells_deferred",
	                     "menu.render.type_interior_deferred" }));
	TEST_EXPECT(noted(FindingProblem::Warning).size() == 21);
	// Only a render check's row says it; its screen it could not map is an Error its check makes.
	TEST_EXPECT(tokens_where([](const FindingCodeRow &row) {
		            return row.problem != FindingProblem::None && row.source != FindingSource::RenderCheck;
	            }).empty());
	TEST_EXPECT(finding_code(MenuFinding::RenderMapping).problem == FindingProblem::None);
	for (size_t i = 0; i < mns::kDiagnosticCodeCount; ++i) {
		const auto code = static_cast<mns::DiagnosticCode>(i);
		std::string token = "style.";
		for (const char *c = mns::diagnostic_code_token(code); *c; ++c) token += *c == '-' ? '_' : *c;
		const FindingCodeRow &row = finding_code(code);
		if (token != row.token)
			std::fprintf(stderr, "%s reads as %s\n", mns::diagnostic_code_token(code), row.token);
		TEST_EXPECT(token == row.token && finding_row(token) == &row && style_finding_codes().holds(&row));
	}
	TEST_EXPECT(&finding_code(mns::DiagnosticCode::LineEnding) == &finding_code(StyleFinding::LineEnding));
	return 0;
}

// The columns decide what the codes' spelling decided before (S13 A6 moved the rules into the
// rows): the fixes of each family (problem_fixes' collect), the Rewrite's words (its
// rewrite_does), the files that do not serialize (ProblemFixIndex's suffix test: .unserializable,
// .invalid_input), and the findings about the file as a whole (the Problems location's
// asset.name.*, build.name_unstorable, build.archive_in_project); and the spelling still agrees
// with them, a naming oracle over every row: the suffixes exactly on the rows that block the save,
// asset.name. only on a row about the file as a whole. Every row's group has the family its token
// starts with; the graph's two rows and the render check's are where they come from.
static int test_columns() {
	using Tokens = std::vector<std::string>;
	TEST_EXPECT(fixed_by(FindingFix::Requirement) ==
	            Tokens({ "play.boot_missing", "requirement.missing", "requirement.optional_missing" }));
	TEST_EXPECT(fixed_by(FindingFix::WrongKind) == Tokens({ "requirement.wrong_kind" }));
	TEST_EXPECT(fixed_by(FindingFix::Rename) ==
	            Tokens({ "asset.name.duplicate", "asset.name.too_long", "build.name_unstorable", "expansion.file.unread" }));
	TEST_EXPECT(fixed_by(FindingFix::ResetRow) == Tokens({ "animation_map.no_reset" }));
	TEST_EXPECT(fixed_by(FindingFix::Reference) == Tokens({ "reference.missing" }));
	TEST_EXPECT(fixed_by(FindingFix::UnimportedTexture) == Tokens({ "import.texture_not_imported" }));
	TEST_EXPECT(fixed_by(FindingFix::Reload) == Tokens({ "document.conflict" }));
	TEST_EXPECT(fixed_by(FindingFix::Reimport) == Tokens({ "import.output_missing" }));
	// S18: an upside-down TGA's rows; every use's finding where an import makes the file it reads.
	TEST_EXPECT(fixed_by(FindingFix::TextureRows) == Tokens({ "texture.tga_upside_down" }));
	TEST_EXPECT(fixed_by(FindingFix::ImportFitsUse) ==
	            Tokens({ "texture.alpha_not_loaded", "texture.blend_map_size", "texture.colormap_size", "texture.foliage_map_overrun",
	                     "texture.foliage_map_shape", "texture.height_wrap", "texture.loading_screen_size", "texture.memory",
	                     "texture.mfd_not_pow2", "texture.normal_map_halved", "texture.particle_too_big",
	                     "texture.tile_atlas_cells", "texture.wrong_reader" }));
	// A finished normal map a normal-map slot's row loads as a diffuse: its row given type 4.
	TEST_EXPECT(fixed_by(FindingFix::NormalRowType) == Tokens({ "texture.normal_slot_loader" }));
	// (A catalog's input the game ignores has none: a save keeps it as the file has it, the demo round's bug 3.)
	TEST_EXPECT(fixed_by(FindingFix::Rewrite) ==
	            Tokens({ "animation_map.ignored_input", "credits.line_ending",
	                     "menu.ignored_input", "mission.event_order", "mission.rewrite_differs", "script.line_ending",
	                     "shader.form", "strings.regrouped", "style.line_ending" }));
	const std::map<std::string, std::string> rewrites = {
		{ "animation_map.ignored_input", "without the input the game ignores" },
		{ "credits.line_ending", "with every line ending CR LF" },
		{ "menu.ignored_input", "without the input the game ignores" },
		{ "mission.event_order", "with each event's triggers and actions where the event stands" },
		{ "mission.rewrite_differs", "with its sections as the game reads them" },
		{ "script.line_ending", "with every line ending CR LF" },
		{ "shader.form", "in the SCR form the game's shader loader takes" },
		{ "strings.regrouped", "with its strings grouped by section the way the game reads them" },
		{ "style.line_ending", "with every line ending CR LF" },
	};
	for (const auto &[token, does] : rewrites) {
		const FindingCodeRow *row = finding_row(token);
		TEST_EXPECT(row && row->rewrite_does && does == row->rewrite_does);
	}
	TEST_EXPECT(tokens_where([](const FindingCodeRow &row) { return row.blocks_save; }) ==
	            Tokens({ "animation_map.invalid_input", "catalog.invalid_input", "catalog.unserializable",
	                     "credits.invalid_input", "credits.unserializable", "document.unserializable",
	                     "menu.invalid_input", "menu.unserializable", "mission.invalid_input", "music_script.invalid_input",
	                     "music_script.unserializable", "strings.invalid_input" }));
	TEST_EXPECT(tokens_where([](const FindingCodeRow &row) { return row.place == FindingPlace::File; }) ==
	            Tokens({ "asset.name.duplicate", "asset.name.empty", "asset.name.too_long", "build.archive_in_project",
	                     "build.expansion.mission_twice", "build.expansion.mission_untitled", "build.expansion.root_only",
	                     "build.name_unstorable", "build.unread", "expansion.file.unread" }));
	// S14, the build follows retail: the codes whose errors gate no build are listed (shown, fixable,
	// never blocking): the ones whose subject names the game's refusal where it is witnessed (a
	// reference, a required file), and the ones the audit found no refusal of the game's behind (the
	// editor's own rules, a read past a table, a stylesheet read otherwise or read up to a line). An
	// error of any other row blocks, as does an error made from no row. A row that says its file does
	// not serialize always gates.
	TEST_EXPECT(tokens_where([](const FindingCodeRow &row) { return !row.gates_build; }) ==
	            Tokens({ "animation_map.no_reset", "build.expansion.exp_desc", "build.expansion.mission_twice",
	                     "build.expansion.mission_untitled", "build.expansion.root_only", "build.unread", "catalog.item_type",
	                     "catalog.name_empty", "expansion.file.unread", "export.cancelled", "export.cleanup", "export.replaced",
	                     "mission.event_missing",
	                     "mission.group_range", "model.frame_missing", "model.light_part", "model.register_missing",
	                     "project.expansion.name_taken", "project.expansion.not_installed", "reference.missing", "reference.wrong_kind", "requirement.missing", "requirement.wrong_kind",
	                     "shader.form", "strings.key_empty", "strings.section_empty", "style.continued_duplicate",
	                     "style.directive_form", "style.directive_tail", "style.if_without_argument",
	                     "style.invalid_name_char", "style.missing_value_delimiter", "style.nul_byte", "style.stops",
	                     "style.value_is_directive" }));
	TEST_EXPECT(finding_row("model.light_no_registers") && finding_row("model.light_no_registers")->gates_build &&
	            finding_row("style.hangs")->gates_build && finding_row("style.line_ending")->gates_build);
	// A missing required file blocks where its manifest row is the boot's refusal (gametext.bin: the
	// boot exits), never where the game boots on (weapon.def: a single None weapon).
	Diagnostic gametext = make_finding(CoreFinding::RequirementMissing, DiagnosticSeverity::Error, "required");
	gametext.subject = RequirementSubject{ "gametext", "gametext.bin" };
	Diagnostic weapons = make_finding(CoreFinding::RequirementMissing, DiagnosticSeverity::Error, "required");
	weapons.subject = RequirementSubject{ "weapon_def", "weapon.def" };
	// A file of another kind behind a fatal row's name gates as its absence does (the boot reads it as
	// its table unchecked); behind any other row it is listed.
	Diagnostic wrong = make_finding(CoreFinding::RequirementWrongKind, DiagnosticSeverity::Error, "wrong kind");
	wrong.subject = RequirementSubject{ "gametext", "gametext.bin" };
	Diagnostic wrong_weapons = make_finding(CoreFinding::RequirementWrongKind, DiagnosticSeverity::Error, "wrong kind");
	wrong_weapons.subject = RequirementSubject{ "weapon_def", "weapon.def" };
	TEST_EXPECT(blocks_build(gametext) && !blocks_build(weapons) && blocks_build(wrong) && !blocks_build(wrong_weapons));
	TEST_EXPECT(!blocks_build(make_finding(CoreFinding::ReferenceMissing, DiagnosticSeverity::Error, "missing")) &&
	            !blocks_build(make_finding(CoreFinding::RequirementMissing, DiagnosticSeverity::Error, "no subject")) &&
	            !blocks_build(make_finding(CoreFinding::RequirementMissing, DiagnosticSeverity::Warning, "a warning")) &&
	            blocks_build(make_finding(CoreFinding::DocumentStale, DiagnosticSeverity::Error, "stale")) &&
	            blocks_build(Diagnostic{}));
	TEST_EXPECT(!diagnostics_block_build({ make_finding(CoreFinding::ReferenceMissing, DiagnosticSeverity::Error, "missing") }) &&
	            diagnostics_block_build({ make_finding(CoreFinding::ReferenceMissing, DiagnosticSeverity::Error, "missing"),
	                                      make_finding(CoreFinding::DocumentStale, DiagnosticSeverity::Error, "stale") }));
	// Review F3: where its kind's row cites the game's refusal (gates_when_missing, the terrain's), a
	// missing reference gates; one of any other kind does not. A file of the name of the wrong kind
	// is its own code, gating where its kind's missing name does (its loader finds nothing it loads).
	Diagnostic terrain = make_finding(CoreFinding::ReferenceMissing, DiagnosticSeverity::Error, "no terrain");
	terrain.subject = ReferenceSubject{ ReferenceKind::Terrain, "nowhere.trn", "", -1 };
	Diagnostic sound = make_finding(CoreFinding::ReferenceMissing, DiagnosticSeverity::Error, "no sound");
	sound.subject = ReferenceSubject{ ReferenceKind::Sound, "nothing.wav", "", -1 };
	TEST_EXPECT(blocks_build(terrain) && !blocks_build(sound));
	Diagnostic wrong_terrain = make_finding(CoreFinding::ReferenceWrongKind, DiagnosticSeverity::Error, "wrong kind");
	wrong_terrain.subject = ReferenceSubject{ ReferenceKind::Terrain, "island.trn", "", -1 };
	Diagnostic wrong_texture = make_finding(CoreFinding::ReferenceWrongKind, DiagnosticSeverity::Error, "wrong kind");
	wrong_texture.subject = ReferenceSubject{ ReferenceKind::Texture, "notes.txt", "", -1 };
	TEST_EXPECT(blocks_build(wrong_terrain) && !blocks_build(wrong_texture) &&
	            !blocks_build(make_finding(CoreFinding::ReferenceWrongKind, DiagnosticSeverity::Error, "wrong kind")));
	std::vector<std::string> gating_kinds;
	for (size_t k = 0; k < kReferenceKindCount; ++k)
		if (reference_row(static_cast<ReferenceKind>(k)).gates_when_missing)
			gating_kinds.push_back(reference_row(static_cast<ReferenceKind>(k)).token);
	TEST_EXPECT(gating_kinds == std::vector<std::string>({ "terrain" }));
	std::set<std::string> keys, titles;
	for (size_t g = 1; g < kFindingGroupCount; ++g) {
		const auto group = static_cast<FindingGroup>(g);
		TEST_EXPECT(keys.insert(finding_group_key(group)).second && titles.insert(finding_group_title(group)).second);
	}
	for (const Table &table : tables()) {
		for (const FindingCodeRow &row : table.rows) {
			const std::string token = row.token;
			TEST_EXPECT(row.blocks_save == (ends_with(token, ".unserializable") || ends_with(token, ".invalid_input")));
			TEST_EXPECT(!row.blocks_save || row.gates_build);
			TEST_EXPECT(!starts_with(token, "asset.name.") || row.place == FindingPlace::File);
			const std::string key = finding_group_key(row.group);
			TEST_EXPECT(row.group != FindingGroup::None && (token == key || starts_with(token, key + ".")));
			const std::string source = finding_source_token(row);
			TEST_EXPECT(row.source == FindingSource::Own ? source == key
			            : row.source == FindingSource::Graph ? source == "graph"
			                                                 : source == "render");
		}
	}
	TEST_EXPECT(tokens_where([](const FindingCodeRow &row) { return row.source == FindingSource::Graph; }) ==
	            Tokens({ "graph.unreadable", "reference.missing", "reference.wrong_kind" }));
	TEST_EXPECT(tokens_where([](const FindingCodeRow &row) { return row.source == FindingSource::RenderCheck; }) ==
	            tokens_where([](const FindingCodeRow &row) { return starts_with(row.token, "menu.render."); }));
	TEST_EXPECT(std::string(finding_group_key(finding_code(CoreFinding::RequirementOptionalMissing).group)) ==
	                    "requirement.optional_missing" &&
	            std::string(finding_group_title(finding_code(CoreFinding::RequirementOptionalMissing).group)) ==
	                    "Optional files");
	// The mission type's family (S14), its codes "mission.*".
	TEST_EXPECT(std::string(finding_group_key(FindingGroup::Missions)) == "mission" &&
	            std::string(finding_group_title(FindingGroup::Missions)) == "Missions");
	return 0;
}

// A finding keeps the row it was made from, its code the row's token; made from a row or its
// enumerator alike; about nothing more until its producer says what; a Diagnostic no finding was
// made into has no row and no code.
static int test_make_finding() {
	const Diagnostic by_row = make_finding(finding_code(StyleFinding::Unused), DiagnosticSeverity::Info,
	                                       "No menu uses it.", "menus/menu_style.mns", "value");
	const Diagnostic by_code = make_finding(StyleFinding::Unused, DiagnosticSeverity::Info, "No menu uses it.",
	                                        "menus/menu_style.mns", "value");
	TEST_EXPECT(by_row == by_code && by_row.row() == &finding_code(StyleFinding::Unused) &&
	            by_row.code() == "style.unused" && by_row.severity == DiagnosticSeverity::Info &&
	            by_row.message == "No menu uses it." && by_row.asset == "menus/menu_style.mns" &&
	            by_row.field == "value" && std::holds_alternative<std::monostate>(by_row.subject) &&
	            !requirement_subject(by_row) && !reference_subject(by_row) && subject_target(by_row).empty());
	const Diagnostic note = make_finding(finding_code(menu::MenuFrameNoteCode::TextTruncated),
	                                     DiagnosticSeverity::Warning, "Cut.");
	TEST_EXPECT(note.row() == finding_row("menu.render.text_truncated") && note.code() == "menu.render.text_truncated");
	const Diagnostic core = make_finding(CoreFinding::GraphUnreadable, DiagnosticSeverity::Warning, "Unread.");
	TEST_EXPECT(core.code() == "graph.unreadable" && core.asset.empty() && core.field.empty());
	const Diagnostic none;
	TEST_EXPECT(!none.row() && none.code().empty() && none != core);
	// Two findings alike but for what they are about, or their row, differ.
	Diagnostic required = make_finding(CoreFinding::RequirementMissing, DiagnosticSeverity::Error, "Missing.");
	Diagnostic other = required;
	required.subject = RequirementSubject{ "gametext", "gametext.bin" };
	other.subject = RequirementSubject{ "gametext", "gameerr.bin" };
	TEST_EXPECT(required != other && subject_target(required) == "gametext.bin" &&
	            requirement_subject(required)->role == "gametext" && !reference_subject(required));
	other.subject = ReferenceSubject{ ReferenceKind::TextTable, "gametext.bin" };
	TEST_EXPECT(required != other && subject_target(other) == "gametext.bin" && !requirement_subject(other));
	Diagnostic optional = make_finding(CoreFinding::RequirementOptionalMissing, DiagnosticSeverity::Error, "Missing.");
	optional.subject = required.subject;
	TEST_EXPECT(optional != required);
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
	TEST_EXPECT(diagnostic_to_json(plain).get_string("code", "") == "graph.unreadable");
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
	failures += test_stand_in_hides_nothing();
	failures += test_type_tables();
	failures += test_columns();
	failures += test_make_finding();
	failures += test_subject_keys();
	return failures == 0 ? 0 : 1;
}
