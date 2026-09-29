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

struct ValidationStats {
	size_t passes = 0;       // validations run over this cache, all told
	size_t files_loaded = 0; // closed files read from disk on the last validation
	size_t files_reused = 0; // closed files unchanged since they were read
	size_t files_failed = 0; // of the files read on the last validation, the ones that did not load
};

// The closed files the validators read, kept from one validation to the next (ADR 0046
// d9, S9e). A file is read again only when the scan says its size or modified time
// changed (the asset graph's extraction cache is the precedent), so an edit to one
// open document does not parse every other catalog, table and menu in the project
// again; a file that does not load keeps its finding the same way. An open document is
// never cached here: it stands in for its file. The issues of a document's serialized
// form are kept per document instance and revision, open or closed, so an unchanged
// document is not serialized again either.
class ValidationCache {
public:
	// A validation starts: the per-pass counters restart and every file is unread.
	void begin();
	// A closed file as a document of its type, read or reused. Null, with `error`, when
	// the file does not load.
	std::shared_ptr<const Document> closed(const ProjectPaths &paths, const ProjectDocument &project,
	                                       const AssetEntry &asset, Diagnostic &error);
	// What serializing `document` reports, from the last serialization of this instance
	// at this revision when there is one.
	const std::vector<SourceIssue> &serialize_issues(const Document &document);
	// The validation ends: the files it did not read (gone from the scan, or open now)
	// leave the cache.
	void end();
	const ValidationStats &stats() const { return stats_; }

private:
	struct Entry {
		uint64_t size = 0;
		int64_t modified = 0;
		AssetKind kind = AssetKind::Unknown;
		std::string game;
		std::shared_ptr<const Document> document; // null when the file did not load
		Diagnostic error;                          // why it did not
		bool filled = false;                       // read at least once
		bool read = false;                         // read by the current validation
	};
	struct Serialized {
		uint64_t identity = 0;
		uint64_t revision = 0;
		std::vector<SourceIssue> issues;
		bool read = false;
	};
	std::map<std::string, Entry> entries_;         // by project-relative path
	std::map<std::string, Serialized> serialized_; // by project-relative path
	ValidationStats stats_;
};

// What one validation hands each document type's validator (document_types.h): the
// project's files, each as a document, an open document standing in for its file.
struct ValidationInput {
	const ProjectPaths &paths;
	const ProjectDocument &project;
	const AssetScan &scan;
	const std::vector<std::shared_ptr<const Document>> &open;
	ValidationCache &cache;

	// The open document for the file, else the file itself through the cache. Null, with
	// `error`, when the file does not load.
	std::shared_ptr<const Document> document(const AssetEntry &asset, Diagnostic &error) const;
};

} // namespace opennova::editor
