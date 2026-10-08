#include <editor/preview/menu_try.h>

#include <algorithm>
#include <cctype>

#include <base/io/cp1252.h>
#include <base/io/strutil.h>
#include <runtime/menu/menu_sound.h>
#include <runtime/profile/player_profiles.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

using Kind = menu::MenuEvent::Kind;

constexpr int kCheckBox = int(mnu::WindowType::CheckBox);
constexpr int kRadio = int(mnu::WindowType::Radio);
constexpr int kRadioEdit = int(mnu::WindowType::RadioEdit);

// What a service verb's row would do, as "the game would ..." ends [orig: CUIWidget_HandleScriptedAction
// @ 0x6497f0, the service arms] (docs/mnu/menu-re.md "Activation and the ACTION walk").
std::string service_words(const std::string &verb, const std::string &text, const std::string &file) {
	const std::string named = text.empty() ? std::string() : " " + text;
	if (verb == "FORM_POST") return "post the NovaWorld login form";
	if (verb == "GLB_LOAD") return "load the NovaWorld game list";
	if (verb == "GLB_LOADANDPING") return "load the NovaWorld game list and ping each game";
	if (verb == "GLB_PING") return "ping the NovaWorld games listed";
	if (verb == "GLB_JOIN") return "join the highlighted NovaWorld game";
	if (verb == "APPMSG") return "send the application the message" + named;
	if (verb == "LAN_SEARCH") return "search the LAN for games";
	if (verb == "LAN_JOIN") return "join the highlighted LAN game";
	if (verb == "MNX") return "run the NovaWorld UI script " + (file.empty() ? text : file);
	return "run " + verb;
}

// A virtual-key name and its code: a HOTKEY's (CWnd_ParseVirtualKeyNameW @ 0x6467f0 reads VK_RETURN,
// VK_ESCAPE and VK_SPACE), and the keys the edit field and the focus read (menu_edit.h; Tab, the TAB rows).
struct KeyName {
	const char *vk;
	const char *plain;
	int code;
};
constexpr KeyName kKeys[] = {
	{ "VK_RETURN", "Enter", 0x0D },
	{ "VK_ESCAPE", "Escape", 0x1B },
	{ "VK_SPACE", "Space", 0x20 },
	{ "VK_BACK", "Backspace", 0x08 },
	{ "VK_TAB", "Tab", 0x09 },
	{ "VK_PRIOR", "PageUp", 0x21 },
	{ "VK_NEXT", "PageDown", 0x22 },
	{ "VK_END", "End", 0x23 },
	{ "VK_HOME", "Home", 0x24 },
	{ "VK_LEFT", "Left", 0x25 },
	{ "VK_UP", "Up", 0x26 },
	{ "VK_RIGHT", "Right", 0x27 },
	{ "VK_DOWN", "Down", 0x28 },
	{ "VK_DELETE", "Delete", 0x2E },
};

JsonValue name_or_null(const menu::MenuRuntime &runtime, int id) {
	return id >= 0 ? json_string(runtime.widget_name_of(id)) : JsonValue::make_null();
}

JsonValue visit_json(const std::string &file, const std::string &screen) {
	JsonValue out = JsonValue::make_object();
	out.set("file", json_string(file));
	out.set("screen", json_string(screen));
	return out;
}

JsonValue outcome_json(const MenuTryOutcome &outcome) {
	JsonValue out = JsonValue::make_object();
	out.set("seq", json_number(double(outcome.seq)));
	out.set("kind", json_string(outcome.kind));
	out.set("control", json_string(outcome.control));
	out.set("file", json_string(outcome.file));
	out.set("screen", json_string(outcome.screen));
	out.set("words", json_string(outcome.words));
	return out;
}

} // namespace

