// Texture reference -> candidate file names: see texture_candidates.h. Our
// loose-folder resolver policy (retail reads archives by exact uppercase
// name), so there is no original function to cite.
#include "texture_candidates.h"

#include <algorithm>

namespace opennova {

namespace {

constexpr const char *kExtensionPriority[] = {"tga", "dds", "dds.tga", "mdt", "pcx", "png", "jpg", "jpeg", "bmp"};

std::string lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
	return s;
}

// The file part of a path (after the last '/' or '\').
std::string file_of(const std::string &path) {
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The text after the last '.' of a file name ("" without one).
std::string extension_of(const std::string &file) {
	const size_t dot = file.rfind('.');
	return dot == std::string::npos ? std::string() : file.substr(dot + 1);
}

// The file name without its last extension.
std::string basename_of(const std::string &file) {
	const size_t dot = file.rfind('.');
	return dot == std::string::npos ? file : file.substr(0, dot);
}

void append_unique(std::vector<std::string> &items, const std::string &value) {
	if (value.empty() || std::find(items.begin(), items.end(), value) != items.end()) return;
	items.push_back(value);
}

bool is_texture_extension(const std::string &extension) {
	const std::string ext = lower(extension);
	if (ext.empty()) return false;
	for (const char *candidate : kExtensionPriority)
		if (ext == candidate) return true;
	return false;
}

} // namespace

std::vector<std::string> texture_candidate_filenames(const std::string &filename) {
	std::vector<std::string> candidates;
	const std::string file = file_of(filename);
	if (file.empty()) return candidates;
	append_unique(candidates, file);
	// Compound extensions: "x.dds.tga" also tries "x.dds".
	if (is_texture_extension(extension_of(file))) {
		std::string collapsed = file;
		while (true) {
			collapsed = basename_of(collapsed);
			if (!is_texture_extension(extension_of(collapsed))) break;
			append_unique(candidates, collapsed);
		}
	}
	std::vector<std::string> stems;
	const std::string stem = basename_of(file);
	if (!stem.empty()) {
		append_unique(stems, stem);
		// NovaLogic outline/overlay textures append an "_O" suffix to the base name.
		const std::string lower_stem = lower(stem);
		if (lower_stem.size() < 2 || lower_stem.compare(lower_stem.size() - 2, 2, "_o") != 0) append_unique(stems, stem + "_O");
	}
	for (const std::string &s : stems)
		for (const char *ext : kExtensionPriority) append_unique(candidates, s + "." + ext);
	return candidates;
}

} // namespace opennova
