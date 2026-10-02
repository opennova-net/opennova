#include <editor/import/import_pass.h>

#include <algorithm>
#include <system_error>

#include <base/io/file_time.h>
#include <base/io/hash.h>
#include <base/io/json.h>
#include <editor/assets/project_scan.h>
#include <editor/import/import_context.h>
#include <editor/import/importer.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// 3: every file an import reads (a source, and since S13 A8 its inputs) by its path, and each
// source's record apart with the content hash of each input it lists; a cache of 2 kept a source's
// size, time, hash and record on one line.
constexpr int kImportCacheSchemaVersion = 3;

} // namespace

ImportPass::ImportPass(const ProjectPaths &paths, const ProjectDocument &project, bool force, std::string only,
		const std::vector<Importer> &table) :
		paths_(paths),
		project_(project),
		force_(force),
		only_(std::move(only)),
		table_(&table),
		root_(paths.root),
		export_dir_(paths.export_dir(project)) {}

// A cache that is missing, broken or of another schema reads as empty: every source
// is then imported again, which is all a lost cache costs.
ImportPass::Cache ImportPass::load_cache(const std::string &path, std::string &text) {
	Cache cache;
	std::string message;
	std::error_code ec;
	if (!fs::is_regular_file(system_path(path), ec) || !read_file_text(path, text, message)) return cache;
	io::JsonValue json;
	if (!io::json_parse(text, json, message) || !json.is_object() ||
	    json.get_int("schema_version", -1) != kImportCacheSchemaVersion)
		return cache;
	if (const io::JsonValue *files = json.get("files"); files && files->is_array())
		for (const io::JsonValue &item : files->array) {
			if (!item.is_object()) continue;
			const std::string file = item.get_string("path", "");
			FileSeen seen;
			uint64_t modified = 0;
			seen.size = uint64_t(item.get_number("size", 0));
			if (file.empty() || !io::parse_hex64(item.get_string("modified", ""), modified) ||
			    !io::parse_hex64(item.get_string("hash", ""), seen.hash))
				continue;
			seen.modified = int64_t(modified);
			cache.files[file] = seen;
		}
	if (const io::JsonValue *records = json.get("records"); records && records->is_array())
		for (const io::JsonValue &item : records->array) {
			Made made;
			const std::string source = item.is_object() ? item.get_string("source", "") : std::string();
			if (source.empty() || !io::parse_hex64(item.get_string("record", ""), made.record)) continue;
			bool read = true;
			if (const io::JsonValue *inputs = item.get("inputs"); inputs && inputs->is_array())
				for (const io::JsonValue &input : inputs->array) {
					uint64_t hash = 0;
					read = read && input.is_string() && io::parse_hex64(input.string, hash);
					made.inputs.push_back(hash);
				}
			if (read) cache.records[source] = std::move(made);
		}
	return cache;
}

