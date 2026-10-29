#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace opennova::editor {

// A catalog of the native files of one immutable release export. An expansion's own
// files override its separately exported base; only the own layer supplies catalog roots.
// The graph reads both layers in full, including outgoing references of inherited files.
// No file is renamed or rewritten. Each root's closure keeps its native names and origin.
struct NativeAssetCatalogOptions {
	std::string project_export;
	std::string base_export;
	std::string game;
	std::string expansion;
	std::string output;
	// Alternative to base_export: resolve the already-exported standalone project's output.
	std::string base_project;
};

struct NativeAssetCatalogResult {
	size_t assets = 0;
	// Roots whose dependency closure could not be established, with the reason.
	std::vector<std::string> omitted;
};

// Writes manifest.json (schema_version 1, assets[{name,kind,sha256,size,files}]) and
// blobs/<sha256>. Each files row is {name,sha256,size,origin:"project"|"base"}.
// Includes models, animations, animation maps, terrains and textures. Missing,
// unverified or unreadable required dependencies omit the root, not the whole catalog.
// Output must not exist or be empty. False for an invalid export or an I/O failure;
// no manifest is published until every referenced blob has been written.
bool write_native_asset_catalog(const NativeAssetCatalogOptions &options,
		NativeAssetCatalogResult &result, std::string &error);

} // namespace opennova::editor
