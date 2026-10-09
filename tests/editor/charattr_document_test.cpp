// The character attributes document (ADR 0046 DI-09's charattr follow-up; S23 B its rows): charattr.def's classes as
// rows read as the game's loader reads them (formats/charattr) with the file's layout modeled, written back over it:
// the file as it was but for a changed value's line. Each class the game reads names three items by their type ids
// (the items.def id less 100000): references of its camouflage fields, which reach the items (Used by); a type id no
// item has is a warning in the game's words (a player of the class spawns as items.def's first row); a rename of the
// item's id writes the type id back at its line. A section the game never reads is no row and names nothing, and is a
// finding (after the first missing class, a second of a label), as are a word no attribute is, a value read as text,
// and a class read with no camouflage item. A class added is the next; one removed the last (the loader stops at the
// first class it lacks). The retail leg (OPENNOVA_JO_DIR): the install's charattr.def, base and each expansion,
// through the document byte for byte.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <base/vfs/vfs.h>
#include <editor/documents/charattr_document.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

constexpr NodeKind kClass = node_kind(CharAttrKind::Class);

std::vector<uint8_t> bytes_of(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

std::string crlf(const std::string &text) {
	std::string lines;
	for (const char c : text) lines += c == '\n' ? std::string("\r\n") : std::string(1, c);
	return lines;
}

Edit set_edit(const NodeAddress &at, const char *field, Value value) {
	Edit edit;
	edit.address = at;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

struct Project {
	editor_test::TempProjectDir dir{"opennova_editor_charattr_document"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string root;

	Project() {
		editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Character attributes"));
		editor_test::create_missing_files(session);
		root = session.view().project.root;
	}
	const AssetGraph &graph() const { return *session.view().findings.graph; }
	std::string path(const char *name) const {
		const AssetEntry *entry = session.view().project.scan->find(name);
		return entry ? entry->relative_path : std::string();
	}
	// Each line ending CR LF, as the game's readers end a line.
	bool write(const std::string &relative, const std::string &text) { return editor_test::write_text(root + "/" + relative, crlf(text)); }
	std::string read(const std::string &relative) const {
		std::vector<uint8_t> bytes;
		std::string error;
		io::read_file_bytes(root + "/" + relative, bytes, error);
		return std::string(bytes.begin(), bytes.end());
	}
	void rescan() { editor_test::handle_to_end(session, request::rescan()); }
};

const char *kItems = "begin \"Null\"\nid 100000\ntype marker\nend\n"
                     "begin \"Soldier SP\"\nid 107001\ntype person\nend\n"
                     "begin \"Soldier MP\"\nid 107002\ntype person\nend\n";

// CHARACTER1 names 7001 twice and 4999 (no item); CHARACTER2 has no ARCTIC_CAMMO, a word no attribute is and a
// value that is no number; CHARACTER4 is after the missing 3, and the second CHARACTER1 a repeat: neither is
// read, so neither's 7002 is a reference.
const char *kCharAttr = "// classes\n"
                        "[CHARACTER1]\nSTEALTH = 25\nJUNGLE_CAMMO = 7001\nDESERT_CAMMO = 7001\nARCTIC_CAMMO = 4999\n"
                        "ATTRIBUTES = AutoScope\n"
                        "[CHARACTER2]\nSTEALTH = quiet\nJUNGLE_CAMMO = 7002\nDESERT_CAMMO = 7002\nATTRIBUTES = Medick NULL\n"
                        "[CHARACTER4]\nJUNGLE_CAMMO = 7002\nDESERT_CAMMO = 7002\nARCTIC_CAMMO = 7002\n"
                        "[CHARACTER1]\nJUNGLE_CAMMO = 7002\n";

std::vector<const GraphEdge *> edges_of(const AssetGraph &graph, const std::string &file) {
	std::vector<const GraphEdge *> out;
	for (const GraphEdge *edge : graph.references_of(file)) out.push_back(edge);
	return out;
}

size_t count_code(const SessionView &view, const std::string &file, const char *code) {
	size_t n = 0;
	for (const Diagnostic &d : view.findings.diagnostics)
		if (d.asset == file && d.code() == code) ++n;
	return n;
}

const Node *class_row(const CharAttrDocument &document, int id) {
	for (const auto &row : document.rows())
		if (row && static_cast<const CharAttrRow &>(*row).row.class_id == id) return row.get();
	return nullptr;
}

} // namespace

// The rows: the classes the game reads, each its keys; the save the file as it was but for a changed value's line; a
// class added the next, one removed the last alone.
static int test_rows() {
	const std::string text = crlf(kCharAttr);
	CharAttrDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(bytes_of(text), "charattr.def", AssetKind::CharAttrDefs, "jo", error));
	TEST_EXPECT(document.rows().size() == 2 && document.serialize().text == text);
	const Node *one = class_row(document, 1), *two = class_row(document, 2);
	TEST_EXPECT(one && two);
	if (!one || !two) return 1;
	const NodeAddress first{one->id, kClass, 0}, second{two->id, kClass, 0};
	Value value;
	TEST_EXPECT(document.get(first, "stealth", value) && value == Value(25.0));
	TEST_EXPECT(document.get(first, "jungle_cammo", value) && value == Value(int64_t(7001)));
	TEST_EXPECT(document.get(first, "attributes", value) && value == Value(int64_t(charattr::kAutoScope)));
	TEST_EXPECT(document.record_title(first) == "CHARACTER1 (AutoScope)");
	// A value changed: its own line.
	TEST_EXPECT(document.apply(set_edit(first, "stealth", 30.0), error));
	std::string expected = text;
	expected.replace(expected.find("STEALTH = 25"), 12, "STEALTH = 30.0");
	TEST_EXPECT(document.serialize().text == expected);
	// The attributes are the table's words' flags.
	TEST_EXPECT(!document.apply(set_edit(first, "attributes", int64_t(0x40)), error));
	TEST_EXPECT(document.apply(set_edit(second, "attributes", int64_t(charattr::kMedic)), error));
	TEST_EXPECT(document.serialize().text.find("[CHARACTER2]\r\nSTEALTH = quiet\r\nJUNGLE_CAMMO = 7002\r\nDESERT_CAMMO = 7002\r\n"
	                                           "ATTRIBUTES = Medic") != std::string::npos);
	// A class added: CHARACTER3 after the file's two, its section in the writer's form; a key set on it, its line.
	const std::string two_classes = crlf("// classes\n[CHARACTER1]\nSTEALTH = 25\nJUNGLE_CAMMO = 7001\n\n[CHARACTER2]\nJUNGLE_CAMMO = 7002\n");
	CharAttrDocument grown;
	TEST_EXPECT(grown.load_bytes(bytes_of(two_classes), "charattr.def", AssetKind::CharAttrDefs, "jo", error) &&
	            grown.rows().size() == 2);
	Edit add;
	add.operation = EditOperation::Add;
	add.address = {0, kClass, 0};
	TEST_EXPECT(grown.apply(add, error));
	const Node *three = class_row(grown, 3);
	TEST_EXPECT(three != nullptr);
	if (!three) return 1;
	TEST_EXPECT(grown.apply(set_edit({three->id, kClass, 0}, "desert_cammo", int64_t(7003)), error));
	const std::string saved = grown.serialize().text;
	TEST_EXPECT(saved.rfind(two_classes, 0) == 0 && saved.find("[CHARACTER3]\r\nDESERT_CAMMO = 7003\r\n") != std::string::npos);
	CharAttrDocument again;
	TEST_EXPECT(again.load_bytes(bytes_of(saved), "charattr.def", AssetKind::CharAttrDefs, "jo", error) && again.rows().size() == 3);
	// The middle class is not removed (the loader would stop there); the last is.
	Edit remove;
	remove.operation = EditOperation::Remove;
	remove.address = {class_row(grown, 2)->id, kClass, 0};
	TEST_EXPECT(!grown.apply(remove, error));
	remove.address = {three->id, kClass, 0};
	TEST_EXPECT(grown.apply(remove, error) && grown.rows().size() == 2 && grown.serialize().text == two_classes);
	std::printf("rows: the classes read, a value's own line, the attributes' words, a class added and the last removed\n");
	return 0;
}

// The type owns charattr.def; its references are the read classes' camouflage items.
static int test_references() {
	Project project;
	const std::string items = project.path("items.def"), charattr = project.path("charattr.def");
	TEST_EXPECT(!items.empty() && !charattr.empty());
	TEST_EXPECT(document_type_for(AssetKind::CharAttrDefs) == document_type(DocumentTypeId::CharAttrs));
	TEST_EXPECT(project.write(items, kItems) && project.write(charattr, kCharAttr));
	project.rescan();
	const AssetGraph &graph = project.graph();
	const std::vector<const GraphEdge *> edges = edges_of(graph, charattr);
	// CHARACTER1's three and CHARACTER2's two; CHARACTER4 and the second CHARACTER1 are never read.
	TEST_EXPECT(edges.size() == 5);
	const GraphEdge *jungle = nullptr, *arctic = nullptr;
	size_t mp = 0;
	for (const GraphEdge *edge : edges) {
		TEST_EXPECT(edge->kind == ReferenceKind::Item && edge->name_offset == 100000);
		if (edge->record == "CHARACTER1" && edge->field == "jungle_cammo") jungle = edge;
		if (edge->record == "CHARACTER1" && edge->field == "arctic_cammo") arctic = edge;
		if (edge->value == "107002") ++mp;
	}
	TEST_EXPECT(mp == 2);
	TEST_EXPECT(jungle && jungle->value == "107001" && graph.resolve(*jungle) == ReferenceStatus::Present);
	// Used by: the item's users hold the class's two.
	const std::vector<const GraphSymbol *> sp = graph.symbols_named(ReferenceKind::Item, "107001");
	TEST_EXPECT(sp.size() == 1);
	if (!sp.empty()) {
		size_t users = 0;
		for (const GraphEdge *user : graph.users_of(*sp.front())) users += user->source == charattr ? 1 : 0;
		TEST_EXPECT(users == 2);
	}
	// A type id no item has: a warning in the game's words.
	TEST_EXPECT(arctic && arctic->value == "104999" && graph.resolve(*arctic) == ReferenceStatus::Missing);
	bool worded = false;
	for (const Diagnostic &d : project.session.view().findings.diagnostics)
		if (d.code() == "reference.missing" && d.asset == charattr && d.message.find("type id 4999") != std::string::npos) {
			worded = d.severity == DiagnosticSeverity::Warning &&
			         d.message.find("spawns as items.def's first row") != std::string::npos;
		}
	TEST_EXPECT(worded);
	// The type's findings: the two unread sections, the word, the text value, CHARACTER2's missing arctic item.
	const SessionView &view = project.session.view();
	TEST_EXPECT(count_code(view, charattr, "charattr.unread_section") == 2);
	TEST_EXPECT(count_code(view, charattr, "charattr.attribute_word") == 1); // NULL is none, Medick no word
	TEST_EXPECT(count_code(view, charattr, "charattr.not_a_number") == 1);
	TEST_EXPECT(count_code(view, charattr, "charattr.no_cammo") == 1);
	for (const Diagnostic &d : view.findings.diagnostics)
		if (d.asset == charattr && d.code() == "charattr.not_a_number")
			TEST_EXPECT(d.record == "CHARACTER2" && d.field == "stealth" && d.line == 9);
	std::printf("references: the read classes' camouflage items, a missing one in the game's words; the findings\n");
	return 0;
}

// A rename of the item's id writes the type id back at the class's lines; the unread sections' numbers stay.
static int test_rename() {
	Project project;
	const std::string items = project.path("items.def"), charattr = project.path("charattr.def");
	TEST_EXPECT(project.write(items, kItems) && project.write(charattr, kCharAttr));
	project.rescan();
	const std::vector<const GraphSymbol *> mp = project.graph().symbols_named(ReferenceKind::Item, "107002");
	TEST_EXPECT(mp.size() == 1);
	if (mp.empty()) return 1;
	const GraphSymbol symbol = *mp.front();
	const ActionOutcome renamed_outcome = editor_test::handle_to_end(
			project.session, request::rename_symbol(symbol.file, symbol.locator, symbol.field, "107003"));
	for (const Diagnostic &d : renamed_outcome.findings) std::printf("  rename: %s\n", d.message.c_str());
	TEST_EXPECT(renamed_outcome.done());
	project.rescan();
	const std::string text = project.read(charattr);
	// CHARACTER2's two now 7003; CHARACTER4's three and the repeat's one, never read, stay 7002.
	size_t renamed = 0, kept = 0;
	for (size_t at = text.find("7003"); at != std::string::npos; at = text.find("7003", at + 1)) ++renamed;
	for (size_t at = text.find("7002"); at != std::string::npos; at = text.find("7002", at + 1)) ++kept;
	TEST_EXPECT(renamed == 2 && kept == 4);
	TEST_EXPECT(text.find("[CHARACTER2]\r\nSTEALTH = quiet\r\nJUNGLE_CAMMO = 7003\r\nDESERT_CAMMO = 7003\r\n") !=
	            std::string::npos);
	size_t reaching = 0;
	for (const GraphEdge *edge : edges_of(project.graph(), charattr)) reaching += edge->value == "107003" ? 1 : 0;
	TEST_EXPECT(reaching == 2);
	std::printf("rename: the item's id written back at the read classes' lines alone\n");
	return 0;
}

static int test_retail() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (the install's charattr.def through the document)");
		return 0;
	}
	std::vector<std::string> mounts{std::string()};
	for (const std::string &expansion : retail::expansions()) mounts.push_back(expansion);
	for (const std::string &expansion : mounts) {
		Vfs vfs;
		TEST_EXPECT(vfs.mount_game(install, expansion, VfsMountMode::Packed));
		std::vector<uint8_t> bytes;
		TEST_EXPECT(vfs.read_file("charattr.def", bytes) && !bytes.empty());
		CharAttrDocument document;
		Diagnostic error;
		TEST_EXPECT(document.load_bytes(bytes, "charattr.def", AssetKind::CharAttrDefs, "jo", error) && !document.blocked());
		const SerializeResult written = document.serialize();
		if (!written.ok() || written.text != std::string(bytes.begin(), bytes.end()) || !written.notes.empty()) {
			std::fprintf(stderr, "FAIL the install's charattr.def %s is not saved as it was\n", expansion.c_str());
			return 1;
		}
		size_t overruns = 0;
		for (const Diagnostic &d : validate_charattr_file(document)) overruns += d.code() == "document.config_overrun";
		TEST_EXPECT(overruns == 0);
		std::printf("retail: charattr.def %s, %zu classes, saved byte for byte\n", expansion.empty() ? "(base)" : expansion.c_str(),
		            document.rows().size());
	}
	return 0;
}

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	failures += test_rows();
	failures += test_references();
	failures += test_rename();
	failures += test_retail();
	if (failures == 0) std::printf("editor_charattr: all passed\n");
	return failures;
}
