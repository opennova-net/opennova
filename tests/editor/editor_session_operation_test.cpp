// Pins the session's operations (ADR 0046 S13 A1): the one slot that steps a long job within
// each poll's budget, its progress going only up, a cancel between two steps discarding the
// work, finish() running once on the poll that sees it done; and the busy gate in handle(),
// driven by the request-kind and operation-kind tables: a request that writes what the running
// operation reads or writes, or reads what it writes, is refused, joins it, supersedes it or
// cancels it as it commits, as the rows say, while one that conflicts with nothing it holds (an
// edit, an open, an import's preview while a build packs) goes on. For every request kind the
// gate refuses exactly what busy_refuses says, the answer the windows enable their controls by.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <editor/project/project_files.h>
#include <editor/session/project_session.h>
#include <editor/session/request_kinds.h>
#include <editor/session/session_json.h>
#include <editor/session/session_operation.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
namespace fs = std::filesystem;
using editor_test::FakePlatform;

namespace {

// What the fake operations did, kept past the slot dropping them.
struct Tally {
	size_t steps = 0;
	size_t finishes = 0;
	size_t joins = 0;
	bool cancelled = false;
	bool destroyed = false;
};

// A job of `total` bytes, a step's budget of them a step, of any kind (it reads and writes what
// its row says): the operations no request starts yet.
struct FakeOperation : SessionOperation {
	Tally &tally;
	uint64_t total;
	OperationKind fake_kind;
	uint64_t done = 0;
	bool can_cancel = true;
	// cancellable() answers can_cancel this many more times, then false (-1: can_cancel always):
	// an operation that stops being cancellable once the gate let a request through.
	mutable int cancellable_answers = -1;
	FakeOperation(Tally &t, uint64_t n, OperationKind k = OperationKind::Refresh) : tally(t), total(n), fake_kind(k) {}
	~FakeOperation() override { tally.destroyed = true; }
	OperationKind kind() const override { return fake_kind; }
	bool step(const StepBudget &budget) override {
		++tally.steps;
		done = std::min(total, done + budget.bytes);
		return done == total;
	}
	OperationProgress progress() const override { return {done, total, OperationUnit::Bytes, "Faking"}; }
	bool cancellable() const override {
		if (cancellable_answers == 0) return false;
		if (cancellable_answers > 0) --cancellable_answers;
		return can_cancel;
	}
	void cancel() override { tally.cancelled = true; }
	void join(const EditorRequest &) override { ++tally.joins; }
	OperationOutcome finish(SessionCore &) override {
		++tally.finishes;
		OperationOutcome outcome;
		outcome.findings.push_back(make_diagnostic(DiagnosticSeverity::Info, "fake.done", "Faked."));
		return outcome;
	}
};

// True when the last request was refused because an operation runs (an operation.busy warning).
bool refused_busy(const ProjectSession &session) {
	const std::vector<Diagnostic> &findings = session.outcome().findings;
	return std::any_of(findings.begin(), findings.end(), [](const Diagnostic &d) { return d.code == "operation.busy"; });
}

// An operation of `kind` as the slot shows it: what its row says it reads and writes.
OperationStatus status_of(OperationKind kind, bool cancellable) {
	OperationStatus status;
	status.id = 3;
	status.kind = kind;
	status.cancellable = cancellable;
	status.reads = operation_kind_row(kind).reads;
	status.writes = operation_kind_row(kind).writes;
	return status;
}

} // namespace

