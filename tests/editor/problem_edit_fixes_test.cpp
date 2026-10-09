// The fixes that edit a document (ADR 0046 DI-11): a finding whose maker planned an edit of its own
// file with the file's records at hand (Diagnostic::planned, an EditRecord row) offers each as an
// edit_record of that file, opened first, one step Undo takes back, never in bulk; and two that plan
// at fix time over the project, an item's id of its own and a stylesheet variable's line removed.
// Each applied over a real session from a closed file: the finding goes, the record holds what the
// fix said, and Undo brings the finding back. Covered: a mission's SSN and zone id of its own (the
// SSN a new entity takes, the first free zone of the 1 to 99), a string key of its own or the string
// removed, a menu screen's and window's NAME of its own, an item's name and id of its own, an end
// pose's trigger moved onto the last frame the game plays (or cleared where that frame fires it), and
// a stylesheet variable no menu names removed, none offered while a menu's text names it inside a
// longer text.
#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <editor/documents/animation_document.h>
#include <editor/documents/def_table.h>
#include <editor/documents/document_types.h>
#include <editor/documents/mission_document.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/rtxt/rtxt.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;

namespace {

using editor_test::NoProcess;

std::string fixture(const char *relative) { return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/" + relative; }

// A fixture file as an author's edits leave it: loaded through its kind's document type, `edit` applied
// (false refuses), and written again by the type's writer; none when it does not load or is refused.
std::vector<uint8_t> edited(const std::string &path, const char *name, AssetKind kind,
                            const std::function<bool(Document &, Diagnostic &)> &edit) {
	const DocumentType *type = document_type_for(kind);
	std::unique_ptr<Document> document = type ? records_of(type->make()) : nullptr;
	Diagnostic error;
	if (!document || !document->load_bytes(test_io::read_file(path), name, kind, "jo", error) || !edit(*document, error)) {
		std::fprintf(stderr, "%s: %s\n", name, error.message.c_str());
		return {};
	}
	const std::string text = document->serialize().text;
	return std::vector<uint8_t>(text.begin(), text.end());
}

Edit set_of(const NodeAddress &address, const char *field, Value value) {
	Edit edit;
	edit.address = address;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

std::vector<std::string> labels_of(const std::vector<ProblemFix> &fixes) {
	std::vector<std::string> labels;
	for (const ProblemFix &fix : fixes) labels.push_back(fix.label);
	return labels;
}

// Every fix an edit of `path`'s document: an edit_record opened first, never in bulk, its words saying Undo
// takes it back.
bool edits_of(const std::vector<ProblemFix> &fixes, const std::string &path) {
	return !fixes.empty() && std::all_of(fixes.begin(), fixes.end(), [&](const ProblemFix &fix) {
		return fix.request.kind == EditorRequestKind::EditRecord && fix.request.path == path && fix.request.open_first &&
		       !fix.bulk && fix.detail.find("Undo takes it back") != std::string::npos &&
		       fix.detail.find("cannot be undone") == std::string::npos;
	});
}

struct Fixture {
	editor_test::TempProjectDir dir;
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string root;

	explicit Fixture(const char *name) : dir(name) {
		editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Fixes"));
		editor_test::create_missing_files(session);
		editor_test::set_missions(session, true);
		root = session.view().project.root;
	}
	const SessionView &view() const { return session.view(); }
	const Diagnostic *finding(const char *code, const std::string &asset) const {
		for (const Diagnostic &d : view().findings.diagnostics)
			if (d.code() == code && d.asset == asset) return &d;
		return nullptr;
	}
	size_t count(const char *code, const std::string &asset) const {
		size_t n = 0;
		for (const Diagnostic &d : view().findings.diagnostics) n += d.code() == code && d.asset == asset;
		return n;
	}
	// The fix applied: done, its document opened and left unsaved.
	bool apply(const ProblemFix &fix) {
		editor_test::handle_to_end(session, fix.request);
		const Document *document = session.document_for(fix.request.path);
		return session.outcome().done() && document && document->dirty();
	}
	// Undo of the document at `path`: back as the file has it.
	bool undo(const std::string &path) {
		editor_test::handle_to_end(session, request::undo(path));
		const Document *document = session.document_for(path);
		return document && !document->dirty();
	}
};

} // namespace

// A mission's records no lookup finds by their id: an SSN of its own (one past the mission's largest, the
// SSN a new entity takes) and a zone id of its own (the first of 1 to 99 no area has), each an edit of
// the closed mission that Undo takes back.
static int test_mission_ids() {
	Fixture f("opennova_editor_edit_fixes_mission");
	int32_t walker = 0, fresh = 0; // fresh: the SSN a new entity takes
	std::set<int32_t> zones;
	const std::vector<uint8_t> bytes =
	        edited(fixture("bms/synth_logic.bms"), "fixes.bms", AssetKind::Mission, [&](Document &document, Diagnostic &error) {
		        const MissionDocument &mission = dynamic_cast<const MissionDocument &>(document);
		        const std::vector<const Node *> items = mission.rows_of(MissionKind::Item), organics = mission.rows_of(MissionKind::Organic);
		        const std::vector<const Node *> areas = mission.rows_of(MissionKind::Area);
		        if (items.empty() || organics.empty() || areas.size() < 2) return false;
		        walker = static_cast<const EntityRow &>(*organics[0]).native.id;
		        const int32_t first_zone = static_cast<const AreaRow &>(*areas[0]).native.id;
		        return document.apply(set_of({items[0]->id, items[0]->kind, 0}, "id", int64_t(walker)), error) &&
		               document.apply(set_of({areas[1]->id, areas[1]->kind, 0}, "id", int64_t(first_zone)), error);
	        });
	TEST_EXPECT(!bytes.empty() && editor_test::write_bytes(f.root + "/missions/fixes.bms", bytes));
	if (bytes.empty()) return 1;
	editor_test::handle_to_end(f.session, request::rescan());
	const std::string path = "missions/fixes.bms";

	const Diagnostic *ssn = f.finding("mission.ssn_duplicate", path);
	TEST_EXPECT(ssn && ssn->planned.size() == 1);
	if (!ssn) return 1;
	TEST_EXPECT(f.session.document_for(path) == nullptr); // the fix opens it
	// The SSN a new entity takes over the file as it is (next_free_ssn), worked out from the file's rows.
	{
		std::unique_ptr<Document> document = records_of(document_type_for(AssetKind::Mission)->make());
		Diagnostic error;
		TEST_EXPECT(document && document->load_bytes(bytes, "fixes.bms", AssetKind::Mission, "jo", error));
		if (!document) return 1;
		fresh = next_free_ssn(document->rows());
		for (const Node *area : dynamic_cast<const MissionDocument &>(*document).rows_of(MissionKind::Area))
			zones.insert(static_cast<const AreaRow &>(*area).native.id);
	}
	std::vector<ProblemFix> fixes = fixes_for(*ssn, f.view());
	TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({"Give it SSN " + std::to_string(fresh)}));
	TEST_EXPECT(edits_of(fixes, path) && fixes[0].detail.find("area check") != std::string::npos);
	if (fixes.size() != 1) return 1;
	const NodeAddress item{ssn->row_id, ssn->record_kind, 0};
	TEST_EXPECT(f.apply(fixes[0]) && !f.finding("mission.ssn_duplicate", path));
	Value value;
	const Document *mission = f.session.document_for(path);
	TEST_EXPECT(mission && mission->get(item, "id", value) && value == Value(int64_t(fresh)));
	TEST_EXPECT(f.undo(path) && f.finding("mission.ssn_duplicate", path));
	TEST_EXPECT(mission && mission->get(item, "id", value) && value == Value(int64_t(walker)));

	// The zone id: the first of 1 to 99 the mission's areas leave free.
	int32_t free_zone = 1;
	while (zones.count(free_zone)) ++free_zone;
	const Diagnostic *zone = f.finding("mission.zone_duplicate", path);
	TEST_EXPECT(zone != nullptr);
	if (!zone) return 1;
	fixes = fixes_for(*zone, f.view());
	TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({"Give it zone " + std::to_string(free_zone)}) && edits_of(fixes, path));
	if (fixes.size() != 1) return 1;
	TEST_EXPECT(f.apply(fixes[0]) && !f.finding("mission.zone_duplicate", path));
	TEST_EXPECT(f.undo(path) && f.finding("mission.zone_duplicate", path));
	return 0;
}

