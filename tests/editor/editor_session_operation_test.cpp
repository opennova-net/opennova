// Pins the session's operations (ADR 0046 S13 A1): the one slot that steps a long job within
// each poll's budget, its progress going only up, a cancel between two steps discarding the
// work, finish() running once on the poll that sees it done; and the busy gate in handle(),
// driven by the request-kind table: a request that needs what the running operation holds is
// refused, joins it, supersedes it or cancels it first, as its row says, while one that needs
// nothing it holds (an edit while a build packs) goes on.
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

using namespace opennova::editor;
namespace fs = std::filesystem;

namespace {

struct FakePlatform : ProcessPlatform {
	int64_t next_pid = 700;
	std::vector<int64_t> running;
	LaunchPlan last_plan;
	int spawns = 0;
	int64_t spawn(const LaunchPlan &plan) override {
		last_plan = plan;
		++spawns;
		running.push_back(next_pid);
		return next_pid++;
	}
	bool is_running(int64_t pid) override {
		for (int64_t p : running) if (p == pid) return true;
		return false;
	}
	bool terminate(int64_t pid) override { return kill(pid); }
	bool kill(int64_t pid) override {
		for (size_t i = 0; i < running.size(); ++i)
			if (running[i] == pid) running.erase(running.begin() + static_cast<std::ptrdiff_t>(i));
		return true;
	}
	void release(int64_t) override {}
	int64_t now_ms() override { return 0; }
	void sleep_ms(int64_t) override {}
};

// What the fake operations did, kept past the slot dropping them.
struct Tally {
	size_t steps = 0;
	size_t finishes = 0;
	bool cancelled = false;
	bool destroyed = false;
};

// A job of `total` bytes, a step's budget of them a step.
struct FakeOperation : SessionOperation {
	Tally &tally;
	uint64_t total;
	uint64_t done = 0;
	bool can_cancel = true;
	FakeOperation(Tally &t, uint64_t n) : tally(t), total(n) {}
	~FakeOperation() override { tally.destroyed = true; }
	OperationKind kind() const override { return OperationKind::Refresh; }
	bool step(const StepBudget &budget) override {
		++tally.steps;
		done = std::min(total, done + budget.bytes);
		return done == total;
	}
	OperationProgress progress() const override { return {done, total, OperationUnit::Bytes, "Faking"}; }
	bool cancellable() const override { return can_cancel; }
	void cancel() override { tally.cancelled = true; }
	OperationOutcome finish(SessionCore &) override {
		++tally.finishes;
		OperationOutcome outcome;
		outcome.findings.push_back(make_diagnostic(DiagnosticSeverity::Info, "fake.done", "Faked."));
		return outcome;
	}
};

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
	Tally other;
	TEST_EXPECT(slot.start(std::make_unique<FakeOperation>(other, 10)) == 0 && other.destroyed && other.steps == 0);
	OperationStatus status = slot.status();
	TEST_EXPECT(status.id == id && status.kind == OperationKind::Refresh && status.label == "Faking" && status.total == 1000 &&
	            status.unit == OperationUnit::Bytes && status.cancellable &&
	            status.holds == operation_kind_row(OperationKind::Refresh).holds);

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

// The table: every request kind has its row, in the enum's order (static_asserted); a project
// switch and Quit cancel the running operation first; Build and Play join a build; a new import
// plan supersedes a running one; the rest are refused when the operation holds what they need.
// What a build holds (the files) refuses a Save and not an edit.
static int test_request_table() {
	for (size_t i = 0; i < kEditorRequestKindCount; ++i)
		TEST_EXPECT(request_kind_row(static_cast<EditorRequestKind>(i)).kind == static_cast<EditorRequestKind>(i));
	for (const EditorRequestKind kind : {EditorRequestKind::NewProject, EditorRequestKind::OpenProject,
	                                     EditorRequestKind::CloseProject, EditorRequestKind::Quit})
		TEST_EXPECT(request_kind_row(kind).on_busy == OnBusy::CancelRunning && request_kind_row(kind).needs == kHoldsAll);
	TEST_EXPECT(request_kind_row(EditorRequestKind::Build).on_busy == OnBusy::Join &&
	            request_kind_row(EditorRequestKind::Play).on_busy == OnBusy::Join);
	for (const EditorRequestKind kind : {EditorRequestKind::PreviewImport, EditorRequestKind::PlanImport,
	                                     EditorRequestKind::PreviewRetailImport, EditorRequestKind::SetImportDependencies})
		TEST_EXPECT(request_kind_row(kind).on_busy == OnBusy::Supersede);
	OperationStatus build;
	build.id = 3;
	build.kind = OperationKind::Build;
	build.holds = operation_kind_row(OperationKind::Build).holds;
	TEST_EXPECT(build.holds == HoldsFiles);
	for (const EditorRequestKind kind : {EditorRequestKind::Save, EditorRequestKind::SaveAll, EditorRequestKind::CreateFile,
	                                     EditorRequestKind::RenameAsset, EditorRequestKind::ImportFiles,
	                                     EditorRequestKind::Rescan, EditorRequestKind::CreateMissing})
		TEST_EXPECT(busy_refuses(kind, build));
	for (const EditorRequestKind kind : {EditorRequestKind::EditRecord, EditorRequestKind::Undo, EditorRequestKind::OpenDocument,
	                                     EditorRequestKind::SelectRecord, EditorRequestKind::Build, EditorRequestKind::Play,
	                                     EditorRequestKind::CloseProject, EditorRequestKind::StopPlay})
		TEST_EXPECT(!busy_refuses(kind, build));
	TEST_EXPECT(!busy_refuses(EditorRequestKind::Save, OperationStatus()));
	return 0;
}

// The busy gate over a real build, stepped slowly: a Save is refused (operation.busy, nothing
// written) and so is the unsaved prompt's Save, an edit goes on; a Build joins the build, a Play
// joins it and starts the game when it lands; an import's plan is refused, the build not being
// what it supersedes; a project's close cancels the build first.
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
	TEST_EXPECT(session.outcome().done() && v.operation.running() && v.operation.holds == HoldsFiles);
	const uint64_t build = v.operation.id;
	TEST_EXPECT(session.outcome().operation == build);
	session.poll();

