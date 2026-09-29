#pragma once

// The fonts and textures a menu screen draws with, read through a file source and kept
// by (file, stamp): the menu's font cache and texture manager [orig:
// CFontCache_LoadOrGetFont @ 0x652f70; CTextureManager_LoadOrFindTexture @ 0x654980 —
// a name already loaded is found again, not reloaded]. The embedder keeps only the pixels
// (MenuTextureDecoder): the game's frame decodes and uploads them (the tests read only the
// size from the header). Witness record: docs/mnu/menu-re.md ("Menu textures and fonts").

#include <base/vfs/file_source.h>
#include <formats/fnt/fnt.h>
#include <formats/mnu/mnu.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/menu/menu_assets.h>
#include <runtime/menu/menu_frame.h>
#include <runtime/menu/menu_screen_inputs.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace opennova::menu {

// The embedder's half of a texture load.
class MenuTextureDecoder {
public:
	virtual ~MenuTextureDecoder() = default;
	// Decode one texture file, `bytes` in the format retail's dispatch picked
	// (menu_texture_source), and keep what it made under `key` until release(key); its
	// size out. False when it does not decode (retail: E_FAIL, no slot).
	virtual bool decode(const std::string &key, MenuTextureFormat format, const std::vector<uint8_t> &bytes,
	                    int &width, int &height) = 0;
	virtual void release(const std::string &key) = 0;
};

// One FONT file as loaded; `serial` names this load, so an embedder's page cache keyed by
// it never outlives it (font_alive).
struct MenuFont {
	fnt::fnt_font_t font = {};
	uint64_t serial = 0;
	bool valid = false; // font holds parsed pages (freed with it)
	MenuFont() = default;
	~MenuFont();
	MenuFont(const MenuFont &) = delete;
	MenuFont &operator=(const MenuFont &) = delete;
};

// A screen's fonts and textures across configures. What one configure uses stays; what
// neither it nor the configure before it used is let go, so the game holds at most two
// screens' assets and the editor's preview reuses everything across edits that do not
// move a file.
class MenuFrameAssets {
public:
	MenuFrameAssets();
	~MenuFrameAssets();
	MenuFrameAssets(const MenuFrameAssets &) = delete;
	MenuFrameAssets &operator=(const MenuFrameAssets &) = delete;

	// One configure of `compiler` over `screen` of `document` (both borrowed by the
	// compiler until the next configure; a null screen configures nothing): the %VAR% list
	// (the shell's, MenuStyleSource), the string tables the screen's windows name read
	// through `files` (MenuTextTableLoader; `override_table` is the expansion's, searched
	// first, borrowed), the document's texture loads noted (the band a texture's first
	// load fixes); then every FONT NAME the windows author, after %VAR% the way the compiler
	// interns it, loads as its `.fnt` (menu_font_file) and is registered, only a font that
	// loaded (there is no default font: a widget whose FONT did not load draws with the
	// nearest ancestor's that did [orig: CWnd_GetFontAndColors @ 0x646a70]); then
	// compiler.configure(screen); then every texture slot it interned loads by retail's
	// dispatch and reports its size. Returns how many font and texture names did not load.
	int configure(MenuFrameCompiler &compiler, const mnu::Document *document, const mnu::Screen *screen,
	              const FileSource &files, MenuTextureDecoder &decoder,
	              const std::map<std::string, std::string> &vars, const rtxt::File *override_table = nullptr);
	// A font named after the configure (a marquee's node fonts), loaded and registered the
	// same way; false when it does not load.
	bool add_font(MenuFrameCompiler &compiler, const std::string &name, const FileSource &files);
	// Whether a texture name loads (retail's dispatch, then a decode), kept like the
	// screen's own.
	bool texture_loads(const std::string &name, const FileSource &files, MenuTextureDecoder &decoder);
	// Everything let go: the compiler configured over nothing, its fonts unregistered.
	void clear(MenuFrameCompiler &compiler, MenuTextureDecoder &decoder);

	// Per compiler texture slot, the key its decode is kept under ("" = it did not load).
	const std::vector<std::string> &slot_keys() const { return slot_keys_; }
	// The font a compiler slot draws with (null for slot 0 or a name that did not load).
	const MenuFont *font_for_slot(const MenuFrameCompiler &compiler, int32_t slot) const;
	// True while the load `serial` names is kept.
	bool font_alive(uint64_t serial) const;
	// The names the last configure could not load (fonts after %VAR%, textures as
	// interned), each once.
	const std::vector<std::string> &unresolved() const { return unresolved_; }
	// Those of them whose file the source has: a font that did not parse, a texture that
	// did not decode or whose name's extension the loader reads nothing for. The source
	// lacks the rest.
	const std::vector<std::string> &unreadable() const { return unreadable_; }
	// The TEXT_RSRC names of the last configure that did not resolve.
	const std::vector<std::string> &missing_tables() const { return missing_tables_; }
	// The TEXT_RSRC names of the last configure that resolved but did not parse.
	const std::vector<std::string> &unreadable_tables() const { return unreadable_tables_; }
	// The files the last configure (and the loads after it) read or looked for: the
	// string tables, the fonts and the textures.
	const std::vector<MenuDependency> &dependencies() const { return dependencies_; }
	size_t kept_textures() const { return textures_.size(); }
	size_t kept_fonts() const { return fonts_.size(); }

private:
	struct TextureEntry {
		bool ok = false;
		int width = 0;
		int height = 0;
		uint64_t used = 0; // the generation that last used it
	};
	struct FontEntry {
		MenuFont font;
		uint64_t used = 0;
	};
	// How a load ended: a name that did not load is absent (the source lacks its file) or
	// unreadable (the file is there and did not parse or decode).
	enum class LoadResult : uint8_t { Loaded, Absent, Unreadable };
	// The kept texture a name resolves to now (loaded on first use): its key and entry
	// when it loaded.
	LoadResult load_texture_(const std::string &name, const FileSource &files, MenuTextureDecoder &decoder,
	                         std::string &key, const TextureEntry **out);
	LoadResult load_font_(MenuFrameCompiler &compiler, const std::string &name, const FileSource &files);
	// True when the name was not noted yet.
	bool note_unresolved_(const std::string &name, LoadResult result);
	void sweep_(MenuTextureDecoder &decoder);

	uint64_t generation_ = 0;
	uint64_t next_serial_ = 1;
	std::map<std::string, TextureEntry> textures_;          // lowercased file '#' stamp
	std::map<std::string, std::unique_ptr<FontEntry>> fonts_; // lowercased file '#' stamp
	std::map<std::string, FontEntry *> registered_;        // lowercased name as registered
	MenuTextTableLoader text_loader_;
	MenuTextTables text_tables_; // the compiler borrows them
	std::vector<std::string> slot_keys_;
	std::vector<std::string> unresolved_;
	std::vector<std::string> unreadable_;
	std::vector<std::string> missing_tables_;
	std::vector<std::string> unreadable_tables_;
	std::vector<MenuDependency> dependencies_;
};

} // namespace opennova::menu
