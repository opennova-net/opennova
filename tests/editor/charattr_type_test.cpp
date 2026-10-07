// The character attributes type (ADR 0046 DI-09's charattr follow-up): charattr.def opens as its text, read as
// the game's loader reads it (formats/charattr). Each class the game reads names three items by their type ids
// (the items.def id less 100000): references at the numbers' spans, the record and key they are written under,
// which reach the items (Go to, Used by); a type id no item has is a warning in the game's words (a player of
// the class spawns as items.def's first row); a rename of the item's id writes the type id back into the text.
// A section the game never reads names nothing and is a finding (after the first missing class, a second of a
// label), as are a word no attribute is, a value read as text, and a class read with no camouflage item.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <editor/documents/charattr_type.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

struct Project {
	editor_test::TempProjectDir dir{"opennova_editor_charattr_type"};
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
	bool write(const std::string &relative, const std::string &text) {
		std::string lines;
		for (const char c : text) lines += c == '\n' ? std::string("\r\n") : std::string(1, c);
		return editor_test::write_text(root + "/" + relative, lines);
	}
	std::string read(const std::string &relative) const {
		std::vector<uint8_t> bytes;
		std::string error;
		read_file_bytes(root + "/" + relative, bytes, error);
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

} // namespace

// The type owns charattr.def; its references are the read classes' camouflage items, each at its number.
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
		TEST_EXPECT(edge->kind == ReferenceKind::Item && edge->name_offset == 100000 && edge->span.line > 0);
		if (edge->record == "CHARACTER1" && edge->field == "JUNGLE_CAMMO") jungle = edge;
		if (edge->record == "CHARACTER1" && edge->field == "ARCTIC_CAMMO") arctic = edge;
		if (edge->value == "107002") ++mp;
	}
	TEST_EXPECT(mp == 2);
	TEST_EXPECT(jungle && jungle->value == "107001" && jungle->span.line == 4 && jungle->span.column == 16 &&
	            jungle->span.length == 4 && jungle->rewritable);
	TEST_EXPECT(jungle && graph.resolve(*jungle) == ReferenceStatus::Present);
	// Used by: the item's users hold the class's two.
	const std::vector<const GraphSymbol *> sp = graph.symbols_named(ReferenceKind::Item, "107001");
	TEST_EXPECT(sp.size() == 1);
	if (!sp.empty()) {
		size_t users = 0;
		for (const GraphEdge *user : graph.users_of(*sp.front())) users += user->source == charattr ? 1 : 0;
		TEST_EXPECT(users == 2);
	}
	// A type id no item has: a warning in the game's words, the number as written.
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
	return 0;
}

// A rename of the item's id writes the type id back at the class's numbers; the unread sections' numbers stay.
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
	return 0;
}

int main() {
	int failures = 0;
	failures += test_references();
	failures += test_rename();
	if (failures == 0) std::printf("editor_charattr_type: all passed\n");
	return failures;
}
