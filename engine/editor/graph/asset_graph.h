#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_registry.h>
#include <editor/graph/graph_edge.h>
#include <editor/graph/graph_index.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/diagnostic.h>
#include <editor/model/document.h>
#include <editor/model/text_document.h>
#include <editor/model/value.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

class GraphLayer;

// The asset graph (ADR 0046 d10, S7): every cross-reference the project's files carry,
// extracted from the engine's own parsed records, as typed edges from a source file
// (and the record and field inside it) to a target name in a namespace, the
// ReferenceKind. A file target (a model, a texture, a font, a menu, a string table, a
// terrain, an environment, a sound bank) resolves against the scan by logical name and
// kind; a symbol target (a weapon or ammo name, an item id, a string id, a style variable,
// a particle effect, a menu screen or window by NAME) against the symbols the files
// define. The graph is the one
// resolver: the inspector's badges, the Problems rows, the pickers, "find references"
// and the rename transaction all read it. Each project file has a slot (graph_index.h) holding
// what the graph read from it and how each of its edges resolves; an update reads again only the
// files whose size and last write, or open document, changed (an open document stands in for its
// file) and patches only the slots whose reading changed, resolving again only what those changes
// reach (S13 D3). A base layer (graph_layer.h, a mounted install's files) answers the names the
// project does not have.

// A name a reference may be given (the picker's rows, AssetGraph::choices): what a pick sets
// the field to, where it is defined, and what the reference would then be.
struct ReferenceChoice {
	std::string name; // a symbol as defined, a style variable as its %NAME%, a file by its logical name
	ReferenceKind kind = ReferenceKind::None; // what it names: the reference's kind, or one it also offers
	std::string file;   // the defining file, or the file itself (a base layer's by its name)
	std::string record; // the defining record ("" for a file)
	// What the picker shows beside the name and filters by too: a record set's record by its own
	// name (a CTRL register's NAME), "" for the rest.
	std::string label;
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

// What the last update (or set_base) did.
struct GraphStats {
	size_t files_extracted = 0; // read again
	size_t files_reused = 0;    // unchanged since the previous update
	size_t files_failed = 0;    // unreadable
	size_t files_patched = 0;   // slots added, gone or read differently (GraphUpdate::files)
	size_t edges_resolved = 0;  // edges resolved again
	size_t findings_made = 0;   // missing edges' findings made (missing_finding)
};

// A closed file of the project read ahead of an update (S13 A3): a validation stepped a file at a
// time reads the files the update would read a step at a time (AssetGraph::files_to_read,
// read_file), and the update takes each reading whose file is still as the scan lists it (the same
// name, kind, size and last write) where it would read the file itself. What extract_from_asset
// made of it.
struct GraphReading {
	std::string logical_name;
	AssetKind kind = AssetKind::Unknown;
	uint64_t size = 0;
	int64_t modified = 0;
	bool ok = false;
	Extracted content;
	Diagnostic error;
};
using GraphReadings = std::map<std::string, GraphReading>; // by project-relative path

// What an update changed (AssetGraph::update, set_base). `changed`: what the graph holds moved
// (a new generation). `files`: the files whose slots were added, removed or read differently,
// project-relative. `bindings`: the style variables whose binding (the definition the game reads:
// its file or its value, or the kinds of the files its own value edges name) changed, upper case
// as the graph keys them. `file_set`: a file added, gone, or of another kind.
struct GraphUpdate {
	bool changed = false;
	std::vector<std::string> files;
	std::vector<std::string> bindings;
	bool file_set = false;
};

class AssetGraph {
public:
	// Brings the graph to the scan; the open documents stand in for their files. Each file of the
	// scan has a slot: one whose row and reading are as they were keeps everything, a file read
	// again with the same result (a rewrite of the same bytes) included. A slot whose reading
	// changed is patched (its content out of the indexes and in again) and what it reaches is
	// resolved again: its own edges; the edges into a symbol name whose definitions changed; the
	// style variables' edges, and the files named through them, when a binding changed; every
	// File edge (and a screen's, whose lookup asks for its menu file) when the file set changed;
	// and, when the project's files come to hide a base layer's file of their name or no longer
	// do, the edges into the names that file defines. A missing edge's finding is worded as it
	// resolves, and again only when what its words read changed (the definitions of a style
	// variable's name; which files the project has, for a kind whose row says its words read
	// them). The generation moves exactly when what the graph holds changed. `read_ahead`: the
	// closed files read before (S13 A3), each taken (moved out) where the scan lists it as it was
	// read, the others read here.
	GraphUpdate update(const ProjectPaths &paths, const ProjectDocument &project,
			const AssetScan &scan, const std::vector<std::shared_ptr<const DocumentBase>> &open,
			GraphReadings *read_ahead = nullptr);
	// The closed files an update over `scan` would read (S13 A3): those of a kind the graph reads,
	// not open as a record document, whose slot holds no reading of them as the scan lists them (new,
	// of another name or kind, or of another size or last write); in the scan's order.
	std::vector<const AssetEntry *> files_to_read(const AssetScan &scan,
			const std::vector<std::shared_ptr<const DocumentBase>> &open) const;
	// A file read as an update reads it (extract_from_asset), for a later update to take.
	static GraphReading read_file(const ProjectPaths &paths, const ProjectDocument &project, const AssetEntry &asset);
	// A value of a process-wide counter: taken anew each time an update changes what the graph
	// holds, and by every graph made, copied, assigned or cleared, so no two graphs and no two
	// states of one graph share it. While it stands, every edge and symbol the graph handed out
	// is where it was (a cache may keep them).
	uint64_t generation() const { return generation_.value; }
	// Every file, edge and symbol gone (a project closed), its base layer too, under a new
	// generation.
	void clear();

