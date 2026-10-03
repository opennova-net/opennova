// The animation workflow's windows over a null ImGui backend (ADR 0046 S17, the animation lane), a
// real session under them: a map's row in the Preview plays its clip and says so (the slot in
// words, the clip, the item that pairs the map with its model), the timeline in frames and seconds
// with its events' legend; Space runs and holds the clock, Right steps a frame and Home goes back to
// the first, the speed is a choice, and the track scrubbed with the mouse holds the clip where it
// is let go; the Inspector says what the game does with the row and who plays the map; the clip's
// own Inspector its length and the rows that play it, its bones and frame events numbered from 0 as
// the game counts them; the clip's outline shows no tool that cannot apply, a map's row its tools.
#include <cctype>
#include <string>

#include <editor/documents/animation_map_document.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/model_viewport.h>

#include "editor_ui_test_support.h"

namespace editor_ui_test {

namespace {

std::string lowered(std::string text) {
	for (char &c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
	return text;
}

// The workspace over a session: each frame's requests served as the Shell serves them, the devices
// following the viewports.
struct ClipRun {
	ProjectSession &session;
	DrawnDevices &devices;
	Ui &ui;
	void pump() {
		EditorRequest request;
		while (ui.windows.take_request(request)) session.handle(request);
		devices.sync(session.viewports(), session.view());
	}
	void settle(int frames = 3) {
		for (int i = 0; i < frames; ++i) {
			pump();
			ui.frames(1);
		}
		pump();
	}
	void key(ImGuiKey key) {
		ui.key(key, true);
		ui.key(key, false);
		settle(1);
	}
	const PreviewClock &clock() const { return session.viewports().clock(); }
	const ModelViewport *model(const std::string &path) const {
		return static_cast<const ModelViewport *>(session.viewports().find(path, ViewportKind::Model));
	}
};

void test_timeline() {
	editor_test::TempProjectDir dir("opennova_editor_ui_clip_timeline");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	CHECK(preview_project(session, dir), "the preview project");
	const SessionView &v = session.view();
	DrawnDevices devices;
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_devices(&devices.cache);
	ClipRun run{session, devices, ui};
	const std::string path = "anims/SKIN.adm";
	session.handle(request::open_document(path));
	Document *table = session.document_for(path);
	NodeAddress walk;
	CHECK(table && find_definition(AssetGraph(), *table, "anim_walk_forward", walk), "the walk row");
	if (!table) return;
	session.handle(request::select_record(path, walk));
	session.handle(request::set_viewport(path, R"({"clock": {"playing": false, "ticks": 0}})"));
	run.settle();
	const ModelViewport *model = run.model(path);
	CHECK(model && model->clip_key() == "anim_walk_forward", "the walk plays");
	if (!model) return;
	ui.away();
	std::string text = lowered(logged_frame(ui));
	CHECK(text.find("playing walk forward: walk") != std::string::npos, "what plays: the slot in words and its clip");
	CHECK(text.find("paired by skinned thing (items.def)") != std::string::npos, "the item pairing the map with its model");
	CHECK(text.find("frame 0 of 4") != std::string::npos && text.find("0.00 s of ") != std::string::npos,
	      "the frame and the time");
	CHECK(text.find("l r footstep") != std::string::npos, "the events' legend");
	// The Inspector: what the game does with the row, who plays the map.
	CHECK(text.find("walk forward (slot 1): walking forward.") != std::string::npos, "the row's meaning");
	CHECK(text.find("played by (1):") != std::string::npos && text.find("skinned thing on skinned.3di") != std::string::npos,
	      "the map's player and its model");

	// Space runs and holds the clock; Right steps a frame, Home goes back to the first.
	ui.focus("Preview");
	run.key(ImGuiKey_Space);
	CHECK(run.clock().playing(), "Space runs it");
	run.key(ImGuiKey_Space);
	CHECK(!run.clock().playing(), "Space holds it");
	run.key(ImGuiKey_RightArrow);
	CHECK(run.clock().ticks() == model->tick_of_frame(1) && !run.clock().playing(), "Right: a frame on, held");
	run.key(ImGuiKey_RightArrow);
	CHECK(run.clock().ticks() == model->tick_of_frame(2), "Right again: the next frame");
	run.key(ImGuiKey_LeftArrow);
	CHECK(run.clock().ticks() == model->tick_of_frame(1), "Left: a frame back");
	run.key(ImGuiKey_Home);
	CHECK(run.clock().ticks() == 0, "Home: the first frame");

	// The speed: half.
	const ImGuiID scope = item_id(Ui::window_id("Preview"), {"model", path.c_str()});
	ui.activate(item_id(scope, {"Speed"}));
	ui.activate(item_id(pushed(ImHashStr("##Combo_00"), 2), {"0.5x"}));
	run.settle(1);
	CHECK(run.clock().rate() == 0.5, "the speed chosen");

	// The track scrubbed with the mouse to its right end and past it: the clip's last tick, held.
	ui.mouse(5000.0f, 10.0f);
	ui.activate(item_id(scope, {"##track"}));
	run.settle(1);
	CHECK(run.clock().ticks() == model->clip_length_ticks() && !run.clock().playing(), "scrubbed to the end, held");
	ui.mouse(-5000.0f, 10.0f);
	ui.activate(item_id(scope, {"##track"}));
	run.settle(1);
	CHECK(run.clock().ticks() == 0, "scrubbed to the start");

	// The clip's own document: its length and the rows that play it.
	session.handle(request::open_document("anims/walk.bad"));
	Document *clip = session.document_for("anims/walk.bad");
	CHECK(clip && !clip->rows().empty(), "the clip open");
	if (!clip || clip->rows().empty()) return;
	session.handle(request::select_record(clip->path(), {clip->rows().front()->id, clip->rows().front()->kind, 0}));
	run.settle();
	ui.away();
	text = lowered(logged_frame(ui));
	CHECK(text.find("4 frames at 30 a second:") != std::string::npos && text.find("it loops.") != std::string::npos,
	      "the clip's length in frames and seconds");
	CHECK(text.find("played by these map rows (1):") != std::string::npos && text.find("anims/skin.adm: walk forward") != std::string::npos,
	      "the rows that play it");
	// One numbering, the game's: the Inspector's bones and frame events from 0, as the events' titles
	// and the timeline count frames (the fixture's clip: 3 bones, 5 frame events).
	CHECK(in_order(text, {"### bones (3) ###", "|   0 | { bn01 pelvis }", "|   2 | { bn03 leg }", "### frame events (5) ###",
	                      "|   0 | { 0 }", "|   4 | { 0 }"}) &&
	              text.find("|   5 |") == std::string::npos,
	      "the bones and the frame events numbered from 0");
	// The clip's one row and its fixed lists: no outline tool can apply, so none shows.
	CHECK(text.find("[ duplicate ]") == std::string::npos && text.find("[ remove ]") == std::string::npos &&
	              text.find("[ up ]") == std::string::npos && text.find("[ down ]") == std::string::npos,
	      "no locked Duplicate, Remove, Up or Down");
	session.handle(request::select_record(clip->path(), {}));
	run.settle();
	ui.away();
	text = lowered(logged_frame(ui));
	CHECK(text.find("[ duplicate ]") == std::string::npos && text.find("[ remove ]") == std::string::npos,
	      "none with nothing selected either: nothing in a clip moves");

	// A map's rows can be duplicated and removed: its outline keeps them.
	session.handle(request::open_document(path));
	session.handle(request::select_record(path, walk));
	run.settle();
	ui.away();
	text = lowered(logged_frame(ui));
	CHECK(text.find("[ duplicate ]") != std::string::npos && text.find("[ remove ]") != std::string::npos,
	      "a map's row: its tools");
	ui.drain();
}

} // namespace

void run_animation_tests() { test_timeline(); }

} // namespace editor_ui_test
