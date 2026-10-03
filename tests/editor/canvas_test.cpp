// S13 V2, V5 (ADR 0046 S13): the viewports' canvas without ImGui. The one gesture machine
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
// BOX and OTHER in one selection (nothing: the screen), the clipboard's rule (a Paste after OTHER at
// position 2)
// and Arrange; a press inside a selected window moving the selection. A window a list's part
// holds is never the canvas's to pick or move, and its clipboard takes nothing (a Paste after the
// listed window holding the part). The shapes the canvas draws. The model's canvas
// (preview/model_canvas) over a real session's model viewport: F frames the selected marker, a
// click selects a marker's record, a drag of it is one gesture ended once when its canvas is not
// drawn, a drag elsewhere orbits and the wheel dollies (each a SetViewport of the viewport's camera,
// which the session applies), the right button and the keys a camera flies by are not its, and a
// press ends when the model's document goes. The camera's ray through a pixel, and a line of text
// among the shapes. Held while an
// operation holds the documents (S13 A3), neither canvas raises an edit: a drag, a resize, the
// arrows and an Arrange of the menu's, a marker's drag of the model's (it orbits), while a click
// and Esc still select. The mission's canvas (preview/mission_canvas, ADR 0046 S14) over a real
// session's mission viewport: a click selects a mark's record, Shift adds, Ctrl toggles; a drag of
// the primary moves the three selected entities, a batch a sample under one token and one EndEdit,
// one undo step; a drag of a mark not selected selects it first; a marquee is one SelectRecord of
// the shown marks in it; a look, a fly at the frame's dt and a wheel notch in one frame are one
// SetViewport, a pan and an orbit each one, F and a double click frame; the arrows nudge the
// selection a metre along the file's axes, PgUp lifts; Delete removes it in one batch; Esc selects
// nothing; held, nothing edits. The views' wiring of all this is tests/editor_ui's (the preview and
// workspace groups).

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
#include <editor/preview/mission_camera.h>
#include <editor/preview/mission_canvas.h>
#include <editor/preview/mission_handle_edit.h>
#include <editor/preview/mission_hint.h>
#include <editor/preview/mission_label_picks.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/mission_viewport.h>
#include <editor/preview/model_canvas.h>
#include <editor/preview/model_overlay.h>
#include <editor/preview/model_viewport.h>
#include <editor/preview/viewport_device.h>
#include <editor/preview/viewport_model.h>
#include <editor/preview/viewport_overlay.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

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
	enum class Kind { Select, Edits, EndEdit, Viewport, Drop, Command };
	Kind kind = Kind::Select;
	std::string path;
	NodeAddress record;
	std::vector<NodeAddress> records; // the records a selection names with it (a marquee's)
	CanvasJoin join = CanvasJoin::Replace;
	std::vector<Edit> edits;
	std::string viewport; // a SetViewport's change
	ViewportDrop drop; // an EditInViewport's drop (the mission's Place, Path and Area tools)
	ViewportCommand command; // an EditInViewport's command (the mission's duplicate, S15)
};

// What the canvas raises, by kind; a kind a canvas never raises fails the test.
struct Recorder final : CanvasRequests {
	std::vector<Request> requests;
	bool unexpected = false;
	void request(EditorRequest raised) override {
		Request out;
		out.path = raised.path;
		switch (raised.kind) {
			case EditorRequestKind::SelectRecord:
				out.kind = Request::Kind::Select;
				out.record = raised.address;
				out.records = raised.records;
				out.join = raised.mode == SelectMode::Add ? CanvasJoin::Add
						: raised.mode == SelectMode::Toggle ? CanvasJoin::Toggle
															: CanvasJoin::Replace;
				break;
			case EditorRequestKind::EditRecord:
				out.kind = Request::Kind::Edits;
				out.edits = std::move(raised.edits);
				break;
			case EditorRequestKind::EndEdit:
				out.kind = Request::Kind::EndEdit;
				break;
			case EditorRequestKind::SetViewport:
				out.kind = Request::Kind::Viewport;
				out.viewport = raised.viewport;
				break;
			case EditorRequestKind::EditInViewport:
				if (raised.command != ViewportCommand()) {
					out.kind = Request::Kind::Command;
					out.command = raised.command;
					break;
				}
				if (raised.drop.file.empty() && raised.drop.reference.empty()) {
					unexpected = true;
					return;
				}
				out.kind = Request::Kind::Drop;
				out.drop = raised.drop;
				break;
			default:
				unexpected = true;
				return;
		}
		requests.push_back(std::move(out));
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
		if (render.configure(document, screen.id, files, {}) != MenuScreenStatus::Ready)
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

	// A drag from MAIN's empty part (a root window not selected) selects what the box touches, in
	// one selection (S13 D7, V5): BOX and OTHER, the last the primary; a box over nothing selects the
	// screen; a click on the background still selects it.
	requests = rig.drag(50.0f, 500.0f, 400.0f, -390.0f);
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Select &&
			requests[0].record == other && requests[0].join == CanvasJoin::Replace &&
			requests[0].records == (std::vector<NodeAddress>{ box, other }));
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

// Held (S13 A3: `editable` false while an operation holds the documents, as the viewport's context
// says from SessionView::allows): a drag of the selected BOX, a resize at its corner, the arrows and
// an Arrange raise no edit; a click still selects, and Esc too. Editable again, the drag moves BOX.
int test_menu_canvas_held() {
	MenuRig rig;
	TEST_EXPECT(rig.load(kLayoutMenu, "layout.mnu"));
	if (!rig.frame.current)
		return 1;
	const MnuDocument &document = rig.document;
	const NodeAddress main = named(document, "MAIN"), box = named(document, "BOX"),
					  other = named(document, "OTHER");
	rig.select(box, { box, other });
	rig.frame.editable = false;
	std::vector<Request> requests = rig.drag(200.0f, 150.0f, 40.0f, 21.0f);
	TEST_EXPECT(count_of(requests, Request::Kind::Edits) == 0);
	requests = rig.drag(300.0f, 200.0f, 13.0f, 7.0f);
	TEST_EXPECT(count_of(requests, Request::Kind::Edits) == 0);
	requests = rig.nudge(1, 0);
	TEST_EXPECT(count_of(requests, Request::Kind::Edits) == 0);
	menu_canvas_arrange(rig.frame, ArrangeOp::AlignLeft, rig.out);
	TEST_EXPECT(rig.out.take().empty());
	requests = rig.click(500.0f, 350.0f);
	TEST_EXPECT((selections(requests) ==
			std::vector<std::pair<NodeAddress, CanvasJoin>>{ { other, CanvasJoin::Replace } }) &&
			count_of(requests, Request::Kind::Edits) == 0);
	rig.select(box);
	requests = rig.escape();
	TEST_EXPECT((selections(requests) ==
			std::vector<std::pair<NodeAddress, CanvasJoin>>{ { main, CanvasJoin::Replace } }));
	rig.select(box);
	rig.frame.editable = true;
	requests = rig.drag(200.0f, 150.0f, 40.0f, 21.0f);
	uint64_t gesture = 0;
	size_t count = 0;
	const std::vector<Edit> last = batches(requests, gesture, count);
	TEST_EXPECT(count >= 1 && gesture != 0 && set_value(last, "position.left") == 144);
	return 0;
}

int test_model_canvas() {
	editor_test::TempProjectDir dir("opennova_editor_canvas_model");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Canvas Test"));
	session.run_operations();
	TEST_EXPECT(editor_test::write_bytes(
			dir.file("project/models/armory.3di"), test_io::read_file(synth("armory.3di"))));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("models/armory.3di"));
	const auto *document =
			dynamic_cast<const ModelDocument *>(session.document_for("models/armory.3di"));
	TEST_EXPECT(document && document->model_row());
	if (!document || !document->model_row())
		return 1;
	const std::string path = document->path();
	// The model's viewport, the size its device draws at the canvas's: 640 x 480.
	const int width = 640, height = 480;
	session.handle(request::set_viewport(
			path, R"({"kind": "model", "device": {"width": 640, "height": 480}})"));
	const auto follow = [&]() {
		return static_cast<const ModelViewport *>(
				session.viewports().follow_one(view, path, ViewportKind::Model));
	};
	const ModelViewport *model = follow();
	TEST_EXPECT(model && model->status() == ViewportStatus::Ready && model->state().width == width);
	if (!model)
		return 1;
	const ModelRow &row = *document->model_row();
	const NodeAddress point{ row.id, node_kind(ModelKind::UserPoint), row.ids.lists[3][0].id };
	EditorRequest select = request::select_record(path, point);
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
	// What the canvas maps of the viewport this frame (the view's input, the picture's size).
	const auto frame_of = [&]() {
		const ViewportContext context{
			ViewportInput{ view, session.viewports().clock(), document, ChangeClass::None }, width,
			height, 0.0f, nullptr
		};
		return follow()->canvas_frame(context);
	};
	// One frame of the canvas as its view draws it: the marker under the pointer found once, then
	// the pointer and the keys.
	ModelCanvasFrame frame = frame_of();
	const auto step = [&](const CanvasInput &in) {
		canvas.follow(frame, out);
		canvas.input(frame, in, model_canvas_under(frame, in), out);
	};
	// The SetViewports the canvas raised, served as the session serves them.
	const auto serve = [&](const std::vector<Request> &requests) {
		size_t served = 0;
		for (const Request &request : requests)
			if (request.kind == Request::Kind::Viewport && request.path == path) {
				session.handle(request::set_viewport(path, request.viewport));
				served += session.outcome().done() ? 1 : 0;
			}
		return served;
	};

