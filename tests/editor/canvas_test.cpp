// S13 V2 (ADR 0046 S13): the Preview's canvas without ImGui. The one gesture machine
// (preview/canvas_gesture): the drag threshold on the screen, one token per gesture, a click told
// from a drag, and exactly one EndEdit for the document a gesture began in when it ends (let go,
// lost, the canvas not drawn, another subject shown: another document, a reload of it, another
// screen of a menu), nothing after. The menu's canvas (preview/menu_canvas) over a screen compiled
// headless, at 100%: a click selects what the game's hit test finds; BOX dragged by (40, 21) lands
// on 144/344/120/220 on the grid of 8, its corner with Alt on 313/207; a drag from OTHER selects
// and moves it; TINY, 12 units across, moves when pressed in its middle and resizes at its corner;
// a zoom or a refit while the button is down moves nothing (the drag is the pointer's travel on
// the screen); through the key channel the arrows move BOX to 101/301 and, with Shift, 92/192,
// the keyboard gone elsewhere ends a nudge, and Esc selects MAIN; a stale picture maps nothing.
// Several windows: Shift and Ctrl
// clicks, a drag of a selected window moving both, the arrows moving both, the marquee picking
// BOX and OTHER (nothing: the screen), the clipboard's rule (a Paste after OTHER at position 2)
// and Arrange; a press inside a selected window moving the selection. A window a list's part
// holds is never the canvas's to pick or move, and its clipboard takes nothing (a Paste after the
// listed window holding the part). The shapes the canvas draws. The model's canvas
// (preview/model_canvas) over a real session: F frames the selected marker, a click selects a
// marker's record, a drag of it is one gesture ended once when its canvas is not drawn, a drag
// elsewhere orbits and the wheel dollies, and a press ends when the model's document goes. The
// panes' wiring of all this is tests/editor_ui's (the preview and workspace groups).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <base/vfs/file_source.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/model_document.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/canvas_gesture.h>
#include <editor/preview/menu_canvas.h>
#include <editor/preview/menu_screen_render.h>
#include <editor/preview/model_canvas.h>
#include <editor/preview/model_overlay.h>
#include <editor/preview/model_preview_state.h>
#include <editor/preview/viewport_overlay.h>
#include <editor/session/project_session.h>
#include <editor/session/session_view.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;

namespace {

using editor_test::NoProcess;

constexpr NodeKind kScreenKind = node_kind(MenuKind::Screen);

// --- a canvas's requests, recorded ---------------------------------------------------------

struct Request {
	enum class Kind { Select, Edits, EndEdit };
	Kind kind = Kind::Select;
	std::string path;
	NodeAddress record;
	CanvasJoin join = CanvasJoin::Replace;
	std::vector<Edit> edits;
};

struct Recorder final : CanvasRequests {
	std::vector<Request> requests;
	void select(const std::string &path, const NodeAddress &record, CanvasJoin join) override {
		Request request;
		request.kind = Request::Kind::Select;
		request.path = path;
		request.record = record;
		request.join = join;
		requests.push_back(std::move(request));
	}
	void edits(const std::string &path, std::vector<Edit> batch) override {
		Request request;
		request.kind = Request::Kind::Edits;
		request.path = path;
		request.edits = std::move(batch);
		requests.push_back(std::move(request));
	}
	void end_edit(const std::string &path) override {
		Request request;
		request.kind = Request::Kind::EndEdit;
		request.path = path;
		requests.push_back(std::move(request));
	}
	std::vector<Request> take() {
		std::vector<Request> out;
		out.swap(requests);
		return out;
	}
};

size_t count_of(const std::vector<Request> &requests, Request::Kind kind) {
	return size_t(std::count_if(
			requests.begin(), requests.end(), [&](const Request &r) { return r.kind == kind; }));
}

// Exactly one EndEdit among `requests`, the last, for `path`.
bool ends_once(const std::vector<Request> &requests, const std::string &path) {
	return count_of(requests, Request::Kind::EndEdit) == 1 &&
			requests.back().kind == Request::Kind::EndEdit && requests.back().path == path;
}

// The selections among `requests`, in order.
std::vector<std::pair<NodeAddress, CanvasJoin>> selections(const std::vector<Request> &requests) {
	std::vector<std::pair<NodeAddress, CanvasJoin>> out;
	for (const Request &request : requests)
		if (request.kind == Request::Kind::Select)
			out.emplace_back(request.record, request.join);
	return out;
}

// The last batch of a drag's requests, how many batches, and the token they all carry (0: none,
// or not one).
std::vector<Edit> batches(const std::vector<Request> &requests, uint64_t &gesture, size_t &count) {
	std::vector<Edit> last;
	gesture = 0;
	count = 0;
	bool one = true;
	for (const Request &request : requests) {
		if (request.kind != Request::Kind::Edits)
			continue;
		++count;
		for (const Edit &edit : request.edits) {
			one = one && edit.gesture != 0 && (gesture == 0 || edit.gesture == gesture);
			gesture = edit.gesture;
		}
		last = request.edits;
	}
	if (!one)
		gesture = 0;
	return last;
}

// Where a batch sets a field (of `window`, when named): -1 when it does not.
int64_t set_value(const std::vector<Edit> &edits, const char *field,
		const NodeAddress &window = NodeAddress()) {
	for (const Edit &edit : edits)
		if (edit.operation == EditOperation::Set && edit.field == field &&
				(!window.child || edit.address == window))
			return std::get<int64_t>(edit.value);
	return -1;
}

// --- the menu's canvas over a headless render ----------------------------------------------

// MAIN over the whole design, BOX, OTHER and the 12-unit TINY inside it, every edge written.
const char *const kLayoutMenu = "<SCREEN>\r\n"
								"\t<NAME>LAYOUT</NAME>\r\n"
								"\t<WINDOW type=\"window\" name=\"MAIN\">\r\n"
								"\t\t<POSITION><LEFT>0</LEFT><TOP>0</TOP>"
								"<RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>\r\n"
								"\t\t<WINDOW type=\"window\" name=\"BOX\">\r\n"
								"\t\t\t<POSITION><LEFT>100</LEFT><TOP>100</TOP>"
								"<RIGHT>300</RIGHT><BOTTOM>200</BOTTOM></POSITION>\r\n"
								"\t\t</WINDOW>\r\n"
								"\t\t<WINDOW type=\"checkbox\" name=\"OTHER\">\r\n"
								"\t\t\t<POSITION><LEFT>400</LEFT><TOP>300</TOP>"
								"<RIGHT>600</RIGHT><BOTTOM>400</BOTTOM></POSITION>\r\n"
								"\t\t</WINDOW>\r\n"
								"\t\t<WINDOW type=\"checkbox\" name=\"TINY\">\r\n"
								"\t\t\t<POSITION><LEFT>600</LEFT><TOP>104</TOP>"
								"<RIGHT>612</RIGHT><BOTTOM>116</BOTTOM></POSITION>\r\n"
								"\t\t</WINDOW>\r\n"
								"\t</WINDOW>\r\n"
								"</SCREEN>\r\n";

// MAIN over the whole design holding PANEL, whose list CHOICES has a scrollbar part holding
// INPART, a window the tree does not list.
const char *const kPartMenu =
		"<SCREEN>\r\n"
		"\t<NAME>PARTS</NAME>\r\n"
		"\t<WINDOW type=\"window\" name=\"MAIN\">\r\n"
		"\t\t<POSITION><LEFT>0</LEFT><TOP>0</TOP>"
		"<RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>\r\n"
		"\t\t<WINDOW type=\"window\" name=\"PANEL\">\r\n"
		"\t\t\t<POSITION><LEFT>100</LEFT><TOP>100</TOP>"
		"<RIGHT>500</RIGHT><BOTTOM>500</BOTTOM></POSITION>\r\n"
		"\t\t\t<WINDOW type=\"list\" name=\"CHOICES\">\r\n"
		"\t\t\t\t<POSITION><LEFT>0</LEFT><TOP>0</TOP>"
		"<RIGHT>300</RIGHT><BOTTOM>300</BOTTOM></POSITION>\r\n"
		"\t\t\t\t<ITEMS><ITEM value=\"1\">One</ITEM><ITEM value=\"2\">Two</ITEM></ITEMS>\r\n"
		"\t\t\t\t<SCROLLBAR><APPEARANCE state=\"default\"></APPEARANCE>"
		"<WINDOW type=\"static\" name=\"INPART\">"
		"<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>20</RIGHT><BOTTOM>20</BOTTOM></POSITION>"
		"</WINDOW></SCROLLBAR>\r\n"
		"\t\t\t</WINDOW>\r\n"
		"\t\t</WINDOW>\r\n"
		"\t</WINDOW>\r\n"
		"</SCREEN>\r\n";

// No file at all: a headless render with nothing mounted.
struct NoFiles final : opennova::FileSource {
	bool read(const std::string &, std::vector<uint8_t> &) const override { return false; }
	uint64_t stamp(const std::string &) const override { return 0; }
};

NodeAddress named(const Document &document, const char *name) {
	NodeAddress address;
	find_definition(AssetGraph(), document, name, address);
	return address;
}

// A window by name wherever it sits, a part's too.
NodeAddress walked(const Document &document, const Node &row, const char *name) {
	NodeAddress found;
	document.walk_records(row, [&](const NodeAddress &record, const Document::Placement &) {
		if (record.kind == node_kind(MenuKind::Window) && document.record_name(record) == name)
			found = record;
		return !found.child;
	});
	return found;
}

// A menu on the canvas at 100% (a pixel is a design unit), driven frame by frame as the pane
// drives it: the frame's start, then the pointer.
struct MenuRig {
	MnuDocument document;
	NoFiles files;
	MenuScreenRender render;
	MenuCanvasFrame frame;
	MenuCanvas canvas;
	Recorder out;
	std::vector<NodeAddress> selected; // the session's selected records, which the frame borrows

