#include <editor/assets/asset_registry.h>

#include <algorithm>
#include <cstring>
#include <numeric>
#include <set>
#include <utility>

#include <base/io/log.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/project_scan.h>
#include <editor/import/importer.h>
#include <editor/project/project_files.h>
#include <formats/pff/pff.h>

namespace opennova::editor {

namespace {

// The entries' order: by key, then by path.
bool entry_before(const AssetEntry &a, const AssetEntry &b) {
	if (a.key != b.key) return a.key < b.key;
	return a.relative_path < b.relative_path;
}

// Whether the index is the entries' as they are: one path slot per entry and (walked in a
// debug build) every entry keyed and in order. `entries` is public, so a scan changed after its
// index() is looked through linearly, with a warning, and never read out of bounds.
bool index_current(const std::vector<AssetEntry> &entries, size_t indexed) {
	if (indexed != entries.size()) return false;
#ifndef NDEBUG
	for (size_t i = 0; i < entries.size(); ++i) {
		if (entries[i].key != normalized_logical_name(entries[i].logical_name)) return false;
		if (i > 0 && entry_before(entries[i], entries[i - 1])) return false;
	}
#endif
	return true;
}

void stale_index(size_t entries, size_t indexed) {
	io::logf(io::LogLevel::kWarn,
			"AssetScan: %zu entries, %zu indexed: its entries changed after index(), which a scan "
			"made by hand calls again; looked through linearly",
			entries, indexed);
}

// The findings of the names, over the indexed entries in their order: the archive's name rules
// bind only a file the build packs (check_file_name's rule: a loose kind, a video, a music bank, a
// config, is copied beside the archives under any name); two files of one name; a kind the game
// does not use.
void name_findings(const std::vector<AssetEntry> &entries, std::vector<Diagnostic> &out) {
	for (size_t i = 0; i < entries.size(); ++i) {
		const AssetEntry &asset = entries[i];
		const bool packed = archive_name_limit_binds(asset.kind);
		if (packed && asset.logical_name.size() > static_cast<size_t>(pff::PFF_NAME_SIZE)) {
			out.push_back(make_diagnostic(
			        DiagnosticSeverity::Error, "asset.name.too_long",
			        "The file name " + asset.logical_name + " is longer than " +
			                std::to_string(pff::PFF_NAME_SIZE) +
			                " characters; the game cannot store it in an archive.",
			        asset.relative_path));
		} else if (packed && asset.key.empty()) {
			out.push_back(make_diagnostic(DiagnosticSeverity::Error, "asset.name.empty",
			                              "The file name is blank once normalized.", asset.relative_path));
		}
		if (i > 0 && entries[i - 1].key == asset.key) {
			out.push_back(make_diagnostic(
			        DiagnosticSeverity::Error, "asset.name.duplicate",
			        "Two files share the name " + asset.logical_name + " (" + entries[i - 1].relative_path + " and " +
			                asset.relative_path + "); the game resolves names without folders, so only one can exist.",
			        asset.relative_path));
		}
		if (asset.kind == AssetKind::Unknown) {
			out.push_back(make_diagnostic(DiagnosticSeverity::Warning, "asset.kind.unknown",
			                              "The game does not use files of this type.", asset.relative_path));
		}
	}
}

// A finding the scan lists already: the import pass's record of a sidecar that does not parse is
// the walk's too, one finding.
bool listed(const std::vector<Diagnostic> &rows, const Diagnostic &d) {
	return std::any_of(rows.begin(), rows.end(), [&d](const Diagnostic &row) {
		return row.severity == d.severity && row.code == d.code && row.asset == d.asset && row.message == d.message;
	});
}

} // namespace

std::string normalized_logical_name(std::string_view name) {
	char buf[256];
	pff::pff_norm_name(name.data(), name.size(), buf, sizeof(buf));
	return std::string(buf);
}

bool logical_name_fits_archive(std::string_view name) {
	if (name.size() > static_cast<size_t>(pff::PFF_NAME_SIZE)) return false;
	return !normalized_logical_name(name).empty();
}

const AssetEntry *AssetScan::find(std::string_view logical_name) const {
	const std::string key = normalized_logical_name(logical_name);
	if (!index_current(entries, by_path_.size())) {
		stale_index(entries.size(), by_path_.size());
		const AssetEntry *first = nullptr;
		for (const AssetEntry &entry : entries)
			if (normalized_logical_name(entry.logical_name) == key &&
					(!first || entry.relative_path < first->relative_path))
				first = &entry;
		return first;
	}
	const auto found = std::lower_bound(entries.begin(), entries.end(), key,
			[](const AssetEntry &entry, const std::string &wanted) { return entry.key < wanted; });
	return found != entries.end() && found->key == key ? &*found : nullptr;
}

const AssetEntry *AssetScan::at_path(std::string_view relative_path) const {
	if (!index_current(entries, by_path_.size())) {
		stale_index(entries.size(), by_path_.size());
		for (const AssetEntry &entry : entries)
			if (entry.relative_path == relative_path) return &entry;
		return nullptr;
	}
	const auto found = std::lower_bound(by_path_.begin(), by_path_.end(), relative_path,
			[this](size_t index, std::string_view wanted) {
				return std::string_view(entries[index].relative_path) < wanted;
			});
	if (found == by_path_.end() || entries[*found].relative_path != relative_path) return nullptr;
	return &entries[*found];
}

void AssetScan::index() {
	for (AssetEntry &entry : entries) entry.key = normalized_logical_name(entry.logical_name);
	std::sort(entries.begin(), entries.end(), entry_before);
	by_path_.resize(entries.size());
	std::iota(by_path_.begin(), by_path_.end(), size_t(0));
	std::sort(by_path_.begin(), by_path_.end(), [this](size_t a, size_t b) {
		return entries[a].relative_path < entries[b].relative_path;
	});
}

void AssetScan::set_visits(std::map<std::string, Visit> visits) {
	visits_ = std::move(visits);
	compose();
}

size_t AssetScan::update(const ProjectPaths &paths, const ProjectDocument &doc, const std::vector<std::string> &changed) {
	// An import record is no file of its own: its outputs are its source's, and a PNG is a source
	// only while its record is there, so the two are visited together.
	const std::string suffix = kImportSidecarSuffix;
	std::set<std::string> again;
	for (const std::string &path : changed) {
		if (path.empty()) continue;
		again.insert(path);
		if (strutil::ends_with_icase(path, suffix))
			again.insert(path.substr(0, path.size() - suffix.size()));
		else if (importer_for(basename_of(path)))
			again.insert(path + suffix);
	}
	// Each dropped first under the path asked and visited again under the path the walk lists it by
	// (a rename that changes only the case asks for both).
	for (const std::string &path : again) visits_.erase(path);
	std::set<std::string> read;
	for (const std::string &path : again) {
		std::string key;
		Visit visit;
		if (!scan_project_file(paths, doc, path, key, visit)) continue;
		visits_[key] = std::move(visit);
		read.insert(key); // two paths of one file (its case changed) read it once
	}
	compose();
	return read.size();
}

void AssetScan::set_import_findings(std::vector<Diagnostic> findings) {
	import_findings_ = std::move(findings);
	compose();
}

// One order however the visits were made: the entries by key then path, the walk's findings by the
// files' paths, the names' in the entries' order, then the import pass's that neither made (a
// record both read, a sidecar that does not parse left as the author wrote it, is one finding).
void AssetScan::compose() {
	entries.clear();
	diagnostics.clear();
	for (const auto &[path, visit] : visits_) {
		entries.insert(entries.end(), visit.entries.begin(), visit.entries.end());
		diagnostics.insert(diagnostics.end(), visit.findings.begin(), visit.findings.end());
	}
	index();
	name_findings(entries, diagnostics);
	for (const Diagnostic &d : import_findings_)
		if (!listed(diagnostics, d)) diagnostics.push_back(d);
}

AssetScan scan_project_assets(const ProjectPaths &paths, const ProjectDocument &doc) {
	ProjectScan scan(paths, doc);
	while (!scan.step(kWholeWalkStep)) {
	}
	return scan.take();
}

} // namespace opennova::editor
