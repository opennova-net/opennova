// Which file a menu texture or font name loads [orig:
// CTextureManager_LoadOrFindTexture @ 0x654980; CFontCache_LoadOrGetFont @ 0x652f70].

#include <runtime/menu/menu_assets.h>

#include <base/io/strutil.h>

namespace opennova::menu {

namespace {

// [orig: String_HasExtension @ 0x64fe00 — stricmp from the last '.']
bool has_extension(const std::string &name, const char *extension) {
	const size_t dot = name.rfind('.');
	return dot != std::string::npos && strutil::iequals(name.substr(dot), extension);
}

} // namespace

std::string replace_or_append_extension(const std::string &name, const std::string &extension) {
	const size_t dot = name.find('.');
	if (dot == std::string::npos) return name + "." + extension;
	return name.substr(0, dot + 1) + extension;
}

std::string menu_font_file(const std::string &name) {
	return replace_or_append_extension(name, "fnt");
}

MenuTextureSource menu_texture_source(const std::string &name,
		const std::function<bool(const std::string &)> &exists, bool url) {
	MenuTextureSource out;
	if (url) {
		// [orig: CUIImage_LoadTextureFromFile @ 0x654190 — .tga, .pcx, .png]
		if (has_extension(name, ".tga")) out.format = MenuTextureFormat::Tga;
		else if (has_extension(name, ".pcx")) out.format = MenuTextureFormat::Pcx;
		else if (has_extension(name, ".png")) out.format = MenuTextureFormat::Png;
		if (out.format != MenuTextureFormat::None) out.file = name;
		return out;
	}
	if (has_extension(name, ".tga")) {
		// [orig: FileSystem_FileExists @ 0x654c3a true -> CUIImage_LoadTGA; false ->
		// the .dds from the first dot -> TextureSlot_LoadFromDXTFile @ 0x654c93]
		if (exists && exists(name)) {
			out.file = name;
			out.format = MenuTextureFormat::Tga;
		} else {
			out.file = replace_or_append_extension(name, "dds");
			out.format = MenuTextureFormat::Dds;
		}
	} else if (has_extension(name, ".dds")) {
		out.file = name;
		out.format = MenuTextureFormat::Dds;
	} else if (has_extension(name, ".pcx")) {
		out.file = name;
		out.format = MenuTextureFormat::Pcx;
	} else if (has_extension(name, ".png")) {
		out.file = name;
		out.format = MenuTextureFormat::Png;
	}
	return out;
}

} // namespace opennova::menu