	// An edit goes on while the build packs; its Save does not.
	session.handle(make_request(EditorRequestKind::OpenDocument, "items.def"));
	Document *items = session.document_for("items.def");
	TEST_EXPECT(session.outcome().done() && items != nullptr && !items->rows().empty());
	if (!items || items->rows().empty()) return 1;
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
	// The unsaved prompt's Save writes files too: refused, the prompt kept.
	session.handle(make_request(EditorRequestKind::CloseDocument, items->path()));
	TEST_EXPECT(session.outcome().unsaved_prompt && v.unsaved_prompt.open);
	EditorRequest answer = make_request(EditorRequestKind::ResolveUnsaved);
	answer.unsaved_choice = UnsavedChoice::Save;
	session.handle(answer);
	TEST_EXPECT(session.outcome().unsaved_prompt && v.unsaved_prompt.open && items->dirty() &&
	            session.outcome().findings.size() == 1 && session.outcome().findings[0].code == "operation.busy");
	answer.unsaved_choice = UnsavedChoice::Cancel;
	session.handle(answer);
	TEST_EXPECT(!v.unsaved_prompt.open);

	// Supersede is the import plans' row; a build is not what they supersede: refused.
	EditorRequest preview = make_request(EditorRequestKind::PreviewImport);
	TEST_EXPECT(editor_test::write_text(dir.file("loose.txt"), "loose"));
	preview.paths = {dir.file("loose.txt")};
	session.handle(preview);
	TEST_EXPECT(!session.outcome().done() && session.outcome().findings.size() == 1 &&
	            session.outcome().findings[0].code == "operation.busy" && !v.import_preview.open);

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
	return 0;
}

// A features change reads the files again (the requirements follow the features): while a build
// holds them it is refused inside the settings' Apply, a failure the result carries back, and
// the rest of the change is written.
static int test_features_wait_for_the_build() {
	editor_test::TempProjectDir dir("opennova_editor_operation_features");
	FakePlatform platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Features"));
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	session.set_poll_budget({0, 256});
	session.handle(make_request(EditorRequestKind::Build));
	TEST_EXPECT(v.operation.running());
	EditorRequest apply = make_request(EditorRequestKind::ApplyProjectSettings);
	apply.settings.serial = 9;
	apply.settings.title = std::string("Renamed");
	apply.settings.mission = true;
	session.handle(apply);
	TEST_EXPECT(v.settings_result.serial == 9 && v.settings_result.failures.size() == 1 &&
	            v.settings_result.failures[0].code == "operation.busy");
	TEST_EXPECT(v.document.title == "Renamed" && !v.document.features.mission && v.operation.running());
	session.run_operations();
	session.handle(apply);
	TEST_EXPECT(v.settings_result.failures.empty() && v.document.features.mission);
	return 0;
}

int main() {
	int failures = 0;
	failures += test_slot_steps_and_finishes();
	failures += test_slot_cancels_between_steps();
	failures += test_request_table();
	failures += test_busy_gate();
	failures += test_features_wait_for_the_build();
	if (failures == 0) std::printf("editor_session_operation: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
