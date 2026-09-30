#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
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
// those the ones that did not load; and every validation run over the cache.
struct ValidationStats {
	size_t passes = 0; // validations run over this cache, all told
	size_t files_validated = 0; // files whose own findings the last validation made
	size_t files_reused = 0; // files whose findings it kept: nothing they were made from moved
	size_t files_loaded = 0; // closed files it read from disk to make theirs
	size_t files_failed = 0; // of those, the ones that did not load
};

// The project's files as one validation reads them (graph/project_validation.h): the scan's
// files, an open document standing in for its file.
struct ValidationInput {
	const ProjectPaths &paths;
	const ProjectDocument &project;
	const AssetScan &scan;
	const std::vector<std::shared_ptr<const Document>> &open;

	// The open document standing in for a file; null for a closed one.
	std::shared_ptr<const Document> open_document(const AssetEntry &asset) const;
};

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
// document a later load or an open of the file makes.
class ValidationCache {
public:
	// A validation starts: its counters restart and no file has been asked.
	void begin();
	// A file's own findings, made or kept (above); none for a kind no document type opens.
	const std::vector<Diagnostic> &file_findings(
			const ValidationInput &input, const AssetEntry &asset);
	// Whether the file's own checks read its records in this validation: it loaded, and no source
	// error blocks it (a blocked file's findings are its source findings alone). What another
	// file's check reads of its records follows (graph/use_checks.h).
	bool records_checked(const std::string &path) const;
	// The validation ends: the files it did not ask (gone from the scan) leave the cache.
	void end();
	const ValidationStats &stats() const { return stats_; }
	// The closed files' documents the current validation loaded that are alive, anywhere: none
	// once their findings are made (nothing keeps a closed file resident).
	size_t documents_alive() const;

private:
	struct Entry {
		bool filled = false; // made at least once
		bool asked = false; // asked by the current validation
		bool checked = false; // its records were read (records_checked)
		// What the findings were made from: an open document, or a closed file as the scan lists
		// it.
		bool open = false;
		uint64_t identity = 0, revision = 0;
		bool dirty = false, wrote_file = false;
		uint64_t size = 0;
		int64_t modified = 0;
		AssetKind kind = AssetKind::Unknown;
		std::string game;
		std::vector<Diagnostic> findings;
	};
	std::map<std::string, Entry> entries_; // by project-relative path
	ValidationStats stats_;
	std::vector<std::weak_ptr<const Document>> loaded_; // the closed files this validation read
};

} // namespace opennova::editor
