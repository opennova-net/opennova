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
#include <runtime/menu/mission_screen.h>
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
		frame.set_clicked([this](int index, int part) { rt.on_widget_clicked(index, part); });
		frame.set_scrolled([this](int index, int value) { rt.on_frame_scroll_value(index, value); });
		rt.set_frame(&frame);
		rt.set_sink([this](const MenuEvent &e) { events.push_back(e); });
		return rt.open_document(&doc, file, std::string());
	}
	void click(float x, float y) {
		rt.process_mouse(x, y, false);
		rt.process_mouse(x, y, true);
		rt.process_mouse(x, y, false);
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
	run.rt.process_mouse(150, 120, false);
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

// A code-side EDIT set clamps the caret to the label it stored; a MULTILINE_EDIT set goes back to
// its first line [orig: CEditWnd_SetText @ 0x661900 - @ 0x661917..0x661933; CMEditWnd_SetText
// @ 0x6612d0 -> CMEditWnd_UpdateScrollRange - +3912 = 0 @ 0x66121e, CScrollWnd_SetScrollPos(0)
// @ 0x661228].
const char *kEdits = R"(<SCREEN>
	<NAME>EDITS</NAME>
	<WINDOW type="window" name="MAIN">
		<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
		<WINDOW type="edit" name="NAME">
			<POSITION><LEFT>100</LEFT><TOP>100</TOP><RIGHT>300</RIGHT><BOTTOM>140</BOTTOM></POSITION>
		</WINDOW>
		<WINDOW type="multiline_edit" name="DESC">
			<POSITION><LEFT>100</LEFT><TOP>200</TOP><RIGHT>300</RIGHT><BOTTOM>400</BOTTOM></POSITION>
		</WINDOW>
	</WINDOW>
</SCREEN>
)";

void test_edit_sets_clamp_the_caret_and_reset_the_first_line() {
	Run run;
	CHECK(run.open(kEdits, "edits.mnu"));
	const int name = run.rt.widget_id("NAME");
	run.click(150, 120);
	CHECK(run.rt.focused_widget() == name);
	for (const int c : { 'A', 'B', 'C', 'D', 'E' }) run.rt.handle_key(vk(c, c));
	CHECK(run.row("NAME") && run.row("NAME")->caret == 5);
	// A shorter label pulls the caret to its end; a longer one leaves it.
	run.rt.set_widget_text(name, "AB");
	CHECK(run.row("NAME")->caret == 2);
	run.rt.set_widget_text(name, "ABCDEFG");
	CHECK(run.row("NAME")->caret == 2);
	// The {hot} cut counts: the label is "XY", two bytes.
	run.rt.handle_key(vk(35)); // End
	CHECK(run.row("NAME")->caret == 7);
	run.rt.set_widget_text(name, "X{hot}Y");
	CHECK(run.row("NAME")->caret == 2);
	// The multiline edit scrolled down goes back to its first line at a set.
	const int desc = run.rt.widget_id("DESC");
	run.rt.set_scroll_row(desc, 3);
	CHECK(run.row("DESC") && run.row("DESC")->scroll_row == 3);
	run.rt.set_widget_text(desc, "one two three four five six seven eight");
	CHECK(run.row("DESC")->scroll_row == 0);
}

// A focused edit's caret starts at 0, a byte index (D-MNU-59): the edit's construction zeroes it and
// only its keys, its typing, a clear and the sets' clamps move it, so a focused edit holding text
// takes a typed character at its start, and End puts the caret past the text's last byte, a
// two-byte character counted twice [orig: CEditWnd_Construct @ 0x661422; CEditWnd_InsertChar
// @ 0x661ee0; CEditWnd_HandleKeyEvent @ 0x6623a0].
void test_a_focused_edit_types_at_its_start() {
	Run run;
	CHECK(run.open(kEdits, "edits.mnu"));
	const int name = run.rt.widget_id("NAME");
	run.rt.set_widget_text(name, "h\xC3\xA9");
	run.click(150, 120);
	CHECK(run.rt.focused_widget() == name && run.row("NAME") && run.row("NAME")->caret == 0);
	run.rt.handle_key(vk('X', 'X'));
	CHECK(run.rt.get_widget_text(name) == "Xh\xC3\xA9" && run.row("NAME")->caret == 1);
	run.rt.handle_key(vk(35)); // End
	CHECK(run.row("NAME")->caret == 4);
	run.rt.handle_key(vk('Y', 'Y'));
	CHECK(run.rt.get_widget_text(name) == "Xh\xC3\xA9Y" && run.row("NAME")->caret == 5);
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

// dfx2.exe's player.mnu shape: PLAYER_INFO with a one-pixel COMBO_LIST spin list and no NATIONALITY or
// DIVISION; and Jointops.exe's character controls on a screen of another name.
const char *kSpinPlayerInfo = R"(<SCREEN>
	<NAME>player_info</NAME>
	<WINDOW type="window" name="MAIN">
		<WINDOW type="spinlist" name="COMBO_LIST"><POSITION><LEFT>525</LEFT><TOP>0</TOP><RIGHT>526</RIGHT><BOTTOM>20</BOTTOM></POSITION></WINDOW>
		<WINDOW type="button" name="ACCEPT"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
	</WINDOW>
</SCREEN>
)";
const char *kCharacterControlsElsewhere = R"(<SCREEN>
	<NAME>PLAYER</NAME>
	<WINDOW type="window" name="MAIN">
		<WINDOW type="combobox" name="NATIONALITY"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
		<WINDOW type="combobox" name="COMBO_LIST"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
		<WINDOW type="button" name="ACCEPT"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
	</WINDOW>
