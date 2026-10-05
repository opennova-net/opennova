#pragma once

#include <array>
#include <cstddef>

namespace opennova::editor {

// The finding codes (ADR 0046 S13 A6): every finding the editor makes is made from a row of one of
// these tables (make_finding, model/diagnostic.h), the editor's own (CoreFinding, below) or a
// document type's (DocumentType::findings, each type's enum and its table beside its validator),
// and it keeps the row it was made from (Diagnostic::row), so a finding cannot carry a code no
// table declares, and a producer that names a code no enum has does not compile. A row says what
// Problems does with a finding of its code: the fixes it offers (session/problem_fixes.h), what a
// Rewrite of the file drops or normalizes, whether the finding says the file does not serialize,
// where Problems takes it, the group it shows under, where it comes from and, for a compiler note
// of the render check, whether it is a Problems row at all. Everything that asks about a finding
// reads its row, never its spelling; session/finding_codes.h is the lookup by a token over every
// table and the columns' wire forms.

// The fixes Problems offers for a finding of the code (session/problem_fixes.h words each one).
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
	TextureRows,       // a TGA stored top first (S18): Save it bottom first (texture_operation)
	ImportFitsUse,     // what a use asks of a texture an import makes (S18): Make the import fit the use
};

// Where Problems takes a finding of the code: what the file holds (its document opened on the
// record and field the finding names; Files for a kind the editor does not open), or the file as a
// whole (its name, its place among the build's files: Files, which shows and renames it).
enum class FindingPlace { Content, File };

// The group Problems shows a finding under when it groups by kind: one per family of codes (the
// first dotted segment every code of the group shares), the optional files apart from the
// required. The editor's own rows name theirs one by one; a document type's table is one family,
// its rows taking the table's (finding_rows). Its key and title are session/finding_codes.h's.
enum class FindingGroup {
	None, // a row not placed yet: no table keeps one
	RequiredFiles,
	OptionalFiles,
	MissingReferences,
	FilesNotChecked,
	ProjectFiles,
	Project,
	Expansion,
	Documents,
	Imports,
	Build,
	Export,
	Play,
	Renames,
	NewFiles,
	CreateMissing,
	EditorSettings,
	LocalSettings,
	Operations,
	UnsavedChanges,
	Viewports,
	Workspace,
	Navigation,
	Catalogs,
	StringTables,
	Menus,
	Stylesheets,
	Models,
	Animations,
	AnimationMaps,
	Scripts,
	MusicScripts,
	Credits,
	Shaders,
	Missions,
	Textures,
	kCount
};
inline constexpr size_t kFindingGroupCount = static_cast<size_t>(FindingGroup::kCount);

// What made a finding of the code, where it is not its group's own part of the editor: the asset
// graph (a reference the project lacks, a file it could not read) or the menu render check (a
// compiler note, a screen it could not map). The menu report reads it (a menu's rows by source).
enum class FindingSource { Own, Graph, RenderCheck };

// Whether a finding of the code is a Problems row, and at what severity, where its row decides it:
// the render check's compiler notes (preview/menu_render_check.h's menu_note_problem reads it), a
// Warning or an Info for a consequence the author may not mean, None for a note only the preview
// shows (a name the project lacks, which is the asset graph's reference.missing; a few that only
// explain the picture: a CUSTOM hook, a state held, a frame whose stencil did not load, a table or
// marquee filled at run time). None on every other row, whose producer picks each finding's
// severity.
enum class FindingProblem { None, Info, Warning };

// One finding code: `token` is the stable dotted code a finding of it carries (Diagnostic::code,
// the wire's `code`); `fixes` what Problems offers; `rewrite_does` a Rewrite's words, what writing
// the file again does ("with every line ending CR LF"), set exactly on a Rewrite row;
// `blocks_save` that the finding says the file does not serialize (its Save is refused, so no
// Rewrite is offered for the file); `place` where Problems takes it; `group` the group it shows
// under; `source` what made it, when not its group's own part; `problem`, on a render check's row
// alone, whether a finding of it is a Problems row and at what severity; `gates_build` whether an
// error of the code, among the rows a build reads, refuses the build (blocks_build,
// graph/reference_kinds.h). The build follows retail, one set of rules (ADR 0046 S14, "the gate"):
// it is refused exactly where the built game would fail to load or run as retail does, each such row
// citing the original's refusal, and where the editor cannot vouch for what it packs (a file it
// cannot read or write, a name the archives cannot store; a file it cannot write gates only where the
// build must write it, not over the game's own bytes packed as stored: graph/reference_kinds.h
// ShippedFiles, S16). Every other code is listed (false): its
// findings are shown, counted and fixable and refuse nothing. A listed code whose subject names the
// witness gates where it does: a missing reference of a kind whose row cites the game's refusal
// (ReferenceKindRow::gates_when_missing), a missing required file whose manifest row is the game's
// refusal to boot (RES_FATAL).
struct FindingCodeRow {
	const char *token = nullptr;
	FindingFix fixes = FindingFix::None;
	const char *rewrite_does = nullptr;
	bool blocks_save = false;
	FindingPlace place = FindingPlace::Content;
	FindingGroup group = FindingGroup::None;
	FindingSource source = FindingSource::Own;
	FindingProblem problem = FindingProblem::None;
	bool gates_build = true;
};