// The slot: one operation at a time, stepped within a poll's budget (ms 0: one step; ms n: steps
// while the clock says n milliseconds have not passed), its progress only going up, finish() once.
static int test_slot_steps_and_finishes() {
	editor_test::TempProjectDir dir("opennova_editor_operation_slot");
	FakePlatform platform;
	ProjectSession core(platform, dir.file("settings.json"));
	OperationSlot slot;
	Tally tally;
	TEST_EXPECT(!slot.running() && !slot.status().running() && slot.last().id == 0);
	const uint64_t id = slot.start(std::make_unique<FakeOperation>(tally, 1000));
	TEST_EXPECT(id == 1 && slot.running());
	// A second one is refused and dropped. Its own statement: the argument's destruction, and so the
	// drop, is when the full expression that made it ends (GCC, Clang), or inside start (MSVC).
	Tally other;
	TEST_EXPECT(slot.start(std::make_unique<FakeOperation>(other, 10)) == 0);
	TEST_EXPECT(other.destroyed);
	TEST_EXPECT(other.steps == 0);
	OperationStatus status = slot.status();
	TEST_EXPECT(status.id == id && status.kind == OperationKind::Refresh && status.label == "Faking" && status.total == 1000 &&
	            status.unit == OperationUnit::Bytes && status.cancellable);
	TEST_EXPECT(status.reads == operation_kind_row(OperationKind::Refresh).reads &&
	            status.writes == operation_kind_row(OperationKind::Refresh).writes);

	// One step per poll at ms 0, the budget's bytes a step.
	const OperationClock frozen = [] { return int64_t(0); };
	uint64_t done = 0;
	for (int poll = 0; poll < 3; ++poll) {
		TEST_EXPECT(!slot.poll({0, 100}, frozen));
		TEST_EXPECT(tally.steps == size_t(poll + 1) && slot.status().done == done + 100);
		done = slot.status().done;
	}
	// Steps while the budget's milliseconds last: a clock a millisecond a look, 5 ms, 5 steps.
	int64_t now = 0;
	const OperationClock ticking = [&now] { return now++; };
	const size_t before = tally.steps;
	TEST_EXPECT(!slot.poll({5, 10}, ticking));
	TEST_EXPECT(tally.steps - before >= 2 && tally.steps - before <= 6 && slot.status().done > done);
	// Its progress only goes up, to the total, and it is done then; nothing written before finish.
	done = slot.status().done;
	while (!slot.poll({0, 97}, frozen)) {
		TEST_EXPECT(slot.status().done >= done && slot.status().done <= 1000);
		done = slot.status().done;
	}
	TEST_EXPECT(slot.done() && slot.status().done == 1000 && tally.finishes == 0);
	TEST_EXPECT(slot.poll({0, 97}, frozen) && tally.finishes == 0); // a done one steps no more
	TEST_EXPECT(slot.finish(core) && tally.finishes == 1 && tally.destroyed && !slot.running());
	TEST_EXPECT(slot.last().id == id && slot.last().kind == OperationKind::Refresh && slot.last().end == OperationEnd::Done &&
	            slot.last().findings.size() == 1);
	TEST_EXPECT(!slot.finish(core) && tally.finishes == 1);

	// run_to_end, then finish; the ids never come back.
	Tally third;
	const uint64_t next = slot.start(std::make_unique<FakeOperation>(third, 5000));
	TEST_EXPECT(next == 2 && slot.run_to_end() && slot.done() && slot.finish(core) && third.finishes == 1);
	TEST_EXPECT(slot.last().id == 2);
	return 0;
}

// A cancel lands between two steps: the operation stops, never finishes, and the slot says it
// was cancelled; one that cannot be cancelled is not.
static int test_slot_cancels_between_steps() {
	editor_test::TempProjectDir dir("opennova_editor_operation_cancel");
	FakePlatform platform;
	ProjectSession core(platform, dir.file("settings.json"));
	OperationSlot slot;
	TEST_EXPECT(!slot.cancel());
	Tally tally;
	const uint64_t id = slot.start(std::make_unique<FakeOperation>(tally, 1000));
	const OperationClock frozen = [] { return int64_t(0); };
	slot.poll({0, 100}, frozen);
	slot.poll({0, 100}, frozen);
	TEST_EXPECT(tally.steps == 2 && slot.status().done == 200);
	TEST_EXPECT(slot.cancel());
	TEST_EXPECT(tally.cancelled && tally.destroyed && tally.steps == 2 && tally.finishes == 0);
	TEST_EXPECT(!slot.running() && !slot.done() && !slot.finish(core));
	TEST_EXPECT(slot.last().id == id && slot.last().end == OperationEnd::Cancelled && slot.last().findings.empty());

	Tally stubborn;
	auto operation = std::make_unique<FakeOperation>(stubborn, 10);
	operation->can_cancel = false;
	slot.start(std::move(operation));
	TEST_EXPECT(!slot.status().cancellable && !slot.cancel() && slot.running() && !stubborn.cancelled);
	TEST_EXPECT(slot.run_to_end() && slot.finish(core) && stubborn.finishes == 1);
	return 0;
}

