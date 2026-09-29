#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_registry.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/diagnostic.h>
#include <editor/model/document.h>
#include <editor/model/value.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

struct SessionView;

// The asset graph (ADR 0046 d10, S7): every cross-reference the project's files carry,
// extracted from the engine's own parsed records, as typed edges from a source file
// (and the record and field inside it) to a target name in a namespace, the
// ReferenceKind. A file target (a model, a texture, a font, a menu, a string table, a
// terrain, an environment, a sound bank) resolves against the scan by logical name and
// kind; a symbol target (a weapon or ammo name, an item id, a string id, a style variable,
// a particle effect, a menu screen or window by NAME) against the symbols the files
// define. The graph is the one
// resolver: the inspector's badges, the Problems rows, the pickers, "find references"
// and the rename transaction all read it. Files whose size and modified time did not
// change keep their last extraction; an open document stands in for its file.

struct GraphEdge {
	std::string source;   // the referencing file, project-relative
	std::string record;   // the record inside it, every name from the row down ("" = the file itself)
	std::string locator;  // a document record's place, stable across a reload (Document::locator)
	NodeAddress address;  // the record's address in the document the edge was read from
	std::string field;    // the field id, or a native format's slot ("material[2].texture[0]")
	ReferenceKind kind = ReferenceKind::None;
	std::string value;    // the reference as written
	std::string target;   // what is looked up: the value after a style variable resolved, normalized
	// A string id's table and section ("GAMETEXT.BIN/WepDes", a menu's "MENUTXT.BIN/menu";
	// "/menu" names no table: the window reads none); "" = any table.
	std::string scope;
	bool rewritable = false; // the source is a document type and the field is in its schema
	// On a style variable edge: what the variable's value must name there (a font, a menu
	// texture), None where it stands for a colour.
	ReferenceKind through = ReferenceKind::None;
	// On a model's texture row: the row's type, by which its loader picks the one file the
	// name loads (FieldSchema::material_type, reference_file_candidates); -1 elsewhere.
	int material_type = -1;
};

// A name a file defines that other files may reference: a document's field whose field_on
// sets FieldSchema::defines (refined by Document::refine_symbol), or a native file's name.
struct GraphSymbol {
	ReferenceKind kind = ReferenceKind::None;
	std::string name;    // normalized
	std::string display; // as defined
	std::string value;   // a style variable's value
	std::string file;    // the defining file, project-relative
	std::string record;  // the defining record, every name from the row down ("" = the file itself)
	std::string locator; // a document record's place, stable across a reload (Document::locator)
	NodeAddress address; // the defining record in the document it was read from
	std::string field;   // the field that defines it ("" for a native file's)
	// Where a lookup finds it: a string id's "TABLE.BIN/Section"; a menu screen's menu file
	// ("MAIN.MNU"); a menu window's menu file and screen ("MAIN.MNU/STARTUP"); a user point's
	// model file ("GUN.3DI").
	std::string scope;
	// Defined, but not what the game reads: a style variable of a stylesheet the game does
	// not load, one defined again later in its file (the game reads the last), one menu_style.mns
	// defines and brand.mns defines again, one on a line after the place the game stops reading
	// its file; a string id in a later section of a name the table already has (a lookup reads
	// the first [orig: TextResource_FindEntryBySectionAndKey @ 0x75d250]); a menu screen or
	// window no by-name lookup returns (MnuDocument::lookup_names: an earlier screen of a name,
	// a later window of a name, one under a window with no NAME or on a screen a later one
	// shadows); a model's user point past the first 16 an item's lookup scans.
	bool inert = false;
	// Why no lookup finds it, in a few words (the picker's, the find's), when it is inert.
	std::string inert_reason;
};

// A name a reference may be given (the picker's rows, AssetGraph::choices): what a pick sets
// the field to, where it is defined, and what the reference would then be.
struct ReferenceChoice {
	std::string name; // a symbol as defined, a style variable as its %NAME%, a file by its logical name
	ReferenceKind kind = ReferenceKind::None; // what it names: the reference's kind, or one it also offers
	std::string file;   // the defining file, or the file itself (project-relative)
	std::string record; // the defining record ("" for a file)
	// The reference set to it, in its scope: Present, or Missing where the lookup would not
	// reach it there (a string id of another section, a menu texture whose loader reads
	// another file of the name).
	ReferenceStatus status = ReferenceStatus::Present;
	bool inert = false;  // defined where no lookup of the game finds it
	std::string reason;  // why, when inert (GraphSymbol::inert_reason)
};

