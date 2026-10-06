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

#include <base/io/strutil.h>
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
	// The pairing is a link (DI-05): a Go to the item's record, its animation map field shown.
	{
		ui.activate(item_id(item_id(Ui::window_id("Preview"), {"model", path.c_str()}), {"###paired"}));
		const std::vector<EditorRequest> went = ui.drain();
		const EditorRequest *pairing = one(went, EditorRequestKind::OpenDocument);
		CHECK(pairing && pairing->path == "defs/items.def" && !pairing->locator.empty() && pairing->field == "anim_def",
		      "paired by: the item's record opened at its animation map");
		ui.away();
	}
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

	// The clip's sounds (DI-04): the Sound popup's Mute is the viewport's sound option, and the button says so.
	ui.activate(item_id(scope, {"Sound###sound"}));
	run.settle(1);
	ui.activate(popup_item(ImHashStr("sound", 0, scope), "Mute"));
	run.settle(1);
	CHECK(model->options().sound.mute, "Mute, the viewport's sound option");
	ui.away();
	CHECK(lowered(logged_frame(ui)).find("sound (muted)") != std::string::npos, "the button says the clip is muted");

	// The track scrubbed with the mouse to its right end and past it: a loop's last tick (its wrap tick
	// shows the first frame again), its last frame shown, held (S17 review).
	ui.mouse(5000.0f, 10.0f);
	ui.activate(item_id(scope, {"##track"}));
	run.settle(1);
	CHECK(model->clip_loops() && run.clock().ticks() == model->clip_length_ticks() - 1 && !run.clock().playing(),
	      "scrubbed to the end, held on the loop's last tick");
	ui.away();
	CHECK(lowered(logged_frame(ui)).find("frame 3 of 4") != std::string::npos, "the last frame shown at the end");
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

// A clip of 60 frames whose frame 30 steps right, on the skinned fixture's three bones.
std::string long_clip() {
	std::string text = "o3a 1\nadm LONG.adm\nrow anim_reset \"longrest\"\nrow anim_idle \"long\"\n";
	const auto clip = [&](const char *name, int frames, int step) {
		text += std::string("clip ") + name + "\nfps 30\nflags 0x1\nframes " + std::to_string(frames) + "\n";
		const char *bones[3] = {"bone -1 0 0 0 0.5 \"BN01 Pelvis\"", "bone 0 0 0 1 0.5 \"BN02 Spine\"",
		                        "bone 0 0 0 -1 0.5 \"BN03 Leg\""};
		for (const char *bone : bones) {
			text += std::string(bone) + "\n";
			for (int k = 0; k <= frames; ++k) text += " k 0 0 0 1\n";
		}
		for (int k = 0; k <= frames; ++k) text += k == step ? "event 0 0 0 0x2 0.9 1.7\n" : "event 0 0 0 0x0 0.9 1.7\n";
	};
	clip("longrest", 1, -1);
	clip("long", 60, 30);
	return text;
}

// A press on an event's mark holds the clock on the event's own tick while it is held, wherever the
// mouse goes (S17 review: the scrub took it back to the mouse's tick, a frame off on a long clip).
void test_event_press() {
	editor_test::TempProjectDir dir("opennova_editor_ui_clip_event_press");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	CHECK(preview_project(session, dir), "the preview project");
	const SessionView &v = session.view();
	const std::string source = dir.file("source");
	CHECK(editor_test::write_text(source + "/long.o3a", long_clip()), "the long clip written");
	EditorRequest import = request::of(EditorRequestKind::ImportFiles);
	import.imports = {{source + "/long.o3a", {}}};
	session.handle(import);
	session.run_operations();
	DrawnDevices devices;
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_devices(&devices.cache);
	ClipRun run{session, devices, ui};
	const std::string path = "anims/long.bad";
	session.handle(request::open_document(path));
	session.handle(request::set_viewport(path, R"({"options": {"rig_model": "skinned.3di"}, "clock": {"playing": false, "ticks": 0}})"));
	run.settle();
	const ModelViewport *model = run.model(path);
	CHECK(model && model->clip_frame_count() == 60 && model->clip_events().size() == 1, "the long clip plays, one event");
	if (!model || model->clip_events().size() != 1) return;
	const int32_t event_tick = model->clip_events().front().tick;
	// The track's line: the Preview's content probed down until the mouse is over the track.
	const ImGuiID track = item_id(item_id(Ui::window_id("Preview"), {"model", path.c_str()}), {"##track"});
	ImGuiWindow *preview = ImGui::FindWindowByName("Preview");
	CHECK(preview != nullptr, "the Preview");
	if (!preview) return;
	const float left = preview->WorkRect.Min.x + 6.0f, right = preview->WorkRect.Max.x - 6.0f;
	const float event_x = left + (right - left) * float(event_tick) / float(model->clip_length_ticks());
	float track_y = -1.0f;
	for (float y = preview->Pos.y; y < preview->Pos.y + preview->Size.y && track_y < 0.0f; y += 3.0f) {
		ui.mouse(event_x, y);
		if (ImGui::GetCurrentContext()->HoveredId == track) track_y = y;
	}
	CHECK(track_y >= 0.0f, "the track found");
	if (track_y < 0.0f) return;
	// Pressed 3 px off the mark (within its reach), then held while the mouse moves 3 px the other way.
	ui.mouse(event_x + 3.0f, track_y);
	ui.button(true);
	run.settle(1);
	CHECK(run.clock().ticks() == event_tick, "the press goes to the event's own tick");
	ui.mouse(event_x - 3.0f, track_y);
	run.settle(2);
	CHECK(run.clock().ticks() == event_tick && !run.clock().playing(), "held there while the press is");
	ui.button(false);
	run.settle(1);
	CHECK(run.clock().ticks() == event_tick, "and there when let go");
	ui.drain();
}

