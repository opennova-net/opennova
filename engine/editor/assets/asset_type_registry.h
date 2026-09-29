#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/assets/asset_kind.h>

namespace opennova::editor {

// The editor's file classifier (ADR 0046 d7/d9). It asks the runtime catalog's shared
// classifier first (engine/base/resource_index/resource_kind.h) and only then applies
// the editor-only rules: the name-keyed .def family, scripts, textures (a model's .mdt
// normal map among them), banks, videos, plain text. `bytes` is the file's content when
// the caller has it (null otherwise): it decides a `.bin` name (RTXT vs SCR0 vs raw), and a
// name no extension types is a texture when it holds a material chunk container.
AssetKind classify_asset(const std::string &logical_name, const std::vector<uint8_t> *bytes);

// True when classify_asset needs the content to decide (a `.bin` name). A name no extension
// types is left unknown without its content: the scan does not read such a file (every save
// scans again), an import and the import plan, which have the bytes, pass them.
bool asset_classification_needs_bytes(const std::string &logical_name);

// Bytes a model's chunk row reads (runtime types 16 to 18): the runtime's own chunk
// loader takes them as one of its three chunks [orig: NQ8B @ 0x58F350; HRZ8 @ 0x58F470;
// AOC8 @ 0x58F590] (renderer::load_material_chunk), whatever the file is named.
bool is_material_chunk_container(const std::vector<uint8_t> &bytes);

// True when a file of that name is of `kind` by its name alone: what classify_asset makes
// of it, for a `.bin` name (whose content decides) any of the table kinds it can be, for
// a `.png` a texture too (the scan's kind for one no import record makes a source), and for
// a name no extension types a texture too (a chunk container, by its content).
bool asset_name_fits_kind(const std::string &logical_name, AssetKind kind);

// The kind a required-resource row's file name implies without reading anything:
// the `.bin` rows are string tables except the music scripts and the raw markers,
// which are named. This is what a requirement compares a scanned file against.
AssetKind expected_asset_kind_for_required_name(const std::string &name);

} // namespace opennova::editor
