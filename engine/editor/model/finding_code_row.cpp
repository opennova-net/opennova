#include "finding_code_row.h"

#include <iterator>

namespace opennova::editor {

namespace {

using C = CoreFinding;
using F = FindingFix;
using G = FindingGroup;
using P = FindingPlace;

// A code of the group, with the fixes Problems offers for it.
constexpr FindingCodeRow code(const char *token, FindingGroup group, FindingFix fixes = F::None) {
	FindingCodeRow row;
	row.token = token;
	row.group = group;
	row.fixes = fixes;
	return row;
}

// A code about a file as a whole (its name, its place): Problems shows it in Files, where it is
// renamed when it has the fix.
constexpr FindingCodeRow about_the_file(const char *token, FindingGroup group, FindingFix fixes) {
	FindingCodeRow row = code(token, group, fixes);
	row.place = P::File;
	return row;
}

// A code saying its file does not serialize: its Save is refused, no Rewrite offered for it.
constexpr FindingCodeRow blocking(FindingCodeRow row) {
	row.blocks_save = true;
	return row;
}

// A code the asset graph makes.
constexpr FindingCodeRow from_graph(FindingCodeRow row) {
	row.source = FindingSource::Graph;
	return row;
}

constexpr FindingCodeEntry<CoreFinding> kEntries[] = {
	{ C::AssetKindUnknown, code("asset.kind.unknown", G::ProjectFiles) },
	{ C::AssetNameDuplicate, about_the_file("asset.name.duplicate", G::ProjectFiles, F::Rename) },
	{ C::AssetNameEmpty, about_the_file("asset.name.empty", G::ProjectFiles, F::None) },
	{ C::AssetNameTooLong, about_the_file("asset.name.too_long", G::ProjectFiles, F::Rename) },
	{ C::AssetUnreadable, code("asset.unreadable", G::ProjectFiles) },
	{ C::BlankDef, code("blank.def", G::NewFiles) },
	{ C::BlankFont, code("blank.font", G::NewFiles) },
	{ C::BlankMenu, code("blank.menu", G::NewFiles) },
	{ C::BlankStrings, code("blank.strings", G::NewFiles) },
	{ C::BlankStyle, code("blank.style", G::NewFiles) },
	{ C::BlankTexture, code("blank.texture", G::NewFiles) },
	{ C::BlankUnavailable, code("blank.unavailable", G::NewFiles) },
	{ C::BuildArchive, code("build.archive", G::Build) },
	{ C::BuildArchiveInProject, about_the_file("build.archive_in_project", G::Build, F::None) },
	{ C::BuildBlocked, code("build.blocked", G::Build) },
	{ C::BuildChanged, code("build.changed", G::Build) },
	{ C::BuildCopy, code("build.copy", G::Build) },
	{ C::BuildNameUnstorable, about_the_file("build.name_unstorable", G::Build, F::Rename) },
	{ C::BuildRead, code("build.read", G::Build) },
	{ C::BuildVerify, code("build.verify", G::Build) },
	{ C::BuildWrite, code("build.write", G::Build) },
	{ C::CreateMissingExists, code("create_missing.exists", G::CreateMissing) },
	{ C::CreateMissingUnknown, code("create_missing.unknown", G::CreateMissing) },
	{ C::CreateMissingWrite, code("create_missing.write", G::CreateMissing) },
	{ C::CreateMissingWrongKind, code("create_missing.wrong_kind", G::CreateMissing) },
	{ C::DocumentBatch, code("document.batch", G::Documents) },
	{ C::DocumentCollection, code("document.collection", G::Documents) },
	{ C::DocumentConflict, code("document.conflict", G::Documents, F::Reload) },
	{ C::DocumentCopy, code("document.copy", G::Documents) },
	{ C::DocumentDecode, code("document.decode", G::Documents) },
	{ C::DocumentDuplicate, code("document.duplicate", G::Documents) },
	{ C::DocumentKind, code("document.kind", G::Documents) },
	{ C::DocumentMissing, code("document.missing", G::Documents) },
	{ C::DocumentName, code("document.name", G::Documents) },
	{ C::DocumentNoFile, code("document.no_file", G::Documents) },
	{ C::DocumentNoRecords, code("document.no_records", G::Documents) },
	{ C::DocumentNotOpen, code("document.not_open", G::Documents) },
	{ C::DocumentParse, code("document.parse", G::Documents) },
	{ C::DocumentPaste, code("document.paste", G::Documents) },
	{ C::DocumentPath, code("document.path", G::Documents) },
	{ C::DocumentPayload, code("document.payload", G::Documents) },
	{ C::DocumentRead, code("document.read", G::Documents) },
	{ C::DocumentRevertNothing, code("document.revert_nothing", G::Documents) },
	{ C::DocumentSelection, code("document.selection", G::Documents) },
	{ C::DocumentSnapshot, code("document.snapshot", G::Documents) },
	{ C::DocumentSpan, code("document.span", G::Documents) },
	{ C::DocumentStale, code("document.stale", G::Documents) },
	{ C::DocumentStructure, code("document.structure", G::Documents) },
	{ C::DocumentUnserializable, blocking(code("document.unserializable", G::Documents)) },
	{ C::DocumentValue, code("document.value", G::Documents) },
	{ C::DocumentWrite, code("document.write", G::Documents) },
	{ C::EditorSettingsJson, code("editor_settings.json", G::EditorSettings) },
	{ C::EditorSettingsSchemaVersionUnsupported, code("editor_settings.schema_version.unsupported", G::EditorSettings) },
	{ C::EditorSettingsUnreadable, code("editor_settings.unreadable", G::EditorSettings) },
	{ C::EditorSettingsWrite, code("editor_settings.write", G::EditorSettings) },
	{ C::GraphUnreadable, from_graph(code("graph.unreadable", G::FilesNotChecked)) },
	{ C::ImportAlphaDropped, code("import.alpha_dropped", G::Imports) },
	{ C::ImportArchive, code("import.archive", G::Imports) },
	{ C::ImportChanged, code("import.changed", G::Imports) },
	{ C::ImportDecode, code("import.decode", G::Imports) },
	{ C::ImportDuplicate, code("import.duplicate", G::Imports) },
	{ C::ImportEncode, code("import.encode", G::Imports) },
	{ C::ImportExists, code("import.exists", G::Imports) },
	{ C::ImportFolder, code("import.folder", G::Imports) },
	{ C::ImportInstall, code("import.install", G::Imports) },
	{ C::ImportKind, code("import.kind", G::Imports) },
	{ C::ImportName, code("import.name", G::Imports) },
	{ C::ImportNotPlanned, code("import.not_planned", G::Imports) },
	{ C::ImportNotPublished, code("import.not_published", G::Imports) },
	{ C::ImportOption, code("import.option", G::Imports) },
	{ C::ImportOrphanRecord, code("import.orphan_record", G::Imports) },
	{ C::ImportOutputMissing, code("import.output_missing", G::Imports, F::Reimport) },
	{ C::ImportPath, code("import.path", G::Imports) },
	{ C::ImportPublish, code("import.publish", G::Imports) },
	{ C::ImportRead, code("import.read", G::Imports) },
	{ C::ImportRecord, code("import.record", G::Imports) },
	{ C::ImportScene, code("import.scene", G::Imports) },
	{ C::ImportSceneNote, code("import.scene_note", G::Imports) },
	{ C::ImportSidecar, code("import.sidecar", G::Imports) },
	{ C::ImportSource, code("import.source", G::Imports) },
	{ C::ImportTextureNotImported, code("import.texture_not_imported", G::Imports, F::UnimportedTexture) },
	{ C::ImportUnreadable, code("import.unreadable", G::Imports) },
	{ C::ImportWrite, code("import.write", G::Imports) },
	{ C::LocalSettingsJson, code("local_settings.json", G::LocalSettings) },
	{ C::LocalSettingsSchemaVersionUnsupported, code("local_settings.schema_version.unsupported", G::LocalSettings) },
	{ C::LocalSettingsUnreadable, code("local_settings.unreadable", G::LocalSettings) },
	{ C::LocalSettingsWrite, code("local_settings.write", G::LocalSettings) },
	{ C::OperationBusy, code("operation.busy", G::Operations) },
	{ C::OperationNone, code("operation.none", G::Operations) },
	{ C::OperationNotCancellable, code("operation.not_cancellable", G::Operations) },
	{ C::PlayAlreadyRunning, code("play.already_running", G::Play) },
	{ C::PlayBootMissing, code("play.boot_missing", G::Play, F::Requirement) },
	{ C::PlayCrashed, code("play.crashed", G::Play) },
	{ C::PlayInstallCopy, code("play.install_copy", G::Play) },
	{ C::PlayInstallMissing, code("play.install_missing", G::Play) },
	{ C::PlayRuntimeMissing, code("play.runtime_missing", G::Play) },
	{ C::PlaySpawn, code("play.spawn", G::Play) },
	{ C::PlayUnsupported, code("play.unsupported", G::Play) },
	{ C::ProjectExists, code("project.exists", G::Project) },
	{ C::ProjectFieldInvalid, code("project.field.invalid", G::Project) },
	{ C::ProjectFileMissing, code("project.file.missing", G::Project) },
	{ C::ProjectFileUnreadable, code("project.file.unreadable", G::Project) },
	{ C::ProjectJson, code("project.json", G::Project) },
	{ C::ProjectNone, code("project.none", G::Project) },
	{ C::ProjectRootUnreadable, code("project.root.unreadable", G::Project) },
	{ C::ProjectSchemaVersionUnsupported, code("project.schema_version.unsupported", G::Project) },
	{ C::ProjectTargetGameUnknown, code("project.target_game.unknown", G::Project) },
	{ C::ProjectTitleEmpty, code("project.title_empty", G::Project) },
	{ C::ProjectWrite, code("project.write", G::Project) },
	{ C::ReferenceMissing, from_graph(code("reference.missing", G::MissingReferences, F::Reference)) },
	{ C::RenameConflict, code("rename.conflict", G::Renames) },
	{ C::RenameCopy, code("rename.copy", G::Renames) },
	{ C::RenameExists, code("rename.exists", G::Renames) },
	{ C::RenameImported, code("rename.imported", G::Renames) },
	{ C::RenameKind, code("rename.kind", G::Renames) },
	{ C::RenameMove, code("rename.move", G::Renames) },
	{ C::RenameName, code("rename.name", G::Renames) },
	{ C::RenamePartial, code("rename.partial", G::Renames) },
	{ C::RenamePath, code("rename.path", G::Renames) },
	{ C::RenameRemove, code("rename.remove", G::Renames) },
	{ C::RenameSite, code("rename.site", G::Renames) },
	{ C::RenameStyle, code("rename.style", G::Renames) },
	{ C::RenameTooLong, code("rename.too_long", G::Renames) },
	{ C::RenameUnchanged, code("rename.unchanged", G::Renames) },
	{ C::RenameUnknownFile, code("rename.unknown_file", G::Renames) },
	{ C::RenameUnknownSymbol, code("rename.unknown_symbol", G::Renames) },
	{ C::RenameWrite, code("rename.write", G::Renames) },
	{ C::RequirementAssigned, code("requirement.assigned", G::RequiredFiles) },
	{ C::RequirementKind, code("requirement.kind", G::RequiredFiles) },
	{ C::RequirementMissing, code("requirement.missing", G::RequiredFiles, F::Requirement) },
	{ C::RequirementOptionalMissing, code("requirement.optional_missing", G::OptionalFiles, F::Requirement) },
	{ C::RequirementUnknown, code("requirement.unknown", G::RequiredFiles) },
	{ C::RequirementUnknownFile, code("requirement.unknown_file", G::RequiredFiles) },
	{ C::RequirementWrongKind, code("requirement.wrong_kind", G::RequiredFiles, F::WrongKind) },
	{ C::UnsavedDiscard, code("unsaved.discard", G::UnsavedChanges) },
	{ C::UnsavedNone, code("unsaved.none", G::UnsavedChanges) },
};

static_assert(std::size(kEntries) == kCoreFindingCount, "every CoreFinding has exactly one row");
static_assert(finding_entries_well_formed(kEntries),
		"the core rows follow CoreFinding's order, each token its own, a Rewrite's words on a Rewrite row");

constexpr std::array<FindingCodeRow, kCoreFindingCount> kRows = finding_rows(kEntries);
static_assert(finding_rows_well_formed(kRows), "every core row has its group");

} // namespace

const FindingCodeRow &finding_code(CoreFinding code) {
	return kRows[static_cast<size_t>(code)];
}

FindingTable core_finding_codes() { return { kRows.data(), kRows.size() }; }

} // namespace opennova::editor
