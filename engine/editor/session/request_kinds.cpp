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

// What each request reads and writes: the files when it reads or writes them on disk (the import
// pass writes them too), the open documents when it reads them or changes what they hold or which
// are open, everything when it switches or closes the project, and the slot when it starts an
// operation. A build reads the files and writes only the slot, so an edit, an open, a selection or
// an import's preview goes on beside it, and a save, a create, an import or a rename waits.
constexpr RequestKindRow kRows[] = {
	{K::NewProject, kNone, kHoldsAll, OnBusy::CancelRunning},
	{K::OpenProject, kNone, kHoldsAll, OnBusy::CancelRunning},
	{K::CloseProject, kNone, kHoldsAll, OnBusy::CancelRunning},
	{K::ForgetRecent, kNone, kNone, OnBusy::Refuse},
	{K::Rescan, kFiles, kFilesAndDocuments, OnBusy::Refuse},
	// The settings dialog waits on its result, so a settings change is never refused whole: each of
	// its parts is weighed against the running operation inside, a refused part a failure its result
	// carries back (ProjectSession::apply_project_settings).
	{K::ApplyProjectSettings, kNone, kNone, OnBusy::Refuse},
	{K::PreviewImport, kFiles, kNone, OnBusy::Supersede},
	{K::PlanImport, kFiles, kNone, OnBusy::Supersede},
	{K::SetImportDependencies, kFiles, kNone, OnBusy::Supersede},
	{K::ImportFiles, kFilesAndDocuments, kFilesAndDocuments, OnBusy::Refuse},
	{K::CancelImport, kNone, kNone, OnBusy::Refuse},
	{K::CreateMissing, kFiles, kFiles, OnBusy::Refuse},
	{K::Build, kFiles, HoldsSlot, OnBusy::Join},
	{K::Play, kFiles, HoldsSlot, OnBusy::Join},
	{K::StopPlay, kNone, kNone, OnBusy::Refuse},
	{K::CancelOperation, kNone, kNone, OnBusy::Refuse},
	{K::CreateFile, kFiles, kFilesAndDocuments, OnBusy::Refuse},
	{K::OpenDocument, kFiles, kDocuments, OnBusy::Refuse},
	{K::ShowInFiles, kNone, kNone, OnBusy::Refuse},
	{K::ReloadDocument, kFiles, kDocuments, OnBusy::Refuse},
	{K::CloseDocument, kNone, kDocuments, OnBusy::Refuse},
	{K::SelectRecord, kNone, kNone, OnBusy::Refuse},
	// A fix's edit opens its document first.
	{K::EditRecord, kFiles, kDocuments, OnBusy::Refuse},
	{K::RevertToSaved, kNone, kDocuments, OnBusy::Refuse},
	{K::EndEdit, kNone, kNone, OnBusy::Refuse},
	{K::Copy, kDocuments, kNone, OnBusy::Refuse},
	{K::Cut, kNone, kDocuments, OnBusy::Refuse},
	{K::Paste, kNone, kDocuments, OnBusy::Refuse},
	{K::Duplicate, kNone, kDocuments, OnBusy::Refuse},
	{K::Save, kFilesAndDocuments, kFilesAndDocuments, OnBusy::Refuse},
	{K::SaveAll, kFilesAndDocuments, kFilesAndDocuments, OnBusy::Refuse},
	{K::Undo, kNone, kDocuments, OnBusy::Refuse},
	{K::Redo, kNone, kDocuments, OnBusy::Refuse},
	// Its Save is weighed as a Save All and its Discard as a write of the documents, inside; the
	// request it answers meets the gate itself.
	{K::ResolveUnsaved, kNone, kNone, OnBusy::Refuse},
	{K::RenameAsset, kFilesAndDocuments, kFilesAndDocuments, OnBusy::Refuse},
	{K::AssignRequirement, kFilesAndDocuments, kFilesAndDocuments, OnBusy::Refuse},
	{K::PreviewRename, kFiles, kNone, OnBusy::Refuse},
	{K::RenameSymbol, kFilesAndDocuments, kFilesAndDocuments, OnBusy::Refuse},
	{K::Reimport, kFiles, kFiles, OnBusy::Refuse},
	{K::PreviewRetailImport, kFiles, kNone, OnBusy::Supersede},
	{K::ClearOutput, kNone, kNone, OnBusy::Refuse},
	{K::Quit, kNone, kHoldsAll, OnBusy::CancelRunning},
	{K::PickDirectory, kNone, kNone, OnBusy::Refuse},
	{K::PickFile, kNone, kNone, OnBusy::Refuse},
	{K::RevealPath, kNone, kNone, OnBusy::Refuse},
};

static_assert(std::size(kRows) == kEditorRequestKindCount, "every request kind has exactly one row");

constexpr bool rows_in_order() {
	for (size_t i = 0; i < kEditorRequestKindCount; ++i)
		if (kRows[i].kind != static_cast<EditorRequestKind>(i)) return false;
	return true;
}
static_assert(rows_in_order(), "the request kind rows follow the enum's order");

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

} // namespace opennova::editor
