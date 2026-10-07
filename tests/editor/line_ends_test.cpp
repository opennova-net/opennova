// The line-ends rule (documents/line_ends.h): every kind whose game reader ends a line at CR LF and
// nowhere else (assets/asset_kinds.h, LineReader) has a file whose lines an LF ends alone said in
// Problems as document.line_ends, what the game reads of it named, with one fix, Restore CR LF line
// ends, one step Undo takes back. The game's own walk [orig: File_ParseASCIIFile @ 0x53D8DE] ends a
// line only where a CR is followed by an LF, so a def of LF line ends is one line, which it skips when
// its first word starts with '/': the editor reads it so (no record), and says so. Covered: a weapon
// table of LF line ends (a comment first: nothing defined; restored, its two weapons, Undo gives the
// one-line reading back and Redo the two again); one whose one-line reading blocks (the restore taken
// while blocked, its undo blocked again and its redo taken still); the sound profiles, whose writer
// ends every line CR LF itself (the rule reads the source the document was read from); a mixed file
// (the lines the game reads as one); a text kind (the avatars, the text type: the span of its text
// replaced) and the HUD layout (Save ends its lines CR LF too); none for a CR LF file nor for a kind of
// another reader; and through a session: the finding over a closed weapon.def, its fix an edit_record
// of the file opened first, which the wire carries and reads back as it was.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/document_types.h>
#include <editor/documents/line_ends.h>
#include <editor/model/document.h>
#include <editor/model/text_document.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_json.h>
#include <editor/session/view/session_view.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;