	// F, through the key channel while the canvas has the keyboard, frames the selected marker (a
	// SetViewport of the camera); without the keyboard it does nothing.
	TEST_EXPECT(frame.selected == 0 && frame.selected_kind == ModelOverlayKind::UserPoint);
	session.handle(request::set_viewport(path, R"({"kind": "model", "camera": {"distance": 40}})"));
	TEST_EXPECT(follow()->camera().distance == 40.0f);
	frame = frame_of();
	CanvasInput keys = at(20.0f, 20.0f);
	keys.hovered = false;
	keys.keyboard.frame = true;
	step(keys);
	TEST_EXPECT(out.take().empty());
	keys.keyboard.focused = true;
	step(keys);
	std::vector<Request> requests = out.take();
	TEST_EXPECT(requests.size() == 1 && serve(requests) == 1 && follow()->camera().distance != 40.0f);
	frame = frame_of();
	const ModelOverlay *marker = nullptr;
	for (const ModelOverlay &overlay : frame.overlays)
		if (overlay.kind == ModelOverlayKind::UserPoint && overlay.index == 0)
			marker = &overlay;
	float x = 0.0f, y = 0.0f;
	TEST_EXPECT(marker && model->camera().project(marker->at, width, height, x, y));
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
	requests = out.take();
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Select &&
			requests[0].record == point && requests[0].path == path);

	// A drag of the selected marker (Alt: placed freely): its first step edits the model under
	// one token; the canvas not drawn the next frame (the menu made active, its view shown): the
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
	TEST_EXPECT(count == 1 && gesture != 0 && requests[0].path == path &&
			count_of(requests, Request::Kind::EndEdit) == 0);
	canvas.end_frame(out);
	requests = out.take();
	TEST_EXPECT(requests.size() == 1 && ends_once(requests, path));
	in.down = false;
	step(in);
	canvas.end_frame(out);
	TEST_EXPECT(out.take().empty());

	// Held (S13 A3: `editable` false while an operation holds the documents, as the viewport's
	// context says from SessionView::allows): the same drag of the selected marker takes no handle
	// and edits nothing; it orbits the camera (a SetViewport of it, which the session applies).
	{
		ModelCanvasFrame held = frame;
		held.editable = false;
		const float before = follow()->camera().yaw;
		const auto held_step = [&](const CanvasInput &input) {
			canvas.follow(held, out);
			canvas.input(held, input, model_canvas_under(held, input), out);
		};
		CanvasInput press = at(x, y);
		press.keys.alt = true;
		press.pressed = press.down = true;
		held_step(press);
		CanvasInput moved = at(x + 30.0f, y + 10.0f);
		moved.keys.alt = true;
		moved.down = true;
		moved.delta = CanvasPoint{ 30.0f, 10.0f };
		held_step(moved);
		moved.down = false;
		moved.delta = CanvasPoint();
		held_step(moved);
		canvas.end_frame(out);
		const std::vector<Request> orbited = out.take();
		TEST_EXPECT(count_of(orbited, Request::Kind::Edits) == 0 && serve(orbited) >= 1 &&
				follow()->camera().yaw != before);
		frame = frame_of();
	}

	// A drag away from the markers orbits the camera: a SetViewport of it per sample, no edit and
	// no selection; the wheel dollies.
	const float yaw = follow()->camera().yaw, distance = follow()->camera().distance;
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
	requests = out.take();
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Viewport &&
			serve(requests) == 1 && follow()->camera().yaw != yaw &&
			follow()->camera().distance == distance);
	frame = frame_of();
	in = at(70.0f, 20.0f);
	in.wheel = 1.0f;
	step(in);
	requests = out.take();
	TEST_EXPECT(requests.size() == 1 && serve(requests) == 1 &&
			std::fabs(follow()->camera().distance - distance * kModelWheelDolly) < 1e-4f);
	// An orbit and the wheel in one frame: one SetViewport with both (the dolly on the orbited
	// camera; two would each start from the camera the frame began with, the second undoing the
	// first's orbit).
	frame = frame_of();
	const float yaw_before = follow()->camera().yaw, distance_before = follow()->camera().distance;
	in = at(20.0f, 20.0f);
	in.pressed = in.down = true;
	step(in);
	TEST_EXPECT(out.take().empty());
	in = at(70.0f, 20.0f);
	in.down = true;
	in.delta = CanvasPoint{ 50.0f, 0.0f };
	in.wheel = 1.0f;
	step(in);
	requests = out.take();
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Viewport && serve(requests) == 1);
	TEST_EXPECT(follow()->camera().yaw != yaw_before &&
			std::fabs(follow()->camera().distance - distance_before * kModelWheelDolly) < 1e-4f);
	in.down = false;
	in.delta = CanvasPoint();
	in.wheel = 0.0f;
	step(in);
	TEST_EXPECT(out.take().empty());
	TEST_EXPECT(!out.unexpected);

	// The right button is not the model's (a picture that fills the canvas hands it over apart
	// from a press, for a kind whose camera looks with it): pressed, dragged and let go, the keys
	// a camera flies by held meanwhile, it raises nothing and begins no gesture.
	frame = frame_of();
	in = at(20.0f, 20.0f);
	in.right_pressed = in.right_down = true;
	in.keyboard.focused = true;
	in.keyboard.move_z = 1;
	in.keyboard.fast = true;
	in.dt = 1.0f / 60.0f;
	step(in);
	in.mouse = in.screen = CanvasPoint{ 70.0f, 40.0f };
	in.right_pressed = false;
	in.delta = CanvasPoint{ 50.0f, 20.0f };
	step(in);
	in.right_down = false;
	in.delta = CanvasPoint();
	step(in);
	TEST_EXPECT(out.take().empty() && !canvas.gesture().pressed());

	// A press ends when the model's document goes from the canvas (an animation's rig model
	// shown, which no document holds): an orbit stops there, raising nothing.
	frame = frame_of();
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

// The camera's ray through a pixel (what a ground pick follows), and a line of text among the
// shapes: the ray through the pixel a point projects to passes through the point, a unit of its
// direction a unit ahead of the eye; the picture's middle looks at the target; the point on the
// plane facing the eye is the ray's; a device with no size has no ray.
int test_camera_ray_and_text() {
	OrbitCamera camera;
	camera.target = PreviewVec3{ 3.0f, 1.0f, -2.0f };
	camera.yaw = 0.7f;
	camera.pitch = 0.4f;
	camera.distance = 25.0f;
	const int width = 640, height = 480;
	const auto close = [](const PreviewVec3 &a, const PreviewVec3 &b) {
		return std::fabs(a.x - b.x) < 1e-3f && std::fabs(a.y - b.y) < 1e-3f &&
				std::fabs(a.z - b.z) < 1e-3f;
	};
	const auto along = [](const PreviewVec3 &from, const PreviewVec3 &direction, float t) {
		return PreviewVec3{ from.x + direction.x * t, from.y + direction.y * t,
			from.z + direction.z * t };
	};
	const PreviewVec3 point{ 5.0f, -1.0f, -6.0f };
	float x = 0.0f, y = 0.0f, depth = 0.0f;
	TEST_EXPECT(camera.project(point, width, height, x, y, &depth) && depth > 0.0f);
	PreviewVec3 from, direction;
	TEST_EXPECT(camera.ray(x, y, width, height, from, direction));
	TEST_EXPECT(close(from, camera.eye()) && close(along(from, direction, depth), point));
	TEST_EXPECT(camera.ray(width * 0.5f, height * 0.5f, width, height, from, direction) &&
			close(along(from, direction, camera.distance), camera.target));
	PreviewVec3 on_plane;
	TEST_EXPECT(camera.on_view_plane(x, y, width, height, point, on_plane) && close(on_plane, point));
	TEST_EXPECT(!camera.ray(x, y, 0, height, from, direction) &&
			!camera.ray(x, y, width, 0, from, direction));

	OverlayList list;
	list.text(CanvasPoint{ 12.0f, 34.0f }, "Barrel (13)", 0xC0FFEE);
	TEST_EXPECT(list.shapes.size() == 1 && list.shapes[0].kind == OverlayKind::Text &&
			list.shapes[0].text == "Barrel (13)" && list.shapes[0].points[0].x == 12.0f &&
			list.shapes[0].points[0].y == 34.0f && list.shapes[0].rgb == 0xC0FFEE &&
			list.shapes[0].filled && list.shapes[0].role == OverlayRole::Normal);
	return 0;
}

// --- the mission's canvas over a real session (ADR 0046 S14) ------------------------------------