// From a model's Animations, a map opened to play on it takes the model alone: the map's viewport keeps
// its own options (S17 review: the request reset its level, overlays, Repeat and Bones).
void test_animations_keep_options() {
	editor_test::TempProjectDir dir("opennova_editor_ui_animations_keep_options");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	CHECK(preview_project(session, dir), "the preview project");
	const SessionView &v = session.view();
	CHECK(editor_test::write_text(v.project.root + "/defs/items.def",
	                              "begin \"Skinned Thing\"\nid 100200\ntype building\ngraphic skinned\nanim_def skin\nend\n"
	                              "begin \"Armored\"\nid 100201\ntype building\ngraphic armory\nanim_def skin\nend\n"),
	      "two items playing SKIN.adm");
	session.handle(request::rescan());
	session.run_operations();
	while (v.activity.validation.running) session.poll();
	DrawnDevices devices;
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_devices(&devices.cache);
	ClipRun run{session, devices, ui};
	const std::string map = "anims/SKIN.adm";
	session.handle(request::open_document(map));
	session.handle(request::set_viewport(map, R"({"options": {"repeat": false, "bones": false}})"));
	run.settle();
	const std::string model_path = "models/armory.3di";
	session.handle(request::open_document(model_path));
	run.settle();
	const ImGuiID scope = item_id(Ui::window_id("Preview"), {"model", model_path.c_str()});
	ui.activate(item_id(scope, {"Animations (1)"}));
	run.settle(1);
	ui.activate(popup_item(ImHashStr("animations", 0, scope), "SKIN.adm  (Armored)"));
	run.settle();
	const ModelViewport *shown = run.model(map);
	CHECK(v.documents.active == map && shown && opennova::strutil::iequals(shown->options().rig_model, "armory.3di"),
	      "the map opened to play on the model");
	CHECK(shown && !shown->options().repeat && !shown->options().bones, "its own options kept");
	ui.drain();
}

// A weapon's map in first person (DI-13): the First person popup says what draws, its Eye is the viewport's
// first-person view, and the timeline's legend names the action's set and says the clip's events go unread.
void test_first_person() {
	editor_test::TempProjectDir dir("opennova_editor_ui_first_person");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	CHECK(preview_project(session, dir), "the preview project");
	const SessionView &v = session.view();
	// Where the project keeps a file of the name (a blank Create Missing made), else at its top.
	const auto at = [&](const std::string &name) {
		const AssetEntry *entry = v.project.scan ? v.project.scan->find(name) : nullptr;
		return v.project.root + "/" + (entry ? entry->relative_path : name);
	};
	// The map is a weapon's (its animadm beside its gfx1), no item's; the walk is its FIRE's clip.
	CHECK(editor_test::write_text(v.project.root + "/defs/items.def",
	                              "begin \"Skinned Thing\"\nid 100200\ntype building\ngraphic skinned\nend\n") &&
	              editor_test::write_text(at("weapon.def"),
	                                      editor_test::crlf("weapon \"WPN_SKIN\"\n\tclipsize 10\n\tanimadm skin\n\tgfx1 skinned\n"
	                                                        "\taction \"fire\"\n\t\tanim anim_walk_forward\n\t\tsoundsetend GS_SKIN\n"
	                                                        "\t\tfunction wpn_std_fire\n\tend\nend\n")) &&
	              editor_test::write_text(at("Avatars.def"),
	                                      editor_test::crlf("define head H1\n{\n\tgraphic skinned.3di\n}\n"
	                                                        "define body B1\n{\n\tgraphic skinned.3di\n}\n"
	                                                        "define arms A1\n{\n\tgraphic armory.3di\n}\n"
	                                                        "nationality 0 AV_N\n{\n\talignment good\n\tdivision 0 AV_D\n"
	                                                        "\t{\n\t\tcombo 1 H1 B1 A1\n\t}\n}\n")),
	      "the weapon and the characters written");
	session.handle(request::rescan());
	session.run_operations();
	while (v.activity.validation.running) session.poll();
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
	CHECK(model && model->first_person().active() && model->first_person().arms_model(), "the gun with its arms");
	if (!model) return;
	ui.away();
	std::string text = lowered(logged_frame(ui));
	CHECK(text.find("first person###first_person") == std::string::npos && text.find("first person") != std::string::npos,
	      "the First person button");
	CHECK(text.find("e fire's set") != std::string::npos, "the legend names the action's set");
	CHECK(text.find("events: not read in first person") != std::string::npos, "the clip's events go unread");
	const ImGuiID scope = item_id(Ui::window_id("Preview"), {"model", path.c_str()});
	ui.activate(item_id(scope, {"First person###first_person"}));
	run.settle(1);
	text = lowered(logged_frame(ui));
	CHECK(text.find("wpn_skin draws skinned with armory.3di") != std::string::npos, "what draws, in words");
	CHECK(text.find("gs_skin plays as fire finishes") != std::string::npos, "the action's leg in words");
	ui.activate(popup_item(ImHashStr("first_person", 0, scope), "Eye"));
	run.settle(1);
	CHECK(model->options().first_person.eye && model->eye_view() && model->camera().posed, "Eye: the first-person eye");
	ui.away();
	CHECK(lowered(logged_frame(ui)).find("first person (eye)") != std::string::npos, "the button says the eye shows");
	ui.drain();
}

} // namespace

void run_animation_tests() {
	test_timeline();
	test_event_press();
	test_animations_keep_options();
	test_first_person();
}

} // namespace editor_ui_test
