#include <editor/assets/asset_type_registry.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <base/io/strutil.h>
#include <base/resource_index/resource_kind.h>
#include <editor/assets/asset_kinds.h>
#include <editor/import/importer.h>
#include <editor/project/project_files.h>
#include <runtime/renderer/material_texture.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

std::string lower_basename(const std::string &logical_name) {
	return strutil::to_lower(basename_of(logical_name));
}

} // namespace

bool asset_classification_needs_bytes(const std::string &logical_name) {
	return resource_extension_for_name(logical_name) == ".bin";
}

bool asset_name_fits_kind(const std::string &logical_name, AssetKind kind) {
	// A `.bin` name is what its content makes it, or the kind a row knows the whole name by
	// (CC.BIN's).
	if (asset_classification_needs_bytes(logical_name))
		return kind == AssetKind::Strings || kind == AssetKind::MusicScript ||
		       kind == AssetKind::RawBin || kind == asset_kind_for_name(logical_name);
	// A file an importer converts is an import source while its import record is there
	// (scan_project_assets), whatever its name makes it otherwise (a PNG a texture): its name fits
	// either.
	if (kind == AssetKind::ImportSource) return importer_for(logical_name) != nullptr;
	const AssetKind named = classify_asset(logical_name, nullptr);
	// A name no rule types may be a material chunk by its content, one with no extension the project's
	// notes (classify_asset).
	return named == kind || (named == AssetKind::Unknown && kind == AssetKind::MaterialChunk) ||
	       (named == AssetKind::Unknown && kind == AssetKind::Notes && resource_extension_for_name(logical_name).empty());
}

bool looks_like_text(const uint8_t *data, size_t size) {
	for (size_t i = 0; i < size; ++i) {
		const uint8_t c = data[i];
		if (c >= 0x20 || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == 0x1A) continue;
		return false;
	}
	return true;
}

bool is_text_file(const std::string &path, uint64_t &read) {
	std::ifstream in(system_path(path), std::ios::binary);
	if (!in) return false;
	char head[kTextSniffBytes];
	in.read(head, static_cast<std::streamsize>(sizeof(head)));
	const size_t got = static_cast<size_t>(in.gcount());
	read += got;
	return looks_like_text(reinterpret_cast<const uint8_t *>(head), got);
}

bool is_material_chunk_container(const std::vector<uint8_t> &bytes) {
	for (const uint8_t type : {uint8_t(16), uint8_t(17), uint8_t(18)})
		if (renderer::load_material_chunk(bytes.data(), bytes.size(), type)) return true;
	return false;
}

bool is_material_chunk_file(const std::string &path, uint64_t &read) {
	std::error_code ec;
	const fs::path file = system_path(path);
	const uint64_t size = fs::file_size(file, ec);
	if (ec) return false;
	std::ifstream in(file, std::ios::binary);
	if (!in) return false;
	size_t reads = 0;
	const renderer::MaterialChunkReader chunk = [&](uint64_t at, uint8_t *out, size_t n) {
		// A container walked past kChunkHeaderReads headers is not taken for one: a file of another
		// kind whose bytes read as many empty chunks costs no more than that.
		if (++reads > kChunkHeaderReads || at > size || n > size - at) return false;
		in.clear();
		in.seekg(static_cast<std::streamoff>(at));
		in.read(reinterpret_cast<char *>(out), static_cast<std::streamsize>(n));
		read += static_cast<uint64_t>(in.gcount());
		return static_cast<size_t>(in.gcount()) == n;
	};
	for (const uint8_t type : {uint8_t(16), uint8_t(17), uint8_t(18)}) {
		reads = 0;
		if (renderer::material_chunk_loads(size, chunk, type)) return true;
	}
	return false;
}

AssetKind classify_asset(const std::string &logical_name, const std::vector<uint8_t> *bytes) {
	// The runtime catalog's kinds by its own classifier, then the rows' own names (asset_kinds).
	const std::string shared = resource_kind_for_file(logical_name, bytes);
	if (!shared.empty()) return asset_kind_for_runtime(shared);
	const AssetKind named = asset_kind_for_name(logical_name);
	if (named == AssetKind::Unknown && bytes && is_material_chunk_container(*bytes)) return AssetKind::MaterialChunk;
	// A name with no extension that holds text is a note (a LICENSE): the Notes row's comment.
	if (named == AssetKind::Unknown && bytes && resource_extension_for_name(logical_name).empty() &&
	    looks_like_text(bytes->data(), std::min(bytes->size(), kTextSniffBytes)))
		return AssetKind::Notes;
	return named;
}

AssetKind expected_asset_kind_for_required_name(const std::string &name) {
	const std::string extension = resource_extension_for_name(name);
	if (extension != ".bin") return classify_asset(name, nullptr);
	// The witnessed `.bin` rows: a kind a row knows by its whole name (CC.BIN, the country
	// code), the music-script pair (and its expansion forms), the three raw markers/credential
	// stores, and string tables for everything else.
	const AssetKind named = asset_kind_for_name(name);
	if (named != AssetKind::RawBin && named != AssetKind::Unknown) return named;
	const std::string basename = lower_basename(name);
	if (basename == "menumus.bin" || basename == "gamemus.bin") return AssetKind::MusicScript;
	if (basename == "fgn2.bin" || basename == "epass.bin" || basename == "passgen.bin")
		return AssetKind::RawBin;
	return AssetKind::Strings;
}

} // namespace opennova::editor