// Written only when it changed: a pass over an untouched project writes nothing, and a
// project with no import sources and no cache yet (a clone opened for `status`) gets none. A file
// whose last write lies too near the pass (io::file_stamp_settled) is left out, read again next
// time: a rewrite of the same size inside its timestamp tick is never taken for the bytes read.
void ImportPass::save_cache() const {
	if (seen_.files.empty() && seen_.records.empty() && cache_text_.empty()) return;
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::JsonValue::make_number(kImportCacheSchemaVersion));
	io::JsonValue files = io::JsonValue::make_array();
	for (const auto &[file, seen] : seen_.files) {
		if (!io::file_stamp_settled(seen.modified, pass_began_)) continue;
		io::JsonValue item = io::JsonValue::make_object();
		item.set("path", io::JsonValue::make_string(file));
		item.set("size", io::JsonValue::make_number(double(seen.size)));
		item.set("modified", io::JsonValue::make_string(io::hex64(uint64_t(seen.modified))));
		item.set("hash", io::JsonValue::make_string(io::hex64(seen.hash)));
		files.push(std::move(item));
	}
	json.set("files", std::move(files));
	io::JsonValue records = io::JsonValue::make_array();
	for (const auto &[source, made] : seen_.records) {
		io::JsonValue item = io::JsonValue::make_object();
		item.set("source", io::JsonValue::make_string(source));
		item.set("record", io::JsonValue::make_string(io::hex64(made.record)));
		if (!made.inputs.empty()) {
			io::JsonValue inputs = io::JsonValue::make_array();
			for (const uint64_t hash : made.inputs) inputs.push(io::JsonValue::make_string(io::hex64(hash)));
			item.set("inputs", std::move(inputs));
		}
		records.push(std::move(item));
	}
	json.set("records", std::move(records));
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
			pass_began_ = io::file_clock_now_ticks();
			cache_ = load_cache(paths_.import_cache_file, cache_text_);
			std::error_code ec;
			// Through the system's paths (project_files.h): a source past MAX_PATH is found and read.
			walk_ = fs::recursive_directory_iterator(system_path(root_.generic_string()),
					fs::directory_options::skip_permission_denied, ec);
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
				if (is_dot_directory(path) || fs::equivalent(path, system_path(export_dir_.generic_string()), ec))
					walk_.disable_recursion_pending();
			} else if (entry.is_regular_file(ec) && importer_for(path.filename().string(), *table_)) {
				std::string relative = fs::relative(path, system_path(root_.generic_string()), ec).generic_string();
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

bool ImportPass::file_hash(const std::string &file, const std::string &relative, uint64_t &hash, uint64_t &spent) {
	std::error_code ec;
	const fs::path on_disk = system_path(file);
	if (!fs::is_regular_file(on_disk, ec)) return false;
	FileSeen now;
	now.size = uint64_t(fs::file_size(on_disk, ec));
	now.modified = io::file_modified_ticks(on_disk);
	// What this pass saw of it already (an input two sources read), else what the cache knew.
	const auto vouched = [&](const std::map<std::string, FileSeen> &files) {
		const auto known = files.find(relative);
		if (known == files.end() || now.modified == 0 || known->second.size != now.size ||
		    known->second.modified != now.modified)
			return false;
		now.hash = known->second.hash;
		return true;
	};
	if (!vouched(seen_.files) && !vouched(cache_.files)) {
		std::vector<uint8_t> bytes;
		std::string message;
		if (!read_file_bytes(file, bytes, message)) return false;
		spent += bytes.size();
		now.hash = io::fnv1a64_bytes(io::kFnv1a64Offset, bytes.data(), bytes.size());
	}
	seen_.files[relative] = now;
	hash = now.hash;
	return true;
}

void ImportPass::take_source(const fs::path &path, const std::string &relative, uint64_t &spent) {
	std::error_code ec;
	const std::string filename = path.filename().string();
	const Importer *importer = importer_for(filename, *table_);
	if (!importer || !fs::is_regular_file(path, ec)) return; // gone since it was listed
	ImportedSource source;
	source.source = relative;
	source.sidecar = source.source + kImportSidecarSuffix;
	source.importer = importer->id;
	source.output_dir = import_output_dir(paths_, source.source);
	const std::string sidecar_path = (root_ / source.sidecar).generic_string();
	// Where the files an import reads are named from: the source's folder, under the project's root as
	// given (the walk's path is the system's, which the context's checks would not find under it).
	const std::string folder = (root_ / relative).parent_path().generic_string();
	const std::string root = root_.generic_string();
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
		if (const auto known = cache_.files.find(source.source); known != cache_.files.end())
			seen_.files[source.source] = known->second;
		if (const auto known = cache_.records.find(source.source); known != cache_.records.end())
			seen_.records[source.source] = known->second;
		source.ok = false;
		result_.sources.push_back(std::move(source));
		return;
	}
	const ImportSidecar recorded = sidecar;

	// The content hash: the cache vouches for it while the size and the time hold.
	FileSeen now;
	now.size = uint64_t(fs::file_size(path, ec));
	now.modified = io::file_modified_ticks(path);
	const auto cached = cache_.files.find(source.source);
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
	if (cached != cache_.files.end() && now.modified != 0 && cached->second.size == now.size &&
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
	// What this machine made the outputs from (none on a clone: they are made again).
	const auto known = cache_.records.find(source.source);
	Made made = known == cache_.records.end() ? Made() : known->second;

	// The other files the last import read, each by the content hash it had when this machine made
	// the outputs (S13 A8): one gone, one outside the project, or one whose content moved makes the
	// import stale. Every one is looked at, so the cache keeps them all.
	bool inputs_current = made.inputs.size() == sidecar.inputs.size();
	for (size_t i = 0; i < sidecar.inputs.size(); ++i) {
		std::string file, input_relative;
		uint64_t hash = 0;
		if (!ImportContext::resolve(folder, root, sidecar.inputs[i], file, input_relative) ||
		    !file_hash(file, input_relative, hash, spent) || i >= made.inputs.size() || hash != made.inputs[i])
			inputs_current = false;
	}

	// The outputs live under the cache, deeper than the source: their checks take the system path.
	bool outputs_present = !sidecar.outputs.empty();
	for (const std::string &output : sidecar.outputs)
		if (!fs::is_regular_file(system_path((root_ / source.output_dir / output).generic_string()), ec))
			outputs_present = false;
	const bool stale = sidecar.importer != importer->id || sidecar.version != importer->version ||
	                   now.hash != sidecar.source_hash || !inputs_current || !outputs_present || made.record == 0 ||
	                   made.record != import_sidecar_fingerprint(sidecar) ||
	                   (force_ && import_source_named(only_, source.source));
	if (stale && (have_bytes || read_source())) {
		ImportContext context(filename, bytes, sidecar.options, folder, root);
		ImportProduct product;
		const bool ok = importer->run(context, product);
		spent += context.bytes_read();
		// What the importer read is known whatever it came to: the cache keeps each input.
		for (size_t i = 0; i < context.inputs().size(); ++i) {
			const ImportInputStamp &stamp = context.stamps()[i];
			seen_.files[stamp.relative] = FileSeen{stamp.size, stamp.modified, context.inputs()[i].hash};
		}
		std::vector<Diagnostic> findings = context.findings();
		findings.insert(findings.end(), product.diagnostics.begin(), product.diagnostics.end());
		for (Diagnostic &d : findings) {
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
					if (!kept) fs::remove(system_path((out_dir / old).generic_string()), ec);
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
			// The record lists the inputs' paths alone; their hashes are this machine's, the cache's.
			sidecar.inputs.clear();
			for (const ImportInput &input : context.inputs()) sidecar.inputs.push_back(input.path);
			// The committed record changes only when what it says changed.
			if (sidecar != recorded && !save_import_sidecar(sidecar_path, sidecar, error)) {
				error.asset = source.sidecar;
				result_.diagnostics.push_back(error);
				source.ok = false;
			}
		}
		if (source.ok) {
			made.record = import_sidecar_fingerprint(sidecar);
			made.inputs.clear();
			for (const ImportInput &input : context.inputs()) made.inputs.push_back(input.hash);
			source.reimported = true;
			++result_.reimported;
		}
	} else if (stale) {
		source.ok = false; // the read failed: its finding is listed
	}
	seen_.files[source.source] = now;
	if (made.record != 0) seen_.records[source.source] = made;
	for (const std::string &input : sidecar.inputs) {
		std::string file, input_relative;
		if (ImportContext::resolve(folder, root, input, file, input_relative)) source.inputs.push_back(input_relative);
	}
	for (const std::string &output : sidecar.outputs)
		source.outputs.push_back((fs::path(source.output_dir) / output).generic_string());
	result_.sources.push_back(std::move(source));
}

ImportRunResult ImportPass::take() {
	return done() ? std::move(result_) : ImportRunResult();
}

} // namespace opennova::editor
