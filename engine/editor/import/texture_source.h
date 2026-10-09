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
// under the project's .replaced/ folder (a dot-folder the scan and the build pass over, but no cache: it
// survives a cache cleared and goes with the project's files to a clone), never deleted, and the next import
// pass makes it again. A plan says, before anything is written, what it will change.

// The folder the files a Replace or an Edit externally sets aside go to, project-relative.
inline constexpr const char *kReplacedFolder = ".replaced";

// The options that make a texture of `name`, stored as `bytes`, again from an image of it: its format by
// its name's extension and its header (a .tga's 32 or 24 bits, a .dds's compression and whether it carries
// its chain, an .mdt, a .png, a .pcx's 8-bit colours or its 24-bit planes, or, from an indexed source
// (`indexed_source`, an 8-bit PCX), its indices as they are), and `name` where the source's stem and the
// format's extension would not make it (`source_name`, the source's file name).
ImportOptions texture_reproducing_options(const std::string &name, const std::vector<uint8_t> &bytes,
                                          const std::string &source_name, bool indexed_source);

// What the texture's uses ask of a source made for it (the session fills it from graph/texture_import_needs):
// its palette indices kept (a foliage or a char map's data), and an exact size a use reads, each with the use
// in words.
struct TextureUseAsks {
	bool indices = false;
	std::string indices_why;
	std::string size; // "<W>x<H>"; "" none
	std::string size_why;
};

// What a texture's source is to be and what it sets aside, planned over the scan.
struct TextureSourcePlan {
	std::string texture;  // the texture's logical name, what every referrer names
	std::string replaced; // the plain project file it replaces, project-relative ("" none)
	std::string old_source;  // an import output's source, set aside where the new source is another file
	std::string source;      // the source, project-relative
	std::vector<uint8_t> bytes; // what the source is made of (written there)
	ImportOptions options;      // its record's
	// What the plan changes, in words, a line each (the dialog's), and the texture before and after: its
	// sides and form in words, and the file the import makes (`made`, its bytes, named `made_name`), which
	// the dialog draws.
	std::vector<std::string> changes;
	std::string before_words, after_words;
	std::vector<uint8_t> made;
	std::string made_name;
	std::vector<Diagnostic> refusals;
	bool ok() const { return refusals.empty(); }
};

// Replace: the texture `texture` (a project file by its path or logical name, an import's output, or a name
// the project lacks: a texture a field names that is missing) made from the image `image_bytes` named
// `image_name` (a PNG, a TGA or a PCX the importer reads): the image copied into art/ under the texture's own
// stem whatever the image is called (body.dds's art/body.png; `<stem>_src` where another file has that name;
// an import's output keeps its source's path where the image is of its form), a TGA kept as a PNG of its
// texels, its record's options those reproducing the texture's
// stored form (an import output's: its import's own), the exact size its uses read where the image is
// another, `overrides` over them. Refused: an image the importer does not read, a texture that is no texture
// or no import's output, a cube map or a volume (the editor writes flat textures), an image of colours for a
// texture whose uses read its palette indices, an override no option row takes.
TextureSourcePlan plan_texture_replace(const ProjectPaths &paths, const AssetScan &scan, const std::string &texture,
                                       const std::string &image_name, const std::vector<uint8_t> &image_bytes,
                                       const ImportOptions &overrides, const TextureUseAsks &asks = TextureUseAsks());

// Edit externally: the source a paint program edits for the texture `texture`: an import output's own
// source (nothing to make: `source` its path, no bytes); a PNG the game reads as it is, itself; a plain
// texture's one made once, a PNG of a TGA's or an MDT's texels or a copy of a PCX (which a paint program opens,
// an 8-bit PCX's indices kept by palette indices, a 24-bit PCX's colours kept in three planes), a PNG of a DDS's first level as its
// reader decodes it, its stored form reproduced and what changes said (a TGA stored top first drawn upright
// from then on; a DDS's blocks encoded again and its chain made anew). Refused: no texture of the project of
// that name, no texture (an import source of another importer), one the game cannot read, a cube map or a
// volume.
TextureSourcePlan plan_texture_source(const ProjectPaths &paths, const AssetScan &scan, const std::string &texture);

// Store as DDS: the .tga `texture` (a project file by its path or logical name) stored as the .dds of its
// name, which the loader of a model's diffuse, detail or flipbook row opens before the .tga
// (renderer::material_texture_source [orig: Texture_LoadByNameWithChannel @ 0x58B53C..0x58B598]), so every
// referrer keeps naming the .tga: DXT1 for a texture of no alpha, DXT5 for one with alpha, every level to
// 1 x 1 (runtime/renderer/dxt_encode.h). A plain .tga becomes an import's output as Edit externally makes it one (its
// copy in art/ under its own name where that is free, the plain file set aside), its record writing `<stem>.dds`; an
// import's output's own record takes the same options (`source` its source, no bytes: the session sets
// them). `reads_tga` holds, in words, each use of the texture whose loader opens the .tga itself and never
// its .dds (a terrain's colour map, a model's plain row, the HUD's art): any refuses it, the texture lost
// there. Refused too: no .tga, one that does not read, sides that are not powers of two (a DDS-reader
// image of other sides is padded to the next ones: render-material-re D-RMAT-18).
TextureSourcePlan plan_texture_dds(const ProjectPaths &paths, const AssetScan &scan, const std::string &texture,
                                   const std::vector<std::string> &reads_tga);

// The project file `relative` set aside under .replaced/<stamp>/ (its own folder kept; a stamp of its own), never
// deleted: what a texture the game never reads is given (set_aside_texture). True where there is none; false, with
// the finding, where it could not be moved.
bool set_aside_project_file(const ProjectPaths &paths, const std::string &relative, std::vector<Diagnostic> &findings);

// The plan done: the replaced files set aside under .replaced/<stamp>/ (their own folders kept; a stamp of
// its own each time, to the millisecond and numbered past one already there), the source written, its record
// written. False with the findings, what was done before taken back: the files set aside put back, a source
// written over restored.
bool apply_texture_source(const ProjectPaths &paths, const TextureSourcePlan &plan, std::vector<Diagnostic> &findings);

} // namespace opennova::editor
