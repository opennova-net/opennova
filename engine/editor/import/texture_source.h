#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/import/import_context.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>

namespace opennova::editor {

// A texture of the project made from an image (ADR 0046 S18): Replace (a texture made from an image the
// modder brings, replace_texture) and Edit externally (a texture given a source a paint program edits,
// edit_externally). Either way the texture becomes the output of an import whose record names it, so every
// referrer keeps the name it writes: the image is an import source in the project's art folder (a PNG, a
// TGA or a PCX: importer.h's record_extensions), its record's options the ones that make the texture as it
// is stored (texture_reproducing_options) and the name it takes; the plain file it replaces is set aside
// under the project's .opennova/replaced/ folder, never deleted, and the next import pass makes it again.

// The options that make a texture of `name`, stored as `bytes`, again from an image of it: its format by
// its name's extension and its header (a .tga's 32 or 24 bits, a .dds's compression and whether it carries
// its chain, an .mdt, a .png, a .pcx's colours or, from an indexed source (`indexed_source`, an 8-bit PCX),
// its indices as they are), and `name` where the source's stem and the format's extension would not make
// it (`source_name`, the source's file name).
ImportOptions texture_reproducing_options(const std::string &name, const std::vector<uint8_t> &bytes,
                                          const std::string &source_name, bool indexed_source);

// What a texture's source is to be and what it sets aside, planned over the scan.
struct TextureSourcePlan {
	std::string texture;  // the texture's logical name, what every referrer names
	std::string replaced; // the plain project file it replaces, project-relative ("" none)
	std::string old_source;  // an import output's source, set aside where the new source is another file
	std::string source;      // the source, project-relative
	std::vector<uint8_t> bytes; // what the source is made of (written there)
	ImportOptions options;      // its record's
	std::vector<Diagnostic> refusals;
	bool ok() const { return refusals.empty(); }
};

// Replace: the texture `texture` (a project file by its path or logical name, an import's output, or a name
// the project lacks: a texture a field names that is missing) made from the image `image_bytes` named
// `image_name` (a PNG, a TGA or a PCX the importer reads): the image copied into art/ (another name where a
// file other than the texture's own source has it), its record's options those reproducing the texture's
// stored form (an import output's: its import's own), `overrides` over them. Refused: an image the importer
// does not read, a texture that is no texture or no import's output, an override no option row takes.
TextureSourcePlan plan_texture_replace(const ProjectPaths &paths, const AssetScan &scan, const std::string &texture,
                                       const std::string &image_name, const std::vector<uint8_t> &image_bytes,
                                       const ImportOptions &overrides);

// Edit externally: the source a paint program edits for the texture `texture`: an import output's own
// source (nothing to make: `source` its path, no bytes); a PNG the game reads as it is, itself; a plain
// texture's one made once, a copy of a TGA or a PCX (which a paint program opens, an 8-bit PCX's indices
// kept by palette indices), a PNG of a DDS's first level as its reader decodes it. Refused: no texture of
// the project of that name, one the game cannot read.
TextureSourcePlan plan_texture_source(const ProjectPaths &paths, const AssetScan &scan, const std::string &texture);

// The plan done: the replaced files set aside under .opennova/replaced/<stamp>/ (their own folders kept),
// the source written, its record written. False with the findings, what was done before taken back where it
// can be.
bool apply_texture_source(const ProjectPaths &paths, const TextureSourcePlan &plan, std::vector<Diagnostic> &findings);

} // namespace opennova::editor
