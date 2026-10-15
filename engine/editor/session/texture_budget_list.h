#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/texture_budget.h>
#include <runtime/renderer/device_texture.h>

namespace opennova::editor {

struct SessionView;

// What the project's model textures cost the game (ADR 0046 S18, the texture budget, the texture_budget query):
// each texture the game makes, the costliest first at full detail, and the totals at each object texture detail
// level and with every texture whose loader reads a `.dds` first made of that `.dds`. The game keeps a texture
// by the name a row writes and one letter, any case (renderer::texture_registry_key): the stage and plain loaders
// both key it "name:1" and the normal-map loader "name:BA:1", each looking the key up before it loads anything
// [orig: Texture_LoadByNameWithChannel @ 0x58B49F..0x58B4C4, its channel 0 from
// Material_LoadStageTexture @ 0x5B173F; Texture_LoadAndRegister @ 0x58B7BF..0x58B7E4;
// Texture_LoadAsNormalMap @ 0x58C4BA..0x58C4E5; the lookup case-insensitive]. So two rows naming one file are
// one texture, made as the first asks (here the first use the session lists), and a row of the plain loader
// takes the texture a stage row made of the name.
struct TextureBudgetRow {
	std::string name; // the name as the first row writes it
	std::string file; // project-relative: the file its loader opens
	TextureBudget budget;
	size_t uses = 0; // the rows that take this texture
};
struct TextureBudgetList {
	std::vector<TextureBudgetRow> rows;
	uint64_t at_detail[renderer::kObjectTexDetailLevels] = {};
	// At full detail, every texture whose loader reads a .dds first made of that .dds; the others as they are.
	uint64_t as_dds = 0;
	// How many textures hold more than the use check's warning (kTextureMemoryWarnBytes).
	size_t past_warning = 0;
};
TextureBudgetList texture_budget_list(const SessionView &view);
// What the texture the game makes of the project file `file` costs, in words (texture_budget_words), by the
// first use whose loader opens that file and whose budget is known: what a texture's tooltip says; "" for
// none (a file no model row loads, a .tga its loader passes over for the .dds beside it).
std::string texture_file_budget_words(const SessionView &view, const std::string &file);

// On the wire: `totals` {`detail` (bytes at each level, 0 to 3), `as_dds`, `textures`, `past_warning`} and each
// row's `name`, `file`, `uses` and `budget` (texture_budget_json).
io::JsonValue texture_budget_row_json(const TextureBudgetRow &row);
io::JsonValue texture_budget_totals_json(const TextureBudgetList &list);

} // namespace opennova::editor