// The tables: every request kind has its row, in the enum's order (static_asserted); a project
// switch and Quit write everything and cancel the running operation as they commit; Build and
// Play join a build; a new import plan supersedes a running one; every operation writes the slot.
// A build reads the files and writes only the slot: what writes the files (a save, a create, an
// import, a rename) is refused, while what reads them (an open, an import's preview, a rename's
// plan) and an edit go on. One that cannot be cancelled refuses what would cancel it, and an
// import plan that cannot be cancelled refuses the plan that would take its place.
static int test_request_table() {
	using K = EditorRequestKind;
	for (size_t i = 0; i < kEditorRequestKindCount; ++i)
		TEST_EXPECT(request_kind_row(static_cast<K>(i)).kind == static_cast<K>(i));
	for (const K kind : {K::NewProject, K::OpenProject, K::CloseProject, K::Quit})
		TEST_EXPECT(request_kind_row(kind).on_busy == OnBusy::CancelRunning && request_kind_row(kind).writes == kHoldsAll);
	TEST_EXPECT(request_kind_row(K::Build).on_busy == OnBusy::Join && request_kind_row(K::Play).on_busy == OnBusy::Join);
	TEST_EXPECT(operation_kind_row(OperationKind::Build).joined_by.has(K::Build) &&
	            operation_kind_row(OperationKind::Build).joined_by.has(K::Play) &&
	            !operation_kind_row(OperationKind::Build).joined_by.has(K::Save));
	for (const K kind : {K::PreviewImport, K::PlanImport, K::PreviewRetailImport, K::SetImportDependencies})
		TEST_EXPECT(request_kind_row(kind).on_busy == OnBusy::Supersede &&
		            operation_kind_row(OperationKind::ImportPlan).superseded_by.has(kind));
	for (size_t k = 0; k < kOperationKindCount; ++k)
		TEST_EXPECT(holds_any(operation_kind_row(static_cast<OperationKind>(k)).writes, HoldsSlot));

	const OperationStatus build = status_of(OperationKind::Build, true);
	TEST_EXPECT(build.reads == HoldsFiles && build.writes == HoldsSlot);
	for (const K kind : {K::Save, K::SaveAll, K::CreateFile, K::RenameAsset, K::AssignRequirement, K::RenameSymbol,
	                     K::ImportFiles, K::Rescan, K::CreateMissing, K::Reimport})
		TEST_EXPECT(gate_answer(kind, build) == GateAnswer::Refuse && busy_refuses(kind, build));
	for (const K kind : {K::EditRecord, K::Undo, K::Redo, K::Cut, K::Paste, K::Copy, K::OpenDocument, K::ReloadDocument,
	                     K::CloseDocument, K::SelectRecord, K::PreviewImport, K::PlanImport, K::SetImportDependencies,
	                     K::PreviewRetailImport, K::PreviewRename, K::StopPlay, K::ApplyProjectSettings})
		TEST_EXPECT(gate_answer(kind, build) == GateAnswer::Proceed && !busy_refuses(kind, build));
	TEST_EXPECT(gate_answer(K::Build, build) == GateAnswer::Join && gate_answer(K::Play, build) == GateAnswer::Join);
	for (const K kind : {K::NewProject, K::OpenProject, K::CloseProject, K::Quit})
		TEST_EXPECT(gate_answer(kind, build) == GateAnswer::CancelRunning && !busy_refuses(kind, build));
	const OperationStatus stubborn_build = status_of(OperationKind::Build, false);
	for (const K kind : {K::NewProject, K::OpenProject, K::CloseProject, K::Quit})
		TEST_EXPECT(gate_answer(kind, stubborn_build) == GateAnswer::Refuse);
	TEST_EXPECT(gate_answer(K::Play, stubborn_build) == GateAnswer::Join);

	TEST_EXPECT(gate_answer(K::PreviewImport, status_of(OperationKind::ImportPlan, true)) == GateAnswer::Supersede);
	TEST_EXPECT(gate_answer(K::PreviewImport, status_of(OperationKind::ImportPlan, false)) == GateAnswer::Refuse);
	// A rename writing the files and the documents refuses an edit, an open and a copy, not a
	// selection; nor does it take a Build.
	const OperationStatus rename = status_of(OperationKind::RenameApply, true);
	for (const K kind : {K::EditRecord, K::OpenDocument, K::Copy, K::PreviewImport, K::Build})
		TEST_EXPECT(busy_refuses(kind, rename));
	TEST_EXPECT(!busy_refuses(K::SelectRecord, rename) && !busy_refuses(K::ApplyProjectSettings, rename));
	TEST_EXPECT(gate_answer(K::Save, OperationStatus()) == GateAnswer::Proceed && !busy_refuses(K::Save, OperationStatus()));
	return 0;
}

// For every request kind the gate refuses (an operation.busy warning) exactly what busy_refuses
// says, which is what the windows enable their controls by (SessionView::allows; the editor_ui
// test test_windows_show_the_gate draws them): under a real build of an open project, a request
// of each kind in turn (the project opened and the build started again when one closed or
// cancelled it), and under a fake of every operation kind, cancellable and not, in a session of
// its own.
static int test_gate_is_what_busy_refuses_says() {
	editor_test::TempProjectDir dir("opennova_editor_operation_every_kind");
	FakePlatform platform;
	const auto agree = [](EditorRequestKind kind, const OperationStatus &running, bool refused) {
		if (refused == busy_refuses(kind, running)) return true;
		std::fprintf(stderr, "under %s (%s): the gate %s %s, the windows %s it\n", operation_kind_row(running.kind).token,
		             running.cancellable ? "cancellable" : "not cancellable", refused ? "refused" : "let through",
		             editor_request_kind_token(kind), busy_refuses(kind, running) ? "disable" : "enable");
		return false;
	};
	ProjectSession session(platform, dir.file("settings.json"));
	const SessionView &v = session.view();
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Every"));
	editor_test::create_missing_files(session);
	session.set_poll_budget({0, 16});
	for (size_t i = 0; i < kEditorRequestKindCount; ++i) {
		const EditorRequestKind kind = static_cast<EditorRequestKind>(i);
		if (!v.project_open) session.handle(make_request(EditorRequestKind::OpenProject, dir.file("project")));
		if (!v.operation.running()) session.handle(make_request(EditorRequestKind::Build));
		const OperationStatus running = v.operation;
		TEST_EXPECT(v.project_open && running.running() && running.kind == OperationKind::Build);
		session.handle(make_request(kind));
		TEST_EXPECT(agree(kind, running, refused_busy(session)));
	}
	for (size_t k = 0; k < kOperationKindCount; ++k) {
		for (const bool cancellable : {true, false}) {
			for (size_t i = 0; i < kEditorRequestKindCount; ++i) {
				const EditorRequestKind kind = static_cast<EditorRequestKind>(i);
				Tally tally; // outlives the session, which drops the operation
				ProjectSession fresh(platform, dir.file("fresh.json"));
				auto operation = std::make_unique<FakeOperation>(tally, 1000, static_cast<OperationKind>(k));
				operation->can_cancel = cancellable;
				TEST_EXPECT(fresh.start_operation(std::move(operation)) != 0);
				const OperationStatus running = fresh.view().operation;
				TEST_EXPECT(running.running() && running.cancellable == cancellable);
				fresh.handle(make_request(kind));
				TEST_EXPECT(agree(kind, running, refused_busy(fresh)));
			}
		}
	}
	return 0;
}

