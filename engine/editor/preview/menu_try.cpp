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
	menu::MenuRuntime &runtime = shell_.runtime();
	frame_.set_configure([this](const std::string &screen, menu::MenuFrameCompiler &compiler) {
		return configure_(screen, compiler);
	});
	// The frame's click and scroll reach the runtime as the game's frame signals them (MenuDriver).
	frame_.set_clicked([&runtime](int index, int part) { runtime.on_widget_clicked(index, part); });
	frame_.set_scrolled([&runtime](int index, int value) { runtime.on_frame_scroll_value(index, value); });
	runtime.set_frame(&frame_);
	shell_.set_host(this);
	shell_.set_sink([this](const menu::MenuEvent &event) { on_event_(event); });
}

MenuTry::~MenuTry() {
	shell_.set_sink(nullptr);
	shell_.set_host(nullptr);
	assets_.clear(frame_.compiler(), decoder_);
}

bool MenuTry::configure_(const std::string &screen, menu::MenuFrameCompiler &compiler) {
	const mnu::Document *document = shell_.runtime().document();
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

std::shared_ptr<const mnu::Document> MenuTry::read_menu(const std::string &file) {
	menus_read_.insert(strutil::to_lower(file));
	return source_ && source_->menu ? source_->menu(file) : nullptr;
}

std::vector<menu::MissionChoice> MenuTry::missions() {
	return source_ && source_->missions ? source_->missions() : std::vector<menu::MissionChoice>();
}

std::vector<ExpansionRecord> MenuTry::expansions() {
	return source_ && source_->expansions ? source_->expansions() : std::vector<ExpansionRecord>();
}

std::string MenuTry::expansion() {
	return source_ ? source_->expansion : std::string();
}

bool MenuTry::start(const MenuTrySource &source, const std::string &file, const std::string &screen,
		std::string &error) {
	source_ = &source;
	start_file_ = file;
	start_screen_ = screen;
	breadcrumb_.clear();
	outcomes_.clear();
	settings_.clear();
	sounds_.clear();
	profile_ = playersav::ProfileRecord{};
	profile_.bindings = profile::default_binding_table();
	// The menu the game would show there: a mission's own menu file runs over a mission.
	shell_.set_menu_files(file, file);
	shell_.set_in_game(menu::is_mission_menu_file(file));
	(void)shell_.flow().take_expansion_reload();
	shell_.runtime().screen_history().clear();
	shell_.runtime().set_version_line(source.version_line);
	started_ = shell_.open_menu(file, screen);
	bindings_revision_ = shell_.options().bindings().revision();
	if (!started_) {
		error = "The game would show nothing of " + file + (screen.empty() ? "" : " at " + screen) + ".";
		outcomes_.clear();
	}
	++serial_;
	source_ = nullptr;
	return started_;
}

bool MenuTry::reset(const MenuTrySource &source, std::string &error) {
	shell_.forget_menus();
	menus_read_.clear();
	return start(source, start_file_, start_screen_, error);
}

void MenuTry::reload(const MenuTrySource &source) {
	if (!started_) return;
	source_ = &source;
	shell_.forget_menus();
	menus_read_.clear();
	menu::MenuRuntime &runtime = shell_.runtime();
	const std::string file = shell_.file();
	const std::string screen = runtime.current_screen();
	// The file's own back stack goes with the document it is opened anew over: its screens join the history
	// across files, which holds one history for every file as the game's scene does.
	for (const std::string &held : runtime.own_history()) runtime.screen_history().push(file, held);
	if (!shell_.open_menu(file, screen)) shell_.open_menu(start_file_, start_screen_);
	++serial_;
	source_ = nullptr;
}

void MenuTry::mouse(const MenuTrySource &source, float x, float y, bool down, bool double_click) {
	if (!started_) return;
	source_ = &source;
	shell_.runtime().process_mouse(x, y, down, double_click);
	source_ = nullptr;
}

void MenuTry::click(const MenuTrySource &source, float x, float y) {
	mouse(source, x, y, false);
	mouse(source, x, y, true);
	mouse(source, x, y, false);
}

bool MenuTry::wheel(const MenuTrySource &source, float x, float y, int steps) {
	if (!started_) return false;
	source_ = &source;
	const bool taken = shell_.runtime().process_wheel(x, y, steps);
	source_ = nullptr;
	return taken;
}

bool MenuTry::key(const MenuTrySource &source, const menu::MenuKeyInput &key) {
	if (!started_) return false;
	source_ = &source;
	// The Options remap capture takes the key while it is armed (the shell's input before the runtime's
	// keys) [orig: the capture pump @0x55c67c].
	menu::RemapInput remap;
	remap.kind = menu::RemapInput::Kind::Key;
	remap.pressed = true;
	remap.escape = key.vk == 0x1B;
	remap.shift = key.shift;
	remap.vk = key.vk;
	const int effects = shell_.consume_options_input(remap);
	bool taken = (effects & menu::OptionsScreen::Consumed) != 0;
	note_bindings_();
	if (!taken) taken = shell_.runtime().handle_key(key);
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
		if (row.hovered || row.pressed) return shell_.runtime().id_at_index(row.index);
	return -1;
}

FileStamps MenuTry::reads(const FileSource &files) const {
	FileStamps out;
	for (const menu::MenuDependency &dependency : assets_.dependencies()) out.note(dependency.name, dependency.stamp);
	for (const std::string &held : menus_read_) out.note(held, files.stamp(held));
	return out;
}

void MenuTry::outcome_(const std::string &kind, const std::string &control, const std::string &words) {
	MenuTryOutcome outcome;
	outcome.seq = ++outcome_seq_;
	outcome.kind = kind;
	outcome.control = control;
	outcome.file = shell_.file();
	outcome.screen = shell_.runtime().current_screen();
	outcome.words = words;
	outcomes_.push_back(std::move(outcome));
	if (outcomes_.size() > kOutcomesKept) outcomes_.erase(outcomes_.begin(), outcomes_.end() - kOutcomesKept);
	++serial_;
}

void MenuTry::setting_(const std::string &control, const std::string &kind, const std::string &value) {
	const std::string &file = shell_.file();
	const std::string screen = shell_.runtime().current_screen();
	for (MenuTrySetting &held : settings_)
		if (held.file == file && held.screen == screen && strutil::iequals(held.control, control)) {
			held.kind = kind;
			held.value = value;
			++serial_;
			return;
		}
	settings_.push_back({ file, screen, control, kind, value });
	++serial_;
}

// The Options screen's bindings changed (a remap, DEFAULTS, CLEAR_KEY): the sandbox's, nothing saved.
void MenuTry::note_bindings_() {
	const uint32_t revision = shell_.options().bindings().revision();
	if (revision == bindings_revision_) return;
	bindings_revision_ = revision;
	setting_("CONTROL_MAPPING", "binding", "changed (the sandbox's bindings: nothing saved)");
}

// --- what the game would do, said ----------------------------------------------------------------

void MenuTry::leave(menu::ShellExit exit, menu::MenuCommand command, const std::string &control,
		const std::string &text) {
	std::string words;
	switch (exit) {
	case menu::ShellExit::StartMission:
		words = "the game would start " + text;
		break;
	case menu::ShellExit::Resume:
		words = "the game would resume the mission";
		break;
	case menu::ShellExit::Exit:
		words = command == menu::MenuCommand::Back ? std::string("the game would quit to the desktop")
		                                           : "the game would " + std::string(menu::menu_command_words(command));
		break;
	default:
		words = "the game would " + std::string(menu::menu_command_words(command));
		break;
	}
	outcome_(menu::menu_command_token(command), control, words);
}

bool MenuTry::launch_update(const std::string &control) {
	outcome_(menu::menu_command_token(menu::MenuCommand::Update), control,
			"the game would " + std::string(menu::menu_command_words(menu::MenuCommand::Update)));
	return false;
}

void MenuTry::open_browser(const std::string &url) {
	outcome_("url", std::string(), "the game would open " + url + " in the web browser");
}

void MenuTry::fetch_page(const std::string &url) {
	outcome_("url", std::string(), "the game would fetch " + url + " in its own page view (the NovaWorld page path)");
}

void MenuTry::service(const menu::MenuEvent &event) {
	const menu::MenuRuntime &runtime = shell_.runtime();
	if (event.kind == Kind::FilterRequested) {
		outcome_("service", runtime.widget_name_of(event.id), "the game would filter the NovaWorld game list by " + event.text);
		return;
	}
	outcome_("service", event.id >= 0 ? runtime.widget_name_of(event.id) : std::string(),
			"the game would " + service_words(event.text, event.text2, event.text3));
}

void MenuTry::delegate_command(menu::MenuCommand command, const std::string &control) {
	outcome_(menu::menu_command_token(command), control,
			"the game would " + std::string(menu::menu_command_words(command)));
}

void MenuTry::notice(menu::ShellNotice notice, const std::string &control, const std::string &text,
		const std::string &text2) {
	switch (notice) {
	case menu::ShellNotice::MenuMissing:
		outcome_("missing_menu", control, "the game would not find " + text + ", so the screen would stay");
		return;
	case menu::ShellNotice::ScreenMissing:
		outcome_("missing_screen", control,
				"the game would find no screen " + text2 + " in " + text + ", so the screen would stay");
		return;
	case menu::ShellNotice::NothingSelected:
		outcome_("nothing", control, "no mission is selected, so the game would start nothing");
		return;
	case menu::ShellNotice::ExpansionUnchanged:
	case menu::ShellNotice::ExpansionQueued: {
		// The pick's row as the list shows it.
		const menu::MenuRuntime &runtime = shell_.runtime();
		std::string title = text;
		if (!shell_.commands().mod_lists().empty()) {
			const int list = shell_.commands().mod_lists().front();
			const int row = runtime.selected_row(list);
			if (row >= 0) title = runtime.item_text(list, row);
		}
		if (notice == menu::ShellNotice::ExpansionUnchanged) {
			outcome_("nothing", control, "the game would take nothing: " + title + " is the game running");
			return;
		}
		const std::string where = text.empty() ? std::string("the base game") : "expansion\\" + text;
		outcome_(menu::menu_command_token(menu::MenuCommand::ApplyExpansion), control,
				"the game would switch to " + title + " (" + where +
						") for this run, reloading everything and showing its main menu");
		return;
	}
	case menu::ShellNotice::ExpansionNeedsPackedInstall:
	case menu::ShellNotice::UpdateNotStarted:
		return;
	}
}

// The in-game dialog's OK and Cancel keep or put back what it holds (the sandbox keeps what it holds
// either way).
void MenuTry::options_effects(int effects, const std::string &control) {
	if (effects & menu::OptionsScreen::CommitPreview)
		outcome_("save_settings", control, "the game would keep the settings the dialog holds");
	if (effects & menu::OptionsScreen::RestorePreview)
		outcome_("restore_settings", control, "the game would put back the settings the dialog opened with");
}

// The runtime's events after the shell ran them: the breadcrumb, the sandbox's settings and the sounds.
void MenuTry::on_event_(const menu::MenuEvent &event) {
	const menu::MenuRuntime &runtime = shell_.runtime();
	note_bindings_();
	switch (event.kind) {
	case Kind::ScreenChanged: {
		const std::string &file = shell_.file();
		if (breadcrumb_.empty() || breadcrumb_.back().file != file || breadcrumb_.back().screen != event.text)
			breadcrumb_.push_back({ file, event.text });
		if (breadcrumb_.size() > kBreadcrumbKept)
			breadcrumb_.erase(breadcrumb_.begin(), breadcrumb_.end() - kBreadcrumbKept);
		++serial_;
		return;
	}
	case Kind::Sound: {
		MenuTrySound sound;
		sound.file = shell_.file();
		sound.screen = runtime.current_screen();
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
		return;
	}
	case Kind::EditCommitted:
		setting_(event.text, "edit", runtime.get_widget_text(event.id));
		return;
	case Kind::WidgetActivated: {
		const int kind = runtime.widget_kind_of(event.id);
		if (kind == kCheckBox) setting_(event.text, "checkbox", runtime.is_widget_checked(event.id) ? "checked" : "unchecked");
		else if (kind == kRadio || kind == kRadioEdit) setting_(event.text, "radio", "checked");
		return;
	}
	default:
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
					// A missing cell draws no text (MenuTableRow::cell reads it as empty, D-MNU-43).
					for (size_t column = 0; column < held.cells.size(); ++column)
						cells.push(json_string(retail_text_to_utf8(held.cell(int(column)))));
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
