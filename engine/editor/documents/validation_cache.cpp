#include "validation_cache.h"

#include <iterator>
#include <utility>

#include <editor/documents/document_types.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>

namespace opennova::editor {

namespace {

// A file of a type whose documents hold neither records nor a text (S13 D6, D9): no validator
// reads it yet.
Diagnostic no_records(const AssetEntry &asset) {
	return make_finding(CoreFinding::DocumentNoRecords, DiagnosticSeverity::Error,
	                    "This file's document holds no records: no validator reads it yet.",
	                    asset.relative_path);
}

} // namespace

bool load_listed(DocumentBase &document, const ProjectPaths &paths, const AssetEntry &asset, const std::string &game,
                 Diagnostic &error) {
	if (!paths.files)
		return document.load(join_path(paths.root, asset.relative_path), asset.relative_path, asset.kind, game, error);
	std::vector<uint8_t> bytes;
	if (!paths.files->read(asset.logical_name, bytes)) {
		error = make_finding(CoreFinding::DocumentRead, DiagnosticSeverity::Error, asset.logical_name + " could not be read.",
		                     asset.relative_path);
		return false;
	}
	return document.load_bytes(bytes, asset.relative_path, asset.kind, game, error);
}

void ValidationCache::begin() {
	const size_t passes = stats_.passes;
	stats_ = ValidationStats();
	stats_.passes = passes + 1;
	loaded_.clear();
	for (auto &[path, entry] : entries_)
		entry.asked = false;
}

const std::vector<Diagnostic> &ValidationCache::file_findings(
		const ValidationInput &input, const AssetEntry &asset) {
	Entry &entry = entries_[asset.relative_path];
	if (entry.asked)
		return entry.findings; // asked again within the same validation: counted once
	entry.asked = true;
	const std::shared_ptr<const DocumentBase> open = input.open_document(asset);
	// Kept while what they were made from holds: the same open document in the same state, or the
	// same closed file.
	bool same = entry.filled && entry.open == (open != nullptr);
	if (same && open)
		same = entry.identity == open->identity() &&
				entry.load_generation == open->load_generation() &&
				entry.revision == open->revision() && entry.dirty == open->dirty() &&
				entry.wrote_file == open->wrote_file();
	else if (same)
		same = entry.size == asset.size_bytes && entry.modified == asset.modified_ticks &&
				entry.kind == asset.kind && entry.game == input.project.target_game;
	if (same) {
		++stats_.files_reused;
		return entry.findings;
	}
	entry = Entry();
	entry.filled = entry.asked = true;
	++stats_.files_validated;
	const DocumentType *type = document_type_for(asset.kind);
	// A type whose documents hold neither records nor a text (S13 D6, D9): no validator reads
	// them yet, the file unread.
	const bool records = type && document_content(*type) != DocumentContent::Other;
	const auto validate = [&entry, type](const DocumentBase &document) {
		if (type->validate_file)
			entry.findings = type->validate_file(document);
		// Each finding on a record keyed on the record as itself (the game's own data's fold).
		key_findings(document, entry.findings);
		entry.checked = !document.blocked();
	};
	if (open) {
		entry.open = true;
		entry.identity = open->identity();
		entry.load_generation = open->load_generation();
		entry.revision = open->revision();
		entry.dirty = open->dirty();
		entry.wrote_file = open->wrote_file();
		if (type && !records)
			entry.findings.push_back(no_records(asset));
		else if (type)
			validate(*open);
		return entry.findings;
	}
	entry.size = asset.size_bytes;
	entry.modified = asset.modified_ticks;
	entry.kind = asset.kind;
	entry.game = input.project.target_game;
	if (!type)
		return entry.findings;
	if (!records) {
		entry.findings.push_back(no_records(asset));
		return entry.findings;
	}
	// The closed file, loaded for its findings alone: it goes when they are made (watched until
	// it does, so a cache that kept one would show it: documents_alive).
	++stats_.files_loaded;
	const std::shared_ptr<DocumentBase> document = type->make();
	loaded_.push_back(document);
	Diagnostic error;
	if (!load_listed(*document, input.paths, asset, input.project.target_game, error)) {
		++stats_.files_failed;
		entry.findings.push_back(std::move(error));
		return entry.findings;
	}
	validate(*document);
	return entry.findings;
}

size_t ValidationCache::documents_alive() const {
	size_t alive = 0;
	for (const auto &document : loaded_)
		alive += document.expired() ? 0 : 1;
	return alive;
}

bool ValidationCache::records_checked(const std::string &path) const {
	const auto found = entries_.find(path);
	return found != entries_.end() && found->second.asked && found->second.checked;
}

const std::vector<Diagnostic> *ValidationCache::kept_findings(const std::string &path) const {
	const auto found = entries_.find(path);
	return found != entries_.end() && found->second.asked ? &found->second.findings : nullptr;
}

void ValidationCache::end() {
	for (auto it = entries_.begin(); it != entries_.end();) {
		if (it->second.asked) {
			++it;
			continue;
		}
		++stats_.files_dropped;
		it = entries_.erase(it);
	}
}

std::shared_ptr<const DocumentBase> ValidationInput::open_document(const AssetEntry &asset) const {
	for (const auto &candidate : open)
		if (candidate && candidate->path() == asset.relative_path) return candidate;
	return nullptr;
}

} // namespace opennova::editor