// A device that answers a ground (the height at a mission point) and draws nothing: what a mission
// canvas reads of its device.
struct GroundDevice final : ViewportDevice {
	std::function<double(double x, double y)> ground;
	void draw(const ViewportPicture &) override {}
	void take(ViewportAction, const ViewportModel &, const SessionView &, const PreviewClock &,
			ViewportDeviceReport &report) override {
		report.surface = bool(ground);
	}
	void tick(const ViewportModel &, const PreviewClock &) override {}
	bool ground_at(double x, double y, double &height) const override {
		if (!ground) return false;
		height = ground(x, y);
		return true;
	}
	bool surface_between(const double from[3], const double to[3], double point[3]) const override {
		return ground && editor_test::ground_crossing(ground, from, to, point);
	}
};

// A session over a project holding the minted mission, the mission open and its viewport drawn at
// 640 x 480, every request the canvas raises served through the session as the windows serve them.
// `device` (null: none) is the device its context carries, as the view's carries the Shell's.
struct MissionRig {
	editor_test::TempProjectDir dir{ "opennova_editor_canvas_mission" };
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{ platform, preferences };
	const SessionView &view = session.view();
	std::string path;
	const Document *document = nullptr;
	static constexpr int width = 640, height = 480;
	MissionCanvas canvas;
	Recorder out;
	ViewportDevice *device = nullptr;

	bool open() {
		session.handle(request::new_project(dir.file("project"), "Canvas Mission"));
		session.run_operations();
		editor_test::create_missing_files(session);
		if (!editor_test::write_bytes(view.project.root + "/missions/synth_logic.bms",
					test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/bms/synth_logic.bms")))
			return false;
		session.handle(request::rescan());
		session.run_operations();
		session.handle(request::open_document("missions/synth_logic.bms"));
		document = session.document_for("missions/synth_logic.bms");
		if (!document) return false;
		path = document->path();
		session.handle(request::set_viewport(path, R"({"kind": "mission", "device": {"width": 640, "height": 480}})"));
		if (!session.outcome().done()) return false;
		const MissionViewport *viewport = follow();
		if (!viewport || viewport->status() != ViewportStatus::Ready || viewport->state().width != width) return false;
		// The camera over every entity at this size (the first framing was at the state's own).
		step(at(1.0f, 1.0f));
		canvas.frame_selected(width, height, out);
		return serve(out.take()) == 1;
	}
	const MissionViewport *follow() {
		return static_cast<const MissionViewport *>(session.viewports().follow_one(view, path, ViewportKind::Mission));
	}
	ViewportContext context() {
		return ViewportContext{ ViewportInput{ view, session.viewports().clock(), document, ChangeClass::None }, width,
			height, 0.0f, device };
	}
	static CanvasInput at(float x, float y, CanvasKeys keys = CanvasKeys()) {
		CanvasInput in;
		in.width = width;
		in.height = height;
		in.mouse = CanvasPoint{ x, y };
		in.screen = CanvasPoint{ x, y };
		in.hovered = true;
		in.keys = keys;
		return in;
	}
	// The marks as the picture shows them now.
	std::vector<MissionMark> marks() { return follow()->marks(width, height, device); }
	// The shown entity marks a press at their own pixel takes (the front-most there), in order.
	std::vector<int> pickable() {
		const std::vector<MissionMark> shown = marks();
		std::vector<int> out;
		for (size_t i = 0; i < shown.size(); ++i)
			if (shown[i].shown && shown[i].entity >= 0 && pick_mission_mark(shown, shown[i].x, shown[i].y) == int(i))
				out.push_back(int(i));
		return out;
	}
	const MissionEntityMark *entity(const NodeAddress &record) { return follow()->scene().entity(record.row); }
	// The canvas's tool set as the toolbar and the wire set it (S15: the viewport's options, `options`
	// a JSON object of them), then a frame of the pointer over nothing so the canvas reads it.
	bool tool(const std::string &options) {
		session.handle(request::set_viewport(path, R"({"kind": "mission", "options": )" + options + "}"));
		if (!session.outcome().done()) return false;
		step(at(1.0f, 1.0f));
		out.take();
		return true;
	}
	// One frame of the canvas as its view draws it: the frame's start over the viewport as it follows
	// the document now, then the pointer and the keys.
	void step(const CanvasInput &in) {
		const MissionViewport *viewport = follow();
		const ViewportContext ctx = context();
		canvas.follow(*viewport, ctx, out);
		canvas.input(ctx, in, out);
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
	// A press at `from` dragged to `to` in `samples` steps and let go there, `keys` held throughout.
	std::vector<Request> drag(CanvasPoint from, CanvasPoint to, int samples, CanvasKeys keys = CanvasKeys()) {
		CanvasInput in = at(from.x, from.y, keys);
		in.pressed = in.down = true;
		step(in);
		for (int i = 1; i <= samples; ++i) {
			const float t = float(i) / float(samples);
			CanvasInput moved = at(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t, keys);
			moved.down = true;
			moved.delta = CanvasPoint{ (to.x - from.x) / float(samples), (to.y - from.y) / float(samples) };
			step(moved);
		}
		CanvasInput up = at(to.x, to.y, keys);
		step(up);
		return out.take();
	}
	// A press at `from` with no key held, then dragged to `to` in `samples` steps with `held` held and
	// let go there (Ctrl: free of the snap).
	std::vector<Request> drag_holding(CanvasPoint from, CanvasPoint to, int samples, CanvasKeys held) {
		CanvasInput in = at(from.x, from.y);
		in.pressed = in.down = true;
		step(in);
		for (int i = 1; i <= samples; ++i) {
			const float t = float(i) / float(samples);
			CanvasInput moved = at(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t, held);
			moved.down = true;
			moved.delta = CanvasPoint{ (to.x - from.x) / float(samples), (to.y - from.y) / float(samples) };
			step(moved);
		}
		CanvasInput up = at(to.x, to.y, held);
		step(up);
		return out.take();
	}
	// The requests served through the session, each as the canvas raised it: how many were done.
	size_t serve(const std::vector<Request> &requests) {
		size_t served = 0;
		for (const Request &request : requests) {
			switch (request.kind) {
			case Request::Kind::Select:
				session.handle(request::select_record(path, request.record, select_mode(request.join), request.records));
				break;
			case Request::Kind::Edits: session.handle(request::edit_record(path, request.edits)); break;
			case Request::Kind::EndEdit: session.handle(request::end_edit(path)); break;
			case Request::Kind::Viewport: session.handle(request::set_viewport(path, request.viewport)); break;
			case Request::Kind::Drop: session.handle(request::edit_in_viewport(path, request.drop)); break;
			case Request::Kind::Command: session.handle(request::edit_in_viewport(path, request.command)); break;
			}
			served += session.outcome().done() ? 1 : 0;
		}
		return served;
	}
	// A frame of the keyboard over the canvas, which has it.
	static CanvasInput keys_at(int arrow_x, int arrow_y, bool held) {
		CanvasInput in = at(20.0f, 20.0f);
		in.keyboard.focused = true;
		in.keyboard.arrow_x = arrow_x;
		in.keyboard.arrow_y = arrow_y;
		in.keyboard.arrow_held = held;
		return in;
	}
};

// Every edit of a batch a Set of `x` or `y` on one of `records`.
bool moves_only(const std::vector<Edit> &edits, const std::vector<NodeAddress> &records) {
	for (const Edit &edit : edits) {
		if (edit.operation != EditOperation::Set || (edit.field != "x" && edit.field != "y")) return false;
		if (std::find(records.begin(), records.end(), edit.address) == records.end()) return false;
	}
	return !edits.empty();
}

// A click on a mark selects its record, Shift adds, Ctrl toggles; hovered, the mark is ringed, the
// selected ones ringed too. A drag of the primary (Ctrl held: free) moves the three selected entities on
// the ground: one batch a sample, every edit a Set of x or y on one of the three under one token,
// exactly one EndEdit, and the session applies it as one undo step. A drag of a mark not selected
// selects it alone first, then moves it alone. A click on nothing selects nothing.
int test_mission_canvas() {
	MissionRig rig;
	TEST_EXPECT(rig.open());
	const Selection &selection = rig.view.documents.selection;
	const std::vector<int> picks = rig.pickable();
	TEST_EXPECT(picks.size() >= 4);
	if (picks.size() < 4) return 1;
	std::vector<MissionMark> marks = rig.marks();
	const NodeAddress a = marks[size_t(picks[0])].record, b = marks[size_t(picks[1])].record,
					  c = marks[size_t(picks[2])].record, d = marks[size_t(picks[3])].record;
	std::vector<Request> requests = rig.click(marks[size_t(picks[0])].x, marks[size_t(picks[0])].y);
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Select && requests[0].record == a &&
			requests[0].join == CanvasJoin::Replace && requests[0].path == rig.path && rig.serve(requests) == 1);
	TEST_EXPECT(selection.holds(a) && selection.primary == a);
	CanvasKeys shift;
	shift.shift = true;
	requests = rig.click(marks[size_t(picks[1])].x, marks[size_t(picks[1])].y, shift);
	TEST_EXPECT(requests.size() == 1 && requests[0].record == b && requests[0].join == CanvasJoin::Add && rig.serve(requests) == 1);
	CanvasKeys ctrl;
	ctrl.ctrl = true;
	requests = rig.click(marks[size_t(picks[2])].x, marks[size_t(picks[2])].y, ctrl);
	TEST_EXPECT(requests.size() == 1 && requests[0].record == c && requests[0].join == CanvasJoin::Toggle && rig.serve(requests) == 1);
	TEST_EXPECT(selection.records.size() == 3 && selection.primary == c);
	// Hovered over the first: its ring; the three selected ringed.
	{
		const CanvasInput over = MissionRig::at(marks[size_t(picks[0])].x, marks[size_t(picks[0])].y);
		rig.step(over);
		const OverlayList shapes = rig.canvas.shapes(rig.context(), over);
		const auto rings = [&](OverlayRole role) {
			return std::count_if(shapes.shapes.begin(), shapes.shapes.end(), [&](const OverlayShape &shape) {
				return shape.kind == OverlayKind::Circle && shape.role == role && !shape.filled;
			});
		};
		TEST_EXPECT(rings(OverlayRole::Hover) == 1 && rings(OverlayRole::Selected) == 3);
		TEST_EXPECT(rig.out.take().empty());
	}
	// The primary dragged 40 pixels across, freely (Ctrl held from the press, S15 review: a press with
	// Ctrl still takes the mark; only a click with it toggles), in two samples.
	const double ax = rig.entity(a)->x, bx = rig.entity(b)->x, cx = rig.entity(c)->x;
	marks = rig.marks();
	const CanvasPoint from{ marks[size_t(picks[2])].x, marks[size_t(picks[2])].y };
	CanvasKeys free;
	free.ctrl = true;
	requests = rig.drag(from, CanvasPoint{ from.x + 40.0f, from.y }, 2, free);
	uint64_t gesture = 0;
	size_t count = 0;
	std::vector<Edit> last = batches(requests, gesture, count);
	TEST_EXPECT(count == 2 && gesture != 0 && moves_only(last, { a, b, c }) && ends_once(requests, rig.path));
	TEST_EXPECT(count_of(requests, Request::Kind::Select) == 0);
	TEST_EXPECT(rig.serve(requests) == requests.size());
	TEST_EXPECT(rig.entity(a)->x != ax && rig.entity(b)->x != bx && rig.entity(c)->x != cx);
	// As far each, within the 16.16 word each lands on.
	TEST_EXPECT(std::fabs((rig.entity(a)->x - ax) - (rig.entity(c)->x - cx)) < 2.0 / 65536.0);
	TEST_EXPECT(rig.view.documents.gestures.empty() && selection.records.size() == 3);
	rig.session.handle(request::undo(rig.path));
	TEST_EXPECT(rig.session.outcome().done() && rig.entity(a)->x == ax && rig.entity(c)->x == cx && !rig.document->dirty());
	// A mark not selected dragged: selected alone first, then moved alone.
	marks = rig.marks();
	TEST_EXPECT(!selection.holds(d) && marks[size_t(picks[3])].shown);
	requests = rig.drag(CanvasPoint{ marks[size_t(picks[3])].x, marks[size_t(picks[3])].y },
			CanvasPoint{ marks[size_t(picks[3])].x, marks[size_t(picks[3])].y + 30.0f }, 2, free);
	TEST_EXPECT(requests.size() >= 3 && requests[0].kind == Request::Kind::Select && requests[0].record == d &&
			requests[0].join == CanvasJoin::Replace);
	last = batches(requests, gesture, count);
	TEST_EXPECT(count == 2 && gesture != 0 && moves_only(last, { d }) && ends_once(requests, rig.path));
	TEST_EXPECT(rig.serve(requests) == requests.size() && selection.records.size() == 1 && selection.primary == d);
	// A click on nothing selects nothing; another on nothing raises nothing.
	requests = rig.click(2.0f, 2.0f);
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Select && !requests[0].record.row &&
			rig.serve(requests) == 1 && selection.empty());
	TEST_EXPECT(rig.click(2.0f, 2.0f).empty());
	TEST_EXPECT(!rig.out.unexpected);
	std::printf("test_mission_canvas passed\n");
	return 0;
}

// A drag from nothing is a marquee: drawn while it goes, and let go, one SelectRecord of every shown
// mark in the box, the nearest its record (the primary), joined as the keys say (Shift adds); a box
// over nothing with something selected selects nothing.
int test_mission_marquee() {
	MissionRig rig;
	TEST_EXPECT(rig.open());
	const Selection &selection = rig.view.documents.selection;
	size_t shown = 0;
	for (const MissionMark &mark : rig.marks()) shown += mark.shown ? 1 : 0;
	TEST_EXPECT(shown > 1);
	CanvasInput in = MissionRig::at(1.0f, 1.0f);
	in.pressed = in.down = true;
	rig.step(in);
	CanvasInput moved = MissionRig::at(float(MissionRig::width) - 1.0f, float(MissionRig::height) - 1.0f);
	moved.down = true;
	moved.delta = CanvasPoint{ float(MissionRig::width) - 2.0f, float(MissionRig::height) - 2.0f };
	rig.step(moved);
	TEST_EXPECT(rig.out.take().empty());
	{
		const OverlayList shapes = rig.canvas.shapes(rig.context(), moved);
		const std::vector<OverlayShape> &drawn = shapes.shapes;
		TEST_EXPECT(std::count_if(drawn.begin(), drawn.end(), [](const OverlayShape &shape) {
			return shape.kind == OverlayKind::Rect && shape.role == OverlayRole::Marquee;
		}) == 1);
	}
	moved.down = false;
	moved.delta = CanvasPoint();
	rig.step(moved);
	std::vector<Request> requests = rig.out.take();
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Select && requests[0].join == CanvasJoin::Replace &&
			requests[0].records.size() == shown && requests[0].record == requests[0].records.back());
	TEST_EXPECT(rig.serve(requests) == 1 && selection.records.size() == shown);
	// Shift: the box adds.
	rig.session.handle(request::select_record(rig.path, NodeAddress()));
	TEST_EXPECT(selection.empty());
	CanvasKeys shift;
	shift.shift = true;
	requests = rig.drag(CanvasPoint{ 1.0f, 1.0f }, CanvasPoint{ float(MissionRig::width) - 1.0f, float(MissionRig::height) - 1.0f }, 2, shift);
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Select && requests[0].join == CanvasJoin::Add &&
			requests[0].records.size() == shown && rig.serve(requests) == 1 && selection.records.size() == shown);
	// A box over nothing selects nothing.
	requests = rig.drag(CanvasPoint{ 1.0f, 1.0f }, CanvasPoint{ 4.0f, 4.0f }, 2);
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Select && !requests[0].record.row &&
			requests[0].records.empty() && rig.serve(requests) == 1 && selection.empty());
	TEST_EXPECT(!rig.out.unexpected);
	std::printf("test_mission_marquee passed\n");
	return 0;
}

