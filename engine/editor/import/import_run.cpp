#include <editor/import/import_run.h>

#include <filesystem>
#include <map>
#include <system_error>

#include <base/io/file_time.h>
#include <base/io/hash.h>
#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/import/importer.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// 2: the content hashes are FNV-1a over its true offset basis (S12 A5); a cache of 1 holds
// digests made with the old constant, which a size and time that still match would keep.
constexpr int kImportCacheSchemaVersion = 2;

// What this machine last saw of one source (ADR 0046 d6, S9c): its size and last-write
// time with the content hash they vouch for, and the fingerprint of the import record
// the outputs under the cache were made from. Machine-local and disposable, so the
// committed sidecar never carries a time a checkout changes.
struct CacheEntry {
	uint64_t size = 0;
	int64_t modified = 0; // the file system's own clock ticks: compared, never shown
	uint64_t hash = 0;
	uint64_t record = 0; // 0 = no outputs made on this machine yet
};

using ImportCache = std::map<std::string, CacheEntry>; // by project-relative source path

// A cache that is missing, broken or of another schema reads as empty: every source
// is then imported again, which is all a lost cache costs.
ImportCache load_import_cache(const std::string &path, std::string &text) {
	ImportCache cache;
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
void save_import_cache(const ProjectPaths &paths, const ImportCache &cache, const std::string &previous_text) {
	if (cache.empty() && previous_text.empty()) return;
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::JsonValue::make_number(kImportCacheSchemaVersion));
	io::JsonValue sources = io::JsonValue::make_array();
	for (const auto &[source, entry] : cache) {
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
	if (text == previous_text) return;
	std::string message;
	if (ensure_project_cache_dir(paths, message)) write_file_atomic(paths.import_cache_file, text, message);
}

} // namespace

bool import_source_named(const std::string &only, const std::string &source_relative_path) {
	if (only.empty()) return true;
	const std::string wanted = strutil::to_lower(only);
	return wanted == strutil::to_lower(source_relative_path) ||
	       wanted == strutil::to_lower(basename_of(source_relative_path));
}

std::string import_output_dir(const ProjectPaths &paths, const std::string &source_relative_path) {
	const std::string key = strutil::to_lower(source_relative_path);
	const uint64_t hash = io::fnv1a64_bytes(io::kFnv1a64Offset, key.data(), key.size());
	const fs::path dir = fs::relative(fs::path(paths.imported_dir), fs::path(paths.root)) / io::hex64(hash);
	return dir.generic_string();
}

ImportRunResult run_imports(const ProjectPaths &paths, const ProjectDocument &project, bool force, const std::string &only) {
	ImportRunResult result;
	const fs::path root(paths.root);
	const fs::path export_dir(paths.export_dir(project));
	std::string cache_text;
	const ImportCache cache = load_import_cache(paths.import_cache_file, cache_text);
	ImportCache seen; // the sources this pass found: a gone source leaves the cache
	std::error_code ec;
	fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
	if (ec) return result;
	const fs::recursive_directory_iterator end;
	for (; it != end; it.increment(ec)) {
		if (ec) break;
		const fs::directory_entry &entry = *it;
		const fs::path &path = entry.path();
		if (entry.is_directory(ec)) {
			if (is_dot_directory(path) || fs::equivalent(path, export_dir, ec)) it.disable_recursion_pending();
			continue;
		}
		if (!entry.is_regular_file(ec)) continue;
		const std::string filename = path.filename().string();
		const Importer *importer = importer_for(filename);
		if (!importer) continue;
		ImportedSource source;
		source.source = fs::relative(path, root, ec).generic_string();
		if (ec) source.source = filename;
		source.sidecar = source.source + kImportSidecarSuffix;
		source.importer = importer->id;
		source.output_dir = import_output_dir(paths, source.source);
		const std::string sidecar_path = (root / source.sidecar).generic_string();
		ImportSidecar sidecar;
		Diagnostic error;
		if (!load_import_sidecar(sidecar_path, sidecar, error)) {
			// No record: the file is not an import source (a PNG without one is a
			// texture the game loads as it is); importing it writes the record
			// (import_assets).
			if (error.code.empty()) continue;
			// A record that is there but does not read (a hand edit with a typo) is the
			// author's: reported and left as it is, never replaced by the defaults, and its
			// source is not imported until it reads again. What this machine knew of the
			// source stays cached, so a record fixed back to what it was imports nothing.
			error.asset = source.sidecar;
			result.diagnostics.push_back(error);
			if (const auto known = cache.find(source.source); known != cache.end()) seen[source.source] = known->second;
			source.ok = false;
			result.sources.push_back(std::move(source));
			continue;
		}
		const ImportSidecar recorded = sidecar;

		// The content hash: the cache vouches for it while the size and the time hold.
		CacheEntry now;
		now.size = uint64_t(fs::file_size(path, ec));
		now.modified = io::file_modified_ticks(path);
		const auto cached = cache.find(source.source);
		std::vector<uint8_t> bytes;
		bool have_bytes = false;
		const auto read_source = [&]() {
			std::string message;
			if (!read_file_bytes(path.generic_string(), bytes, message)) {
				result.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "import.read", message, source.source));
				return false;
			}
			have_bytes = true;
			return true;
		};
		if (cached != cache.end() && now.modified != 0 && cached->second.size == now.size &&
		    cached->second.modified == now.modified) {
			now.hash = cached->second.hash;
		} else {
			if (!read_source()) {
				source.ok = false;
				result.sources.push_back(std::move(source));
				continue;
			}
			now.hash = io::fnv1a64_bytes(io::kFnv1a64Offset, bytes.data(), bytes.size());
		}
		now.record = cached == cache.end() ? 0 : cached->second.record;

		bool outputs_present = !sidecar.outputs.empty();
		for (const std::string &output : sidecar.outputs)
			if (!fs::is_regular_file(root / source.output_dir / output, ec)) outputs_present = false;
		const bool stale = sidecar.importer != importer->id || sidecar.version != importer->version ||
		                   now.hash != sidecar.source_hash || !outputs_present || now.record == 0 ||
		                   now.record != import_sidecar_fingerprint(sidecar) ||
		                   (force && import_source_named(only, source.source));
		if (stale && (have_bytes || read_source())) {
			ImportProduct product;
			const bool ok = importer->run(filename, bytes, sidecar.options, product);
			for (Diagnostic &d : product.diagnostics) {
				if (d.asset.empty() || d.asset == filename) d.asset = source.source;
				result.diagnostics.push_back(d);
			}
			if (!ok) {
				source.ok = false;
			} else {
				const fs::path out_dir = root / source.output_dir;
				std::string dir_error;
				if (!ensure_project_cache_dir(paths, dir_error) || !ensure_directory(out_dir.generic_string(), dir_error)) {
					result.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "import.write", dir_error, source.source));
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
							result.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "import.write", write_error, source.source));
							source.ok = false;
							break;
						}
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
					result.diagnostics.push_back(error);
					source.ok = false;
				}
			}
			if (source.ok) {
				now.record = import_sidecar_fingerprint(sidecar);
				source.reimported = true;
				++result.reimported;
			}
		} else if (stale) {
			source.ok = false; // the read failed: its finding is listed
		}
		seen[source.source] = now;
		for (const std::string &output : sidecar.outputs)
			source.outputs.push_back((fs::path(source.output_dir) / output).generic_string());
		result.sources.push_back(std::move(source));
	}
	save_import_cache(paths, seen, cache_text);
	return result;
}

} // namespace opennova::editor
