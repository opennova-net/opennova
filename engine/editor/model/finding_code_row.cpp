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

// A code whose findings are listed and never refuse a build, whatever their severity.
constexpr FindingCodeRow listed(FindingCodeRow row) {
	row.gates_build = false;
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
	{ C::BlankMission, code("blank.mission", G::NewFiles) },
	{ C::BlankStrings, code("blank.strings", G::NewFiles) },
	{ C::BlankStyle, code("blank.style", G::NewFiles) },
	{ C::BlankTexture, code("blank.texture", G::NewFiles) },
	{ C::BlankUnavailable, code("blank.unavailable", G::NewFiles) },
	{ C::BuildArchive, code("build.archive", G::Build) },
	{ C::BuildArchiveInProject, about_the_file("build.archive_in_project", G::Build, F::None) },
	{ C::BuildBlocked, code("build.blocked", G::Build) },
	{ C::BuildChanged, code("build.changed", G::Build) },
	{ C::BuildCopy, code("build.copy", G::Build) },
	// An expansion whose base game does not mount (no install, none of its archives): its build cannot
	// be compared with the base nor gated over it (ADR 0046 S16), so it gates, the editor's integrity.
	{ C::BuildExpansionBaseMissing, code("build.expansion.base_missing", G::Build) },
	// The Mods list's description of an expansion past its record's 272 bytes: the copy spills into the
	// next expansion's record [orig: Expansion_ScanAndRegister @ 0x4a4612], a picture the game shows
	// wrong, no refusal of it: listed.
	{ C::BuildExpansionExpDesc, listed(code("build.expansion.exp_desc", G::Build)) },
	// The Mods list's name of an expansion of 64 bytes or more: the copy runs into the record's folder
	// name [orig: Expansion_ScanAndRegister @ 0x4a4598, after the folder's @ 0x4a4532], so choosing the
	// expansion there loads another name's, the base game [orig: Options_HandleAcceptOrBack @ 0x55ad43;
	// Expansion_LoadAssets @ 0x4a4767]: it gates.
	{ C::BuildExpansionExpName, code("build.expansion.exp_name", G::Build) },
	// A mission (or map project) an expansion ships whose name the base game lists too: the mission list
	// lists it twice, the base pair's walk and the expansion pair's, with no dedupe [orig:
	// MissionList_ScanAndBuildFromFiles @ 0x563170, @ 0x5635a5..0x5635bb], both rows loading the
	// expansion's copy; a picture, no refusal: listed.
	{ C::BuildExpansionMissionTwice, listed(about_the_file("build.expansion.mission_twice", G::Build, F::None)) },
	// A mission (or map project) an expansion ships with no text table of its own in its pair: its row is
	// untitled, the list titling a mission only from the text archive paired with its own [orig:
	// Mission_BuildMapListFromPFF @ 0x562c2d, PFF_FileExists(bin, textArchive)]; the game runs it: listed.
	{ C::BuildExpansionMissionUntitled, listed(about_the_file("build.expansion.mission_untitled", G::Build, F::None)) },
	// A file an expansion's build leaves out because the game reads its kind only from the install's
	// folder (ADR 0046 S16, AssetKindRow::expansion_loose): said, refusing nothing.
	{ C::BuildExpansionRootOnly, listed(about_the_file("build.expansion.root_only", G::Build, F::None)) },
	{ C::BuildNameUnstorable, about_the_file("build.name_unstorable", G::Build, F::Rename) },
	{ C::BuildOutDirInProject, code("build.out_dir_in_project", G::Build) },
	// A player's or this machine's file the project holds, which a build leaves out (ADR 0046 S14,
	// assets/player_files.h).
	{ C::BuildPlayerFile, code("build.player_file", G::Build) },
	{ C::BuildRead, code("build.read", G::Build) },
	// A file the game could never read as the build would ship it (a NovaWorld screen, read through the
	// archives alone, under a name no archive can store): the build leaves it out, refusing nothing (S16).
	{ C::BuildUnread, listed(about_the_file("build.unread", G::Build, F::None)) },
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
	{ C::DocumentValues, code("document.values", G::Documents) },
	{ C::DocumentWrite, code("document.write", G::Documents) },
	{ C::EditorSettingsJson, code("editor_settings.json", G::EditorSettings) },
	{ C::EditorSettingsSchemaVersionUnsupported, code("editor_settings.schema_version.unsupported", G::EditorSettings) },
	{ C::EditorSettingsUnreadable, code("editor_settings.unreadable", G::EditorSettings) },
	{ C::EditorSettingsWrite, code("editor_settings.write", G::EditorSettings) },
	// A project file the game never reads for the project's expansion setting (ADR 0046 S16): a music
	// bank but the streamed pair, an expansion's base music scripts. Listed, its fix a Rename: the
	// game runs without it.
	{ C::ExpansionFileUnread, listed(about_the_file("expansion.file.unread", G::Expansion, F::Rename)) },
	// Export (ADR 0046 S16, project_build/export_build.h): a folder that is the person's, never written
	// over; the runtime to ship that is not there; a copy or a rename refused.
	// An Export cancelled before its folder was replaced: the folder is as it was (S16).
	{ C::ExportCancelled, listed(code("export.cancelled", G::Export)) },
	// The last export, set aside while the new one went in, that could not be removed (a file of it open
	// elsewhere): the new export is in; the next export removes it first (S16). Listed.
	{ C::ExportCleanup, listed(code("export.cleanup", G::Export)) },
	{ C::ExportFolder, code("export.folder", G::Export) },
	// What an export over an earlier one of the project did to it (S16): the files the person added there,
	// kept in the new export, and the earlier export's files it no longer holds, removed. Listed.
	{ C::ExportReplaced, listed(code("export.replaced", G::Export)) },
	{ C::ExportRuntime, code("export.runtime", G::Export) },
	{ C::ExportWrite, code("export.write", G::Export) },
	{ C::GraphUnreadable, from_graph(code("graph.unreadable", G::FilesNotChecked)) },
	{ C::ImportAlphaDropped, code("import.alpha_dropped", G::Imports) },
	{ C::ImportArchive, code("import.archive", G::Imports) },
	{ C::ImportChanged, code("import.changed", G::Imports) },
	{ C::ImportDecode, code("import.decode", G::Imports) },
	{ C::ImportDuplicate, code("import.duplicate", G::Imports) },
	{ C::ImportEncode, code("import.encode", G::Imports) },
	{ C::ImportExists, code("import.exists", G::Imports) },
	{ C::ImportFolder, code("import.folder", G::Imports) },
	{ C::ImportInput, code("import.input", G::Imports) },
	{ C::ImportInstall, code("import.install", G::Imports) },
	{ C::ImportKind, code("import.kind", G::Imports) },
	{ C::ImportName, code("import.name", G::Imports) },
	{ C::ImportNotPlanned, code("import.not_planned", G::Imports) },
	{ C::ImportNotPublished, code("import.not_published", G::Imports) },
	{ C::ImportOption, code("import.option", G::Imports) },
	{ C::ImportOrphanRecord, code("import.orphan_record", G::Imports) },
	{ C::ImportOutputMissing, code("import.output_missing", G::Imports, F::Reimport) },
	{ C::ImportPath, code("import.path", G::Imports) },
	// A player's or this machine's file, which an import never takes (ADR 0046 S14).
	{ C::ImportPlayerFile, code("import.player_file", G::Imports) },
	{ C::ImportPublish, code("import.publish", G::Imports) },
	{ C::ImportRead, code("import.read", G::Imports) },
	{ C::ImportRecord, code("import.record", G::Imports) },
	// An import request whose fields ask for two things at once (every file and some by name; every
	// file and a walk; an import of nothing named and nothing planned): refused, never half-served.
	{ C::ImportRequest, code("import.request", G::Imports) },
	{ C::ImportScene, code("import.scene", G::Imports) },
	{ C::ImportSceneNote, code("import.scene_note", G::Imports) },
	{ C::ImportSidecar, code("import.sidecar", G::Imports) },
	{ C::ImportNotFound, code("import.not_found", G::Imports) },
	{ C::ImportTextureNotImported, code("import.texture_not_imported", G::Imports, F::UnimportedTexture) },
	{ C::ImportUnreadable, code("import.unreadable", G::Imports) },
	{ C::ImportWrite, code("import.write", G::Imports) },
	{ C::LocalSettingsJson, code("local_settings.json", G::LocalSettings) },
	{ C::LocalSettingsSchemaVersionUnsupported, code("local_settings.schema_version.unsupported", G::LocalSettings) },
	{ C::LocalSettingsUnreadable, code("local_settings.unreadable", G::LocalSettings) },
	{ C::LocalSettingsWrite, code("local_settings.write", G::LocalSettings) },
	{ C::MissionSidecarUnused, code("mission.sidecar.unused", G::Missions) },
	{ C::OperationBusy, code("operation.busy", G::Operations) },
	{ C::OperationNone, code("operation.none", G::Operations) },
	{ C::OperationNotCancellable, code("operation.not_cancellable", G::Operations) },
	{ C::PlayAlreadyRunning, code("play.already_running", G::Play) },
	{ C::PlayBootMissing, code("play.boot_missing", G::Play, F::Requirement) },
	{ C::PlayCrashed, code("play.crashed", G::Play) },
	{ C::PlayInstallCopy, code("play.install_copy", G::Play) },
	{ C::PlayInstallMissing, code("play.install_missing", G::Play) },
	{ C::PlayMissionFailed, code("play.mission.failed", G::Play) },
	{ C::PlayMissionUnknown, code("play.mission.unknown", G::Play) },
	{ C::PlayRunDirectory, code("play.run_directory", G::Play) },
	{ C::PlayRuntimeMissing, code("play.runtime_missing", G::Play) },
	{ C::PlaySpawn, code("play.spawn", G::Play) },
	{ C::PlayUnsupported, code("play.unsupported", G::Play) },
	{ C::ProjectExists, code("project.exists", G::Project) },
	// The project's expansion against its game install (ADR 0046 S16, expansion_name.h): a name the
	// install has already, an expansion to build on it lacks. Refused where a project is made or its
	// settings applied; listed where an open project's install is read again (the install is this
	// machine's, the project the modder's), refusing no build: the game mounts the build's own
	// expansion whatever else the install holds.
	{ C::ProjectExpansionNameTaken, listed(code("project.expansion.name_taken", G::Project)) },
	{ C::ProjectExpansionNotInstalled, listed(code("project.expansion.not_installed", G::Project)) },
	// An expansion for a game other than Joint Operations, whose expansions alone are witnessed.
	{ C::ProjectExpansionUnsupported, code("project.expansion.unsupported", G::Project) },
	{ C::ProjectFieldInvalid, code("project.field.invalid", G::Project) },
	{ C::ProjectFileMissing, code("project.file.missing", G::Project) },
	{ C::ProjectFileUnreadable, code("project.file.unreadable", G::Project) },
	{ C::ProjectJson, code("project.json", G::Project) },
	{ C::ProjectMissionFeatureOff, code("project.mission.feature_off", G::Project) },
	{ C::ProjectNone, code("project.none", G::Project) },
	{ C::ProjectRootUnreadable, code("project.root.unreadable", G::Project) },
	{ C::ProjectSchemaVersionUnsupported, code("project.schema_version.unsupported", G::Project) },
	{ C::ProjectTargetGameUnknown, code("project.target_game.unknown", G::Project) },
	{ C::ProjectTitleEmpty, code("project.title_empty", G::Project) },
	{ C::ProjectWrite, code("project.write", G::Project) },
	// A name the project lacks is shown, counted and fixable, and gates no build (ADR 0046 S14): the
	// shipped game's own files name what its install does not hold and it runs, so a build refused
	// for one would assert a failure no one has witnessed; where the reference's kind cites the
	// game's refusal (a mission's terrain: ReferenceKindRow::gates_when_missing) it gates.
	{ C::ReferenceMissing, listed(from_graph(code("reference.missing", G::MissingReferences, F::Reference))) },
	// A file of the name the project holds, of a kind the reference's loader does not load: the
	// loader finds nothing it loads there, as for a missing name, so it gates where a missing
	// reference of its kind does and is listed elsewhere (the audit of the gate: no refusal of a
	// file of the wrong kind is witnessed beyond the kind's own).
	{ C::ReferenceWrongKind, listed(from_graph(code("reference.wrong_kind", G::MissingReferences))) },
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
	// A required file the project lacks: listed, and gating where its manifest row is the game's
	// refusal to boot (RES_FATAL: the string tables the boot exits without, the main menu it dead-ends
	// without; blocks_build reads the row); the game boots on without any other, degraded as the
	// row's failure says.
	{ C::RequirementMissing, listed(code("requirement.missing", G::RequiredFiles, F::Requirement)) },
	{ C::RequirementOptionalMissing, code("requirement.optional_missing", G::OptionalFiles, F::Requirement) },
	{ C::RequirementUnknown, code("requirement.unknown", G::RequiredFiles) },
	{ C::RequirementUnknownFile, code("requirement.unknown_file", G::RequiredFiles) },
	// A required name holding a file of another kind: the boot's loaders read what the file holds
	// with no check of its kind (a string table's keeps any bytes it opens [orig:
	// TextResource_LoadFromArchive @ 0x75d0b0] and makes its header's offsets pointers unchecked
	// [orig: TextResource_FixupPointers @ 0x75d050]): listed, gating where the row is the boot's
	// refusal (RES_FATAL), whose table the boot then runs on wild pointers.
	{ C::RequirementWrongKind, listed(code("requirement.wrong_kind", G::RequiredFiles, F::WrongKind)) },
	{ C::UnsavedDiscard, code("unsaved.discard", G::UnsavedChanges) },
	{ C::UnsavedNone, code("unsaved.none", G::UnsavedChanges) },
	{ C::ViewportRefused, code("viewport.refused", G::Viewports) },
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