	// The base layer (ADR 0046 d10: a read-only dependency mount; project assets win): a lookup by
	// name tries the project, then the base, whose files the project has a file of the name of
	// are hidden; choices offers the base's names after the project's (the names a lookup finds
	// before the others); the base makes no edge and no finding. Every edge is resolved again,
	// under a new generation; null takes it away.
	GraphUpdate set_base(std::shared_ptr<const GraphLayer> base);
	const std::shared_ptr<const GraphLayer> &base() const { return base_; }

	// The project's edges and symbols, in the files' order (the scan's), then each file's own.
	void for_each_edge(const std::function<void(const GraphEdge &)> &visit) const;
	void for_each_symbol(const std::function<void(const GraphSymbol &)> &visit) const;
	size_t edge_count() const { return index_.edge_count(); }
	size_t symbol_count() const { return index_.symbol_count(); }
	// Every symbol of `kind`, inert ones too, in the order the files define them.
	std::vector<const GraphSymbol *> symbols_of_kind(ReferenceKind kind) const;
	const GraphStats &stats() const { return stats_; }
	// The slots and their indexes, read-only (a test comparing two graphs entry by entry).
	const GraphIndex &index() const { return index_; }

	// The three queries of a file below name it alike: the project's file at that project-relative
	// path, as the scan lists it, else, for a name with no folder, the file the name resolves to
	// (of two files of one name the first by path, else the base layer's). A name that is the path
	// of a file at the project's root names that file, even where another file of the name comes
	// first. The edges out of a file (none for a base layer's):
	std::vector<const GraphEdge *> references_of(const std::string &file) const;
	// The edges that resolve to a file (any kind that names it).
	std::vector<const GraphEdge *> referrers_of_file(const std::string &file) const;
	// The edges into a symbol of `kind` (indexed by kind and name): with `scope`, the symbol's
	// own, only the edges whose scope reads it there (scope_matches); a record of a file's record
	// set by its index (a Record kind, S13 D8) in the file `scope` names, which only that file's
	// edges name.
	std::vector<const GraphEdge *> referrers_of(ReferenceKind kind, const std::string &name,
	                                            const std::string &scope = std::string()) const;
	// Who uses a file: the edges that resolve to it (referrers_of_file), then the edges into each
	// symbol it defines that a lookup finds (a string table's ids, a catalog's names, a
	// stylesheet's variables the game reads, a menu's screens and windows, a model's user points),
	// each edge once. A file's own records by index (its record sets) are no use of it: their users
	// are its own edges.
	std::vector<const GraphEdge *> usages_of(const std::string &file) const;
	// The edges whose name reaches exactly this definition (resolve_symbol returns it: never a
	// definition of the name in another scope, nor one no lookup finds, which has none).
	std::vector<const GraphEdge *> users_of(const GraphSymbol &symbol) const;
	// Find in the project: the files whose logical name, and the symbols whose name as defined,
	// holds `text` (ASCII letters without case), files first (by name), then symbols in the order
	// the files define them, each with its usage count; a record set's records, named by an index
	// and no name, are not searched. None for an empty text.
	std::vector<GraphSearchHit> search(const std::string &text) const;
	// The symbol a document's field defines, by its file, its record's locator and the field; null
	// for none (a rename everywhere names its symbol so: the graph may have been rebuilt since).
	const GraphSymbol *symbol_at(const std::string &file, const std::string &locator, const std::string &field) const;
	// The symbols a record defines (its file and record, as the edges name them): indexed, a
	// string table defining one per key.
	std::vector<const GraphSymbol *> symbols_of(const std::string &file, const std::string &record) const;
	// The definitions the graph read from `document` when its slot is current for it (the same
	// instance at the same revision: find_definition's reuse), each with its inert as the
	// document's own lookup makes it (a stylesheet's earlier definition of a name), not the game's
	// reading of the project; false, visiting nothing, when the slot is not.
	bool for_each_definition(const Document &document,
			const std::function<void(const GraphSymbol &symbol, bool inert)> &visit) const;