// A document type's row of a code whose findings are listed and refuse no build (gates_build false):
// what the game does with what it is about is no refusal of its load or its run, or is not witnessed.
constexpr FindingCodeRow listed_code(const char *token, FindingFix fixes = FindingFix::None,
		const char *rewrite_does = nullptr) {
	FindingCodeRow row;
	row.token = token;
	row.fixes = fixes;
	row.rewrite_does = rewrite_does;
	row.gates_build = false;
	return row;
}

// A table's rows, in the order of the enum it answers for.
struct FindingTable {
	const FindingCodeRow *rows = nullptr;
	size_t count = 0;
	const FindingCodeRow *begin() const { return rows; }
	const FindingCodeRow *end() const { return rows + count; }
	// Whether `row` is one of this table's (by address: a finding keeps the table's own row).
	bool holds(const FindingCodeRow *row) const { return row && row >= rows && row < rows + count; }
};

// A row as a table writes it: with the enumerator it answers for, which the table's static_asserts
// hold in the enum's order (finding_entries_well_formed); finding_rows makes the rows a
// FindingTable spans.
template <typename Code>
struct FindingCodeEntry {
	Code code;
	FindingCodeRow row;
};

// The rows of a table written as entries, in their order.
template <typename Code, size_t N>
constexpr std::array<FindingCodeRow, N> finding_rows(const FindingCodeEntry<Code> (&entries)[N]) {
	std::array<FindingCodeRow, N> rows{};
	for (size_t i = 0; i < N; ++i) rows[i] = entries[i].row;
	return rows;
}

// The rows of a document type's table, each taking the table's group (the type's family).
template <typename Code, size_t N>
constexpr std::array<FindingCodeRow, N> finding_rows(const FindingCodeEntry<Code> (&entries)[N],
		FindingGroup group) {
	std::array<FindingCodeRow, N> rows = finding_rows(entries);
	for (size_t i = 0; i < N; ++i) rows[i].group = group;
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
		// A file that does not serialize cannot be packed as the editor holds it: it gates, but over
		// the game's own bytes held unedited, which the build packs as stored (ShippedFiles, S16).
		if (row.blocks_save && !row.gates_build) return false;
		for (size_t j = 0; j < i; ++j)
			if (same_finding_token(row.token, entries[j].row.token)) return false;
	}
	return true;
}

// Whether no two rows of a table share a token (a table joined from two entry lists), every row
// has its group, and only a render check's row says whether its findings are Problems rows.
template <size_t N>
constexpr bool finding_rows_well_formed(const std::array<FindingCodeRow, N> &rows) {
	for (size_t i = 0; i < N; ++i) {
		if (rows[i].group == FindingGroup::None || rows[i].group == FindingGroup::kCount) return false;
		if (rows[i].problem != FindingProblem::None && rows[i].source != FindingSource::RenderCheck)
			return false;
		for (size_t j = 0; j < i; ++j)
			if (same_finding_token(rows[i].token, rows[j].token)) return false;
	}
	return true;
}

// What a Rewrite does for input the game ignores, which the reader left out (a catalog's, a
// menu's, an animation table's ignored input).
inline constexpr const char *kRewriteDropsIgnoredInput = "without the input the game ignores";

