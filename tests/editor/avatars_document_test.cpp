// The avatars table document (ADR 0046 S23 B; editor/documents/avatars_document.h): Avatars.def as rows over
// formats/avatars with the file's layout modeled (formats/textlayout): its parts (a head, a body, arms: a model and
// a text key each) and its nationalities, each holding its divisions, each its combinations of a head, a body and
// arms; a value changed changes its own line; the graph reads the document's records (a part's models, its shown
// name a key of Game.bin's Avatars section, a combination's parts by name in the file's scope of their kind, the
// earlier of two parts of a name inert where no combination binds it); the reader's notes as findings at their
// lines; the order the game binds a combination's parts in (the last of a name above it, in the text a save
// writes); a file the reader stops in held read only with the gating finding; in a session, an
// edit of a head's model moves the graph's reference with it, and Undo moves it back. The retail leg
// (OPENNOVA_JO_DIR): the install's Avatars.def (its base archives and each expansion's) through the document, saved
// byte for byte.
#include <editor/documents/avatars_document.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>

#include <base/vfs/vfs.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

constexpr NodeKind kPart = node_kind(AvatarsKind::Part);
constexpr NodeKind kNationality = node_kind(AvatarsKind::Nationality);
constexpr NodeKind kDivision = node_kind(AvatarsKind::Division);
constexpr NodeKind kCombo = node_kind(AvatarsKind::Combo);

