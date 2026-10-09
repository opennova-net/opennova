#pragma once

#include <cstddef>
#include <string>

#include <base/resource_index/file_kind.h>

namespace opennova::editor {

// What a project file IS to the engine (ADR 0046 d6/d7): the engine's one kind vocabulary
// (base/resource_index/file_kind.h, FileKind), whose row of facts says what the game's loaders
// know of each kind (its names, its archive, its place under an expansion, its line reader). What
// each kind is to the editor (its token, words, document type, folder) is its row in
// assets/asset_kinds: a new kind is one FileKind value and one row in each table.
using AssetKind = FileKind;

inline constexpr size_t kAssetKindCount = kFileKindCount;

// The stable lower-case token for a kind ("mission", "item_defs"); "unknown" for Unknown
// (its row's, assets/asset_kinds).
const char *asset_kind_token(AssetKind kind);

// The user-facing label ("Mission", "Item definitions").
const char *asset_kind_label(AssetKind kind);

// The kind a token names; Unknown when no kind carries it.
AssetKind asset_kind_from_token(const std::string &token);

} // namespace opennova::editor