	bool load(const char *text, const char *path) {
		Diagnostic error;
		const std::string bytes = text;
		if (!document.load_bytes(std::vector<uint8_t>(bytes.begin(), bytes.end()), path,
					AssetKind::Menu, "jo", error) ||
				document.rows().empty())
			return false;
		const Node &screen = *document.rows()[0];
		if (render.configure(document, screen.id, files, {}) != MenuPreviewStatus::Ready)
			return false;
		frame.document = &document;
		frame.screen = &screen;
		frame.compiler = &render.compiler();
		frame.state = &render.state();
		frame.current = true;
		return true;
	}
	// The session's selection: `primary` the primary record among `records`.
	void select(const NodeAddress &primary, const std::vector<NodeAddress> &records) {
		selected = records;
		menu_canvas_select(frame, primary, selected);
	}
	void select(const NodeAddress &record) { select(record, { record }); }

	static CanvasInput at(float x, float y, CanvasKeys keys = CanvasKeys()) {
		CanvasInput in;
		in.width = 800;
		in.height = 600;
		in.mouse = CanvasPoint{ x, y };
		in.screen = CanvasPoint{ x, y }; // the picture's corner at the screen's origin
		in.hovered = true;
		in.keys = keys;
		return in;
	}
	// A frame of the keyboard over the canvas, which has it: an arrow pressed (-1, 0 or 1 on each
	// axis), held or not.
	static CanvasInput keys_at(int arrow_x, int arrow_y, bool held, bool shift = false) {
		CanvasInput in = at(700.0f, 550.0f);
		in.keys.shift = shift;
		in.keyboard.focused = true;
		in.keyboard.arrow_x = arrow_x;
		in.keyboard.arrow_y = arrow_y;
		in.keyboard.arrow_held = held;
		return in;
	}
	void step(const CanvasInput &in) {
		canvas.follow(frame, out);
		canvas.input(frame, in, out);
	}
	// A press at (x, y), let go there.
	std::vector<Request> click(float x, float y, CanvasKeys keys = CanvasKeys()) {
		CanvasInput in = at(x, y, keys);
		in.pressed = in.down = true;
		step(in);
		in.pressed = in.down = false;
		step(in);
		return out.take();
	}
	// A press at (x, y), four samples to (x + dx, y + dy), let go there.
	std::vector<Request> drag(
			float x, float y, float dx, float dy, CanvasKeys keys = CanvasKeys()) {
		CanvasInput in = at(x, y, keys);
		in.pressed = in.down = true;
		step(in);
		for (int i = 1; i <= 4; ++i) {
			in = at(x + dx * float(i) / 4.0f, y + dy * float(i) / 4.0f, keys);
			in.down = true;
			step(in);
		}
		in.down = false;
		step(in);
		return out.take();
	}
	// An arrow pressed (-1, 0 or 1 on each axis), then let go: the key channel's frames.
	std::vector<Request> nudge(int arrow_x, int arrow_y, bool shift = false) {
		step(keys_at(arrow_x, arrow_y, true, shift));
		step(keys_at(0, 0, false, shift));
		return out.take();
	}
	// Esc pressed on the canvas.
	std::vector<Request> escape() {
		CanvasInput in = keys_at(0, 0, false);
		in.keyboard.escape = true;
		step(in);
		return out.take();
	}
};

// --- the gesture machine -------------------------------------------------------------------

// The subjects the machine's tests draw on: a document, a reload of it, another screen of it,
// another document.
const CanvasSubject kMenuA{ "a.mnu", 1, 10 };
const CanvasSubject kMenuAReloaded{ "a.mnu", 2, 10 };
const CanvasSubject kMenuAOtherScreen{ "a.mnu", 1, 11 };
const CanvasSubject kMenuB{ "b.mnu", 3, 10 };

int test_gesture_machine() {
	Recorder out;
	CanvasGesture gesture;
	TEST_EXPECT(canvas_join(CanvasKeys{ true, true, false }) == CanvasJoin::Toggle &&
			canvas_join(CanvasKeys{ true, false, false }) == CanvasJoin::Add &&
			canvas_join(CanvasKeys{ false, false, true }) == CanvasJoin::Replace);

	// A press that never travels kDragThreshold on the screen is a click, and raises nothing.
	gesture.press(kMenuA, CanvasPoint{ 10.0f, 10.0f }, out);
	TEST_EXPECT(gesture.pressed() && !gesture.dragging() && gesture.path() == "a.mnu" &&
			gesture.subject() == kMenuA);
	TEST_EXPECT(!gesture.move(CanvasPoint{ 12.0f, 12.0f })); // 2.8 pixels away
	TEST_EXPECT(gesture.travel(CanvasPoint{ 12.0f, 7.0f }).x == 2.0f &&
			gesture.travel(CanvasPoint{ 12.0f, 7.0f }).y == -3.0f);
	TEST_EXPECT(gesture.release(out) && out.take().empty() &&
			gesture.mode() == CanvasGesture::Mode::None);

	// Past the threshold it is a drag, once; its steps carry one token.
	gesture.press(kMenuA, CanvasPoint{ 10.0f, 10.0f }, out);
	TEST_EXPECT(gesture.move(CanvasPoint{ 13.0f, 10.0f }) && gesture.dragging());
	TEST_EXPECT(!gesture.move(CanvasPoint{ 20.0f, 10.0f }) && gesture.dragging());
	const uint64_t token = gesture.token();
	TEST_EXPECT(token != 0 && gesture.token() == token);
	// No step went out: letting go raises nothing.
	TEST_EXPECT(!gesture.release(out) && out.take().empty());

	// A step went out: exactly one EndEdit for the document the drag began in, then nothing.
	gesture.press(kMenuA, CanvasPoint{ 0.0f, 0.0f }, out);
	gesture.move(CanvasPoint{ 5.0f, 0.0f });
	TEST_EXPECT(gesture.token() != token); // a new gesture, a new token
	gesture.sent();
	TEST_EXPECT(!gesture.release(out));
	std::vector<Request> raised = out.take();
	TEST_EXPECT(raised.size() == 1 && ends_once(raised, "a.mnu"));
	TEST_EXPECT(!gesture.release(out));
	gesture.end(out);
	TEST_EXPECT(out.take().empty());

	// Lost mid-drag: the end raised once, and letting go after raises nothing.
	gesture.press(kMenuA, CanvasPoint{ 0.0f, 0.0f }, out);
	gesture.move(CanvasPoint{ 5.0f, 0.0f });
	gesture.sent();
	gesture.end(out);
	raised = out.take();
	TEST_EXPECT(raised.size() == 1 && ends_once(raised, "a.mnu"));
	TEST_EXPECT(!gesture.release(out) && out.take().empty());

	// A nudge is a gesture too; a press ends it (its end first), one gesture at a time.
	gesture.nudge(kMenuA, out);
	TEST_EXPECT(gesture.nudging() && gesture.token() != 0);
	gesture.sent();
	gesture.press(kMenuB, CanvasPoint{ 0.0f, 0.0f }, out);
	raised = out.take();
	TEST_EXPECT(raised.size() == 1 && ends_once(raised, "a.mnu") && gesture.pressed() &&
			gesture.path() == "b.mnu");
	gesture.end(out);
	TEST_EXPECT(out.take().empty()); // the press sent nothing

	// The frame bracket: a canvas that draws its subject keeps its gesture, one that does not
	// draw ends it.
	gesture.nudge(kMenuA, out);
	gesture.sent();
	gesture.frame(kMenuA, out);
	gesture.end_frame(out);
	TEST_EXPECT(gesture.nudging() && out.take().empty());
	gesture.end_frame(out); // a frame it did not draw
	raised = out.take();
	TEST_EXPECT(raised.size() == 1 && ends_once(raised, "a.mnu") && !gesture.nudging());
	gesture.end_frame(out);
	TEST_EXPECT(out.take().empty());

	// A canvas that draws another subject ends the gesture begun on the first, its one end for
	// the document it began in: another document, a reload of the same path, another screen of
	// the same menu.
	for (const CanvasSubject &shown : { kMenuB, kMenuAReloaded, kMenuAOtherScreen }) {
		gesture.press(kMenuA, CanvasPoint{ 0.0f, 0.0f }, out);
		gesture.move(CanvasPoint{ 0.0f, 9.0f });
		gesture.sent();
		gesture.frame(shown, out);
		raised = out.take();
		TEST_EXPECT(raised.size() == 1 && ends_once(raised, "a.mnu") && !gesture.pressed());
		gesture.frame(shown, out);
		TEST_EXPECT(out.take().empty());
	}
	return 0;
}

// --- one window on the menu's canvas (was the Preview window's S9k1 UI test) --------------

int test_menu_canvas() {
	MenuRig rig;
	TEST_EXPECT(rig.load(kLayoutMenu, "layout.mnu"));
	if (!rig.frame.current)
		return 1;
	const MnuDocument &document = rig.document;
	const NodeAddress main = named(document, "MAIN"), box = named(document, "BOX"),
					  other = named(document, "OTHER"), tiny = named(document, "TINY");
	const NodeAddress screen{ document.rows()[0]->id, kScreenKind, 0 };
	rig.select(screen);

	// A click selects the window the game's hit test finds, and edits nothing.
	std::vector<Request> requests = rig.click(200.0f, 150.0f);
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Select &&
			requests[0].record == box && requests[0].join == CanvasJoin::Replace &&
			requests[0].path == "layout.mnu");