// A key the game reads the first of: a key of its own (the number a copied section's name takes), or the
// string removed, which no lookup reads.
static int test_string_key() {
	Fixture f("opennova_editor_edit_fixes_strings");
	opennova::rtxt::File table;
	table.sections = {{"Menu", 3}};
	opennova::rtxt::Entry first, again, other;
	first.key = again.key = "MM_Exit";
	first.text = "Exit";
	again.text = "Leave";
	other.key = "MM_Exit2";
	other.text = "Quit";
	table.entries = {first, other, again};
	std::vector<uint8_t> bytes;
	std::string io_error;
	TEST_EXPECT(opennova::rtxt::write(table, bytes, io_error) && editor_test::write_bytes(f.root + "/strings/dup.bin", bytes));
	editor_test::handle_to_end(f.session, request::rescan());
	const std::string path = "strings/dup.bin";
	const Diagnostic *twice = f.finding("strings.key_duplicate", path);
	TEST_EXPECT(twice != nullptr);
	if (!twice) return 1;
	// MM_Exit2 is taken: the next number.
	const std::vector<ProblemFix> fixes = fixes_for(*twice, f.view());
	TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({"Give it the key MM_Exit3", "Remove it"}) && edits_of(fixes, path));
	if (fixes.size() != 2) return 1;
	const NodeAddress string{twice->row_id, twice->record_kind, twice->child_id};
	TEST_EXPECT(f.apply(fixes[0]) && !f.finding("strings.key_duplicate", path));
	const Document *strings = f.session.document_for(path);
	Value key;
	TEST_EXPECT(strings && strings->get(string, "key", key) && key == Value(std::string("MM_Exit3")));
	TEST_EXPECT(f.undo(path) && f.finding("strings.key_duplicate", path));
	const Diagnostic *back = f.finding("strings.key_duplicate", path);
	if (!back) return 1;
	const std::vector<ProblemFix> again_fixes = fixes_for(*back, f.view());
	if (again_fixes.size() != 2) return 1;
	TEST_EXPECT(f.apply(again_fixes[1]) && !f.finding("strings.key_duplicate", path));
	TEST_EXPECT(strings && strings->rows().size() == 1 && strings->collections_of({strings->rows()[0]->id, strings->rows()[0]->kind, 0})
	                                                                      .front()
	                                                                      .ids.size() == 2);
	TEST_EXPECT(f.undo(path) && f.finding("strings.key_duplicate", path));
	return 0;
}

