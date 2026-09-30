#include "validation_cache.h"

#include <filesystem>
#include <iterator>
#include <utility>

#include <editor/documents/document_types.h>

namespace opennova::editor {

namespace {

// A file of a type whose documents hold no records (S13 D6): no validator reads it yet.
Diagnostic no_records(const AssetEntry &asset) {
	return make_diagnostic(DiagnosticSeverity::Error, "document.no_records",
	                       "This file's document holds no records: no validator reads it yet.",
	                       asset.relative_path);
}

} // namespace

void ValidationCache::begin() {
	const size_t passes = stats_.passes;
	stats_ = ValidationStats();
	stats_.passes = passes + 1;
	for (auto &[path, entry] : entries_) entry.read = false;
	for (auto &[path, entry] : serialized_) entry.read = false;
}

std::shared_ptr<const Document> ValidationCache::closed(const ProjectPaths &paths, const ProjectDocument &project,
                                                        const AssetEntry &asset, Diagnostic &error) {
	Entry &entry = entries_[asset.relative_path];
	if (entry.read) {
		// Asked again within the same validation: counted once.
	} else if (entry.filled && entry.size == asset.size_bytes && entry.modified == asset.modified_ticks &&
	           entry.kind == asset.kind && entry.game == project.target_game) {
		++stats_.files_reused;
	} else {
		entry = Entry();
		entry.filled = true;
		entry.size = asset.size_bytes;
		entry.modified = asset.modified_ticks;
		entry.kind = asset.kind;
		entry.game = project.target_game;
		++stats_.files_loaded;
		const DocumentType *type = document_type_for(asset.kind);
		std::shared_ptr<Document> loaded = type ? records_of(type->make()) : nullptr;
		if (!type) {
			entry.error = make_diagnostic(DiagnosticSeverity::Error, "document.kind", "This kind of file has no editor yet.",
			                              asset.relative_path);
		} else if (!loaded) {
			entry.error = no_records(asset);
		} else if (loaded->load((std::filesystem::path(paths.root) / asset.relative_path).generic_string(),
		                        asset.relative_path, asset.kind, project.target_game, entry.error)) {
			entry.document = std::move(loaded);
		}
		if (!entry.document) ++stats_.files_failed;
	}
	entry.read = true;
	if (!entry.document) error = entry.error;
	return entry.document;
}

const std::vector<SourceIssue> &ValidationCache::serialize_issues(const Document &document) {
	// A document's identity is never 0, so a new entry always serializes.
	Serialized &entry = serialized_[document.path()];
	if (entry.identity != document.identity() || entry.revision != document.revision()) {
		entry.identity = document.identity();
		entry.revision = document.revision();
		entry.issues = document.serialize().issues;
	}
	entry.read = true;
	return entry.issues;
}

void ValidationCache::end() {
	for (auto it = entries_.begin(); it != entries_.end();) it = it->second.read ? std::next(it) : entries_.erase(it);
	for (auto it = serialized_.begin(); it != serialized_.end();) it = it->second.read ? std::next(it) : serialized_.erase(it);
}

std::shared_ptr<const Document> ValidationInput::document(const AssetEntry &asset, Diagnostic &error) const {
	// An open document of another kind than records stands in for nothing here: the cache answers
	// for its type (document.no_records) without reading the file.
	for (const auto &candidate : open)
		if (candidate && candidate->path() == asset.relative_path)
			if (std::shared_ptr<const Document> records = records_of(candidate)) return records;
	return cache.closed(paths, project, asset, error);
}

} // namespace opennova::editor