	// Where a name of `kind` resolves. `file_out` receives the project-relative path of
	// the file it names or the file defining the symbol (a base layer's file by its name). A file
	// resolves to the file its kind's loader reads (reference_file_candidates, by `loader_arg`: a
	// model's texture row's type).
	ReferenceStatus resolve(ReferenceKind kind, const std::string &name, const std::string &scope = std::string(),
	                        std::string *file_out = nullptr, int32_t loader_arg = -1) const;
	// Where an edge resolves: its value as written (the name the loader is handed: a model
	// texture's rule reads its case), in its scope, by its loader's argument; for a symbol kind's
	// edge with a fallback (GraphEdge::fallback), the fallback where the value finds nothing; then
	// each scope the edge tries after its own (GraphEdge::scopes_after), both names in each.
	ReferenceStatus resolve(const GraphEdge &edge, std::string *file_out = nullptr) const;
	// The definition a symbol kind's edge reaches (resolve_symbol): its value's, else its
	// fallback's, in the first of its scopes a lookup finds either in; null for none.
	const GraphSymbol *symbol_reached(const GraphEdge &edge) const;
	// The name of the two a symbol kind's edge reaches (its value, or its fallback where only that
	// one is defined in the first scope either is found in); the value where neither is.
	const std::string &reached_name(const GraphEdge &edge) const;
	// The edges of `kind` with a fallback (GraphEdge::fallback) that name `name` as their value or
	// their fallback, in the files' order: what a rename to `name` checks (a use it would take over).
	std::vector<const GraphEdge *> edges_naming(ReferenceKind kind, const std::string &name) const;
	// The one definition a name of a symbol kind reaches, as the game's lookup finds it: a style
	// variable's binding (style_binding), else the first symbol of the name, as the kind
	// compares names, that `scope` matches (scope_matches: a string id in its table and
	// section, a window on its screen) and a lookup finds, the project's before the base's; for a
	// Record kind the record at that index of the file `scope` names (its record set: the project's
	// file's own); null for none, and for a file kind.
	const GraphSymbol *resolve_symbol(ReferenceKind kind, const std::string &name,
	                                  const std::string &scope = std::string()) const;
	// A menu-style %NAME% through the stylesheets the game reads: its value, or the input
	// unchanged.
	std::string resolve_style(const std::string &value) const;
	// The definition the game reads for a style variable ("NAME" or "%NAME%"): brand.mns
	// over menu_style.mns, the last definition in each [orig: Menu_InitShellResources @
	// 0x552500], each the project's file of that name, else the base's; null when neither defines
	// it where the game reads.
	const GraphSymbol *style_binding(const std::string &name) const;
	// The names a reference of `kind` in `scope` may be given (the pickers), each with where it
	// is defined and what the reference would then be (resolve, by `loader_arg`: a model's
	// texture row's type): the project's files its loader can read; the symbols of the kind,
	// only those the scope matches where the kind's picker narrows (scope_matches: a string id's
	// table and section, the screens of a SCREEN ACTION's file, the windows of the acting
	// window's screen, the points of the item's model; a scope of none, every one), each name
	// once, as the first definition a lookup finds; then, marked inert with the reason, the names
	// defined only where no lookup finds them; a style variable as its %NAME%, the definition the
	// game reads. In each of the two the project's names, then the base layer's the same way, the
	// names offered before left out: a name the project defines only where no lookup finds it is
	// the base's live definition when the base has one. A base layer's style variable is inert
	// unless it is the binding. A Record kind's: the records of the file `scope` names that its
	// collection holds, in their order, each by its index.
	std::vector<ReferenceChoice> choices(ReferenceKind kind, const std::string &scope = std::string(),
	                                     int32_t loader_arg = -1) const;