// The busy gate over a real build, stepped slowly: a Save is refused (operation.busy, nothing
// written) and so is the unsaved prompt's Save, while its Discard goes on (a build reads no
// document); an open, an edit and an import's preview go on, the import is refused; a Build joins
// the build, a Play joins it and starts the game when it lands; a project's close cancels it as
// it commits, and a project switch that fails keeps it.
static int test_busy_gate() {
	editor_test::TempProjectDir dir("opennova_editor_operation_gate");
	FakePlatform platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Gate"));
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	const std::string root = v.project_root;
	const std::string runtime = dir.file("runtime/opennova.exe");
	TEST_EXPECT(editor_test::write_text(runtime, "MZ"));
	PlayLauncher launcher;
	launcher.executable = runtime;
	session.set_launcher(launcher);
	TEST_EXPECT(editor_test::write_text(root + "/defs/items.def", "begin \"Marker\"\nid 100001\ntype marker\nhp 10\nend\n"));
	session.handle(make_request(EditorRequestKind::Rescan));
	session.set_poll_budget({0, 256});

	session.handle(make_request(EditorRequestKind::Build));
	TEST_EXPECT(session.outcome().done() && v.operation.running() && v.operation.reads == HoldsFiles &&
	            v.operation.writes == HoldsSlot && v.operation.cancellable);
	const uint64_t build = v.operation.id;
	TEST_EXPECT(session.outcome().operation == build);
	session.poll();

	// An open and an edit go on while the build packs; a Save does not.
	const auto open_items = [&session]() -> Document * {
		session.handle(make_request(EditorRequestKind::OpenDocument, "items.def"));
		Document *document = session.document_for("items.def");
		return session.outcome().done() && document && !document->rows().empty() ? document : nullptr;
	};
	Document *items = open_items();
	TEST_EXPECT(items != nullptr);
	if (!items) return 1;
	EditorRequest edit = make_request(EditorRequestKind::EditRecord, items->path());
	edit.edit.address = {items->rows()[0]->id, items->rows()[0]->kind, 0};
	edit.edit.field = "hp";
	edit.edit.value = int64_t(20);
	session.handle(edit);
	TEST_EXPECT(session.outcome().done() && items->dirty() && v.operation.id == build);
	for (const EditorRequestKind kind : {EditorRequestKind::Save, EditorRequestKind::SaveAll}) {
		session.handle(make_request(kind, kind == EditorRequestKind::Save ? items->path() : std::string()));
		TEST_EXPECT(!session.outcome().done() && session.outcome().findings.size() == 1 &&
		            session.outcome().findings[0].code == "operation.busy" &&
		            session.outcome().findings[0].severity == DiagnosticSeverity::Warning);
		TEST_EXPECT(session.outcome().findings[0].message == "Wait for the build to finish, or cancel it, first.");
		TEST_EXPECT(items->dirty() && v.operation.id == build);
	}
	std::string text, error;
	TEST_EXPECT(read_file_text(root + "/defs/items.def", text, error) && text.find("hp 10") != std::string::npos);
	// The unsaved prompt's Save writes files too: refused, the prompt kept. Its Discard drops the
	// document, which the build does not read: it goes on.
	session.handle(make_request(EditorRequestKind::CloseDocument, items->path()));
	TEST_EXPECT(session.outcome().unsaved_prompt && v.unsaved_prompt.open);
	EditorRequest answer = make_request(EditorRequestKind::ResolveUnsaved);
	answer.unsaved_choice = UnsavedChoice::Save;
	session.handle(answer);
	TEST_EXPECT(session.outcome().unsaved_prompt && v.unsaved_prompt.open && items->dirty() &&
	            session.outcome().findings.size() == 1 && session.outcome().findings[0].code == "operation.busy");
	answer.unsaved_choice = UnsavedChoice::Discard;
	session.handle(answer);
	TEST_EXPECT(session.outcome().done() && !v.unsaved_prompt.open && session.document_for("items.def") == nullptr &&
	            v.operation.id == build);
	items = open_items();
	TEST_EXPECT(items != nullptr && !items->dirty());
	if (!items) return 1;
	edit.edit.address = {items->rows()[0]->id, items->rows()[0]->kind, 0};
	session.handle(edit);
	TEST_EXPECT(session.outcome().done() && items->dirty() && v.operation.id == build);

	// An import's preview reads the files: it goes on beside the build, which is not what it
	// supersedes; the import it plans writes them: refused.
	EditorRequest preview = make_request(EditorRequestKind::PreviewImport);
	TEST_EXPECT(editor_test::write_text(dir.file("loose.txt"), "loose"));
	preview.paths = {dir.file("loose.txt")};
	session.handle(preview);
	TEST_EXPECT(session.outcome().done() && v.import_preview.open && v.operation.id == build);
	session.handle(make_request(EditorRequestKind::ImportFiles));
	TEST_EXPECT(!session.outcome().done() && refused_busy(session) && v.operation.id == build);
	session.handle(make_request(EditorRequestKind::CancelImport));
	TEST_EXPECT(!v.import_preview.open);

	// A Build with unsaved edits asks about them first: it would not join a build that packs the
	// files without them. The prompt's Save waits for the build (refused, the prompt kept).
	session.handle(make_request(EditorRequestKind::Build));
	TEST_EXPECT(session.outcome().unsaved_prompt && v.unsaved_prompt.open &&
	            v.unsaved_prompt.action == EditorRequestKind::Build && session.outcome().operation == 0 &&
	            v.operation.id == build);
	answer.unsaved_choice = UnsavedChoice::Save;
	session.handle(answer);
	TEST_EXPECT(session.outcome().unsaved_prompt && v.unsaved_prompt.open && items->dirty() &&
	            session.outcome().findings.size() == 1 && session.outcome().findings[0].code == "operation.busy");
	answer.unsaved_choice = UnsavedChoice::Cancel;
	session.handle(answer);
	TEST_EXPECT(!v.unsaved_prompt.open && v.operation.id == build);

	// The edit undone, nothing is unsaved: a Build joins the build; a Play joins it too and starts
	// the game when it lands.
	session.handle(make_request(EditorRequestKind::Undo, items->path()));
	TEST_EXPECT(!items->dirty());
	session.handle(make_request(EditorRequestKind::Build));
	TEST_EXPECT(session.outcome().done() && session.outcome().operation == build && v.operation.id == build);
	session.handle(make_request(EditorRequestKind::Play));
	TEST_EXPECT(session.outcome().done() && session.outcome().operation == build && v.operation.id == build &&
	            platform.spawns == 0);
	TEST_EXPECT(action_outcome_to_json(session.outcome()).get_number("operation", 0.0) == double(build));
	session.run_operations();
	TEST_EXPECT(v.last_operation.id == build && v.last_operation.end == OperationEnd::Done && v.last_build.ok);
	TEST_EXPECT(platform.spawns == 1 && v.play_state == PlayState::Running);
	// With the build done, the edit made again is saved.
	session.handle(edit);
	session.handle(make_request(EditorRequestKind::Save, items->path()));
	TEST_EXPECT(session.outcome().done() && !items->dirty());
	TEST_EXPECT(read_file_text(root + "/defs/items.def", text, error) && text.find("hp 20") != std::string::npos);

	// A project's close with unsaved edits asks about them first, the build packing on: a Cancel
	// keeps the build; the prompt's Save cancels it (the close would), writes the file and closes.
	session.handle(make_request(EditorRequestKind::StopPlay));
	session.poll();
	session.handle(make_request(EditorRequestKind::Build));
	const uint64_t second = v.operation.id;
	TEST_EXPECT(second > build && v.operation.running());
	session.poll();
	edit.edit.value = int64_t(30);
	session.handle(edit);
	TEST_EXPECT(items->dirty() && v.operation.id == second);
	session.handle(make_request(EditorRequestKind::CloseProject));
	TEST_EXPECT(session.outcome().unsaved_prompt && v.unsaved_prompt.open && v.project_open && v.operation.id == second);
	answer.unsaved_choice = UnsavedChoice::Cancel;
	session.handle(answer);
	TEST_EXPECT(v.project_open && v.operation.id == second && v.last_operation.id == build);
	session.handle(make_request(EditorRequestKind::CloseProject));
	TEST_EXPECT(session.outcome().unsaved_prompt && v.unsaved_prompt.open);
	answer.unsaved_choice = UnsavedChoice::Save;
	session.handle(answer);
	TEST_EXPECT(session.outcome().done() && !v.project_open && !v.operation.running());
	TEST_EXPECT(v.last_operation.id == second && v.last_operation.end == OperationEnd::Cancelled);
	TEST_EXPECT(read_file_text(root + "/defs/items.def", text, error) && text.find("hp 30") != std::string::npos);

	// With nothing unsaved a close cancels the build at once: nothing of it is kept.
	session.handle(make_request(EditorRequestKind::OpenProject, dir.file("project")));
	session.handle(make_request(EditorRequestKind::Build));
	const uint64_t third = v.operation.id;
	TEST_EXPECT(third > second && v.operation.running());
	session.poll();
	session.handle(make_request(EditorRequestKind::CloseProject));
	TEST_EXPECT(session.outcome().done() && !v.project_open && !v.operation.running());
	TEST_EXPECT(v.last_operation.id == third && v.last_operation.end == OperationEnd::Cancelled);

	// A project switch cancels the build only once its own checks pass: a NewProject where a
	// project is already, and an OpenProject of a folder that holds none, fail and keep it; one
	// that goes through cancels it.
	session.handle(make_request(EditorRequestKind::OpenProject, dir.file("project")));
	session.handle(make_request(EditorRequestKind::Build));
	const uint64_t fourth = v.operation.id;
	TEST_EXPECT(fourth > third && v.operation.running());
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Again"));
	TEST_EXPECT(!session.outcome().done() && v.project_open && v.document.title == "Gate" && v.operation.id == fourth &&
	            v.last_operation.id == third);
	TEST_EXPECT(editor_test::write_text(dir.file("empty/readme.txt"), "no project here"));
	session.handle(make_request(EditorRequestKind::OpenProject, dir.file("empty")));
	TEST_EXPECT(!session.outcome().done() && v.project_open && v.document.title == "Gate" && v.operation.id == fourth);
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("second"), "Second"));
	TEST_EXPECT(session.outcome().done() && v.project_open && v.document.title == "Second" && !v.operation.running());
	TEST_EXPECT(v.last_operation.id == fourth && v.last_operation.end == OperationEnd::Cancelled);
	return 0;
}

