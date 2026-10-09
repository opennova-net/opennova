// Dialog banks followed (ADR 0046 DI-32): a mission's .dbf as a record document over the engine's own reader and
// from-scratch writer (each dialog a row holding its lines), its dialogs' names the graph's Dialog symbols, which a
// mission's Play dialog actions and Dialog triggers reference by the number their name forms (dlg%03i: Go to, used
// by, a rename writing the number, Add it there), a line's wave one of the bank's sounds (<bank>.lwf), and the
// session's play_sound of a dialog: its lines one after another as the game plays them, each the wave of its name at
// its dialog volume, its subtitle in the words. Minted fixtures alone (a bank, its sounds, a mission and its text
// minted here through the engine's writers, the short PCM wave); the retail leg (OPENNOVA_JO_ASSETS) reads every
// shipped dialog bank through the document and writes it back byte for byte, every line's wave in its sounds.
#include <editor/blank/blank_factory.h>
#include <editor/documents/dialog_bank_document.h>
#include <editor/documents/document_types.h>
#include <editor/documents/mission_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/display_names.h>
#include <editor/graph/reference_kinds.h>
#include <editor/preview/dialog_preview.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>
#include <formats/dbf/dbf.h>
#include <formats/lwf/lwf.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/audio/dialog_queue.h>
#include <runtime/mission/mission_sidecars.h>

#include <base/io/os_path.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

constexpr NodeKind kDialog = node_kind(DialogBankKind::Dialog);
constexpr NodeKind kLine = node_kind(DialogBankKind::Line);

std::string repo() { return test_paths_repo_root(__FILE__); }

