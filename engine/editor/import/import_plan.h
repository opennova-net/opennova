#pragma once

// What importing a selection brings along (ADR 0046 S11f), planned before anything is
// written: the rows ProjectSession's import preview shows (the import dialog, the editor
// MCP) and the command line's --dry-run prints (S11g). The selected sources are read as
// import_assets reads them; a converter source (an .o3d, an .o3a) is converted in memory
// and its outputs are the files planned, so the textures an .o3d's materials name are
// dependencies like any other rather than copies the
// converter makes. Then, breadth first, each reference to a file (the asset graph's edges,
// read from the bytes: extract_from_bytes) is looked up by the names its loader reads
// (reference_file_candidates: a model texture by its row's type, a menu texture and a font
// by the menu loader's rules), each a file only when it is of the kind the reference loads
// (by its bytes where its name cannot tell: a .bin, a chunk container). In order: the project
// already resolves it (no row); a file the plan takes provides it (no new row); the origin of
// the file that names it (the folder a loose source sits in, the archive a member comes from)
// has it; the game install has it; else it is not found. Every place with a file for the
// reference other than the one taken is reported beside the file taken, with whether the two
// differ, the file a later reference meets included. A dependency found is copied as the
// game's own (ImportChoice::native: a PNG stays the texture the reference names). A %NAME%
// resolves through the stylesheets the project reads once the import is in, as the shell
// loads them (menu::load_shell_style: menu_style.mns, then brand.mns over it, each the
// selection's copy where it brings one, else the project's); one they do not define is not
// followed (its style variable's own reference is listed), nor is a stylesheet value the
// game does not read. A reference to a symbol or a sound names no file, and a file of a kind
// whose references the graph does not read (a terrain, a script, a sound bank, a def table
// beyond the catalogs and the avatar table) is taken with the files it names not looked for:
// each kind is listed once as not followed. A name is planned once (a cycle stops there); a
// cap on the files planned stops the plan, the selection's files included, the files one
// converter source makes taken whole or not at all (truncated). The plan reads the sources
// and the places it looks, and writes nothing.
//
// The closure of a mission (ADR 0046 S14, plan R4: a project is its own files, and an import
// brings everything the game would open for what it imports). Beyond the file references:
// a reference to a SYMBOL (an item id, a weapon, an ammo, a powerup, a particle effect, a
// string id, a screen, a window, a user point, a style variable) is followed to the file that
// defines it, where no file of the project or the plan does: the file its scope names where
// the kind's row says so (a string id's table, a screen's menu, a user point's model), else
// the first file of the kind the row says defines it (ReferenceKindRow::defined_in: the
// catalogs, the particle files, the shell's stylesheets) in the places that has a definition;
// one no place defines is counted (ImportPlan::undefined). A file reference with a fallback (a
// mission's text table, else medmssn.bin) brings the fallback where no place has the name, and
// one the game runs without (GraphEdge::optional: a mission's script, its dialog bank) found
// nowhere is no row. The files the game finds by a mission's name are the mission's edges
// (documents/mission_file_set.h), followed as any other. A planned MISSION brings every file the
// game opens by a fixed literal at boot, at the menu, at mission start (gameprofile's
// required-resource manifest; a Required one found nowhere is not found, an optional one no row)
// and while a mission runs. A stylesheet the walk brings is read for the %NAME%s the menus
// followed so far expand through, and those menus are followed again.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/vfs/vfs.h>
#include <editor/assets/asset_import.h>
#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/install_view.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/diagnostic.h>
#include <editor/model/value.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