// A file or a symbol whose name holds a searched text (AssetGraph::search), with how many uses
// it has.
struct GraphSearchHit {
	const GraphSymbol *symbol = nullptr; // the symbol; null for a file
	std::string name;   // the file's logical name, or the symbol as defined
	std::string file;   // the file, or the file defining the symbol (project-relative)
	size_t usages = 0;  // the file's usages (usages_of), or the symbol's users (users_of)
};

struct GraphStats {
	size_t files_extracted = 0; // re-read on the last update
	size_t files_reused = 0;    // unchanged since the previous update
	size_t files_failed = 0;    // unreadable on the last update
};

// What one file references and defines.
struct Extracted {
	std::vector<GraphEdge> edges;
	std::vector<GraphSymbol> symbols;
};

class AssetGraph {
public:
	// Rebuild over the scan; the open documents stand in for their files. An update that finds
	// the files as they were (their rows in the scan's order, and what each file the graph reads
	// references and defines) changes nothing: the edges and symbols stay where they were.
	void update(const ProjectPaths &paths, const ProjectDocument &project, const AssetScan &scan,
	            const std::vector<std::shared_ptr<const Document>> &open);
	// Moves each time an update changes what the graph holds, and only then: while it stands,
	// every edge and symbol the graph handed out is where it was (a cache may keep them).
	uint64_t generation() const { return generation_; }

	const std::vector<GraphEdge> &edges() const { return edges_; }
	const std::vector<GraphSymbol> &symbols() const { return symbols_; }
	// Every symbol of `kind`, inert ones too, in the order the files define them.
	std::vector<const GraphSymbol *> symbols_of_kind(ReferenceKind kind) const;
	const GraphStats &stats() const { return stats_; }

	// The edges out of a file, by project-relative path or logical name.
	std::vector<const GraphEdge *> references_of(const std::string &file) const;
	// The edges that resolve to a file (any kind that names it).
	std::vector<const GraphEdge *> referrers_of_file(const std::string &logical_name) const;
	// The edges into a symbol of `kind` (indexed by kind and name): with `scope`, the symbol's
	// own, only the edges whose scope reads it there (scope_matches).
	std::vector<const GraphEdge *> referrers_of(ReferenceKind kind, const std::string &name,
	                                            const std::string &scope = std::string()) const;
	// Who uses a file (by project-relative path or logical name): the edges that resolve to it
	// (referrers_of_file), then the edges into each symbol it defines that a lookup finds (a
	// string table's ids, a catalog's names, a stylesheet's variables the game reads, a
	// menu's screens and windows, a model's user points), each edge once.
	std::vector<const GraphEdge *> usages_of(const std::string &file) const;
	// The edges whose name reaches exactly this definition (resolve_symbol returns it: never a
	// definition of the name in another scope, nor one no lookup finds, which has none).
	std::vector<const GraphEdge *> users_of(const GraphSymbol &symbol) const;
	// Find in the project: the files whose logical name, and the symbols whose name as defined,
	// holds `text` (ASCII letters without case), files first (by name), then symbols in the order
	// the files define them, each with its usage count. None for an empty text.
	std::vector<GraphSearchHit> search(const std::string &text) const;
	// The symbol a document's field defines, by its file, its record's locator and the field; null
	// for none (a rename everywhere names its symbol so: the graph may have been rebuilt since).
	const GraphSymbol *symbol_at(const std::string &file, const std::string &locator, const std::string &field) const;
	// The symbols a record defines (its file and record, as the edges name them): indexed, a
	// string table defining one per key.
	std::vector<const GraphSymbol *> symbols_of(const std::string &file, const std::string &record) const;

