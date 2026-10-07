// A menu run headless through the runtime's own driver (ADR 0046 DI-35): the frame seam over a
// compiler and its state (menu/menu_state_frame.h), its writes the ones Godot's MenuFrame lands
// (frame_set_*, the edit field's input), its mouse the compiler's own pump at design scale and its
// click the game frame's (MenuClickLatch); the runtime's Sound events naming whose sound it is and its
// ServiceRequested rows; what the game's code binds by name (menu/menu_commands.h); and the Options
// policy the engine applies (OptionsScreen::apply_policy).
//
// Pinned: a hover plays MOUSEIN with the widget's id, state and NAME; a click runs the button's SCREEN
// row through the frame's pump (SELECTED first) and the next screen is configured; Esc reaches the
// hotkey whose POP_SCREEN goes back; a check box toggles, an edit takes the focus and the typed
// characters land in the frame's state; a LAN_SEARCH row raises ServiceRequested; the shell's and the
// screens' commands by name (a play screen's launch, a delegate's ownership, a mission's own); the
// Options sliders, pinned rows and locked controls.
#include <formats/mnu/mnu.h>
#include <runtime/controls/binding_set.h>
#include <runtime/menu/menu_commands.h>
#include <runtime/menu/menu_runtime.h>
#include <runtime/menu/menu_sound.h>
#include <runtime/menu/menu_state_frame.h>
#include <runtime/menu/options_screen.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::menu;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

const char *kMenu = R"(<SCREEN>
	<NAME>STARTUP</NAME>
	<WINDOW type="window" name="MAIN">
		<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
		<WINDOW type="button" name="GO">
			<ACTION type="screen" file="main.mnu">SECOND</ACTION>
			<SOUND state="mousein" trigger="OVER">menu.lwf</SOUND>
			<SOUND state="selected" trigger="CLICK">menu.lwf</SOUND>
			<POSITION><LEFT>100</LEFT><TOP>100</TOP><RIGHT>300</RIGHT><BOTTOM>140</BOTTOM></POSITION>
		</WINDOW>
		<WINDOW type="checkbox" name="CHECK">
			<POSITION><LEFT>100</LEFT><TOP>200</TOP><RIGHT>300</RIGHT><BOTTOM>240</BOTTOM></POSITION>
		</WINDOW>
		<WINDOW type="edit" name="NAME">
			<POSITION><LEFT>100</LEFT><TOP>300</TOP><RIGHT>300</RIGHT><BOTTOM>340</BOTTOM></POSITION>
		</WINDOW>
		<WINDOW type="button" name="LAN">
			<ACTION type="lan_search">ANY</ACTION>
			<POSITION><LEFT>100</LEFT><TOP>400</TOP><RIGHT>300</RIGHT><BOTTOM>440</BOTTOM></POSITION>
		</WINDOW>
		<WINDOW type="button" name="EXIT">
			<POSITION><LEFT>400</LEFT><TOP>100</TOP><RIGHT>600</RIGHT><BOTTOM>140</BOTTOM></POSITION>
		</WINDOW>
	</WINDOW>
</SCREEN>
<SCREEN>
	<NAME>SECOND</NAME>
	<WINDOW type="window" name="MAIN">
		<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
		<WINDOW type="button" name="BACK">
			<HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY>
			<ACTION type="pop_screen"></ACTION>
			<POSITION><LEFT>100</LEFT><TOP>500</TOP><RIGHT>300</RIGHT><BOTTOM>540</BOTTOM></POSITION>
		</WINDOW>
	</WINDOW>
</SCREEN>
)";

struct Run {
	mnu::Document doc;
	MenuStateFrame frame;
	MenuRuntime rt;
	std::vector<MenuEvent> events;

	bool open(const char *text, const char *file) {
		std::string error;
		if (!mnu::parse(text, doc, error)) return false;
		frame.set_configure([this](const std::string &screen, MenuFrameCompiler &compiler) {
			const mnu::Screen *found = screen.empty() ? doc.first_screen() : doc.find_screen(screen);
			compiler.configure(found);
			return found != nullptr;
		});
		frame.set_clicked([this](int index) { rt.on_widget_clicked(index); });
		frame.set_scrolled([this](int index, int value) { rt.on_frame_scroll_value(index, value); });
		rt.set_frame(&frame);
		rt.set_sink([this](const MenuEvent &e) { events.push_back(e); });
		return rt.open_document(&doc, file, std::string());
	}
	void click(float x, float y) {
		rt.process_mouse(x, y, false, 1000);
		rt.process_mouse(x, y, true, 1000);
		rt.process_mouse(x, y, false, 1000);
	}
	std::vector<const MenuEvent *> of(MenuEvent::Kind kind) const {
		std::vector<const MenuEvent *> out;
		for (const MenuEvent &e : events)
			if (e.kind == kind) out.push_back(&e);
		return out;
	}
	const MenuWidgetState *row(const char *name) const {
		return find_frame_widget(frame.state(), rt.frame_index(rt.widget_id(name)));
	}
};

