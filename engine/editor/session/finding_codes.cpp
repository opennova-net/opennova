#include "finding_codes.h"

#include <string_view>
#include <unordered_map>

#include <editor/documents/document_types.h>

namespace opennova::editor {

namespace {

using C = CoreFinding;
using F = FindingFix;
using P = FindingPlace;

// A file's name, as a whole: Problems shows it in Files (renamed there when it has the fix).
constexpr FindingCodeRow about_the_name(const char *token, FindingFix fixes) {
	return { token, fixes, nullptr, false, P::File };
}

constexpr FindingCodeEntry<CoreFinding> kEntries[] = {
	{ C::AssetKindUnknown, { "asset.kind.unknown" } },
	{ C::AssetNameDuplicate, about_the_name("asset.name.duplicate", F::Rename) },
	{ C::AssetNameEmpty, about_the_name("asset.name.empty", F::None) },
	{ C::AssetNameTooLong, about_the_name("asset.name.too_long", F::Rename) },
	{ C::AssetUnreadable, { "asset.unreadable" } },
	{ C::BlankDef, { "blank.def" } },
	{ C::BlankFont, { "blank.font" } },
	{ C::BlankMenu, { "blank.menu" } },
	{ C::BlankStrings, { "blank.strings" } },
	{ C::BlankStyle, { "blank.style" } },
	{ C::BlankTexture, { "blank.texture" } },
	{ C::BlankUnavailable, { "blank.unavailable" } },
	{ C::BuildArchive, { "build.archive" } },
	// An archive among the project's files, which the build does not pack: its place.
	{ C::BuildArchiveInProject, about_the_name("build.archive_in_project", F::None) },
	{ C::BuildBlocked, { "build.blocked" } },
	{ C::BuildChanged, { "build.changed" } },
	{ C::BuildCopy, { "build.copy" } },
	{ C::BuildNameUnstorable, about_the_name("build.name_unstorable", F::Rename) },
	{ C::BuildRead, { "build.read" } },
	{ C::BuildVerify, { "build.verify" } },
	{ C::BuildWrite, { "build.write" } },
	{ C::CreateMissingExists, { "create_missing.exists" } },
	{ C::CreateMissingUnknown, { "create_missing.unknown" } },
	{ C::CreateMissingWrite, { "create_missing.write" } },
	{ C::CreateMissingWrongKind, { "create_missing.wrong_kind" } },
	{ C::DocumentBatch, { "document.batch" } },
	{ C::DocumentCollection, { "document.collection" } },
	{ C::DocumentConflict, { "document.conflict", F::Reload } },
	{ C::DocumentCopy, { "document.copy" } },
	{ C::DocumentDecode, { "document.decode" } },
	{ C::DocumentDuplicate, { "document.duplicate" } },
	{ C::DocumentKind, { "document.kind" } },
	{ C::DocumentMissing, { "document.missing" } },
	{ C::DocumentName, { "document.name" } },
	{ C::DocumentNoFile, { "document.no_file" } },
	{ C::DocumentNoRecords, { "document.no_records" } },
	{ C::DocumentNotOpen, { "document.not_open" } },
	{ C::DocumentParse, { "document.parse" } },
	{ C::DocumentPaste, { "document.paste" } },
	{ C::DocumentPath, { "document.path" } },
	{ C::DocumentPayload, { "document.payload" } },
	{ C::DocumentRead, { "document.read" } },
	{ C::DocumentRevertNothing, { "document.revert_nothing" } },
	{ C::DocumentSelection, { "document.selection" } },
	{ C::DocumentSnapshot, { "document.snapshot" } },
	{ C::DocumentStale, { "document.stale" } },
	{ C::DocumentStructure, { "document.structure" } },
	// A Save refused: the document holds what it cannot write.
	{ C::DocumentUnserializable, { "document.unserializable", F::None, nullptr, true } },
	{ C::DocumentValue, { "document.value" } },
	{ C::DocumentWrite, { "document.write" } },
	{ C::EditorSettingsJson, { "editor_settings.json" } },
	{ C::EditorSettingsSchemaVersionUnsupported, { "editor_settings.schema_version.unsupported" } },
	{ C::EditorSettingsUnreadable, { "editor_settings.unreadable" } },
	{ C::EditorSettingsWrite, { "editor_settings.write" } },
	{ C::GraphUnreadable, { "graph.unreadable" } },
	{ C::ImportAlphaDropped, { "import.alpha_dropped" } },
	{ C::ImportArchive, { "import.archive" } },
	{ C::ImportChanged, { "import.changed" } },
	{ C::ImportDecode, { "import.decode" } },
	{ C::ImportDuplicate, { "import.duplicate" } },
	{ C::ImportEncode, { "import.encode" } },
	{ C::ImportExists, { "import.exists" } },
	{ C::ImportFolder, { "import.folder" } },
	{ C::ImportInstall, { "import.install" } },
	{ C::ImportKind, { "import.kind" } },
	{ C::ImportName, { "import.name" } },
	{ C::ImportNotPlanned, { "import.not_planned" } },
	{ C::ImportNotPublished, { "import.not_published" } },
	{ C::ImportOption, { "import.option" } },
	{ C::ImportOrphanRecord, { "import.orphan_record" } },
	{ C::ImportOutputMissing, { "import.output_missing", F::Reimport } },
	{ C::ImportPath, { "import.path" } },
	{ C::ImportPublish, { "import.publish" } },
	{ C::ImportRead, { "import.read" } },
	{ C::ImportRecord, { "import.record" } },
	{ C::ImportScene, { "import.scene" } },
	{ C::ImportSceneNote, { "import.scene_note" } },
	{ C::ImportSidecar, { "import.sidecar" } },
	{ C::ImportSource, { "import.source" } },
	{ C::ImportTextureNotImported, { "import.texture_not_imported", F::UnimportedTexture } },
	{ C::ImportUnreadable, { "import.unreadable" } },
	{ C::ImportWrite, { "import.write" } },
	{ C::LocalSettingsJson, { "local_settings.json" } },
	{ C::LocalSettingsSchemaVersionUnsupported, { "local_settings.schema_version.unsupported" } },
	{ C::LocalSettingsUnreadable, { "local_settings.unreadable" } },
	{ C::LocalSettingsWrite, { "local_settings.write" } },
	{ C::OperationBusy, { "operation.busy" } },
	{ C::OperationNone, { "operation.none" } },
	{ C::OperationNotCancellable, { "operation.not_cancellable" } },
	{ C::PlayAlreadyRunning, { "play.already_running" } },
	// A file the game reported missing when it booted: the requirement's fixes, while its row is
	// missing.
	{ C::PlayBootMissing, { "play.boot_missing", F::Requirement } },
	{ C::PlayCrashed, { "play.crashed" } },
	{ C::PlayInstallCopy, { "play.install_copy" } },
	{ C::PlayInstallMissing, { "play.install_missing" } },
	{ C::PlayRuntimeMissing, { "play.runtime_missing" } },
	{ C::PlaySpawn, { "play.spawn" } },
	{ C::PlayUnsupported, { "play.unsupported" } },
	{ C::ProjectExists, { "project.exists" } },
	{ C::ProjectFieldInvalid, { "project.field.invalid" } },
	{ C::ProjectFileMissing, { "project.file.missing" } },
	{ C::ProjectFileUnreadable, { "project.file.unreadable" } },
	{ C::ProjectJson, { "project.json" } },
	{ C::ProjectNone, { "project.none" } },
	{ C::ProjectRootUnreadable, { "project.root.unreadable" } },
	{ C::ProjectSchemaVersionUnsupported, { "project.schema_version.unsupported" } },
	{ C::ProjectTargetGameUnknown, { "project.target_game.unknown" } },
	{ C::ProjectTitleEmpty, { "project.title_empty" } },
	{ C::ProjectWrite, { "project.write" } },
	{ C::ReferenceMissing, { "reference.missing", F::Reference } },
	{ C::RenameConflict, { "rename.conflict" } },
	{ C::RenameCopy, { "rename.copy" } },
	{ C::RenameExists, { "rename.exists" } },
	{ C::RenameImported, { "rename.imported" } },
	{ C::RenameKind, { "rename.kind" } },
	{ C::RenameMove, { "rename.move" } },
	{ C::RenameName, { "rename.name" } },
	{ C::RenamePartial, { "rename.partial" } },
	{ C::RenamePath, { "rename.path" } },
	{ C::RenameRemove, { "rename.remove" } },
	{ C::RenameSite, { "rename.site" } },
	{ C::RenameStyle, { "rename.style" } },
	{ C::RenameTooLong, { "rename.too_long" } },
	{ C::RenameUnchanged, { "rename.unchanged" } },
	{ C::RenameUnknownFile, { "rename.unknown_file" } },
	{ C::RenameUnknownSymbol, { "rename.unknown_symbol" } },
	{ C::RenameWrite, { "rename.write" } },
	{ C::RequirementAssigned, { "requirement.assigned" } },
	{ C::RequirementKind, { "requirement.kind" } },
	{ C::RequirementMissing, { "requirement.missing", F::Requirement } },
	{ C::RequirementOptionalMissing, { "requirement.optional_missing", F::Requirement } },
	{ C::RequirementUnknown, { "requirement.unknown" } },
	{ C::RequirementUnknownFile, { "requirement.unknown_file" } },
	{ C::RequirementWrongKind, { "requirement.wrong_kind", F::WrongKind } },
	{ C::UnsavedDiscard, { "unsaved.discard" } },
	{ C::UnsavedNone, { "unsaved.none" } },
};

static_assert(std::size(kEntries) == kCoreFindingCount, "every CoreFinding has exactly one row");
static_assert(finding_entries_well_formed(kEntries),
		"the core rows follow CoreFinding's order, each token its own, a Rewrite's words on a Rewrite row");

constexpr std::array<FindingCodeRow, kCoreFindingCount> kRows = finding_rows(kEntries);

} // namespace

const FindingCodeRow &finding_code(CoreFinding code) {
	return kRows[static_cast<size_t>(code)];
}

FindingTable core_finding_codes() { return { kRows.data(), kRows.size() }; }

const FindingCodeRow *finding_row(const std::string &token) {
	// The core's by a map made once; a type's by its own table, as the registry answers it now (a
	// test's stand-in in its type's place).
	static const std::unordered_map<std::string_view, const FindingCodeRow *> core = [] {
		std::unordered_map<std::string_view, const FindingCodeRow *> out;
		for (const FindingCodeRow &row : kRows) out.emplace(row.token, &row);
		return out;
	}();
	if (const auto found = core.find(std::string_view(token)); found != core.end())
		return found->second;
	for (size_t i = 1; i <= kDocumentTypeCount; ++i) {
		const DocumentType *type = document_type(static_cast<DocumentTypeId>(i));
		if (!type || !type->findings) continue;
		for (const FindingCodeRow &row : type->findings())
			if (token == row.token) return &row;
	}
	return nullptr;
}

} // namespace opennova::editor