MenuTry::MenuTry() {
	frame_.set_configure([this](const std::string &screen, menu::MenuFrameCompiler &compiler) {
		return configure_(screen, compiler);
	});
	// The frame's click and scroll reach the runtime as the game's frame signals them (MenuDriver).
	frame_.set_clicked([this](int index) { runtime_.on_widget_clicked(index); });
	frame_.set_scrolled([this](int index, int value) { runtime_.on_frame_scroll_value(index, value); });
	runtime_.set_frame(&frame_);
	runtime_.set_sink([this](const menu::MenuEvent &event) { on_event_(event); });
	// The single-player screen's controls, the shell's (menu_commands.h).
	flow_.set_mission_controls(menu::menu_name_set(menu::MenuNameSet::SinglePlayerLists),
			menu::menu_name_set(menu::MenuNameSet::Briefings),
			menu::menu_name_set(menu::MenuNameSet::SinglePlayerAccepts));
	mods_.set_descriptions(menu::menu_name_set(menu::MenuNameSet::ModDescriptions));
}

MenuTry::~MenuTry() {
	runtime_.set_sink(nullptr);
	assets_.clear(frame_.compiler(), decoder_);
}

bool MenuTry::configure_(const std::string &screen, menu::MenuFrameCompiler &compiler) {
	const mnu::Document *document = image_.get();
	const mnu::Screen *found = !document ? nullptr : screen.empty() ? document->first_screen()
	                                                                : document->find_screen(screen);
	screen_ = found;
	if (!found || !source_ || !source_->files) return false;
	static const std::map<std::string, std::string> kNoVars;
	// Every configure a first load of its textures, as the device's own (the editor's menu device).
	compiler.reset_texture_loads();
	assets_.configure(compiler, document, found, *source_->files, decoder_, source_->vars ? *source_->vars : kNoVars);
	return true;
}

std::shared_ptr<const mnu::Document> MenuTry::menu_(const std::string &file) {
	Held &held = menus_[strutil::to_lower(file)];
	if (!held.read) {
		held.read = true;
		held.image = source_ && source_->menu ? source_->menu(file) : nullptr;
	}
	return held.image;
}

bool MenuTry::start(const MenuTrySource &source, const std::string &file, const std::string &screen,
		std::string &error) {
	source_ = &source;
	start_file_ = file;
	start_screen_ = screen;
	in_mission_ = menu::is_mission_menu_file(file);
	breadcrumb_.clear();
	outcomes_.clear();
	settings_.clear();
	sounds_.clear();
	profile_ = playersav::ProfileRecord{};
	profile_.bindings = profile::default_binding_table();
	runtime_.screen_history().clear();
	runtime_.set_game_code(std::string());
	started_ = open_(file, screen);
	if (!started_) error = "The game would show nothing of " + file + (screen.empty() ? "" : " at " + screen) + ".";
	++serial_;
	source_ = nullptr;
	return started_;
}

bool MenuTry::reset(const MenuTrySource &source, std::string &error) {
	menus_.clear();
	return start(source, start_file_, start_screen_, error);
}

void MenuTry::reload(const MenuTrySource &source) {
	if (!started_) return;
	source_ = &source;
	menus_.clear();
	const std::string file = file_;
	const std::string screen = runtime_.current_screen();
	// The file's own back stack goes with the document it is opened anew over: its screens join the history
	// across files, which holds one history for every file as the game's scene does.
	for (const std::string &held : runtime_.own_history()) runtime_.screen_history().push(file, held);
	if (!open_(file, screen)) open_(start_file_, start_screen_);
	++serial_;
	source_ = nullptr;
}

bool MenuTry::open_(const std::string &file, const std::string &screen) {
	std::shared_ptr<const mnu::Document> image = menu_(file);
	if (!image || image->screens.empty()) return false;
	// The shell's open (menu_shell.gd open_menu): the selected mission cleared, the document opened, its
	// Options surface prepared, its commands bound and its lists filled.
	flow_.clear_selected_mission();
	image_ = std::move(image);
	file_ = file;
	if (!runtime_.open_document(image_.get(), file, screen)) return false;
	prepare_();
	return true;
}

