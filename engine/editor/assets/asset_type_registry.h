#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/assets/asset_kind.h>

namespace opennova::editor {

// The editor's file classifier (ADR 0046 d7/d9). It asks the runtime catalog's shared
// classifier first (engine/base/resource_index/resource_kind.h) and only then applies
// the editor-only rules: the name-keyed .def family, scripts, textures, banks, videos,
// plain text. `bytes` is the file's content when the caller has it; it is consulted
// only for `.bin` names (RTXT vs SCR0 vs raw) and may be null otherwise.
AssetKind classify_asset(const std::string &logical_name, const std::vector<uint8_t> *bytes);

// True when classify_asset needs the content to decide (a `.bin` name).
bool asset_classification_needs_bytes(const std::string &logical_name);

// The kind a required-resource row's file name implies without reading anything:
// the `.bin` rows are string tables except the music scripts and the raw markers,
// which are named. This is what a requirement compares a scanned file against.
AssetKind expected_asset_kind_for_required_name(const std::string &name);

} // namespace opennova::editor
