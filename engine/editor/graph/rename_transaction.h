#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_registry.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/diagnostic.h>
#include <editor/model/document.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>

namespace opennova::editor {

// A rename that keeps the project consistent (ADR 0046 d10, S7): every field that
// names the file is rewritten through its document type, the file moves under its new
// name, and nothing changes when any site cannot be rewritten. The plan lists the
// sites and the refusals first, so a window can show it and a test can read it.
struct RenameSite {
	std::string file;   // the referencing document, project-relative
	AssetKind kind = AssetKind::Unknown;
	std::string record;
	// The record's place in the file (Document::locator), what the commit finds it by; in a text
	// document the span's place ("line:column", TextDocument::locator).
	std::string locator;
	std::string field;  // "" in a text document, whose site is its span
	TextSpan span;      // in a text document (S13 D9): where the name is written (line 0 for none)
	std::string before;
	std::string after;
	std::string target; // the file the site names today, project-relative (the renamed file, or one of its outputs)
};

// An import output a renamed source takes along (ADR 0046 d6, S9c): its importer names
// it after the source (logo.png makes logo.pcx), so it is renamed with the source and
// every site naming it is rewritten; the next import pass makes it under the new name.
struct RenameOutput {
	std::string path;     // project-relative, under the source's old output directory
	std::string old_name; // its logical name
	std::string new_name;
};

struct RenamePlan {
	std::string path;     // the file, project-relative
	std::string old_name; // its logical name
	std::string new_name; // the new logical name
	std::string new_path; // where it will be, project-relative
	std::vector<RenameSite> sites;
	// An import source's record and outputs: the sidecar moves beside the new name and
	// the old output directory is removed once the rename commits ("" = none).
	std::string sidecar;
	std::string new_sidecar;
	std::string output_dir;
	std::string new_output_dir;
	std::vector<RenameOutput> outputs;
	// A mission's companions (ADR 0046 S14): the files the game finds by its name that the project
	// has (documents/mission_file_set.h), each renamed with it to the new base name and its own
	// extension, every site naming one rewritten; the commit moves them together.
	std::vector<RenameOutput> companions;
	// A split (ADR 0046 S18, split_texture): the file copied under the new name, not moved, and only the
	// sites of the files it was asked for rewritten to the copy; an import's output split as its source
	// copied beside it (`split_source`, project-relative), the copy's record making the new name with the
	// output's own options (`sidecar` its record, so the import pass runs after the commit).
	bool split = false;
	std::string split_source;
	std::vector<Diagnostic> refusals;
	bool ok() const { return refusals.empty(); }
};

// Refused when: the file is unknown or an import output (rename its source); the new
// name fails check_project_file_name (empty, has folders, exceeds the archive's 16 bytes
// for a kind the build packs, reads as another kind), exceeds those 16 bytes for a file of
// no kind the game knows (which the build packs too), changes the extension (the kind
// comes from it), or is taken; when an import source's output would take a name that
// does not fit an archive or is taken; when a site (of the file, or of an output renamed
// with it) sits in a file the editor cannot rewrite (the avatar table, a sound bank, a mission) or
// names a particle flipbook's frame (named from its graphic's name). A site in a terrain, an
// environment, a particle file or the HUD layout is rewritten in its text (native_text_sites.h; one
// stored in the SCR form is written back as plain text). The plan reads the graph, in which an open document stands
// in for its file; a site in an open document with unsaved edits, or the file itself open
// with them, is planned like any other: the session asks to save them before the commit
// (which reads the files on disk) runs. A site keeps the spelling it wrote with the file's
// new stem where its loader reaches the file from that spelling (a model's normal-map row
// naming bump.tga whose bump.dds is renamed stone.dds becomes stone.tga, so the loader
// still reads it as a normal map), else takes the new name; the rename is refused when
// neither would load the renamed file the same way (the loader would take another file of
// the project first). A field naming the file through a style variable keeps the
// variable: the variable's value in
// the stylesheet the game reads it from is the site, and the rename is refused when that
// value is not one it rewrites (a name without its extension: change the variable
// instead).
RenamePlan plan_rename(const ProjectPaths &paths, const AssetScan &scan, const AssetGraph &graph, const std::string &file,
                       const std::string &new_name);
// Where a companion renamed with its mission goes: its own folder, its new name.
std::string companion_path(const RenameOutput &companion);

// A split (ADR 0046 S18): the file `file` (a texture two uses ask different things of) copied as
// `new_name`, and the fields of the files `referrers` lists that name it rewritten to the copy, every
// other use left on the file; an import's output copied as its source (another name beside it, its
// record the output's options naming `new_name`). Refused, as a rename is, for a name the rules refuse,
// that changes the extension or is taken, a site the editor cannot rewrite; and with texture.split for
// an import's source (split the file it makes), no referrer given, or none that names the file. In an
// expansion (`base`, ADR 0046 S16), a name the base game serves is taken too: a copy of it would stand in
// for the base's file for every use.
RenamePlan plan_split(const ProjectPaths &paths, const AssetScan &scan, const AssetGraph &graph, const std::string &file,
                      const std::string &new_name, const std::vector<std::string> &referrers,
                      const BaseNames *base = nullptr);

// Commit a plan that is ok, to its end (RenameTransaction below steps it a file at a time): every
// referencing document is read and rewritten through its type in memory, then the file (and an
// import source's sidecar) is copied under the new name, the rewritten documents are saved, and
// the old file, its sidecar and its old outputs are removed. Only the planned sites are rewritten
// (the same record place, field and value, still resolving to the file the site names); a file
// left with fewer rewrites than its planned sites is a `rename.partial` finding and the old file
// stays. An interruption leaves both names present and no reference to the file dangling (a site
// naming a renamed output resolves once the next import pass makes it, and every refresh runs that
// pass first); the findings say what failed. Open documents among the sites must be reloaded by
// the caller.
bool apply_rename(const ProjectPaths &paths, const ProjectDocument &project, const AssetScan &scan,
                  const AssetGraph &graph, const RenamePlan &plan, std::vector<Diagnostic> &findings);

// A name renamed everywhere (ADR 0046 S12, "Rename everywhere"): the field that defines a
// symbol (a weapon's or an ammo's name, an item's id, a string's key, a style variable, a menu
// screen or window's NAME, a model's user point) and every use that reaches exactly that
// definition (AssetGraph::users_of: never a use a same-named symbol of another scope answers),
// in every file. The plan lists the sites (the definition first, each file, record, field, the
// value before and after) and the refusals first, so a window can preview it and a test read it. A
// use in a text document (S13 D9: a script's operand) is its span, which the rename replaces.
struct SymbolRenamePlan {
	ReferenceKind kind = ReferenceKind::None;
	std::string file;    // the file defining it, project-relative
	std::string locator; // the defining record (Document::locator)
	std::string field;   // the defining field
	std::string scope;   // where its lookup finds it (GraphSymbol::scope)
	std::string old_name; // as defined
	std::string new_name; // as the definition takes it (a use takes it as its kind spells a name there)
	std::vector<RenameSite> sites;
	std::vector<Diagnostic> refusals;
	bool ok() const { return refusals.empty(); }
};

// A name its defining field holds as a number (an item's id) is taken as that number written
// (canonical: "0100301" is 100301), and refused when it is none. Refused, each finding naming the
// file it is about: no new name, or the same one; a new name
// another definition of the kind already has where a lookup would find it (the same scope: a
// string id's table and section, a window's screen, the whole project for a weapon, an ammo, an
// item or a style variable), unless only its case changes; a definition in a file the editor
// cannot rewrite, or a use in one (a mission, the avatar table, a particle file, an
// environment); a site whose field cannot hold the new name (too long for its width, or not a
// number where the field is one: an item id). A use in a document with unsaved edits is planned
// like any other: the session asks to save them before the commit (which reads the files on
// disk) runs. A use of a style variable is written %NAME% (the kind's spell), any other the name.
SymbolRenamePlan plan_symbol_rename_project(const AssetScan &scan, const AssetGraph &graph, const GraphSymbol &symbol,
                                            const std::string &new_name);

// What the rename would do to each file, through the files' own types, nothing written (the
// preview's check): each file of its sites read (an open document among `open` as it stands, else
// the file on disk), every site found by its record's place, its field and the value it held (a
// use only where it still reaches exactly the renamed definition in `graph`) and set through the
// document, and what the file would write made. False with the findings, each naming its file: an
// open document that would not write as it stands (its unsaved edits could not be saved first), a
// site its document refuses (a stylesheet's name the reader would not take) or would hold in
// another form than planned (a stylesheet's name trimmed of its spaces), one no longer there, a
// file that would not write. A text document's sites are its spans, each found by its place and
// the name it held, replaced in one batch from the last to the first, and read back from the text
// as the new name at the same place (a name the text would read otherwise, cut or split, is
// refused).
bool check_symbol_rename(const ProjectPaths &paths, const ProjectDocument &project, const AssetScan &scan,
                         const AssetGraph &graph, const SymbolRenamePlan &plan,
                         const std::vector<std::shared_ptr<const DocumentBase>> &open,
                         std::vector<Diagnostic> &findings);
// Commit a plan that is ok, on disk as apply_rename does (not undoable, like a file's rename), to
// its end (RenameTransaction steps it): every file of its sites read again from disk and rewritten
// in memory as check_symbol_rename does, and only when every one of them takes every site and
// would write, written together (write_files_together: each text beside its file, then each
// replaced, `replace` the step; a file the system will not replace puts back the ones replaced
// before it); else nothing is written and the findings say why. Open documents among the sites
// must be reloaded by the caller.
bool apply_symbol_rename(const ProjectPaths &paths, const ProjectDocument &project, const AssetScan &scan,
                         const AssetGraph &graph, const SymbolRenamePlan &plan, std::vector<Diagnostic> &findings,
                         const FileReplace &replace = replace_file);

// A rename's commit a file at a time (ADR 0046 S13 A3): apply_rename's or apply_symbol_rename's
// work as a cursor, so the session runs it as an operation's steps. Each step before the commit
// reads one file of the sites (in the order of their paths) and sets its sites in memory, nothing
// written, so a transaction dropped between two steps has done nothing; once every file is
// staged, the commit is one step, the only one that writes (a file's rename: the file copied under
// its new name, the rewritten documents saved, the old file removed; a name's: every file written
// together). A plan that is not ok commits at once, refused. It reads `scan` and `graph` as they
// are at each step: a site is found again in the file staged (the same record place, field and
// value) and must still resolve where the plan said, and the commit writes the bytes staged. Its
// caller keeps `scan` as it was planned over; the graph may move between two steps (the session's
// validation steps before its operation every poll), but nothing writes the project's files or
// the open documents while the session's operation holds them, so a validation brings the graph
// to the same files and resolves each staged site where it did.
class RenameTransaction {
public:
	RenameTransaction(const ProjectPaths &paths, const ProjectDocument &project, const AssetScan &scan,
			const AssetGraph &graph, RenamePlan plan);
	RenameTransaction(const ProjectPaths &paths, const ProjectDocument &project, const AssetScan &scan,
			const AssetGraph &graph, SymbolRenamePlan plan, FileReplace replace = replace_file);
	~RenameTransaction();
	RenameTransaction(const RenameTransaction &) = delete;
	RenameTransaction &operator=(const RenameTransaction &) = delete;