void MenuTry::prepare_() {
	options_.prepare(runtime_, &profile_);
	options_.apply_policy(runtime_);
	options_.seed_profile(runtime_, &profile_);
	commands_.wire(runtime_, in_mission_);
	flow_.clear_rows();
	if (!commands_.mission_lists().empty() && source_ && source_->missions) {
		const std::vector<menu::MissionChoice> rows = source_->missions();
		for (const int id : commands_.mission_lists()) flow_.seed_missions(runtime_, id, rows);
	}
	// The Mods lists: the base game's row, then the game folder's expansions, the one running highlighted.
	if (!commands_.mod_lists().empty() && source_) {
		expansion_ = source_->expansion;
		mods_.set_records(source_->expansions ? source_->expansions() : std::vector<ExpansionRecord>());
		for (const int id : commands_.mod_lists()) mods_.populate(runtime_, id, expansion_);
	}
}

void MenuTry::mouse(const MenuTrySource &source, float x, float y, bool down, uint32_t now_ms) {
	if (!started_) return;
	source_ = &source;
	runtime_.process_mouse(x, y, down, now_ms);
	source_ = nullptr;
}

void MenuTry::click(const MenuTrySource &source, float x, float y, uint32_t now_ms) {
	mouse(source, x, y, false, now_ms);
	mouse(source, x, y, true, now_ms);
	mouse(source, x, y, false, now_ms);
}

bool MenuTry::wheel(const MenuTrySource &source, float x, float y, int steps) {
	if (!started_) return false;
	source_ = &source;
	const bool taken = runtime_.process_wheel(x, y, steps);
	source_ = nullptr;
	return taken;
}

bool MenuTry::key(const MenuTrySource &source, const menu::MenuKeyInput &key) {
	if (!started_) return false;
	source_ = &source;
	// The Options remap capture takes the key while it is armed (options_menu_controller.gd's
	// consume_input before the runtime's keys) [orig: the capture pump @0x55c67c].
	menu::RemapInput remap;
	remap.kind = menu::RemapInput::Kind::Key;
	remap.pressed = true;
	remap.escape = key.vk == 0x1B;
	remap.shift = key.shift;
	remap.vk = key.vk;
	const uint32_t revision = options_.bindings().revision();
	const int effects = options_.consume(runtime_, remap);
	bool taken = (effects & menu::OptionsScreen::Consumed) != 0;
	if (options_.bindings().revision() != revision)
		setting_("CONTROL_MAPPING", "binding", "changed (the sandbox's bindings: nothing saved)");
	if (!taken) taken = runtime_.handle_key(key);
	source_ = nullptr;
	return taken;
}

std::vector<MenuTrySound> MenuTry::take_sounds() {
	std::vector<MenuTrySound> out;
	out.swap(sounds_);
	return out;
}

int MenuTry::hovered() const {
	const menu::MenuFrameState &state = frame_.state();
	for (const menu::MenuWidgetState &row : state.widgets)
		if (row.hovered || row.pressed) return runtime_.id_at_index(row.index);
	return -1;
}

FileStamps MenuTry::reads(const FileSource &files) const {
	FileStamps out;
	for (const menu::MenuDependency &dependency : assets_.dependencies()) out.note(dependency.name, dependency.stamp);
	for (const auto &held : menus_)
		if (held.second.read) out.note(held.first, files.stamp(held.first));
	return out;
}

bool MenuTry::pop_history_() {
	// In a mission the history's top is the mark on the screen the mission was started from, which pops
	// nothing (D-MNU-28); a row whose file does not load changes nothing.
	menu::ScreenHistoryRow row;
	if (!runtime_.screen_history().pop(&row)) return false;
	const std::string file = row.file.empty() ? start_file_ : row.file;
	if (!open_(file, row.screen)) {
		outcome_("missing_menu", std::string(), "the game would not find " + file + ", so the screen would stay");
		return false;
	}
	return true;
}