	// Where a name of `kind` resolves. `file_out` receives the project-relative path of
	// the file it names or the file defining the symbol. A file resolves to the file its
	// kind's loader reads (reference_file_candidates), a model's texture row by its
	// `material_type`.
	ReferenceStatus resolve(ReferenceKind kind, const std::string &name, const std::string &scope = std::string(),
	                        std::string *file_out = nullptr, int material_type = -1) const;
	// Where an edge resolves: its value as written (the name the loader is handed: a model
	// texture's rule reads its case), in its scope, by its material type.
	ReferenceStatus resolve(const GraphEdge &edge, std::string *file_out = nullptr) const;
	// The one definition a name of a symbol kind reaches, as the game's lookup finds it: a style
	// variable's binding (style_binding), else the first symbol of the name, as the kind
	// compares names, that `scope` matches (scope_matches: a string id in its table and
	// section, a window on its screen) and a lookup finds; null for none, and for a file kind.
	const GraphSymbol *resolve_symbol(ReferenceKind kind, const std::string &name,
	                                  const std::string &scope = std::string()) const;
	// A menu-style %NAME% through the stylesheets the game reads: its value, or the input
	// unchanged.
	std::string resolve_style(const std::string &value) const;
	// The definition the game reads for a style variable ("NAME" or "%NAME%"): brand.mns
	// over menu_style.mns, the last definition in each [orig: Menu_InitShellResources @
	// 0x552500]; null when neither defines it where the game reads.
	const GraphSymbol *style_binding(const std::string &name) const;
	// The names a reference of `kind` in `scope` may be given (the pickers), each with where it
	// is defined and what the reference would then be (resolve, by `material_type` for a
	// model's texture row): the project's files its loader can read; the symbols of the kind,
	// only those the scope matches where the kind's picker narrows (scope_matches: a string id's
	// table and section, the screens of a SCREEN ACTION's file, the windows of the acting
	// window's screen, the points of the item's model; a scope of none, every one), each name once, as the first definition a lookup finds; then, marked
	// inert with the reason, the names defined only where no lookup finds them; a style
	// variable as its %NAME%, the definition the game reads.
	std::vector<ReferenceChoice> choices(ReferenceKind kind, const std::string &scope = std::string(),
	                                     int material_type = -1) const;

	// Every verifiable edge whose target is missing, and the same as findings
	// ("reference.missing": an error, or a warning for what the game tolerates: a style
	// variable, a string id, a menu's sound bank or credits file, the screen or window an
	// ACTION names; a file a menu names through a style variable is reported
	// once, where the stylesheet names it, and never for a stylesheet value the game does
	// not read), then a warning for each file of a native kind the graph
	// could not read ("graph.unreadable": its references are not checked; a document
	// type's own validation reports a file of its kinds that does not load).
	std::vector<const GraphEdge *> missing() const;
	std::vector<Diagnostic> diagnostics() const;
	// The "reference.missing" finding about an edge nothing resolves (diagnostics' row, which a
	// picker builds for a field's value too, for the fixes Problems would offer).
	Diagnostic missing_finding(const GraphEdge &edge) const;

	// Whether the project has a file of this name (its logical name, or a path's file name).
	bool has_file(const std::string &name) const;
	// Every symbol of `kind` named `name` (compared as the kind compares names), inert ones
	// too, in the order the files define them.
	std::vector<const GraphSymbol *> symbols_named(ReferenceKind kind, const std::string &name) const;

private:
	struct Extraction {
		uint64_t size = 0;
		int64_t modified = 0;
		uint64_t identity = 0; // the open document's instance
		uint64_t revision = 0;
		bool open = false;
		bool ok = true;
		Diagnostic failure; // the graph.unreadable warning (no code when a validator reports it)
		Extracted content;
	};
	void assemble(const AssetScan &scan);
	std::string resolved_target(const GraphEdge &edge) const;
	bool same_files(const AssetScan &scan) const;

	std::map<std::string, Extraction> cache_; // by project-relative path
	std::vector<GraphEdge> edges_;
	std::vector<GraphSymbol> symbols_;
	std::map<std::string, size_t> style_bindings_; // upper-case name -> symbols_ index of what the game reads
	// (name, kind) of each binding's value edges (a font or texture a variable names).
	std::set<std::pair<std::string, ReferenceKind>> style_value_edges_;
	// (file, locator) of each style variable the game does not read (GraphSymbol::inert):
	// the file its value names is never loaded through it.
	std::set<std::pair<std::string, std::string>> inert_style_values_;
	struct FileRow { std::string path; std::string logical_name; AssetKind kind = AssetKind::Unknown; };
	std::map<std::string, FileRow> files_; // normalized logical name -> the file
	// The scan's files in its order, as the graph was last assembled over them.
	std::vector<FileRow> scanned_;
	uint64_t generation_ = 0;
	std::multimap<std::string, size_t> symbol_index_; // kind token + '\n' + name -> symbols_ index
	std::map<std::pair<std::string, std::string>, std::vector<size_t>> record_symbols_; // (file, record) -> symbols_ indexes
	std::map<std::string, std::vector<size_t>> file_symbols_; // file -> symbols_ indexes
	// The file edges that resolve to each file (by its path), made on the first ask after an
	// update (resolving every file edge once), which referrers_of_file reads.
	mutable std::shared_ptr<std::map<std::string, std::vector<size_t>>> file_users_;
	std::multimap<std::string, size_t> edge_index_; // kind token + '\n' + target -> edges_ index
	GraphStats stats_;
};