// A menu's screen and window no by-name lookup finds: a NAME of its own (unique_name's number), none for a
// NAME a stylesheet variable stands for.
static int test_menu_names() {
	Fixture f("opennova_editor_edit_fixes_menu");
	const std::string window =
	        "\t\t<WINDOW type=\"button\" name=\"GO\">\r\n"
	        "\t\t\t<APPEARANCE state=\"default\"></APPEARANCE>\r\n"
	        "\t\t\t<POSITION><LEFT>10</LEFT><TOP>10</TOP></POSITION>\r\n"
	        "\t\t</WINDOW>\r\n";
	const auto screen = [&window](const char *name, int windows) {
		std::string text = std::string("<SCREEN>\r\n\t<NAME>") + name +
		                   "</NAME>\r\n"
		                   "\t<WINDOW type=\"window\" name=\"MAIN\">\r\n"
		                   "\t\t<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>\r\n";
		for (int i = 0; i < windows; ++i) text += window;
		return text + "\t</WINDOW>\r\n</SCREEN>\r\n";
	};
	// Screen A with two windows named GO, then two screens named B.
	TEST_EXPECT(editor_test::write_text(f.root + "/menus/twice.mnu", screen("A", 2) + screen("B", 1) + screen("B", 1)));
	editor_test::handle_to_end(f.session, request::rescan());
	const std::string path = "menus/twice.mnu";

	// The earlier B: B2 (the game finds the last screen of a name).
	const Diagnostic *earlier = f.finding("menu.duplicate_screen", path);
	TEST_EXPECT(earlier != nullptr);
	if (!earlier) return 1;
	std::vector<ProblemFix> fixes = fixes_for(*earlier, f.view());
	TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({"Name it B2"}) && edits_of(fixes, path));
	if (fixes.size() != 1) return 1;
	TEST_EXPECT(f.apply(fixes[0]) && !f.finding("menu.duplicate_screen", path));
	TEST_EXPECT(f.undo(path) && f.finding("menu.duplicate_screen", path));

	// The later GO on A: GO2 (the game finds the first window of a name on its screen).
	TEST_EXPECT(f.count("menu.duplicate_window", path) == 1);
	const Diagnostic *later = f.finding("menu.duplicate_window", path);
	if (!later) return 1;
	fixes = fixes_for(*later, f.view());
	TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({"Name it GO2"}) && edits_of(fixes, path));
	if (fixes.size() != 1) return 1;
	const NodeAddress go{later->row_id, later->record_kind, later->child_id};
	TEST_EXPECT(f.apply(fixes[0]) && f.count("menu.duplicate_window", path) == 0);
	const Document *menu = f.session.document_for(path);
	TEST_EXPECT(menu && menu->record_name(go) == "GO2");
	TEST_EXPECT(f.undo(path) && f.count("menu.duplicate_window", path) == 1);

	// A NAME a stylesheet variable stands for is the variable's: no fix.
	std::string variable = screen("A", 2);
	variable.replace(variable.find("name=\"GO\""), 9, "name=\"%GO%\"");
	variable.replace(variable.find("name=\"GO\""), 9, "name=\"%GO%\"");
	TEST_EXPECT(editor_test::write_text(f.root + "/menus/var.mnu", variable));
	editor_test::handle_to_end(f.session, request::rescan());
	const Diagnostic *named = f.finding("menu.duplicate_window", "menus/var.mnu");
	TEST_EXPECT(named && named->planned.empty() && fixes_for(*named, f.view()).empty());
	return 0;
}