Edit set_edit(NodeAddress address, const char *field, Value value) {
	Edit edit;
	edit.address = address;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

bool has_code(const std::vector<Diagnostic> &findings, const char *code) {
	return std::any_of(findings.begin(), findings.end(), [&](const Diagnostic &d) { return d.code() == code; });
}

dbf::Line line_of(const char *wave, const char *sequence = "##", uint8_t delay = 0) {
	dbf::Line line;
	line.def_id_name = wave;
	line.sequence = sequence;
	line.delay = delay;
	return line;
}

// A bank: dlg001 two lines (the second after half a second, its subtitle the text's entry 1), dlg002 one line naming a
// wave its sounds lack; with `findings`, a second dlg002, a dialog no number forms, one of no line and one whose line
// names no wave.
std::vector<uint8_t> dbf_bytes(bool findings = false) {
	dbf::File bank;
	dbf::Group one;
	one.group_name = "dlg001";
	one.lines = {line_of("Z01R100"), line_of("Z01R101", "_00001", 5)};
	bank.groups.push_back(one);
	dbf::Group two;
	two.group_name = "dlg002";
	two.lines = {line_of("Z99R999")};
	bank.groups.push_back(two);
	if (findings) {
		dbf::Group again = two;
		bank.groups.push_back(again);
		dbf::Group intro;
		intro.group_name = "intro";
		intro.lines = {line_of("Z01R100")};
		bank.groups.push_back(intro);
		dbf::Group silent;
		silent.group_name = "dlg010";
		bank.groups.push_back(silent);
		dbf::Group nameless;
		nameless.group_name = "dlg011";
		nameless.lines = {line_of("")};
		bank.groups.push_back(nameless);
	}
	std::vector<uint8_t> out;
	std::string error;
	dbf::encode_dbf(bank, out, error);
	return out;
}

// The bank's sounds: Z01R100 at the dialog volume 210, Z01R101 at 0 (full), each its file.
std::vector<uint8_t> sounds_bytes() {
	lwf::File bank;
	for (const auto &[name, volume] : std::vector<std::pair<const char *, uint16_t>>{{"Z01R100", 0xD200}, {"Z01R101", 0}}) {
		lwf::Single single;
		single.name = name;
		single.path = strutil::to_lower(name) + ".wav";
		single.value_hi = volume;
		bank.singles.push_back(single);
	}
	std::vector<uint8_t> out;
	std::string error;
	lwf::encode_lwf(bank, out, error);
	return out;
}

// The mission's text: [Mission Dialog] Z01R100's subtitle, and an [Info] entry the second line's _00001 names.
std::vector<uint8_t> text_bytes() {
	rtxt::File text;
	text.sections = {rtxt::Section{"Mission Dialog", 1}, rtxt::Section{"Info", 1}};
	rtxt::Entry said, info;
	said.key = "Z01R100";
	said.text = "Move out.";
	info.key = "TITLE";
	info.text = "Hold the gate.";
	info.section_index = 1;
	text.entries = {said, info};
	std::vector<uint8_t> out;
	std::string error;
	rtxt::write(text, out, error);
	return out;
}

// A mission whose pre-mission event plays dialog 1, a second event dialog 2 and a third dialog 3 (which the bank
// lacks); a fourth a Dialog trigger on dialog 1.
std::vector<uint8_t> mission_bytes() {
	bms::File file;
	mission::make_default(file);
	const auto event_playing = [&](int dialog, uint32_t flags) {
		mission::MissionEventRecord event;
		event.flags = int(flags);
		const size_t index = mission::add_event(file, event);
		mission::MissionActionRecord action;
		action.action_type = int(bms::ActionType::PlayWavList);
		action.param1 = dialog;
		std::string error;
		mission::insert_event_action(file, index, 0, action, error);
		return index;
	};
	event_playing(1, uint32_t(bms::EventFlags::PreMission));
	event_playing(2, 0);
	event_playing(3, 0);
	const size_t done = event_playing(0, 0);
	mission::MissionTriggerRecord trigger;
	trigger.main_type = int(bms::TriggerMainType::Player);
	trigger.sub_type = int(bms::PlayerTriggerType::PlayerDialogFinished);
	trigger.param1 = 1;
	std::string error;
	mission::insert_event_trigger(file, done, 0, trigger, error);
	mission::sync_counts(file);
	std::vector<uint8_t> bytes;
	if (!bms::write(file, bytes, error)) bytes.clear();
	return bytes;
}

// --- the document -------------------------------------------------------------------------------------------

// The minted bank reads into its dialogs and lines and writes back the bytes it was minted with (our writer made
// both); the titles say each dialog's waves and a line's wait.
int test_reads_and_writes_back() {
	const std::vector<uint8_t> bytes = dbf_bytes(true);
	DialogBankDocument bank;
	Diagnostic error;
	TEST_EXPECT(bank.load_bytes(bytes, "missions/talk.dbf", AssetKind::DialogBank, "jo", error));
	TEST_EXPECT(bank.dialogs().size() == 6);
	const DialogBankRow *one = bank.find_dialog("dlg001");
	TEST_EXPECT(one && one->dialog.lines.size() == 2 && one->dialog.lines[1].wave == "Z01R101" &&
	            one->dialog.lines[1].sequence == "_00001" && one->dialog.lines[1].delay == 5);
	if (!one) return 1;
	TEST_EXPECT(bank.record_title({one->id, kDialog, 0}) == "dlg001: Z01R100, Z01R101");
	TEST_EXPECT(bank.record_title({one->id, kLine, one->ids.lists[0][1].id}) == "Z01R101 after 0.5 s");
	const SerializeResult written = bank.serialize();
	TEST_EXPECT(written.ok() && std::vector<uint8_t>(written.text.begin(), written.text.end()) == bytes);
	TEST_EXPECT(bank.rewrite_need() == DocumentBase::RewriteNeed::None);
	Value number;
	TEST_EXPECT(bank.get({one->id, kDialog, 0}, "number", number) && std::get<int64_t>(number) == 1);
	// The scopes: the bank's own name, its sounds' (.lwf, else .pwf).
	TEST_EXPECT(dialog_bank_scope("missions/talk.dbf") == "TALK.DBF" && dialog_sounds_scope("missions/talk.dbf") == "TALK.LWF" &&
	            dialog_sounds_alternate("missions/talk.dbf") == "TALK.PWF");
	return 0;
}

// Every field edits through the table: a dialog's name, a line's wave, subtitle entry and wait; a new dialog takes the
// first free dlg%03i, its line added in one batch; written and read back by the engine's reader; the refusals.
int test_edits() {
	DialogBankDocument bank;
	Diagnostic error;
	TEST_EXPECT(bank.load_bytes(dbf_bytes(), "talk.dbf", AssetKind::DialogBank, "jo", error));
	const DialogBankRow *two = bank.find_dialog("dlg002");
	TEST_EXPECT(two != nullptr);
	if (!two) return 1;
	const NodeAddress line{two->id, kLine, two->ids.lists[0][0].id};
	TEST_EXPECT(bank.apply({set_edit(line, "wave", std::string("Z01R101")), set_edit(line, "sequence", std::string("_00007")),
	                        set_edit(line, "delay", int64_t(30))},
	                       error));
	TEST_EXPECT(!bank.apply(set_edit(line, "delay", int64_t(256)), error));
	TEST_EXPECT(!bank.apply(set_edit({two->id, kDialog, 0}, "name", std::string(24, 'd')), error));
	TEST_EXPECT(!bank.apply(set_edit({two->id, kDialog, 0}, "number", int64_t(5)), error)); // derived
	// A new dialog: dlg003, the first no dialog has; its line in the same batch.
	Edit add;
	add.operation = EditOperation::Add;
	add.address = {0, kDialog, 0};
	Edit add_line;
	add_line.operation = EditOperation::Add;
	add_line.address = {batch_made(0), kLine, 0};
	TEST_EXPECT(bank.apply({add, add_line}, error));
	const DialogBankRow *made = bank.find_dialog("dlg003");
	TEST_EXPECT(made && made->dialog.lines.size() == 1 && made->dialog.lines[0].sequence == "##" &&
	            made->dialog.lines[0].def_id_index == 0xFF);
	if (!made) return 1;
	TEST_EXPECT(bank.apply(set_edit({made->id, kLine, made->ids.lists[0][0].id}, "wave", std::string("Z01R100")), error));
	const SerializeResult written = bank.serialize();
	dbf::File file;
	std::string message;
	TEST_EXPECT(written.ok() && dbf::parse_dbf_memory(reinterpret_cast<const uint8_t *>(written.text.data()),
	                                                  written.text.size(), file, message));
	TEST_EXPECT(file.groups.size() == 3 && file.groups[1].lines[0].def_id_name == "Z01R101" &&
	            file.groups[1].lines[0].sequence == "_00007" && file.groups[1].lines[0].delay == 30 &&
	            file.groups[2].group_name == "dlg003" && file.groups[2].lines[0].def_id_name == "Z01R100");
	// A copy of a dialog takes the next free name, as a new one does.
	Edit duplicate;
	duplicate.operation = EditOperation::Duplicate;
	duplicate.address = {made->id, kDialog, 0};
	TEST_EXPECT(bank.apply(duplicate, error) && bank.find_dialog("dlg004"));
	return 0;
}

// The bank's findings: a second dialog of a name, a name no number forms, a dialog of no line, a line of no wave.
int test_findings() {
	DialogBankDocument bank;
	Diagnostic error;
	TEST_EXPECT(bank.load_bytes(dbf_bytes(true), "talk.dbf", AssetKind::DialogBank, "jo", error));
	const std::vector<Diagnostic> findings = validate_dialog_bank_file(bank);
	TEST_EXPECT(has_code(findings, "dialog_bank.name_repeated") && has_code(findings, "dialog_bank.name_unplayed") &&
	            has_code(findings, "dialog_bank.silent") && has_code(findings, "dialog_bank.line_no_wave"));
	TEST_EXPECT(findings.size() == 4);
	DialogBankDocument clean;
	TEST_EXPECT(clean.load_bytes(dbf_bytes(), "talk.dbf", AssetKind::DialogBank, "jo", error) &&
	            validate_dialog_bank_file(clean).empty());
	// The blank: a bank of no dialog the reader takes.
	BlankRequest request;
	request.logical_name = "talk.dbf";
	std::vector<uint8_t> bytes;
	TEST_EXPECT(make_blank(request, AssetKind::DialogBank, bytes, error));
	DialogBankDocument blank;
	TEST_EXPECT(blank.load_bytes(bytes, "talk.dbf", AssetKind::DialogBank, "jo", error) && blank.rows().empty());
	TEST_EXPECT(find_blank_factory_for_kind(AssetKind::DialogBank) != nullptr);
	return 0;
}

// --- the project ------------------------------------------------------------------------------------------

struct TalkProject {
	editor_test::TempProjectDir dir{"opennova_editor_dialog_bank"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string root;
	bool made = false;
	TalkProject() {
		editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Talk"));
		editor_test::create_missing_files(session);
		root = session.view().project.root;
		const std::vector<uint8_t> tone = test_io::read_file(repo() + "/fixtures/lwf/tone.wav");
		made = !tone.empty() && editor_test::write_bytes(root + "/missions/talk.bms", mission_bytes()) &&
		       editor_test::write_bytes(root + "/missions/talk.dbf", dbf_bytes()) &&
		       editor_test::write_bytes(root + "/missions/talk.lwf", sounds_bytes()) &&
		       editor_test::write_bytes(root + "/missions/talk.bin", text_bytes()) &&
		       editor_test::write_bytes(root + "/sounds/z01r100.wav", tone) &&
		       editor_test::write_bytes(root + "/sounds/z01r101.wav", tone);
		editor_test::handle_to_end(session, request::rescan());
		while (session.view().activity.validation.running) session.poll();
	}
	const SessionView &view() const { return session.view(); }
	const Diagnostic *missing(ReferenceKind kind, const std::string &target) const {
		for (const Diagnostic &d : view().findings.diagnostics)
			if (const ReferenceSubject *subject = reference_subject(d);
			    subject && d.code() == "reference.missing" && subject->kind == kind && subject->target == target)
				return &d;
		return nullptr;
	}
};

const GraphEdge *edge_named(const AssetGraph &graph, const std::string &file, ReferenceKind kind, const std::string &value) {
	for (const GraphEdge *edge : graph.references_of(file))
		if (edge->kind == kind && edge->value == value) return edge;
	return nullptr;
}

// A Play dialog names its dialog by the number that forms dlg%03i in the mission's dialog bank: present for 1 and 2,
// the dialog's users the action and the trigger; missing for 3, a warning saying the bank has none. A line's wave is
// one of the bank's sounds (talk.lwf), found without case; one the sounds lack a warning, its words the game's "EX
// Cannot load audio". The picker lists the bank's dialogs by number.
int test_graph() {
	TalkProject project;
	TEST_EXPECT(project.made);
	const AssetGraph *graph = project.view().findings.graph.get();
	TEST_EXPECT(graph != nullptr);
	if (!graph) return 1;
	const GraphEdge *one = edge_named(*graph, "missions/talk.bms", ReferenceKind::Dialog, "dlg001");
	const GraphEdge *three = edge_named(*graph, "missions/talk.bms", ReferenceKind::Dialog, "dlg003");
	TEST_EXPECT(one && one->scope == "TALK.DBF" && graph->resolve(*one) == ReferenceStatus::Present);
	TEST_EXPECT(three && graph->resolve(*three) == ReferenceStatus::Missing);
	const std::vector<const GraphSymbol *> dialogs = graph->symbols_named(ReferenceKind::Dialog, "dlg001");
	TEST_EXPECT(dialogs.size() == 1 && dialogs.front()->file == "missions/talk.dbf" && dialogs.front()->scope == "TALK.DBF");
	if (dialogs.size() == 1) TEST_EXPECT(graph->users_of(*dialogs.front()).size() == 2); // the action and the trigger
	const Diagnostic *lacking = project.missing(ReferenceKind::Dialog, "dlg003");
	TEST_EXPECT(lacking && lacking->severity == DiagnosticSeverity::Warning &&
	            lacking->message.find("TALK.DBF has no dialog of") != std::string::npos);
	const GraphEdge *wave = edge_named(*graph, "missions/talk.dbf", ReferenceKind::BankWave, "Z01R100");
	TEST_EXPECT(wave && wave->scope == "TALK.LWF" && wave->scope_alternate == "TALK.PWF" &&
	            graph->resolve(*wave) == ReferenceStatus::Present);
	const Diagnostic *unloaded = project.missing(ReferenceKind::BankWave, "Z99R999");
	TEST_EXPECT(unloaded && unloaded->severity == DiagnosticSeverity::Warning &&
	            unloaded->message.find("EX Cannot load audio") != std::string::npos);
	// The action's parameter: a keyed Dialog reference, its picker the bank's dialogs by number.
	const Document *mission = nullptr;
	project.session.handle(request::open_document("missions/talk.bms"));
	mission = project.session.document_for("missions/talk.bms");
	TEST_EXPECT(mission != nullptr);
	if (!mission) return 1;
	const auto *missions = static_cast<const MissionDocument *>(mission);
	TEST_EXPECT(mission_dialog_bank(*missions) == "talk.dbf");
	const Node *event = missions->rows_of(MissionKind::Event).size() > 1 ? missions->rows_of(MissionKind::Event)[1] : nullptr;
	TEST_EXPECT(event != nullptr);
	if (!event) return 1;
	const NodeAddress action{event->id, node_kind(MissionKind::Action), static_cast<const EventRow &>(*event).ids.lists[1][0].id};
	for (const FieldSchema &schema : mission->fields(action.kind)) {
		if (schema.id != "param1") continue;
		const FieldUse use = mission->field_on(action, schema);
		FieldUse keyed;
		Value key;
		TEST_EXPECT(keyed_reference(*graph, use, Value(int64_t(2)), keyed, key) && keyed.reference == ReferenceKind::Dialog &&
		            key == Value(std::string("dlg002")) && keyed.scope == "TALK.DBF");
		std::vector<std::string> numbers;
		for (const ReferenceChoice &choice : picker_choices(graph, *mission, action, use, nullptr)) numbers.push_back(choice.name);
		TEST_EXPECT(numbers == std::vector<std::string>({"1", "2"}));
	}
	return 0;
}

// Rename everywhere of a dialog writes the number its new name forms into each Play dialog and Dialog trigger naming
// it; a name no number forms is refused there. Add it there adds the dialog a mission names and the bank lacks.
int test_rename_and_add_there() {
	TalkProject project;
	TEST_EXPECT(project.made);
	const AssetGraph *graph = project.view().findings.graph.get();
	const std::vector<const GraphSymbol *> dialogs = graph ? graph->symbols_named(ReferenceKind::Dialog, "dlg001") : std::vector<const GraphSymbol *>();
	TEST_EXPECT(dialogs.size() == 1);
	if (dialogs.size() != 1) return 1;
	const GraphSymbol symbol = *dialogs.front();
	// A name no number forms: refused at the mission's uses.
	const ActionOutcome refused =
	        editor_test::handle_to_end(project.session, request::rename_symbol(symbol.file, symbol.locator, symbol.field, "intro"));
	TEST_EXPECT(!refused.done() && std::any_of(refused.findings.begin(), refused.findings.end(), [](const Diagnostic &d) {
		return d.message.find("is no name a number forms") != std::string::npos;
	}));
	const ActionOutcome renamed =
	        editor_test::handle_to_end(project.session, request::rename_symbol(symbol.file, symbol.locator, symbol.field, "dlg007"));
	for (const Diagnostic &d : renamed.findings) std::fprintf(stderr, "rename: %s\n", d.message.c_str());
	TEST_EXPECT(renamed.done());
	while (project.session.view().activity.validation.running) project.session.poll();
	bms::File file;
	std::string error;
	const std::vector<uint8_t> bytes = test_io::read_file(project.root + "/missions/talk.bms");
	TEST_EXPECT(bms::parse(bytes.data(), bytes.size(), file, error));
	size_t sevens = 0;
	for (const bms::Action &action : file.actions)
		if (action.action_type == bms::ActionType::PlayWavList && action.param1 == 7) ++sevens;
	for (const bms::Trigger &trigger : file.triggers) sevens += trigger.param1 == 7 ? 1 : 0;
	TEST_EXPECT(sevens == 2);
	DialogBankDocument bank;
	Diagnostic loaded;
	TEST_EXPECT(bank.load(project.root + "/missions/talk.dbf", "missions/talk.dbf", AssetKind::DialogBank, "jo", loaded) &&
	            bank.find_dialog("dlg007") && !bank.find_dialog("dlg001"));
	// Add it there: the dialog 3 the mission names, added to talk.dbf.
	const Diagnostic *lacking = project.missing(ReferenceKind::Dialog, "dlg003");
	TEST_EXPECT(lacking != nullptr);
	if (!lacking) return 1;
	const std::vector<ProblemFix> fixes = fixes_for(*lacking, project.view());
	TEST_EXPECT(!fixes.empty() && fixes.front().label == "Add dlg003 to talk.dbf");
	if (fixes.empty()) return 1;
	editor_test::handle_to_end(project.session, fixes.front().request);
	while (project.session.view().activity.validation.running) project.session.poll();
	TEST_EXPECT(project.session.outcome().done() && !project.missing(ReferenceKind::Dialog, "dlg003"));
	return 0;
}

// The session's play_sound of a dialog: from the bank, its lines one after another (the second once the first has
// ended, after its half second), each the project's wave at its dialog volume, the subtitles said; from the mission,
// the same bank by the mission's name; a line alone; the refusals.
int test_session_play() {
	TalkProject project;
	TEST_EXPECT(project.made);
	ProjectSession &session = project.session;
	const WorkspaceView::Sound &sound = session.view().workspace.sound;
	session.handle(request::play_dialog("dlg001", "missions/talk.dbf"));
	TEST_EXPECT(sound.state == WorkspaceView::SoundState::Starting && sound.set == "dlg001" && sound.bank == "talk.dbf" &&
	            sound.voices.size() == 2);
	if (sound.voices.size() == 2) {
		TEST_EXPECT(sound.voices[0].path == "sounds/z01r100.wav" && sound.voices[0].volume == 210 && sound.voices[0].start_ms == 0);
		// The tone's length, then the second line's half second.
		TEST_EXPECT(sound.voices[1].path == "sounds/z01r101.wav" && sound.voices[1].volume == 255 &&
		            sound.voices[1].start_ms > 500);
	}
	TEST_EXPECT(sound.words.find("\"Move out.\"") != std::string::npos && sound.words.find("\"Hold the gate.\"") != std::string::npos);
	const uint64_t serial = sound.serial;
	// From the mission: dialog 1 of the bank the mission loads.
	session.handle(request::play_dialog("1", "missions/talk.bms"));
	TEST_EXPECT(sound.serial == serial + 1 && sound.set == "dlg001" && sound.voices.size() == 2);
	// One line alone, at once.
	session.handle(request::play_dialog("dlg001", "missions/talk.dbf", 1));
	TEST_EXPECT(sound.serial == serial + 2 && sound.voices.size() == 1 && sound.voices[0].start_ms == 0 &&
	            sound.voices[0].path == "sounds/z01r101.wav");
	// Refused: a dialog the bank lacks, one whose line plays a wave the sounds lack, a line past the dialog, no bank.
	session.handle(request::play_dialog("dlg003", "missions/talk.dbf"));
	TEST_EXPECT(sound.serial == serial + 2 && session.view().activity.status.find("has no dialog dlg003") != std::string::npos);
	session.handle(request::play_dialog("dlg002", "missions/talk.dbf"));
	TEST_EXPECT(sound.serial == serial + 2 && session.view().activity.status.find("EX Cannot load audio") != std::string::npos);
	session.handle(request::play_dialog("dlg001", "missions/talk.dbf", 2));
	TEST_EXPECT(sound.serial == serial + 2);
	session.handle(request::play_dialog("dlg001", "sounds/z01r100.wav"));
	TEST_EXPECT(sound.serial == serial + 2);
	// The wire: the second voice's start.
	const JsonValue workspace = workspace_to_json(session.view());
	(void)workspace;
	session.handle(request::play_dialog("dlg001", "missions/talk.dbf"));
	const JsonValue again = workspace_to_json(session.view());
	const JsonValue *played = again.get("sound");
	const JsonValue *voices = played ? played->get("voices") : nullptr;
	TEST_EXPECT(voices && voices->array.size() == 2 && voices->array[1].get_number("at", 0) > 0.5);
	return 0;
}

// --- retail -------------------------------------------------------------------------------------------------

// Every shipped dialog bank through the document, written back byte for byte; every line's wave in the bank's own
// sounds (the .LWF of its name), as the game's lookup finds it.
int test_retail_banks() {
	const std::string assets = retail::assets();
	if (assets.empty()) {
		retail::skip_leg("OPENNOVA_JO_ASSETS (the shipped dialog banks through the document)");
		return 0;
	}
	size_t banks = 0, exact = 0, lines = 0, found = 0;
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(io::os_path(assets), ec)) {
		const std::string name = io::utf8_path(entry.path().filename());
		if (strutil::to_lower(entry.path().extension().string()) != ".dbf") continue;
		const std::vector<uint8_t> bytes = test_io::read_file(io::utf8_path(entry.path()));
		DialogBankDocument bank;
		Diagnostic error;
		TEST_EXPECT(bank.load_bytes(bytes, name, AssetKind::DialogBank, "jo", error));
		const SerializeResult written = bank.serialize();
		// Byte for byte, or (04TR.DBF: its sequences hold "##" over an older "_0000N", the bytes past the terminator
		// that no reader reaches [orig: Dialog_LoadAudioClip @ 0x44ddec, strlen]) every value the game reads the same.
		if (written.ok() && std::vector<uint8_t>(written.text.begin(), written.text.end()) == bytes) ++exact;
		dbf::File original, again;
		std::string message;
		TEST_EXPECT(written.ok() && dbf::parse_dbf_memory(bytes.data(), bytes.size(), original, message) &&
		            dbf::parse_dbf_memory(reinterpret_cast<const uint8_t *>(written.text.data()), written.text.size(), again,
		                                  message));
		bool same = original.groups.size() == again.groups.size();
		for (size_t g = 0; same && g < original.groups.size(); ++g) {
			const dbf::Group &a = original.groups[g], &b = again.groups[g];
			same = a.group_name == b.group_name && a.idlist_count == b.idlist_count && a.def_id_indices == b.def_id_indices &&
			       a.lines.size() == b.lines.size();
			for (size_t l = 0; same && l < a.lines.size(); ++l)
				same = a.lines[l].def_id_name == b.lines[l].def_id_name && a.lines[l].sequence == b.lines[l].sequence &&
				       a.lines[l].delay == b.lines[l].delay && a.lines[l].line_flags == b.lines[l].line_flags &&
				       a.lines[l].def_id_index == b.lines[l].def_id_index && a.lines[l].param == b.lines[l].param;
		}
		if (!same) std::fprintf(stderr, "%s does not read back the same\n", name.c_str());
		TEST_EXPECT(same);
		const std::string sounds_path = retail::asset_file((strutil::to_lower(mission::mission_base_name(name)) + ".lwf").c_str());
		lwf::File sounds;
		const std::vector<uint8_t> sound_bytes = sounds_path.empty() ? std::vector<uint8_t>() : test_io::read_file(sounds_path);
		const bool read = !sound_bytes.empty() && lwf::parse_lwf_buffer(sound_bytes.data(), sound_bytes.size(), sounds, message);
		for (const DialogBankRow *row : bank.dialogs())
			for (const BankLine &line : row->dialog.lines) {
				++lines;
				found += read && audio::find_dialog_wave(sounds, line.wave) ? 1 : 0;
			}
		++banks;
	}
	std::printf("retail: %zu dialog banks (%zu written back byte for byte), %zu lines, %zu waves found in their sounds\n",
	            banks, exact, lines, found);
	TEST_EXPECT(banks >= 17 && exact + 1 >= banks && lines > 0 && found == lines);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failed = 0;
	failed += test_reads_and_writes_back();
	failed += test_edits();
	failed += test_findings();
	failed += test_graph();
	failed += test_rename_and_add_there();
	failed += test_session_play();
	failed += test_retail_banks();
	if (failed == 0) std::printf("editor dialog bank: all tests passed\n");
	return failed == 0 ? 0 : 1;
}