	// BOX selected: dragged by (40, 21), snapped: one gesture of Sets, then its end.
	rig.select(box);
	requests = rig.drag(200.0f, 150.0f, 40.0f, 21.0f);
	uint64_t gesture = 0;
	size_t count = 0;
	std::vector<Edit> last = batches(requests, gesture, count);
	TEST_EXPECT(count >= 2 && gesture != 0);
	TEST_EXPECT(set_value(last, "position.left") == 144 &&
			set_value(last, "position.right") == 344 && set_value(last, "position.top") == 120 &&
			set_value(last, "position.bottom") == 220);
	TEST_EXPECT(ends_once(requests, "layout.mnu") && selections(requests).empty());

	// Its bottom-right handle, Alt held: resized, not snapped.
	requests = rig.drag(300.0f, 200.0f, 13.0f, 7.0f, CanvasKeys{ false, false, true });
	last = batches(requests, gesture, count);
	TEST_EXPECT(gesture != 0 && set_value(last, "position.right") == 313 &&
			set_value(last, "position.bottom") == 207 && set_value(last, "position.left") == -1 &&
			set_value(last, "position.top") == -1);
	TEST_EXPECT(ends_once(requests, "layout.mnu"));

	// A drag that starts on another window selects it and moves it.
	requests = rig.drag(500.0f, 350.0f, -8.0f, 0.0f);
	last = batches(requests, gesture, count);
	TEST_EXPECT((selections(requests) ==
			std::vector<std::pair<NodeAddress, CanvasJoin>>{ { other, CanvasJoin::Replace } }));
	TEST_EXPECT(set_value(last, "position.left") == 392 && ends_once(requests, "layout.mnu"));