// The camera: a look with the right button, a fly at the frame's dt while it is held and a wheel
// notch in one frame are one SetViewport, which the session applies (the heading turned, the eye
// moved, the distance shrunk); let go, nothing more and no gesture; the middle button pans (the
// target moved, the angles kept); Alt and a drag from nothing orbits; F and a double click frame.
int test_mission_camera() {
	MissionRig rig;
	TEST_EXPECT(rig.open());
	const OrbitCamera before = rig.follow()->camera();
	CanvasInput in = MissionRig::at(100.0f, 100.0f);
	in.right_pressed = in.right_down = true;
	rig.step(in);
	TEST_EXPECT(rig.out.take().empty() && rig.canvas.looking());
	CanvasInput moved = MissionRig::at(140.0f, 100.0f);
	moved.right_down = true;
	moved.delta = CanvasPoint{ 40.0f, 0.0f };
	moved.keyboard.focused = true;
	moved.keyboard.move_z = 1;
	moved.dt = 0.1f;
	moved.wheel = 1.0f;
	rig.step(moved);
	std::vector<Request> requests = rig.out.take();
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Viewport && rig.serve(requests) == 1);
	const OrbitCamera after = rig.follow()->camera();
	TEST_EXPECT(after.yaw != before.yaw && after.distance < before.distance);
	TEST_EXPECT(after.eye().x != before.eye().x || after.eye().z != before.eye().z);
	moved.right_down = false;
	moved.delta = CanvasPoint();
	moved.keyboard.move_z = 0;
	moved.wheel = 0.0f;
	rig.step(moved);
	TEST_EXPECT(rig.out.take().empty() && !rig.canvas.looking() && !rig.canvas.gesture().pressed());
	// The middle button.
	const OrbitCamera panned_from = rig.follow()->camera();
	CanvasInput middle = MissionRig::at(100.0f, 100.0f);
	middle.pressed = middle.down = middle.middle = true;
	rig.step(middle);
	CanvasInput dragged = MissionRig::at(150.0f, 100.0f);
	dragged.down = dragged.middle = true;
	dragged.delta = CanvasPoint{ 50.0f, 0.0f };
	rig.step(dragged);
	dragged.down = dragged.middle = false;
	dragged.delta = CanvasPoint();
	rig.step(dragged);
	requests = rig.out.take();
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Viewport && rig.serve(requests) == 1);
	{
		const OrbitCamera panned = rig.follow()->camera();
		TEST_EXPECT(panned.yaw == panned_from.yaw && panned.pitch == panned_from.pitch &&
				(panned.target.x != panned_from.target.x || panned.target.z != panned_from.target.z));
	}
	// Alt and a drag from nothing: an orbit.
	const float yaw = rig.follow()->camera().yaw;
	CanvasKeys alt;
	alt.alt = true;
	requests = rig.drag(CanvasPoint{ 2.0f, 2.0f }, CanvasPoint{ 52.0f, 2.0f }, 2, alt);
	TEST_EXPECT(!requests.empty() && count_of(requests, Request::Kind::Viewport) == requests.size() &&
			rig.serve(requests) == requests.size() && rig.follow()->camera().yaw != yaw);
	// F, with the keyboard, frames; a double click too.
	rig.session.handle(request::set_viewport(rig.path, R"({"kind": "mission", "camera": {"distance": 5000}})"));
	TEST_EXPECT(rig.follow()->camera().distance == 5000.0f);
	CanvasInput keys = MissionRig::at(20.0f, 20.0f);
	keys.keyboard.frame = true;
	rig.step(keys);
	TEST_EXPECT(rig.out.take().empty());
	keys.keyboard.focused = true;
	rig.step(keys);
	requests = rig.out.take();
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Viewport && rig.serve(requests) == 1 &&
			rig.follow()->camera().distance < 5000.0f);
	rig.session.handle(request::set_viewport(rig.path, R"({"kind": "mission", "camera": {"distance": 5000}})"));
	CanvasInput twice = MissionRig::at(20.0f, 20.0f);
	twice.double_clicked = true;
	rig.step(twice);
	requests = rig.out.take();
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Viewport && rig.serve(requests) == 1 &&
			rig.follow()->camera().distance < 5000.0f);
	TEST_EXPECT(!rig.out.unexpected);
	std::printf("test_mission_camera passed\n");
	return 0;
}

