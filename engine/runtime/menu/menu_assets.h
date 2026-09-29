#pragma once

// Which file a menu texture or font name loads [orig:
// CTextureManager_LoadOrFindTexture @ 0x654980; CFontCache_LoadOrGetFont
// @ 0x652f70]. The embedder reads and decodes the file; the choice is here so the
// game, the editor preview and the editor's asset graph agree on it.
// Witness record: docs/mnu/menu-re.md ("Menu textures and fonts").

#include <functional>
#include <string>

namespace opennova::menu {

enum class MenuTextureFormat { None, Tga, Dds, Pcx, Png };

struct MenuTextureSource {
	std::string file; // the name to read (empty with None)
	MenuTextureFormat format = MenuTextureFormat::None;
};

// The file a texture name loads, by the extension from its LAST dot, compared
// ignoring case [orig: String_HasExtension @ 0x64fe00]: ".tga" loads the name when
// it exists, else the name with everything after its FIRST dot replaced by "dds"
// (a DXT file) [orig: @ 0x654c27..0x654c93; String_ReplaceOrAppendExtension
// @ 0x64fe60]; ".dds", ".pcx" and ".png" load the name; anything else loads
// nothing (the loader returns E_FAIL @ 0x654d17). A URL image (`url`, the loader's
// flag 1) decodes ".tga", ".pcx" or ".png" and nothing else, with no fallback
// [orig: CUIImage_LoadTextureFromFile @ 0x654190]. `exists` answers whether a name
// is in the mounted file set.
MenuTextureSource menu_texture_source(const std::string &name,
		const std::function<bool(const std::string &)> &exists, bool url = false);

// A name with everything after its first dot replaced by `extension`, or
// ".extension" appended when it has no dot [orig: String_ReplaceOrAppendExtension
// @ 0x64fe60].
std::string replace_or_append_extension(const std::string &name, const std::string &extension);

// The .fnt a FONT NAME loads [orig: CFontCache_LoadOrGetFont @ 0x652f70 builds the
// path with String_ReplaceOrAppendExtension(name, "fnt")].
std::string menu_font_file(const std::string &name);

} // namespace opennova::menu