	// A window 12 units across: pressed in its middle it moves (every point of it is within a
	// handle's reach of a corner, so the handles keep out of its middle); its corner still
	// resizes it.
	rig.select(tiny);
	requests = rig.drag(606.0f, 110.0f, 16.0f, 0.0f);
	last = batches(requests, gesture, count);
	TEST_EXPECT(gesture != 0 && set_value(last, "position.left") == 616 &&
			set_value(last, "position.right") == 628 && set_value(last, "position.top") == -1 &&
			set_value(last, "position.bottom") == -1);
	TEST_EXPECT(selections(requests).empty());
	requests = rig.drag(612.0f, 116.0f, 12.0f, 12.0f);
	last = batches(requests, gesture, count);
	TEST_EXPECT(gesture != 0 && set_value(last, "position.right") == 624 &&
			set_value(last, "position.bottom") == 128 && set_value(last, "position.left") == -1 &&
			set_value(last, "position.top") == -1);

	// A zoom about the pointer (Ctrl+wheel) or a refit while the button is down is no travel:
	// the drag is the pointer's travel on the screen at the press's scale. BOX pressed at
	// (200, 150) and dragged 40 to the right (Alt: free), then the picture zoomed to 150% about
	// the pointer (the design point under it, (240, 150), now at picture (360, 225)): the same
	// rect; 10 more pixels on the screen: 10 more units, not the 175 the picture's coordinates
	// would say; then refitted to 640 x 480 under the still pointer: nothing more.
	rig.select(box);
	const CanvasKeys alt{ false, false, true };
	CanvasInput in = MenuRig::at(200.0f, 150.0f, alt);
	in.pressed = in.down = true;
	rig.step(in);
	in = MenuRig::at(240.0f, 150.0f, alt);
	in.down = true;
	rig.step(in);
	last = batches(rig.out.take(), gesture, count);
	TEST_EXPECT(count == 1 && set_value(last, "position.left") == 140 &&
			set_value(last, "position.right") == 340);
	CanvasInput zoomed = in;
	zoomed.width = 1200;
	zoomed.height = 900;
	zoomed.mouse = CanvasPoint{ 360.0f, 225.0f };
	rig.step(zoomed);
	TEST_EXPECT(count_of(rig.out.take(), Request::Kind::Edits) == 0); // the same rect
	zoomed.mouse = CanvasPoint{ 375.0f, 225.0f };
	zoomed.screen = CanvasPoint{ 250.0f, 150.0f };
	rig.step(zoomed);
	last = batches(rig.out.take(), gesture, count);
	TEST_EXPECT(count == 1 && set_value(last, "position.left") == 150 &&
			set_value(last, "position.right") == 350);
	CanvasInput refitted = zoomed;
	refitted.width = 640;
	refitted.height = 480;
	refitted.mouse = CanvasPoint{ 200.0f, 120.0f };
	rig.step(refitted);
	TEST_EXPECT(count_of(rig.out.take(), Request::Kind::Edits) == 0);
	refitted.down = false;
	rig.step(refitted);
	TEST_EXPECT(ends_once(rig.out.take(), "layout.mnu"));

	// The arrows, through the key channel, nudge the selected window (Shift: 8), one gesture while
	// one is held, ended when none is.
	requests = rig.nudge(1, 0);
	last = batches(requests, gesture, count);
	TEST_EXPECT(count == 1 && gesture != 0 && set_value(last, "position.left") == 101 &&
			set_value(last, "position.right") == 301);
	TEST_EXPECT(ends_once(requests, "layout.mnu"));
	requests = rig.nudge(0, -1, true);
	last = batches(requests, gesture, count);
	TEST_EXPECT(set_value(last, "position.top") == 92 && set_value(last, "position.bottom") == 192);
	// Held on while the keyboard goes elsewhere (another window focused): the nudge's one end
	// then; held and given the keyboard back, nothing until an arrow is pressed again.
	rig.step(MenuRig::keys_at(1, 0, true));
	TEST_EXPECT(count_of(rig.out.take(), Request::Kind::Edits) == 1);
	CanvasInput away = MenuRig::keys_at(0, 0, true);
	away.keyboard.focused = false;
	rig.step(away);
	TEST_EXPECT(ends_once(rig.out.take(), "layout.mnu"));
	rig.step(MenuRig::keys_at(0, 0, true));
	TEST_EXPECT(rig.out.take().empty() && !rig.canvas.gesture().nudging());
	// No arrow reaches a canvas without the keyboard.
	CanvasInput unfocused = MenuRig::keys_at(1, 0, true);
	unfocused.keyboard.focused = false;
	rig.step(unfocused);
	TEST_EXPECT(rig.out.take().empty());

	// Esc selects what holds the primary.
	requests = rig.escape();
	TEST_EXPECT((selections(requests) ==
			std::vector<std::pair<NodeAddress, CanvasJoin>>{ { main, CanvasJoin::Replace } }));
	// Not the active document (no selection on this screen): Esc is not the canvas's.
	rig.select(NodeAddress(), {});
	TEST_EXPECT(!menu_canvas_escape(rig.frame, rig.out) && rig.escape().empty());

	// A picture of another revision maps nothing: no pick, no drag, no nudge.
	rig.select(box);
	rig.frame.current = false;
	TEST_EXPECT(rig.click(500.0f, 350.0f).empty());
	TEST_EXPECT(rig.drag(200.0f, 150.0f, 40.0f, 0.0f).empty());
	TEST_EXPECT(rig.nudge(1, 0).empty());
	TEST_EXPECT(rig.canvas.shapes(rig.frame, MenuRig::at(200.0f, 150.0f)).shapes.empty());
	rig.frame.current = true;
	return 0;
}