// The arrows nudge the selection a metre (no snap) along the file's axis nearest the camera's right:
// looking north, the right arrow held two frames moves two selected entities two metres east, a
// batch a frame under one token and, let go, one EndEdit; PgUp lifts a metre; the keyboard gone
// elsewhere ends a nudge; nothing selected, nothing.
int test_mission_nudge() {
	MissionRig rig;
	TEST_EXPECT(rig.open());
	const Selection &selection = rig.view.documents.selection;
	const std::vector<int> picks = rig.pickable();
	TEST_EXPECT(picks.size() >= 2);
	if (picks.size() < 2) return 1;
	const std::vector<MissionMark> marks = rig.marks();
	const NodeAddress a = marks[size_t(picks[0])].record, b = marks[size_t(picks[1])].record;
	rig.session.handle(request::select_record(rig.path, a, SelectMode::Replace, { b }));
	TEST_EXPECT(selection.records.size() == 2);
	TEST_EXPECT(std::fabs(mission_camera_heading(rig.follow()->camera())) < 1e-3);
	const double ax = rig.entity(a)->x, ay = rig.entity(a)->y, bx = rig.entity(b)->x;
	rig.step(MissionRig::keys_at(1, 0, true));
	std::vector<Request> requests = rig.out.take();
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Edits && rig.serve(requests) == 1);
	TEST_EXPECT(rig.canvas.gesture().nudging());
	rig.step(MissionRig::keys_at(1, 0, true));
	requests = rig.out.take();
	uint64_t gesture = 0;
	size_t count = 0;
	batches(requests, gesture, count);
	TEST_EXPECT(count == 1 && gesture != 0 && rig.serve(requests) == 1);
	rig.step(MissionRig::keys_at(0, 0, false));
	requests = rig.out.take();
	TEST_EXPECT(ends_once(requests, rig.path) && rig.serve(requests) == 1 && !rig.canvas.gesture().nudging());
	TEST_EXPECT(std::fabs(rig.entity(a)->x - (ax + 2.0)) < 1e-6 && std::fabs(rig.entity(a)->y - ay) < 1e-6);
	TEST_EXPECT(std::fabs(rig.entity(b)->x - (bx + 2.0)) < 1e-6);
	// One undo step.
	rig.session.handle(request::undo(rig.path));
	TEST_EXPECT(rig.session.outcome().done() && std::fabs(rig.entity(a)->x - ax) < 1e-6 && std::fabs(rig.entity(b)->x - bx) < 1e-6);
	// PgUp.
	const double az = rig.entity(a)->z;
	CanvasInput page = MissionRig::keys_at(0, 0, true);
	page.keyboard.page = 1;
	rig.step(page);
	requests = rig.out.take();
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Edits && rig.serve(requests) == 1);
	// The keyboard elsewhere: the nudge ends.
	page.keyboard.focused = false;
	rig.step(page);
	requests = rig.out.take();
	TEST_EXPECT(ends_once(requests, rig.path) && rig.serve(requests) == 1);
	TEST_EXPECT(std::fabs(rig.entity(a)->z - (az + 1.0)) < 1e-6);
	// Nothing selected: nothing.
	rig.session.handle(request::select_record(rig.path, NodeAddress()));
	rig.step(MissionRig::keys_at(1, 0, true));
	rig.step(MissionRig::keys_at(0, 0, false));
	TEST_EXPECT(rig.out.take().empty());
	TEST_EXPECT(!rig.out.unexpected);
	std::printf("test_mission_nudge passed\n");
	return 0;
}

// Delete removes the selected records in one batch of Removes, which the session applies (two
// entities fewer); Esc with a selection selects nothing, without one raises nothing; held (an
// operation holds the documents), Delete and the arrows raise nothing while Esc still selects.
int test_mission_delete() {
	MissionRig rig;
	TEST_EXPECT(rig.open());
	const Selection &selection = rig.view.documents.selection;
	const std::vector<int> picks = rig.pickable();
	TEST_EXPECT(picks.size() >= 2);
	if (picks.size() < 2) return 1;
	const std::vector<MissionMark> marks = rig.marks();
	const NodeAddress a = marks[size_t(picks[0])].record, b = marks[size_t(picks[1])].record;
	rig.session.handle(request::select_record(rig.path, a, SelectMode::Replace, { b }));
	const size_t entities = rig.follow()->scene().entities().size();
	CanvasInput remove = MissionRig::keys_at(0, 0, false);
	remove.keyboard.remove = true;
	rig.step(remove);
	std::vector<Request> requests = rig.out.take();
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Edits && requests[0].edits.size() == 2);
	if (requests.size() != 1 || requests[0].edits.size() != 2) return 1;
	// A Remove of each selected record (in the selection's order).
	const std::vector<Edit> &removes = requests[0].edits;
	TEST_EXPECT(removes[0].operation == EditOperation::Remove && removes[1].operation == EditOperation::Remove);
	TEST_EXPECT((removes[0].address == a && removes[1].address == b) || (removes[0].address == b && removes[1].address == a));
	TEST_EXPECT(rig.serve(requests) == 1 && rig.follow()->scene().entities().size() == entities - 2 && selection.empty());
	rig.session.handle(request::undo(rig.path));
	TEST_EXPECT(rig.follow()->scene().entities().size() == entities);
	// Esc.
	rig.session.handle(request::select_record(rig.path, a));
	CanvasInput escape = MissionRig::keys_at(0, 0, false);
	escape.keyboard.escape = true;
	rig.step(escape);
	requests = rig.out.take();
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Select && !requests[0].record.row &&
			rig.serve(requests) == 1 && selection.empty());
	rig.step(escape);
	TEST_EXPECT(rig.out.take().empty());
	// Held.
	rig.session.handle(request::select_record(rig.path, a, SelectMode::Replace, { b }));
	TEST_EXPECT(rig.session.start_operation(std::make_unique<editor_test::HoldingOperation>()) != 0);
	TEST_EXPECT(!rig.context().editable());
	rig.step(remove);
	rig.step(MissionRig::keys_at(1, 0, true));
	rig.step(MissionRig::keys_at(0, 0, false));
	TEST_EXPECT(rig.out.take().empty() && rig.follow()->scene().entities().size() == entities);
	rig.step(escape);
	requests = rig.out.take();
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Select && !requests[0].record.row);
	rig.session.run_operations();
	TEST_EXPECT(!rig.out.unexpected);
	std::printf("test_mission_delete passed\n");
	return 0;
}