	// One step: the next file staged, or once each is, the commit. True once committed.
	bool step();
	bool committed() const { return committed_; }
	// What the commit came to (apply_rename's and apply_symbol_rename's answer); false before it.
	bool ok() const { return committed_ && ok_; }
	const std::vector<Diagnostic> &findings() const { return findings_; }
	// Where it stands: the files staged of the files the sites are in, and the file staged last.
	size_t files_staged() const { return next_; }
	size_t files_total() const { return files_.size(); }
	const std::string &current() const { return current_; }
	// The project-relative paths a commit writes, makes or removes (the file and its new name, an
	// import source's records, every file of the sites): what a scan reads again after it.
	std::vector<std::string> touched() const;

private:
	struct Staged;

	void stage_file(const std::string &file, const std::vector<const RenameSite *> &sites);
	void stage_native(const AssetEntry &asset, const std::vector<const RenameSite *> &sites, Staged &staged);
	void commit();
	void commit_file_rename();
	void commit_split();
	// Each staged file of a file's rename or a split written in its turn, with the findings its staging made;
	// false when one did not take or did not write.
	bool save_staged();
	void commit_symbol_rename();

	const ProjectPaths &paths_;
	const ProjectDocument &project_;
	const AssetScan &scan_;
	const AssetGraph &graph_;
	bool symbol_ = false;
	RenamePlan file_plan_;
	SymbolRenamePlan symbol_plan_;
	FileReplace replace_;
	// The sites by file, in the order of the files' paths.
	std::map<std::string, std::vector<const RenameSite *>> files_;
	std::map<std::string, std::vector<const RenameSite *>>::const_iterator at_;
	size_t next_ = 0;
	std::string current_;
	std::vector<std::unique_ptr<Staged>> staged_;
	std::vector<Diagnostic> findings_;
	bool staged_ok_ = true;
	bool committed_ = false;
	bool ok_ = false;
};

} // namespace opennova::editor