</SCREEN>
)";

// The PLAYER_INFO screen's controls, which its delegate owns; and a Mods screen (a mod list, no mission list).
const char *kPlayerInfo = R"(<SCREEN>
	<NAME>PLAYER_INFO</NAME>
	<WINDOW type="window" name="MAIN">
		<WINDOW type="combobox" name="NATIONALITY"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
		<WINDOW type="list" name="COMBO_LIST"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
		<WINDOW type="button" name="ACCEPT"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
		<WINDOW type="button" name="EXIT"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
	</WINDOW>
</SCREEN>
)";

const char *kMods = R"(<SCREEN>
	<NAME>MODS</NAME>
	<WINDOW type="window" name="MAIN">
		<WINDOW type="list" name="AVAIL_LIST"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
		<WINDOW type="button" name="ACCEPT"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
		<WINDOW type="button" name="NW_MULTI_PLAYER"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
	</WINDOW>
</SCREEN>
)";

// The updater's buttons: STARTUP's and OPTIONS's UPDATE, front end only, with no availability gate;
// and an APPMSG row's "UPDATE" (without case, untrimmed), on any screen.
// [orig: UI_RegisterStartupCallbacks @ 0x555890 (UPDATE @ 0x5558f2); UI_RegisterOptionsCallbacks
//  @ 0x55d610 (UPDATE @ 0x55d7da); UI_DispatchScreenEvent @ 0x54e6a0 case 10 @ 0x54f3a4..0x54f3c1]
const char *kUpdateScreens = R"(<SCREEN>
	<NAME>STARTUP</NAME>
	<WINDOW type="window" name="MAIN">
		<WINDOW type="button" name="UPDATE"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
	</WINDOW>
</SCREEN>
<SCREEN>
	<NAME>OPTIONS</NAME>
	<WINDOW type="window" name="MAIN">
		<WINDOW type="button" name="UPDATE"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
	</WINDOW>
</SCREEN>
<SCREEN>
	<NAME>OTHER</NAME>
	<WINDOW type="window" name="MAIN">
		<WINDOW type="button" name="UPDATE"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>
	</WINDOW>
</SCREEN>
)";