// The Place tool (S14 V10): with an item set, a click on the picture is one EditInViewport drop of
// that item at the click (the mission's kind), a click on a mark too (it selects nothing); a drag
// raises nothing (no marquee, no move), Alt and a drag still orbit; held by an operation, a click
// places nothing; the tool off, a click selects again.
int test_mission_place() {
	MissionRig rig;
	TEST_EXPECT(rig.open());
	TEST_EXPECT(rig.tool(R"({"tool": "place", "item": 106101})") && rig.canvas.place() == 106101);
	std::vector<Request> requests = rig.click(100.0f, 120.0f);
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Drop && requests[0].path == rig.path);
	if (requests.size() == 1) {
		const ViewportDrop &drop = requests[0].drop;
		TEST_EXPECT(drop.reference == "item" && drop.name == "106101" && drop.file.empty() && drop.x == 100.0f &&
				drop.y == 120.0f && drop.kind == ViewportKind::Mission);
	}
	const std::vector<int> picks = rig.pickable();
	TEST_EXPECT(!picks.empty());
	if (!picks.empty()) {
		const MissionMark mark = rig.marks()[size_t(picks[0])];
		requests = rig.click(mark.x, mark.y);
		TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Drop && count_of(requests, Request::Kind::Select) == 0);
		requests = rig.drag(CanvasPoint{ mark.x, mark.y }, CanvasPoint{ mark.x + 40.0f, mark.y }, 2);
		TEST_EXPECT(requests.empty());
	}
	CanvasKeys alt;
	alt.alt = true;
	requests = rig.drag(CanvasPoint{ 2.0f, 2.0f }, CanvasPoint{ 52.0f, 2.0f }, 2, alt);
	TEST_EXPECT(!requests.empty() && count_of(requests, Request::Kind::Viewport) == requests.size());
	// Held: nothing placed.
	TEST_EXPECT(rig.session.start_operation(std::make_unique<editor_test::HoldingOperation>()) != 0);
	TEST_EXPECT(rig.click(100.0f, 120.0f).empty());
	rig.session.run_operations();
	// Off: a click on nothing with nothing selected raises nothing, on a mark selects it.
	TEST_EXPECT(rig.tool(R"({"tool": "select"})"));
	TEST_EXPECT(rig.click(2.0f, 2.0f).empty());
	if (!picks.empty()) {
		const MissionMark mark = rig.marks()[size_t(picks[0])];
		requests = rig.click(mark.x, mark.y);
		TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Select && requests[0].record == mark.record);
	}
	TEST_EXPECT(!rig.out.unexpected);
	std::printf("test_mission_place passed\n");
	return 0;
}

// The primary's handles (S14 review m10, m1): a tap on its yaw handle (no travel, no mark under it)
// changes nothing, the selection and its handles standing; with the Turn snap set to 90 a drag of the
// yaw handle round to the other side lands the yaw on a multiple of 90.
int test_mission_handle_tap_and_turn() {
	MissionRig rig;
	TEST_EXPECT(rig.open());
	const Selection &selection = rig.view.documents.selection;
	// An entity whose yaw handle stands over no mark.
	const MissionViewport *viewport = rig.follow();
	float hx = 0.0f, hy = 0.0f;
	MissionMark held;
	bool found = false;
	for (const int pick : rig.pickable()) {
		const MissionMark mark = rig.marks()[size_t(pick)];
		TEST_EXPECT(rig.serve(rig.click(mark.x, mark.y)) == 1 && selection.primary == mark.record);
		PreviewVec3 at;
		const std::vector<MissionMark> marks = rig.marks();
		if (!viewport->handle_at(marks[size_t(pick)], MissionHandle::Yaw, at) ||
				!viewport->camera().project(at, MissionRig::width, MissionRig::height, hx, hy) ||
				pick_mission_mark(marks, hx, hy) >= 0 || hx < 1.0f || hy < 1.0f || hx > MissionRig::width - 1.0f ||
				hy > MissionRig::height - 1.0f)
			continue;
		held = mark;
		found = true;
		break;
	}
	TEST_EXPECT(found);
	if (!found) return 1;
	TEST_EXPECT(rig.click(hx, hy).empty() && selection.primary == held.record && selection.records.size() == 1);
	// Turned to the other side of its anchor, the Turn snap 90.
	rig.canvas.set_turn(90.0f);
	const std::vector<Request> requests =
			rig.drag(CanvasPoint{ hx, hy }, CanvasPoint{ 2.0f * held.x - hx, 2.0f * held.y - hy }, 3);
	uint64_t gesture = 0;
	size_t count = 0;
	const std::vector<Edit> last = batches(requests, gesture, count);
	TEST_EXPECT(count >= 1 && last.size() == 1 && last[0].field == "yaw");
	if (last.size() == 1 && std::holds_alternative<int64_t>(last[0].value))
		TEST_EXPECT(std::get<int64_t>(last[0].value) % 90 == 0);
	TEST_EXPECT(!rig.out.unexpected);
	std::printf("test_mission_handle_tap_and_turn passed\n");
	return 0;
}

// A group turn keeps its handle under the pointer (S15 review): two entities selected, the primary's
// yaw handle dragged a quarter turn clockwise about the group's centre (the pointer from the handle to
// the handle's point carried round the centre): the primary's heading turned a quarter, both carried
// round the centre a quarter, the pivot drawn while it goes. (Measured about the primary's own anchor,
// the turn was another angle, and the handle ran from the pointer.)
int test_mission_group_turn() {
	MissionRig rig;
	TEST_EXPECT(rig.open());
	const MissionViewport *viewport = rig.follow();
	const std::vector<int> picks = rig.pickable();
	TEST_EXPECT(picks.size() >= 2);
	if (picks.size() < 2) return 1;
	rig.canvas.set_turn(0.0f); // whole degrees
	bool turned = false;
	for (size_t i = 0; i + 1 < picks.size() && !turned; ++i) {
		const MissionMark primary = rig.marks()[size_t(picks[i])], other = rig.marks()[size_t(picks[i + 1])];
		rig.session.handle(request::select_record(rig.path, primary.record, SelectMode::Replace, { other.record, primary.record }));
		const std::vector<MissionMark> marks = rig.marks();
		MissionPressed a, b;
		PreviewVec3 handle;
		float hx = 0.0f, hy = 0.0f;
		if (!viewport->pressed(primary.record, a) || !viewport->pressed(other.record, b) ||
				!viewport->handle_at(marks[size_t(picks[i])], MissionHandle::Yaw, handle) ||
				!viewport->camera().project(handle, MissionRig::width, MissionRig::height, hx, hy) || pick_mission_mark(marks, hx, hy) >= 0)
			continue;
		double pivot[2];
		TEST_EXPECT(mission_turn_centre({ a, b }, pivot));
		// The handle's point carried a quarter turn clockwise round the centre: (e, n) to (n, -e).
		double h[3];
		preview_to_mission(handle, h);
		const double e = h[0] - pivot[0], n = h[1] - pivot[1];
		float px = 0.0f, py = 0.0f;
		if (!viewport->camera().project(mission_scene_point(pivot[0] + n, pivot[1] - e, a.z), MissionRig::width, MissionRig::height, px,
					py) ||
				px < 1.0f || py < 1.0f || px > MissionRig::width - 1.0f || py > MissionRig::height - 1.0f)
			continue;
		// Drawn round the centre in steps, so every sample's bearing is the pointer's own.
		CanvasInput in = MissionRig::at(hx, hy);
		in.pressed = in.down = true;
		rig.step(in);
		bool pivot_drawn = false;
		for (int step = 1; step <= 6; ++step) {
			const double turn = 90.0 * step / 6.0 * 3.14159265358979323846 / 180.0, c = std::cos(turn), s = std::sin(turn);
			float sx = 0.0f, sy = 0.0f;
			if (!viewport->camera().project(mission_scene_point(pivot[0] + e * c + n * s, pivot[1] + n * c - e * s, a.z),
						MissionRig::width, MissionRig::height, sx, sy))
				continue;
			CanvasInput moved = MissionRig::at(sx, sy);
			moved.down = true;
			rig.step(moved);
			if (step == 3) {
				const OverlayList shapes = rig.canvas.shapes(rig.context(), moved);
				pivot_drawn = std::any_of(shapes.shapes.begin(), shapes.shapes.end(), [](const OverlayShape &shape) {
					return shape.kind == OverlayKind::Marker && shape.glyph == OverlayGlyph::Cross && shape.role == OverlayRole::Selected;
				});
			}
		}
		rig.step(MissionRig::at(px, py));
		TEST_EXPECT(rig.serve(rig.out.take()) > 0);
		const MissionEntityMark *now_a = rig.entity(primary.record), *now_b = rig.entity(other.record);
		TEST_EXPECT(now_a && now_b && pivot_drawn);
		if (!now_a || !now_b) return 1;
		const int expect = (a.yaw + 90) % 360;
		TEST_EXPECT(std::abs(now_a->yaw - expect) <= 1 || std::abs(now_a->yaw - expect) >= 359);
		const double ae = a.x - pivot[0], an = a.y - pivot[1], be = b.x - pivot[0], bn = b.y - pivot[1];
		TEST_EXPECT(std::fabs(now_a->x - (pivot[0] + an)) < 0.05 * std::max(1.0, std::hypot(ae, an)) &&
				std::fabs(now_a->y - (pivot[1] - ae)) < 0.05 * std::max(1.0, std::hypot(ae, an)));
		TEST_EXPECT(std::fabs(now_b->x - (pivot[0] + bn)) < 0.05 * std::max(1.0, std::hypot(be, bn)) &&
				std::fabs(now_b->y - (pivot[1] - be)) < 0.05 * std::max(1.0, std::hypot(be, bn)));
		turned = true;
	}
	TEST_EXPECT(turned);
	TEST_EXPECT(!rig.out.unexpected);
	std::printf("test_mission_group_turn passed\n");
	return 0;
}

