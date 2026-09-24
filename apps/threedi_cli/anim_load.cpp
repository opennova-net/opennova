// opennova-3di anim: loading a clip set — a `.adm` table plus every `.bad` it
// names, or a lone `.bad` — for `scene`, `info` and `compare`. The table's
// grammar and its variant rings are formats/adm's; the clips are formats/bad's.

#include "anim_cli.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>

#include <base/io/strutil.h>
#include <formats/adm/adm.h>

using namespace opennova::bad;

namespace threedi_cli {

namespace {

bool read_file(const std::string &path, std::vector<uint8_t> &out) {
	FILE *f = std::fopen(path.c_str(), "rb");
	if (f == nullptr) return false;
	std::fseek(f, 0, SEEK_END);
	const long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	out.assign(size > 0 ? static_cast<size_t>(size) : 0, 0);
	const size_t read = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
	std::fclose(f);
	return read == out.size();
}

bool same_name(const std::string &a, const std::string &b) {
	return opennova::strutil::iequals(a, b);
}

// The file a variant names, beside the table: as given, else the directory's
// case-insensitive match (a retail table names Dt1RunF.bad, the file is
// DT1RUNF.BAD).
std::string resolve(const std::filesystem::path &dir, const std::string &file) {
	std::error_code ec;
	const std::filesystem::path direct = dir / file;
	if (std::filesystem::exists(direct, ec)) return direct.string();
	for (const std::filesystem::directory_entry &entry :
			std::filesystem::directory_iterator(dir, ec)) {
		if (same_name(entry.path().filename().string(), file)) return entry.path().string();
	}
	return std::string();
}

} // namespace

std::string anim_clip_stem(const std::string &variant) {
	if (variant.size() > 4 && opennova::strutil::ends_with_icase(variant, ".bad"))
		return variant.substr(0, variant.size() - 4);
	return variant;
}

void anim_free(AnimLoadedSet &set) {
	for (AnimLoadedClip &clip : set.clips) bad_free(&clip.file);
	set.clips.clear();
}

bool anim_load(const std::string &path, AnimLoadedSet &out, std::string &error) {
	out = AnimLoadedSet{};
	const std::filesystem::path input = std::filesystem::absolute(std::filesystem::path(path));
	const std::filesystem::path dir = input.parent_path();
	const std::string ext = input.extension().string();

	std::vector<std::string> wanted; // stems, in table order
	if (opennova::strutil::iequals(ext, ".bad")) {
		out.table_path = std::string();
		wanted.push_back(input.stem().string());
		AnimLoadedClip clip;
		clip.name = wanted.back();
		clip.path = path;
		if (!read_file(clip.path, clip.bytes)) {
			error = "cannot read " + clip.path;
			return false;
		}
		if (bad_parse_buffer(clip.bytes.data(), clip.bytes.size(), &clip.file) != 0) {
			error = clip.path + " does not parse as a .bad clip";
			return false;
		}
		out.clips.push_back(std::move(clip));
		return true;
	}

	std::vector<uint8_t> bytes;
	if (!read_file(path, bytes)) {
		error = "cannot read " + path;
		return false;
	}
	opennova::adm::AdmFile table{};
	if (opennova::adm::adm_parse_buffer(reinterpret_cast<const char *>(bytes.data()), bytes.size(),
				&table) != 0) {
		error = path + " does not parse as a .adm table";
		return false;
	}
	out.table_name = input.filename().string();
	out.table_path = path;
	for (size_t i = 0; i < table.count; ++i) {
		const opennova::adm::AdmEntry &entry = table.entries[i];
		BadBuildRow row;
		row.key = entry.key;
		for (size_t v = 0; v < entry.variant_count; ++v) row.variants.push_back(entry.variants[v]);
		out.rows.push_back(row);
	}
	opennova::adm::adm_free(&table);

	for (const BadBuildRow &row : out.rows) {
		for (const std::string &variant : row.variants) {
			const std::string stem = anim_clip_stem(variant);
			const bool seen = std::any_of(out.clips.begin(), out.clips.end(),
					[&](const AnimLoadedClip &c) { return same_name(c.name, stem); });
			if (seen) continue;
			const std::string file = resolve(dir, stem + ".bad");
			if (file.empty()) {
				out.missing.push_back(variant);
				continue;
			}
			AnimLoadedClip clip;
			clip.name = stem;
			clip.path = file;
			if (!read_file(file, clip.bytes) ||
					bad_parse_buffer(clip.bytes.data(), clip.bytes.size(), &clip.file) != 0) {
				bad_free(&clip.file);
				out.missing.push_back(variant);
				continue;
			}
			out.clips.push_back(std::move(clip));
		}
	}
	return true;
}

} // namespace threedi_cli
