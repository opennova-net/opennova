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
// game's own (ImportSource::native: a PNG stays the texture the reference names). A %NAME%
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

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <base/vfs/vfs.h>
#include <editor/assets/asset_import.h>
#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_registry.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/diagnostic.h>
#include <editor/model/value.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

// A place the files of an import come from and its dependencies are looked for: the folder
// a loose source sits in (listed once), the archive a member comes from, or the game install
// (mounted as a stock launch mounts it: mount_retail). Names compare as the scan's logical
// names do, without case (normalized_logical_name).
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
	// What a file of the origin is to the engine once copied as the game's own: its kind by its
	// name, by its bytes where the name cannot tell (a .bin, a chunk container), a PNG a
	// texture (it gets no import record); Unknown when the origin has no such file.
	AssetKind file_kind(const std::string &name) const;
	// The source import_assets takes for the origin's file `name`: a folder's file copied as
	// the game's own (native).
	ImportSource source(const std::string &name) const;
	// Where it is, in words: "the folder C:/art", "the archive C:/mod/menus.pff", "the game
	// install".
	std::string words() const;

private:
	Kind kind_ = Kind::Folder;
	std::string path_;
	Vfs vfs_;                                        // the archive, or the game install
	std::map<std::string, std::string> names_;       // normalized name -> the origin's spelling
	mutable std::map<std::string, AssetKind> kinds_; // normalized name -> file_kind, once asked
};

// A reference that wants a file: the file naming it (its logical name), the record in it and
// the field, what kind of file it names and the name as the loader is handed it (a style
// variable's value where one stands; its case matters to a model texture's rule), and what the
// reference's loader picks the file by (GraphEdge::loader_arg: a model texture row's type).
struct ImportNeed {
	std::string file;
	std::string record;
	std::string field;
	ReferenceKind reference = ReferenceKind::None;
	std::string name;
	int32_t loader_arg = -1;
};

// Another place with a file for a reference the planned file serves, which the plan's order
// passed over.
struct ImportRival {
	std::string name;     // its spelling there
	std::string found_in; // the place, in words
	ImportSource source;
	bool differs = false; // its bytes differ from those of the file planned
};

struct ImportPlanRow {
	// A selected source's file (a converter source's outputs, each a row), a dependency found,
	// or one looked for and found nowhere.
	enum class State { Selected, Found, NotFound };
	State state = State::Selected;
	ImportSource source;     // what import_assets takes (the outputs of one converter source share it; none when not found)
	std::string name;        // the logical name the project gets; not found: the name as the reference gives it
	AssetKind kind = AssetKind::Unknown; // what the file is to the engine once imported (not found: the kind wanted)
	std::string destination; // project-relative, where import_assets writes it
	std::string made_from;   // on a converter's output, its source's name ("" = the file itself)
	std::string found_in;    // where it comes from, in words
	// The first reference that wanted it (empty for a selected source); not found, the first
	// whose lookup nothing planned meets (two references to one name can want different files:
	// a diffuse row takes a .dds the plan brings, a plain row does not).
	ImportNeed needed_by;
	// What the import takes: the selected sources (one with a problem is refused when the import
	// runs), and each dependency found that the project can take (one with a problem is listed,
	// not taken).
	bool selected = false;
	// Why the project cannot take the file as it is (check_project_file_name; a kind the game
	// does not use; a second selected file of the name), "" when it can.
	std::string problem;
	std::vector<ImportRival> rivals;
};

// What the walk does not follow, once per kind: references of a kind that names no file (a
// symbol, a def's sound), with how many the planned files hold; or the planned files whose
// references the graph does not read (of a kind it does not read, or a mission's .mis:
// graph_reads_file). `first` is the file where it was met first.
struct ImportNotFollowed {
	ReferenceKind reference = ReferenceKind::None; // the reference kind; None when `kind` is a file kind
	AssetKind kind = AssetKind::Unknown;
	size_t count = 0;
	std::string first;
};

struct ImportPlan {
	std::vector<ImportPlanRow> rows; // in the walk's order, the selected sources first
	std::vector<ImportNotFollowed> not_followed;
	// A source that cannot be read or converted (as import_assets reports it), a file whose
	// references could not be read, a folder that cannot be listed, a game install that does
	// not mount.
	std::vector<Diagnostic> diagnostics;
	// The cap on the files planned stopped the plan: files of the selection past it, or
	// dependencies, are not in it.
	bool truncated = false;
};

inline constexpr size_t kImportPlanFileCap = 1000;

// The plan of importing `sources` into the project (its files `scan`, resolved by `graph`),
// with the files they need when `with_dependencies`, looked for in the game install at
// `retail_directory` too ("" for none), `file_cap` files at most.
ImportPlan plan_import(const std::vector<ImportSource> &sources, bool with_dependencies, const ProjectPaths &paths,
                       const ProjectDocument &document, const AssetScan &scan, const AssetGraph &graph,
                       const std::string &retail_directory, size_t file_cap = kImportPlanFileCap);

// Whether two plans come to the same import (an import checks the plan it shows against the
// one it makes again before it writes): the same rows in the same order, each the same file
// from the same place (its source) to the same destination, of the same kind, found or not
// found alike, with the same problem; and both stopped by the cap, or neither. What wanted a
// file first, where else it was found and the findings do not decide what is written.
bool same_import(const ImportPlan &a, const ImportPlan &b);

} // namespace opennova::editor
