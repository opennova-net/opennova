#include <editor/preview/menu_preview_state.h>

#include <base/io/strutil.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/mnu_document.h>
#include <editor/session/view/session_view.h>
#include <runtime/menu/menu_frame_assets.h>

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

const char *menu_preview_status_token(MenuPreviewStatus status) {
	switch (status) {
	case MenuPreviewStatus::NoProject: return "no_project";
	case MenuPreviewStatus::NoMenu: return "no_menu";
	case MenuPreviewStatus::NoScreen: return "no_screen";
	case MenuPreviewStatus::NoDevice: return "no_device";
	case MenuPreviewStatus::Unserializable: return "unserializable";
	case MenuPreviewStatus::ScreenMissing: return "screen_missing";
	case MenuPreviewStatus::Ready: return "ready";
	}
	return "no_project";
}

std::string menu_preview_status_message(MenuPreviewStatus status, const std::string &detail) {
	switch (status) {
	case MenuPreviewStatus::NoProject: return "Open a project to preview its menus.";
	case MenuPreviewStatus::NoMenu: return "Open a menu to preview its screens.";
	case MenuPreviewStatus::NoScreen: return "Select one of the menu's screens.";
	case MenuPreviewStatus::NoDevice: return "No preview renderer is attached.";
	case MenuPreviewStatus::Unserializable:
		return "The game could not read this menu as it stands" + (detail.empty() ? std::string(".") : ": " + detail);
	case MenuPreviewStatus::ScreenMissing: return "Screen " + detail + " is not in the menu the game would read.";
	case MenuPreviewStatus::Ready: return std::string();
	}
	return std::string();
}

const char *menu_preview_state_token(int state) {
	switch (state) {
	case menu::kStateMouseover: return "mouseover";
	case menu::kStateSelected: return "selected";
	case menu::kStateDisabled: return "disabled";
	default: return "normal";
	}
}

bool menu_preview_state_from_token(const std::string &token, int &out) {
	if (token == "normal") out = -1;
	else if (token == "mouseover") out = menu::kStateMouseover;
	else if (token == "selected") out = menu::kStateSelected;
	else if (token == "disabled") out = menu::kStateDisabled;
	else return false;
	return true;
}

namespace {

// The frame state's row for a widget, made on first use (the runtime's per-widget state).
menu::MenuWidgetState &widget_state(menu::MenuFrameState &state, int index) {
	for (menu::MenuWidgetState &row : state.widgets)
		if (row.index == index) return row;
	menu::MenuWidgetState row;
	row.index = index;
	state.widgets.push_back(row);
	return state.widgets.back();
}

} // namespace

void apply_menu_preview_options(const MenuPreviewOptions &options, int forced_index, const menu::MenuFrameCompiler &compiler,
                                menu::MenuFrameState &state) {
	if (options.show_hidden) {
		for (int index = 0; index < compiler.widget_count(); ++index) {
			menu::MenuWidgetState &row = widget_state(state, index);
			row.hide = false;
			row.show = true;
		}
	}
	if (forced_index < 0 || forced_index >= compiler.widget_count()) return;
	menu::MenuWidgetState &row = widget_state(state, forced_index);
	// The pump's verdicts: hovered with the button up is state 2, held down state 3; the
	// runtime's disable replaces the authored one.
	switch (options.force_state) {
	case menu::kStateMouseover: row.hovered = true; break;
	case menu::kStateSelected: row.hovered = row.pressed = true; break;
	case menu::kStateDisabled: row.has_disabled = row.disabled = true; break;
	default: break;
	}
	if (options.checked) row.has_checked = row.checked = true;
	if (options.popup_open) row.popup_open = true;
	if (options.focused) {
		row.focused = true;
		// A clock in the caret's shown half-second (MenuFrameState::time_ms).
		state.time_ms = 0x300;
	}
}

void MenuPreviewModel::set_options(const MenuPreviewOptions &options) {
	if (options == options_) return;
	options_ = options;
	++options_serial_;
}

MenuPreviewAction MenuPreviewModel::stop_(MenuPreviewStatus status, const std::string &detail) {
	status_ = status;
	detail_ = detail;
	screen_ = nullptr;
	dependencies_.clear();
	missing_.clear();
	unreadable_.clear();
	forced_index_ = -1;
	const bool was = device_configured_;
	device_configured_ = false;
	return was ? MenuPreviewAction::Clear : MenuPreviewAction::Keep;
}

bool MenuPreviewModel::dependencies_moved_(const FileSource &files) const {
	for (const menu::MenuDependency &dependency : dependencies_)
		if (files.stamp(dependency.name) != dependency.stamp) return true;
	return false;
}

MenuPreviewAction MenuPreviewModel::follow(const SessionView &view) {
	retired_.reset(); // the device has configured past the image it replaced
	if (!view.project.open || !view.findings.assets) {
		have_shown_ = false;
		return stop_(MenuPreviewStatus::NoProject, std::string());
	}
	const MnuDocument *document = nullptr;
	bool menu_open = false; // a menu is open, though no screen of it is selected yet
	for (const auto &open : view.documents.open) {
		const auto *menu = dynamic_cast<const MnuDocument *>(open.get());
		menu_open = menu_open || menu;
		if (menu && menu->path() == view.documents.previews.menu.path) document = menu;
	}
	const Node *row = document ? document->row(view.documents.previews.menu.screen) : nullptr;
	if (!document || !row) {
		have_shown_ = false;
		return stop_(menu_open ? MenuPreviewStatus::NoScreen : MenuPreviewStatus::NoMenu, std::string());
	}
	const Key key{document->identity(), document->revision(), options_serial_, row->id};
	const uint64_t generation = view.findings.assets->generation();
	if (have_shown_ && key == shown_) {
		// A menu the game could not read stays that way until it changes.
		if (failed_) return MenuPreviewAction::Keep;
		if (generation == generation_) return MenuPreviewAction::Keep;
		generation_ = generation;
		if (!dependencies_moved_(*view.findings.assets)) return MenuPreviewAction::Keep;
	}
	have_shown_ = true;
	shown_ = key;
	shown_path_ = document->path();
	generation_ = generation;
	std::vector<SourceIssue> issues;
	std::shared_ptr<const mnu::Document> image = document->saved_image(&issues);
	if (!image) {
		failed_ = true;
		return stop_(MenuPreviewStatus::Unserializable, issues.empty() ? std::string() : issues.front().message);
	}
	const size_t position = document->screen_position(row->id);
	if (position >= image->screens.size()) {
		failed_ = true;
		return stop_(MenuPreviewStatus::ScreenMissing, row->name());
	}
	failed_ = false;
	if (image != image_) retired_ = std::move(image_);
	image_ = std::move(image);
	screen_ = &image_->screens[position];
	style_vars_ = style_.vars(*view.findings.assets);
	forced_index_ = -1;
	if (options_.force_window) {
		const NodeAddress forced = document->address_of(options_.force_window);
		if (forced.row == row->id) forced_index_ = document->window_index(forced);
	}
	status_ = MenuPreviewStatus::Ready;
	detail_.clear();
	device_configured_ = true;
	return MenuPreviewAction::Configure;
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

void MenuPreviewModel::configured(const menu::MenuFrameAssets &assets) {
	dependencies_.clear();
	style_.dependencies(dependencies_);
	dependencies_.insert(dependencies_.end(), assets.dependencies().begin(), assets.dependencies().end());
	split_unloaded(assets, missing_, unreadable_);
}

} // namespace opennova::editor