// The scope a menu's string id resolves in: the "menu" section of the table its window
// reads (menu::window_text_rsrc; "" = none, so the scope matches no symbol and the game
// shows the id) [orig: CUIStringTable_LookupString @ 0x6527c0].
std::string menu_text_scope(const std::string &table);
// True when a string id a table defines in section `symbol_scope` ("TABLE.BIN/Section")
// is the one a reference scoped `reference_scope` reads: the same table (the flat file
// name, without case), and the same section when the reference names one ("" = any
// table).
bool scope_matches(const std::string &symbol_scope, const std::string &reference_scope);
// The scope a menu screen resolves in: its menu file's flat name, upper case ("MAIN.MNU").
std::string menu_screen_scope(const std::string &menu_file);
// The scope a menu window resolves in: its menu file and its screen's NAME, upper case
// ("MAIN.MNU/STARTUP"). Screens of one name share it: the lookup finds the later one.
std::string menu_window_scope(const std::string &menu_file, const std::string &screen);

// The file names a reference of a file kind to `name` loads, in the order the game probes
// them: the file is the first of them `exists` has (the graph asks the project, as the
// kind; a fix the game install; the import plan each place it looks). A menu texture and a
// model's texture row (`material_type` >= 0) name the one file their loader opens, picked
// by what `exists` has: a menu texture by its extension, a .tga the files lack loading its
// .dds (menu::menu_texture_source); a texture row by the row's type, as the renderer's
// loaders pick (renderer/material_texture.h; a loose file never first, the project's files
// being packed as the game mounts them), none when that loader opens no file. A font names
// the .fnt its name loads (menu::menu_font_file). A texture of anything but a model (a
// particle's, a sky map's, a def's) names what the runtime's texture lookup probes: the
// name as written, then its stem with each texture extension (texture_candidate_filenames).
// Any other kind names the name as written, then with each extension the kind's loader
// appends. None for a symbol, an unverified kind or an empty name. Each rule is the kind's row
// (reference_kinds: its file_names, else its extensions).
std::vector<std::string> reference_file_candidates(ReferenceKind kind, const std::string &name, int material_type,
                                                   const std::function<bool(const std::string &)> &exists);
// Whether a project file, of the kind the scan gives it, can be what a reference of `kind`
// loads: a file of the reference's file kind, and for a model's chunk row (runtime types 16
// to 18) one the scan could not type by its name too, since that loader reads the name as
// written as a chunk container whatever it is called (the scan reads no such file's bytes).
bool file_serves_reference(AssetKind file, ReferenceKind kind, int material_type);

// The reference a field's value makes (graph/extractors.cpp): the kind the graph resolves
// it in (a def's game-text kinds become string ids in their table and section), the name
// and the scope. False when the value references nothing (empty, NONE, NULL, a literal
// where a style variable could stand).
bool reference_target(const FieldSchema &field, const Value &value, ReferenceKind &kind, std::string &name,
                      std::string &scope);
// Where "Go to" on a use goes: the record of the edge's file that makes it (its locator, the
// field shown), opened when the editor edits the file's kind, else the file shown in Files.
ReferenceTarget usage_target(const GraphEdge &edge, const SessionView &view);
// Where "Go to" on a definition goes: the record that defines a symbol (at its defining field),
// and a file itself; opened, or shown in Files, as usage_target.
ReferenceTarget symbol_target(const GraphSymbol &symbol, const SessionView &view);
ReferenceTarget file_target(const std::string &file, const SessionView &view);

// What a document references and defines, through its schema: every field's reference as it
// applies to its record (Document::field_on), and a symbol for every field field_on says
// defines one, as the type refines it (Document::refine_symbol).
void extract_from_document(const Document &document, Extracted &out);
// What a file references and defines, from its bytes as stored (decoded as the game's
// loader decodes them): a document type's through its document (Document::load_bytes), a
// native kind's through the engine's parser. `name` is what the edges and symbols name
// the file by (the project-relative path in the graph). True with nothing for a kind the
// graph does not read.
bool extract_from_bytes(const std::string &name, AssetKind kind, const std::vector<uint8_t> &bytes,
                        const std::string &game, Extracted &out, Diagnostic &error);
// A project file read, then extract_from_bytes.
bool extract_from_asset(const ProjectPaths &paths, const ProjectDocument &project, const AssetEntry &asset,
                        Extracted &out, Diagnostic &error);
// True when files of this kind carry references or symbols the graph reads.
bool graph_reads_kind(AssetKind kind);

} // namespace opennova::editor