// An item a lookup by its name or its id does not find: a name of its own (the one a copy takes), and an id
// of its own (the project's free id by the reserved-id rule, set: nothing reaches it by the id).
static int test_catalog() {
	Fixture f("opennova_editor_edit_fixes_catalog");
	const AssetEntry *items = f.view().project.scan->find("items.def");
	TEST_EXPECT(items != nullptr);
	if (!items) return 1;
	const std::string path = items->relative_path;
	TEST_EXPECT(editor_test::write_text(f.root + "/" + path,
	                                    "begin \"Marker\"\nid 100001\ntype marker\nhp 10\nend\n"
	                                    "begin \"MARKER\"\nid 100002\ntype marker\nhp 10\nend\n"
	                                    "begin \"Crate\"\nid 100001\ntype marker\nhp 10\nend\n"));
	editor_test::handle_to_end(f.session, request::rescan());

	const Diagnostic *named = f.finding("catalog.name_duplicate", path);
	TEST_EXPECT(named && named->record == "MARKER");
	if (!named) return 1;
	std::vector<ProblemFix> fixes = fixes_for(*named, f.view());
	TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({"Name it MARKER (copy)"}) && edits_of(fixes, path));
	if (fixes.size() != 1) return 1;
	TEST_EXPECT(f.apply(fixes[0]) && !f.finding("catalog.name_duplicate", path));
	TEST_EXPECT(f.undo(path) && f.finding("catalog.name_duplicate", path));

	const Diagnostic *same = f.finding("catalog.item_identity", path);
	TEST_EXPECT(same && same->record == "Crate");
	if (!same) return 1;
	const int free = free_item_id([](int id) { return id == 100001 || id == 100002; });
	fixes = fixes_for(*same, f.view());
	TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({"Use id " + std::to_string(free)}) && edits_of(fixes, path));
	if (fixes.size() != 1) return 1;
	const NodeAddress crate{same->row_id, same->record_kind, same->child_id};
	TEST_EXPECT(f.apply(fixes[0]) && !f.finding("catalog.item_identity", path));
	Value id;
	const Document *catalog = f.session.document_for(path);
	TEST_EXPECT(catalog && catalog->get(crate, "id", id) && id == Value(int64_t(free)));
	TEST_EXPECT(f.undo(path) && f.finding("catalog.item_identity", path));
	return 0;
}

