#include <editor/import/import_pass.h>

#include <algorithm>
#include <system_error>

#include <base/io/file_time.h>
#include <base/io/hash.h>
#include <base/io/json.h>
#include <editor/assets/project_scan.h>
#include <editor/import/importer.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// 2: the content hashes are FNV-1a over its true offset basis (S12 A5); a cache of 1 holds
// digests made with the old constant, which a size and time that still match would keep.
constexpr int kImportCacheSchemaVersion = 2;

} // namespace

ImportPass::ImportPass(const ProjectPaths &paths, const ProjectDocument &project, bool force, std::string only) :
		paths_(paths),
		project_(project),
		force_(force),
		only_(std::move(only)),
		root_(paths.root),
		export_dir_(paths.export_dir(project)) {}

// A cache that is missing, broken or of another schema reads as empty: every source
// is then imported again, which is all a lost cache costs.
ImportPass::Cache ImportPass::load_cache(const std::string &path, std::string &text) {
	Cache cache;
	std::string message;
	std::error_code ec;
	if (!fs::is_regular_file(path, ec) || !read_file_text(path, text, message)) return cache;
	io::JsonValue json;
	if (!io::json_parse(text, json, message) || !json.is_object() ||
	    json.get_int("schema_version", -1) != kImportCacheSchemaVersion)
		return cache;
	const io::JsonValue *sources = json.get("sources");
	if (!sources || !sources->is_array()) return cache;
	for (const io::JsonValue &item : sources->array) {
		if (!item.is_object()) continue;
		const std::string source = item.get_string("source", "");
		CacheEntry entry;
		uint64_t modified = 0;
		entry.size = uint64_t(item.get_number("size", 0));
		if (source.empty() || !io::parse_hex64(item.get_string("modified", ""), modified) ||
		    !io::parse_hex64(item.get_string("hash", ""), entry.hash) || !io::parse_hex64(item.get_string("record", ""), entry.record))
			continue;
		entry.modified = int64_t(modified);
		cache[source] = entry;
	}
	return cache;
}

// Written only when it changed: a pass over an untouched project writes nothing, and a
// project with no import sources and no cache yet (a clone opened for `status`) gets none.
void ImportPass::save_cache() const {
	if (seen_.empty() && cache_text_.empty()) return;
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::JsonValue::make_number(kImportCacheSchemaVersion));
	io::JsonValue sources = io::JsonValue::make_array();
	for (const auto &[source, entry] : seen_) {
		io::JsonValue item = io::JsonValue::make_object();
		item.set("source", io::JsonValue::make_string(source));
		item.set("size", io::JsonValue::make_number(double(entry.size)));
		item.set("modified", io::JsonValue::make_string(io::hex64(uint64_t(entry.modified))));
		item.set("hash", io::JsonValue::make_string(io::hex64(entry.hash)));
		item.set("record", io::JsonValue::make_string(io::hex64(entry.record)));
		sources.push(std::move(item));
	}
	json.set("sources", std::move(sources));
	const std::string text = io::json_write(json);
	if (text == cache_text_) return;
	std::string message;
	if (ensure_project_cache_dir(paths_, message)) write_file_atomic(paths_.import_cache_file, text, message);
}

bool ImportPass::step(uint64_t budget) {
	uint64_t spent = 0;
	do {
		switch (phase_) {
		case Phase::Start: {
			cache_ = load_cache(paths_.import_cache_file, cache_text_);
			std::error_code ec;
			walk_ = fs::recursive_directory_iterator(root_, fs::directory_options::skip_permission_denied, ec);
			if (ec) {
				// No project to walk: nothing imported, and the cache as it was.
				phase_ = Phase::Done;
				return true;
			}
			phase_ = Phase::Listing;
			break;
		}
		case Phase::Listing: {
			if (walk_ == fs::recursive_directory_iterator()) {
				std::sort(listed_.begin(), listed_.end(),
						[](const auto &a, const auto &b) { return a.first < b.first; });
				phase_ = Phase::Importing;
				break;
			}
			spent += kWalkEntryCost;
			std::error_code ec;
			const fs::directory_entry &entry = *walk_;
			const fs::path path = entry.path();
			if (entry.is_directory(ec)) {
				if (is_dot_directory(path) || fs::equivalent(path, export_dir_, ec)) walk_.disable_recursion_pending();
			} else if (entry.is_regular_file(ec) && importer_for(path.filename().string())) {
				std::string relative = fs::relative(path, root_, ec).generic_string();
				if (ec) relative = path.filename().string();
				listed_.emplace_back(std::move(relative), path);
			}
			walk_.increment(ec);
			if (ec) walk_ = fs::recursive_directory_iterator(); // the walk stops where it could not go on
			break;
		}
		case Phase::Importing: {
			if (next_ == listed_.size()) {
				save_cache();
				phase_ = Phase::Done;
				return true;
			}
			const auto &[relative, path] = listed_[next_++];
			current_ = relative;
			spent += kWalkEntryCost;
			take_source(path, relative, spent);
			break;
		}
		case Phase::Done: return true;
		}
	} while (spent < budget);
	return done();
}