void test_update_routes() {
	Run run;
	CHECK(run.open(kUpdateScreens, "front.mnu"));
	MenuCommands commands;
	commands.wire(run.rt, false);
	CHECK(commands.command_of("STARTUP", "update") == MenuCommand::Update);
	CHECK(commands.command_of("options", "UPDATE") == MenuCommand::Update);
	CHECK(commands.command_of("OTHER", "UPDATE") == MenuCommand::None);
	commands.wire(run.rt, true);
	CHECK(commands.command_of("STARTUP", "UPDATE") == MenuCommand::None);
	CHECK(app_message_command("update") == MenuCommand::Update);
	CHECK(app_message_command("UPDATE") == MenuCommand::Update);
	CHECK(app_message_command(" UPDATE") == MenuCommand::None);
	CHECK(app_message_command("UPDATER") == MenuCommand::None);
	CHECK(std::string(menu_command_token(MenuCommand::Update)) == "update");
}

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
		CHECK(commands.mission_lists().empty() && commands.mod_lists().empty());
	}
	{
		Run run;
		CHECK(run.open(kPlayerInfo, "player.mnu"));
		MenuCommands commands;
		commands.wire(run.rt, false);
		// PLAYER_INFO's delegate owns the document: its ACCEPT saves the profile, nothing else binds.
		CHECK(commands.owner() == "player_info");
		CHECK(commands.command_of("PLAYER_INFO", "ACCEPT") == MenuCommand::SaveProfile);
		CHECK(commands.command_of("PLAYER_INFO", "EXIT") == MenuCommand::None);
	}
	{
		Run run;
		CHECK(run.open(kMods, "options.mnu"));
		MenuCommands commands;
		commands.wire(run.rt, false);
		// A mod list and no mission list: ACCEPT applies the pick; NW_MULTI_PLAYER opens NovaWorld.
		CHECK(commands.owner().empty());
		CHECK(commands.mod_lists().size() == 1 && commands.mod_lists()[0] == run.rt.widget_id("AVAIL_LIST"));
		CHECK(commands.mission_lists().empty());
		CHECK(commands.command_of("MODS", "ACCEPT") == MenuCommand::ApplyExpansion);
		CHECK(commands.command_of("MODS", "NW_MULTI_PLAYER") == MenuCommand::NovaWorld);
	}
	{
		// The character screen is the screen named PLAYER_INFO, without case, whatever it holds
		// [orig: UI_DispatchScreenEvent @ 0x54e9d4; @ 0x54bc2d (dfx2.exe)]: its delegate binds its
		// ACCEPT alone.
		Run run;
		CHECK(run.open(kSpinPlayerInfo, "player.mnu"));
		CHECK(is_player_info_document(run.rt));
		MenuCommands commands;
		commands.wire(run.rt, false);
		CHECK(commands.owner() == "player_info");
		CHECK(commands.command_of("PLAYER_INFO", "ACCEPT") == MenuCommand::SaveProfile);
	}
	{
		Run run;
		CHECK(run.open(kCharacterControlsElsewhere, "other.mnu"));
		CHECK(!is_player_info_document(run.rt));
		MenuCommands commands;
		commands.wire(run.rt, false);
		CHECK(commands.owner().empty());
		CHECK(commands.command_of("PLAYER", "ACCEPT") == MenuCommand::None);
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
	// Off the surface, nothing.
	options.apply_policy(run.rt);
	CHECK(!run.rt.is_widget_disabled(run.rt.widget_id("DIFFICULTY")));
	options.prepare(run.rt, nullptr, opennova::controls::default_catalog());
	CHECK(options.is_surface());
	options.apply_policy(run.rt);
	MenuScrollRangeState range;
	CHECK(run.rt.get_widget_scroll_range(run.rt.widget_id("SOUNDFXVOLUME"), range) && range.minimum == 0 &&
	      range.maximum == 255 && range.page == 10 && range.value == 0);
	const int poly = run.rt.widget_id("TERRAINPOLY");
	CHECK(run.rt.selected_row(poly) == 1 && run.rt.is_widget_disabled(poly));
	// The auto-reload box is the profile's (OptionsScreen::seed_profile), served.
	CHECK(!run.rt.is_widget_disabled(run.rt.widget_id("OPTIONS_AUTORELOAD")));
	CHECK(run.rt.is_widget_disabled(run.rt.widget_id("DIFFICULTY")));
}

// The main frame's input under a mission's menu screen: none up runs the frame's own; INGAME,
// WEAPON, CMAP and VEHICLE hold it; DEATH and STAT pass it through while the pass-through word
// holds, which no other screen raises
// [orig: UI_ProcessMenuScreenFrame @0x54E650 (@0x54E686); UI_SetMenuInputPassThrough @0x54B980].
void test_mission_screens() {
	CHECK(!mission_screen_open(MissionScreen::None));
	CHECK(!mission_screen_holds_input(MissionScreen::None, false));
	CHECK(!mission_screen_passes_input(MissionScreen::None, true));
	for (const MissionScreen held : {MissionScreen::Ingame, MissionScreen::Weapon,
			     MissionScreen::CommandMap, MissionScreen::Vehicle}) {
		CHECK(mission_screen_open(held));
		CHECK(mission_screen_holds_input(held, false) && mission_screen_holds_input(held, true));
		CHECK(!mission_screen_passes_input(held, true));
	}
	for (const MissionScreen passing : {MissionScreen::Death, MissionScreen::Stat}) {
		CHECK(mission_screen_open(passing));
		CHECK(mission_screen_passes_input(passing, true) && !mission_screen_holds_input(passing, true));
		CHECK(!mission_screen_passes_input(passing, false) && mission_screen_holds_input(passing, false));
	}
}

} // namespace

int main() {
	test_runtime_over_the_state_frame();
	test_frame_state_writes();
	test_edit_sets_clamp_the_caret_and_reset_the_first_line();
	test_a_focused_edit_types_at_its_start();
	test_commands();
	test_mission_screens();
	test_update_routes();
	test_options_policy();
	if (failures) {
		std::printf("menu_state_frame: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("menu_state_frame OK\n");
	return 0;
}
