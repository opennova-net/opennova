// What a menu screen reads besides its own windows, through a file source [orig:
// CWnd_GetInheritedTextRsrc @ 0x646AB0; CUIStringTable_LookupString @ 0x6527c0;
// Menu_InitShellResources @ 0x552500].

#include <runtime/menu/menu_screen_inputs.h>

#include <base/io/strutil.h>

#include <utility>

namespace opennova::menu {

namespace {

void collect(const mnu::Window &w, std::vector<std::string> &names) {
	if (w.has_text_rsrc) {
		bool seen = false;
		for (const std::string &name : names)
			if (strutil::iequals(name, w.text_rsrc)) seen = true;
		if (!seen) names.push_back(w.text_rsrc);
	}
	for (const mnu::WindowPart *part : {&w.list_box, &w.spinup, &w.spindown, &w.scrollbar})
		if (part->present()) collect(**part, names);
	for (const mnu::Window &child : w.children) collect(child, names);
}

} // namespace

const std::string *window_text_rsrc(const mnu::Window &window, const mnu::Window *fallback) {
	// [orig: CWnd_GetInheritedTextRsrc @ 0x646AB0 — the widget's +0xC0, else the value
	// the walk up +0xFC ends on]
	if (window.has_text_rsrc) return &window.text_rsrc;
	if (fallback != nullptr && fallback->has_text_rsrc) return &fallback->text_rsrc;
	return nullptr;
}

std::vector<std::string> screen_text_rsrc_names(const mnu::Screen &screen) {
	std::vector<std::string> names;
	for (const mnu::Window &root : screen.roots) collect(root, names);
	return names;
}

void MenuTextTableLoader::load(const mnu::Screen &screen, const FileSource &files, const rtxt::File *override_table,
                               MenuTextTables &out, std::vector<MenuDependency> *dependencies) {
	out.clear();
	out.set_override(override_table);
	for (const std::string &name : screen_text_rsrc_names(screen)) {
		const uint64_t stamp = name.empty() ? 0 : files.stamp(name);
		if (dependencies) dependencies->push_back({name, stamp});
		if (stamp == 0) {
			// [orig: CUIStringTable_LookupString @ 0x6527c0 — a load that fails is not
			// stored (the count is not bumped), so the next lookup tries again]
			out.set_table(name, nullptr);
			continue;
		}
		Loaded &loaded = tables_[strutil::to_lower(name)];
		if (loaded.stamp != stamp) {
			loaded.stamp = stamp;
			loaded.file.reset();
			std::vector<uint8_t> bytes;
			auto file = std::make_shared<rtxt::File>();
			std::string error;
			if (files.read(name, bytes) && !bytes.empty() && rtxt::parse(bytes.data(), bytes.size(), *file, error))
				loaded.file = std::move(file);
		}
		out.set_table(name, loaded.file);
	}
}

void MenuTextTableLoader::clear() { tables_.clear(); }

const std::map<std::string, std::string> &MenuStyleSource::vars(const FileSource &files) {
	uint64_t stamps[2] = {files.stamp(kShellStylesheets[0].name), files.stamp(kShellStylesheets[1].name)};
	if (loaded_ && stamps[0] == stamps_[0] && stamps[1] == stamps_[1]) return vars_;
	loaded_ = true;
	stamps_[0] = stamps[0];
	stamps_[1] = stamps[1];
	// [orig: Menu_InitShellResources @ 0x552500 — menu_style.mns with append 0, then
	// brand.mns with append 1, through NapiConfigMap_LoadIncludeFile @ 0x63b970]
	style_ = load_shell_style([&files](const std::string &name, std::string &bytes) {
		std::vector<uint8_t> data;
		if (!files.read(name, data)) return false;
		bytes.assign(data.begin(), data.end());
		return true;
	});
	vars_.clear();
	for (const auto &entry : style_.list.sheet().variables) vars_[entry.first] = entry.second;
	return vars_;
}

void MenuStyleSource::dependencies(std::vector<MenuDependency> &out) const {
	if (!loaded_) return;
	for (size_t i = 0; i < 2; ++i) out.push_back({kShellStylesheets[i].name, stamps_[i]});
}

void MenuStyleSource::clear() {
	loaded_ = false;
	stamps_[0] = stamps_[1] = 0;
	style_ = ShellStyle();
	vars_.clear();
}

} // namespace opennova::menu