	// Every verifiable edge whose target is missing, in the files' order, and the same as findings
	// ("reference.missing": an error, or a warning for what the game tolerates: a style
	// variable, a string id, a menu's sound bank or credits file, the screen or window an
	// ACTION names; a file a menu names through a style variable is reported
	// once, where the stylesheet names it, and never for a stylesheet value the game does
	// not read; never a kind whose row has no missing message, a Record reference, which its
	// file's own validation reports), then a warning for each file of a native kind the graph
	// could not read ("graph.unreadable": its references are not checked; a document
	// type's own validation reports a file of its kinds that does not load), by path. Both are
	// kept as the update resolves the edges (each finding with its edge), never made again by a
	// call; the list is gathered again only when a finding in it, or a failure, moved.
	std::vector<const GraphEdge *> missing() const;
	size_t missing_count() const { return index_.missing().size(); }
	const std::vector<Diagnostic> &diagnostics() const { return diagnostics_; }
	// The "reference.missing" finding about an edge nothing resolves (diagnostics' row, which a
	// picker builds for a field's value too, for the fixes Problems would offer).
	Diagnostic missing_finding(const GraphEdge &edge) const;

	// Whether the project, or its base layer, has a file of this name (its logical name, or a
	// path's file name).
	bool has_file(const std::string &name) const;
	// Every symbol of `kind` named `name` (compared as the kind compares names), inert ones
	// too, in the order the files define them, the project's then the base's; of a Record kind,
	// the record at that index of the file `scope` names.
	std::vector<const GraphSymbol *> symbols_named(ReferenceKind kind, const std::string &name,
	                                               const std::string &scope = std::string()) const;

private:
	using Ref = GraphIndex::Ref;
	// What an update's reading of the files changed, which the resolution that follows reaches.
	struct Patch;
	// The definition the game reads of a style variable: the project's stylesheet's, or the
	// base layer's; its file and value, and the kinds of the files its own value edges name (a
	// menu's reference to a file through it is reported where the stylesheet names the file). Two
	// bindings of a name are the same to what they reach when those are: a line moved in its file
	// changes no edge naming the variable.
	struct Binding {
		bool base = false;
		Ref symbol;
		std::string file, value;
		std::vector<ReferenceKind> value_kinds;
		bool same(const Binding &other) const {
			return base == other.base && file == other.file && value == other.value &&
					value_kinds == other.value_kinds;
		}
	};

