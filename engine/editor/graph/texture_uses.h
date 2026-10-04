#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/assets/asset_kind.h>
#include <editor/documents/texture_load_rules.h>
#include <editor/documents/texture_roles.h>

namespace opennova::editor {

class AssetGraph;
class Document;
struct AssetScan;
struct GraphEdge;

// What a use says of itself beyond its role (ADR 0046 S18): a model row's slot, type and flags and its
// material's shader, flags and alpha-test reference; the referrer's key (a .trn's, an .env's, a def's
// member, a hudpos keyword); a HUD caller's alpha mode; a particle graphic's mode.
struct TextureUseContext {
	int material = -1; // the model row's material, by its place among the model's materials
	uint8_t slot = 0, type = 0, row_flags = 0;
	std::string shader;
	uint8_t material_flags = 0, alpha_ref = 0;
	std::string key;
	int hud_mode = -1; // 0 colour, 1 alpha only; -1 not a HUD use
	int blend_mode = -1; // a particle graphic's mode (formats/particle BlendMode); -1 not a particle's
	bool alpha_test() const { return (material_flags & 0x01) != 0; }
	bool alpha_test_inverted() const { return (material_flags & 0x02) != 0; }
};

// One use of a texture file (ADR 0046 S18): its role (kCount where the referrer's loader is not
// witnessed yet), the referrer (its file, the record and its locator, the field; none for a name the
// game opens itself), the use in words, the name as written, its context, what the role's loader opens
// for that name (`load`) and the project file that is (`served`, "" for none), and whether the file the
// uses were asked of is that file (`reads_file`: false where the loader takes another, a .tga's .dds).
struct TextureUse {
	TextureRoleId role = TextureRoleId::kCount;
	std::string referrer, record, locator, field;
	std::string words;
	std::string name_written;
	TextureUseContext context;
	TextureLoad load;
	std::string served;
	bool reads_file = true;
	// A name the game opens itself (no referrer): what for, and the witness.
	bool fixed = false;
	std::string fixed_for, fixed_witness;
	// How its loader is asked again (texture_use_opens): a texture reference's loader argument (texture_roles.h;
	// -1 for a menu's or a mission's, which its role's loader takes), a fixed name's loader.
	int32_t loader_arg = -1;
	TextureLoader loader = TextureLoader::kCount;
	bool known() const { return role != TextureRoleId::kCount; }
};

// Whether the use's loader would open the file named `file` (by its logical name) were that the one file of
// its names the project held: a model row naming body.tga opens body.dds (the .dds beside it first) and
// body.tga, never body.mdt, which is another file. What a file's uses are, never a stem's
// (session/texture_import_state: the uses an import's output serves).
bool texture_use_opens(const TextureUse &use, const std::string &file);

// A name the game opens itself, its role and what for (the HUD's art, the scars, the weather, the view
// effects, the screens): the fixed-names table, from the runtime's own name constants where it has them.
struct FixedTextureName {
	std::string name;
	TextureRoleId role = TextureRoleId::kCount;
	const char *what = "";
	const char *witness = "";
	// The loader where it is not the role's (the night vision's scale through FILE, the vignette through
	// ARCHIVE); kCount the role's.
	TextureLoader loader = TextureLoader::kCount;
	int hud_mode = -1;
};
const std::vector<FixedTextureName> &fixed_texture_names();

// The role a graph edge's use is: the role its loader argument names (texture_roles.h); a model row's
// by its row (its slot, its runtime type, its flipbook flag, read from `model`, the model document the
// edge is in, null where it does not read: the row's type alone then); a menu's image, frame stencil
// or brush, or cursor by its field; a mission's loading screen; kCount for a use not witnessed.
TextureRoleId texture_role_of_edge(const GraphEdge &edge, const Document *model, TextureUseContext &context);

// What reads a referring model: its document at a project-relative path (open, or read from its file;
// null when it does not read).
using TextureModelSource = std::function<std::shared_ptr<const Document>(const std::string &path)>;

// Every use of the project file `file`: the graph's texture edges naming it by its loader (those the
// loader resolves to it, and those whose name is the file's though the loader opens another), each
// with its role and context, then the fixed names its logical name is, each with what the game opens
// it for. `exists` answers whether the project holds a name (the loaders' probes).
std::vector<TextureUse> texture_uses(const AssetGraph &graph, const AssetScan &scan, const std::string &file,
                                     const TextureModelSource &models, const TextureNameTest &exists);

// Every use of a name whose stem is `stem` (case aside), whatever file its loader finds: the graph's
// texture references writing one (a missing one too), then the names the game opens itself; `served` the
// project file the loader opens ("" none), `reads_file` whether it opens one. What an import of that stem
// is asked for (graph/texture_import_needs).
std::vector<TextureUse> texture_uses_named(const AssetGraph &graph, const AssetScan &scan, const std::string &stem,
                                           const TextureModelSource &models, const TextureNameTest &exists);

io::JsonValue texture_use_json(const TextureUse &use);

} // namespace opennova::editor