// The editor's own codes, which no document type declares: the project, its files and their names,
// the requirements, the documents' lifecycle and edits, the graph, imports, the build, Play,
// renames, the settings, the session's operations and the unsaved-changes prompt; and what a texture
// use asks of the file its loader opens (ADR 0046 S18, graph/texture_checks), a finding on the use
// (the referring file's field, of any type) or on a file the game opens by name.
enum class CoreFinding {
	AssetKindUnknown,
	AssetNameDuplicate,
	AssetNameEmpty,
	AssetNameTooLong,
	AssetUnreadable,
	BlankDef,
	BlankFont,
	BlankMenu,
	BlankMission,
	BlankStrings,
	BlankStyle,
	BlankTexture,
	BlankUnavailable,
	BuildArchive,
	BuildArchiveInProject,
	BuildBlocked,
	BuildChanged,
	BuildCopy,
	BuildExpansionBaseMissing,
	BuildExpansionExpDesc,
	BuildExpansionExpName,
	BuildExpansionMissionTwice,
	BuildExpansionMissionUntitled,
	BuildExpansionRootOnly,
	BuildNameUnstorable,
	BuildOutDirInProject,
	BuildPlayerFile,
	BuildRead,
	BuildUnread,
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
	DocumentSpan,
	DocumentStale,
	DocumentStructure,
	DocumentUnserializable,
	DocumentValue,
	DocumentValues,
	DocumentWrite,
	EditorSettingsJson,
	EditorSettingsSchemaVersionUnsupported,
	EditorSettingsUnreadable,
	EditorSettingsWrite,
	ExpansionFileUnread,
	ExportCancelled,
	ExportCleanup,
	ExportFolder,
	ExportReplaced,
	ExportRuntime,
	ExportWrite,
	GraphUnreadable,
	ImportAlphaDropped,
	ImportArchive,
	ImportChanged,
	ImportDecode,
	ImportDuplicate,
	ImportEncode,
	ImportExists,
	ImportFolder,
	ImportInput,
	ImportInstall,
	ImportKind,
	ImportName,
	ImportNotPlanned,
	ImportNotPublished,
	ImportOption,
	ImportOrphanRecord,
	ImportOutputMissing,
	ImportPath,
	ImportPlayerFile,
	ImportPublish,
	ImportRead,
	ImportRecord,
	ImportRequest,
	ImportScene,
	ImportSceneNote,
	ImportSidecar,
	ImportNotFound,
	ImportTextureNotImported,
	ImportUnreadable,
	ImportWrite,
	LocalSettingsJson,
	LocalSettingsSchemaVersionUnsupported,
	LocalSettingsUnreadable,
	LocalSettingsWrite,
	MissionSidecarUnused,
	NavigationNone,
	OperationBusy,
	OperationNone,
	OperationNotCancellable,
	PlayAlreadyRunning,
	PlayBootMissing,
	PlayCrashed,
	PlayInstallCopy,
	PlayInstallMissing,
	PlayInstallRunning,
	PlayMissionFailed,
	PlayMissionUnknown,
	PlayRunDirectory,
	PlayRuntimeMissing,
	PlaySpawn,
	PlayStrictExpansion,
	PlayUnsupported,
	ProjectExists,
	ProjectExpansionNameTaken,
	ProjectExpansionNotInstalled,
	ProjectExpansionUnsupported,
	ProjectFieldInvalid,
	ProjectFileMissing,
	ProjectFileUnreadable,
	ProjectInstallInvalid,
	ProjectJson,
	ProjectMissionFeatureOff,
	ProjectNone,
	ProjectRootUnreadable,
	ProjectSchemaVersionUnsupported,
	ProjectTargetGameUnknown,
	ProjectTitleEmpty,
	ProjectWrite,
	ReferenceMissing,
	ReferenceWrongKind,
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
	TextureAlphaNotLoaded,
	TextureBlendMapSize,
	TextureColourMapSize,
	TextureExternal,
	TextureFoliageMapOverrun,
	TextureFoliageMapShape,
	TextureHeightWrap,
	TextureLoadingScreenSize,
	TextureMfdNotPowerOfTwo,
	TextureNormalMapHalved,
	TextureOperation,
	TextureParticleTooBig,
	TextureReplace,
	TextureShowUse,
	TextureSplit,
	TextureTileAtlasCells,
	TextureWrongReader,
	UnsavedDiscard,
	UnsavedNone,
	ViewportRefused,
	WorkspaceRefused,
	kCount
};
inline constexpr size_t kCoreFindingCount = static_cast<size_t>(CoreFinding::kCount);

// A core code's row (finding_code_row.cpp holds one per code, in the enum's order; static_asserts
// there check it), and the whole table.
const FindingCodeRow &finding_code(CoreFinding code);
FindingTable core_finding_codes();

} // namespace opennova::editor
