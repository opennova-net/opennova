#include <editor/preview/menu_screen_render.h>

#include <algorithm>

#include <base/io/strutil.h>
#include <editor/documents/mnu_document.h>
#include <formats/mns/mns.h>

namespace opennova::editor {

namespace {

bool listed(const std::vector<std::string> &names, const std::string &name) {
	for (const std::string &kept : names)
		if (strutil::iequals(kept, name)) return true;
	return false;
}

void add_once(std::vector<std::string> &names, const std::string &name) {
	if (!listed(names, name)) names.push_back(name);
}

} // namespace

const char *menu_screen_status_token(MenuScreenStatus status) {
	switch (status) {
	case MenuScreenStatus::NoProject: return "no_project";
	case MenuScreenStatus::NoMenu: return "no_menu";
	case MenuScreenStatus::NoScreen: return "no_screen";
	case MenuScreenStatus::Unserializable: return "unserializable";
	case MenuScreenStatus::ScreenMissing: return "screen_missing";
	case MenuScreenStatus::Ready: return "ready";
	}
	return "no_project";
}

std::string menu_screen_status_message(MenuScreenStatus status, const std::string &detail) {
	switch (status) {
	case MenuScreenStatus::NoProject: return "Open a project to preview its menus.";
	case MenuScreenStatus::NoMenu: return "Open a menu to preview its screens.";
	case MenuScreenStatus::NoScreen: return "Select one of the menu's screens.";
	case MenuScreenStatus::Unserializable:
		return "The game could not read this menu as it stands" + (detail.empty() ? std::string(".") : ": " + detail);
	case MenuScreenStatus::ScreenMissing: return "Screen " + detail + " is not in the menu the game would read.";
	case MenuScreenStatus::Ready: return std::string();
	}
	return std::string();
}

void split_unloaded(const menu::MenuFrameAssets &assets, std::vector<std::string> &missing,
		std::vector<std::string> &unreadable) {
	missing.clear();
	unreadable.clear();
	for (const std::string &name : assets.unreadable()) add_once(unreadable, name);
	for (const std::string &name : assets.unreadable_tables()) add_once(unreadable, name);
	for (const std::string &name : assets.unresolved())
		if (!listed(assets.unreadable(), name)) add_once(missing, name);
	for (const std::string &name : assets.missing_tables()) add_once(missing, name);
}

std::vector<std::string> menu_variables_named(const std::string &text) {
	std::vector<std::string> names;
	for (size_t at = text.find('%'); at != std::string::npos;) {
		const size_t length = mns::variable_reference_at(text, at);
		if (length == 0) {
			at = text.find('%', at + 1);
			continue;
		}
		names.push_back(strutil::to_upper(text.substr(at + 1, length - 2)));
		at = text.find('%', at + length);
	}
	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());
	return names;
}

std::vector<std::string> changed_menu_variables(const std::map<std::string, std::string> &before,
		const std::map<std::string, std::string> &after) {
	std::vector<std::string> out;
	auto was = before.begin();
	auto now = after.begin();
	while (was != before.end() || now != after.end()) {
		if (now == after.end() || (was != before.end() && was->first < now->first)) {
			out.push_back(was->first);
			++was;
		} else if (was == before.end() || now->first < was->first) {
			out.push_back(now->first);
			++now;
		} else {
			if (was->second != now->second) out.push_back(now->first);
			++was;
			++now;
		}
	}
	return out;
}

MenuScreenRender::MenuScreenRender() = default;

MenuScreenRender::~MenuScreenRender() {
	assets_.clear(compiler_, decoder_);
}

MenuScreenStatus MenuScreenRender::configure(const MnuDocument &document, NodeId screen_row, const FileSource &files,
                                             const std::map<std::string, std::string> &vars) {
	state_ = menu::MenuFrameState();
	detail_.clear();
	revision_ = document.revision();
	std::vector<SourceIssue> issues;
	std::shared_ptr<const mnu::Document> image = document.saved_image(&issues);
	const Node *row = document.row(screen_row);
	const size_t position = document.screen_position(screen_row);
	if (!image || !row || position >= image->screens.size()) {
		assets_.clear(compiler_, decoder_);
		image_.reset();
		screen_ = nullptr;
		status_ = !image ? MenuScreenStatus::Unserializable : MenuScreenStatus::ScreenMissing;
		if (!image && !issues.empty()) detail_ = issues.front().message;
		else if (image && row) detail_ = row->name();
		return status_;
	}
	// Every configure is a first load of its textures (as the Shell's frame's): no other menu's
	// first load fixes a band height here.
	compiler_.reset_texture_loads();
	screen_ = &image->screens[position];
	assets_.configure(compiler_, image.get(), screen_, files, decoder_, vars);
	image_ = std::move(image); // after the configure: the compiler borrowed the new image
	status_ = MenuScreenStatus::Ready;
	return status_;
}

const menu::MenuDrawList &MenuScreenRender::compile(float scale_x, float scale_y) {
	return compiler_.compile(state_, scale_x, scale_y);
}

std::vector<menu::MenuFrameNote> MenuScreenRender::notes() const {
	std::vector<menu::MenuFrameNote> notes;
	if (status_ != MenuScreenStatus::Ready) return notes;
	notes = compiler_.build_notes();
	for (menu::MenuFrameNote &note : compiler_.layout_notes(state_)) notes.push_back(std::move(note));
	return notes;
}

} // namespace opennova::editor