// The line under the picture's words (S15 review): the arrows nudge a metre when the snap is free; the
// Area box says the toolbar's grid, which it takes whatever is held.
int test_mission_hint_words() {
	MissionHintInput in;
	in.selected = 1;
	in.grid = 0.0f;
	TEST_EXPECT(mission_canvas_hint(in).find("arrows nudge (1 m") != std::string::npos);
	in.grid = 5.0f;
	TEST_EXPECT(mission_canvas_hint(in).find("arrows nudge (5 m") != std::string::npos);
	in.tool = MissionTool::Area;
	in.snap = 0.0f; // Ctrl held
	TEST_EXPECT(mission_canvas_hint(in).find("snaps to 5 m") != std::string::npos);
	std::printf("test_mission_hint_words passed\n");
	return 0;
}

// Over a device that answers a ground (z = 3 + x / 10; S14 review M2): a move of an entity with
// Stick keeps its height over that ground where it goes, the arrows' nudge too, and an area's mark
// stands on the ground at its middle (not at its z_min), where a click selects it.
int test_mission_ground_on_canvas() {
	GroundDevice ground;
	ground.ground = [](double x, double) { return 3.0 + x / 10.0; };
	MissionRig rig;
	rig.device = &ground;
	TEST_EXPECT(rig.open());
	const auto over = [&](const MissionEntityMark &entity) { return entity.z - ground.ground(entity.x, entity.y); };
	std::vector<int> picks = rig.pickable();
	TEST_EXPECT(!picks.empty());
	if (picks.empty()) return 1;
	MissionMark mark = rig.marks()[size_t(picks[0])];
	const double clearance = over(*rig.entity(mark.record)), x0 = rig.entity(mark.record)->x;
	// Ctrl taken as the drag goes (pressed after the press): free all the same.
	CanvasKeys free;
	free.ctrl = true;
	std::vector<Request> requests =
			rig.drag_holding(CanvasPoint{ mark.x, mark.y }, CanvasPoint{ mark.x + 60.0f, mark.y }, 3, free);
	TEST_EXPECT(rig.serve(requests) == requests.size());
	const MissionEntityMark *moved = rig.entity(mark.record);
	TEST_EXPECT(moved && std::fabs(moved->x - x0) > 1.0 && std::fabs(over(*moved) - clearance) < 1e-3);
	// The arrows: a metre east (or along the camera's axis), its height over the ground kept.
	const double before = over(*rig.entity(mark.record));
	rig.step(MissionRig::keys_at(1, 0, true));
	rig.step(MissionRig::keys_at(0, 0, false));
	TEST_EXPECT(rig.serve(rig.out.take()) >= 1 && std::fabs(over(*rig.entity(mark.record)) - before) < 1e-3);
	// An area's mark on the ground at its middle: a click there selects it (where nothing stands in
	// front of it).
	const std::vector<MissionMark> marks = rig.marks();
	bool area = false;
	for (size_t i = 0; i < marks.size(); ++i) {
		if (marks[i].area < 0 || !marks[i].shown) continue;
		const MissionAreaMark &held = rig.follow()->scene().areas()[size_t(marks[i].area)];
		const double mx = (held.min[0] + held.max[0]) * 0.5, my = (held.min[1] + held.max[1]) * 0.5;
		double anchor[3];
		preview_to_mission(marks[i].at, anchor);
		TEST_EXPECT(std::fabs(anchor[2] - ground.ground(mx, my)) < 1e-3);
		if (pick_mission_mark(marks, marks[i].x, marks[i].y) != int(i)) continue;
		requests = rig.click(marks[i].x, marks[i].y);
		TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Select && requests[0].record == marks[i].record);
		area = true;
		break;
	}
	TEST_EXPECT(area);
	TEST_EXPECT(!rig.out.unexpected);
	std::printf("test_mission_ground_on_canvas passed\n");
	return 0;
}

// Two mission positions within the 16.16 word a stored one lands on.
bool near_metres(double a, double b) { return std::fabs(a - b) < 1e-4; }

// An Alt-drag copies (S15): while it goes nothing is written and the copies show where they go (a
// ring each over the picture); let go, one duplicate command of what it took, by as far as it went,
// which the session plans as one batch (the copies made and moved, the originals where they stood),
// one undo step. Ctrl+D copies the selection a step to the camera's right (a metre east, looking
// north); Shift and an arrow nudge a tenth of a step.
int test_mission_copy() {
	MissionRig rig;
	TEST_EXPECT(rig.open());
	const std::vector<int> picks = rig.pickable();
	TEST_EXPECT(!picks.empty());
	if (picks.empty()) return 1;
	std::vector<MissionMark> marks = rig.marks();
	const NodeAddress a = marks[size_t(picks[0])].record;
	const double ax = rig.entity(a)->x, ay = rig.entity(a)->y;
	const size_t entities = rig.follow()->scene().entities().size();
	const std::string before = rig.document->serialize().text;
	// Alt-dragged 40 pixels across: selected first, nothing written while it goes.
	const CanvasPoint from{ marks[size_t(picks[0])].x, marks[size_t(picks[0])].y };
	CanvasKeys alt;
	alt.alt = true;
	CanvasInput in = MissionRig::at(from.x, from.y, alt);
	in.pressed = in.down = true;
	rig.step(in);
	CanvasInput moved = MissionRig::at(from.x + 40.0f, from.y, alt);
	moved.down = true;
	moved.delta = CanvasPoint{ 40.0f, 0.0f };
	rig.step(moved);
	std::vector<Request> requests = rig.out.take();
	TEST_EXPECT(count_of(requests, Request::Kind::Edits) == 0 && count_of(requests, Request::Kind::Select) == 1);
	TEST_EXPECT(rig.serve(requests) == requests.size());
	{
		rig.step(moved);
		const OverlayList shapes = rig.canvas.shapes(rig.context(), moved);
		const size_t hover_rings = size_t(std::count_if(shapes.shapes.begin(), shapes.shapes.end(), [](const OverlayShape &shape) {
			return shape.kind == OverlayKind::Circle && shape.role == OverlayRole::Hover;
		}));
		TEST_EXPECT(hover_rings >= 1);
		TEST_EXPECT(rig.canvas.hint(rig.context(), moved).find("copies") != std::string::npos);
	}
	CanvasInput up = MissionRig::at(from.x + 40.0f, from.y, alt);
	rig.step(up);
	requests = rig.out.take();
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Command && requests[0].command.name == "duplicate" &&
			requests[0].command.ids == std::vector<NodeId>({ a.row }) && requests[0].command.by.size() == 2);
	if (requests.size() != 1 || requests[0].command.by.size() != 2) return 1;
	const double east = requests[0].command.by[0];
	TEST_EXPECT(east > 1.0 && std::fabs(requests[0].command.by[1]) < 1e-3);
	TEST_EXPECT(rig.serve(requests) == 1);
	TEST_EXPECT(rig.follow()->scene().entities().size() == entities + 1 && near_metres(rig.entity(a)->x, ax));
	const MissionEntityMark *copy = rig.entity(rig.view.documents.selection.primary);
	TEST_EXPECT(copy && copy->row != a.row && near_metres(copy->x, ax + east) && near_metres(copy->y, ay));
	rig.session.handle(request::undo(rig.path));
	TEST_EXPECT(rig.session.outcome().done() && rig.document->serialize().text == before);
	// Ctrl+D: a metre to the camera's right (east, looking north).
	rig.session.handle(request::select_record(rig.path, a));
	CanvasInput keys = MissionRig::keys_at(0, 0, false);
	keys.keyboard.duplicate = true;
	rig.step(keys);
	requests = rig.out.take();
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Command && requests[0].command.ids.empty() &&
			requests[0].command.by == std::vector<double>({ 1.0, 0.0 }));
	TEST_EXPECT(rig.serve(requests) == 1);
	copy = rig.entity(rig.view.documents.selection.primary);
	TEST_EXPECT(copy && copy->row != a.row && near_metres(copy->x, ax + 1.0));
	rig.session.handle(request::undo(rig.path));
	// Shift and the right arrow: a tenth of the step.
	rig.session.handle(request::select_record(rig.path, a));
	CanvasInput fine = MissionRig::keys_at(1, 0, true);
	fine.keys.shift = true;
	rig.step(fine);
	rig.step(MissionRig::keys_at(0, 0, false));
	TEST_EXPECT(rig.serve(rig.out.take()) >= 1 && near_metres(rig.entity(a)->x, ax + 0.1));
	TEST_EXPECT(!rig.out.unexpected);
	std::printf("test_mission_copy passed\n");
	return 0;
}