// A trigger on a clip's end pose, which the game never reads: moved onto the last frame it plays, beside
// what that frame fires; only cleared where that frame fires it already.
static int test_end_pose() {
	Fixture f("opennova_editor_edit_fixes_clip");
	size_t last = 0;
	const auto clip_with = [&](uint32_t before, uint32_t end) {
		return edited(fixture("anim/walk.bad"), "walk.bad", AssetKind::Animation, [&](Document &document, Diagnostic &error) {
			const ClipRow *clip = dynamic_cast<const AnimationDocument &>(document).clip();
			if (!clip || clip->version != 1 || clip->events.size() < 2) {
				error.message = "the fixture is no version 1 clip with events";
				return false;
			}
			last = clip->events.size() - 1;
			const NodeKind event = node_kind(AnimationKind::Event);
			return document.apply(set_of({clip->id, event, clip->collections[1][last - 1]}, "trigger", int64_t(before)), error) &&
			       document.apply(set_of({clip->id, event, clip->collections[1][last]}, "trigger", int64_t(end)), error);
		});
	};
	const std::vector<uint8_t> moved = clip_with(0x40, 0x1), held = clip_with(0x1, 0x1);
	TEST_EXPECT(!moved.empty() && !held.empty() && editor_test::write_bytes(f.root + "/anims/moved.bad", moved) &&
	            editor_test::write_bytes(f.root + "/anims/held.bad", held));
	editor_test::handle_to_end(f.session, request::rescan());

	const std::string path = "anims/moved.bad";
	const Diagnostic *end = f.finding("animation.end_pose_trigger", path);
	TEST_EXPECT(end != nullptr);
	if (!end) return 1;
	std::vector<ProblemFix> fixes = fixes_for(*end, f.view());
	TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({"Move it to frame " + std::to_string(last - 1)}) && edits_of(fixes, path));
	TEST_EXPECT(!fixes.empty() && fixes[0].detail.find("beside the sound 2 it fires already") != std::string::npos);
	if (fixes.size() != 1) return 1;
	TEST_EXPECT(f.apply(fixes[0]) && !f.finding("animation.end_pose_trigger", path));
	const AnimationDocument *clip = dynamic_cast<const AnimationDocument *>(f.session.document_for(path));
	TEST_EXPECT(clip && clip->clip() && uint32_t(clip->clip()->events[last - 1].trigger) == 0x41 &&
	            clip->clip()->events[last].trigger == 0);
	TEST_EXPECT(f.undo(path) && f.finding("animation.end_pose_trigger", path));

	const Diagnostic *repeated = f.finding("animation.end_pose_trigger", "anims/held.bad");
	TEST_EXPECT(repeated != nullptr);
	if (!repeated) return 1;
	fixes = fixes_for(*repeated, f.view());
	TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({"Clear it from the end pose"}) && edits_of(fixes, "anims/held.bad"));
	if (fixes.size() != 1) return 1;
	TEST_EXPECT(fixes[0].request.edits.size() == 1);
	TEST_EXPECT(f.apply(fixes[0]) && !f.finding("animation.end_pose_trigger", "anims/held.bad"));
	return 0;
}

// A stylesheet variable no menu names: its line removed, none offered while a menu's text names it inside
// a longer text (the render check's variables; the graph's edges are the whole values alone).
static int test_unused_variable() {
	Fixture f("opennova_editor_edit_fixes_style");
	const AssetEntry *style = f.view().project.scan->find("menu_style.mns");
	TEST_EXPECT(style != nullptr);
	if (!style) return 1;
	const std::string path = style->relative_path;
	std::string text;
	std::string read_error;
	TEST_EXPECT(opennova::io::read_file_text(f.root + "/" + path, text, read_error));
	text += "DI11_UNUSED\tFF00FF00\r\nDI11_INSIDE\tthere\r\n";
	TEST_EXPECT(editor_test::write_text(f.root + "/" + path, text));
	TEST_EXPECT(editor_test::write_text(f.root + "/menus/inside.mnu",
	                                    "<SCREEN>\r\n<NAME>INSIDE</NAME>\r\n<WINDOW type=\"static\" name=\"HELLO\">\r\n"
	                                    "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>90</RIGHT><BOTTOM>20</BOTTOM></POSITION>\r\n"
	                                    "<STRING>Hello %DI11_INSIDE%!</STRING>\r\n</WINDOW>\r\n</SCREEN>\r\n"));
	editor_test::handle_to_end(f.session, request::rescan());
	const auto unused = [&](const char *name) -> const Diagnostic * {
		for (const Diagnostic &d : f.view().findings.diagnostics)
			if (d.code() == "style.unused" && d.asset == path && d.message.find(name) != std::string::npos) return &d;
		return nullptr;
	};
	const Diagnostic *lone = unused("%DI11_UNUSED%"), *inside = unused("%DI11_INSIDE%");
	TEST_EXPECT(lone && inside);
	if (!lone || !inside) return 1;
	TEST_EXPECT(fixes_for(*inside, f.view()).empty());
	const std::vector<ProblemFix> fixes = fixes_for(*lone, f.view());
	TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({"Remove the line"}) && edits_of(fixes, path));
	if (fixes.size() != 1) return 1;
	const size_t lines = f.view().findings.diagnostics.size();
	TEST_EXPECT(f.apply(fixes[0]) && !unused("%DI11_UNUSED%") && unused("%DI11_INSIDE%"));
	TEST_EXPECT(f.view().findings.diagnostics.size() + 1 == lines);
	TEST_EXPECT(f.undo(path) && unused("%DI11_UNUSED%"));
	return 0;
}

int main() {
	if (test_mission_ids() != 0) return 1;
	if (test_string_key() != 0) return 1;
	if (test_menu_names() != 0) return 1;
	if (test_catalog() != 0) return 1;
	if (test_end_pose() != 0) return 1;
	if (test_unused_variable() != 0) return 1;
	return 0;
}
