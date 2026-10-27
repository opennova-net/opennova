#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_registry.h>
#include <editor/model/diagnostic.h>
#include <editor/model/document.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

// What the last validation over a cache did (ADR 0046 S13 D4): the files whose own findings it
// made and those whose findings it kept, the closed files it read from disk to make theirs and of
// those the ones that did not load, and the files it let go; and every validation run over the
// cache.
struct ValidationStats {
	size_t passes = 0; // validations run over this cache, all told
	size_t files_validated = 0; // files whose own findings the last validation made
	size_t files_reused = 0; // files whose findings it kept: nothing they were made from moved
	size_t files_loaded = 0; // closed files it read from disk to make theirs
	size_t files_failed = 0; // of those, the ones that did not load
	size_t files_dropped = 0; // files it did not ask (gone from the scan), whose findings went
};

// The files of its scan a validation validates where it does not validate every one (ADR 0046 S15: the
// game's own data's check, session/original_files.h, over the files the project's rows are about and those
// their findings read): each one's own findings made (so records_checked is false for every other file),
// the texture headers its uses' checks read, the per-file project checks over it (a menu rendered, the tile
// check's missions). Every file is still listed, and the asset graph reads every file it reads: a cross-file
// finding about a file in the scope reads the references of every file (which use of a texture comes first,
// whether any menu names a variable), so the findings about the scope's files are those a validation of
// every file makes.
struct ValidationScope {
	std::set<std::string> paths; // project-relative paths of the scan's files in it
	bool tiles = false;          // one of them is a tile placement (the tile check then runs over every mission)
	// A value of a process-wide counter, taken anew by every change (taken()): what a cache keeps of a scope's
	// validation is kept by it.
	uint64_t version = 0;
	bool has(const std::string &path) const { return paths.count(path) != 0; }
	void taken();
};

class InstallFileSource;

// The project's files as one validation reads them (graph/project_validation.h): the scan's
// files, an open document standing in for its file; `scope` the files it validates (null: every file);
// `base` an expansion project's base game as the game mounts it below the expansion's pair
// (assets/install_view.h base_game_files; null for a standalone project, or while the base is unread),
// held alive by the caller for the validation's length.
struct ValidationInput {
	const ProjectPaths &paths;
	const ProjectDocument &project;
	const AssetScan &scan;
	const std::vector<std::shared_ptr<const DocumentBase>> &open;
	const ValidationScope *scope = nullptr;
	const InstallFileSource *base = nullptr;

	// The open document standing in for a file; null for a closed one.
	std::shared_ptr<const DocumentBase> open_document(const AssetEntry &asset) const;
	// Whether the validation validates the file at `path` (every file, without a scope).
	bool in_scope(const std::string &path) const { return !scope || scope->has(path); }
};

// A file of a validation's scan loaded into document: from the project's folder, or through the paths'
// source by its logical name (ProjectPaths::files: the game install's files as the game is served them).
bool load_listed(DocumentBase &document, const ProjectPaths &paths, const AssetEntry &asset, const std::string &game,
                 Diagnostic &error);

// Each file's own findings (ADR 0046 d9, S9e, S13 D4), kept from one validation to the next: its
// document type's validate_file over its document (documents/document_types.h), or why the file
// did not load. They are made again only when what they were made from moved: an open
// document's instance, revision, unsaved state and whether Save wrote its file (a save reads the
// file's source findings again, and a stylesheet's line ends follow the file written); a closed
// file's size, last write and kind as the scan lists them, and the project's game (the asset
// graph's extraction is the precedent). So an edit of one open document validates that document
// alone and reads no file. A closed file is loaded for its findings and let go: the cache keeps
// the findings, never the document (a model's geometry included). A load gives the same file's
// records the same identities, so a finding still names its record (row_id, child_id) in the
// document a later load or an open of the file makes. A type whose documents hold neither records
// nor a text (document_content, S13 D6 and D9) makes document.no_records for its file, unread.
class ValidationCache {
public:
	// A validation starts over every file of the scan of `serial` (AssetScan::serial) read for the game `game`:
	// its counters restart and no file has been asked.
	void begin(uint64_t serial = 0, const std::string &game = std::string());
	// Whether a validation over the scan of `serial` for `game` may keep every file's findings as the last one
	// that ended left them (begin_kept): that one read the same scan (the same files at the same stamps) for
	// the same game. Never for a scan not indexed (serial 0).
	bool keeps(uint64_t serial, const std::string &game) const;
	// A validation starts over that scan (keeps() true), every file's findings kept as the last one that ended
	// left them, counted reused, but those of the files `asked` names (project-relative), which file_findings
	// makes or keeps again: the files an open document stands in for, now or at the last validation
	// (open_files). An edit of one document validates that document and asks no other.
	void begin_kept(uint64_t serial, const std::string &game, const std::vector<std::string> &asked);
	// The files whose findings were made from an open document, project-relative, sorted.
	std::vector<std::string> open_files() const { return {open_.begin(), open_.end()}; }
	// A file's own findings, made or kept (above); none for a kind no document type opens.
	const std::vector<Diagnostic> &file_findings(
			const ValidationInput &input, const AssetEntry &asset);
	// Whether the file's own checks read its records in this validation: it loaded, and no source
	// error blocks it (a blocked file's findings are its source findings alone). What another
	// file's check reads of its records follows (graph/use_checks.h).
	bool records_checked(const std::string &path) const;
	// A file's own findings as this validation made or kept them (file_findings); null for a
	// file it did not ask.
	const std::vector<Diagnostic> *kept_findings(const std::string &path) const;
	// The validation ends: the files it did not ask (gone from the scan) leave the cache (none of a validation
	// that kept them, begin_kept).
	void end();
	const ValidationStats &stats() const { return stats_; }
	// The closed files' documents the current validation loaded that are alive, anywhere: none
	// once their findings are made (nothing keeps a closed file resident).
	size_t documents_alive() const;

private:
	struct Entry {
		bool filled = false; // made at least once
		uint64_t pass = 0;   // the validation that last asked it (asked by the current one: pass_)
		bool checked = false; // its records were read (records_checked)
		// What the findings were made from: an open document, or a closed file as the scan lists
		// it.
		bool open = false;
		// A load in place keeps the instance and starts the revision again: the load too.
		uint64_t identity = 0, load_generation = 0, revision = 0, companion = 0;
		bool dirty = false, wrote_file = false;
		uint64_t size = 0;
		int64_t modified = 0;
		AssetKind kind = AssetKind::Unknown;
		std::string game;
		std::vector<Diagnostic> findings;
	};
	// Whether an entry is the current validation's: asked by it, or by one it kept the findings of (floor_).
	bool current(const Entry &entry) const { return entry.pass >= floor_ && entry.pass != 0; }

	std::map<std::string, Entry> entries_; // by project-relative path
	std::set<std::string> open_;           // the entries made from an open document (open_files)
	uint64_t pass_ = 0;                    // the current validation, counted (Entry::pass)
	uint64_t floor_ = 0;                   // the first validation whose asked entries are the current one's
	bool keeping_ = false;                 // the current validation keeps the files it does not ask (begin_kept)
	// The scan and the game the current validation reads, and those of the last that ended (keeps), with the
	// first validation whose entries that one's are.
	uint64_t serial_ = 0;
	std::string game_;
	bool ended_ = false;
	uint64_t ended_serial_ = 0, ended_floor_ = 0;
	std::string ended_game_;
	ValidationStats stats_;
	std::vector<std::weak_ptr<const DocumentBase>> loaded_; // the closed files this validation read
};

} // namespace opennova::editor