MenuKeyInput vk(int code, int unicode = 0) {
	MenuKeyInput key;
	key.vk = code;
	key.unicode = unicode;
	return key;
}

void test_runtime_over_the_state_frame() {
	Run run;
	CHECK(run.open(kMenu, "main.mnu"));
	CHECK(run.frame.is_configured() && run.rt.current_screen() == "STARTUP" && run.frame.configures() == 1);
	// Onto GO: the pump hovers it, and its MOUSEIN names GO by its id, state and NAME.
	const uint64_t before = run.frame.serial();
	run.rt.process_mouse(150, 120, false, 1000);
	const MenuWidgetState *go = run.row("GO");
	CHECK(go && go->hovered && run.frame.serial() != before);
	std::vector<const MenuEvent *> sounds = run.of(MenuEvent::Kind::Sound);
	CHECK(sounds.size() == 1 && sounds[0]->id == run.rt.widget_id("GO") && sounds[0]->value == kSoundMouseIn &&
	      sounds[0]->text3 == "GO" && sounds[0]->text2 == "OVER" && sounds[0]->text == "menu.lwf");
	// The click through the frame's own pump: SELECTED, then the SCREEN row; the next screen configured.
	run.events.clear();
	run.click(150, 120);
	sounds = run.of(MenuEvent::Kind::Sound);
	CHECK(!sounds.empty() && sounds[0]->value == kSoundSelected && sounds[0]->text2 == "CLICK");
	CHECK(run.rt.current_screen() == "SECOND" && run.frame.configures() == 2);
	// Esc reaches BACK's hotkey: its POP_SCREEN goes back.
	CHECK(run.rt.handle_key(vk(27)));
	CHECK(run.rt.current_screen() == "STARTUP");
	// A check box toggles on its click.
	run.click(150, 220);
	CHECK(run.rt.is_widget_checked(run.rt.widget_id("CHECK")));
	// An edit takes the focus on its press, and the characters typed land in the frame's state.
	run.click(150, 320);
	const int name = run.rt.widget_id("NAME");
	CHECK(run.rt.focused_widget() == name);
	run.rt.handle_key(vk('A', 'A'));
	run.rt.handle_key(vk('B', 'b'));
	CHECK(frame_widget_text(run.frame.compiler(), run.frame.state(), run.rt.frame_index(name)) == "Ab");
	CHECK(run.rt.get_widget_text(name) == "Ab");
	// A service verb's row is raised for the embedder.
	run.events.clear();
	run.click(150, 420);
	const std::vector<const MenuEvent *> service = run.of(MenuEvent::Kind::ServiceRequested);
	CHECK(service.size() == 1 && service[0]->text == "LAN_SEARCH" && service[0]->text2 == "ANY" &&
	      service[0]->id == run.rt.widget_id("LAN") && service[0]->value == 14);
}

void test_frame_state_writes() {
	MenuFrameState state;
	frame_set_scroll_range(state, 3, 10, 5, 2, 7);
	const MenuWidgetState *row = find_frame_widget(state, 3);
	CHECK(row && row->has_scroll_range && row->scroll_min == 0 && row->scroll_max == 0 && row->scroll_value == 0);
	frame_set_scroll_range(state, 3, 0, 255, 10, 300);
	CHECK(row->scroll_value == 255 && state.widgets.size() == 1);
	CHECK(frame_set_hover_item(state, 3, 2) && !frame_set_hover_item(state, 3, 2));
	frame_set_shown(state, 4, false);
	CHECK(find_frame_widget(state, 4)->hide && !find_frame_widget(state, 4)->show);
	CHECK(find_frame_widget(state, 9) == nullptr && frame_widget_caret(state, 9) == -1);
}

// Each window holds a child element: retail creates no WINDOW with none.
const char *kPlayScreen = R"(<SCREEN>
	<NAME>SINGLE_PLAYER</NAME>
	<WINDOW type="window" name="MAIN">
		<WINDOW type="list" name="IA_LIST"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
		<WINDOW type="button" name="ACCEPT"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
		<WINDOW type="button" name="EXIT"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
		<WINDOW type="button" name="HIDDEN_BACK"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
		<WINDOW type="button" name="CONFIRM_YES"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
	</WINDOW>
</SCREEN>
)";

const char *kIngame = R"(<SCREEN>
	<NAME>INGAME</NAME>
	<WINDOW type="window" name="MAIN">
		<WINDOW type="button" name="HIDDEN_BACK"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
		<WINDOW type="button" name="CONFIRM_YES"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
		<WINDOW type="button" name="RESTART"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
	</WINDOW>
</SCREEN>
)";

const char *kLan = R"(<SCREEN>
	<NAME>LAN</NAME>
	<WINDOW type="window" name="MAIN">
		<WINDOW type="list" name="LAN_GAME_LIST"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
		<WINDOW type="button" name="START_GAME"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
		<WINDOW type="button" name="EXIT"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
	</WINDOW>
