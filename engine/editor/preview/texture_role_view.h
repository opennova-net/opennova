#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/texture_image.h>
#include <editor/documents/texture_roles.h>
#include <formats/trn/trn.h>

namespace opennova::editor {

// What a use's role makes of a texture beyond its loader (ADR 0046 S18, each role in its own picture): the texels
// its consumer draws from, and what its values mean to the game.

// One row of a texture's values as its role reads them: a blend map's channel and the splat detail it weighs, a
// foliage map's code and the foliage it grows. Its key ("red", a code's number), its swatch, its share of the
// texture (a channel's mean weight, a code's texels), and the game's use of it in words.
struct TextureLegendRow {
	std::string key;
	uint8_t rgb[3] = {};
	double share = 0.0;
	std::string words;
};
// What the legend lists ("" none) and its rows, and a line said of the use beside them (a particle graphic's page).
struct TextureRoleView {
	std::string title;
	std::vector<TextureLegendRow> legend;
	std::string words;
	bool empty() const { return title.empty() && words.empty(); }
	bool operator==(const TextureRoleView &other) const;
};

// The texels a role's consumer takes of `image` (the loader's): a terrain blend map's weights as the terrain
// normalizes them; a particle graphic as its atlas page holds it, alone on an empty page of its mode
// (`blend_mode`, formats/particle BlendMode); `image` itself for every other role.
std::shared_ptr<const TextureImage> texture_role_texels(const std::shared_ptr<const TextureImage> &image, TextureRoleId role,
                                                        int blend_mode);

// What the role reads of `source` (the file as its reader decodes it) and `used` (texture_role_texels' of it): a
// blend map's three channels, each its mean weight and the splat detail the terrain names for it; a foliage map's
// codes, each its texels and the definitions of `terrain` it selects; a particle graphic's share of an atlas page
// of its mode. `terrain` is the use's .trn (null where it did not read). None for any other role.
TextureRoleView texture_role_view(const TextureImage &source, const TextureImage &used, TextureRoleId role, int blend_mode,
                                  const TrnConfig *terrain);

// On the wire: {title, legend: [{key, rgb, share, words}], words}.
io::JsonValue texture_role_view_json(const TextureRoleView &view);

} // namespace opennova::editor
