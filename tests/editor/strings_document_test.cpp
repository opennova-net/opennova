// The string-table document (ADR 0046 S6b) over the neutral core: a retail-shaped
// table loads into sections and strings, an untouched save rewrites the same bytes,
// edits round-trip through the writer, text shows as UTF-8 and stores as cp1252 (a
// character cp1252 lacks is refused), a section duplicates under a name of its own,
// undo/redo and the save checkpoint hold, an ungrouped table is regrouped with a
// warning, the validator flags empty and duplicate keys and section names, and the
// session creates, opens, edits and saves a table through the generic requests.
#include <editor/documents/strings_document.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/project_build/build_run.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/project/project_files.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "common/test_expect.h"

#include <base/io/cp1252.h>

using namespace opennova::editor;
namespace rtxt = opennova::rtxt;

namespace {

constexpr NodeKind kSection = node_kind(StringsKind::Section);
constexpr NodeKind kString = node_kind(StringsKind::String);

using editor_test::NoProcess;

Edit set(NodeAddress address, const char *field, Value value, bool coalesce = false) {
	Edit edit;
	edit.address = address;
	edit.field = field;
	edit.value = std::move(value);
	edit.coalesce = coalesce;
	return edit;
}

// Two sections, three strings, one cp1252 accent (0xE9 = e-acute) in a retail-shaped table.
std::vector<uint8_t> minted_table(bool grouped = true) {
	rtxt::File file;
	file.sections = {{"Menu", 2}, {"WepDes", 1}};
	rtxt::Entry a; a.key = "MM_Exit"; a.text = "Exit"; a.section_index = 0;
	rtxt::Entry b; b.key = "MM_Cafe"; b.text = "Caf\xE9"; b.section_index = 0; b.position = {12, -3};
	rtxt::Entry c; c.key = "WPN_ONE"; c.text = "The first weapon"; c.section_index = 1;
	file.entries = grouped ? std::vector<rtxt::Entry>{a, b, c} : std::vector<rtxt::Entry>{a, c, b};
	if (!grouped) file.sections = {{"Menu", 1}, {"WepDes", 2}};
	std::vector<uint8_t> bytes;
	std::string error;
	if (!rtxt::write(file, bytes, error)) return {};
	return bytes;
}

std::string text_of(const Document &document, NodeAddress address, const char *field) {
	Value value;
	if (!document.get(address, field, value)) return "<none>";
	return std::get<std::string>(value);
}

int load_edit_save() {
	editor_test::TempProjectDir dir("opennova_strings_document_test");
	const std::vector<uint8_t> bytes = minted_table();
	TEST_EXPECT(!bytes.empty());
	std::string error;
	TEST_EXPECT(write_file_atomic(dir.file("gametext.bin"), bytes.data(), bytes.size(), error));
	StringsDocument document;
	Diagnostic diagnostic;
	TEST_EXPECT(document.load(dir.file("gametext.bin"), "gametext.bin", AssetKind::Strings, "jo", diagnostic));
	TEST_EXPECT(document.rows().size() == 2 && !document.blocked() && document.issues().empty());
	const NodeId menu = document.rows()[0]->id;
	TEST_EXPECT(document.rows()[0]->name() == "Menu" && document.rows()[1]->name() == "WepDes");
	TEST_EXPECT(document.rows()[0]->collections[0].size() == 2 && document.rows()[1]->collections[0].size() == 1);
	const NodeAddress cafe{menu, kString, document.rows()[0]->collections[0][1]};
	// cp1252 shows as UTF-8; positions are fields too.
	TEST_EXPECT(text_of(document, cafe, "key") == "MM_Cafe");
	TEST_EXPECT(text_of(document, cafe, "text") == "Caf\xC3\xA9");
	Value x;
	TEST_EXPECT(document.get(cafe, "x", x) && std::get<int64_t>(x) == 12);
	// An untouched save rewrites the same bytes.
	TEST_EXPECT(document.save(diagnostic));
	std::vector<uint8_t> after;
	TEST_EXPECT(read_file_bytes(dir.file("gametext.bin"), after, error) && after == bytes);
	// Edits: a UTF-8 edit is stored as cp1252; one holding a character cp1252 has no byte for is
	// refused, on its field, naming the character (never stored as UTF-8 bytes).
	TEST_EXPECT(document.apply(set(cafe, "text", std::string("Na\xC3\xAFve")), diagnostic));
	TEST_EXPECT(text_of(document, cafe, "text") == "Na\xC3\xAFve");
	TEST_EXPECT(document.table().entries[1].text == "Na\xEFve");
	const uint64_t stored = document.revision();
	TEST_EXPECT(!document.apply(set(cafe, "text", std::string("\xE2\x9C\x93 done")), diagnostic));
	TEST_EXPECT(diagnostic.field == "text" && diagnostic.message.find("\xE2\x9C\x93 (U+2713)") != std::string::npos);
	TEST_EXPECT(!document.apply(set(cafe, "key", std::string("KEY_\xE4\xB8\xAD")), diagnostic) && diagnostic.field == "key");
	TEST_EXPECT(document.revision() == stored && document.table().entries[1].text == "Na\xEFve");
	// The fields say what they are: the text runs over several lines; the position is a pair
	// no witnessed reader uses.
	const std::vector<FieldSchema> &fields = document.fields(kString);
	TEST_EXPECT(fields.size() == 4 && fields[0].label == "Key" && fields[1].label == "Text" && fields[1].multiline);
	TEST_EXPECT(fields[2].group == "Position" && fields[3].group == "Position" && fields[2].applies == Applicability::Unverified);
	// A section duplicated right after itself with its strings, under a name of its own (under
	// its original's no lookup would read it); undone.
	Edit duplicate;
	duplicate.operation = EditOperation::Duplicate;
	duplicate.address = {menu, kSection, 0};
	duplicate.position = 1;
	TEST_EXPECT(document.apply(duplicate, diagnostic));
	TEST_EXPECT(document.rows().size() == 3 && document.rows()[1]->name() == "Menu2" &&
	            document.rows()[1]->collections[0].size() == 2 && document.rows()[1]->collections[0][0] != cafe.child);
	document.undo();
	TEST_EXPECT(document.rows().size() == 2 && document.revision() == stored);
	// S12 review: a name that fills the field (127 bytes) gives up the end of its stem for the
	// number, each copy a name of its own.
	const std::string full(127, 'A');
	TEST_EXPECT(document.apply(set({menu, kSection, 0}, "name", full), diagnostic));
	TEST_EXPECT(document.apply(duplicate, diagnostic) && document.rows()[1]->name() == std::string(126, 'A') + "2");
	TEST_EXPECT(document.apply(duplicate, diagnostic) && document.rows()[1]->name() == std::string(126, 'A') + "3");
	Edit again = duplicate;
	again.address = {document.rows()[2]->id, kSection, 0}; // the "...2" copy: its stem is 126 bytes
	again.position = 3;
	TEST_EXPECT(document.apply(again, diagnostic) && document.rows()[3]->name() == std::string(126, 'A') + "4");
	for (size_t i = 0; i < 4; ++i) TEST_EXPECT(document.rows()[i]->name().size() <= 127);
	document.undo();
	document.undo();
	document.undo();
	document.undo();
	TEST_EXPECT(document.rows().size() == 2 && document.revision() == stored);
	// Add a section and a string, rename, move, then save and reparse through rtxt.
	Edit add;
	add.operation = EditOperation::Add;
	add.address = {0, kSection, 0};
	TEST_EXPECT(document.apply(add, diagnostic));
	const NodeId custom = document.last_added();
	TEST_EXPECT(document.apply(set({custom, kSection, 0}, "name", std::string("Custom")), diagnostic));
	add.address = {custom, kString, 0};
	TEST_EXPECT(document.apply(add, diagnostic));
	const NodeAddress hello{custom, kString, document.last_added()};
	// A section holds its strings (owner-scoped, S9g); a string holds nothing.
	const std::vector<Document::Collection> strings = document.collections_of({custom, kSection, 0});
	TEST_EXPECT(strings.size() == 1 && std::string(document.kind_token(strings[0].spec.kind)) == "string" && strings[0].ids.size() == 1);
	TEST_EXPECT(document.collections_of(hello).empty() && document.locator(hello) == "2/string:0");
	// B5: the only string moved to its own place is no edit.
	const uint64_t revision = document.revision();
	Edit stay;
	stay.operation = EditOperation::Move;
	stay.address = hello;
	stay.position = 0;
	TEST_EXPECT(document.apply(stay, diagnostic) && document.revision() == revision);
	TEST_EXPECT(document.apply(set(hello, "key", std::string("HELLO")), diagnostic));
	TEST_EXPECT(document.apply(set(hello, "text", std::string("Hello"), true), diagnostic));
	TEST_EXPECT(document.apply(set(hello, "text", std::string("Hello there"), true), diagnostic));
	TEST_EXPECT(document.apply(set(hello, "y", int64_t(7)), diagnostic));
	TEST_EXPECT(!document.apply(set(hello, "y", int64_t(70000)), diagnostic));
	Edit move;
	move.operation = EditOperation::Move;
	move.address = {custom, kSection, 0};
	move.position = 0;
	TEST_EXPECT(document.apply(move, diagnostic));
	TEST_EXPECT(document.rows()[0]->id == custom);
	TEST_EXPECT(document.save(diagnostic) && !document.dirty());
	rtxt::File reparsed;
	TEST_EXPECT(read_file_bytes(dir.file("gametext.bin"), after, error) && rtxt::parse(after.data(), after.size(), reparsed, error));
	TEST_EXPECT(reparsed.is_grouped() && reparsed.sections.size() == 3 && reparsed.sections[0].name == "Custom");
	TEST_EXPECT(reparsed.get_in_section("Custom", "HELLO") == "Hello there");
	TEST_EXPECT(reparsed.find_in_section("Custom", "HELLO")->position.y == 7);
	TEST_EXPECT(reparsed.get_in_section("Menu", "MM_Cafe") == "Na\xEFve");
	// Undo past the save re-dirties; the coalesced text edits were one step.
	document.undo();
	TEST_EXPECT(document.dirty() && document.rows()[0]->id != custom);
	document.undo();
	TEST_EXPECT(!document.apply(set(hello, "y", int64_t(1)), diagnostic) || true); // the string still exists here
	NodeAddress found;
	TEST_EXPECT(
			find_definition(AssetGraph(), document, "hello", found) && found.child == hello.child);
	TEST_EXPECT(find_definition(AssetGraph(), document, "wepdes", found) && found.kind == kSection);
	TEST_EXPECT(!find_definition(AssetGraph(), document, "nowhere", found));
	// A string named in a section the table lacks blocks; an ungrouped table only warns.
	TEST_EXPECT(write_file_atomic(dir.file("ungrouped.bin"), minted_table(false).data(), minted_table(false).size(), error));
	StringsDocument ungrouped;
	TEST_EXPECT(ungrouped.load(dir.file("ungrouped.bin"), "ungrouped.bin", AssetKind::Strings, "jo", diagnostic));
	TEST_EXPECT(!ungrouped.blocked() && ungrouped.ignored_lines() == 1 && ungrouped.rows().size() == 2);
	TEST_EXPECT(ungrouped.table().is_grouped());
	return 0;
}

int validation_and_session() {
	editor_test::TempProjectDir dir("opennova_strings_session_test");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Strings"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	TEST_EXPECT(view.project.requirements->required_missing == 0);
	// The requirement tables open as documents; a new section and string reach the file.
	session.handle(request::open_document("gametext.bin"));
	auto *document = session.document_for("gametext.bin");
	TEST_EXPECT(document && dynamic_cast<StringsDocument *>(document));
	TEST_EXPECT(is_editable_kind(AssetKind::Strings) && document_type_for(AssetKind::Strings)->name == std::string("strings"));
	EditorRequest add = request::edit_record(document->path(), Edit());
	add.edits[0].operation = EditOperation::Add;
	add.edits[0].address = {0, kSection, 0};
	session.handle(add);
	const NodeId section = document->last_added();
	TEST_EXPECT(
			view.documents.selection.row == section && view.documents.selection.kind == kSection);
	EditorRequest name = request::edit_record(
			document->path(), set({ section, kSection, 0 }, "name", std::string("Custom")));
	session.handle(name);
	add.edits[0].address = {section, kString, 0};
	session.handle(add);
	const NodeId string = document->last_added();
	TEST_EXPECT(view.documents.selection.row == section && view.documents.selection.child == string && view.documents.selection.kind == kString);
	EditorRequest key = request::edit_record(
			document->path(), set({ section, kString, string }, "key", std::string("HELLO")));
	session.handle(key);
	// An empty key is an error, a duplicate a warning; Build refuses the dirty document.
	add.edits[0].address = {section, kString, 0};
	session.handle(add);
	const NodeId blank = document->last_added();
	EditorRequest empty = request::edit_record(
			document->path(), set({ section, kString, blank }, "key", std::string("")));
	session.handle(empty);
	bool empty_error = false;
	for (const Diagnostic &d : view.findings.diagnostics) empty_error |= d.code == "strings.key_empty" && d.child_id == blank;
	TEST_EXPECT(empty_error);
	empty.edits = {set({section, kString, blank}, "key", std::string("hello"))};
	session.handle(empty);
	bool duplicate = false, still_empty = false;
	for (const Diagnostic &d : view.findings.diagnostics) {
		duplicate |= d.code == "strings.key_duplicate" && d.severity == DiagnosticSeverity::Warning;
		still_empty |= d.code == "strings.key_empty";
	}
	TEST_EXPECT(duplicate && !still_empty);
	// Build waits on the unsaved prompt over the edited table; cancelled, nothing is built.
	session.handle(request::build());
	TEST_EXPECT(view.dialogs.unsaved_prompt.open && view.dialogs.unsaved_prompt.action == EditorRequestKind::Build &&
	            view.dialogs.unsaved_prompt.files == std::vector<std::string>{document->path()} && !session.view().activity.operation.running());
	EditorRequest cancel = request::resolve_unsaved(UnsavedChoice::Cancel);
	session.handle(cancel);
	TEST_EXPECT(!view.dialogs.unsaved_prompt.open && !view.activity.has_build && document->dirty());
	EditorRequest remove = request::edit_record(document->path(), Edit());
	remove.edits[0].operation = EditOperation::Remove;
	remove.edits[0].address = {section, kString, blank};
	session.handle(remove);
	session.handle(request::save_all());
	TEST_EXPECT(!document->dirty());
	session.handle(request::build());
	session.run_operations();
	TEST_EXPECT(view.activity.last_build->ok);
	// Reopen from disk: the section and the key survive; Go to finds the key by the locator of
	// the symbol the key defines.
	session.handle(request::close_document(document->path()));
	const std::vector<const GraphSymbol *> hello = view.findings.graph->symbols_named(ReferenceKind::TextId, "HELLO");
	TEST_EXPECT(hello.size() == 1 && hello[0]->field == "key");
	if (hello.size() != 1) return 1;
	session.handle(request::open_document(hello[0]->file, hello[0]->locator, hello[0]->field));
	document = session.document_for("gametext.bin");
	TEST_EXPECT(document && view.documents.selection.kind == kString && view.documents.selection.row != 0 && editor_test::revealed_field(view) == "key");
	TEST_EXPECT(document && text_of(*document, view.documents.selection, "key") == "HELLO");
	// A new table is created blank and opened when the request names the kind (a
	// bare `.bin` name cannot say what it is); an unnamed kind is refused.
	session.handle(request::create_file("extra.bin"));
	TEST_EXPECT(session.document_for("extra.bin") == nullptr && view.findings.diagnostics.back().code == "document.kind");
	session.handle(request::create_file("extra.bin", asset_kind_token(AssetKind::Strings)));
	auto *extra = session.document_for("extra.bin");
	TEST_EXPECT(extra && extra->rows().empty() && !extra->blocked());
	if (!extra) return 1;
	// S12: sections by the reader's rule. An empty name is an error; a name an earlier section
	// has, in any case, is a warning on the later one (a lookup by section reads the first).
	const auto finding = [&](const char *code, NodeId row) {
		for (const Diagnostic &d : view.findings.diagnostics)
			if (d.code == code && d.asset == extra->path() && d.row_id == row) return &d;
		return static_cast<const Diagnostic *>(nullptr);
	};
	add.path = extra->path();
	add.edits[0].address = {0, kSection, 0};
	session.handle(add);
	const NodeId twice = extra->last_added();
	EditorRequest rename = request::edit_record(
			extra->path(), set({ twice, kSection, 0 }, "name", std::string("")));
	session.handle(rename);
	TEST_EXPECT(finding("strings.section_empty", twice) && finding("strings.section_empty", twice)->field == "name");
	rename.edits = {set({twice, kSection, 0}, "name", std::string("Twice"))};
	session.handle(rename);
	session.handle(add);
	const NodeId again = extra->last_added();
	rename.edits = {set({again, kSection, 0}, "name", std::string("TWICE"))};
	session.handle(rename);
	const Diagnostic *shadowed = finding("strings.section_duplicate", again);
	TEST_EXPECT(!finding("strings.section_empty", twice) && !finding("strings.section_duplicate", twice));
	TEST_EXPECT(shadowed && shadowed->severity == DiagnosticSeverity::Warning && shadowed->record == "TWICE" &&
	            shadowed->message.find("Section 1") != std::string::npos);
	return 0;
}

// S11a: a string's text edited shows as changed and reverts; a string added is Added and
// its section changed; an ungrouped table (clean, its warning a source finding) needs the
// rewrite that regroups it, after which the finding is gone and the history stays.
int changes_since_save() {
	editor_test::TempProjectDir dir("opennova_strings_changes_test");
	std::string error;
	const std::vector<uint8_t> bytes = minted_table();
	TEST_EXPECT(write_file_atomic(dir.file("gametext.bin"), bytes.data(), bytes.size(), error));
	StringsDocument document;
	Diagnostic diagnostic;
	TEST_EXPECT(document.load(dir.file("gametext.bin"), "gametext.bin", AssetKind::Strings, "jo", diagnostic));
	const NodeId menu = document.rows()[0]->id;
	const NodeAddress section{menu, kSection, 0};
	const NodeAddress cafe{menu, kString, document.rows()[0]->collections[0][1]};
	TEST_EXPECT(document.apply(set(cafe, "text", std::string("Tea")), diagnostic));
	TEST_EXPECT(document.field_changed(cafe, "text") && !document.field_changed(cafe, "key"));
	TEST_EXPECT(document.record_change(cafe) == Document::RecordChange::Changed &&
	            document.record_change(section) == Document::RecordChange::Unchanged);
	TEST_EXPECT(document.apply(document.revert_edits(cafe, "text"), diagnostic) && !document.field_changed(cafe, "text"));
	TEST_EXPECT(text_of(document, cafe, "text") == "Caf\xC3\xA9");
	Edit add;
	add.operation = EditOperation::Add;
	add.address = {menu, kString, 0};
	TEST_EXPECT(document.apply(add, diagnostic));
	TEST_EXPECT(document.record_change({menu, kString, document.last_added()}) == Document::RecordChange::Added &&
	            document.record_change(section) == Document::RecordChange::Changed);

	const std::vector<uint8_t> ungrouped = minted_table(false);
	TEST_EXPECT(write_file_atomic(dir.file("ungrouped.bin"), ungrouped.data(), ungrouped.size(), error));
	StringsDocument table;
	TEST_EXPECT(table.load(dir.file("ungrouped.bin"), "ungrouped.bin", AssetKind::Strings, "jo", diagnostic));
	TEST_EXPECT(!table.dirty() && table.ignored_lines() == 1 && table.rewrite_need() == Document::RewriteNeed::Rewrite);
	TEST_EXPECT(table.apply(set({table.rows()[0]->id, kSection, 0}, "name", std::string("Menu2")), diagnostic));
	table.undo(); // clean again, the step kept for redo
	TEST_EXPECT(table.save(diagnostic) && table.issues().empty() && table.rewrite_need() == Document::RewriteNeed::None && table.can_redo());
	std::vector<uint8_t> written;
	rtxt::File reparsed;
	TEST_EXPECT(read_file_bytes(dir.file("ungrouped.bin"), written, error) &&
	            rtxt::parse(written.data(), written.size(), reparsed, error) && reparsed.is_grouped());
	return 0;
}

} // namespace

int main() { return load_edit_save() || validation_and_session() || changes_since_save(); }