// A place the files of an import come from and its dependencies are looked for: the folder
// a loose source sits in (listed once), the archive a member comes from, or the game install
// (as the project imports it, assets/install_view.h: its files by the names the project gets).
// Names compare as the scan's logical names do, without case (normalized_logical_name).
class ImportOrigin {
public:
	enum class Kind { Folder, Archive, GameInstall };
	// False, with `error` saying why, when the folder cannot be listed, the archive does not
	// open or the folder holds none of the game's archives.
	bool open(Kind kind, const std::string &path, const ProjectDocument &document, std::string &error);
	Kind kind() const { return kind_; }
	const std::string &path() const { return path_; }
	// The origin's own spelling of its file of `name`; "" when it has none.
	std::string find(const std::string &name) const;
	// A file of the origin as import_assets reads it (an archive's or the install's decoded),
	// by the name `find` spells.
	bool read(const std::string &name, std::vector<uint8_t> &out) const;
	// The size of a file of the origin as it is stored there (a folder's file on the disk, an
	// archive's entry), without reading it; 0 when the origin has no such file.
	uint64_t size(const std::string &name) const;
	// The origin's files a name alone makes of `kind` (classify_asset with no bytes: a kind of a
	// file name or an extension, never one its bytes decide), in the origin's order, each as it
	// spells it.
	std::vector<std::string> files_of_kind(AssetKind kind) const;
	// What a file of the origin is to the engine once copied as the game's own: its kind by its
	// name, by its bytes where the name cannot tell (a .bin, a chunk container), a PNG a
	// texture (it gets no import record); Unknown when the origin has no such file.
	AssetKind file_kind(const std::string &name) const;
	// The source import_assets takes for the origin's file `name`: a folder's file copied as
	// the game's own (native).
	ImportChoice source(const std::string &name) const;
	// Where its file `name` is, in words: "the folder C:/art", "the archive C:/mod/menus.pff", "the game
	// install" (for an install with an expansion, "the game install's expansion jox01" or "the game
	// install's base game", the layer the file is served from; ADR 0046 S16).
	std::string words(const std::string &name = std::string()) const;

private:
	Kind kind_ = Kind::Folder;
	std::string path_;
	Vfs vfs_;                                        // the archive
	InstallView install_;                            // the game install
	std::map<std::string, std::string> names_;       // normalized name -> the origin's spelling
	mutable std::map<std::string, AssetKind> kinds_; // normalized name -> file_kind, once asked
};

// A reference that wants a file: the file naming it (its logical name), the record in it and
// the field, what kind of file it names and the name as the loader is handed it (a style
// variable's value where one stands; its case matters to a model texture's rule), and what the
// reference's loader picks the file by (GraphEdge::loader_arg: a model texture row's type).
// `record_kind`: the kind of the record (GraphEdge::address). `words`: the record and the field as the
// file's document type words them, once the plan is whole (the UX round's project lane), "" where the
// record and the field say it already (the game's own needs).
struct ImportNeed {
	std::string file;
	std::string record;
	std::string field;
	ReferenceKind reference = ReferenceKind::None;
	std::string name;
	int32_t loader_arg = -1;
	NodeKind record_kind = 0;
	std::string words;
};

// What wanted a file, in words (the import dialog's Needed by, the wire's needed_by.words): the file,
// then the record by its kind's label and its names ("main.mnu, window STARTUP > MAIN: String table"),
// as its document type words them; the record and the field as written where the type has no words
// for them.
std::string import_need_text(const ImportNeed &need);

// Another place with a file for a reference the planned file serves, which the plan's order
// passed over.
struct ImportRival {
	std::string name;     // its spelling there
	std::string found_in; // the place, in words
	ImportChoice source;
	bool differs = false; // its bytes differ from those of the file planned
};

struct ImportPlanRow {
	// A selected source's file (a converter source's outputs, each a row), a dependency found,
	// or one looked for and found nowhere.
	enum class State { Selected, Found, NotFound };
	State state = State::Selected;
	ImportChoice source;     // what import_assets takes (the outputs of one converter source share it; none when not found)
	std::string name;        // the logical name the project gets; not found: the name as the reference gives it
	AssetKind kind = AssetKind::Unknown; // what the file is to the engine once imported (not found: the kind wanted)
	std::string destination; // project-relative, where import_assets writes it
	// On a converter's output, its source's name; on a file an import source brings beside it (a font
	// set's glyph sheet: import_source_inputs), that source's; "" the file itself. The files of one
	// source come together.
	std::string made_from;
	std::string found_in;    // where it comes from, in words
	// The file's bytes as it is stored where it comes from (a converter's output: as made); 0 for
	// one not found. What the dialog sums before anything is copied.
	uint64_t size = 0;
	// The first reference that wanted it (empty for a selected source); not found, the first
	// whose lookup nothing planned meets (two references to one name can want different files:
	// a diffuse row takes a .dds the plan brings, a plain row does not).
	ImportNeed needed_by;
	// Every planned file that names it, in the order the walk met them, needed_by's first (the UX
	// round's project lane: a texture main.mnu and a model both name is under each in the dialog's tree,
	// and leaving out one's branch keeps it while the other is taken); empty for a selected source.
	std::vector<std::string> wanted_by;
	// What the import takes: the selected sources (one with a problem is refused when the import
	// runs; one the project holds is not, `held`), and each dependency found that the project can
	// take (one with a problem is listed, not taken).
	bool selected = false;
	// A chosen file of a name the project holds already (ADR 0046 S14: the whole game install over a
	// project with files of its own): the import leaves the project's file as it is unless it is
	// asked to replace it (the row checked in the dialog, or Replace existing files: import_files'
	// `replace`). Not selected.
	bool held = false;
	// For a held file, whether the project's holds the same bytes as the one chosen (the UX round's
	// project lane: a Replace of a file the author edited loses the edits): compared for the first
	// kHeldCompared held files, Unknown past them.
	enum class Held : uint8_t { Unknown, Same, Differs };
	Held held_as = Held::Unknown;
	// Why the project cannot take the file as it is (check_project_file_name; a kind the game
	// does not use; a second selected file of the name), "" when it can.
	std::string problem;
	std::vector<ImportRival> rivals;
};

