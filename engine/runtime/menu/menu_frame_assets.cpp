// A menu screen's fonts and textures through a file source [orig: CFontCache_LoadOrGetFont
// @ 0x652f70; CTextureManager_LoadOrFindTexture @ 0x654980].

#include <runtime/menu/menu_frame_assets.h>

#include <base/io/strutil.h>

#include <utility>

namespace opennova::menu {

namespace {

std::string kept_key(const std::string &file, uint64_t stamp) {
	return strutil::to_lower(file) + '#' + std::to_string(stamp);
}

// What retail's dispatch reads for a texture name now (menu_texture_source): the file and its
// format; whether that file is not the name (a missing .tga's .dds, or no file at all for an
// extension the dispatch reads nothing for), with the name's own stamp then; and the file's stamp
// (0: the source lacks it, or there is no file). The one resolver of a load and of what is kept
// (load_texture_, texture_kept), so what a configure would decode and what is known kept never part.
struct TextureFile {
	MenuTextureSource source;
	bool renamed = false;
	uint64_t name_stamp = 0;
	uint64_t stamp = 0;
};

TextureFile resolve_texture(const std::string &name, const FileSource &files) {
	TextureFile out;
	out.source = menu_texture_source(name, [&files](const std::string &file) { return files.stamp(file) != 0; });
	out.renamed = !strutil::iequals(out.source.file, name);
	if (out.renamed) out.name_stamp = files.stamp(name);
	if (out.source.format != MenuTextureFormat::None) out.stamp = files.stamp(out.source.file);
	return out;
}

// Every FONT NAME a window tree authors (roots, children and parts), each once
// (case-insensitive), in document order.
void collect_font_names(const mnu::Window &w, std::vector<std::string> &names) {
	if (!w.font.name.empty()) {
		bool seen = false;
		for (const std::string &name : names)
			if (strutil::iequals(name, w.font.name)) seen = true;
		if (!seen) names.push_back(w.font.name);
	}
	for (const mnu::WindowPart *part : {&w.list_box, &w.spinup, &w.spindown, &w.scrollbar})
		if (part->present()) collect_font_names(**part, names);
	for (const mnu::Window &child : w.children) collect_font_names(child, names);
}

} // namespace

MenuFont::~MenuFont() {
	if (valid) fnt::fnt_free(&font);
}

MenuFrameAssets::MenuFrameAssets() = default;
MenuFrameAssets::~MenuFrameAssets() = default;

bool MenuFrameAssets::note_unresolved_(const std::string &name, LoadResult result) {
	for (const std::string &seen : unresolved_)
		if (strutil::iequals(seen, name)) return false;
	unresolved_.push_back(name);
	if (result == LoadResult::Unreadable) unreadable_.push_back(name);
	return true;
}

// One FONT NAME [orig: CFontCache_LoadOrGetFont @ 0x652f70 — a name already in the cache
// (stricmp) is found again; on a miss the path is the name with its extension replaced by
// "fnt" (String_ReplaceOrAppendExtension), and a load that fails leaves the handle 0].
MenuFrameAssets::LoadResult MenuFrameAssets::load_font_(MenuFrameCompiler &compiler, const std::string &name,
                                                        const FileSource &files) {
	if (name.empty()) return LoadResult::Absent;
	const std::string registered_key = strutil::to_lower(name);
	const auto registered = registered_.find(registered_key);
	if (registered != registered_.end()) {
		registered->second->used = generation_;
		return LoadResult::Loaded;
	}
	const std::string file = menu_font_file(name);
	const uint64_t stamp = files.stamp(file);
	dependencies_.push_back({file, stamp});
	if (stamp == 0) return LoadResult::Absent;
	std::unique_ptr<FontEntry> &entry = fonts_[kept_key(file, stamp)];
	if (!entry) {
		entry = std::make_unique<FontEntry>();
		entry->font.serial = next_serial_++;
		std::vector<uint8_t> bytes;
		if (files.read(file, bytes) && !bytes.empty() &&
		    fnt::fnt_parse(bytes.data(), bytes.size(), &entry->font.font) == fnt::FNT_OK)
			entry->font.valid = true;
	}
	entry->used = generation_;
	if (!entry->font.valid) return LoadResult::Unreadable;
	compiler.register_font(name, &entry->font.font);
	registered_[registered_key] = entry.get();
	return LoadResult::Loaded;
}

// One texture name through retail's dispatch [orig: CTextureManager_LoadOrFindTexture
// @ 0x654980 — the name's extension picks the decoder (menu_texture_source), a name
// already loaded is found again, and a decode that fails makes no slot].
MenuFrameAssets::LoadResult MenuFrameAssets::load_texture_(const std::string &name, const FileSource &files,
                                                           MenuTextureDecoder &decoder, std::string &key,
                                                           const TextureEntry **out) {
	*out = nullptr;
	key.clear();
	if (name.empty()) return LoadResult::Absent;
	const TextureFile file = resolve_texture(name, files);
	// A .tga the files lack loads its .dds: the .tga is a dependency too (it may appear).
	// So is a name whose extension the dispatch reads nothing for (there or not, it
	// does not load).
	if (file.renamed) {
		dependencies_.push_back({name, file.name_stamp});
		if (file.source.format == MenuTextureFormat::None)
			return file.name_stamp != 0 ? LoadResult::Unreadable : LoadResult::Absent;
	}
	dependencies_.push_back({file.source.file, file.stamp});
	if (file.stamp == 0) return LoadResult::Absent;
	key = kept_key(file.source.file, file.stamp);
	const auto found = textures_.find(key);
	TextureEntry &entry = found != textures_.end() ? found->second : textures_[key];
	if (found == textures_.end()) {
		std::vector<uint8_t> bytes;
		entry.ok = files.read(file.source.file, bytes) && !bytes.empty() &&
		           decoder.decode(key, file.source.format, bytes, entry.width, entry.height) && entry.width > 0 &&
		           entry.height > 0;
	}
	entry.used = generation_;
	if (!entry.ok) {
		key.clear();
		return LoadResult::Unreadable;
	}
	*out = &entry;
	return LoadResult::Loaded;
}

int MenuFrameAssets::configure(MenuFrameCompiler &compiler, const mnu::Document *document, const mnu::Screen *screen,
                               const FileSource &files, MenuTextureDecoder &decoder,
                               const std::map<std::string, std::string> &vars, const rtxt::File *override_table) {
	++generation_;
	compiler.clear_registered_fonts();
	registered_.clear();
	slot_keys_.clear();
	unresolved_.clear();
	unreadable_.clear();
	missing_tables_.clear();
	unreadable_tables_.clear();
	dependencies_.clear();
	compiler.set_style_vars(vars);
	if (screen == nullptr) {
		text_tables_.clear();
		compiler.set_text_tables(nullptr);
		compiler.configure(nullptr);
		sweep_(decoder);
		return 0;
	}
	// The string tables each window's ids read [orig: CWnd_GetInheritedTextRsrc @ 0x646AB0].
	text_loader_.load(*screen, files, override_table, text_tables_, &dependencies_);
	for (const MenuDependency &table : dependencies_) {
		if (table.stamp == 0) missing_tables_.push_back(table.name);
		else if (!text_tables_.loaded(table.name)) unreadable_tables_.push_back(table.name);
	}
	compiler.set_text_tables(&text_tables_);
	if (document != nullptr) compiler.note_texture_loads(*document);
	// The fonts first: the compiler resolves each widget's font chain as it configures.
	std::vector<std::string> names;
	for (const mnu::Window &root : screen->roots) collect_font_names(root, names);
	std::vector<std::pair<std::string, LoadResult>> fonts_not_loaded;
	for (const std::string &raw : names) {
		const std::string resolved = compiler.resolve_style_var(raw);
		const LoadResult result = load_font_(compiler, resolved, files);
		if (result != LoadResult::Loaded && note_unresolved_(resolved, result) && !resolved.empty())
			fonts_not_loaded.emplace_back(resolved, result);
	}
	compiler.configure(screen);
	// What did not load, noted on the windows naming it (configure started the notes over).
	for (const std::string &name : missing_tables_) compiler.add_load_note(MenuFrameNoteCode::TextTableMissing, name);
	for (const std::string &name : unreadable_tables_)
		compiler.add_load_note(MenuFrameNoteCode::TextTableUnreadable, name);
	for (const auto &font : fonts_not_loaded)
		compiler.add_load_note(font.second == LoadResult::Unreadable ? MenuFrameNoteCode::FontUnreadable
		                                                             : MenuFrameNoteCode::FontMissing,
		                       font.first);
	// Every texture the screen interned, its size reported for the rect fallbacks; one
	// that did not load measures 0 and draws nothing.
	const std::vector<std::string> &textures = compiler.texture_names();
	slot_keys_.reserve(textures.size());
	for (size_t slot = 0; slot < textures.size(); ++slot) {
		const TextureEntry *entry = nullptr;
		std::string key;
		const LoadResult result = load_texture_(textures[slot], files, decoder, key, &entry);
		slot_keys_.push_back(key);
		if (entry == nullptr) {
			if (note_unresolved_(textures[slot], result))
				compiler.add_load_note(result == LoadResult::Unreadable ? MenuFrameNoteCode::TextureUnreadable
				                                                        : MenuFrameNoteCode::TextureMissing,
				                       textures[slot]);
			continue;
		}
		compiler.set_texture_size(static_cast<int32_t>(slot), entry->width, entry->height);
	}
	sweep_(decoder);
	return static_cast<int>(unresolved_.size());
}

bool MenuFrameAssets::add_font(MenuFrameCompiler &compiler, const std::string &name, const FileSource &files) {
	return load_font_(compiler, name, files) == LoadResult::Loaded;
}

bool MenuFrameAssets::texture_loads(const std::string &name, const FileSource &files, MenuTextureDecoder &decoder) {
	const TextureEntry *entry = nullptr;
	std::string key;
	return load_texture_(name, files, decoder, key, &entry) == LoadResult::Loaded;
}

// What load_texture_ would read for the name, kept or not (the one resolver both take): nothing to
// read (no file for its extension, or the source lacks it), or the file at its stamp now, kept.
bool MenuFrameAssets::texture_kept(const std::string &name, const FileSource &files) const {
	if (name.empty()) return true;
	const TextureFile file = resolve_texture(name, files);
	if (file.source.format == MenuTextureFormat::None || file.stamp == 0) return true;
	return textures_.count(kept_key(file.source.file, file.stamp)) != 0;
}

void MenuFrameAssets::sweep_(MenuTextureDecoder &decoder) {
	for (auto it = textures_.begin(); it != textures_.end();) {
		if (it->second.used + 1 < generation_) {
			if (it->second.ok) decoder.release(it->first);
			it = textures_.erase(it);
		} else {
			++it;
		}
	}
	for (auto it = fonts_.begin(); it != fonts_.end();) {
		if (it->second->used + 1 < generation_) it = fonts_.erase(it);
		else ++it;
	}
}

void MenuFrameAssets::clear(MenuFrameCompiler &compiler, MenuTextureDecoder &decoder) {
	compiler.clear_registered_fonts();
	compiler.set_text_tables(nullptr);
	compiler.configure(nullptr);
	text_tables_.clear();
	text_loader_.clear();
	missing_tables_.clear();
	unreadable_tables_.clear();
	registered_.clear();
	for (const auto &entry : textures_)
		if (entry.second.ok) decoder.release(entry.first);
	textures_.clear();
	fonts_.clear();
	slot_keys_.clear();
	unresolved_.clear();
	unreadable_.clear();
	dependencies_.clear();
}

const MenuFont *MenuFrameAssets::font_for_slot(const MenuFrameCompiler &compiler, int32_t slot) const {
	const std::vector<std::string> &names = compiler.font_names();
	if (slot <= 0 || slot >= static_cast<int32_t>(names.size())) return nullptr;
	const auto found = registered_.find(strutil::to_lower(names[static_cast<size_t>(slot)]));
	return found != registered_.end() ? &found->second->font : nullptr;
}

bool MenuFrameAssets::font_alive(uint64_t serial) const {
	for (const auto &entry : fonts_)
		if (entry.second->font.serial == serial) return true;
	return false;
}

} // namespace opennova::menu