// The gestures end when the canvas stops drawing, once each, for the menu they began in: hidden
// (a frame the canvas does not draw: the window closed, collapsed or its tab hidden), switched
// to the other kind (the model's canvas draws while the menu's does not), another menu shown,
// another screen of the same menu shown, the same menu reloaded. Letting go after raises nothing.
int test_menu_gestures_end() {
	MenuRig rig;
	TEST_EXPECT(rig.load(kLayoutMenu, "layout.mnu"));
	if (!rig.frame.current)
		return 1;
	const NodeAddress box = named(rig.document, "BOX");
	rig.select(box);
	const auto start_drag = [&]() {
		CanvasInput in = MenuRig::at(200.0f, 150.0f);
		in.pressed = in.down = true;
		rig.step(in);
		in = MenuRig::at(224.0f, 150.0f);
		in.down = true;
		rig.step(in);
		return count_of(rig.out.take(), Request::Kind::Edits) == 1;
	};
	const auto let_go = [&]() {
		CanvasInput in = MenuRig::at(230.0f, 150.0f);
		rig.step(in);
		rig.canvas.end_frame(rig.out);
		return rig.out.take();
	};

	// Hidden mid-drag: the frame bracket ends it once; drawn again with the button up, nothing.
	TEST_EXPECT(start_drag());
	rig.canvas.end_frame(rig.out); // the frame it was drawn in
	TEST_EXPECT(rig.out.take().empty());
	rig.canvas.end_frame(rig.out); // a frame it was not drawn in
	std::vector<Request> requests = rig.out.take();
	TEST_EXPECT(requests.size() == 1 && ends_once(requests, "layout.mnu"));
	TEST_EXPECT(let_go().empty());

	// Closed mid-drag and opened again with the button still down: ended once, and the button
	// down on the canvas again begins nothing.
	TEST_EXPECT(start_drag());
	rig.canvas.end_frame(rig.out);
	rig.canvas.end_frame(rig.out);
	requests = rig.out.take();
	TEST_EXPECT(requests.size() == 1 && ends_once(requests, "layout.mnu"));
	CanvasInput held = MenuRig::at(260.0f, 150.0f);
	held.down = true;
	rig.step(held);
	TEST_EXPECT(rig.out.take().empty() && !rig.canvas.gesture().pressed());
	TEST_EXPECT(let_go().empty());

	// Switched to the other kind mid-nudge: the model's canvas draws, the menu's does not; the
	// menu's nudge ends once, the model's canvas (no gesture) raises nothing.
	Recorder model_out;
	CanvasGesture model_gesture;
	rig.step(MenuRig::keys_at(1, 0, true));
	rig.canvas.end_frame(rig.out);
	TEST_EXPECT(count_of(rig.out.take(), Request::Kind::Edits) == 1);
	model_gesture.frame(CanvasSubject{ "models/armory.3di", 7, 0 }, model_out);
	model_gesture.end_frame(model_out);
	rig.canvas.end_frame(rig.out);
	requests = rig.out.take();
	TEST_EXPECT(
			requests.size() == 1 && ends_once(requests, "layout.mnu") && model_out.take().empty());
	rig.canvas.end(rig.out); // the arrow let go
	TEST_EXPECT(rig.out.take().empty());

	// Another menu shown mid-drag: the drag's end, for the menu it began in.
	TEST_EXPECT(start_drag());
	MnuDocument other;
	Diagnostic error;
	const std::string bytes = kLayoutMenu;
	TEST_EXPECT(other.load_bytes(std::vector<uint8_t>(bytes.begin(), bytes.end()), "other.mnu",
			AssetKind::Menu, "jo", error));
	MenuCanvasFrame shown = rig.frame;
	shown.document = &other;
	shown.screen = other.rows()[0].get();
	shown.current = false;
	rig.canvas.follow(shown, rig.out);
	requests = rig.out.take();
	TEST_EXPECT(requests.size() == 1 && ends_once(requests, "layout.mnu"));

	// The same menu reloaded mid-drag (another instance at the same path): the drag's end.
	rig.canvas.follow(rig.frame, rig.out);
	TEST_EXPECT(start_drag());
	MnuDocument reloaded;
	TEST_EXPECT(reloaded.load_bytes(std::vector<uint8_t>(bytes.begin(), bytes.end()), "layout.mnu",
			AssetKind::Menu, "jo", error));
	shown.document = &reloaded;
	shown.screen = reloaded.rows()[0].get();
	rig.canvas.follow(shown, rig.out);
	requests = rig.out.take();
	TEST_EXPECT(requests.size() == 1 && ends_once(requests, "layout.mnu"));
	TEST_EXPECT(let_go().empty());
	return 0;
}

// Another screen of the same menu shown mid-drag (a selection in the tree or the MCP, an undo):
// the drag begun on the first screen ends, once, for the menu; it never plans against the second.
int test_menu_screen_switch() {
	MenuRig rig;
	const std::string two = std::string(kLayoutMenu) +
			"<SCREEN>\r\n\t<NAME>SECOND</NAME>\r\n\t<WINDOW type=\"window\" name=\"ALONE\">"
			"<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>"
			"</WINDOW>\r\n</SCREEN>\r\n";
	TEST_EXPECT(rig.load(two.c_str(), "layout.mnu"));
	if (!rig.frame.current || rig.document.rows().size() != 2)
		return 1;
	rig.select(named(rig.document, "BOX"));
	CanvasInput in = MenuRig::at(200.0f, 150.0f);
	in.pressed = in.down = true;
	rig.step(in);
	in = MenuRig::at(224.0f, 150.0f);
	in.down = true;
	rig.step(in);
	TEST_EXPECT(count_of(rig.out.take(), Request::Kind::Edits) == 1);
	MenuCanvasFrame second = rig.frame;
	second.screen = rig.document.rows()[1].get();
	second.current = false; // the device configures the second screen on the next pump
	rig.select(NodeAddress(), {});
	second.windows.clear();
	second.indexes.clear();
	second.primary = NodeAddress();
	second.primary_index = -1;
	in = MenuRig::at(260.0f, 150.0f);
	in.down = true;
	rig.canvas.follow(second, rig.out);
	rig.canvas.input(second, in, rig.out);
	std::vector<Request> requests = rig.out.take();
	TEST_EXPECT(requests.size() == 1 && ends_once(requests, "layout.mnu"));
	in.down = false;
	rig.canvas.follow(second, rig.out);
	rig.canvas.input(second, in, rig.out);
	TEST_EXPECT(rig.out.take().empty());
	return 0;
}

