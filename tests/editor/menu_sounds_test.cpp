// A menu's sounds heard in its preview (DI-34; editor/preview/menu_sounds, MenuViewport::fire_sounds, the
// session's fire_menu_sounds): the game's mouse over the menu's picture, held where a client (or the canvas)
// holds it, runs the game's pump of the windows' sounds (menu::MenuSoundPump, the runtime's own). Headless (no
// device follows the viewport: a SetViewport's sample follows it), over a menu written here, a bank minted
// through lwf::encode_lwf and a short wave.
//
// Pinned: MOUSEIN as the mouse comes onto a window (the last row of the state, at the menu's master volume, the
// row's own bank alone), nothing more while it stays or while the button is down, SELECTED on the click and
// MOUSEIN again on the next sample there (the Shell's next frame), MOUSEOUT as it leaves; a press begun on a
// window that takes no capture and let go over a button clicks it (the game's click rule, menu_click.h); a
// disabled window and
// a window with no row play nothing; a bank the project lacks (no_bank) and a set the bank lacks (missing);
// Mute (fired and listed, nothing handed to the Shell); the canvas's mouse over the picture the game's while it
// is there, over a pointer a client holds; the envelope's options and sounds_fired; what the Shell
// is handed (clip_sounds_since, the clip sounds' order); the hit's sounds; the inspector's lines
// (menu_window_sounds, menu_image_window).
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/mnu_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/menu_sounds.h>
#include <editor/preview/menu_viewport.h>
#include <editor/preview/viewport_json.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/lwf/lwf.h>
#include <formats/mnu/mnu.h>
#include <runtime/menu/menu_sound.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

std::string repo() { return test_paths_repo_root(__FILE__); }

// A menu bank of one set per name, each one layer (no falloff, as every shipped menu layer) playing its one
// wave (the name lower case), the member's volume 200.
std::vector<uint8_t> menu_bank(const std::vector<std::string> &sets) {
	lwf::File bank;
	for (size_t i = 0; i < sets.size(); ++i) {
		lwf::Single single;
		single.name = sets[i];
		single.path = "SFX\\MENU\\" + strutil::to_lower(sets[i]) + ".wav";
		single.value_hi = 0xD200;
		bank.singles.push_back(single);
		lwf::Multi set;
		set.name = sets[i];
		set.pitch_base = lwf::kAuthoredSetPitchBase;
		set.playlist_indices.push_back(uint32_t(i));
		bank.multis.push_back(set);
		lwf::Playlist layer;
		layer.flags = lwf::kFlagInternal | lwf::kFlagExternal;
		layer.sndparm_indices.push_back(uint32_t(i));
		bank.playlists.push_back(layer);
		lwf::Sndparm member;
		member.single_index = uint32_t(i);
		member.pitch_scaled = lwf::kPitchUnityQ16;
		member.volume = 200;
		member.clamp_volume = 255;
		bank.sndparms.push_back(member);
	}
	std::vector<uint8_t> out;
	std::string error;
	lwf::encode_lwf(bank, out, error);
	return out;
}

// STARTUP: MAIN (the whole screen, no sound) holding BTN (two MOUSEIN rows, the last OVER; SELECTED CLICK;
// MOUSEOUT OUT), OFF (disabled, MOUSEIN OVER), LOST (MOUSEIN from a bank the project lacks), ODD (MOUSEIN a
// set the bank lacks) and QUIET (no SOUND).
const char *kMenu = R"(<SCREEN>
	<NAME>STARTUP</NAME>
	<WINDOW type="window" name="MAIN">
		<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
		<WINDOW type="button" name="BTN">
			<POSITION><LEFT>100</LEFT><TOP>100</TOP><RIGHT>300</RIGHT><BOTTOM>140</BOTTOM></POSITION>
			<SOUND state="mousein" trigger="FIRST">menu.lwf</SOUND>
			<SOUND state="mousein" trigger="OVER">menu.lwf</SOUND>
			<SOUND state="selected" trigger="CLICK">menu.lwf</SOUND>
			<SOUND state="mouseout" trigger="OUT">menu.lwf</SOUND>
		</WINDOW>
		<WINDOW type="button" name="OFF" disable="1">
			<POSITION><LEFT>100</LEFT><TOP>200</TOP><RIGHT>300</RIGHT><BOTTOM>240</BOTTOM></POSITION>
			<SOUND state="mousein" trigger="OVER">menu.lwf</SOUND>
		</WINDOW>
		<WINDOW type="button" name="LOST">
			<POSITION><LEFT>100</LEFT><TOP>300</TOP><RIGHT>300</RIGHT><BOTTOM>340</BOTTOM></POSITION>
			<SOUND state="mousein" trigger="OVER">nowhere.lwf</SOUND>
		</WINDOW>
		<WINDOW type="button" name="ODD">
			<POSITION><LEFT>100</LEFT><TOP>400</TOP><RIGHT>300</RIGHT><BOTTOM>440</BOTTOM></POSITION>
			<SOUND state="mousein" trigger="NO_SUCH_SET">menu.lwf</SOUND>
		</WINDOW>
		<WINDOW type="button" name="QUIET">
			<POSITION><LEFT>400</LEFT><TOP>100</TOP><RIGHT>600</RIGHT><BOTTOM>140</BOTTOM></POSITION>
		</WINDOW>
	</WINDOW>