// The tools (S15): Area, a box dragged on the ground is one box drop of an area at the box's corners
// (served, an area more), a click nothing; Path, a click is a drop of the path's next stop; each tool's
// hint says what a click does, and Esc under a tool selects nothing (the view goes back to Select). The
// primary's yaw handle under the pointer: the cursor a move, its words and step beside it, the hint
// naming it; the labels option over many marks: no two labels' boxes overlap.
int test_mission_tools() {
	MissionRig rig;
	TEST_EXPECT(rig.open());
	const size_t areas = rig.follow()->scene().areas().size();
	TEST_EXPECT(rig.tool(R"({"tool": "area"})"));
	TEST_EXPECT(rig.canvas.tool() == MissionTool::Area);
	TEST_EXPECT(rig.canvas.hint(rig.context(), MissionRig::at(100.0f, 100.0f)).find("Area: drag a box") == 0);
	std::vector<Request> requests = rig.drag(CanvasPoint{ 200.0f, 200.0f }, CanvasPoint{ 300.0f, 260.0f }, 3);
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Drop && requests[0].drop.reference == "area" &&
			requests[0].drop.box && requests[0].drop.x == 200.0f && requests[0].drop.x2 == 300.0f && requests[0].drop.y2 == 260.0f);
	TEST_EXPECT(rig.serve(requests) == 1 && rig.follow()->scene().areas().size() == areas + 1);
	TEST_EXPECT(rig.click(250.0f, 250.0f).empty());
	// Esc while a press is down cancels it (S15 review): a press, a drag, Esc, let go, what each frame
	// raised served before the next (as the Shell serves it): no box drop, the tool kept (the view leaves
	// a tool only on an Esc with no press down). The requests raised and how many were done.
	size_t done = 0;
	const auto escape_mid_drag = [&](CanvasPoint from, CanvasPoint to, CanvasKeys keys) {
		std::vector<Request> all;
		done = 0;
		const auto frame = [&](const CanvasInput &in) {
			rig.step(in);
			std::vector<Request> raised = rig.out.take();
			done += rig.serve(raised);
			all.insert(all.end(), raised.begin(), raised.end());
		};
		CanvasInput in = MissionRig::at(from.x, from.y, keys);
		in.pressed = in.down = true;
		frame(in);
		CanvasInput moved = MissionRig::at(to.x, to.y, keys);
		moved.down = true;
		moved.delta = CanvasPoint{ to.x - from.x, to.y - from.y };
		frame(moved);
		CanvasInput escape = MissionRig::at(to.x, to.y, keys);
		escape.down = true;
		escape.keyboard.focused = true;
		escape.keyboard.escape = true;
		frame(escape);
		frame(MissionRig::at(to.x, to.y, keys));
		return all;
	};
	requests = escape_mid_drag(CanvasPoint{ 200.0f, 200.0f }, CanvasPoint{ 300.0f, 260.0f }, CanvasKeys());
	TEST_EXPECT(count_of(requests, Request::Kind::Drop) == 0 && rig.canvas.tool() == MissionTool::Area &&
			rig.follow()->scene().areas().size() == areas + 1);
	// Path: a click is the path's next stop.
	TEST_EXPECT(rig.tool(R"({"tool": "path", "path": 1})"));
	TEST_EXPECT(rig.canvas.tool() == MissionTool::Path);
	TEST_EXPECT(rig.canvas.hint(rig.context(), MissionRig::at(100.0f, 100.0f)).find("Path 1: click") == 0);
	requests = rig.click(320.0f, 240.0f);
	TEST_EXPECT(requests.size() == 1 && requests[0].kind == Request::Kind::Drop && requests[0].drop.reference == "path" &&
			requests[0].drop.name == "1");
	// Esc under a tool: no selection raised.
	const std::vector<int> picks = rig.pickable();
	if (!picks.empty()) rig.session.handle(request::select_record(rig.path, rig.marks()[size_t(picks[0])].record));
	CanvasInput escape = MissionRig::keys_at(0, 0, false);
	escape.keyboard.escape = true;
	rig.step(escape);
	TEST_EXPECT(rig.out.take().empty());
	TEST_EXPECT(rig.tool(R"({"tool": "select"})"));
	TEST_EXPECT(rig.canvas.tool() == MissionTool::Select);
	// Esc mid-move: what the drag wrote put back where the press found it (served, the mark stands where
	// it stood); Esc mid-Alt-drag: no copies.
	if (!picks.empty()) {
		const MissionMark mark = rig.marks()[size_t(picks[0])];
		const double x0 = rig.entity(mark.record)->x, y0 = rig.entity(mark.record)->y;
		requests = escape_mid_drag(CanvasPoint{ mark.x, mark.y }, CanvasPoint{ mark.x + 50.0f, mark.y + 10.0f }, CanvasKeys());
		TEST_EXPECT(count_of(requests, Request::Kind::Edits) >= 2 && done == requests.size());
		TEST_EXPECT(rig.entity(mark.record)->x == x0 && rig.entity(mark.record)->y == y0);
		CanvasKeys alt;
		alt.alt = true;
		const size_t entities = rig.follow()->scene().entities().size();
		const MissionMark again = rig.marks()[size_t(picks[0])];
		requests = escape_mid_drag(CanvasPoint{ again.x, again.y }, CanvasPoint{ again.x + 50.0f, again.y }, alt);
		TEST_EXPECT(count_of(requests, Request::Kind::Command) == 0 && count_of(requests, Request::Kind::Edits) == 0);
		TEST_EXPECT(rig.follow()->scene().entities().size() == entities);
	}
	// The primary's yaw handle under the pointer.
	bool found = false;
	const MissionViewport *viewport = rig.follow();
	for (const int pick : picks) {
		const MissionMark mark = rig.marks()[size_t(pick)];
		rig.session.handle(request::select_record(rig.path, mark.record));
		PreviewVec3 at;
		float hx = 0.0f, hy = 0.0f;
		const std::vector<MissionMark> now = rig.marks();
		if (!viewport->handle_at(now[size_t(pick)], MissionHandle::Yaw, at) ||
				!viewport->camera().project(at, MissionRig::width, MissionRig::height, hx, hy) || pick_mission_mark(now, hx, hy) >= 0)
			continue;
		const CanvasInput over = MissionRig::at(hx, hy);
		rig.step(over);
		TEST_EXPECT(rig.canvas.cursor(rig.context(), over) == CanvasCursor::Move);
		TEST_EXPECT(rig.canvas.hint(rig.context(), over).find("Handle: drag to turn") == 0);
		const OverlayList shapes = rig.canvas.shapes(rig.context(), over);
		TEST_EXPECT(std::any_of(shapes.shapes.begin(), shapes.shapes.end(), [](const OverlayShape &shape) {
			return shape.kind == OverlayKind::Text && shape.text.find("Turn (") == 0;
		}));
		found = true;
		break;
	}
	TEST_EXPECT(found);
	// The labels option: no two labels overlap (each a line of 7-pixel characters, 15 high).
	rig.session.handle(request::set_viewport(rig.path, R"({"kind": "mission", "options": {"marks": {"labels": true}}})"));
	const CanvasInput away = MissionRig::at(1.0f, 1.0f);
	rig.step(away);
	const OverlayList shapes = rig.canvas.shapes(rig.context(), away);
	std::vector<const OverlayShape *> labels;
	for (const OverlayShape &shape : shapes.shapes)
		if (shape.kind == OverlayKind::Text) labels.push_back(&shape);
	TEST_EXPECT(!labels.empty());
	bool apart = true;
	for (size_t i = 0; i < labels.size(); ++i)
		for (size_t j = i + 1; j < labels.size(); ++j) {
			const OverlayShape &p = *labels[i], &q = *labels[j];
			const float pw = float(p.text.size()) * kMissionLabelCharWidth, qw = float(q.text.size()) * kMissionLabelCharWidth;
			if (p.points[0].x < q.points[0].x + qw && q.points[0].x < p.points[0].x + pw &&
					p.points[0].y < q.points[0].y + kMissionLabelHeight && q.points[0].y < p.points[0].y + kMissionLabelHeight)
				apart = false;
		}
	TEST_EXPECT(apart);
	TEST_EXPECT(!rig.out.unexpected);
	std::printf("test_mission_tools passed\n");
	return 0;
}

} // namespace

int main() {
	if (test_camera_ray_and_text() != 0)
		return 1;
	if (test_gesture_machine() != 0)
		return 1;
	if (test_menu_canvas() != 0)
		return 1;
	if (test_menu_canvas_held() != 0)
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
	if (test_mission_canvas() != 0)
		return 1;
	if (test_mission_handle_tap_and_turn() != 0)
		return 1;
	if (test_mission_group_turn() != 0)
		return 1;
	if (test_mission_hint_words() != 0)
		return 1;
	if (test_mission_ground_on_canvas() != 0)
		return 1;
	if (test_mission_copy() != 0)
		return 1;
	if (test_mission_tools() != 0)
		return 1;
	if (test_mission_marquee() != 0)
		return 1;
	if (test_mission_camera() != 0)
		return 1;
	if (test_mission_nudge() != 0)
		return 1;
	if (test_mission_delete() != 0)
		return 1;
	if (test_mission_place() != 0)
		return 1;
	std::printf("editor_canvas: all tests passed\n");
	return 0;
}