void ImportPass::take_source(const fs::path &path, const std::string &relative, uint64_t &spent) {
	std::error_code ec;
	const std::string filename = path.filename().string();
	const Importer *importer = importer_for(filename);
	if (!importer || !fs::is_regular_file(path, ec)) return; // gone since it was listed
	ImportedSource source;
	source.source = relative;
	source.sidecar = source.source + kImportSidecarSuffix;
	source.importer = importer->id;
	source.output_dir = import_output_dir(paths_, source.source);
	const std::string sidecar_path = (root_ / source.sidecar).generic_string();
	ImportSidecar sidecar;
	Diagnostic error;
	if (!load_import_sidecar(sidecar_path, sidecar, error)) {
		// No record: the file is not an import source (a PNG without one is a
		// texture the game loads as it is); importing it writes the record
		// (import_assets).
		if (error.code().empty()) return;
		// A record that is there but does not read (a hand edit with a typo) is the
		// author's: reported and left as it is, never replaced by the defaults, and its
		// source is not imported until it reads again. What this machine knew of the
		// source stays cached, so a record fixed back to what it was imports nothing.
		error.asset = source.sidecar;
		result_.diagnostics.push_back(error);
		if (const auto known = cache_.find(source.source); known != cache_.end()) seen_[source.source] = known->second;
		source.ok = false;
		result_.sources.push_back(std::move(source));
		return;
	}
	const ImportSidecar recorded = sidecar;

	// The content hash: the cache vouches for it while the size and the time hold.
	CacheEntry now;
	now.size = uint64_t(fs::file_size(path, ec));
	now.modified = io::file_modified_ticks(path);
	const auto cached = cache_.find(source.source);
	std::vector<uint8_t> bytes;
	bool have_bytes = false;
	const auto read_source = [&]() {
		std::string message;
		if (!read_file_bytes(path.generic_string(), bytes, message)) {
			result_.diagnostics.push_back(make_finding(CoreFinding::ImportRead, DiagnosticSeverity::Error, message, source.source));
			return false;
		}
		spent += bytes.size();
		have_bytes = true;
		return true;
	};
	if (cached != cache_.end() && now.modified != 0 && cached->second.size == now.size &&
	    cached->second.modified == now.modified) {
		now.hash = cached->second.hash;
	} else {
		if (!read_source()) {
			source.ok = false;
			result_.sources.push_back(std::move(source));
			return;
		}
		now.hash = io::fnv1a64_bytes(io::kFnv1a64Offset, bytes.data(), bytes.size());
	}
	now.record = cached == cache_.end() ? 0 : cached->second.record;

	bool outputs_present = !sidecar.outputs.empty();
	for (const std::string &output : sidecar.outputs)
		if (!fs::is_regular_file(root_ / source.output_dir / output, ec)) outputs_present = false;
	const bool stale = sidecar.importer != importer->id || sidecar.version != importer->version ||
	                   now.hash != sidecar.source_hash || !outputs_present || now.record == 0 ||
	                   now.record != import_sidecar_fingerprint(sidecar) ||
	                   (force_ && import_source_named(only_, source.source));
	if (stale && (have_bytes || read_source())) {
		ImportProduct product;
		const bool ok = importer->run(filename, bytes, sidecar.options, product);
		for (Diagnostic &d : product.diagnostics) {
			if (d.asset.empty() || d.asset == filename) d.asset = source.source;
			result_.diagnostics.push_back(d);
		}
		if (!ok) {
			source.ok = false;
		} else {
			const fs::path out_dir = root_ / source.output_dir;
			std::string dir_error;
			if (!ensure_project_cache_dir(paths_, dir_error) || !ensure_directory(out_dir.generic_string(), dir_error)) {
				result_.diagnostics.push_back(make_finding(CoreFinding::ImportWrite, DiagnosticSeverity::Error, dir_error, source.source));
				source.ok = false;
			} else {
				// Outputs the last import wrote and this one did not are removed.
				for (const std::string &old : sidecar.outputs) {
					bool kept = false;
					for (const ImportOutput &output : product.outputs) if (output.name == old) kept = true;
					if (!kept) fs::remove(out_dir / old, ec);
				}
				sidecar.outputs.clear();
				for (const ImportOutput &output : product.outputs) {
					std::string write_error;
					if (!write_file_atomic((out_dir / output.name).generic_string(), output.bytes.data(), output.bytes.size(),
					                       write_error)) {
						result_.diagnostics.push_back(make_finding(CoreFinding::ImportWrite, DiagnosticSeverity::Error, write_error, source.source));
						source.ok = false;
						break;
					}
					spent += output.bytes.size();
					sidecar.outputs.push_back(output.name);
				}
			}
		}
		if (source.ok) {
			sidecar.importer = importer->id;
			sidecar.version = importer->version;
			sidecar.source_hash = now.hash;
			// The committed record changes only when what it says changed.
			if (sidecar != recorded && !save_import_sidecar(sidecar_path, sidecar, error)) {
				error.asset = source.sidecar;
				result_.diagnostics.push_back(error);
				source.ok = false;
			}
		}
		if (source.ok) {
			now.record = import_sidecar_fingerprint(sidecar);
			source.reimported = true;
			++result_.reimported;
		}
	} else if (stale) {
		source.ok = false; // the read failed: its finding is listed
	}
	seen_[source.source] = now;
	for (const std::string &output : sidecar.outputs)
		source.outputs.push_back((fs::path(source.output_dir) / output).generic_string());
	result_.sources.push_back(std::move(source));
}

ImportRunResult ImportPass::take() {
	return done() ? std::move(result_) : ImportRunResult();
}

} // namespace opennova::editor
