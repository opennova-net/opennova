#include <editor/session/request_kinds.h>

#include <cstddef>
#include <iterator>

namespace opennova::editor {

namespace {

using K = EditorRequestKind;

constexpr Holds kFilesAndDocuments = HoldsFiles | HoldsDocuments;

// A request needs the files when it writes them or runs the import pass (a build reads them as
// they were when it started), the open documents when it changes what they hold or which are open
// (an import or a rename reads them again), everything when it switches or closes the project;
// reading alone needs nothing. A build holds the files alone, so an edit, an undo, a selection or
// a copy goes on while it packs.
constexpr RequestKindRow kRows[] = {
	{K::NewProject, kHoldsAll, OnBusy::CancelRunning},
	{K::OpenProject, kHoldsAll, OnBusy::CancelRunning},
	{K::CloseProject, kHoldsAll, OnBusy::CancelRunning},
	{K::ForgetRecent, HoldsNothing, OnBusy::Refuse},
	{K::Rescan, kFilesAndDocuments, OnBusy::Refuse},
	// The settings dialog waits on its result, so a settings change is never refused whole: what a
	// build must not see (a features change, which reads the files again) is refused inside it, a
	// failure its result carries back (ProjectSession::apply_project_settings).
	{K::ApplyProjectSettings, HoldsProject, OnBusy::Refuse},
	{K::PreviewImport, HoldsFiles, OnBusy::Supersede},
	{K::PlanImport, HoldsFiles, OnBusy::Supersede},
	{K::SetImportDependencies, HoldsFiles, OnBusy::Supersede},
	{K::ImportFiles, kFilesAndDocuments, OnBusy::Refuse},
	{K::CancelImport, HoldsNothing, OnBusy::Refuse},
	{K::CreateMissing, HoldsFiles, OnBusy::Refuse},
	{K::Build, HoldsFiles, OnBusy::Join},
	{K::Play, HoldsFiles, OnBusy::Join},
	{K::StopPlay, HoldsNothing, OnBusy::Refuse},
	{K::CancelOperation, HoldsNothing, OnBusy::Refuse},
	{K::CreateFile, kFilesAndDocuments, OnBusy::Refuse},
	{K::OpenDocument, HoldsDocuments, OnBusy::Refuse},
	{K::ShowInFiles, HoldsNothing, OnBusy::Refuse},
	{K::ReloadDocument, HoldsDocuments, OnBusy::Refuse},
	{K::CloseDocument, HoldsDocuments, OnBusy::Refuse},
	{K::SelectRecord, HoldsNothing, OnBusy::Refuse},
	{K::EditRecord, HoldsDocuments, OnBusy::Refuse},
	{K::RevertToSaved, HoldsDocuments, OnBusy::Refuse},
	{K::EndEdit, HoldsNothing, OnBusy::Refuse},
	{K::Copy, HoldsNothing, OnBusy::Refuse},
	{K::Cut, HoldsDocuments, OnBusy::Refuse},
	{K::Paste, HoldsDocuments, OnBusy::Refuse},
	{K::Duplicate, HoldsDocuments, OnBusy::Refuse},
	{K::Save, kFilesAndDocuments, OnBusy::Refuse},
	{K::SaveAll, kFilesAndDocuments, OnBusy::Refuse},
	{K::Undo, HoldsDocuments, OnBusy::Refuse},
	{K::Redo, HoldsDocuments, OnBusy::Refuse},
	// Its Save is gated as a SaveAll's, and the request it answers goes through the gate itself.
	{K::ResolveUnsaved, HoldsNothing, OnBusy::Refuse},
	{K::RenameAsset, kFilesAndDocuments, OnBusy::Refuse},
	{K::AssignRequirement, kFilesAndDocuments, OnBusy::Refuse},
	{K::PreviewRename, HoldsNothing, OnBusy::Refuse},
	{K::RenameSymbol, kFilesAndDocuments, OnBusy::Refuse},
	{K::Reimport, HoldsFiles, OnBusy::Refuse},
	{K::PreviewRetailImport, HoldsFiles, OnBusy::Supersede},
	{K::ClearOutput, HoldsNothing, OnBusy::Refuse},
	{K::Quit, kHoldsAll, OnBusy::CancelRunning},
	{K::PickDirectory, HoldsNothing, OnBusy::Refuse},
	{K::PickFile, HoldsNothing, OnBusy::Refuse},
	{K::RevealPath, HoldsNothing, OnBusy::Refuse},
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

bool busy_refuses(EditorRequestKind kind, const OperationStatus &running) {
	if (!running.running()) return false;
	const RequestKindRow &row = request_kind_row(kind);
	return holds_any(row.needs, running.holds) && row.on_busy == OnBusy::Refuse;
}

} // namespace opennova::editor