// An operation that cannot be cancelled keeps the slot: a project switch and Quit, which cancel
// the running operation as they commit, are refused at the gate as busy_refuses says, nothing
// closed or made; one that stops being cancellable once the gate let the request through is
// refused where the request commits (close_project, Quit, a new project before its folder is
// made), nothing closed or made; and the unsaved prompt's Save and Discard are refused when what
// waits (a project's close) could not cancel the operation, the prompt and the edits kept: a
// rename's, which writes the documents a Discard drops, and a build's, which holds none.
static int test_uncancellable_operation() {
	using K = EditorRequestKind;
	editor_test::TempProjectDir dir("opennova_editor_operation_stubborn");
	FakePlatform platform;
	Tally tally, second, packed; // outlive the session, which drops the operations
	ProjectSession session(platform, dir.file("settings.json"));
	const SessionView &v = session.view();
	session.handle(make_request(K::NewProject, dir.file("other"), "Other"));
	session.handle(make_request(K::NewProject, dir.file("project"), "Stubborn"));
	TEST_EXPECT(v.project_open && v.document.title == "Stubborn");
	TEST_EXPECT(editor_test::write_text(v.project_root + "/defs/items.def",
	                                    "begin \"Marker\"\nid 100001\ntype marker\nhp 10\nend\n"));
	session.handle(make_request(K::Rescan));
	session.handle(make_request(K::OpenDocument, "items.def"));
	Document *items = session.document_for("items.def");
	TEST_EXPECT(items != nullptr && !items->rows().empty());
	if (!items || items->rows().empty()) return 1;

	auto owned = std::make_unique<FakeOperation>(tally, 1000, OperationKind::RenameApply);
	FakeOperation *rename = owned.get();
	rename->can_cancel = false;
	const uint64_t id = session.start_operation(std::move(owned));
	TEST_EXPECT(id != 0 && v.operation.id == id && v.operation.kind == OperationKind::RenameApply && !v.operation.cancellable);
	const EditorRequest switches[] = {make_request(K::CloseProject), make_request(K::OpenProject, dir.file("other")),
	                                  make_request(K::NewProject, dir.file("third"), "Third"), make_request(K::Quit)};
	for (const EditorRequest &request : switches) {
		TEST_EXPECT(busy_refuses(request.kind, v.operation));
		session.handle(request);
		TEST_EXPECT(!session.outcome().done() && refused_busy(session));
		TEST_EXPECT(session.outcome().findings.back().message == "Wait for the rename to finish first.");
		TEST_EXPECT(v.project_open && v.document.title == "Stubborn" && v.operation.id == id && !v.quit_requested &&
		            !tally.cancelled);
	}
	TEST_EXPECT(!fs::exists(dir.file("third")));
	session.handle(make_request(K::CancelOperation));
	TEST_EXPECT(!session.outcome().done() && session.outcome().findings.size() == 1 &&
	            session.outcome().findings[0].code == "operation.not_cancellable" && v.operation.id == id);

	// Cancellable when the gate asks, not when the request commits: refused there, nothing closed
	// and no folder made (a new project's is made only once the open one has closed).
	rename->can_cancel = true;
	for (const EditorRequest &request : {switches[1], switches[2], switches[3], switches[0]}) {
		rename->cancellable_answers = 1;
		session.handle(request);
		TEST_EXPECT(!session.outcome().done() && refused_busy(session));
		TEST_EXPECT(v.project_open && v.document.title == "Stubborn" && v.operation.id == id && !v.quit_requested &&
		            !tally.cancelled);
	}
	TEST_EXPECT(!fs::exists(dir.file("third")));
	rename->cancellable_answers = -1;
	session.handle(make_request(K::CancelOperation));
	TEST_EXPECT(session.outcome().done() && tally.cancelled && !v.operation.running());

	// With unsaved edits, a project's close asks first. The rename that runs meanwhile (it writes
	// the documents) turned stubborn: the prompt's Save and Discard cannot cancel it for the
	// close, refused and the prompt kept, the edits with it.
	EditorRequest edit = make_request(K::EditRecord, items->path());
	edit.edit.address = {items->rows()[0]->id, items->rows()[0]->kind, 0};
	edit.edit.field = "hp";
	edit.edit.value = int64_t(20);
	session.handle(edit);
	TEST_EXPECT(session.outcome().done() && items->dirty());
	auto again = std::make_unique<FakeOperation>(second, 1000, OperationKind::RenameApply);
	FakeOperation *held = again.get();
	const uint64_t next = session.start_operation(std::move(again));
	TEST_EXPECT(next > id);
	session.handle(make_request(K::CloseProject));
	TEST_EXPECT(session.outcome().unsaved_prompt && v.unsaved_prompt.open && v.operation.id == next);
	held->can_cancel = false;
	EditorRequest answer = make_request(K::ResolveUnsaved);
	for (const UnsavedChoice choice : {UnsavedChoice::Save, UnsavedChoice::Discard}) {
		answer.unsaved_choice = choice;
		session.handle(answer);
		TEST_EXPECT(session.outcome().unsaved_prompt && refused_busy(session) && v.unsaved_prompt.open);
		TEST_EXPECT(v.project_open && items->dirty() && v.operation.id == next && !second.cancelled);
	}
	std::string text, error;
	TEST_EXPECT(read_file_text(v.project_root + "/defs/items.def", text, error) && text.find("hp 10") != std::string::npos);
	answer.unsaved_choice = UnsavedChoice::Cancel;
	session.handle(answer);
	held->can_cancel = true;
	session.handle(make_request(K::CancelOperation));
	TEST_EXPECT(!v.unsaved_prompt.open && second.cancelled && !v.operation.running() && items->dirty());

	// A build holds no document, so a Discard drops nothing it reads; but the close it answers
	// would be refused once answered (the build turned stubborn), so the answer is refused first:
	// the prompt, the document and its edits stay. Cancellable again, a Discard cancels the build
	// and the project closes.
	auto build = std::make_unique<FakeOperation>(packed, 1000, OperationKind::Build);
	FakeOperation *packing = build.get();
	const uint64_t building = session.start_operation(std::move(build));
	TEST_EXPECT(building > next);
	session.handle(make_request(K::CloseProject));
	TEST_EXPECT(session.outcome().unsaved_prompt && v.unsaved_prompt.open && v.operation.id == building);
	packing->can_cancel = false;
	for (const UnsavedChoice choice : {UnsavedChoice::Discard, UnsavedChoice::Save}) {
		answer.unsaved_choice = choice;
		session.handle(answer);
		TEST_EXPECT(session.outcome().unsaved_prompt && refused_busy(session) && v.unsaved_prompt.open);
		TEST_EXPECT(v.project_open && session.document_for("items.def") == items && items->dirty() &&
		            v.operation.id == building && !packed.cancelled);
	}
	packing->can_cancel = true;
	answer.unsaved_choice = UnsavedChoice::Discard;
	session.handle(answer);
	TEST_EXPECT(session.outcome().done() && !v.project_open && packed.cancelled && !v.operation.running());
	TEST_EXPECT(v.last_operation.id == building && v.last_operation.end == OperationEnd::Cancelled);
	TEST_EXPECT(read_file_text(dir.file("project") + "/defs/items.def", text, error) && text.find("hp 10") != std::string::npos);
	return 0;
}

