#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>

#include <editor/model/diagnostic.h>

namespace opennova::editor {

// The finding codes (ADR 0046 S13 A6): every finding the editor makes is made from a row of one
// of these tables, the editor's own (CoreFinding) or a document type's (DocumentType::findings,
// each type's enum and its table beside its validator), so a code no table declares is a compile
// error, never a free text. A row says what Problems does with a finding of its code: the fixes it
// offers (problem_fixes.h), what a Rewrite of the file drops or normalizes, whether the finding
// says the file does not serialize, and where Problems takes it. Everything that asks about a code
// reads its row (finding_row), never its spelling.
//
// A seam header like graph/reference_kinds.h: every ranked library may include it (a document
// type's validator, the model's refusals, the graph's findings make their findings from its rows),
// so it includes the model alone.

// The fixes Problems offers for a finding of the code (problem_fixes.h words each one).
enum class FindingFix {
	None,              // Problems goes to its place
	Requirement,       // a file the game reads by name that the project lacks: Create it, Import
	                   // it from the game data, for a required one Use a file of its kind as it
	WrongKind,         // a file of the name of another kind: Import the game's own, Rename... it
	Rename,            // a name the archives cannot take or another file has: Files' Rename...
	ResetRow,          // an animation table with no anim_reset row: Add one
	Reference,         // a reference to a name the project lacks (ReferenceSubject): Import, Create,
	                   // a placeholder texture, or Open the file where a symbol belongs
	UnimportedTexture, // a texture an import's model names that it did not bring: the reference's
	                   // own fixes while the project still lacks it
	Reload,            // an open document whose file changed outside the editor: Reload it
	Reimport,          // an import whose output is missing: Import it again
	Rewrite,           // input a rewrite drops or normalizes: Rewrite the file (rewrite_does)
};

// Where Problems takes a finding of the code: what the file holds (its document opened on the
// record and field the finding names; Files for a kind the editor does not open), or the file as a
// whole (its name, its place among the build's files: Files, which shows and renames it).
enum class FindingPlace { Content, File };

// One finding code: `token` is the stable dotted code the finding carries (Diagnostic::code, the
// wire's `code`); `fixes` what Problems offers; `rewrite_does` a Rewrite's words, what writing the
// file again does ("with every line ending CR LF"), set exactly on a Rewrite row; `blocks_save`
// that the finding says the file does not serialize (its Save is refused, so no Rewrite is offered
// for the file); `place` where Problems takes it.
struct FindingCodeRow {
	const char *token = nullptr;
	FindingFix fixes = FindingFix::None;
	const char *rewrite_does = nullptr;
	bool blocks_save = false;
	FindingPlace place = FindingPlace::Content;
};

// A table's rows, in the order of the enum it answers for.
struct FindingTable {
	const FindingCodeRow *rows = nullptr;
	size_t count = 0;
	const FindingCodeRow *begin() const { return rows; }
	const FindingCodeRow *end() const { return rows + count; }
};

// A row as a table writes it: with the enumerator it answers for, which the table's static_asserts
// hold in the enum's order (finding_entries_well_formed); finding_rows makes the rows a
// FindingTable spans.
template <typename Code>
struct FindingCodeEntry {
	Code code;
	FindingCodeRow row;
};

template <typename Code, size_t N>
constexpr std::array<FindingCodeRow, N> finding_rows(const FindingCodeEntry<Code> (&entries)[N]) {
	std::array<FindingCodeRow, N> rows{};
	for (size_t i = 0; i < N; ++i) rows[i] = entries[i].row;
	return rows;
}

constexpr bool same_finding_token(const char *a, const char *b) {
	while (*a && *a == *b) {
		++a;
		++b;
	}
	return *a == *b;
}

// Whether a table's entries follow its enum's order, each with a token of its own, a Rewrite's
// words exactly on its Rewrite rows, and no Rewrite on a row that says the file does not
// serialize.
template <typename Code, size_t N>
constexpr bool finding_entries_well_formed(const FindingCodeEntry<Code> (&entries)[N]) {
	for (size_t i = 0; i < N; ++i) {
		const FindingCodeRow &row = entries[i].row;
		if (static_cast<size_t>(entries[i].code) != i || !row.token || !*row.token) return false;
		if ((row.fixes == FindingFix::Rewrite) != (row.rewrite_does != nullptr)) return false;
		if (row.blocks_save && row.fixes == FindingFix::Rewrite) return false;
		for (size_t j = 0; j < i; ++j)
			if (same_finding_token(row.token, entries[j].row.token)) return false;
	}
	return true;
}

// Whether no two rows of a table share a token (a table joined from two entry lists).
template <size_t N>
constexpr bool finding_tokens_unique(const std::array<FindingCodeRow, N> &rows) {
	for (size_t i = 0; i < N; ++i)
		for (size_t j = 0; j < i; ++j)
			if (same_finding_token(rows[i].token, rows[j].token)) return false;
	return true;
}

// What a Rewrite does for input the game ignores, which the reader left out (a catalog's, a
// menu's, an animation table's ignored input).
inline constexpr const char *kRewriteDropsIgnoredInput = "without the input the game ignores";

// The editor's own codes, which no document type declares: the project, its files and their names,
// the requirements, the documents' lifecycle and edits, the graph, imports, the build, Play,
// renames, the settings, the session's operations and the unsaved-changes prompt.
enum class CoreFinding {
	AssetKindUnknown,
	AssetNameDuplicate,
	AssetNameEmpty,
	AssetNameTooLong,
	AssetUnreadable,
	BlankDef,
	BlankFont,
	BlankMenu,
	BlankStrings,
	BlankStyle,
	BlankTexture,
	BlankUnavailable,
	BuildArchive,
	BuildArchiveInProject,
	BuildBlocked,
	BuildChanged,
	BuildCopy,
	BuildNameUnstorable,
	BuildRead,
	BuildVerify,
	BuildWrite,
	CreateMissingExists,
	CreateMissingUnknown,
	CreateMissingWrite,
	CreateMissingWrongKind,
	DocumentBatch,
	DocumentCollection,
	DocumentConflict,
	DocumentCopy,
	DocumentDecode,
	DocumentDuplicate,
	DocumentKind,
	DocumentMissing,
	DocumentName,
	DocumentNoFile,
	DocumentNoRecords,
	DocumentNotOpen,
	DocumentParse,
	DocumentPaste,
	DocumentPath,
	DocumentPayload,
	DocumentRead,
	DocumentRevertNothing,
	DocumentSelection,
	DocumentSnapshot,
	DocumentStale,
	DocumentStructure,
	DocumentUnserializable,
	DocumentValue,
	DocumentWrite,
	EditorSettingsJson,
	EditorSettingsSchemaVersionUnsupported,
	EditorSettingsUnreadable,
	EditorSettingsWrite,
	GraphUnreadable,
	ImportAlphaDropped,
	ImportArchive,
	ImportChanged,
	ImportDecode,
	ImportDuplicate,
	ImportEncode,
	ImportExists,
	ImportFolder,
	ImportInstall,
	ImportKind,
	ImportName,
	ImportNotPlanned,
	ImportNotPublished,
	ImportOption,
	ImportOrphanRecord,
	ImportOutputMissing,
	ImportPath,
	ImportPublish,
	ImportRead,
	ImportRecord,
	ImportScene,
	ImportSceneNote,
	ImportSidecar,
	ImportSource,
	ImportTextureNotImported,
	ImportUnreadable,
	ImportWrite,
	LocalSettingsJson,
	LocalSettingsSchemaVersionUnsupported,
	LocalSettingsUnreadable,
	LocalSettingsWrite,
	OperationBusy,
	OperationNone,
	OperationNotCancellable,
	PlayAlreadyRunning,
	PlayBootMissing,
	PlayCrashed,
	PlayInstallCopy,
	PlayInstallMissing,
	PlayRuntimeMissing,
	PlaySpawn,
	PlayUnsupported,
	ProjectExists,
	ProjectFieldInvalid,
	ProjectFileMissing,
	ProjectFileUnreadable,
	ProjectJson,
	ProjectNone,
	ProjectRootUnreadable,
	ProjectSchemaVersionUnsupported,
	ProjectTargetGameUnknown,
	ProjectTitleEmpty,
	ProjectWrite,
	ReferenceMissing,
	RenameConflict,
	RenameCopy,
	RenameExists,
	RenameImported,
	RenameKind,
	RenameMove,
	RenameName,
	RenamePartial,
	RenamePath,
	RenameRemove,
	RenameSite,
	RenameStyle,
	RenameTooLong,
	RenameUnchanged,
	RenameUnknownFile,
	RenameUnknownSymbol,
	RenameWrite,
	RequirementAssigned,
	RequirementKind,
	RequirementMissing,
	RequirementOptionalMissing,
	RequirementUnknown,
	RequirementUnknownFile,
	RequirementWrongKind,
	UnsavedDiscard,
	UnsavedNone,
	kCount
};
inline constexpr size_t kCoreFindingCount = static_cast<size_t>(CoreFinding::kCount);

// A core code's row (finding_codes.cpp holds one per code, in the enum's order; static_asserts
// there check it), and the whole table.
const FindingCodeRow &finding_code(CoreFinding code);
FindingTable core_finding_codes();

// The row of a token: the core's, else a registered document type's own (DocumentType::findings,
// a test's stand-in in its type's place); null for a token no table declares.
const FindingCodeRow *finding_row(const std::string &token);

// A finding of a row's code, at a place (the file's project-relative path, the field).
inline Diagnostic make_finding(const FindingCodeRow &row, DiagnosticSeverity severity,
		std::string message, std::string asset = std::string(),
		std::string field = std::string()) {
	Diagnostic d;
	d.severity = severity;
	d.code = row.token;
	d.message = std::move(message);
	d.asset = std::move(asset);
	d.field = std::move(field);
	return d;
}

// A finding of a table's code, by its enumerator: CoreFinding's, or a document type's own enum
// (whose finding_code overload, declared beside it, names its row).
template <typename Code, typename = std::enable_if_t<std::is_enum<Code>::value>>
Diagnostic make_finding(Code code, DiagnosticSeverity severity, std::string message,
		std::string asset = std::string(), std::string field = std::string()) {
	return make_finding(finding_code(code), severity, std::move(message), std::move(asset),
			std::move(field));
}

} // namespace opennova::editor