	// A slot's reading replaced (the slot patched), or left as it was when it reads the same.
	void take(uint32_t id, Extracted content, bool ok, const Diagnostic &failure, Patch &patch);
	// A slot gone (its file no longer listed, or another file at its path).
	void drop(uint32_t id, Patch &patch);
	// The patch resolved: the bindings, the style variables' inert, every edge the patch reaches,
	// the findings.
	void resolve_patch(Patch &patch, GraphUpdate &out);
	// The bindings made again from the shell's stylesheets; the names whose binding changed.
	std::vector<std::string> rebind();
	// A style variable's inert as the game reads the project (the bindings); true when it moved.
	bool derive(uint32_t id, uint32_t symbol);
	// Whether no lookup of the game finds a definition: a style variable's unless it is the binding
	// (a base layer's keeps only its own file's inert), any other's as the graph keeps it; and why
	// a style variable's is not read (its own file's reason, else the stylesheets the game reads).
	bool unread(const GraphSymbol &symbol) const;
	std::string unread_reason(const GraphSymbol &symbol, bool own_inert,
	                          const std::string &own_reason) const;
	// One edge resolved: its target, its status, the file it loads, whether it is missing and its
	// finding, each entry of the index that changed moved. True when the diagnostics moved: the
	// edge, resolved before, went in or out of the missing, or its finding changed.
	bool resolve_edge(Ref ref);
	bool counts_missing(const GraphSlot &slot, const GraphEdge &edge, const std::string &target,
	                    ReferenceStatus status) const;
	// A missing edge's finding worded again; true when it changed.
	bool reword(Ref ref);
	// The diagnostics listed again: each missing edge's finding, then the failures by path.
	void list_diagnostics();
	std::string resolved_target(const GraphEdge &edge) const;
	// The file a name resolves to: the project's first of it by path, else the base's.
	const GraphSlot *file_named(const std::string &key) const;
	// The file a query names (references_of): the project's at the path, else by the name.
	const GraphSlot *named_file(const std::string &file) const;
	// Whether a base layer's file shows: the project has no file of its name.
	bool base_file_shows(const GraphSlot &slot) const;
	// The generation: the counter's next value on every construction, copy and assignment (a
	// copy holds edges and symbols of its own); update() takes another when it changes anything.
	static uint64_t next_generation();
	struct Generation {
		uint64_t value = AssetGraph::next_generation();
		Generation() = default;
		Generation(const Generation &) {}
		Generation &operator=(const Generation &) {
			value = AssetGraph::next_generation();
			return *this;
		}
	};