</SCREEN>
)";

void test_commands() {
	{
		Run run;
		CHECK(run.open(kPlayScreen, "sp.mnu"));
		MenuCommands commands;
		commands.wire(run.rt, false);
		// The single-player screen's ACCEPT starts its mission; the list is one the game fills.
		CHECK(commands.command_of("SINGLE_PLAYER", "accept") == MenuCommand::StartMission);
		CHECK(commands.mission_lists().size() == 1 && commands.mission_lists()[0] == run.rt.widget_id("IA_LIST"));
		CHECK(commands.command_of("SINGLE_PLAYER", "EXIT") == MenuCommand::Exit);
		CHECK(commands.command_of("SINGLE_PLAYER", "HIDDEN_BACK") == MenuCommand::Back);
		// CONFIRM_YES leaves a mission only: the front end binds nothing to it.
		CHECK(commands.command_of("SINGLE_PLAYER", "CONFIRM_YES") == MenuCommand::None);
		CHECK(commands.owner().empty());
	}
	{
		Run run;
		CHECK(run.open(kIngame, "game.mnu"));
		MenuCommands commands;
		commands.wire(run.rt, true);
		CHECK(commands.command_of("INGAME", "CONFIRM_YES") == MenuCommand::ReturnToMenu);
		CHECK(commands.command_of("INGAME", "RESTART") == MenuCommand::Restart);
		CHECK(commands.command_of("INGAME", "HIDDEN_BACK") == MenuCommand::Back);
		// No mission list: no launch.
		CHECK(commands.command_of("INGAME", "ACCEPT") == MenuCommand::None);
	}
	{
		Run run;
		CHECK(run.open(kLan, "mp.mnu"));
		MenuCommands commands;
		commands.wire(run.rt, false);
		// A delegate owns the LAN screens and binds its own alone.
		CHECK(commands.owner() == "lan");
		CHECK(commands.command_of("LAN", "START_GAME") == MenuCommand::HostGame);
		CHECK(commands.command_of("LAN", "EXIT") == MenuCommand::None);
	}
	CHECK(is_mission_menu_file("GAME.MNU") && is_mission_menu_file("death.mnu") && !is_mission_menu_file("main.mnu") &&
	      !is_mission_menu_file("mp.mnu"));
	MenuNameSet set = MenuNameSet::kCount;
	CHECK(menu_name_set_from_token("start", set) && set == MenuNameSet::Start &&
	      menu_name_set(set).front() == "START_GAME" && std::string(menu_name_set_token(set)) == "start");
	CHECK(!menu_name_set_from_token("nope", set) && menu_name_set(MenuNameSet::kCount).empty());
	CHECK(std::string(menu_command_token(MenuCommand::Exit)) == "exit" &&
	      std::string(menu_command_words(MenuCommand::Exit)) == "quit to the desktop");
}

const char *kOptions = R"(<SCREEN>
	<NAME>OPTIONS</NAME>
	<WINDOW type="window" name="MAIN">
		<WINDOW type="scroll" name="SOUNDFXVOLUME"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
		<WINDOW type="spinlist" name="TERRAINPOLY">
			<ITEMS>
				<ITEM value="0">LOW</ITEM>
				<ITEM value="3">HIGH</ITEM>
			</ITEMS>
		</WINDOW>
		<WINDOW type="checkbox" name="OPTIONS_AUTORELOAD"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
		<WINDOW type="button" name="DIFFICULTY"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
	</WINDOW>
</SCREEN>
)";

void test_options_policy() {
	Run run;
	CHECK(run.open(kOptions, "options.mnu"));
	OptionsScreen options;
	const controls::BindingSet bindings;
	// Off the surface, nothing.
	options.apply_policy(run.rt);
	CHECK(!run.rt.is_widget_disabled(run.rt.widget_id("DIFFICULTY")));
	options.prepare(run.rt, bindings);
	CHECK(options.is_surface());
	options.apply_policy(run.rt);
	MenuScrollRangeState range;
	CHECK(run.rt.get_widget_scroll_range(run.rt.widget_id("SOUNDFXVOLUME"), range) && range.minimum == 0 &&
	      range.maximum == 255 && range.page == 10 && range.value == 0);
	const int poly = run.rt.widget_id("TERRAINPOLY");
	CHECK(run.rt.selected_row(poly) == 1 && run.rt.is_widget_disabled(poly));
	CHECK(run.rt.is_widget_checked(run.rt.widget_id("OPTIONS_AUTORELOAD")));
	CHECK(run.rt.is_widget_disabled(run.rt.widget_id("DIFFICULTY")));
}

} // namespace

int main() {
	test_runtime_over_the_state_frame();
	test_frame_state_writes();
	test_commands();
	test_options_policy();
	if (failures) {
		std::printf("menu_state_frame: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("menu_state_frame OK\n");
	return 0;
}