void MenuTry::outcome_(const std::string &kind, const std::string &control, const std::string &words) {
	MenuTryOutcome outcome;
	outcome.seq = ++outcome_seq_;
	outcome.kind = kind;
	outcome.control = control;
	outcome.file = file_;
	outcome.screen = runtime_.current_screen();
	outcome.words = words;
	outcomes_.push_back(std::move(outcome));
	if (outcomes_.size() > kOutcomesKept) outcomes_.erase(outcomes_.begin(), outcomes_.end() - kOutcomesKept);
	++serial_;
}

void MenuTry::setting_(const std::string &control, const std::string &kind, const std::string &value) {
	const std::string screen = runtime_.current_screen();
	for (MenuTrySetting &held : settings_)
		if (held.file == file_ && held.screen == screen && strutil::iequals(held.control, control)) {
			held.kind = kind;
			held.value = value;
			++serial_;
			return;
		}
	settings_.push_back({ file_, screen, control, kind, value });
	++serial_;
}

void MenuTry::on_command_(menu::MenuCommand command, const std::string &control) {
	const std::string words = menu::menu_command_words(command);
	switch (command) {
	case menu::MenuCommand::None: return;
	case menu::MenuCommand::StartMission: {
		// The launch reads the list's selection alone: with none, nothing starts (menu_shell.gd
		// _on_start_control; the retail ACCEPT is disabled until a pick, MenuFlow).
		const std::string &mission = flow_.selected_mission();
		if (mission.empty()) {
			outcome_("nothing", control, "no mission is selected, so the game would start nothing");
			return;
		}
		outcome_(menu::menu_command_token(command), control, "the game would start " + mission);
		return;
	}
	case menu::MenuCommand::ApplyExpansion: {
		// ACCEPT's pick of the Mods list: the game running takes nothing; another switches the game for the
		// run, the menu booted anew at its main menu (menu_flow.h request_expansion, D-MNU-31).
		if (commands_.mod_lists().empty()) return;
		const int list = commands_.mod_lists().front();
		const std::string pick = mods_.pick(runtime_, list);
		const int row = runtime_.selected_row(list);
		const std::string title = row >= 0 ? runtime_.item_text(list, row) : pick;
		if (strutil::iequals(pick, expansion_)) {
			outcome_("nothing", control, "the game would take nothing: " + title + " is the game running");
			return;
		}
		const std::string where = pick.empty() ? std::string("the base game") : "expansion\\" + pick;
		outcome_(menu::menu_command_token(command), control, "the game would switch to " + title + " (" + where +
				") for this run, reloading everything and showing its main menu");
		return;
	}
	case menu::MenuCommand::Back:
		// The history across files first; then in a mission the resume, else the quit.
		if (pop_history_()) return;
		outcome_(menu::menu_command_token(command), control,
				in_mission_ ? "the game would resume the mission" : "the game would quit to the desktop");
		return;
	default:
		outcome_(menu::menu_command_token(command), control, "the game would " + words);
		return;
	}
}