// --- several windows (was the Preview window's S9k2 UI test) -------------------------------

int test_menu_several_windows() {
	MenuRig rig;
	TEST_EXPECT(rig.load(kLayoutMenu, "layout.mnu"));
	if (!rig.frame.current)
		return 1;
	const MnuDocument &document = rig.document;
	const NodeAddress main = named(document, "MAIN"), box = named(document, "BOX"),
					  other = named(document, "OTHER"), tiny = named(document, "TINY");
	const NodeAddress screen{ document.rows()[0]->id, kScreenKind, 0 };
	using Selections = std::vector<std::pair<NodeAddress, CanvasJoin>>;
	rig.select(box);

	// Shift+click adds OTHER; Ctrl+click toggles BOX; neither edits.
	std::vector<Request> requests = rig.click(500.0f, 350.0f, CanvasKeys{ true, false, false });
	TEST_EXPECT(selections(requests) == (Selections{ { other, CanvasJoin::Add } }) &&
			count_of(requests, Request::Kind::Edits) == 0);
	requests = rig.click(200.0f, 150.0f, CanvasKeys{ false, true, false });
	TEST_EXPECT(selections(requests) == (Selections{ { box, CanvasJoin::Toggle } }) &&
			count_of(requests, Request::Kind::Edits) == 0);

	// BOX and OTHER selected, OTHER the primary: a drag of BOX moves both, snapped by BOX; the
	// window dragged becomes the primary, the other stays selected.
	rig.select(other, { box, other });
	requests = rig.drag(200.0f, 150.0f, 40.0f, 21.0f);
	uint64_t gesture = 0;
	size_t count = 0;
	std::vector<Edit> last = batches(requests, gesture, count);
	TEST_EXPECT(count >= 2 && gesture != 0);
	TEST_EXPECT(set_value(last, "position.left", box) == 144 &&
			set_value(last, "position.top", box) == 120 &&
			set_value(last, "position.left", other) == 444 &&
			set_value(last, "position.right", other) == 644 &&
			set_value(last, "position.top", other) == 320);
	TEST_EXPECT(selections(requests) == (Selections{ { box, CanvasJoin::Add } }) &&
			ends_once(requests, "layout.mnu"));

	// The arrows move both.
	requests = rig.nudge(1, 0);
	last = batches(requests, gesture, count);
	TEST_EXPECT(count == 1 && set_value(last, "position.left", box) == 101 &&
			set_value(last, "position.left", other) == 401);

	// A drag from MAIN's empty part (a root window not selected) selects what the box touches;
	// a box over nothing selects the screen; a click on the background still selects it.
	requests = rig.drag(50.0f, 500.0f, 400.0f, -390.0f);
	TEST_EXPECT(selections(requests) ==
					(Selections{ { box, CanvasJoin::Replace }, { other, CanvasJoin::Add } }) &&
			count_of(requests, Request::Kind::Edits) == 0 &&
			count_of(requests, Request::Kind::EndEdit) == 0);
	requests = rig.drag(20.0f, 500.0f, 40.0f, 60.0f);
	TEST_EXPECT(selections(requests) == (Selections{ { screen, CanvasJoin::Replace } }));
	requests = rig.click(20.0f, 500.0f);
	TEST_EXPECT(selections(requests) == (Selections{ { main, CanvasJoin::Replace } }));

	// The clipboard (the menu clipboard's one rule, through the canvas's selection): windows
	// the tree lists are copyable, and a Paste goes after the primary window (OTHER) among its
	// siblings; with the screen among the selection nothing is copyable.
	MenuClipboard board = menu_canvas_clipboard(rig.frame, false);
	TEST_EXPECT(board.copy && !board.paste);
	board = menu_canvas_clipboard(rig.frame, true);
	TEST_EXPECT(board.paste && board.paste_row.row == screen.row &&
			board.paste_parent == main.child && board.paste_position == 2);
	rig.select(other, { screen, box, other });
	TEST_EXPECT(!menu_canvas_clipboard(rig.frame, true).copy);
	rig.select(other, { box, other });

	// Arrange: BOX's left edge to the primary OTHER's, one batch; Bring to front of BOX and
	// OTHER, one Move putting TINY before them; nothing on a stale picture.
	menu_canvas_arrange(rig.frame, ArrangeOp::AlignLeft, rig.out);
	requests = rig.out.take();
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Edits &&
			set_value(requests[0].edits, "position.left", box) == 400 &&
			set_value(requests[0].edits, "position.right", box) == 600 &&
			set_value(requests[0].edits, "position.left", other) == -1);
	menu_canvas_arrange(rig.frame, ArrangeOp::BringToFront, rig.out);
	requests = rig.out.take();
	TEST_EXPECT(requests.size() == 1 && requests[0].edits.size() == 1 &&
			requests[0].edits[0].operation == EditOperation::Move &&
			requests[0].edits[0].address == tiny && requests[0].edits[0].position == 0);
	rig.frame.current = false;
	menu_canvas_arrange(rig.frame, ArrangeOp::AlignLeft, rig.out);
	TEST_EXPECT(rig.out.take().empty());
	rig.frame.current = true;

	// MAIN and OTHER selected, OTHER the primary: a press on BOX (not selected, but inside MAIN's
	// rect) is a press inside a selected window, so the drag moves MAIN (OTHER rides inside it),
	// and MAIN becomes the primary with OTHER still selected.
	rig.select(other, { main, other });
	requests = rig.drag(200.0f, 150.0f, 16.0f, 8.0f);
	last = batches(requests, gesture, count);
	TEST_EXPECT(gesture != 0 && set_value(last, "position.left", main) == 16 &&
			set_value(last, "position.top", main) == 8 &&
			set_value(last, "position.left", box) == -1 &&
			set_value(last, "position.left", other) == -1);
	TEST_EXPECT(selections(requests) == (Selections{ { main, CanvasJoin::Add } }));
	TEST_EXPECT(ends_once(requests, "layout.mnu"));
	TEST_EXPECT(
			requests.size() == 1 + count + 1); // the selection, the steps, the end: nothing else
	return 0;
}

