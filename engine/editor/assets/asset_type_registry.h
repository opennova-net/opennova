#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/assets/asset_kind.h>

namespace opennova::editor {

// The editor's file classifier (ADR 0046 d7/d9). It asks the runtime catalog's shared
// classifier first (engine/base/resource_index/resource_kind.h), a kind it gives being the
// row of asset_kinds that names its catalog token, and only then the rows' own names: the
// name-keyed .def family and score.ini by their whole names, then the extensions (scripts,
// textures with a model's .mdt normal map, waves, banks, videos, plain text). `bytes` is
// the file's content when the caller has it (null otherwise): it decides a `.bin` name (RTXT
// vs SCR0 vs raw), and a name no extension types is a material chunk when it holds a material
// chunk container, and a name with no extension at all (a LICENSE) the project's notes when it
// holds text (looks_like_text).
AssetKind classify_asset(const std::string &logical_name, const std::vector<uint8_t> *bytes);

// Whether bytes read as text a person wrote: no NUL and no control character but a tab, a line
// end, a form feed or the DOS end-of-file mark (UTF-8 or a code page alike). The first
// kTextSniffBytes of a file decide it.
inline constexpr size_t kTextSniffBytes = 4096;
bool looks_like_text(const uint8_t *data, size_t size);
// The same answer for the file at `path`, read by its first kTextSniffBytes alone; `read` grows by
// the bytes read. The scan types a file with no extension by it (the project's notes, else of no kind).
bool is_text_file(const std::string &path, uint64_t &read);

// True when classify_asset needs the whole content to decide (a `.bin` name). A name no
// extension types is left unknown without its content: the scan asks such a file's chunk
// headers alone (is_material_chunk_file), an import and the import plan, which have the bytes,
// pass them.
bool asset_classification_needs_bytes(const std::string &logical_name);

// Bytes a model's chunk row reads (runtime types 16 to 18): the runtime's own chunk
// loader takes them as one of its three chunks [orig: NQ8B @ 0x58F350; HRZ8 @ 0x58F470;
// AOC8 @ 0x58F590] (renderer::load_material_chunk), whatever the file is named.
bool is_material_chunk_container(const std::vector<uint8_t> &bytes);
// The same answer for the file at `path`, read by its chunk headers alone
// (renderer::material_chunk_loads: the 8-byte header, each chunk's tag and size, the found chunk's
// first 28 bytes), so a large file of another kind costs a few small reads, kChunkHeaderReads at
// most for each of the three chunks (a file whose bytes read as more empty chunks than that is not
// taken for a container); `read` grows by the bytes read. The scan types a file no rule names by it
// (S13 A8: the build packs a material chunk and leaves a file of no kind out).
inline constexpr size_t kChunkHeaderReads = 1024;
bool is_material_chunk_file(const std::string &path, uint64_t &read);

// True when a file of that name is of `kind` by its name alone: what classify_asset makes
// of it, for a `.bin` name (whose content decides) any of the table kinds it can be, an import
// source for a name an importer converts (the scan's kind for one with its import record), for
// a name no rule types a material chunk too (a chunk container, by its content), and for a name
// with no extension the project's notes too (a text, by its content).
bool asset_name_fits_kind(const std::string &logical_name, AssetKind kind);

// The kind a required-resource row's file name implies without reading anything:
// the `.bin` rows are string tables except the music scripts and the raw markers,
// which are named. This is what a requirement compares a scanned file against.
AssetKind expected_asset_kind_for_required_name(const std::string &name);

} // namespace opennova::editor