void MenuTry::on_event_(const menu::MenuEvent &event) {
	switch (event.kind) {
	case Kind::ScreenChanged: {
		// A screen change drops an armed remap (options_menu_controller.gd _on_screen_changed).
		options_.end_remap(runtime_, true);
		if (breadcrumb_.empty() || breadcrumb_.back().file != file_ || breadcrumb_.back().screen != event.text)
			breadcrumb_.push_back({ file_, event.text });
		if (breadcrumb_.size() > kBreadcrumbKept)
			breadcrumb_.erase(breadcrumb_.begin(), breadcrumb_.end() - kBreadcrumbKept);
		++serial_;
		return;
	}
	case Kind::MenuRequested: {
		// A jump to another menu (menu_shell.gd _on_menu_requested): a menu the game cannot read, or a
		// target screen it lacks, changes nothing; else it opens there and the screen it leaves is pushed.
		std::shared_ptr<const mnu::Document> image = menu_(event.text);
		if (!image) {
			outcome_("missing_menu", std::string(),
					"the game would not find " + event.text + ", so the screen would stay");
			return;
		}
		if (!event.text2.empty() && !image->find_screen(event.text2)) {
			outcome_("missing_screen", std::string(),
					"the game would find no screen " + event.text2 + " in " + event.text + ", so the screen would stay");
			return;
		}
		const std::string from_file = file_;
		const std::string from_screen = runtime_.current_screen();
		if (open_(event.text, event.text2)) runtime_.screen_history().push(from_file, from_screen);
		return;
	}
	case Kind::PopRequested:
		pop_history_();
		return;
	case Kind::UrlRequested:
		outcome_("url", std::string(),
				event.flag ? "the game would open " + event.text + " in the web browser"
				           : "the game would fetch " + event.text + " in its own page view (the NovaWorld page path)");
		return;
	case Kind::ServiceRequested:
		outcome_("service", event.id >= 0 ? runtime_.widget_name_of(event.id) : std::string(),
				"the game would " + service_words(event.text, event.text2, event.text3));
		return;
	case Kind::FilterRequested:
		outcome_("service", runtime_.widget_name_of(event.id),
				"the game would filter the NovaWorld game list by " + event.text);
		return;
	case Kind::Sound: {
		MenuTrySound sound;
		sound.file = file_;
		sound.screen = runtime_.current_screen();
		sound.id = event.id;
		sound.window = event.text3;
		sound.state = event.value;
		sound.bank = event.text;
		sound.trigger = event.text2;
		sounds_.push_back(std::move(sound));
		return;
	}
	case Kind::ValueChanged: {
		std::string value = event.text3;
		if (event.text2 != "edit" && event.text2 != "multiline" && event.text2 != "scroll")
			value = (value.empty() ? std::string("row ") + std::to_string(event.value) : value);
		else if (event.text2 == "scroll")
			value = std::to_string(event.value);
		setting_(event.text, event.text2, value);
		// A pick of a mission list selects its mission, of a Mods list describes its game (menu_shell.gd
		// _on_widget_value_changed).
		const int id = runtime_.widget_id(event.text);
		if (event.text2 == "list" &&
				std::find(commands_.mission_lists().begin(), commands_.mission_lists().end(), id) !=
						commands_.mission_lists().end())
			flow_.select_mission(runtime_, id, event.value, event.text3);
		else if (event.text2 == "list" &&
				std::find(commands_.mod_lists().begin(), commands_.mod_lists().end(), id) != commands_.mod_lists().end())
			mods_.select(runtime_, id, event.value);
		return;
	}
	case Kind::EditCommitted:
		setting_(event.text, "edit", runtime_.get_widget_text(event.id));
		return;
	case Kind::WidgetActivated: {
		const int kind = runtime_.widget_kind_of(event.id);
		if (kind == kCheckBox) setting_(event.text, "checkbox", runtime_.is_widget_checked(event.id) ? "checked" : "unchecked");
		else if (kind == kRadio || kind == kRadioEdit) setting_(event.text, "radio", "checked");
		// The Options surface's own controls (the device radios, DEFAULTS, CLEAR_KEY, its ACCEPT), over the
		// sandbox's profile record.
		const std::string screen = runtime_.current_screen();
		const uint32_t revision = options_.bindings().revision();
		const int effects = options_.activate(runtime_, &profile_, event.text);
		if (options_.bindings().revision() != revision)
			setting_("CONTROL_MAPPING", "binding", "changed (the sandbox's bindings: nothing saved)");
		// The in-game dialog's OK and Cancel keep or put back what it holds, then show the menu again
		// (options_menu_controller.gd; the sandbox keeps what it holds either way).
		if (effects & menu::OptionsScreen::CommitPreview)
			outcome_("save_settings", event.text, "the game would keep the settings the dialog holds");
		if (effects & menu::OptionsScreen::RestorePreview)
			outcome_("restore_settings", event.text, "the game would put back the settings the dialog opened with");
		if (effects & (menu::OptionsScreen::CommitPreview | menu::OptionsScreen::RestorePreview))
			menu::OptionsScreen::show_ingame_main(runtime_);
		on_command_(commands_.command_of(screen, event.text), event.text);
		return;
	}
	case Kind::ListActivated: {
		// A double click: a mission list's starts its mission, the controls table's arms the remap.
		const std::string name = runtime_.widget_name_of(event.id);
		if (std::find(commands_.mission_lists().begin(), commands_.mission_lists().end(), event.id) !=
				commands_.mission_lists().end()) {
			flow_.activate_mission(event.id, event.value);
			on_command_(menu::MenuCommand::StartMission, name);
			return;
		}
		options_.arm(runtime_, event.id, event.value);
		return;
	}
	case Kind::MusicVar:
	case Kind::HoverChanged:
	case Kind::ShownChanged:
	case Kind::TableCellClicked:
		return;
	}
}

