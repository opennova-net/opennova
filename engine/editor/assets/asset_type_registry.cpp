#include <editor/assets/asset_type_registry.h>

#include <filesystem>

#include <base/io/strutil.h>
#include <base/resource_index/resource_kind.h>
#include <editor/assets/asset_kinds.h>
#include <runtime/renderer/material_texture.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

std::string lower_basename(const std::string &logical_name) {
	return strutil::to_lower(fs::path(logical_name).filename().string());
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
	const AssetKind named = classify_asset(logical_name, nullptr);
	// A PNG is a texture the game loads as it is unless an import record makes it a source
	// (scan_project_assets): its name fits either. A name no extension types may be a
	// texture by its content (a material chunk container, classify_asset).
	return named == kind || (named == AssetKind::ImageSource && kind == AssetKind::Texture) ||
	       (named == AssetKind::Unknown && kind == AssetKind::Texture);
}

bool is_material_chunk_container(const std::vector<uint8_t> &bytes) {
	for (const uint8_t type : {uint8_t(16), uint8_t(17), uint8_t(18)})
		if (renderer::load_material_chunk(bytes.data(), bytes.size(), type)) return true;
	return false;
}

AssetKind classify_asset(const std::string &logical_name, const std::vector<uint8_t> *bytes) {
	// The runtime catalog's kinds by its own classifier, then the rows' own names (asset_kinds).
	const std::string shared = resource_kind_for_file(logical_name, bytes);
	if (!shared.empty()) return asset_kind_for_runtime(shared);
	const AssetKind named = asset_kind_for_name(logical_name);
	if (named == AssetKind::Unknown && bytes && is_material_chunk_container(*bytes)) return AssetKind::Texture;
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