	GraphIndex index_;
	std::shared_ptr<const GraphLayer> base_;
	std::map<std::string, Binding> bindings_; // upper-case name -> the definition the game reads
	std::vector<Diagnostic> diagnostics_;
	GraphStats stats_;
	Generation generation_;
};

// True when a string id a table defines in section `symbol_scope` ("TABLE.BIN/Section")
// is the one a reference scoped `reference_scope` reads: the same table (the flat file
// name, without case), and the same section when the reference names one ("" = any
// table).
bool scope_matches(const std::string &symbol_scope, const std::string &reference_scope);

// The file names a reference of a file kind to `name` loads, in the order the game probes
// them: the file is the first of them `exists` has (the graph asks the project, as the
// kind; a fix the game install; the import plan each place it looks). A menu texture and a
// model's texture row (`loader_arg` >= 0, its type) name the one file their loader opens,
// picked by what `exists` has: a menu texture by its extension, a .tga the files lack loading its
// .dds (menu::menu_texture_source); a texture row by the row's type, as the renderer's
// loaders pick (renderer/material_texture.h; a loose file never first, the project's files
// being packed as the game mounts them), none when that loader opens no file. A font names
// the .fnt its name loads (menu::menu_font_file). A texture of anything but a model (a
// particle's, a sky map's, a def's) names what the runtime's texture lookup probes: the
// name as written, then its stem with each texture extension (texture_candidate_filenames).
// Any other kind names the name as written, then with each extension the kind's loader
// appends. None for a symbol, an unverified kind or an empty name. Each rule is the kind's row
// (reference_kinds: its file_names, else its extensions).
std::vector<std::string> reference_file_candidates(ReferenceKind kind, const std::string &name, int32_t loader_arg,
                                                   const std::function<bool(const std::string &)> &exists);
// Whether a project file, of the kind the scan gives it, can be what a reference of `kind`
// loads: a file of the reference's file kind, and for a model's chunk row (runtime types 16
// to 18, a texture row's loader_arg) one the scan could not type by its name too, since that
// loader reads the name as written as a chunk container whatever it is called (the scan reads
// no such file's bytes).
bool file_serves_reference(AssetKind file, ReferenceKind kind, int32_t loader_arg);

// The kind a field's value references: the field's own reference, else, where a whole %NAME%
// stands for the stylesheet variable's value (FieldUse::variable_through: a menu's text), the
// variable (StyleVar); None for a value of such a field that is no whole %NAME%.
ReferenceKind value_reference(const FieldUse &field, const Value &value);
// The reference a field's value makes (graph/extractors.cpp), the field as it applies to its
// record: the kind the graph resolves it in (a def's game-text kinds become string ids in
// their table and section; a menu text's whole %NAME% the variable, value_reference), the name
// and the scope. False when the value references nothing (empty, NONE, NULL, a literal where a
// style variable could stand).
bool reference_target(const FieldUse &field, const Value &value, ReferenceKind &kind, std::string &name,
                      std::string &scope);

// What a document references and defines, through its schema: every field's reference as it
// applies to its record (Document::field_on), and a symbol for every field field_on says
// defines one, with what the type's lookup makes of it (Document::refine_symbol). And its record
// sets (S13 D8): of each collection a Record reference names (Document::targeted_collections),
// every record as a symbol of the Record kind named by its index in the file, in the file's order,
// scoped to the file and defined by no field (what an index resolves to, lists its users and
// offers the picker).
void extract_from_document(const Document &document, Extracted &out);
// What a text document references (ADR 0046 S13 D9): an edge of each name its type's references
// read from its text (DocumentType::references, a script's operands), with its span, its locator
// the span's "line:column"; nothing for a type whose text names nothing. It defines nothing.
void extract_from_text(const TextDocument &document, Extracted &out);
// What a file references and defines, from its bytes as stored (decoded as the game's
// loader decodes them): a record type's or a text type's through its document
// (DocumentBase::load_bytes), a native kind's through the engine's parser. `name` is what the edges
// and symbols name the file by (the project-relative path in the graph). True with nothing for a
// file the graph does not read (graph_reads_file), a type's whose documents hold neither records
// nor a text that names anything included.
bool extract_from_bytes(const std::string &name, AssetKind kind, const std::vector<uint8_t> &bytes,
                        const std::string &game, Extracted &out, Diagnostic &error);
// A project file read, then extract_from_bytes.
bool extract_from_asset(const ProjectPaths &paths, const ProjectDocument &project, const AssetEntry &asset,
                        Extracted &out, Diagnostic &error);
// True when files of this kind carry references or symbols the graph reads: a record type's
// (document_content), a text type's whose text names references (DocumentType::references), or
// a native extractor's kind.
bool graph_reads_kind(AssetKind kind);
// The same for one file, by its name: false for a mission's .mis, the mission editors' text
// form, which the mission document will read (the graph reads the .bms the game loads), so
// the graph skips the file and what it names goes unchecked.
bool graph_reads_file(AssetKind kind, const std::string &name);

} // namespace opennova::editor