// Whether what a file of a kind names goes unread: the kind names files
// (AssetKindRow::names_files) and the graph does not read it (graph_reads_kind: a kind it has no
// record type or extractor for: a dialog bank, the def tables beyond the catalogs, the avatar
// table, a face, a mission text). An import takes such a file and lists it as not followed.
bool references_unread(AssetKind kind);

// What the walk does not follow, once per kind: references of a kind that names no file (a
// symbol, a def's sound), with how many the planned files hold; or the planned files whose
// references go unread (references_unread). `first` is the file where it was met first.
struct ImportNotFollowed {
	ReferenceKind reference = ReferenceKind::None; // the reference kind; None when `kind` is a file kind
	AssetKind kind = AssetKind::Unknown;
	size_t count = 0;
	std::string first;
};

// The files a plan takes of one kind: how many, and their bytes as stored where they come from.
struct ImportPlanKind {
	AssetKind kind = AssetKind::Unknown;
	size_t files = 0;
	uint64_t bytes = 0;
};

struct ImportPlan {
	std::vector<ImportPlanRow> rows; // in the walk's order, the selected sources first
	std::vector<ImportNotFollowed> not_followed;
	// The symbols followed to no file (ADR 0046 S14): references to a name no place defines (a
	// string id no table of the project, the plan or the places has; an item id of no items.def
	// anywhere), once per reference kind, with how many and where the first was met. What the
	// game would show the id of, or leave out.
	std::vector<ImportNotFollowed> undefined;
	// The symbols a place's copy of a file defines where the project's own copy of that file, the one
	// the game reads, does not (an edited items.def without an item a mission places): the import
	// keeps the project's file unless asked to replace it, so the place's copy is not brought and the
	// names stay undefined in the project (review F6); once per reference kind (`kind` the defining
	// file's), with how many and where the first was met.
	std::vector<ImportNotFollowed> shadowed;
	// A source that cannot be read or converted (as import_assets reports it), a file whose
	// references could not be read, a folder that cannot be listed, a game install that does
	// not mount.
	std::vector<Diagnostic> diagnostics;
	// The cap on the files planned stopped the plan: files of the selection past it, or
	// dependencies, are not in it.
	bool truncated = false;

	// The files the plan takes (every row but those not found) and their bytes as stored.
	size_t file_count() const;
	uint64_t total_bytes() const;
	// Those files by kind, the largest kind first (by bytes, then by files, then by the kind's
	// token): the dialog's summary line and the wire's `summary` (ADR 0046 S14: a mission's closure
	// is thousands of rows, read by kind before one by one).
	std::vector<ImportPlanKind> by_kind() const;
};

// A guard, not a limit a real import meets (ADR 0046 S14: a mission's closure is most of a game
// install, 8,700 files of JO's 9,290): the walk stops there and says so.
inline constexpr size_t kImportPlanFileCap = 50000;
// The held files a plan compares with the project's (ImportPlanRow::held_as): a few chosen again over
// a project are compared, a whole install over one is not read twice. The sizes first: a folder's file
// of another size differs, nothing read; an archive's or the install's may be stored in the SCR or the
// compressed form, so one of another size is read only up to kHeldReadBytes (a text), a larger one left
// unknown.
inline constexpr size_t kHeldCompared = 64;
inline constexpr uint64_t kHeldReadBytes = uint64_t(4) << 20;