// A new import plan takes the place of a running one (its row's superseded_by): the plan is
// cancelled, never finished, and the new preview runs; one that cannot be cancelled keeps the
// slot and the preview is refused, as busy_refuses says.
static int test_import_plan_superseded() {
	editor_test::TempProjectDir dir("opennova_editor_operation_supersede");
	FakePlatform platform;
	Tally first, second, third; // outlive the session, which drops the operations
	ProjectSession session(platform, dir.file("settings.json"));
	const SessionView &v = session.view();
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Supersede"));
	TEST_EXPECT(editor_test::write_text(dir.file("loose.txt"), "loose"));
	EditorRequest preview = make_request(EditorRequestKind::PreviewImport);
	preview.paths = {dir.file("loose.txt")};

	const uint64_t id = session.start_operation(std::make_unique<FakeOperation>(first, 1000, OperationKind::ImportPlan));
	TEST_EXPECT(id != 0 && v.operation.id == id &&
	            gate_answer(EditorRequestKind::PreviewImport, v.operation) == GateAnswer::Supersede);
	session.handle(preview);
	TEST_EXPECT(session.outcome().done() && v.import_preview.open);
	TEST_EXPECT(first.cancelled && first.destroyed && first.finishes == 0 && !v.operation.running());
	TEST_EXPECT(v.last_operation.id == id && v.last_operation.end == OperationEnd::Cancelled);
	session.handle(make_request(EditorRequestKind::CancelImport));
	TEST_EXPECT(!v.import_preview.open);

	auto stubborn = std::make_unique<FakeOperation>(second, 1000, OperationKind::ImportPlan);
	stubborn->can_cancel = false;
	const uint64_t kept = session.start_operation(std::move(stubborn));
	TEST_EXPECT(kept > id && busy_refuses(EditorRequestKind::PreviewImport, v.operation));
	session.handle(preview);
	TEST_EXPECT(!session.outcome().done() && refused_busy(session) && !v.import_preview.open);
	TEST_EXPECT(!second.cancelled && v.operation.id == kept);
	// No second operation starts beside it.
	TEST_EXPECT(session.start_operation(std::make_unique<FakeOperation>(third, 10)) == 0);
	TEST_EXPECT(third.destroyed);
	TEST_EXPECT(v.operation.id == kept);
	return 0;
}

