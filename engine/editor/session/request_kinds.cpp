#include <editor/session/request_kinds.h>

#include <cstddef>
#include <iterator>

namespace opennova::editor {

namespace {

using K = EditorRequestKind;

constexpr Holds kNone = HoldsNothing;
constexpr Holds kFiles = HoldsFiles;
constexpr Holds kDocuments = HoldsDocuments;
constexpr Holds kFilesAndDocuments = HoldsFiles | HoldsDocuments;

// What each request reads and writes, what it asks of the busy gate, and what the unsaved-changes
// prompt guards of it and whether the prompt offers Discard. What it reads and writes: the files
// when it reads or writes them on disk (the import pass writes them too), the open documents when
// it reads them or changes what they hold or which are open, everything when it switches or closes
// the project, and the slot when it starts an operation. A running build reads the files and
// writes only the slot, so an edit, an open, a selection or an import's preview goes on beside it,
// and a save, a create, an import or a rename waits.
constexpr RequestKindRow kRows[] = {
	{K::NewProject, kNone, kHoldsAll, OnBusy::CancelRunning, GuardScope::AllDirty, true},
	{K::OpenProject, kNone, kHoldsAll, OnBusy::CancelRunning, GuardScope::AllDirty, true},
	{K::CloseProject, kNone, kHoldsAll, OnBusy::CancelRunning, GuardScope::AllDirty, true},
	{K::ForgetRecent, kNone, kNone, OnBusy::Refuse, GuardScope::None, false},
	{K::Rescan, kFiles, kFilesAndDocuments, OnBusy::Refuse, GuardScope::None, false},
	// The settings dialog waits on its result, so a settings change is never refused whole: each of
	// its parts is weighed against the running operation inside, a refused part a failure its result
	// carries back (SessionCore::apply_project_settings).
	{K::ApplyProjectSettings, kNone, kNone, OnBusy::Refuse, GuardScope::None, false},
	// An import's preview plans in place today. When S13 A3 makes the plan an operation
	// (ImportPlan), these rows (and PreviewRetailImport's) start one: they must write the slot then,
	// as Build and Play do, so none of them runs beside another operation.
	{K::PreviewImport, kFiles, kNone, OnBusy::Supersede, GuardScope::None, false},
	{K::PlanImport, kFiles, kNone, OnBusy::Supersede, GuardScope::None, false},
	{K::SetImportDependencies, kFiles, kNone, OnBusy::Supersede, GuardScope::None, false},
	{K::ImportFiles, kFilesAndDocuments, kFilesAndDocuments, OnBusy::Refuse,
	 GuardScope::PlannedWrites, false},
	{K::CancelImport, kNone, kNone, OnBusy::Refuse, GuardScope::None, false},
	{K::CreateMissing, kFiles, kFiles, OnBusy::Refuse, GuardScope::None, false},
	// Before its operation starts, a Build (a Play's too) reads again the documents whose files
	// changed and refreshes the project (SessionCore::start_build: the import pass, the scan): it
	// writes the files and the documents, and the slot. A running build still serves it (its row's
	// joined_by).
	{K::Build, kFilesAndDocuments, kFilesAndDocuments | HoldsSlot, OnBusy::Join,
	 GuardScope::AllDirty, false},
	{K::Play, kFilesAndDocuments, kFilesAndDocuments | HoldsSlot, OnBusy::Join,
	 GuardScope::AllDirty, false},
	{K::StopPlay, kNone, kNone, OnBusy::Refuse, GuardScope::None, false},
	{K::CancelOperation, kNone, kNone, OnBusy::Refuse, GuardScope::None, false},
	{K::CreateFile, kFiles, kFilesAndDocuments, OnBusy::Refuse, GuardScope::None, false},
	{K::OpenDocument, kFiles, kDocuments, OnBusy::Refuse, GuardScope::None, false},
	{K::ShowInFiles, kNone, kNone, OnBusy::Refuse, GuardScope::None, false},
	{K::ReloadDocument, kFiles, kDocuments, OnBusy::Refuse, GuardScope::Document, true},
	{K::CloseDocument, kNone, kDocuments, OnBusy::Refuse, GuardScope::Document, true},
	{K::SelectRecord, kNone, kNone, OnBusy::Refuse, GuardScope::None, false},
	// A fix's edit opens its document first.
	{K::EditRecord, kFiles, kDocuments, OnBusy::Refuse, GuardScope::None, false},
	{K::RevertToSaved, kNone, kDocuments, OnBusy::Refuse, GuardScope::None, false},
	{K::EndEdit, kNone, kNone, OnBusy::Refuse, GuardScope::None, false},
	{K::Copy, kDocuments, kNone, OnBusy::Refuse, GuardScope::None, false},
	{K::Cut, kNone, kDocuments, OnBusy::Refuse, GuardScope::None, false},
	{K::Paste, kNone, kDocuments, OnBusy::Refuse, GuardScope::None, false},
	{K::Duplicate, kNone, kDocuments, OnBusy::Refuse, GuardScope::None, false},
	{K::Save, kFilesAndDocuments, kFilesAndDocuments, OnBusy::Refuse, GuardScope::None, false},
	{K::SaveAll, kFilesAndDocuments, kFilesAndDocuments, OnBusy::Refuse, GuardScope::None, false},
	{K::Undo, kNone, kDocuments, OnBusy::Refuse, GuardScope::None, false},
	{K::Redo, kNone, kDocuments, OnBusy::Refuse, GuardScope::None, false},
	// Its Save is weighed as a Save All and its Discard as a write of the documents, inside; the
	// request it answers meets the gate itself.
	{K::ResolveUnsaved, kNone, kNone, OnBusy::Refuse, GuardScope::None, false},
	{K::RenameAsset, kFilesAndDocuments, kFilesAndDocuments, OnBusy::Refuse,
	 GuardScope::PlannedWrites, false},
	{K::AssignRequirement, kFilesAndDocuments, kFilesAndDocuments, OnBusy::Refuse,
	 GuardScope::PlannedWrites, false},
	{K::PreviewRename, kFiles, kNone, OnBusy::Refuse, GuardScope::None, false},
	{K::RenameSymbol, kFilesAndDocuments, kFilesAndDocuments, OnBusy::Refuse,
	 GuardScope::PlannedWrites, false},
	{K::Reimport, kFiles, kFiles, OnBusy::Refuse, GuardScope::None, false},
	{K::PreviewRetailImport, kFiles, kNone, OnBusy::Supersede, GuardScope::None, false},
	{K::ClearOutput, kNone, kNone, OnBusy::Refuse, GuardScope::None, false},
	{K::Quit, kNone, kHoldsAll, OnBusy::CancelRunning, GuardScope::AllDirty, true},
	{K::PickDirectory, kNone, kNone, OnBusy::Refuse, GuardScope::None, false},
	{K::PickFile, kNone, kNone, OnBusy::Refuse, GuardScope::None, false},
	{K::RevealPath, kNone, kNone, OnBusy::Refuse, GuardScope::None, false},
};

static_assert(std::size(kRows) == kEditorRequestKindCount, "every request kind has exactly one row");

constexpr bool rows_in_order() {
	for (size_t i = 0; i < kEditorRequestKindCount; ++i)
		if (kRows[i].kind != static_cast<EditorRequestKind>(i)) return false;
	return true;
}
static_assert(rows_in_order(), "the request kind rows follow the enum's order");

// The prompt offers Discard only for a request it guards whose plan writes nothing over the files:
// an import's or a rename's Save writes the edits first, as Build's and Play's does.
constexpr bool discard_only_where_nothing_is_written() {
	for (const RequestKindRow &row : kRows)
		if (row.can_discard && (row.guard == GuardScope::None || row.guard == GuardScope::PlannedWrites))
			return false;
	return true;
}
static_assert(discard_only_where_nothing_is_written(),
		"Discard is offered only where the files are not written over");

} // namespace

const RequestKindRow &request_kind_row(EditorRequestKind kind) {
	const size_t index = static_cast<size_t>(kind);
	return kRows[index < kEditorRequestKindCount ? index : static_cast<size_t>(EditorRequestKind::RevealPath)];
}

GateAnswer gate_answer(EditorRequestKind kind, const OperationStatus &running) {
	if (!running.running()) return GateAnswer::Proceed;
	const RequestKindRow &request = request_kind_row(kind);
	const OperationKindRow &operation = operation_kind_row(running.kind);
	if (request.on_busy == OnBusy::Join && operation.joined_by.has(kind)) return GateAnswer::Join;
	// What would take the place of an operation that cannot be cancelled waits for it.
	if (request.on_busy == OnBusy::Supersede && operation.superseded_by.has(kind))
		return running.cancellable ? GateAnswer::Supersede : GateAnswer::Refuse;
	if (!holds_conflict(request.reads, request.writes, running.reads, running.writes)) return GateAnswer::Proceed;
	if (request.on_busy == OnBusy::CancelRunning && running.cancellable) return GateAnswer::CancelRunning;
	return GateAnswer::Refuse;
}

bool busy_refuses(EditorRequestKind kind, const OperationStatus &running) {
	return gate_answer(kind, running) == GateAnswer::Refuse;
}

bool busy_refuses_answer(EditorRequestKind waiting, UnsavedChoice choice, const OperationStatus &running) {
	if (!running.running() || choice == UnsavedChoice::Cancel) return false;
	const GateAnswer answer = gate_answer(waiting, running);
	if (answer == GateAnswer::Refuse) return true;
	if (answer == GateAnswer::CancelRunning) return false; // it cancels the operation first
	const RequestKindRow &save_all = request_kind_row(EditorRequestKind::SaveAll);
	return choice == UnsavedChoice::Save
	               ? holds_conflict(save_all.reads, save_all.writes, running.reads, running.writes)
	               : holds_conflict(HoldsNothing, HoldsDocuments, running.reads, running.writes);
}

} // namespace opennova::editor