// The plan made a step at a time (ADR 0046 S14; S13 A3's rule for every long job): the game
// install mounted where the plan looks there (unless the caller mounted it), then the chosen
// sources taken, a few a step, then, with dependencies, the stylesheets and the walk, each queued
// file read and its references followed within the step's bytes. A file's bytes are read when its
// references are followed, never when it is queued, and a file the graph does not read is not
// read at all (its kind from its name, its size from where it is stored). The project's scan and
// graph are the caller's and outlive the planner (its paths and document it copies).
class ImportPlanner {
public:
	ImportPlanner(std::vector<ImportChoice> sources, bool with_dependencies, const ProjectPaths &paths,
	              const ProjectDocument &document, const AssetScan &scan, const AssetGraph &graph,
	              std::string retail_directory, size_t file_cap = kImportPlanFileCap,
	              std::shared_ptr<const ImportOrigin> install_mounted = nullptr);
	~ImportPlanner();
	ImportPlanner(const ImportPlanner &) = delete;
	ImportPlanner &operator=(const ImportPlanner &) = delete;

	// One step within `bytes` read (at least one source or file); true once the plan is whole.
	bool step(uint64_t bytes);
	bool done() const;
	// Its progress: the files it knows of so far (those planned and the chosen sources not yet
	// taken; the walk finds more as it reads, and the count never falls), and those done with (a
	// planned file whose references were followed, or that has none to follow).
	size_t files_known() const;
	size_t files_done() const;
	// The plan: whole once done (taken once; the planner holds none after).
	ImportPlan take();

private:
	class Walk;
	std::unique_ptr<Walk> walk_;
};

// The plan of importing `sources` into the project (its files `scan`, resolved by `graph`),
// with the files they need when `with_dependencies`, looked for in the game install at
// `retail_directory` too ("" for none), `file_cap` files at most. `install_mounted`: the game
// install at `retail_directory` opened already (ImportOrigin::Kind::GameInstall), which a caller
// stepping the plan mounts in a step of its own (S13 A3); null, the plan mounts it when it needs it.
// An ImportPlanner run to its end.
ImportPlan plan_import(const std::vector<ImportChoice> &sources, bool with_dependencies, const ProjectPaths &paths,
                       const ProjectDocument &document, const AssetScan &scan, const AssetGraph &graph,
                       const std::string &retail_directory, size_t file_cap = kImportPlanFileCap,
                       std::shared_ptr<const ImportOrigin> install_mounted = nullptr);

// Whether two plans come to the same import (an import checks the plan it shows against the
// one it makes again before it writes): the same rows in the same order, each the same file
// from the same place (its source) to the same destination, of the same kind, found or not
// found alike, taken or held alike, with the same problem; and both stopped by the cap, or neither. What wanted a
// file first, where else it was found and the findings do not decide what is written.
bool same_import(const ImportPlan &a, const ImportPlan &b);

// Why the import cannot take the plan's row `index`: its file's problem, or that of another file its
// converter source makes (the files of one source come together or not at all); "" when it can.
std::string import_row_refusal(const ImportPlan &plan, size_t index);
// The checks of a new plan (the import dialog's, held by the session's workspace): each row the plan takes
// (a chosen file whatever its problem, the import waiting until it is unchecked; a dependency only when the
// project can take it), and with `replace_existing` each file the project holds that it can take.
std::vector<bool> import_default_checks(const ImportPlan &plan, bool replace_existing);

// What an Import of a plan takes by its checks: the one rule of the dialog's Import and of import_files planned
// (the MCP gaps lane). Each checked row's source once, in the plan's order (a converter's outputs share it), a row
// found nowhere or one the project cannot take left out; whether it replaces: Replace existing files, or a
// checked row the project holds (a held file is written over only where its row is checked: an unchecked one's
// source is never taken); how many rows are checked; and `blocked`, the first checked row the project cannot
// take, in the dialog's words ("" none): the dialog's Import waits on it, a planned import leaves it out.
struct ImportSelection {
	std::vector<ImportChoice> sources;
	bool replace = false;
	size_t checked = 0;
	std::string blocked;
};
ImportSelection import_selection(const ImportPlan &plan, const std::vector<bool> &checked, bool replace_existing);

} // namespace opennova::editor