namespace {

std::vector<uint8_t> bytes_of(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

std::string lf_only(const std::string &text) {
	std::string out;
	for (size_t i = 0; i < text.size(); ++i)
		if (!(text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')) out += text[i];
	return out;
}

std::unique_ptr<DocumentBase> loaded(const std::string &text, const char *name, AssetKind kind) {
	std::unique_ptr<DocumentBase> document = document_type_for(kind)->make();
	Diagnostic error;
	if (!document->load_bytes(bytes_of(text), name, kind, "jo", error)) {
		std::fprintf(stderr, "%s: %s\n", name, error.message.c_str());
		return nullptr;
	}
	return document;
}

bool contains(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

size_t rows_of(const DocumentBase &document) {
	const Document *records = records_of(document);
	return records ? records->rows().size() : 0;
}

// The one finding the rule makes, its fix the restore; null where it makes another number.
const Diagnostic *the_finding(const std::vector<Diagnostic> &findings) {
	if (findings.size() != 1 || findings[0].code() != "document.line_ends" ||
	    findings[0].severity != DiagnosticSeverity::Warning || findings[0].planned.size() != 1)
		return nullptr;
	const PlannedFix &fix = findings[0].planned[0];
	if (fix.label != "Restore CR LF line ends" || fix.edits.size() != 1 || !is_line_ends_restore(fix.edits[0]))
		return nullptr;
	return &findings[0];
}

const std::string kWeapons = "// The weapons\r\nweapon \"WPN_ONE\"\r\n\tname \"One\"\r\nend\r\n"
                             "weapon \"WPN_TWO\"\r\nend\r\n";

} // namespace

// A weapon table of LF line ends, a comment first: one line to the game, which it skips; the editor
// reads no weapon, and says so. Restored: its two weapons, the file read as Save writes it; Undo gives the
// one-line reading back, the finding with it, and Redo the two weapons again.
static int test_weapons_whole_file() {
	std::unique_ptr<DocumentBase> document = loaded(lf_only(kWeapons), "weapon.def", AssetKind::WeaponDefs);
	TEST_EXPECT(document && rows_of(*document) == 0 && !document->blocked());
	if (!document) return 1;
	const std::vector<Diagnostic> findings = line_end_findings(*document, "jo");
	const Diagnostic *d = the_finding(findings);
	TEST_EXPECT(d != nullptr);
	if (!d) return 1;
	TEST_EXPECT(d->line == 1 && d->asset == "weapon.def");
	TEST_EXPECT(contains(d->message, "None of the file's 6 lines ends CR LF") &&
	            contains(d->message, "reads the whole file as one line, which it skips: it begins with a comment.") &&
	            contains(d->message, "Read so, the file defines 0 records; with CR LF line ends it defines 2."));
	TEST_EXPECT(contains(d->planned[0].detail, "the game then reads its 6 lines, 2 records"));
	Diagnostic error;
	TEST_EXPECT(document->apply(d->planned[0].edits, error));
	TEST_EXPECT(rows_of(*document) == 2 && document->dirty() && line_end_findings(*document, "jo").empty());
	const Document &records = *records_of(*document);
	TEST_EXPECT(records.source_state() && !records.source_state()->odd_lines);
	TEST_EXPECT(document->serialize().text == kWeapons);
	document->undo();
	TEST_EXPECT(rows_of(*document) == 0 && !document->dirty() && the_finding(line_end_findings(*document, "jo")));
	document->redo();
	TEST_EXPECT(rows_of(*document) == 2 && document->dirty() && line_end_findings(*document, "jo").empty());
	// A restore of a file whose lines end CR LF changes nothing: no step.
	const uint64_t revision = document->revision();
	TEST_EXPECT(document->apply(line_ends_restore(), error) && document->revision() == revision);
	std::printf("weapons, whole file: one line skipped, 0 records against 2; restored, undone, redone\n");
	return 0;
}

// A weapon table whose one-line reading blocks (an unfinished block): the finding, and its restore
// taken by the blocked document; its undo blocks it again, and its redo is taken still.
static int test_blocked_reading() {
	const std::string text = "weapon \"WPN_ONE\"\r\n\tname \"One\"\r\nend\r\nweapon \"WPN_TWO\"\r\nend\r\n";
	std::unique_ptr<DocumentBase> document = loaded(lf_only(text), "weapon.def", AssetKind::WeaponDefs);
	TEST_EXPECT(document && document->blocked());
	if (!document) return 1;
	const std::vector<Diagnostic> findings = line_end_findings(*document, "jo");
	const Diagnostic *d = the_finding(findings);
	TEST_EXPECT(d && contains(d->message, "reads the whole file as one line.") &&
	            contains(d->message, "with CR LF line ends it defines 2."));
	if (!d) return 1;
	Diagnostic error;
	// Any other edit of the blocked document is refused, as before.
	Edit set;
	set.address = {1, 0, 0};
	set.field = "name";
	set.value = std::string("x");
	TEST_EXPECT(!document->apply(set, error) && error.code() == "document.parse");
	TEST_EXPECT(document->apply(line_ends_restore(), error) && !document->blocked() && rows_of(*document) == 2);
	TEST_EXPECT(document->serialize().text == text);
	document->undo();
	TEST_EXPECT(document->blocked() && rows_of(*document) == 1 && document->can_redo());
	document->redo();
	TEST_EXPECT(!document->blocked() && rows_of(*document) == 2);
	std::printf("blocked reading: restored, undone (blocked again), redone\n");
	return 0;
}

// The sound profiles' writer ends every line CR LF itself, so the rule reads the source the document was
// read from: the finding stands though a Save would write CR LF (and the one profile the game reads).
static int test_canonical_writer() {
	const std::string text = "begin \"default\"\r\n\tSSLFootGND FSP_DIRT_L 0 0 0\r\nend\r\n"
	                         "begin \"SP_Truck\"\r\n\tsoundloop_1 V_TRUCK_ILP 0.8 1.2 2\r\nend\r\n";
	std::unique_ptr<DocumentBase> document = loaded(lf_only(text), "SndProf.def", AssetKind::SoundProfileDefs);
	TEST_EXPECT(document && rows_of(*document) == 1);
	if (!document) return 1;
	TEST_EXPECT(contains(document->serialize().text, "\r\n"));
	const std::vector<Diagnostic> findings = line_end_findings(*document, "jo");
	const Diagnostic *d = the_finding(findings);
	TEST_EXPECT(d && contains(d->message, "Read so, the file defines 1 record; with CR LF line ends it defines 2."));
	Diagnostic error;
	TEST_EXPECT(d && document->apply(d->planned[0].edits, error) && rows_of(*document) == 2 &&
	            line_end_findings(*document, "jo").empty());
	std::printf("sound profiles: the source read, not what Save writes\n");
	return 0;
}

// A CR LF file with one LF alone: the lines the game reads as one, keyed by the first's word; a CR LF
// file and a kind of another reader make none.
static int test_mixed_and_none() {
	std::string text = kWeapons;
	const size_t name = text.find("\tname");
	text.erase(name - 2, 1); // line 2's CR
	std::unique_ptr<DocumentBase> document = loaded(text, "weapon.def", AssetKind::WeaponDefs);
	TEST_EXPECT(document != nullptr);
	if (!document) return 1;
	const std::vector<Diagnostic> findings = line_end_findings(*document, "jo");
	const Diagnostic *d = the_finding(findings);
	TEST_EXPECT(d && d->line == 2 &&
	            contains(d->message, "Line 2 ends with an LF alone: the game's reader ends a line at CR LF and nowhere "
	                                 "else, so it reads lines 2 to 3 as one line."));
	std::unique_ptr<DocumentBase> clean = loaded(kWeapons, "weapon.def", AssetKind::WeaponDefs);
	TEST_EXPECT(clean && line_end_findings(*clean, "jo").empty());
	// A configuration's reader is another (none of the rule's): its LF line ends are its own.
	std::unique_ptr<DocumentBase> config = loaded("a = 1\nb = 2\n", "notes.cfg", AssetKind::Config);
	TEST_EXPECT(config && line_end_findings(*config, "jo").empty());
	std::printf("mixed: lines 2 to 3 as one; none for CR LF, none for a configuration\n");
	return 0;
}

// A text kind (the avatars, held by the text type as the file stores it): the finding at the first LF
// alone, its restore the span of the text from it to the last written CR LF, one step; the HUD layout,
// whose Save ends its lines CR LF itself, says so.
static int test_texts() {
	const std::string text = "nationality N00 FIRST\n{\n}\r\nnationality N01 SECOND\r\n{\r\n}\n";
	std::unique_ptr<DocumentBase> document = loaded(text, "Avatars.def", AssetKind::AvatarDefs);
	TEST_EXPECT(document && text_of(*document));
	if (!document) return 1;
	const std::vector<Diagnostic> findings = line_end_findings(*document, "jo");
	const Diagnostic *d = the_finding(findings);
	TEST_EXPECT(d && d->line == 1 && d->column == 22 &&
	            contains(d->message, "Line 1 ends with an LF alone (3 line ends in the file are so)") &&
	            contains(d->message, "reads lines 1 to 3 as one line.") && !contains(d->message, "Save"));
	if (!d) return 1;
	// The text's batch form reads its op alone too, back as it was.
	const EditorRequest fix = request::edit_record("Avatars.def", d->planned[0].edits, true);
	EditorRequest back;
	std::string why;
	TEST_EXPECT(editor_request_from_json(editor_request_to_json(fix), back, why) && back == fix);
	Diagnostic error;
	TEST_EXPECT(document->apply(d->planned[0].edits, error));
	TEST_EXPECT(text_of(*document)->text() == "nationality N00 FIRST\r\n{\r\n}\r\nnationality N01 SECOND\r\n{\r\n}\r\n" &&
	            line_end_findings(*document, "jo").empty());
	document->undo();
	TEST_EXPECT(text_of(*document)->text() == text && !document->dirty());
	std::unique_ptr<DocumentBase> layout = loaded("HUDHEALTH 25,741,177,751\nHUDCLIP 14,648\r\n", "hudpos.def",
	                                              AssetKind::HudPosDefs);
	const std::vector<Diagnostic> hud_findings = layout ? line_end_findings(*layout, "jo") : std::vector<Diagnostic>();
	const Diagnostic *hud = the_finding(hud_findings);
	TEST_EXPECT(hud && hud->line == 1 && contains(hud->message, "Save ends every line CR LF."));
	std::printf("texts: the avatars' span restored and undone; the HUD layout's Save said\n");
	return 0;
}

// Through a session: a closed weapon.def of LF line ends is a Problems row, its fix an edit_record of the
// file opened first (the wire writes it as its op and reads it back the same); applied, the two weapons
// and no finding; Undo gives the finding back.
static int test_session() {
	editor_test::TempProjectDir dir("opennova_editor_line_ends");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Line ends"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	std::string path;
	for (const AssetEntry &entry : view.project.scan->entries)
		if (entry.kind == AssetKind::WeaponDefs) path = entry.relative_path;
	TEST_EXPECT(!path.empty());
	if (path.empty()) return 1;
	TEST_EXPECT(editor_test::write_bytes(view.project.root + "/" + path, bytes_of(lf_only(kWeapons))));
	editor_test::handle_to_end(session, request::rescan());
	const auto finding = [&]() -> const Diagnostic * {
		for (const Diagnostic &d : session.view().findings.diagnostics)
			if (d.code() == "document.line_ends" && d.asset == path) return &d;
		return nullptr;
	};
	const Diagnostic *d = finding();
	TEST_EXPECT(d && session.document_for(path) == nullptr);
	if (!d) return 1;
	const std::vector<ProblemFix> fixes = fixes_for(*d, view);
	TEST_EXPECT(fixes.size() == 1 && fixes[0].label == "Restore CR LF line ends" && !fixes[0].bulk &&
	            fixes[0].request.kind == EditorRequestKind::EditRecord && fixes[0].request.path == path &&
	            fixes[0].request.open_first && contains(fixes[0].detail, "Undo takes it back"));
	if (fixes.size() != 1) return 1;
	// The wire: its op alone, read back as it was.
	const opennova::io::JsonValue json = editor_request_to_json(fixes[0].request);
	const std::string wire = opennova::io::json_write(json);
	TEST_EXPECT(contains(wire, "restore_line_ends") && !contains(wire, "payload"));
	EditorRequest back;
	std::string why;
	TEST_EXPECT(editor_request_from_json(json, back, why) && back == fixes[0].request);
	editor_test::handle_to_end(session, fixes[0].request);
	const Document *weapons = session.document_for(path);
	TEST_EXPECT(session.outcome().done() && weapons && weapons->dirty() && weapons->rows().size() == 2 && !finding());
	editor_test::handle_to_end(session, request::undo(path));
	TEST_EXPECT(weapons && weapons->rows().empty() && !weapons->dirty() && finding());
	std::printf("session: the closed file's row, its fix over the wire, applied and undone\n");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_weapons_whole_file();
	failures += test_blocked_reading();
	failures += test_canonical_writer();
	failures += test_mixed_and_none();
	failures += test_texts();
	failures += test_session();
	if (failures == 0) std::printf("editor_line_ends: all passed\n");
	return failures == 0 ? 0 : 1;
}
