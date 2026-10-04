#pragma once

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
struct ValidationInput;

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

// What a role asks of a file's sides, by its header as the role's loader reads it (`file` its name,
// `where` the use in words: "the terrain colour map of isle.trn", `context` what the use says of itself:
// a particle graphic's mode): each finding handed to `add`, the code, its severity and its words. The
// check above makes one finding of each; a test or a retail leg asks it of a file directly.
using TextureFindingSink = std::function<void(CoreFinding, DiagnosticSeverity, const std::string &message)>;
void check_texture_role(TextureRoleId role, const std::string &file, const TextureHeader &header, const std::string &where,
                        const TextureFindingSink &add, const TextureUseContext &context = TextureUseContext());

} // namespace opennova::editor
