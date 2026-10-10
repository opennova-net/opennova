#include <editor/assets/asset_type_registry.h>

#include <algorithm>
#include <fstream>

#include <base/io/strutil.h>
#include <base/resource_index/file_kind.h>
#include <base/resource_index/resource_kind.h>
#include <editor/assets/asset_kinds.h>
#include <editor/import/importer.h>
#include <editor/project/project_files.h>
#include <runtime/renderer/material_texture.h>

namespace opennova::editor {

bool asset_classification_needs_bytes(const std::string &logical_name) {
	return resource_extension_for_name(logical_name) == ".bin";
}

bool asset_name_fits_kind(const std::string &logical_name, AssetKind kind) {
	// A `.bin` name is what its content makes it, or the kind a row knows the whole name by
	// (CC.BIN's).
	if (asset_classification_needs_bytes(logical_name))
		return kind == AssetKind::Strings || kind == AssetKind::MusicScript ||
		       kind == AssetKind::RawBin || kind == file_kind_for_name(logical_name);
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

bool is_text_file(const std::string &path, uint64_t &read) {
	std::ifstream in(system_path(path), std::ios::binary);
	if (!in) return false;
	char head[strutil::kTextSniffBytes];
	in.read(head, static_cast<std::streamsize>(sizeof(head)));
	const size_t got = static_cast<size_t>(in.gcount());
	read += got;
	return strutil::looks_like_text(reinterpret_cast<const uint8_t *>(head), got);
}

AssetKind classify_asset(const std::string &logical_name, const std::vector<uint8_t> *bytes) {
	// The runtime catalog's kinds by its own classifier, then the facts' own names (file_kind).
	const AssetKind named = file_kind_for_file(logical_name, bytes);
	if (named == AssetKind::Unknown && bytes && renderer::is_material_chunk_container(*bytes)) return AssetKind::MaterialChunk;
	// A name with no extension that holds text is a note (a LICENSE): the Notes row's comment.
	if (named == AssetKind::Unknown && bytes && resource_extension_for_name(logical_name).empty() &&
	    strutil::looks_like_text(bytes->data(), std::min(bytes->size(), strutil::kTextSniffBytes)))
		return AssetKind::Notes;
	return named;
}

} // namespace opennova::editor