</SCREEN>
)";

struct MenuProject {
	editor_test::TempProjectDir dir{"opennova_editor_menu_sounds"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string root;
	std::string path;
	bool made = false;

	MenuProject() {
		session.handle(request::new_project(dir.file("project"), "Menu Sounds"));
		session.run_operations();
		editor_test::create_missing_files(session);
		root = session.view().project.root;
		const AssetEntry *menu = session.view().project.scan->named("main.mnu");
		path = menu ? menu->relative_path : std::string();
		bool ok = menu && editor_test::write_text(root + "/" + path, kMenu);
		ok = ok && editor_test::write_bytes(root + "/sounds/menu.lwf", menu_bank({"FIRST", "OVER", "CLICK", "OUT"}));
		const std::vector<uint8_t> tone = test_io::read_file(repo() + "/fixtures/lwf/tone.wav");
		for (const char *wave : {"first", "over", "click", "out"})
			ok = ok && editor_test::write_bytes(root + "/sounds/" + wave + ".wav", tone);
		editor_test::handle_to_end(session, request::rescan());
		while (session.view().activity.validation.running) session.poll();
		session.handle(request::open_document(path));
		made = ok && session.view().documents.previews[ViewportKind::Menu].path == path;
	}
	const MenuViewport *viewport() {
		return dynamic_cast<const MenuViewport *>(session.viewports().find(path, ViewportKind::Menu));
	}
	bool set(const std::string &options) {
		session.handle(request::set_viewport(path, R"({"options": )" + options + "}"));
		return session.outcome().done();
	}
	// The game's mouse at (x, y) with its button, as the canvas or a client holds it.
	bool mouse(float x, float y, bool down = false) {
		char text[96];
		std::snprintf(text, sizeof(text), R"({"pointer_at": [%g, %g], "pointer_down": %s})", x, y, down ? "true" : "false");
		return set(text);
	}
	// The sounds fired after `seq`, spelled "BTN:MOUSEIN:OVER:played".
	std::vector<std::string> fired_after(uint64_t seq) {
		std::vector<std::string> out;
		if (const MenuViewport *menu = viewport())
			for (const MenuSoundFired &fired : menu->sounds_fired())
				if (fired.sound.seq > seq)
					out.push_back(fired.name + ":" + menu::menu_sound_state_token(fired.state) + ":" + fired.sound.set + ":" +
					              fired.sound.state);
		return out;
	}
	uint64_t seq() { return session.viewports().clip_sound_seq(); }
	JsonValue json() {
		ViewportModel *model = session.viewports().follow_one(session.view(), path, ViewportKind::Menu);
		return model ? viewport_to_json(session.view(), *model, JsonPage()) : JsonValue();
	}
};

using Lines = std::vector<std::string>;

int test_hover_click_leave() {
	MenuProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	// Onto BTN: its MOUSEIN, the last row of the state, from its own bank at the menu's master volume.
	uint64_t before = project.seq();
	TEST_EXPECT(project.mouse(200, 120));
	TEST_EXPECT(project.fired_after(before) == Lines({"BTN:MOUSEIN:OVER:played"}));
	const MenuSoundFired &over = project.viewport()->sounds_fired().back();
	// The member's volume is 200; a layer with no falloff plays at the menu's master volume, 255.
	TEST_EXPECT(over.sound.bank == "menu.lwf" && over.screen == "STARTUP" && over.window != 0 &&
	            over.sound.voices.size() == 1 && over.sound.voices[0].path == "sounds/over.wav" &&
	            over.sound.voices[0].volume == 255 &&
	            over.sound.words == "OVER in menu.lwf: over.wav at pitch 1.00, volume 255.");
	// Staying, a step within it, and the frames passing: nothing more.
	before = project.seq();
	TEST_EXPECT(project.mouse(210, 125));
	project.session.advance(1.0 / 60.0);
	TEST_EXPECT(project.fired_after(before).empty());
	// Pressed: nothing; let go over it, the click's SELECTED, and the next frame with the mouse still there
	// MOUSEIN again.
	TEST_EXPECT(project.mouse(210, 125, true));
	TEST_EXPECT(project.fired_after(before).empty());
	TEST_EXPECT(project.mouse(210, 125, false));
	TEST_EXPECT(project.fired_after(before) == Lines({"BTN:SELECTED:CLICK:played"}));
	project.session.advance(1.0 / 60.0);
	TEST_EXPECT(project.fired_after(before) == Lines({"BTN:SELECTED:CLICK:played", "BTN:MOUSEIN:OVER:played"}));
	// Off it onto MAIN, which has no row: BTN's MOUSEOUT alone.
	before = project.seq();
	TEST_EXPECT(project.mouse(20, 20));
	TEST_EXPECT(project.fired_after(before) == Lines({"BTN:MOUSEOUT:OUT:played"}));
	// A press begun on MAIN (whose press takes no capture) and let go over BTN clicks BTN, held under the
	// mouse with the button down the sample before (the game's rule, menu_click.h, D-MNU-30); the next frame
	// with the mouse still there, its MOUSEIN.
	before = project.seq();
	TEST_EXPECT(project.mouse(20, 20, true));
	TEST_EXPECT(project.mouse(200, 120, true));
	TEST_EXPECT(project.mouse(200, 120, false));
	TEST_EXPECT(project.fired_after(before) == Lines({"BTN:SELECTED:CLICK:played"}));
	project.session.advance(1.0 / 60.0);
	TEST_EXPECT(project.fired_after(before) == Lines({"BTN:SELECTED:CLICK:played", "BTN:MOUSEIN:OVER:played"}));
	// Pressed on BTN (its press captures), left with the button held and let go off it: no click, no
	// MOUSEOUT, nor MOUSEIN as the mouse comes back up.
	before = project.seq();
	TEST_EXPECT(project.mouse(200, 120, true) && project.mouse(20, 20, true) && project.mouse(20, 20, false) &&
	            project.mouse(200, 120, false));
	TEST_EXPECT(project.fired_after(before).empty());
	// Let go of (pointer_at null): the mouse off the picture, BTN's MOUSEOUT.
	TEST_EXPECT(project.set(R"({"pointer_at": null})"));
	TEST_EXPECT(project.fired_after(before) == Lines({"BTN:MOUSEOUT:OUT:played"}));

	// A disabled window and a window with no SOUND play nothing; a bank the project lacks and a set its bank
	// lacks are fired and say why.
	before = project.seq();
	TEST_EXPECT(project.mouse(200, 220) && project.mouse(500, 120));
	TEST_EXPECT(project.fired_after(before).empty());
	TEST_EXPECT(project.mouse(200, 320) && project.mouse(200, 420));
	TEST_EXPECT(project.fired_after(before) == Lines({"LOST:MOUSEIN:OVER:no_bank", "ODD:MOUSEIN:NO_SUCH_SET:missing"}));
	const MenuSoundFired &lost = project.viewport()->sounds_fired()[project.viewport()->sounds_fired().size() - 2];
	TEST_EXPECT(lost.sound.words == "OVER: the project has no sound bank nowhere.lwf the game reads, so the game plays nothing.");

	// The Shell is handed what played, in the clip sounds' order, and nothing muted.
	const std::vector<ClipSoundPlay> plays = project.session.clip_sounds_since(0);
	TEST_EXPECT(plays.size() == 7 && plays[0].voices.size() == 1 && plays[0].voices[0].path == "sounds/over.wav" &&
	            plays[1].voices[0].path == "sounds/click.wav" && plays[0].seq < plays[1].seq);
	before = project.seq();
	TEST_EXPECT(project.set(R"({"sound": {"mute": true}})") && project.mouse(200, 120));
	TEST_EXPECT(project.fired_after(before) == Lines({"BTN:MOUSEIN:OVER:muted"}));
	TEST_EXPECT(project.session.clip_sounds_since(before).empty());
	TEST_EXPECT(!project.set(R"({"sound": {"mute": 1}})") && !project.set(R"({"sound": {"loud": true}})") &&
	            !project.set(R"({"pointer_down": "yes"})"));

	// The envelope: the options' sound, pointer_down, and the last sounds fired.
	JsonValue shown = project.json();
	const JsonValue *options = shown.get("options");
	TEST_EXPECT(options && options->get("sound") && options->get("sound")->get_bool("mute", false) &&
	            !options->get_bool("pointer_down", true) && options->get("pointer_at")->array.size() == 2);
	const JsonValue *fired = shown.get("body") ? shown.get("body")->get("sounds_fired") : nullptr;
	TEST_EXPECT(fired && fired->array.size() == 10 && fired->array.back().get_string("sound", "") == "MOUSEIN" &&
	            fired->array.back().get_string("name", "") == "BTN" &&
	            fired->array.back().get_string("set", "") == "OVER" &&
	            fired->array.back().get_string("bank", "") == "menu.lwf" &&
	            fired->array.back().get_string("state", "") == "muted" &&
	            fired->array.back().get("voices")->array.size() == 1);
	return 0;
}

// The canvas's mouse over the picture is the game's while it is there (ProjectSession::canvas_mice, the Shell's
// frames), over a pointer a client holds; the frames' samples play what it does; let go of, the held pointer is
// the game's mouse again.
int test_canvas_mouse() {
	MenuProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	TEST_EXPECT(project.mouse(500, 120)); // a client holds it over QUIET: nothing
	const uint64_t before = project.seq();
	ViewportMouse mouse;
	mouse.path = project.path;
	mouse.kind = ViewportKind::Menu;
	mouse.over = true;
	mouse.x = 200;
	mouse.y = 120;
	const auto frame = [&](const std::vector<ViewportMouse> &mice) {
		project.session.canvas_mice(mice);
		project.session.advance(1.0 / 60.0);
	};
	frame({mouse});
	TEST_EXPECT(project.fired_after(before) == Lines({"BTN:MOUSEIN:OVER:played"}));
	mouse.down = true;
	frame({mouse});
	mouse.down = false;
	frame({mouse});
	frame({mouse});
	TEST_EXPECT(project.fired_after(before) ==
	            Lines({"BTN:MOUSEIN:OVER:played", "BTN:SELECTED:CLICK:played", "BTN:MOUSEIN:OVER:played"}));
	// Off the picture (or a pan, a drag): the client's held pointer over QUIET is the game's mouse again.
	mouse.over = false;
	frame({mouse});
	frame({});
	TEST_EXPECT(project.fired_after(before) == Lines({"BTN:MOUSEIN:OVER:played", "BTN:SELECTED:CLICK:played",
	                                                  "BTN:MOUSEIN:OVER:played", "BTN:MOUSEOUT:OUT:played"}));
	return 0;
}

int test_hit_and_inspector() {
	MenuProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	project.json();
	const MenuViewport *menu = project.viewport();
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	// The hit says what the window plays and when, in the pump's order.
	const ViewportHit hit = menu->hit(viewport_context(project.session.view(), *menu, 0.0f), 200, 120);
	TEST_EXPECT(hit.name == "BTN" && hit.sounds.is_array() && hit.sounds.array.size() == 3);
	if (hit.sounds.array.size() == 3) {
		TEST_EXPECT(hit.sounds.array[0].get_string("sound", "") == "MOUSEIN" &&
		            hit.sounds.array[0].get_string("when", "") == "on hover" &&
		            hit.sounds.array[0].get_string("set", "") == "OVER" &&
		            hit.sounds.array[0].get_string("bank", "") == "menu.lwf");
		TEST_EXPECT(hit.sounds.array[1].get_string("sound", "") == "SELECTED" &&
		            hit.sounds.array[2].get_string("sound", "") == "MOUSEOUT");
	}
	const ViewportHit quiet = menu->hit(viewport_context(project.session.view(), *menu, 0.0f), 500, 120);
	TEST_EXPECT(quiet.name == "QUIET" && quiet.sounds.is_array() && quiet.sounds.array.empty());
	// The inspector's window, read as the game reads the menu.
	const auto *document = dynamic_cast<const MnuDocument *>(project.session.document_for(project.path));
	NodeAddress btn;
	TEST_EXPECT(document && find_definition(AssetGraph(), *document, "BTN", btn));
	if (!document) return 1;
	std::shared_ptr<const mnu::Document> image;
	const mnu::Window *read = menu_image_window(*document, btn, image);
	TEST_EXPECT(read && read->name == "BTN" && image);
	if (read) {
		const std::vector<MenuWindowSound> sounds = menu_window_sounds(*read);
		TEST_EXPECT(sounds.size() == 3 && sounds[0].state == menu::kSoundMouseIn && sounds[0].set == "OVER" &&
		            std::string(menu_sound_state_words(sounds[1].state)) == "on click");
	}
	return 0;
}

} // namespace

int main() {
	TEST_EXPECT(test_hover_click_leave() == 0);
	TEST_EXPECT(test_canvas_mouse() == 0);
	TEST_EXPECT(test_hit_and_inspector() == 0);
	std::printf("editor_menu_sounds OK\n");
	return 0;
}