// The settings' Apply is never refused whole (the settings dialog waits on its result): each part
// is weighed against the running operation inside, and a refused part is a failure the result
// carries under the request's serial, the rest written. A build reads the files: the features
// (whose refresh reads and writes them) wait, the name does not. An operation that writes the
// project (opening one) refuses the name and the game install too, never the editor's own
// settings.
static int test_settings_parts_weighed() {
	editor_test::TempProjectDir dir("opennova_editor_operation_settings");
	FakePlatform platform;
	Tally tally; // outlives the session, which drops the operation
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Features"));
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	session.set_poll_budget({0, 256});
	session.handle(make_request(EditorRequestKind::Build));
	TEST_EXPECT(v.operation.running() && !busy_refuses(EditorRequestKind::ApplyProjectSettings, v.operation));
	EditorRequest apply = make_request(EditorRequestKind::ApplyProjectSettings);
	apply.settings.serial = 9;
	apply.settings.title = std::string("Renamed");
	apply.settings.mission = true;
	session.handle(apply);
	TEST_EXPECT(v.settings_result.serial == 9 && v.settings_result.failures.size() == 1 &&
	            v.settings_result.failures[0].code == "operation.busy");
	TEST_EXPECT(v.settings_result.failures[0].message ==
	            "Wait for the build to finish, or cancel it, before changing the project's features.");
	TEST_EXPECT(v.document.title == "Renamed" && !v.document.features.mission && v.operation.running());
	session.run_operations();
	session.handle(apply);
	TEST_EXPECT(v.settings_result.serial == 9 && v.settings_result.failures.empty() && v.document.features.mission);

	// An operation that writes the project: the name and the game install wait, the runtime (the
	// editor's own setting) is written.
	const uint64_t id = session.start_operation(std::make_unique<FakeOperation>(tally, 1000, OperationKind::Open));
	TEST_EXPECT(id != 0 && !busy_refuses(EditorRequestKind::ApplyProjectSettings, v.operation));
	const std::string install = v.retail_directory;
	EditorRequest held = make_request(EditorRequestKind::ApplyProjectSettings);
	held.settings.serial = 11;
	held.settings.title = std::string("Held");
	held.settings.retail_directory = dir.file("install");
	held.settings.runtime_executable = dir.file("runtime/opennova.exe");
	session.handle(held);
	TEST_EXPECT(v.settings_result.serial == 11 && v.settings_result.failures.size() == 2);
	for (const Diagnostic &failure : v.settings_result.failures) TEST_EXPECT(failure.code == "operation.busy");
	TEST_EXPECT(v.document.title == "Renamed" && v.retail_directory == install &&
	            v.runtime_setting == dir.file("runtime/opennova.exe"));
	TEST_EXPECT(v.operation.id == id && !tally.cancelled);
	return 0;
}

int main() {
	int failures = 0;
	failures += test_slot_steps_and_finishes();
	failures += test_slot_cancels_between_steps();
	failures += test_request_table();
	failures += test_gate_is_what_busy_refuses_says();
	failures += test_busy_gate();
	failures += test_uncancellable_operation();
	failures += test_import_plan_superseded();
	failures += test_settings_parts_weighed();
	if (failures == 0) std::printf("editor_session_operation: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
