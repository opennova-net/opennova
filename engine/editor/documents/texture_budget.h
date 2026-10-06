#pragma once

#include <cstdint>
#include <string>

#include <base/io/json.h>
#include <editor/documents/texture_image.h>
#include <editor/documents/texture_roles.h>
#include <runtime/renderer/device_texture.h>

namespace opennova::editor {

// What a texture costs the game (ADR 0046 S18, the texture budget): the device texture a model row's
// loader makes of the file it opens, at each object texture detail level, and what the same texture would
// cost as the `.dds` that loader reads first. The rules are the game's (runtime/renderer/device_texture.h,
// render-material-re.md "The device texture"); this reads them for a use, from the file's header alone.

// How a model texture row's loader makes its device texture: the stage loader (a diffuse, a detail map, a
// flipbook frame: the `.dds` beside the name first, else the pixels), the plain loader (the pixels of the
// whole name) or the normal-map loader (always the pixels, a `.dds` decoded first, capped at 512).
enum class TextureBudgetLoader : uint8_t { Stage, Plain, Normal };
const char *texture_budget_loader_token(TextureBudgetLoader loader);
// The loader a role's file is costed by; false for a role whose device texture is not witnessed yet (a
// terrain's, the HUD's, a menu's, a producer's volume).
bool texture_role_budget_loader(TextureRoleId role, TextureBudgetLoader &out);

// The cost a model texture holds past which the use check says so (texture.memory): 16 MB with its chain, a
// 2048 x 2048 texture uncompressed; no model texture the shipped game loads holds more than 1.3 MB (a 512 x
// 512 normal map, or a 1024 x 1024 DXT5: render-material-re.md, "The device texture").
inline constexpr uint64_t kTextureMemoryWarnBytes = uint64_t(16) * 1024 * 1024;

struct TextureBudget {
	bool known = false;
	TextureBudgetLoader loader = TextureBudgetLoader::Stage;
	uint8_t slot = 0;
	// The file the loader opens, by its logical name.
	std::string file;
	// The device texture at each object texture detail level, 0 the lowest, 3 full detail.
	renderer::DeviceTexture detail[renderer::kObjectTexDetailLevels];
	// The `.dds` the row's loader would read first, where it reads one and the file is no DDS: DXT5 for a
	// texture holding an alpha, else DXT1, with its full chain, at full detail.
	bool offers_dds = false;
	renderer::DeviceTexture as_dds;
	const renderer::DeviceTexture &full() const { return detail[renderer::kObjectTexDetailFull]; }
};

// The budget of the file `file` (its header as the loader's reader reads it) for a model row of `slot`
// loaded by `loader`; unknown where the header does not read.
TextureBudget texture_budget(const TextureHeader &header, const std::string &file, TextureBudgetLoader loader, uint8_t slot);

// Bytes in words: "21.3 MB", "340 KB", "96 bytes".
std::string texture_bytes_words(uint64_t bytes);
// A device texture in words: "2048 x 2048, A8R8G8B8 (uncompressed), 10 levels: 21.3 MB".
std::string device_texture_words(const renderer::DeviceTexture &texture);
// The budget in a sentence: "21.3 MB in the game (2048 x 2048, A8R8G8B8 (uncompressed), 10 levels); 5.3 MB
// as a DXT5 .dds".
std::string texture_budget_words(const TextureBudget &budget);
// On the wire (texture_uses' `budget`): `loader`, `slot`, `file`, `detail` (a device texture a level, 0 to
// 3: `level`, `width`, `height`, `format`, `levels`, `bytes`, `stat_bytes`, `halvings`, `whole`,
// `dxt5_as_dxt1`), `as_dds` (one such, or null) and `words`.
io::JsonValue texture_budget_json(const TextureBudget &budget);

} // namespace opennova::editor