std::vector<uint8_t> bytes_of(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

Edit set_edit(const NodeAddress &at, const char *field, Value value) {
	Edit edit;
	edit.address = at;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

const std::string kFile =
		"// the parts\r\n"
		"define head HEAD_A\r\n{\r\n\tname\t\tAV_HEAD_A\r\n\tgraphic\t\thead_a.3di\r\n\tcamo\t\t0 0 0\r\n\tvoice\t\t1\r\n\tsex\t\tm\r\n}\r\n"
		"define head HEAD_A\r\n{\r\n\tgraphic\t\thead_a2.3di\r\n}\r\n"
		"define body BODY_A\r\n{\r\n\tgraphic\t\tbody_a.3di\r\n}\r\n"
		"define arms ARMS_A\r\n{\r\n\tgraphic\t\tarms_a.3di\r\n}\r\n"
		"\r\n"
		"nationality N00 AV_NAT_US\r\n{\r\n\talignment good\r\n"
		"\tdivision D00 AV_DIV_ARMY\r\n\t{\r\n\t\tcombo 1 HEAD_A BODY_A ARMS_A\r\n\t}\r\n}\r\n";

int test_rows() {
	AvatarsDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(bytes_of(kFile), "defs/Avatars.def", AssetKind::AvatarDefs, "jo", error));
	TEST_EXPECT(document.rows().size() == 5 && !document.blocked());
	TEST_EXPECT(document.serialize().text == kFile);
	const auto &first = static_cast<const AvatarPartRow &>(*document.rows()[0]);
	TEST_EXPECT(first.part.name == "HEAD_A" && first.part.display_name == "AV_HEAD_A" && first.part.voice == 1);
	TEST_EXPECT(document.record_title({first.id, kPart, 0}) == "Head HEAD_A");
	const auto &nationality = static_cast<const AvatarNationalityRow &>(*document.rows()[4]);
	TEST_EXPECT(nationality.nationality.divisions.size() == 1 && nationality.nationality.divisions[0].combos.size() == 1);
	TEST_EXPECT(document.record_title({nationality.id, kNationality, 0}) == "Nationality N00 AV_NAT_US");
	// A head's model changed: its own line.
	TEST_EXPECT(document.apply(set_edit({first.id, kPart, 0}, "graphic", std::string("head_b.3di")), error));
	std::string expected = kFile;
	expected.replace(expected.find("head_a.3di"), 10, "head_b.3di");
	TEST_EXPECT(document.serialize().text == expected);
	// A word of the walk holds no blank.
	TEST_EXPECT(!document.apply(set_edit({first.id, kPart, 0}, "graphic", std::string("head b.3di")), error));
	// A combination's parts name parts of their kinds in the file's scopes; a part defines its name there.
	const NodeId division_id = nationality.ids.lists[0][0].id;
	NodeAddress combo{};
	document.walk_records(nationality, [&](const NodeAddress &nested, const Document::Placement &) {
		if (nested.kind == kCombo) combo = nested;
		return true;
	});
	TEST_EXPECT(division_id != 0 && combo.kind == kCombo && combo.child != 0);
	for (const FieldSchema &field : document.fields(kCombo))
		if (field.id == "head") {
			const FieldUse use = document.field_on(combo, field);
			TEST_EXPECT(use.reference == ReferenceKind::AvatarPart && use.scope == "AVATARS.DEF/HEAD");
		}
	TEST_EXPECT(avatar_part_scope("defs/Avatars.def", avatars::AVATAR_PART_ARMS) == "AVATARS.DEF/ARMS");
	// A part added: a head named apart from the file's; a nationality the first free id.
	Edit add;
	add.operation = EditOperation::Add;
	add.address = {0, kPart, 0};
	TEST_EXPECT(document.apply(add, error));
	add.address = {0, kNationality, 0};
	TEST_EXPECT(document.apply(add, error));
	AvatarsDocument again;
	TEST_EXPECT(again.load_bytes(bytes_of(document.serialize().text), "defs/Avatars.def", AssetKind::AvatarDefs, "jo", error) &&
	            again.rows().size() == 7);
	TEST_EXPECT(static_cast<const AvatarNationalityRow &>(*again.rows()[6]).nationality.raw_id == "N01");
	// A division goes into a nationality, not the rows.
	add.address = {0, kDivision, 0};
	TEST_EXPECT(!document.apply(add, error));
	std::printf("rows: the parts and nationalities, a value's own line, a combination's parts in their kinds' scopes, a "
	            "part and a nationality added\n");
	return 0;
}

// The graph reads the document's records: the models, the shown name in Game.bin's Avatars section, the parts a
// combination names, the earlier of two heads of a name inert.
int test_graph() {
	Extracted out;
	Diagnostic error;
	TEST_EXPECT(extract_from_bytes("Avatars.def", AssetKind::AvatarDefs, bytes_of(kFile), "jo", out, error));
	const auto edge = [&](ReferenceKind kind, const std::string &value, const std::string &scope) {
		return std::any_of(out.edges.begin(), out.edges.end(), [&](const GraphEdge &e) {
			return e.kind == kind && e.value == value && e.scope == scope;
		});
	};
	TEST_EXPECT(edge(ReferenceKind::Model, "head_a.3di", "") && edge(ReferenceKind::Model, "arms_a.3di", ""));
	TEST_EXPECT(edge(ReferenceKind::TextId, "AV_HEAD_A", "GAME.BIN/Avatars") &&
	            edge(ReferenceKind::TextId, "AV_NAT_US", "GAME.BIN/Avatars"));
	TEST_EXPECT(edge(ReferenceKind::AvatarPart, "HEAD_A", "AVATARS.DEF/HEAD") &&
	            edge(ReferenceKind::AvatarPart, "ARMS_A", "AVATARS.DEF/ARMS"));
	// The earlier HEAD_A, which no combination binds (the one combination comes after both), defines its name in a
	// section of its rank, inert.
	size_t heads = 0, inert = 0;
	for (const GraphSymbol &symbol : out.symbols)
		if (symbol.kind == ReferenceKind::AvatarPart && symbol.scope.rfind("AVATARS.DEF/HEAD", 0) == 0) {
			++heads;
			inert += symbol.inert && symbol.scope == "AVATARS.DEF/HEAD#1" ? 1 : 0;
		}
	TEST_EXPECT(heads == 2 && inert == 1);
	std::printf("graph: %zu edges, %zu symbols from the document's records\n", out.edges.size(), out.symbols.size());
	return 0;
}

// The reader's notes, each at its line, listed; the session's graph follows an edit and its undo.
int test_findings_and_session() {
	const std::string twice = "nationality N00 FIRST\r\n{\r\n}\r\nnationality N00 DUP_NAT\r\n{\r\n}\r\n";
	AvatarsDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(bytes_of(twice), "Avatars.def", AssetKind::AvatarDefs, "jo", error));
	const std::vector<Diagnostic> findings = validate_avatars_file(document);
	TEST_EXPECT(findings.size() == 1 && findings[0].code() == "avatars.ignored_input" &&
	            findings[0].severity == DiagnosticSeverity::Warning && findings[0].line == 4 &&
	            findings[0].message.find("duplicate nationality") != std::string::npos);
	TEST_EXPECT(document.serialize().text == twice);

	editor_test::TempProjectDir dir("opennova_editor_avatars_document");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Avatars"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
	TEST_EXPECT(editor_test::write_text(root + "/Avatars.def", twice + "\r\n" +
	                                    "define head HEAD_A\r\n{\r\n\tgraphic\t\thead_a.3di\r\n\tcamo\t\t0 0 0\r\n"
	                                    "\tvoice\t\t1\r\n\tsex\t\tm\r\n}\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	const auto names = [&session](const std::string &value) {
		for (const GraphEdge *edge : session.view().findings.graph->references_of("Avatars.def"))
			if (edge->kind == ReferenceKind::Model && edge->value == value) return true;
		return false;
	};
	TEST_EXPECT(names("head_a.3di"));
	size_t noted = 0;
	for (const Diagnostic &d : session.view().findings.diagnostics)
		noted += d.code() == "avatars.ignored_input" && d.asset == "Avatars.def" && d.line == 4;
	TEST_EXPECT(noted == 1);
	editor_test::handle_to_end(session, request::open_document("Avatars.def"));
	const auto *open = dynamic_cast<const AvatarsDocument *>(session.document_base_for("Avatars.def"));
	TEST_EXPECT(open != nullptr);
	if (!open) return 1;
	const Node *head = nullptr;
	for (const auto &row : open->rows())
		if (row && row->kind == kPart) head = row.get();
	TEST_EXPECT(head != nullptr);
	if (!head) return 1;
	editor_test::handle_to_end(session, request::edit_record("Avatars.def", set_edit({head->id, kPart, 0}, "graphic",
	                                                                                 std::string("head_b.3di"))));
	TEST_EXPECT(session.last_edit_ok() && open->dirty());
	TEST_EXPECT(names("head_b.3di") && !names("head_a.3di"));
	editor_test::handle_to_end(session, request::undo("Avatars.def"));
	TEST_EXPECT(names("head_a.3di") && !open->dirty());
	std::printf("findings: the reader's note at its line; session: the graph follows an edit and its undo\n");
	return 0;
}

// The game binds a combination's parts among those above it, the last of a name [orig: CAvatarDefs_ParseConfigLine
// @ 0x57A830..0x57A854]: a combination between two heads of a name binds the earlier, one after both the later, and
// neither head is inert; a combination set to a head the layout puts below it makes the save write the writer's form
// (every part first), so nothing is dropped; the reader stopping on the text a save writes is the gating finding.
int test_order() {
	const std::string between =
			"define head HEAD_A\r\n{\r\n\tgraphic\t\ta1.3di\r\n}\r\n"
			"define body BODY_A\r\n{\r\n\tgraphic\t\tb.3di\r\n}\r\n"
			"nationality N00 AV_NAT_US\r\n{\r\n\tdivision D00 AV_DIV_ARMY\r\n\t{\r\n\t\tcombo 1 HEAD_A BODY_A\r\n\t}\r\n}\r\n"
			"define head HEAD_A\r\n{\r\n\tgraphic\t\ta2.3di\r\n}\r\n"
			"define head HEAD_B\r\n{\r\n\tgraphic\t\thb.3di\r\n}\r\n"
			"define arms ARMS_B\r\n{\r\n\tgraphic\t\tab.3di\r\n}\r\n"
			"nationality N01 AV_NAT_UK\r\n{\r\n\tdivision D00 AV_DIV_ARMY\r\n\t{\r\n\t\tcombo 1 HEAD_A BODY_A\r\n\t}\r\n}\r\n";
	Extracted out;
	Diagnostic error;
	TEST_EXPECT(extract_from_bytes("Avatars.def", AssetKind::AvatarDefs, bytes_of(between), "jo", out, error));
	size_t first = 0, last = 0, inert = 0;
	for (const GraphSymbol &symbol : out.symbols)
		if (symbol.kind == ReferenceKind::AvatarPart && symbol.name == "HEAD_A") {
			first += symbol.scope == "AVATARS.DEF/HEAD#1";
			last += symbol.scope == "AVATARS.DEF/HEAD";
			inert += symbol.inert;
		}
	TEST_EXPECT(first == 1 && last == 1 && inert == 0);
	size_t to_first = 0, to_last = 0;
	for (const GraphEdge &e : out.edges)
		if (e.kind == ReferenceKind::AvatarPart && e.value == "HEAD_A") {
			to_first += e.scope == "AVATARS.DEF/HEAD#1";
			to_last += e.scope == "AVATARS.DEF/HEAD";
		}
	TEST_EXPECT(to_first == 1 && to_last == 1);

	AvatarsDocument document;
	TEST_EXPECT(document.load_bytes(bytes_of(between), "Avatars.def", AssetKind::AvatarDefs, "jo", error));
	TEST_EXPECT(validate_avatars_file(document).empty());
	NodeAddress combo{};
	for (const auto &row : document.rows())
		if (row && row->kind == kNationality && combo.kind != kCombo)
			document.walk_records(*row, [&](const NodeAddress &nested, const Document::Placement &) {
				if (nested.kind == kCombo) combo = nested;
				return true;
			});
	TEST_EXPECT(combo.kind == kCombo);
	// The first nationality's combination set to HEAD_B, which the layout puts below it: the text would not read
	// back as the rows (the game would drop it), so the save writes the writer's form, every part first, and says so;
	// nothing is dropped, so no finding. There both combinations bind the later HEAD_A, and the earlier is inert.
	TEST_EXPECT(document.apply(set_edit(combo, "head", std::string("HEAD_B")), error));
	const SerializeResult rewritten = document.serialize();
	TEST_EXPECT(rewritten.ok() && !rewritten.notes.empty() &&
	            rewritten.text.find("define arms ARMS_B") < rewritten.text.find("nationality N00"));
	TEST_EXPECT(validate_avatars_file(document).empty());
	size_t shadowed = 0;
	for (const auto &row : document.rows())
		if (row && row->kind == kPart && static_cast<const AvatarPartRow &>(*row).part.name == "HEAD_A") {
			SymbolFacts facts;
			static_cast<const Document &>(document).refine_symbol({row->id, kPart, 0}, facts);
			shadowed += facts.inert;
		}
	TEST_EXPECT(shadowed == 1);
	std::printf("order: a combination binds the last part of a name above it in the text a save writes\n");

	// Parts added past the reader's 512: the text a save writes stops the game's reader, the finding refusing a build.
	std::string near;
	for (int i = 0; i < 511; ++i) near += "define head H" + std::to_string(i) + "\r\n";
	near += "nationality N00 AV_NAT_US\r\n{\r\n}\r\n";
	AvatarsDocument growing;
	TEST_EXPECT(growing.load_bytes(bytes_of(near), "Avatars.def", AssetKind::AvatarDefs, "jo", error) &&
	            validate_avatars_file(growing).empty());
	Edit add;
	add.operation = EditOperation::Add;
	add.address = {0, kPart, 0};
	TEST_EXPECT(growing.apply(add, error));
	const std::vector<Diagnostic> grown = validate_avatars_file(growing);
	TEST_EXPECT(grown.size() == 1 && grown[0].code() == "avatars.reader_stops" && grown[0].row()->gates_build);

	// A file past the reader's 512 parts opens read only, holding the parts before the line it stops at, the
	// finding there refusing a build.
	std::string many;
	for (int i = 0; i < 513; ++i) many += "define head H" + std::to_string(i) + "\r\n";
	AvatarsDocument full;
	TEST_EXPECT(full.load_bytes(bytes_of(many), "Avatars.def", AssetKind::AvatarDefs, "jo", error));
	TEST_EXPECT(full.blocked() && full.rows().size() == 512 && full.reader_stops_line() == 513);
	const std::vector<Diagnostic> stops = validate_avatars_file(full);
	TEST_EXPECT(stops.size() == 1 && stops[0].code() == "avatars.reader_stops" && stops[0].line == 513 &&
	            stops[0].severity == DiagnosticSeverity::Error && stops[0].row()->gates_build &&
	            stops[0].row()->game_refusal != nullptr);
	std::printf("order: a file past the reader's 512 parts held read only, the reader's stop a gating finding\n");
	return 0;
}

int test_retail() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (the install's Avatars.def through the document)");
		return 0;
	}
	std::vector<std::string> mounts{std::string()};
	for (const std::string &expansion : retail::expansions()) mounts.push_back(expansion);
	for (const std::string &expansion : mounts) {
		Vfs vfs;
		TEST_EXPECT(vfs.mount_game(install, expansion, VfsMountMode::Packed));
		std::vector<uint8_t> bytes;
		TEST_EXPECT(vfs.read_file("Avatars.def", bytes) && !bytes.empty());
		AvatarsDocument document;
		Diagnostic error;
		TEST_EXPECT(document.load_bytes(bytes, "Avatars.def", AssetKind::AvatarDefs, "jo", error));
		TEST_EXPECT(!document.blocked());
		const SerializeResult written = document.serialize();
		if (!written.ok() || written.text != std::string(bytes.begin(), bytes.end()) || !written.notes.empty()) {
			std::fprintf(stderr, "FAIL the install's Avatars.def %s is not saved as it was\n", expansion.c_str());
			return 1;
		}
		std::printf("retail: Avatars.def %s, %zu rows, saved byte for byte\n", expansion.empty() ? "(base)" : expansion.c_str(),
		            document.rows().size());
	}
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (test_rows() || test_graph() || test_findings_and_session() || test_order() || test_retail()) return 1;
	return 0;
}
