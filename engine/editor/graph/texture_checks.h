#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <editor/documents/texture_document.h>
#include <editor/documents/texture_image.h>
#include <editor/documents/texture_roles.h>
#include <editor/graph/texture_uses.h>
#include <editor/model/diagnostic.h>

namespace opennova::editor {

class AssetGraph;
class ValidationCache;
struct AssetEntry;
struct ValidationInput;
enum class CoreFinding;

// The texture use check (ADR 0046 S18; the texture kind's row of graph/use_checks): what each texture
// reference's role asks of the file its loader opens, read from that file's header (texture_header,
// kept while the file's stamp stands), each finding (the core's texture codes) on the reference's record
// and field: a terrain's
// colour map not 1024 x 1024 (an error where the game reads past it), a foliage map whose rows the
// lookup overruns (an error) or of the wrong shape, a file the role's loader cannot read (an error for a
// map whose loss aborts the mission), a cut-out material over a texture of no alpha, a normal map halved
// past 512, a height whose side is no power of two, a particle graphic the atlas cannot place, a tile
// atlas not in 64-texel cells, a loading screen not 800 x 600; a file its loader passes over for another
// of the name (a .tga beside the .dds a model row loads), on that file (the texture type's code); and of the
// names the game opens itself, the MFD's sides and the default loading screen's size.
void check_texture_uses(const AssetGraph &graph, const ValidationCache &files, const ValidationInput &input,
                        std::vector<Diagnostic> &out);

// What the texture check has read since the process started (the perf query): the headers it read from
// their files (each read once while its file's stamp stands), the bytes those reads took, and the walks of
// the graph's texture uses it made.
struct TextureCheckStats {
	uint64_t headers_read = 0;
	uint64_t header_bytes = 0;
	uint64_t use_walks = 0;
};
TextureCheckStats texture_check_stats();
// What the check keeps let go: its walks of the uses (each kept while the graph's state, the scan, the project's
// folder and the files' source stand) and the headers and char map classes it read (use_checks.h's
// forget_use_checks).
void forget_texture_checks();

// A project file's header as `reader` reads it (`root` the project's directory), read once and kept while
// the file's stamp (the scan's size and last write) stands: the use check's read of it, which the uses'
// budgets (TextureUseIndex) share. A PCX's or a PNG's is read from the file's first bytes alone (the 128 of a
// PCX's header), a TGA's and a DDS's from the whole file. A project folder's file whose last write has settled is
// kept on disk too (ProjectPaths::texture_headers_file, by its size and last write), so a later session's open
// reads it no more.
TextureHeader texture_file_header(const std::string &root, const AssetEntry &entry, TextureReader reader);

// A read the check makes of a file of the validation's scan: its header as `reader` reads it, or (`classes`) a
// char map's texels, read whole.
struct TextureRead {
	const AssetEntry *entry = nullptr;
	TextureReader reader = TextureReader::None;
	bool classes = false;
};
// Every read the check makes over `input` (each use's target as the check resolves it: the file its loader opens
// and its reader, a terrain detail's own name's, a char map's classes; the names the game opens itself), each
// once, in the graph's edges' order: what a validation's step reads ahead of the composition (ProjectValidation's
// texture step), so the check reads nothing as it composes. None while the check keeps its walk for the graph
// and the scan as they stand. A file is read through the input's paths: their source by its logical name where
// they have one (ProjectPaths::files), else the project's folder.
std::vector<TextureRead> texture_use_reads(const AssetGraph &graph, const ValidationInput &input);
// One of those reads made where the check has not kept it for the file as the scan lists it; the bytes read (0:
// kept).
uint64_t read_texture_use(const ValidationInput &input, const TextureRead &read);

// What a role asks of a file's sides, by its header as the role's loader reads it (`file` its name,
// `where` the use in words: "the terrain colour map of isle.trn", `context` what the use says of itself:
// a particle graphic's mode): each finding handed to `add`, the code, its severity and its words. The
// check above makes one finding of each; a test asks it of a file directly.
using TextureFindingSink = std::function<void(CoreFinding, DiagnosticSeverity, const std::string &message)>;
void check_texture_role(renderer::TextureRoleId role, const std::string &file, const TextureHeader &header, const std::string &where,
                        const TextureFindingSink &add, const TextureUseContext &context = TextureUseContext());

} // namespace opennova::editor