// --- the wire ------------------------------------------------------------------------------------

io::JsonValue menu_try_to_json(const MenuTry &menu_try) {
	JsonValue out = JsonValue::make_object();
	out.set("on", JsonValue::make_bool(true));
	const menu::MenuRuntime &runtime = menu_try.runtime();
	out.set("started", JsonValue::make_bool(menu_try.started()));
	out.set("file", json_string(menu_try.file()));
	out.set("screen", json_string(menu_try.screen_name()));
	out.set("in_mission", JsonValue::make_bool(menu_try.in_mission()));
	out.set("start", visit_json(menu_try.start_file(), menu_try.start_screen()));
	JsonValue crumbs = JsonValue::make_array();
	for (const MenuTryVisit &visit : menu_try.breadcrumb()) crumbs.push(visit_json(visit.file, visit.screen));
	out.set("breadcrumb", std::move(crumbs));
	JsonValue history = JsonValue::make_array();
	for (const menu::ScreenHistoryRow &row : runtime.screen_history().rows())
		if (!row.mark) history.push(visit_json(row.file, row.screen));
	out.set("history", std::move(history));
	out.set("focus", name_or_null(runtime, runtime.focused_widget()));
	out.set("popup", name_or_null(runtime, runtime.open_popup()));
	out.set("hovered", name_or_null(runtime, menu_try.hovered()));
	JsonValue outcomes = JsonValue::make_array();
	for (const MenuTryOutcome &outcome : menu_try.outcomes()) outcomes.push(outcome_json(outcome));
	out.set("outcomes", std::move(outcomes));
	out.set("last", menu_try.outcomes().empty() ? JsonValue::make_null() : outcome_json(menu_try.outcomes().back()));
	JsonValue settings = JsonValue::make_array();
	for (const MenuTrySetting &setting : menu_try.settings()) {
		JsonValue row = JsonValue::make_object();
		row.set("file", json_string(setting.file));
		row.set("screen", json_string(setting.screen));
		row.set("control", json_string(setting.control));
		row.set("kind", json_string(setting.kind));
		row.set("value", json_string(setting.value));
		settings.push(std::move(row));
	}
	out.set("settings", std::move(settings));
	return out;
}

