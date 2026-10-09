#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/assets/asset_kind.h>

namespace opennova::editor {

// The editor's file classifier (ADR 0046 d7/d9): the engine's (base/resource_index/file_kind.h,
// file_kind_for_file: the runtime catalog's shared classifier first, a kind it gives being the
// kind whose facts name its catalog token, and only then the facts' own names: the name-keyed .def
// family and score.ini by their whole names, then the extensions (scripts, textures with a model's
// .mdt normal map, waves, banks, videos, plain text)). `bytes` is the file's content when the
// caller has it (null otherwise): it decides a `.bin` name (RTXT vs SCR0 vs raw), and a name no
// extension types is a material chunk when it holds a material chunk container
// (renderer::is_material_chunk_container), and a name with no extension at all (a LICENSE) the
// project's notes when it holds text (strutil::looks_like_text).
AssetKind classify_asset(const std::string &logical_name, const std::vector<uint8_t> *bytes);

// strutil::looks_like_text's answer for the file at `path`, read by its first
// strutil::kTextSniffBytes alone; `read` grows by the bytes read. The scan types a file with no
// extension by it (the project's notes, else of no kind).
bool is_text_file(const std::string &path, uint64_t &read);

// True when classify_asset needs the whole content to decide (a `.bin` name). A name no
// extension types is left unknown without its content: the scan asks such a file's chunk
// headers alone (renderer::is_material_chunk_file: S13 A8, the build packs a material chunk and
// leaves a file of no kind out), an import and the import plan, which have the bytes, pass them.
bool asset_classification_needs_bytes(const std::string &logical_name);

// True when a file of that name is of `kind` by its name alone: what classify_asset makes
// of it, for a `.bin` name (whose content decides) any of the table kinds it can be, an import
// source for a name an importer converts (the scan's kind for one with its import record), for
// a name no rule types a material chunk too (a chunk container, by its content), and for a name
// with no extension the project's notes too (a text, by its content).
bool asset_name_fits_kind(const std::string &logical_name, AssetKind kind);

// The kind a required-resource row's file name implies without reading anything, what a
// requirement compares a scanned file against, is the engine's (file_kind_for_required_name).

} // namespace opennova::editor
