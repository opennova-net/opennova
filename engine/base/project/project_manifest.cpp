#include <base/project/project_manifest.h>

#include <algorithm>
#include <unordered_map>
#include <utility>

#include <base/io/cp1252.h>
#include <base/io/json.h>
#include <base/io/os_path.h>
#include <base/io/sha256.h>
#include <base/io/strutil.h>
#include <base/project/case_fold.h>

namespace opennova::project {

namespace {

bool is_refused_char(unsigned char c) {
	return c < 0x20 || c == 0x7F || c == '\\' || c == '<' || c == '>' || c == ':' || c == '"' ||
			c == '|' || c == '?' || c == '*';
}

} // namespace

bool is_sha256_hex(std::string_view text) {
	if (text.size() != 64)
		return false;
	for (const char c : text)
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return false;
	return true;
}

// The io layer's strict check (base/io/cp1252.h), the store's rule.
bool is_utf8(std::string_view text) {
	return opennova::is_valid_utf8(text);
}

bool is_service_managed_file(std::string_view path) {
	const size_t slash = path.find_last_of("/\\");
	const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
	return strutil::iequals(name, "gt.ssc");
}

bool is_project_path(std::string_view path, std::string &why) {
	if (path.empty()) {
		why = "a file's path is empty";
		return false;
	}
	if (path.size() > kMaxPathBytes) {
		why = "a path is longer than " + std::to_string(kMaxPathBytes) + " bytes";
		return false;
	}
	if (!is_utf8(path)) {
		why = "the path is not UTF-8";
		return false;
	}
	for (const char c : path)
		if (is_refused_char(static_cast<unsigned char>(c))) {
			why = "the path holds a character a Windows file name refuses";
			return false;
		}
	size_t start = 0;
	for (bool first = true;; first = false) {
		const size_t end = std::min(path.find('/', start), path.size());
		const std::string_view part = path.substr(start, end - start);
		if (part.empty() || part == "." || part == "..") {
			why = "the path has an empty, \".\" or \"..\" part";
			return false;
		}
		if (part.size() > kMaxPartBytes) {
			why = "a part of the path is longer than " + std::to_string(kMaxPartBytes) + " bytes";
			return false;
		}
		if (part.back() == '.' || part.back() == ' ') {
			why = "a part of the path ends in a dot or a space";
			return false;
		}
		if (io::is_windows_device_name(part)) {
			why = "a part of the path is a Windows device's name";
			return false;
		}
		if (first && strutil::iequals(part, ".opennova")) {
			why = "the path is under .opennova/, the machine's own folder";
			return false;
		}
		if (end == path.size())
			return true;
		start = end + 1;
	}
}

std::string folded_path(std::string_view path) {
	std::string out;
	out.reserve(path.size());
	for (size_t i = 0; i < path.size();) {
		const unsigned char lead = static_cast<unsigned char>(path[i]);
		const size_t length = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 0;
		uint32_t code = length == 1 ? lead : length == 2 ? lead & 0x1F : length == 3 ? lead & 0x0F : lead & 0x07;
		bool whole = length != 0 && i + length <= path.size();
		for (size_t k = 1; whole && k < length; ++k) {
			const unsigned char next = static_cast<unsigned char>(path[i + k]);
			whole = (next & 0xC0) == 0x80;
			code = (code << 6) | (next & 0x3F);
		}
		// A byte of no whole character (no project path holds one: is_utf8) stands as it is.
		if (!whole) {
			out += path[i++];
			continue;
		}
		opennova::utf8_append(out, simple_case_fold(code));
		i += length;
	}
	return out;
}

bool check_files(const std::vector<ProjectFile> &files, std::string &why) {
	std::unordered_map<std::string, const ProjectFile *> folded;
	folded.reserve(files.size());
	std::unordered_map<std::string_view, uint64_t> sizes; // sha256 -> size
	sizes.reserve(files.size());
	for (const ProjectFile &file : files) {
		std::string bad;
		if (!is_project_path(file.path, bad)) {
			why = "\"" + file.path + "\": " + bad;
			return false;
		}
		if (!is_sha256_hex(file.sha256)) {
			why = "\"" + file.path + "\": its sha256 is not 64 lower-case hex digits";
			return false;
		}
		if (file.size > kMaxFileBytes) {
			why = "\"" + file.path + "\": it is larger than 4 GiB";
			return false;
		}
		const auto [at, added] = folded.emplace(folded_path(file.path), &file);
		if (!added) {
			why = at->second->path == file.path ? "\"" + file.path + "\": listed twice"
												: "\"" + file.path + "\" and \"" +
							at->second->path + "\": one file on Windows and macOS";
			return false;
		}
		const auto [held, fresh] = sizes.emplace(file.sha256, file.size);
		if (!fresh && held->second != file.size) {
			why = "\"" + file.path + "\": its sha256 is listed at another size too";
			return false;
		}
	}
	// A file is never a folder of another path's: one of the two could not be laid out (and
	// "Maps" beside "maps/a.bms" is the same clash on Windows and macOS).
	for (const ProjectFile &file : files) {
		const std::string path = folded_path(file.path);
		for (size_t slash = path.find('/'); slash != std::string::npos;
				slash = path.find('/', slash + 1)) {
			const auto at = folded.find(path.substr(0, slash));
			if (at != folded.end()) {
				why = "\"" + at->second->path + "\" is a file, and a folder of \"" + file.path +
						"\"";
				return false;
			}
		}
	}
	return true;
}

std::string encode_manifest(std::vector<ProjectFile> files) {
	std::sort(files.begin(), files.end(),
			[](const ProjectFile &a, const ProjectFile &b) { return a.path < b.path; });
	io::JsonValue list = io::JsonValue::make_array();
	for (const ProjectFile &file : files) {
		io::JsonValue entry = io::JsonValue::make_object();
		entry.set("path", io::json_string(file.path));
		entry.set("sha256", io::json_string(file.sha256));
		entry.set("size", io::json_number(static_cast<double>(file.size)));
		list.push(std::move(entry));
	}
	io::JsonValue manifest = io::JsonValue::make_object();
	manifest.set("schema_version", io::json_number(kManifestSchemaVersion));
	manifest.set("files", std::move(list));
	return io::json_write(manifest);
}

bool decode_manifest(std::string_view text, std::vector<ProjectFile> &out, std::string &error) {
	out.clear();
	io::JsonValue manifest;
	if (!io::json_parse(text, manifest, error)) {
		error = "the manifest is not JSON: " + error;
		return false;
	}
	if (!manifest.is_object()) {
		error = "the manifest is not a JSON object";
		return false;
	}
	if (!io::json_members_known(manifest, { "files", "schema_version" }, "manifest", error))
		return false;
	if (manifest.get_number("schema_version", 0) != kManifestSchemaVersion) {
		error = "the manifest's schema_version is not " + std::to_string(kManifestSchemaVersion);
		return false;
	}
	const io::JsonValue *files = manifest.get("files");
	if (!files || !files->is_array()) {
		error = "the manifest has no files list";
		return false;
	}
	out.reserve(files->array.size());
	for (const io::JsonValue &entry : files->array) {
		const io::JsonValue *path = entry.get("path"), *sha = entry.get("sha256"),
							*size = entry.get("size");
		ProjectFile file;
		if (!entry.is_object() ||
				!io::json_members_known(entry, { "path", "sha256", "size" }, "file", error) ||
				!path || !path->is_string() || !sha || !sha->is_string() || !size ||
				!io::json_exact_whole(*size, file.size)) {
			if (error.empty())
				error = "a file of the manifest is not {path, sha256, size}";
			return false;
		}
		file.path = path->string;
		file.sha256 = sha->string;
		out.push_back(std::move(file));
	}
	if (!check_files(out, error))
		return false;
	if (encode_manifest(out) != text) {
		error = "the manifest is not in its canonical form (sorted by path, as encode_manifest "
				"writes it)";
		return false;
	}
	return true;
}

std::string manifest_sha256(std::string_view text) {
	return io::sha256_hex(text.data(), text.size());
}

uint64_t total_bytes(const std::vector<ProjectFile> &files) {
	uint64_t bytes = 0;
	for (const ProjectFile &file : files)
		bytes += file.size;
	return bytes;
}

} // namespace opennova::project