io::JsonValue menu_try_widgets_json(const MenuTry &menu_try) {
	JsonValue out = JsonValue::make_array();
	if (!menu_try.started()) return out;
	const menu::MenuRuntime &runtime = menu_try.runtime();
	const menu::MenuFrameCompiler &compiler = menu_try.compiler();
	const menu::MenuFrameState &state = menu_try.state();
	for (int index = 0; index < compiler.widget_count(); ++index) {
		JsonValue widget = JsonValue::make_object();
		const int id = runtime.id_at_index(index);
		const int type = compiler.widget_kind(index);
		widget.set("index", json_number(index));
		widget.set("name", json_string(compiler.widget_name(index)));
		widget.set("type", json_string(type >= 0 ? mnu::window_type_name(static_cast<mnu::WindowType>(type)) : ""));
		mnu::RectEdges rect{};
		if (compiler.widget_rect(index, state, &rect)) {
			JsonValue edges = JsonValue::make_array();
			for (const int edge : { rect.left, rect.top, rect.right, rect.bottom }) edges.push(json_number(edge));
			widget.set("rect", std::move(edges));
		}
		widget.set("shown", JsonValue::make_bool(compiler.widget_shown(index, state)));
		widget.set("disabled", JsonValue::make_bool(compiler.widget_disabled(index, state)));
		const menu::MenuWidgetState *row = menu::find_frame_widget(state, index);
		widget.set("hovered", JsonValue::make_bool(row && row->hovered));
		widget.set("pressed", JsonValue::make_bool(row && row->pressed));
		if (id >= 0) {
			if (type == kCheckBox || type == kRadio || type == kRadioEdit)
				widget.set("checked", JsonValue::make_bool(runtime.is_widget_checked(id)));
			widget.set("focused", JsonValue::make_bool(runtime.focused_widget() == id));
			// Text as the game draws it, each byte a glyph of the code page (a retail VERSION line's 0xA9).
			const std::string text = runtime.get_widget_text(id);
			if (!text.empty()) widget.set("text", json_string(retail_text_to_utf8(text)));
			const int count = runtime.item_count(id);
			if (count > 0) {
				const int selected = runtime.selected_row(id);
				widget.set("selected", json_number(selected));
				JsonValue items = JsonValue::make_array();
				for (int item = 0; item < count; ++item)
					items.push(json_string(retail_text_to_utf8(runtime.item_display_text(id, item))));
				widget.set("items", std::move(items));
			}
			// A table's rows, each its cells (a code-filled table: the controls the game binds).
			const int rows = type == int(mnu::WindowType::Table) ? runtime.table_row_count(id) : 0;
			if (rows > 0 && row) {
				JsonValue table = JsonValue::make_array();
				for (const menu::MenuTableRow &held : row->table_rows) {
					JsonValue cells = JsonValue::make_array();
					for (const std::string &cell : held.cells) cells.push(json_string(retail_text_to_utf8(cell)));
					table.push(std::move(cells));
				}
				widget.set("rows", std::move(table));
				JsonValue selected = JsonValue::make_array();
				for (const int row : runtime.table_selected_rows(id)) selected.push(json_number(row));
				widget.set("selected_rows", std::move(selected));
			}
			menu::MenuScrollRangeState range;
			if (type == int(mnu::WindowType::Scroll) && runtime.get_widget_scroll_range(id, range)) {
				widget.set("value", json_number(range.value));
				JsonValue span = JsonValue::make_array();
				span.push(json_number(range.minimum));
				span.push(json_number(range.maximum));
				widget.set("range", std::move(span));
			}
		}
		out.push(std::move(widget));
	}
	return out;
}

bool menu_try_key_from_name(const std::string &name, bool shift, menu::MenuKeyInput &out) {
	out = menu::MenuKeyInput();
	out.shift = shift;
	for (const KeyName &key : kKeys)
		if (strutil::iequals(name, key.vk) || strutil::iequals(name, key.plain)) {
			out.vk = key.code;
			// The keys that type a character type it too (WM_CHAR after WM_KEYDOWN).
			if (key.code == 0x20) out.unicode = ' ';
			return true;
		}
	// One printable character: its key's code where it has one (a letter's capital, a digit), and it typed.
	if (name.size() != 1) return false;
	const unsigned char c = static_cast<unsigned char>(name[0]);
	if (c < 0x20 || c > 0x7E) return false;
	if (std::isalpha(c)) out.vk = std::toupper(c);
	else if (std::isdigit(c)) out.vk = c;
	out.unicode = c;
	out.printable_keycode = c;
	return true;
}

} // namespace opennova::editor