// A window a list's part holds (INPART, in CHOICES's scrollbar): the game compiles no widget for
// it, so the canvas never picks it (a press where the part draws picks CHOICES, the listed window
// holding it) and never moves it; selected from the tree, the canvas's clipboard takes nothing to
// copy and pastes after CHOICES, never into the part.
int test_menu_part_window() {
	MenuRig rig;
	TEST_EXPECT(rig.load(kPartMenu, "parts.mnu"));
	if (!rig.frame.current)
		return 1;
	const MnuDocument &document = rig.document;
	const NodeAddress panel = named(document, "PANEL"), choices = named(document, "CHOICES");
	const NodeAddress in_part = walked(document, *document.rows()[0], "INPART");
	TEST_EXPECT(
			panel.child && choices.child && in_part.child && document.window_index(in_part) < 0);

	// Pressed where the part draws (CHOICES's right edge, its embedded scrollbar): CHOICES.
	std::vector<Request> requests = rig.click(395.0f, 110.0f);
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Select &&
			requests[0].record == choices);
	TEST_EXPECT(menu_canvas_clipboard(rig.frame, true).copy == false); // nothing selected yet
	rig.select(choices);
	MenuClipboard board = menu_canvas_clipboard(rig.frame, true);
	TEST_EXPECT(board.copy && board.paste_parent == panel.child && board.paste_position == 1);

	// INPART selected (the tree's selection): not copyable, a Paste after CHOICES in PANEL.
	rig.select(in_part);
	TEST_EXPECT(rig.frame.primary == in_part);
	board = menu_canvas_clipboard(rig.frame, true);
	TEST_EXPECT(!board.copy && board.paste && board.paste_parent == panel.child &&
			board.paste_position == 1);
	// No handles, no nudge, no drag of it: it has no rect on the screen.
	const OverlayList shapes = rig.canvas.shapes(rig.frame, MenuRig::at(700.0f, 550.0f));
	TEST_EXPECT(std::none_of(shapes.shapes.begin(), shapes.shapes.end(),
			[](const OverlayShape &shape) { return shape.kind == OverlayKind::Marker; }));
	TEST_EXPECT(rig.nudge(1, 0).empty());
	return 0;
}

// What the canvas draws over the picture: the primary outlined thick with its eight handles on
// the rect the game draws it at, the other selected windows thin, the window under the pointer,
// the marquee's box while it is dragged; and the cursor over a handle.
int test_menu_shapes() {
	MenuRig rig;
	TEST_EXPECT(rig.load(kLayoutMenu, "layout.mnu"));
	if (!rig.frame.current)
		return 1;
	const MnuDocument &document = rig.document;
	const NodeAddress box = named(document, "BOX"), other = named(document, "OTHER");
	rig.select(box, { other, box });
	const auto of = [](const OverlayList &list, OverlayKind kind, OverlayRole role) {
		std::vector<OverlayShape> out;
		for (const OverlayShape &shape : list.shapes)
			if (shape.kind == kind && shape.role == role)
				out.push_back(shape);
		return out;
	};
	// The pointer over TINY, nothing pressed.
	CanvasInput in = MenuRig::at(606.0f, 110.0f);
	OverlayList list = rig.canvas.shapes(rig.frame, in);
	const std::vector<OverlayShape> selected = of(list, OverlayKind::Rect, OverlayRole::Selected);
	TEST_EXPECT(selected.size() == 2);
	const OverlayShape &thin = selected[0], &thick = selected[1];
	TEST_EXPECT(thin.thickness == 1.0f && thin.points[0].x == 400.0f && thin.points[1].y == 400.0f);
	TEST_EXPECT(thick.thickness == 2.0f && thick.points[0].x == 100.0f &&
			thick.points[0].y == 100.0f && thick.points[1].x == 300.0f &&
			thick.points[1].y == 200.0f);
	const std::vector<OverlayShape> handles = of(list, OverlayKind::Marker, OverlayRole::Normal);
	TEST_EXPECT(handles.size() == 8 && handles[0].glyph == OverlayGlyph::Square &&
			handles[0].points[0].x == 100.0f && handles[0].points[0].y == 100.0f);
	const std::vector<OverlayShape> hover = of(list, OverlayKind::Rect, OverlayRole::Hover);
	TEST_EXPECT(
			hover.size() == 1 && hover[0].points[0].x == 600.0f && hover[0].points[1].x == 612.0f);
	TEST_EXPECT(rig.canvas.hover_tip(rig.frame, in)
						.rfind("TINY (checkbox)\nOn the screen: left 600, top 104", 0) == 0);
	TEST_EXPECT(
			rig.canvas.cursor(rig.frame, MenuRig::at(300.0f, 200.0f)) == CanvasCursor::ResizeNWSE);
	TEST_EXPECT(
			rig.canvas.cursor(rig.frame, MenuRig::at(200.0f, 100.0f)) == CanvasCursor::ResizeNS);
	TEST_EXPECT(rig.canvas.cursor(rig.frame, MenuRig::at(200.0f, 150.0f)) == CanvasCursor::Move);
	TEST_EXPECT(rig.canvas.cursor(rig.frame, MenuRig::at(700.0f, 550.0f)) == CanvasCursor::Default);

	// Mid-marquee: its box, and no hover or tip while the button is down.
	in = MenuRig::at(50.0f, 500.0f);
	in.pressed = in.down = true;
	rig.step(in);
	in = MenuRig::at(250.0f, 450.0f);
	in.down = true;
	rig.step(in);
	list = rig.canvas.shapes(rig.frame, in);
	const std::vector<OverlayShape> box_drawn = of(list, OverlayKind::Rect, OverlayRole::Marquee);
	TEST_EXPECT(box_drawn.size() == 1 && box_drawn[0].points[0].x == 50.0f &&
			box_drawn[0].points[0].y == 450.0f && box_drawn[0].points[1].x == 250.0f &&
			box_drawn[0].points[1].y == 500.0f);
	TEST_EXPECT(of(list, OverlayKind::Rect, OverlayRole::Hover).empty() &&
			rig.canvas.hover_tip(rig.frame, in).empty());
	rig.canvas.end(rig.out);
	rig.out.take();
	return 0;
}

// --- the model's canvas over a real session (was the workspace's model gestures UI test) ---

std::string synth(const char *name) {
	return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/synth/" + name;
}

// What the model pane hands its canvas: the model shown, the picture's markers, the selected
// record's marker while the model is the active document.
ModelCanvasFrame model_frame(
		const SessionView &view, ModelPreviewModel &model, const ModelDocument &document) {
	ModelCanvasFrame frame;
	frame.document = &document;
	frame.model = &model;
	frame.current = model.shown_revision() == document.revision();
	if (frame.current && view.active_document == document.path())
		model_overlay_of(document, view.selection, frame.selected_kind, frame.selected);
	frame.overlays = model.overlays();
	return frame;
}

int test_model_canvas() {
	editor_test::TempProjectDir dir("opennova_editor_canvas_model");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	const SessionView &view = session.view();
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Canvas Test"));
	TEST_EXPECT(editor_test::write_bytes(
			dir.file("project/models/armory.3di"), test_io::read_file(synth("armory.3di"))));
	session.handle(make_request(EditorRequestKind::Rescan));
	session.handle(make_request(EditorRequestKind::OpenDocument, "models/armory.3di"));
	const auto *document =
			dynamic_cast<const ModelDocument *>(session.document_for("models/armory.3di"));
	ModelPreviewModel model;
	TEST_EXPECT(
			document && document->model_row() && model.follow(view) == ModelPreviewAction::Rebuild);
	if (!document || !document->model_row())
		return 1;
	const int width = 640, height = 480;
	model.set_device_size(width, height);
	const ModelRow &row = *document->model_row();
	const NodeAddress point{ row.id, node_kind(ModelKind::UserPoint), row.collections[3][0] };
	EditorRequest select = make_request(EditorRequestKind::SelectRecord, document->path());
	select.edit.address = point;
	session.handle(select);
	ModelCanvas canvas;
	Recorder out;
	const auto at = [&](float x, float y) {
		CanvasInput in;
		in.width = width;
		in.height = height;
		in.mouse = CanvasPoint{ x, y };
		in.screen = CanvasPoint{ x, y };
		in.hovered = true;
		return in;
	};
	// One frame of the canvas as the pane draws it: the marker under the pointer found once,
	// then the pointer and the keys.
	ModelCanvasFrame frame = model_frame(view, model, *document);
	const auto step = [&](const CanvasInput &in) {
		canvas.follow(frame, out);
		canvas.input(frame, in, model_canvas_under(frame, in), out);
	};

	// F, through the key channel while the canvas has the keyboard, frames the selected marker;
	// without the keyboard it does nothing.
	TEST_EXPECT(frame.selected == 0 && frame.selected_kind == ModelOverlayKind::UserPoint);
	CanvasInput keys = at(20.0f, 20.0f);
	keys.hovered = false;
	keys.keyboard.frame = true;
	model.camera().distance = 40.0f;
	step(keys);
	TEST_EXPECT(model.camera().distance == 40.0f);
	keys.keyboard.focused = true;
	step(keys);
	TEST_EXPECT(model.camera().distance != 40.0f);
	frame = model_frame(view, model, *document);
	const ModelOverlay *marker = nullptr;
	for (const ModelOverlay &overlay : frame.overlays)
		if (overlay.kind == ModelOverlayKind::UserPoint && overlay.index == 0)
			marker = &overlay;
	float x = 0.0f, y = 0.0f;
	TEST_EXPECT(marker && model.camera().project(marker->at, width, height, x, y));
	if (!marker)
		return 1;

	// Hovered, the marker is ringed and named; selected, ringed again.
	CanvasInput in = at(x, y);
	const int under = model_canvas_under(frame, in);
	TEST_EXPECT(under >= 0 && size_t(under) < frame.overlays.size() &&
			&frame.overlays[size_t(under)] == marker);
	const OverlayList shapes = canvas.shapes(frame, in, under);
	const auto rings = [&](OverlayRole role) {
		return std::count_if(
				shapes.shapes.begin(), shapes.shapes.end(), [&](const OverlayShape &shape) {
					return shape.kind == OverlayKind::Circle && shape.role == role && !shape.filled;
				});
	};
	TEST_EXPECT(rings(OverlayRole::Hover) == 1 && rings(OverlayRole::Selected) == 1);
	TEST_EXPECT(canvas.hover_tip(frame, under) == marker->name && !marker->name.empty());
	TEST_EXPECT(canvas.hover_tip(frame, -1).empty());

	// A click on the marker selects its record.
	in.pressed = in.down = true;
	step(in);
	in.pressed = in.down = false;
	step(in);
	std::vector<Request> requests = out.take();
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Select &&
			requests[0].record == point && requests[0].path == document->path());

	// A drag of the selected marker (Alt: placed freely): its first step edits the model under
	// one token; the canvas not drawn the next frame (the menu made active, its pane shown): the
	// drag's one end, for the model; letting go raises nothing.
	in = at(x, y);
	in.keys.alt = true;
	in.pressed = in.down = true;
	step(in);
	in = at(x + 30.0f, y + 10.0f);
	in.keys.alt = true;
	in.down = true;
	in.delta = CanvasPoint{ 30.0f, 10.0f };
	step(in);
	canvas.end_frame(out);
	requests = out.take();
	uint64_t gesture = 0;
	size_t count = 0;
	batches(requests, gesture, count);
	TEST_EXPECT(count == 1 && gesture != 0 && requests[0].path == document->path() &&
			count_of(requests, Request::Kind::EndEdit) == 0);
	canvas.end_frame(out);
	requests = out.take();
	TEST_EXPECT(requests.size() == 1 && ends_once(requests, document->path()));
	in.down = false;
	step(in);
	canvas.end_frame(out);
	TEST_EXPECT(out.take().empty());

	// A drag away from the markers orbits the camera and raises nothing; the wheel dollies.
	const float yaw = model.camera().yaw, distance = model.camera().distance;
	in = at(20.0f, 20.0f);
	in.pressed = in.down = true;
	step(in);
	in = at(70.0f, 20.0f);
	in.down = true;
	in.delta = CanvasPoint{ 50.0f, 0.0f };
	step(in);
	in.down = false;
	in.delta = CanvasPoint();
	step(in);
	TEST_EXPECT(model.camera().yaw != yaw && out.take().empty());
	in = at(70.0f, 20.0f);
	in.wheel = 1.0f;
	step(in);
	TEST_EXPECT(std::fabs(model.camera().distance - distance * kModelWheelDolly) < 1e-4f &&
			out.take().empty());

	// A press ends when the model's document goes from the canvas (an animation's rig model
	// shown, which no document holds): an orbit stops there, raising nothing.
	in = at(20.0f, 20.0f);
	in.pressed = in.down = true;
	step(in);
	TEST_EXPECT(canvas.gesture().pressed());
	ModelCanvasFrame rig_model = frame;
	rig_model.document = nullptr;
	rig_model.current = false;
	rig_model.selected = -1;
	canvas.follow(rig_model, out);
	TEST_EXPECT(!canvas.gesture().pressed() && out.take().empty());
	return 0;
}

} // namespace

int main() {
	if (test_gesture_machine() != 0)
		return 1;
	if (test_menu_canvas() != 0)
		return 1;
	if (test_menu_gestures_end() != 0)
		return 1;
	if (test_menu_screen_switch() != 0)
		return 1;
	if (test_menu_several_windows() != 0)
		return 1;
	if (test_menu_part_window() != 0)
		return 1;
	if (test_menu_shapes() != 0)
		return 1;
	if (test_model_canvas() != 0)
		return 1;
	std::printf("editor_canvas: all tests passed\n");
	return 0;
}
