// S13 V5 (ADR 0046 S13): the viewport seam, over devices that draw nothing (viewport_test_support.h)
// and a real session. What a device takes as its viewport follows the session: the picture made when
// a device attaches, then Keep while nothing moves; Rebuild for an edit of a menu, a file its picture
// read moving its stamp, a menu's options; Update for a model's options and an edit only its overlays
// show; Clear for a document the game could not read, kept so until it changes (the failure latch);
// what changed in the document (ChangeClass None, Changed, Unknown) read once a follow. The one
// preview clock. Two menus keep their own options; the device cache gives up its least recently used
// device (four held), its viewport keeping its state (a camera) for the next, which makes the picture
// again. The envelope and its pages. A SetViewport, a drag and a command through a real session, the
// refused answering false. One undo step per gesture through a real session: a menu drag of four
// samples, a drag of three selected windows moving all three, a gesture its canvas ends by not
// drawing, a model marker's drag (two selected markers moving as one). S13 V7: a SetViewport with no
// path the active document's; a hit naming its viewport. An edit in a viewport over the wire
// (edit_in_viewport) is viewport_wire_test.cpp's. S13 V6: devices that build their pictures over
// frames (loading, then ready; a newer Rebuild cancelling one by generation; an Update folded into a
// build; a Clear and a failure mid-build; the frame's budget, shared by the builds in flight), and the
// follow's comparison of a file a build read between two follows.
//
// S13 V8, change sets: a menu's edit of a later screen is Keep (no configure), of the shown screen's
// window a Rebuild; a screen before the shown one feeds it (a texture's first load fixes the band a
// later screen draws), as do the screens reordered; a stylesheet variable the screens up to the shown
// one name a Rebuild, one only a later screen names a Keep; the menu read again "all" (Unknown); a user
// point moved an Update over the scene that stands (a patch, no read); a menu drag configured again
// live, a model's scene held while a gesture is open in its document and built once at its end, a
// hold only over a picture the device holds; a model marker's drag an Update a sample, never a
// Rebuild; the menu's caret on the preview clock with no configure, the device's frame drawn again
// only as the caret's half of the blink changes (and, with OPENNOVA_JO_DIR, mp.mnu's GAME_NAME box
// blinking its caret on it).

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <base/io/json.h>
#include <base/vfs/vfs.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/mnu_table.h>
#include <editor/documents/model_document.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/canvas_half.h>
#include <editor/preview/menu_viewport.h>
#include <editor/preview/model_overlay.h>
#include <editor/preview/model_viewport.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/viewport_json.h>
#include <editor/preview/viewport_model.h>
#include <editor/preview/viewports.h>
#include <editor/session/editor_queries.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

using editor_test::FakeDevice;
using editor_test::FakeDevices;
using editor_test::NoProcess;
using Actions = std::vector<ViewportAction>;

std::string synth(const char *name) {
	return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/synth/" + name;
}

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

// Two screens, each naming its own stylesheet variables: FIRST's labels DEF_TEXT_FG (MAIN's font),
// SECOND's TRIM_COLOR; FIRST holds an edit box, EDIT, whose caret blinks once it is focused.
const char *const kTwoScreens = "<SCREEN>\r\n"
								"\t<NAME>FIRST</NAME>\r\n"
								"\t<WINDOW type=\"window\" name=\"MAIN\">\r\n"
								"\t\t<POSITION><LEFT>0</LEFT><TOP>0</TOP>"
								"<RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>\r\n"
								"\t\t<FONT><NAME>%DEF_FONTNAME_LG%</NAME><DEFAULT_FG>%DEF_TEXT_FG%</DEFAULT_FG></FONT>\r\n"
								"\t\t<WINDOW type=\"static\" name=\"LABEL\">\r\n"
								"\t\t\t<POSITION><LEFT>100</LEFT><TOP>100</TOP>"
								"<RIGHT>300</RIGHT><BOTTOM>140</BOTTOM></POSITION>\r\n"
								"\t\t\t<STRING>First</STRING>\r\n"
								"\t\t</WINDOW>\r\n"
								"\t\t<WINDOW type=\"edit\" name=\"EDIT\">\r\n"
								"\t\t\t<POSITION><LEFT>100</LEFT><TOP>200</TOP>"
								"<RIGHT>400</RIGHT><BOTTOM>240</BOTTOM></POSITION>\r\n"
								"\t\t</WINDOW>\r\n"
								"\t</WINDOW>\r\n"
								"</SCREEN>\r\n"
								"<SCREEN>\r\n"
								"\t<NAME>SECOND</NAME>\r\n"
								"\t<WINDOW type=\"window\" name=\"MAIN\">\r\n"
								"\t\t<POSITION><LEFT>0</LEFT><TOP>0</TOP>"
								"<RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>\r\n"
								"\t\t<FONT><NAME>%DEF_FONTNAME%</NAME><DEFAULT_FG>%TRIM_COLOR%</DEFAULT_FG></FONT>\r\n"
								"\t\t<WINDOW type=\"static\" name=\"NOTE\">\r\n"
								"\t\t\t<POSITION><LEFT>100</LEFT><TOP>100</TOP>"
								"<RIGHT>300</RIGHT><BOTTOM>140</BOTTOM></POSITION>\r\n"
								"\t\t\t<STRING>Second</STRING>\r\n"
								"\t\t</WINDOW>\r\n"
								"\t</WINDOW>\r\n"
								"</SCREEN>\r\n";

// A screen of a window showing sheet.tga (64 x 80) through an IMAGE row of its own HEIGHT, its MAIN's
// font colour the stylesheet variable `colour` names ("" none). Three make the shared-texture menu:
// FIRST's 20 (DEF_TEXT_FG), SECOND's 24, THIRD's 40 (TRIM_COLOR). The first load of the texture fixes
// the band every later user of it draws, across the screens (menu-re.md "The IMAGE pass"): FIRST's 20
// is SECOND's band.
std::string image_screen(const char *screen, const char *window, int height, const char *colour = "") {
	const std::string font = *colour ? std::string("\t\t<FONT><NAME>%DEF_FONTNAME%</NAME><DEFAULT_FG>%") + colour +
					"%</DEFAULT_FG></FONT>\r\n"
									 : std::string();
	return std::string("<SCREEN>\r\n\t<NAME>") + screen + "</NAME>\r\n\t<WINDOW type=\"window\" name=\"MAIN\">\r\n"
			"\t\t<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>\r\n" +
			font + "\t\t<WINDOW type=\"static\" name=\"" + window + "\">\r\n"
			"\t\t\t<POSITION><LEFT>100</LEFT><TOP>100</TOP><RIGHT>164</RIGHT><BOTTOM>124</BOTTOM></POSITION>\r\n"
			"\t\t\t<APPEARANCE STATE=\"DEFAULT\" TYPE=\"IMAGE\" HEIGHT=\"" + std::to_string(height) +
			"\">sheet.tga</APPEARANCE>\r\n\t\t</WINDOW>\r\n\t</WINDOW>\r\n</SCREEN>\r\n";
}

// An uncompressed 32-bit TGA, `width` x `height`, one colour.
std::vector<uint8_t> tga(int width, int height) {
	std::vector<uint8_t> bytes(18, 0);
	bytes[2] = 2;
	bytes[12] = uint8_t(width & 0xFF);
	bytes[13] = uint8_t(width >> 8);
	bytes[14] = uint8_t(height & 0xFF);
	bytes[15] = uint8_t(height >> 8);
	bytes[16] = 32;
	bytes[17] = 0x28;
	for (int i = 0; i < width * height; ++i) bytes.insert(bytes.end(), { 40, 200, 255, 255 });
	return bytes;
}

JsonValue parse(const char *text) {
	JsonValue json;
	std::string error;
	opennova::io::json_parse(text, json, error);
	return json;
}

void set(ProjectSession &session, const Document &document, const NodeAddress &address, const char *field,
		Value value) {
	EditorRequest request = request::edit_record(document.path(), Edit());
	request.edits[0].address = address;
	request.edits[0].field = field;
	request.edits[0].value = std::move(value);
	session.handle(request);
}

// A whole-number field of a record (-1: none).
int64_t field_of(const Document &document, const NodeAddress &address, const char *field) {
	Value value;
	return document.get(address, field, value) && std::holds_alternative<int64_t>(value) ? std::get<int64_t>(value)
																						  : -1;
}

NodeAddress named(const Document &document, const char *name) {
	NodeAddress address;
	find_definition(AssetGraph(), document, name, address);
	return address;
}

// A window's four edges: left, top, right, bottom.
std::vector<int64_t> edges(const Document &document, const NodeAddress &window) {
	return { field_of(document, window, "position.left"), field_of(document, window, "position.top"),
		field_of(document, window, "position.right"), field_of(document, window, "position.bottom") };
}

template <typename Kind>
const Kind *viewport_of(ProjectSession &session, const std::string &path, ViewportKind kind) {
	return static_cast<const Kind *>(session.viewports().find(path, kind));
}

// A canvas's requests served as they are raised, as the windows raise theirs; what was raised kept,
// and whether the session refused one.
struct Served final : CanvasRequests {
	ProjectSession &session;
	std::vector<EditorRequest> raised;
	bool refused = false;
	explicit Served(ProjectSession &owner) : session(owner) {}
	void request(EditorRequest request) override {
		session.handle(request);
		refused = refused || !session.outcome().done();
		raised.push_back(std::move(request));
	}
	size_t count(EditorRequestKind kind) const {
		size_t n = 0;
		for (const EditorRequest &request : raised) n += request.kind == kind ? 1 : 0;
		return n;
	}
	// Every batch carries one gesture's token (0: none raised, or not one).
	uint64_t gesture() const {
		uint64_t token = 0;
		for (const EditorRequest &request : raised)
			for (const Edit &edit : request.edits) {
				if (request.kind != EditorRequestKind::EditRecord) continue;
				if (!edit.gesture || (token && edit.gesture != token)) return 0;
				token = edit.gesture;
			}
		return token;
	}
};

// A canvas over a viewport of a real session, a frame at a time as its view draws it: the Shell's
// pump (its devices following the session), then the kind's half of the canvas over the viewport at
// the canvas's size, every request served as it is raised; then the frame's bracket.
struct CanvasRig {
	ProjectSession &session;
	FakeDevices &devices;
	std::string path;
	ViewportKind kind;
	// The grid a drag snaps to, as the kind's toolbar sets it (a menu's Snap on: 1; a model's metres).
	float snap = 0.0f;
	int width = 800;
	int height = 600;
	std::unique_ptr<CanvasHalf> half;
	Served out{ session };

	const ViewportModel *viewport() { return session.viewports().find(path, kind); }
	CanvasInput at(float x, float y) const {
		CanvasInput in;
		in.width = width;
		in.height = height;
		in.mouse = CanvasPoint{ x, y };
		in.screen = CanvasPoint{ x, y };
		in.hovered = true;
		return in;
	}
	// A frame the canvas draws.
	void frame(const CanvasInput &in) {
		devices.sync(session);
		const ViewportModel *model = viewport();
		if (!model) return;
		if (!half) half = model->make_canvas();
		ViewportContext context = viewport_context(session.view(), *model, snap);
		context.width = in.width;
		context.height = in.height;
		half->follow(*model, context, out);
		half->input(context, in, out);
		half->end_frame(out);
	}
	// A frame it does not draw (its window hidden, its tab not shown).
	void hidden() {
		devices.sync(session);
		if (half) half->end_frame(out);
	}
	// A press at (x, y), four samples to (x + dx, y + dy), let go there.
	void drag(float x, float y, float dx, float dy, CanvasKeys keys = CanvasKeys()) {
		CanvasInput in = at(x, y);
		in.keys = keys;
		in.pressed = in.down = true;
		frame(in);
		for (int i = 1; i <= 4; ++i) {
			in = at(x + dx * float(i) / 4.0f, y + dy * float(i) / 4.0f);
			in.keys = keys;
			in.down = true;
			frame(in);
		}
		in.down = false;
		frame(in);
	}
};

// A viewport of the model's kind (its row holds for a gesture) whose follow answers `next`: the hold's
// rule over the actions it merges, with no document behind it.
class ScriptedViewport final : public ViewportModel {
public:
	ScriptedViewport() : ViewportModel(ViewportKind::Model, "models/scripted.3di", ViewportState{ 64, 64 }) {}
	ViewportAction next = ViewportAction::Keep;
	ViewportStatus status() const override { return ViewportStatus::Ready; }
	const char *reason() const override { return "ready"; }
	std::string message() const override { return std::string(); }
	const std::string &detail() const override { return detail_; }
	const char *units() const override { return "pixels"; }
	ViewportLayout layout() const override { return ViewportLayout(); }
	std::unique_ptr<CanvasHalf> make_canvas() const override { return nullptr; }
	ViewportHit hit(const ViewportContext &, float, float) const override { return ViewportHit(); }
	bool handle_point(const ViewportContext &, NodeId, const std::string &, float &, float &,
			std::string &) const override {
		return false;
	}
	bool drag(const ViewportContext &, const ViewportDrag &, CanvasRequests &, std::string &) const override {
		return false;
	}
	bool command(const ViewportContext &, const std::string &, const std::vector<NodeId> &, CanvasRequests &,
			std::string &) const override {
		return false;
	}
	JsonValue options_json() const override { return JsonValue::make_object(); }
	JsonValue body_json(const ViewportInput &) const override { return JsonValue::make_object(); }
	JsonValue items_json(const ViewportInput &) const override { return JsonValue::make_array(); }

protected:
	ViewportAction follow_(const ViewportInput &, PreviewClock &) override { return next; }
	bool takes_(const std::string &) const override { return false; }
	bool check_(const JsonValue &, std::string &) const override { return true; }
	void apply_(const JsonValue &, PreviewClock &) override {}

private:
	std::string detail_;
};

} // namespace

// What a device takes as its viewport follows the session.
static int test_actions() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_actions");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	FakeDevices devices;
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Viewport Test"));
	session.run_operations();
	editor_test::create_missing_files(session);

	// A menu opened: its viewport made as the Preview's target names it, no device until the Shell's
	// pump attaches one, whose first action makes the picture; then nothing moved: Keep.
	session.handle(request::open_document("main.mnu"));
	Document *menu = session.document_for("main.mnu");
	const std::string path = menu ? menu->path() : std::string();
	const auto *shown = viewport_of<MenuViewport>(session, path, ViewportKind::Menu);
	TEST_EXPECT(menu && shown && !shown->attached() && shown->builds() == 0);
	if (!menu || !shown) return 1;
	devices.sync(session);
	FakeDevice *device = devices.held(path, ViewportKind::Menu);
	TEST_EXPECT(device && device->taken == (Actions{ ViewportAction::Rebuild }));
	if (!device) return 1;
	TEST_EXPECT(shown->attached() && shown->builds() == 1 && shown->configures() == 1);
	devices.sync(session);
	TEST_EXPECT(device->last() == ViewportAction::Keep && shown->configures() == 1);

	// An edit (ChangeClass Unknown): the screen configured again.
	const NodeAddress title = named(*menu, "TITLE"), exit = named(*menu, "EXIT");
	TEST_EXPECT(title.child && exit.child);
	set(session, *menu, title, "position.left", int64_t(8));
	size_t from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(device->since(from) == (Actions{ ViewportAction::Rebuild }) && shown->configures() == 2);

	// The device's size, the viewport's state: nothing made again (the device sizes itself to it).
	session.handle(request::set_viewport(path, R"({"device": {"width": 640, "height": 480}})"));
	TEST_EXPECT(session.outcome().done());
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(device->since(from).empty() && shown->state().width == 640 && shown->state().height == 480);
	TEST_EXPECT(shown->size().width == 640 && shown->size().height == 480 && !shown->canvas_sized());
	// A canvas drawing it at a size of its own (a picture fitted to the room it has): the device
	// reports that size, and a SetViewport of the device's size is refused, naming why; a pump with
	// no canvas drawing it, it is the state's size again and the SetViewport applies.
	ViewportPicture fitted;
	fitted.width = 720;
	fitted.height = 540;
	fitted.canvas_sized = true;
	device->draw(fitted);
	devices.sync(session);
	TEST_EXPECT(shown->canvas_sized() && shown->size().width == 720 && shown->size().height == 540 &&
			shown->state().width == 640);
	session.handle(request::set_viewport(path, R"({"device": {"width": 1024, "height": 768}})"));
	TEST_EXPECT(!session.outcome().done() && shown->state().width == 640 && !session.outcome().findings.empty() &&
			session.outcome().findings.front().message.find("canvas") != std::string::npos);
	// Drawn at the state's size (a menu at its Device size): not a canvas's own size.
	fitted.width = 640;
	fitted.height = 480;
	fitted.canvas_sized = false;
	device->draw(fitted);
	devices.sync(session);
	TEST_EXPECT(!shown->canvas_sized() && shown->size().width == 640);
	session.handle(request::set_viewport(path, R"({"device": {"width": 1024, "height": 768}})"));
	TEST_EXPECT(session.outcome().done() && shown->state().width == 1024);
	session.handle(request::set_viewport(path, R"({"device": {"width": 640, "height": 480}})"));
	TEST_EXPECT(session.outcome().done());
	devices.sync(session);
	TEST_EXPECT(shown->size().width == 640 && shown->size().height == 480 && !shown->canvas_sized());
	// Its options: the screen configured again; the same options again, nothing.
	session.handle(request::set_viewport(path, R"({"options": {"show_hidden": true}})"));
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(device->since(from) == (Actions{ ViewportAction::Rebuild }) && shown->options().show_hidden);
	session.handle(request::set_viewport(path, R"({"kind": "menu", "options": {"show_hidden": true}})"));
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(device->since(from).empty() && shown->configures() == 3);

	// A file its picture read moved its stamp (the stylesheet, not open, written outside the
	// editor): a line added that changes no variable leaves the picture as it is (S13 V8); a variable
	// the screen names (DEF_TEXT_FG, its labels' colour) of another value configures the screen again.
	const AssetEntry *style_file = view.project.scan->find("menu_style.mns");
	TEST_EXPECT(style_file != nullptr);
	if (!style_file) return 1;
	const std::string style = view.project.root + "/" + style_file->relative_path;
	std::vector<uint8_t> bytes = test_io::read_file(style);
	TEST_EXPECT(!bytes.empty());
	bytes.push_back('\r');
	bytes.push_back('\n');
	TEST_EXPECT(editor_test::write_bytes(style, bytes));
	session.handle(request::rescan());
	session.run_operations();
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(device->since(from).empty() && shown->configures() == 3);
	std::string sheet(bytes.begin(), bytes.end());
	const size_t white = sheet.find("FFFFFFFF");
	TEST_EXPECT(white != std::string::npos && sheet.find("DEF_TEXT_FG") < white);
	sheet.replace(white, 8, "FF00FF00");
	sheet += "\r\n"; // another size: the scan reads the file again whatever its clock's tick
	TEST_EXPECT(editor_test::write_text(style, sheet));
	session.handle(request::rescan());
	session.run_operations();
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(device->since(from) == (Actions{ ViewportAction::Rebuild }) && shown->configures() == 4);

	// A menu the game could not read: Clear, and kept so, no retry each pump (the failure latch),
	// until it changes; undone, the picture made again.
	set(session, *menu, exit, "string.justify", std::string("CEN\"TER"));
	from = device->taken.size();
	devices.sync(session);
	devices.sync(session);
	TEST_EXPECT(device->since(from) == (Actions{ ViewportAction::Clear }) && shown->status() == ViewportStatus::Failed);
	TEST_EXPECT(shown->configures() == 5);
	session.handle(request::undo(menu->path()));
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(device->since(from) == (Actions{ ViewportAction::Rebuild }) && shown->status() == ViewportStatus::Ready);

	// A model: its options are an Update (the picture stands, the state applied again), as is an
	// edit only its overlays show.
	TEST_EXPECT(editor_test::write_bytes(view.project.root + "/models/armory.3di", test_io::read_file(synth("armory.3di"))));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("models/armory.3di"));
	auto *document = dynamic_cast<ModelDocument *>(session.document_for("models/armory.3di"));
	devices.sync(session);
	FakeDevice *model_device = devices.held("models/armory.3di", ViewportKind::Model);
	const auto *model = viewport_of<ModelViewport>(session, "models/armory.3di", ViewportKind::Model);
	TEST_EXPECT(document && model && model_device && model_device->taken == (Actions{ ViewportAction::Rebuild }));
	if (!document || !model || !model_device) return 1;
	// The menu's device stays held, and follows (nothing moved in it).
	TEST_EXPECT(devices.held(path, ViewportKind::Menu) == device && device->last() == ViewportAction::Keep);
	session.handle(request::set_viewport("models/armory.3di", R"({"options": {"lod": 0}})"));
	devices.sync(session);
	TEST_EXPECT(model_device->since(1) == (Actions{ ViewportAction::Update }) && model->lod() == 0);

	// What changed in the document, read once a follow (S13 V8: the model's change set): nothing
	// (None) reads nothing; a user point's edit and its undo (Changed, the model row alike but for its
	// user points) patch the held model and read nothing, an Update each; a light's edit (Changed,
	// something drawn) reads it and builds; the document read again from its file (Unknown: it
	// cannot say what changed) reads it, and builds only when the drawn model moved.
	const uint64_t reads = model->reads();
	devices.sync(session);
	TEST_EXPECT(model->reads() == reads && model->followed_change() == ChangeClass::None);
	const ModelRow *row = document->model_row();
	const NodeAddress point{ row->id, node_kind(ModelKind::UserPoint), row->ids.lists[3][0].id };
	Value value;
	TEST_EXPECT(document->get(point, "position.x", value));
	set(session, *document, point, "position.x", std::get<double>(value) + 1.0);
	from = model_device->taken.size();
	devices.sync(session);
	TEST_EXPECT(model->followed_change() == ChangeClass::Changed);
	devices.sync(session);
	TEST_EXPECT(model->reads() == reads && model->patches() == 1 &&
			model_device->since(from) == (Actions{ ViewportAction::Update }));
	session.handle(request::undo(document->path()));
	from = model_device->taken.size();
	devices.sync(session);
	TEST_EXPECT(model->reads() == reads && model->patches() == 2 && !document->dirty() &&
			model_device->since(from) == (Actions{ ViewportAction::Update }));
	const NodeAddress light{ row->id, node_kind(ModelKind::Light), document->model_row()->ids.lists[2][0].id };
	set(session, *document, light, "start.r", int64_t(12));
	from = model_device->taken.size();
	devices.sync(session);
	TEST_EXPECT(model->reads() == reads + 1 && model_device->since(from) == (Actions{ ViewportAction::Rebuild }));
	session.handle(request::undo(document->path()));
	devices.sync(session);
	TEST_EXPECT(model->reads() == reads + 2 && !document->dirty());
	session.handle(request::reload_document("models/armory.3di"));
	session.run_operations();
	TEST_EXPECT(session.outcome().done());
	from = model_device->taken.size();
	devices.sync(session);
	TEST_EXPECT(model->reads() == reads + 3 && model->status() == ViewportStatus::Ready &&
			model->followed_change() == ChangeClass::Unknown &&
			model_device->since(from) == (Actions{ ViewportAction::Update }));
	std::printf("test_actions passed\n");
	return 0;
}

// The one preview clock: it runs while it plays, `rate` times as fast as the Shell's frames pass,
// its milliseconds wrapping as the game's and its ticks held at their largest; every viewport reads
// it, so a SetViewport's clock on one is every viewport's.
static int test_clock() {
	PreviewClock clock;
	TEST_EXPECT(clock.playing() && clock.rate() == 1.0 && clock.ms() == 0 && clock.ticks() == 0);
	clock.advance(0.5);
	TEST_EXPECT(clock.ms() == 500 && clock.ticks() == 31);
	clock.set_rate(2.0);
	clock.advance(0.25);
	TEST_EXPECT(clock.ms() == 1000 && clock.ticks() == 62);
	clock.set_playing(false);
	clock.advance(1.0);
	TEST_EXPECT(clock.ms() == 1000 && clock.ticks() == 62);
	clock.set_playing(true);
	clock.set_rate(-3.0);
	clock.advance(1.0);
	TEST_EXPECT(clock.rate() == 0.0 && clock.ms() == 1000);
	clock.set_rate(1.0);
	clock.seek_ms(UINT32_MAX);
	clock.advance(0.002);
	TEST_EXPECT(clock.ms() == 1);
	clock.seek_ticks(INT32_MAX - 1);
	clock.advance(1.0);
	TEST_EXPECT(clock.ticks() == INT32_MAX);
	clock.seek_ticks(-5);
	TEST_EXPECT(clock.ticks() == 0);

	editor_test::TempProjectDir dir("opennova_editor_viewport_clock");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Clock Test"));
	session.run_operations();
	editor_test::create_missing_files(session);
	TEST_EXPECT(editor_test::write_bytes(view.project.root + "/models/armory.3di", test_io::read_file(synth("armory.3di"))));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("main.mnu"));
	session.handle(request::open_document("models/armory.3di"));
	session.advance(0.25);
	TEST_EXPECT(session.viewports().clock().ms() == 250);
	// Paused through the menu's viewport: the model's envelope reads the same clock.
	session.handle(request::set_viewport(session.document_for("main.mnu")->path(),
			R"({"clock": {"playing": false, "time_ms": 40}})"));
	TEST_EXPECT(session.outcome().done());
	session.advance(1.0);
	const ViewportModel *armory = session.viewports().find("models/armory.3di", ViewportKind::Model);
	TEST_EXPECT(armory != nullptr);
	if (!armory) return 1;
	const JsonValue timing = *viewport_to_json(view, *armory, JsonPage()).get("clock");
	TEST_EXPECT(!timing.get_bool("playing", true) && timing.get_number("time_ms", 0) == 40.0);
	std::printf("test_clock passed\n");
	return 0;
}

// Two menus open, each its own viewport and its own options; the Preview back on the first shows
// its own, and no device is made again.
static int test_two_menus() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_menus");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	FakeDevices devices;
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Two Menus"));
	session.run_operations();
	editor_test::create_missing_files(session);
	session.handle(request::open_document("main.mnu"));
	devices.sync(session);
	session.handle(request::create_file("extra.mnu", "menu"));
	const Document *main_menu = session.document_for("main.mnu");
	const Document *extra_menu = session.document_for("extra.mnu");
	TEST_EXPECT(main_menu && extra_menu);
	if (!main_menu || !extra_menu) return 1;
	const std::string main_path = main_menu->path(), extra_path = extra_menu->path();
	TEST_EXPECT(view.documents.previews[ViewportKind::Menu].path == extra_path);
	devices.sync(session);
	TEST_EXPECT(devices.made.size() == 2 && devices.cache.size() == 2);
	session.handle(request::set_viewport(main_path, R"({"options": {"show_hidden": true}})"));
	session.handle(request::set_viewport(extra_path, R"({"device": {"width": 1024, "height": 768}})"));
	const size_t from = devices.held(main_path, ViewportKind::Menu)->taken.size();
	devices.sync(session);
	// The menu no longer the Preview's keeps its device, which takes what its options ask.
	TEST_EXPECT(devices.held(main_path, ViewportKind::Menu)->since(from) == (Actions{ ViewportAction::Rebuild }));
	const auto *main = viewport_of<MenuViewport>(session, main_path, ViewportKind::Menu);
	const auto *extra = viewport_of<MenuViewport>(session, extra_path, ViewportKind::Menu);
	TEST_EXPECT(main && extra && main != extra);
	TEST_EXPECT(main->options().show_hidden && !extra->options().show_hidden);
	TEST_EXPECT(main->state().width == 800 && extra->state().width == 1024 && extra->state().height == 768);
	TEST_EXPECT(viewport_to_json(view, *extra, JsonPage()).get("options")->get_bool("show_hidden", true) == false);
	session.handle(request::open_document("main.mnu"));
	devices.sync(session);
	TEST_EXPECT(view.documents.previews[ViewportKind::Menu].path == main_path && devices.made.size() == 2);
	TEST_EXPECT(viewport_to_json(view, *main, JsonPage()).get("options")->get_bool("show_hidden", false));
	std::printf("test_two_menus passed\n");
	return 0;
}

// The device cache holds four: a fifth given its device gives up the least recently used, whose
// viewport keeps its state (its camera) and holds nothing; the Preview back on it, a device is made
// that makes the picture again at the camera it kept. A canvas asking for a device has it at the
// next pump.
static int test_device_cache() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_cache");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	FakeDevices devices;
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Device Cache"));
	session.run_operations();
	const std::vector<uint8_t> armory = test_io::read_file(synth("armory.3di"));
	std::vector<std::string> paths;
	for (int i = 0; i < 5; ++i) {
		paths.push_back("models/m" + std::to_string(i) + ".3di");
		TEST_EXPECT(editor_test::write_bytes(view.project.root + "/" + paths.back(), armory));
	}
	session.handle(request::rescan());
	session.run_operations();
	for (size_t i = 0; i < paths.size(); ++i) {
		session.handle(request::open_document(paths[i]));
		devices.sync(session);
		if (i == 0) {
			session.handle(request::set_viewport(paths[0],
					R"({"camera": {"yaw": 1.25, "pitch": 0.5, "distance": 12, "target": [1, 2, 3]}})"));
			TEST_EXPECT(session.outcome().done());
			devices.sync(session);
		}
	}
	TEST_EXPECT(kViewportDeviceCapacity == 4 && devices.made.size() == 5 && devices.cache.size() == 4);
	const auto *first = viewport_of<ModelViewport>(session, paths[0], ViewportKind::Model);
	TEST_EXPECT(first && !devices.held(paths[0], ViewportKind::Model) && !first->attached());
	if (!first) return 1;
	TEST_EXPECT(first->camera().yaw == 1.25f && first->camera().distance == 12.0f && first->builds() == 1);
	for (size_t i = 1; i < paths.size(); ++i) TEST_EXPECT(devices.held(paths[i], ViewportKind::Model));

	// The Preview back on the first: a device made, which makes the picture at the camera kept; the
	// next least recently used (the second) given up.
	session.handle(request::open_document(paths[0]));
	devices.sync(session);
	TEST_EXPECT(devices.made.size() == 6 && devices.held(paths[0], ViewportKind::Model) == devices.made.back());
	TEST_EXPECT(devices.made.back()->taken == (Actions{ ViewportAction::Rebuild }) && first->attached() && first->builds() == 2);
	TEST_EXPECT(first->camera().yaw == 1.25f && first->camera().distance == 12.0f && first->camera().target.z == 3.0f);
	TEST_EXPECT(!devices.held(paths[1], ViewportKind::Model) &&
			!session.viewports().find(paths[1], ViewportKind::Model)->attached());

	// A canvas asks for the second's device (its view drawn): none until the pump makes it, the
	// least recently used then the third.
	TEST_EXPECT(devices.cache.device(paths[1], ViewportKind::Model) == nullptr);
	devices.sync(session);
	TEST_EXPECT(devices.made.size() == 7 && devices.cache.device(paths[1], ViewportKind::Model) == devices.made.back());
	TEST_EXPECT(devices.made.back()->taken == (Actions{ ViewportAction::Rebuild }));
	TEST_EXPECT(!devices.held(paths[2], ViewportKind::Model) && devices.cache.size() == 4);

	// Closed, a document's viewport goes, and its device with it.
	session.handle(request::close_document(paths[0]));
	devices.sync(session);
	TEST_EXPECT(!session.viewports().find(paths[0], ViewportKind::Model) && !devices.held(paths[0], ViewportKind::Model) &&
			devices.cache.size() == 3);

	// Opened again and synced, closed and opened again between two syncs (a viewport made again, at
	// the defaults, attached to no device): the device of the one that went goes, and the one made
	// again is given a device of its own, which makes its picture.
	session.handle(request::open_document(paths[0]));
	devices.sync(session);
	const FakeDevice *reopened = devices.held(paths[0], ViewportKind::Model);
	TEST_EXPECT(reopened && session.viewports().find(paths[0], ViewportKind::Model)->attached());
	session.handle(request::close_document(paths[0]));
	session.handle(request::open_document(paths[0]));
	const ViewportModel *again = session.viewports().find(paths[0], ViewportKind::Model);
	TEST_EXPECT(again && !again->attached());
	devices.sync(session);
	TEST_EXPECT(again && again->attached() && again->status() == ViewportStatus::Ready && again->builds() == 1);
	TEST_EXPECT(devices.held(paths[0], ViewportKind::Model) == devices.made.back() &&
			devices.made.back()->taken == (Actions{ ViewportAction::Rebuild }));
	std::printf("test_device_cache passed\n");
	return 0;
}

// The cache never gives up a device used in the round it needs one (a sync and the asks since the
// one before): one a view asked for since the last sync is kept, the least recently used of the
// rest given up (the second here, the first bumped); five viewports a view asks for every frame keep
// four devices and the fifth waits, rather than thrashing. A workspace's cache pins the Preview's
// target of the kind the Preview window shows alone (the menu's while a menu is shown), a headless
// one every kind's.
static int test_cache_rounds() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_rounds");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	FakeDevices devices;
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Rounds"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const std::vector<uint8_t> armory = test_io::read_file(synth("armory.3di"));
	std::vector<std::string> paths;
	for (int i = 0; i < 5; ++i) {
		paths.push_back("models/m" + std::to_string(i) + ".3di");
		TEST_EXPECT(editor_test::write_bytes(view.project.root + "/" + paths.back(), armory));
	}
	session.handle(request::rescan());
	session.run_operations();
	for (size_t i = 0; i < 4; ++i) {
		session.handle(request::open_document(paths[i]));
		devices.sync(session);
	}
	TEST_EXPECT(devices.cache.size() == 4);
	// m0, the least recently used, asked for by a view: kept; m4 opened takes m1's device.
	FakeDevice *first = devices.held(paths[0], ViewportKind::Model);
	TEST_EXPECT(first && devices.cache.device(paths[0], ViewportKind::Model) == first);
	session.handle(request::open_document(paths[4]));
	devices.sync(session);
	TEST_EXPECT(devices.held(paths[0], ViewportKind::Model) == first && !devices.held(paths[1], ViewportKind::Model) &&
			devices.held(paths[4], ViewportKind::Model) && devices.cache.size() == 4);
	TEST_EXPECT(!session.viewports().find(paths[1], ViewportKind::Model)->attached());

	// Five asked for every round: four keep their devices, the fifth waits; nothing made again.
	const size_t made = devices.made.size();
	for (int round = 0; round < 3; ++round) {
		for (const std::string &path : paths) devices.cache.device(path, ViewportKind::Model);
		devices.sync(session);
	}
	TEST_EXPECT(devices.made.size() == made && devices.cache.size() == 4 && !devices.held(paths[1], ViewportKind::Model));

	// A workspace's cache (set_pin_all_targets(false)), over a project of its own: the Preview window
	// showing the menu, its target given a device and the model's not; showing the model, the model's
	// too (the menu's kept). A headless cache (the default) gives both kinds' targets one.
	editor_test::TempProjectDir pin_dir("opennova_editor_viewport_pins");
	ProjectSession pins(platform, preferences);
	const SessionView &pinned = pins.view();
	pins.handle(request::new_project(pin_dir.file("project"), "Pins"));
	pins.run_operations();
	editor_test::create_missing_files(pins);
	TEST_EXPECT(editor_test::write_bytes(pinned.project.root + "/models/armory.3di", armory));
	pins.handle(request::rescan());
	pins.run_operations();
	pins.handle(request::open_document("models/armory.3di"));
	pins.handle(request::open_document("main.mnu"));
	const Document *menu_document = pins.document_for("main.mnu");
	TEST_EXPECT(menu_document != nullptr);
	if (!menu_document) return 1;
	const std::string menu = menu_document->path(), model = "models/armory.3di";
	TEST_EXPECT(pinned.documents.preview_shown == ViewportKind::Menu &&
			pinned.documents.previews[ViewportKind::Model].path == model);
	{
		FakeDevices shown;
		shown.cache.set_pin_all_targets(false);
		shown.sync(pins);
		TEST_EXPECT(shown.held(menu, ViewportKind::Menu) && !shown.held(model, ViewportKind::Model) && shown.cache.size() == 1);
		TEST_EXPECT(!pins.viewports().find(model, ViewportKind::Model)->attached());
		pins.handle(request::open_document(model));
		TEST_EXPECT(pinned.documents.preview_shown == ViewportKind::Model);
		shown.sync(pins);
		TEST_EXPECT(shown.held(model, ViewportKind::Model) && shown.held(menu, ViewportKind::Menu) && shown.cache.size() == 2);
	}
	pins.handle(request::open_document("main.mnu"));
	FakeDevices headless;
	headless.sync(pins);
	TEST_EXPECT(headless.held(model, ViewportKind::Model) && headless.held(menu, ViewportKind::Menu));
	std::printf("test_cache_rounds passed\n");
	return 0;
}

// A viewport's state moves by a SetViewport, or by one of the three things its follow derives (ADR
// 0046 S13 V5's derived changes): a menu's held window following the selection, a model framed
// when another is shown, the clock sought for a new clip or a selected event (model_viewport_test's
// test_animation pins the third). Each moves the view's Viewports concern, as a SetViewport does
// (a refused one moves nothing); syncs with nothing asked move no viewport's state, the clock or
// the concern.
static int test_state_moves() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_state");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	FakeDevices devices;
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "State"));
	session.run_operations();
	editor_test::create_missing_files(session);
	TEST_EXPECT(editor_test::write_bytes(view.project.root + "/models/armory.3di", test_io::read_file(synth("armory.3di"))));
	session.handle(request::rescan());
	session.run_operations();
	const auto moves = [&]() { return view.revisions.of(ViewConcern::Viewports); };

	// The menu: a SetViewport moves the concern, a refused one does not; the held window follows the
	// selection (derived), moving it at the sync that follows.
	session.handle(request::open_document("main.mnu"));
	Document *menu = session.document_for("main.mnu");
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	devices.sync(session);
	const NodeAddress title = named(*menu, "TITLE"), exit = named(*menu, "EXIT");
	uint64_t at = moves();
	session.handle(request::set_viewport(menu->path(), R"({"options": {"force_state": "mouseover", "force_id": )" +
			std::to_string(exit.child) + "}}"));
	TEST_EXPECT(session.outcome().done() && moves() == at + 1);
	at = moves();
	session.handle(request::set_viewport(menu->path(), R"({"options": {"force_state": "sideways"}})"));
	TEST_EXPECT(!session.outcome().done() && moves() == at);
	devices.sync(session);
	at = moves();
	session.handle(request::select_record(menu->path(), title));
	TEST_EXPECT(moves() == at);
	devices.sync(session);
	const auto *shown_menu = viewport_of<MenuViewport>(session, menu->path(), ViewportKind::Menu);
	TEST_EXPECT(shown_menu && shown_menu->options().force_window == title.child && moves() == at + 1);

	// The model: framed when it is first shown (derived).
	session.handle(request::open_document("models/armory.3di"));
	at = moves();
	devices.sync(session);
	const auto *model = viewport_of<ModelViewport>(session, "models/armory.3di", ViewportKind::Model);
	TEST_EXPECT(model && model->status() == ViewportStatus::Ready && moves() > at);
	if (!model) return 1;

	// Nothing asked: syncs and polls move no viewport's state, the clock or the concern.
	devices.sync(session);
	struct Seen {
		std::string menu_options, model_options, camera;
		int width = 0, height = 0;
		bool playing = false;
		uint32_t ms = 0;
		int32_t ticks = 0;
		double rate = 0.0;
		uint64_t moved = 0;
		bool operator==(const Seen &o) const {
			return menu_options == o.menu_options && model_options == o.model_options && camera == o.camera &&
					width == o.width && height == o.height && playing == o.playing && ms == o.ms &&
					ticks == o.ticks && rate == o.rate && moved == o.moved;
		}
	};
	const auto seen = [&]() {
		Seen out;
		out.menu_options = opennova::io::json_write(shown_menu->options_json());
		out.model_options = opennova::io::json_write(model->options_json());
		out.camera = opennova::io::json_write(model->camera_json());
		out.width = model->state().width;
		out.height = model->state().height;
		const PreviewClock &clock = session.viewports().clock();
		out.playing = clock.playing();
		out.ms = clock.ms();
		out.ticks = clock.ticks();
		out.rate = clock.rate();
		out.moved = moves();
		return out;
	};
	const Seen before = seen();
	for (int i = 0; i < 3; ++i) {
		session.poll();
		devices.sync(session);
	}
	TEST_EXPECT(seen() == before);
	std::printf("test_state_moves passed\n");
	return 0;
}

// The envelope: every member, a page of its items (and its notes by the same page), a model's camera,
// and the kind's empty viewport where none is kept.
static int test_envelope() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_envelope");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	FakeDevices devices;
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Envelope"));
	session.run_operations();
	editor_test::create_missing_files(session);
	TEST_EXPECT(editor_test::empty_viewport_json(view, ViewportKind::Menu).get_string("reason", "") == "no_menu");
	session.handle(request::open_document("main.mnu"));
	devices.sync(session);
	const std::string path = session.document_for("main.mnu")->path();
	const ViewportModel *menu = session.viewports().find(path, ViewportKind::Menu);
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	const JsonValue whole = viewport_to_json(view, *menu, JsonPage());
	for (const char *key : { "kind", "path", "as_saved", "status", "reason", "message", "detail", "progress",
				 "revision", "shown_revision", "current", "builds", "units", "device", "options", "camera", "clock",
				 "body", "items", "notes", "count", "offset", "next_offset", "note_count" })
		TEST_EXPECT(whole.get(key) != nullptr);
	// Its device's build (S13 V6): a device that makes its picture whole as it takes it reports none,
	// and no progress shows.
	TEST_EXPECT(whole.get("progress")->is_null() && whole.get("device")->get("build") &&
			!whole.get("device")->get("build")->get_bool("loading", true));
	for (const char *key :
			{ "generation", "loading", "failed", "done", "total", "frames", "frame_us", "unit_us", "total_us" })
		TEST_EXPECT(whole.get("device")->get("build")->get(key) != nullptr);
	// The viewport query stamps it (S13 V7) with the view's clock at which what its row reads last
	// moved: a SetViewport moves it.
	TEST_EXPECT(!whole.get("view_revision"));
	const auto stamp_of = [&session]() {
		std::string error;
		JsonValue args = JsonValue::make_object();
		args.set("op", JsonValue::make_string("state"));
		return session.query("viewport", args, error).get_number("view_revision", -1);
	};
	const ConcernSet reads = editor_query_row(EditorQueryKind::Viewport).reads;
	const double before = stamp_of();
	TEST_EXPECT(before == double(view.revisions.stamp_of(reads)) && before > 0.0);
	session.handle(request::set_viewport(path, R"({"options": {"show_hidden": true}})"));
	TEST_EXPECT(stamp_of() > before && stamp_of() == double(view.revisions.stamp(ViewConcern::Viewports)));
	session.handle(request::set_viewport(path, R"({"options": {"show_hidden": false}})"));
	devices.sync(session);
	TEST_EXPECT(whole.get_string("kind", "") == "menu" && whole.get_string("path", "") == path &&
			whole.get_bool("as_saved", false) && whole.get_string("status", "") == "ready" &&
			whole.get_string("reason", "") == "ready" && whole.get_string("message", "x").empty());
	TEST_EXPECT(whole.get_bool("current", false) && whole.get_number("builds", 0) == 1.0 &&
			whole.get_string("units", "") == "design" && whole.get("camera")->is_null());
	TEST_EXPECT(whole.get_number("revision", -1) == whole.get_number("shown_revision", -2));
	const JsonValue &device = *whole.get("device");
	TEST_EXPECT(device.get_bool("attached", false) && device.get_number("width", 0) == 800 &&
			device.get_number("height", 0) == 600 && !device.get_bool("canvas_sized", true));
	const JsonValue &timing = *whole.get("clock");
	TEST_EXPECT(timing.get_bool("playing", false) && timing.get_number("rate", 0) == 1.0 &&
			timing.get_number("time_ms", -1) == 0.0 && timing.get_number("ticks", -1) == 0.0);
	TEST_EXPECT(whole.get("body")->get("screen")->get_string("name", "") == "STARTUP");
	const size_t total = whole.get("items")->array.size();
	TEST_EXPECT(total >= 3 && whole.get_number("count", 0) == double(total) && whole.get_number("offset", -1) == 0.0 &&
			whole.get("next_offset")->is_null());
	TEST_EXPECT(whole.get_number("note_count", -1) == double(whole.get("notes")->array.size()));
	// A page: two items from the second, the next page's offset after them.
	JsonPage page;
	page.offset = 1;
	page.limit = 2;
	const JsonValue paged = viewport_to_json(view, *menu, page);
	TEST_EXPECT(paged.get("items")->array.size() == 2 && paged.get_number("count", 0) == double(total) &&
			paged.get_number("offset", -1) == 1.0);
	TEST_EXPECT(total > 3 ? paged.get_number("next_offset", 0) == 3.0 : paged.get("next_offset")->is_null());
	TEST_EXPECT(paged.get("items")->array[0].get_string("name", "") ==
			whole.get("items")->array[1].get_string("name", "?"));

	// A model's: pixels, its camera with the field of view, its body's level, registers and animation.
	TEST_EXPECT(editor_test::write_bytes(view.project.root + "/models/armory.3di", test_io::read_file(synth("armory.3di"))));
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(editor_test::empty_viewport_json(view, ViewportKind::Model).get_string("reason", "") == "no_model");
	session.handle(request::open_document("models/armory.3di"));
	devices.sync(session);
	const ViewportModel *armory = session.viewports().find("models/armory.3di", ViewportKind::Model);
	TEST_EXPECT(armory != nullptr);
	if (!armory) return 1;
	const JsonValue model = viewport_to_json(view, *armory, JsonPage());
	TEST_EXPECT(model.get_string("kind", "") == "model" && model.get_string("units", "") == "pixels" &&
			model.get_string("status", "") == "ready");
	const JsonValue *camera = model.get("camera");
	TEST_EXPECT(camera && camera->is_object() && camera->get("fov") && camera->get("target") &&
			camera->get("target")->array.size() == 3 && camera->get_number("distance", 0) > 0.0);
	TEST_EXPECT(model.get("body")->get("lod")->get("count") && model.get("body")->get("registers") &&
			model.get("body")->get("animation")->is_null());
	TEST_EXPECT(model.get("options")->get_string("lod", "") == "auto");
	// A hit: what lies under a point of a viewport, in its units, naming the viewport (its kind and its
	// document) beside the item.
	ViewportHit hit;
	hit.index = 2;
	hit.id = 7;
	hit.name = "TITLE";
	hit.kind = "static";
	hit.current = true;
	const JsonValue hit_json = viewport_hit_to_json(*menu, hit);
	TEST_EXPECT(hit_json.get_string("viewport", "") == "menu" && hit_json.get_string("path", "") == path &&
			hit_json.get_number("index", 0) == 2.0 && hit_json.get_number("id", 0) == 7.0 &&
			hit_json.get_string("name", "") == "TITLE" && hit_json.get_string("kind", "") == "static" &&
			hit_json.get_bool("current", false));
	// No viewport kept once the model closes: the kind's follow over no document says so (the wire's
	// reads are of a document's, so none answers this).
	session.handle(request::close_document("models/armory.3di"));
	TEST_EXPECT(!session.viewports().find("models/armory.3di", ViewportKind::Model));
	const JsonValue none = editor_test::empty_viewport_json(view, ViewportKind::Model);
	TEST_EXPECT(none.get_string("status", "") == "empty" && none.get_string("reason", "") == "no_model" &&
			none.get_string("path", "x").empty() && !none.get("device")->get_bool("attached", true) &&
			none.get("items")->array.empty());
	std::printf("test_envelope passed\n");
	return 0;
}

// A SetViewport over the wire (the session's JSON requests): read, served, done; with no path the
// active document's (S13 V7), refused when it shows in no viewport (a stylesheet), a document by its
// logical name; refused whole (a Problems row, viewport.refused, the request's outcome), the viewport
// as it was. A drag and a command planned by the viewport (the MCP's) and served: one undo step
// each; a drag the session refuses as it is served answers false, the menu as it was.
static int test_requests() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_requests");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	FakeDevices devices;
	session.handle(request::new_project(dir.file("project"), "Requests"));
	session.run_operations();
	editor_test::create_missing_files(session);
	session.handle(request::open_document("main.mnu"));
	Document *menu = session.document_for("main.mnu");
	devices.sync(session);
	const std::string path = menu ? menu->path() : std::string();
	const auto *shown = viewport_of<MenuViewport>(session, path, ViewportKind::Menu);
	TEST_EXPECT(menu && shown && shown->status() == ViewportStatus::Ready);
	if (!menu || !shown) return 1;

	const auto wire = [&](std::string text) {
		for (size_t at = text.find("MAIN_PATH"); at != std::string::npos; at = text.find("MAIN_PATH"))
			text.replace(at, 9, path);
		return session.handle_json(parse(text.c_str()));
	};
	JsonValue answer = wire(
			R"({"kind": "set_viewport", "path": "MAIN_PATH", "viewport": {"kind": "menu", "options": {"show_hidden": true}}})");
	TEST_EXPECT(answer.get_bool("ok", false) && answer.get_bool("served", false) &&
			answer.get("outcome")->get_bool("done", false) && shown->options().show_hidden);
	// No path: the active document's (S13 V7, A5's pathless rule), its kind the one it shows in.
	answer = wire(R"({"kind": "set_viewport", "viewport": {"options": {"show_hidden": false}}})");
	TEST_EXPECT(answer.get("outcome")->get_bool("done", false) && !shown->options().show_hidden);
	for (const char *refused : {
				 R"({"kind": "set_viewport", "path": "MAIN_PATH", "viewport": {"options": {"bogus": 1}}})",
				 R"({"kind": "set_viewport", "path": "MAIN_PATH", "viewport": {"kind": "model", "options": {"lod": 0}}})",
				 R"({"kind": "set_viewport", "viewport": {"kind": "model", "options": {"lod": 0}}})",
				 R"({"kind": "set_viewport", "path": "nosuch.mnu", "viewport": {"kind": "menu"}})",
				 R"({"kind": "set_viewport", "path": "MAIN_PATH", "viewport": {"device": {"width": 0}}})",
				 R"({"kind": "set_viewport", "path": "MAIN_PATH", "viewport": {"options": {"show_hidden": true}, "zoom": 2}})" }) {
		answer = wire(refused);
		const JsonValue *outcome = answer.get("outcome");
		TEST_EXPECT(answer.get_bool("ok", false) && outcome && !outcome->get_bool("done", true));
		TEST_EXPECT(outcome && outcome->get("findings")->array.size() == 1 &&
				outcome->get("findings")->array[0].get_string("code", "") == "viewport.refused" &&
				outcome->get("findings")->array[0].get_string("severity", "") == "error");
		TEST_EXPECT(!shown->options().show_hidden && shown->state().width == 800);
	}
	// A change that is no object is not read at all.
	answer = wire(R"({"kind": "set_viewport", "path": "MAIN_PATH", "viewport": 3})");
	TEST_EXPECT(!answer.get_bool("ok", true));
	// A stylesheet active (it feeds the menu's viewport, and shows in none): a pathless change refused,
	// naming why; the menu named by its logical name, changed.
	session.handle(request::open_document("menu_style.mns"));
	answer = wire(R"({"kind": "set_viewport", "viewport": {"options": {"show_hidden": true}}})");
	TEST_EXPECT(!answer.get("outcome")->get_bool("done", true) &&
			answer.get("outcome")->get("findings")->array[0].get_string("message", "").find("shows in no viewport") !=
					std::string::npos);
	answer = wire(R"({"kind": "set_viewport", "path": "MAIN.MNU", "viewport": {"options": {"show_hidden": true}}})");
	TEST_EXPECT(answer.get("outcome")->get_bool("done", false) && shown->options().show_hidden);
	answer = wire(R"({"kind": "set_viewport", "path": "MAIN.MNU", "viewport": {"options": {"show_hidden": false}}})");
	TEST_EXPECT(answer.get("outcome")->get_bool("done", false) && !shown->options().show_hidden);
	session.handle(request::open_document("main.mnu"));
	// A viewport the change is for is made only once the change applies: a refused one makes none.
	{
		Viewports fresh;
		std::string error;
		TEST_EXPECT(!fresh.set(session.view(), path, parse(R"({"kind": "menu", "options": {"bogus": 1}})"), error) &&
				!error.empty() && fresh.size() == 0);
		error.clear();
		TEST_EXPECT(fresh.set(session.view(), path, parse(R"({"kind": "menu", "options": {"show_hidden": true}})"), error) &&
				error.empty() && fresh.size() == 1 && fresh.find(path, ViewportKind::Menu));
	}

	// The MCP's drag, planned by the viewport and served: one undo step.
	const NodeAddress title = named(*menu, "TITLE"), exit = named(*menu, "EXIT");
	devices.sync(session);
	ViewportDrag drag;
	drag.id = title.child;
	drag.handle = "move";
	drag.x = 16;
	drag.y = 8;
	drag.snap = 1;
	editor_test::Gathered planned;
	std::string error;
	TEST_EXPECT(shown->drag(viewport_context(session.view(), *shown), drag, planned, error));
	TEST_EXPECT(editor_test::serve(session, planned.requests) && menu->dirty());
	session.handle(request::undo(menu->path()));
	TEST_EXPECT(!menu->dirty() && !menu->can_undo());
	// Refused as it is served (an operation began holding the documents after the plan): false,
	// nothing written.
	devices.sync(session);
	planned.requests.clear();
	TEST_EXPECT(shown->drag(viewport_context(session.view(), *shown), drag, planned, error));
	const uint64_t revision = menu->revision();
	TEST_EXPECT(session.start_operation(std::make_unique<editor_test::HoldingOperation>()) != 0);
	TEST_EXPECT(!editor_test::serve(session, planned.requests));
	TEST_EXPECT(menu->revision() == revision && !menu->dirty());
	session.run_operations();

	// The command (an arrange): EXIT's left edge to TITLE's, one batch, one undo step.
	devices.sync(session);
	editor_test::Gathered arranged;
	TEST_EXPECT(shown->command(viewport_context(session.view(), *shown), "align_left", { title.child, exit.child },
			arranged, error));
	TEST_EXPECT(arranged.requests.size() == 1 && editor_test::serve(session, arranged.requests));
	TEST_EXPECT(field_of(*menu, exit, "position.left") == field_of(*menu, title, "position.left") || menu->dirty());
	session.handle(request::undo(menu->path()));
	TEST_EXPECT(!menu->dirty() && !menu->can_undo());
	std::printf("test_requests passed\n");
	return 0;
}

// One undo step per gesture, through a real session.
static int test_gestures() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_gestures");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	FakeDevices devices;
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Gestures"));
	session.run_operations();
	TEST_EXPECT(editor_test::write_text(view.project.root + "/layout.mnu", kLayoutMenu));
	TEST_EXPECT(editor_test::write_bytes(view.project.root + "/models/armory.3di", test_io::read_file(synth("armory.3di"))));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("layout.mnu"));
	Document *menu = session.document_for("layout.mnu");
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	const NodeAddress box = named(*menu, "BOX"), other = named(*menu, "OTHER"), tiny = named(*menu, "TINY");
	TEST_EXPECT(box.child && other.child && tiny.child);
	const std::string path = menu->path();
	CanvasRig rig{ session, devices, path, ViewportKind::Menu, 1.0f };
	devices.sync(session);
	TEST_EXPECT(rig.viewport() && rig.viewport()->status() == ViewportStatus::Ready);

	// A drag of BOX, four samples on the grid: a batch a step under one token, its one end, one undo
	// step; BOX on 144/344/120/220.
	session.handle(request::select_record(path, box));
	rig.drag(200.0f, 150.0f, 40.0f, 21.0f);
	TEST_EXPECT(!rig.out.refused && rig.out.count(EditorRequestKind::EditRecord) >= 2 &&
			rig.out.count(EditorRequestKind::EndEdit) == 1 && rig.out.gesture() != 0);
	TEST_EXPECT(edges(*menu, box) == (std::vector<int64_t>{ 144, 120, 344, 220 }));
	session.handle(request::undo(path));
	TEST_EXPECT(edges(*menu, box) == (std::vector<int64_t>{ 100, 100, 300, 200 }) && !menu->dirty() &&
			!menu->can_undo());

	// Three windows selected, BOX dragged: all three move as BOX snaps, in one step.
	session.handle(request::select_record(path, box, SelectMode::Replace, { box, other, tiny }));
	rig.out.raised.clear();
	rig.drag(200.0f, 150.0f, 40.0f, 21.0f);
	TEST_EXPECT(!rig.out.refused && rig.out.count(EditorRequestKind::EndEdit) == 1 && rig.out.gesture() != 0);
	TEST_EXPECT(edges(*menu, box) == (std::vector<int64_t>{ 144, 120, 344, 220 }));
	TEST_EXPECT(edges(*menu, other) == (std::vector<int64_t>{ 444, 320, 644, 420 }));
	TEST_EXPECT(edges(*menu, tiny) == (std::vector<int64_t>{ 644, 124, 656, 136 }));
	session.handle(request::undo(path));
	TEST_EXPECT(edges(*menu, box) == (std::vector<int64_t>{ 100, 100, 300, 200 }));
	TEST_EXPECT(edges(*menu, other) == (std::vector<int64_t>{ 400, 300, 600, 400 }));
	TEST_EXPECT(edges(*menu, tiny) == (std::vector<int64_t>{ 600, 104, 612, 116 }));
	TEST_EXPECT(!menu->dirty() && !menu->can_undo());

	// A drag its canvas stops drawing (its window hidden mid-drag): its end raised once, then
	// nothing as the button comes up; one undo step for what it wrote.
	session.handle(request::select_record(path, box));
	rig.out.raised.clear();
	CanvasInput in = rig.at(200.0f, 150.0f);
	in.pressed = in.down = true;
	rig.frame(in);
	for (const float x : { 224.0f, 248.0f }) {
		in = rig.at(x, 150.0f);
		in.down = true;
		rig.frame(in);
	}
	TEST_EXPECT(rig.out.count(EditorRequestKind::EditRecord) == 2 && rig.out.count(EditorRequestKind::EndEdit) == 0);
	rig.hidden();
	TEST_EXPECT(rig.out.count(EditorRequestKind::EndEdit) == 1);
	in = rig.at(248.0f, 150.0f);
	rig.frame(in);
	TEST_EXPECT(rig.out.count(EditorRequestKind::EndEdit) == 1 && rig.out.count(EditorRequestKind::EditRecord) == 2);
	TEST_EXPECT(field_of(*menu, box, "position.left") != 100 && menu->dirty());
	session.handle(request::undo(path));
	TEST_EXPECT(edges(*menu, box) == (std::vector<int64_t>{ 100, 100, 300, 200 }) && !menu->can_undo());

	// A model marker's drag: the selected user point's place, four samples, one step; with a second
	// user point selected, both move as far in the same step.
	session.handle(request::open_document("models/armory.3di"));
	auto *document = dynamic_cast<ModelDocument *>(session.document_for("models/armory.3di"));
	TEST_EXPECT(document && document->model_row());
	if (!document || !document->model_row()) return 1;
	const ModelRow &row = *document->model_row();
	TEST_EXPECT(row.ids.lists[3].size() >= 2);
	if (row.ids.lists[3].size() < 2) return 1;
	const NodeAddress point{ row.id, node_kind(ModelKind::UserPoint), row.ids.lists[3][0].id };
	const NodeAddress second{ row.id, node_kind(ModelKind::UserPoint), row.ids.lists[3][1].id };
	CanvasRig model_rig{ session, devices, "models/armory.3di", ViewportKind::Model };
	const auto place = [&](const NodeAddress &record) {
		std::vector<double> at;
		for (const char *field : { "position.x", "position.y", "position.z" }) {
			Value value;
			at.push_back(document->get(record, field, value) ? std::get<double>(value) : 0.0);
		}
		return at;
	};
	const auto marker_pixel = [&](int index, float &x, float &y) {
		const auto *model = viewport_of<ModelViewport>(session, "models/armory.3di", ViewportKind::Model);
		for (const ModelOverlay &overlay : model->overlays(session.viewports().clock()))
			if (overlay.kind == ModelOverlayKind::UserPoint && overlay.index == index)
				return model->camera().project(overlay.at, model->size().width, model->size().height, x, y);
		return false;
	};
	for (const bool both : { false, true }) {
		const std::vector<double> was = place(point), second_was = place(second);
		session.handle(both ? request::select_record(document->path(), point, SelectMode::Replace, { second, point })
							: request::select_record(document->path(), point));
		devices.sync(session);
		float x = 0.0f, y = 0.0f;
		TEST_EXPECT(marker_pixel(0, x, y));
		model_rig.out.raised.clear();
		CanvasKeys alt;
		alt.alt = true;
		model_rig.drag(x, y, 24.0f, -12.0f, alt);
		TEST_EXPECT(!model_rig.out.refused && model_rig.out.count(EditorRequestKind::EditRecord) >= 2 &&
				model_rig.out.count(EditorRequestKind::EndEdit) == 1 && model_rig.out.gesture() != 0);
		TEST_EXPECT(place(point) != was && document->dirty());
		TEST_EXPECT(both ? place(second) != second_was : place(second) == second_was);
		if (both) {
			// As far: the second's move is the first's.
			const std::vector<double> moved = place(point), second_moved = place(second);
			for (size_t i = 0; i < 3; ++i)
				TEST_EXPECT(std::fabs((moved[i] - was[i]) - (second_moved[i] - second_was[i])) < 1e-3);
		}
		session.handle(request::undo(document->path()));
		TEST_EXPECT(place(point) == was && place(second) == second_was && !document->dirty() && !document->can_undo());
	}
	// The MCP's drag (the viewport's planner) of a selected marker's place moves the other selected
	// markers as far, in one step, as the canvas's does; of a marker not selected, it alone.
	for (const bool selected : { true, false }) {
		const std::vector<double> was = place(point), second_was = place(second);
		session.handle(selected ? request::select_record(document->path(), point, SelectMode::Replace, { second, point })
								: request::select_record(document->path(), second));
		devices.sync(session);
		const auto *model = viewport_of<ModelViewport>(session, "models/armory.3di", ViewportKind::Model);
		float x = 0.0f, y = 0.0f;
		TEST_EXPECT(model && marker_pixel(0, x, y));
		if (!model) return 1;
		ViewportDrag drag;
		drag.id = point.child;
		drag.handle = "place";
		drag.by = false;
		drag.x = x + 24.0f;
		drag.y = y - 12.0f;
		editor_test::Gathered planned;
		std::string error;
		TEST_EXPECT(model->drag(viewport_context(session.view(), *model), drag, planned, error));
		TEST_EXPECT(editor_test::serve(session, planned.requests));
		TEST_EXPECT(place(point) != was && (selected ? place(second) != second_was : place(second) == second_was));
		if (selected) {
			const std::vector<double> moved = place(point), second_moved = place(second);
			for (size_t i = 0; i < 3; ++i)
				TEST_EXPECT(std::fabs((moved[i] - was[i]) - (second_moved[i] - second_was[i])) < 1e-3);
		}
		session.handle(request::undo(document->path()));
		TEST_EXPECT(place(point) == was && place(second) == second_was && !document->dirty() && !document->can_undo());
	}
	std::printf("test_gestures passed\n");
	return 0;
}

// S13 V8: what reaches a menu's picture, from its change set and the files it read. A menu of two
// screens, FIRST shown: an edit of SECOND's window, and a screen added after it, leave the picture as
// it is (no configure, the picture current); an edit of FIRST's window configures again; a stylesheet
// variable SECOND alone names leaves it, one FIRST names (its labels' colour) configures again; the
// menu read again is everything (Unknown), configured again; SECOND's window made unwritable takes
// the picture away (the game would read none of the menu), and undone makes it again.
static int test_menu_change_sets() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_change_sets");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	FakeDevices devices;
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Change Sets"));
	session.run_operations();
	editor_test::create_missing_files(session);
	TEST_EXPECT(editor_test::write_text(view.project.root + "/two.mnu", kTwoScreens));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("two.mnu"));
	Document *menu = session.document_for("two.mnu");
	TEST_EXPECT(menu && menu->rows().size() == 2);
	if (!menu || menu->rows().size() != 2) return 1;
	const std::string path = menu->path();
	devices.sync(session);
	const auto *shown = viewport_of<MenuViewport>(session, path, ViewportKind::Menu);
	FakeDevice *device = devices.held(path, ViewportKind::Menu);
	TEST_EXPECT(shown && device && shown->status() == ViewportStatus::Ready &&
			shown->screen_row() == menu->rows()[0]->id && shown->configures() == 1);
	if (!shown || !device) return 1;
	const NodeAddress label = named(*menu, "LABEL"), note = named(*menu, "NOTE");
	TEST_EXPECT(label.child && note.child && note.row == menu->rows()[1]->id);
	const auto current = [&]() {
		return shown->current(ViewportInput{ view, session.viewports().clock(), session.document_for(path) });
	};

	// Another screen's window moved: its change set names SECOND's row alone (Keep).
	size_t from = device->taken.size();
	set(session, *menu, note, "position.left", int64_t(120));
	devices.sync(session);
	TEST_EXPECT(device->since(from).empty() && shown->configures() == 1 &&
			shown->followed_change() == ChangeClass::Changed && current());
	// A screen added after it (the session selects it; FIRST's label selected again before the pump,
	// the Preview stays on FIRST): Keep.
	EditorRequest add = request::edit_record(path, Edit());
	add.edits[0].operation = EditOperation::Add;
	add.edits[0].address = NodeAddress{ 0, node_kind(MenuKind::Screen), 0 };
	session.handle(add);
	TEST_EXPECT(session.outcome().done() && menu->rows().size() == 3);
	session.handle(request::select_record(path, label));
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(shown->screen_row() == menu->rows()[0]->id && device->since(from).empty() &&
			shown->configures() == 1 && current());
	// The shown screen's window moved: Rebuild.
	set(session, *menu, label, "position.left", int64_t(120));
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(device->since(from) == (Actions{ ViewportAction::Rebuild }) && shown->configures() == 2 && current());

	// The stylesheet (open, standing in for its file): TRIM_COLOR, which SECOND alone names, leaves
	// the picture; DEF_TEXT_FG, FIRST's labels' colour, configures it again.
	session.handle(request::open_document("menu_style.mns"));
	Document *style = session.document_for("menu_style.mns");
	TEST_EXPECT(style && view.documents.previews[ViewportKind::Menu].path == path);
	if (!style) return 1;
	set(session, *style, named(*style, "TRIM_COLOR"), "value", std::string("FF102030"));
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(style->dirty() && device->since(from).empty() && shown->configures() == 2);
	set(session, *style, named(*style, "DEF_TEXT_FG"), "value", std::string("FFFF0000"));
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(device->since(from) == (Actions{ ViewportAction::Rebuild }) && shown->configures() == 3);
	TEST_EXPECT(shown->style_vars().count("DEF_TEXT_FG") && shown->style_vars().at("DEF_TEXT_FG") == "FFFF0000");

	// The menu read again from its file (another instance at the path: the document cannot say what
	// changed): everything, configured again.
	session.handle(request::save(path));
	session.handle(request::reload_document(path));
	session.run_operations();
	TEST_EXPECT(session.outcome().done());
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(shown == viewport_of<MenuViewport>(session, path, ViewportKind::Menu) &&
			shown->followed_change() == ChangeClass::Unknown && shown->configures() == 4 &&
			device->since(from) == (Actions{ ViewportAction::Rebuild }));

	// SECOND's window made unwritable (a justify the writer cannot write), a screen after the shown
	// one: the game would read none of the menu, so the picture goes (Clear, Failed); undone, it is
	// made again.
	menu = session.document_for(path);
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	set(session, *menu, named(*menu, "NOTE"), "string.justify", std::string("CEN\"TER"));
	TEST_EXPECT(session.outcome().done() && !menu->serialize().ok());
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(device->since(from) == (Actions{ ViewportAction::Clear }) && shown->status() == ViewportStatus::Failed);
	session.handle(request::undo(path));
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(device->since(from) == (Actions{ ViewportAction::Rebuild }) && shown->status() == ViewportStatus::Ready);
	std::printf("test_menu_change_sets passed\n");
	return 0;
}

// S13 V8 (the review): a screen before the shown one feeds its picture. SECOND shown, its band
// FIRST's (sheet.tga's first load: HEIGHT 20 against its own 24, an image_height_shared note): THIRD's
// HEIGHT changed after it leaves the picture as it is (no configure); FIRST's changed configures it
// again, its band FIRST's new one (24: no note; 16: the note says 16); the screens reordered (SECOND
// first, its own HEIGHT the first load: no note) and that undone configure it again; a stylesheet
// variable FIRST alone names configures it, one THIRD alone names does not.
static int test_menu_shared_texture() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_shared_texture");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	FakeDevices devices;
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Shared Texture"));
	session.run_operations();
	editor_test::create_missing_files(session);
	TEST_EXPECT(editor_test::write_bytes(view.project.root + "/sheet.tga", tga(64, 80)));
	TEST_EXPECT(editor_test::write_text(view.project.root + "/shared.mnu",
			image_screen("FIRST", "ONE", 20, "DEF_TEXT_FG") + image_screen("SECOND", "TWO", 24) +
					image_screen("THIRD", "TRE", 40, "TRIM_COLOR")));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("shared.mnu"));
	auto *menu = dynamic_cast<MnuDocument *>(session.document_for("shared.mnu"));
	TEST_EXPECT(menu && menu->rows().size() == 3);
	if (!menu || menu->rows().size() != 3) return 1;
	const std::string path = menu->path();
	const NodeAddress one = named(*menu, "ONE"), two = named(*menu, "TWO"), tre = named(*menu, "TRE");
	TEST_EXPECT(one.child && two.child && tre.child);
	session.handle(request::select_record(path, two));
	devices.sync(session);
	const auto *shown = viewport_of<MenuViewport>(session, path, ViewportKind::Menu);
	FakeDevice *device = devices.held(path, ViewportKind::Menu);
	TEST_EXPECT(shown && device && shown->status() == ViewportStatus::Ready && shown->screen_row() == two.row);
	if (!shown || !device) return 1;
	// The band SECOND's row draws, as its picture's notes say it: the first load's HEIGHT, "" where its
	// own is the first load's.
	const auto shared = [&]() {
		for (const opennova::menu::MenuFrameNote &note : shown->notes())
			if (note.code == opennova::menu::MenuFrameNoteCode::ImageHeightShared) return note.subject;
		return std::string();
	};
	// The HEIGHT of `window`'s IMAGE row set: true when the edit went through.
	const auto height = [&](const NodeAddress &window, int64_t value) {
		const NodeAddress row = menu->record_at(*menu->row(window.row), size_t(menu->window_index(window)),
				menu_window_list("appearance"), 0);
		if (!row.child) return false;
		set(session, *menu, row, "height", value);
		return session.outcome().done();
	};
	const auto current = [&]() {
		return shown->current(ViewportInput{ view, session.viewports().clock(), session.document_for(path) });
	};
	TEST_EXPECT(shared() == "20");
	uint64_t configures = shown->configures();

	// After it: Keep.
	TEST_EXPECT(height(tre, 30));
	size_t from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(device->since(from).empty() && shown->configures() == configures && current() && shared() == "20");
	// Before it: configured again, FIRST's new HEIGHT its band.
	TEST_EXPECT(height(one, 24));
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(device->since(from) == (Actions{ ViewportAction::Rebuild }) && shown->configures() == ++configures &&
			shared().empty());
	TEST_EXPECT(height(one, 16));
	devices.sync(session);
	TEST_EXPECT(shown->configures() == ++configures && shared() == "16");
	// The screens reordered: SECOND first, its own HEIGHT the first load.
	EditorRequest move = request::edit_record(path, Edit());
	move.edits[0].operation = EditOperation::Move;
	move.edits[0].address = NodeAddress{ two.row, node_kind(MenuKind::Screen), 0 };
	move.edits[0].position = 0;
	session.handle(move);
	TEST_EXPECT(session.outcome().done() && menu->rows().front()->id == two.row);
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(device->since(from) == (Actions{ ViewportAction::Rebuild }) && shown->configures() == ++configures &&
			shared().empty());
	session.handle(request::undo(path));
	devices.sync(session);
	TEST_EXPECT(menu->rows()[1]->id == two.row && shown->configures() == ++configures && shared() == "16");

	// The stylesheet (open, standing in for its file): TRIM_COLOR, which THIRD alone names, leaves the
	// picture; DEF_TEXT_FG, which FIRST names (a screen whose loads SECOND's compile takes), configures it.
	session.handle(request::open_document("menu_style.mns"));
	Document *style = session.document_for("menu_style.mns");
	TEST_EXPECT(style && view.documents.previews[ViewportKind::Menu].path == path);
	if (!style) return 1;
	set(session, *style, named(*style, "TRIM_COLOR"), "value", std::string("FF102030"));
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(style->dirty() && device->since(from).empty() && shown->configures() == configures);
	set(session, *style, named(*style, "DEF_TEXT_FG"), "value", std::string("FFFF0000"));
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(device->since(from) == (Actions{ ViewportAction::Rebuild }) && shown->configures() == ++configures);
	std::printf("test_menu_shared_texture passed\n");
	return 0;
}

// S13 V8: a picture made again waits for the gesture open in its document where its kind's row holds
// for one (the model's scene), and nowhere else. A drag of BOX of four samples: the menu configures
// again live, its device making each sample's picture (a Rebuild a pump, never held), the picture
// current. A light's colour dragged over the model (a batch a sample, one token): the viewport reads
// every sample, its device keeping the last scene (no Rebuild, held), the Inspector typing into the
// menu meanwhile, its EndEdit and its Undo leaving it held (a request ends the gesture of its own
// document only), and the gesture's end builds once. A model marker's drag (a user point's place) is
// an Update a sample, never a Rebuild: the overlays follow the live rows over the scene that stands.
static int test_gesture_holds_rebuild() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_gesture_hold");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	FakeDevices devices;
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Gesture Hold"));
	session.run_operations();
	TEST_EXPECT(editor_test::write_text(view.project.root + "/layout.mnu", kLayoutMenu));
	TEST_EXPECT(editor_test::write_bytes(view.project.root + "/models/armory.3di", test_io::read_file(synth("armory.3di"))));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("layout.mnu"));
	Document *menu = session.document_for("layout.mnu");
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	const std::string path = menu->path();
	const NodeAddress box = named(*menu, "BOX");
	CanvasRig rig{ session, devices, path, ViewportKind::Menu, 1.0f };
	devices.sync(session);
	const auto *shown = viewport_of<MenuViewport>(session, path, ViewportKind::Menu);
	FakeDevice *device = devices.held(path, ViewportKind::Menu);
	TEST_EXPECT(shown && device && shown->status() == ViewportStatus::Ready);
	if (!shown || !device) return 1;
	session.handle(request::select_record(path, box));
	const auto rebuilds = [&](size_t from) {
		size_t n = 0;
		for (size_t i = from; i < device->taken.size(); ++i) n += device->taken[i] == ViewportAction::Rebuild ? 1 : 0;
		return n;
	};
	// BOX's left edge as the viewport's own compile places it.
	const auto compiled_left = [&]() {
		opennova::mnu::RectEdges rect{};
		const int index = static_cast<const MnuDocument *>(menu)->window_index(box);
		return shown->render().compiler().widget_rect(index, shown->render().state(), &rect) ? rect.left : -1;
	};
	const size_t from = device->taken.size();
	const size_t configures = shown->configures();
	const int left = compiled_left();
	CanvasInput in = rig.at(200.0f, 150.0f);
	in.pressed = in.down = true;
	rig.frame(in);
	bool held = false;
	for (int i = 1; i <= 4; ++i) {
		in = rig.at(200.0f + 10.0f * float(i), 150.0f);
		in.down = true;
		rig.frame(in);
		held = held || shown->held();
	}
	// The next frame's pump, the button still down: the compile has every sample, the picture
	// current, the device's picture made again a sample (the menu's row holds for no gesture).
	devices.sync(session);
	const bool current = shown->current(viewport_context(session.view(), *shown).input);
	const size_t batches = rig.out.count(EditorRequestKind::EditRecord);
	TEST_EXPECT(view.documents.gesture_in(path).open() && batches >= 3);
	TEST_EXPECT(!held && !shown->held() && current && compiled_left() > left && rebuilds(from) >= 3 &&
			shown->configures() >= configures + 3);
	in.down = false;
	rig.frame(in); // let go: the gesture's end
	TEST_EXPECT(!view.documents.gesture_in(path).open() && view.documents.gestures.empty() &&
			rig.out.count(EditorRequestKind::EndEdit) == 1);
	const size_t made = rebuilds(from);
	devices.sync(session);
	TEST_EXPECT(rebuilds(from) == made && compiled_left() == field_of(*menu, box, "position.left"));
	session.handle(request::undo(path));
	TEST_EXPECT(!menu->dirty() && !menu->can_undo());

	session.handle(request::open_document("models/armory.3di"));
	auto *document = dynamic_cast<ModelDocument *>(session.document_for("models/armory.3di"));
	TEST_EXPECT(document && document->model_row());
	if (!document || !document->model_row()) return 1;
	const ModelRow &row = *document->model_row();
	devices.sync(session);
	const auto *model = viewport_of<ModelViewport>(session, "models/armory.3di", ViewportKind::Model);
	FakeDevice *model_device = devices.held("models/armory.3di", ViewportKind::Model);
	TEST_EXPECT(model && model_device);
	if (!model || !model_device) return 1;
	const auto count = [&](size_t start, ViewportAction action) {
		size_t n = 0;
		for (size_t i = start; i < model_device->taken.size(); ++i) n += model_device->taken[i] == action ? 1 : 0;
		return n;
	};

	// A light's colour dragged: three samples of one gesture, each read (the model the document's), the
	// scene held; the gesture's end builds once; one undo step.
	{
		const NodeAddress light{ row.id, node_kind(ModelKind::Light), row.ids.lists[2][0].id };
		const uint64_t token = next_edit_gesture();
		const size_t start = model_device->taken.size();
		const uint64_t reads = model->reads(), builds = model->builds();
		for (const int64_t red : { 10, 20, 30 }) {
			EditorRequest sample = request::edit_record(document->path(), Edit());
			sample.edits[0].address = light;
			sample.edits[0].field = "start.r";
			sample.edits[0].value = red;
			sample.edits[0].gesture = token;
			session.handle(sample);
			TEST_EXPECT(session.outcome().done());
			devices.sync(session);
		}
		TEST_EXPECT(view.documents.gesture_in(document->path()).open() && model->held() && model->reads() == reads + 3 &&
				count(start, ViewportAction::Rebuild) == 0 && model->builds() == builds);
		// The Inspector typing into another document (the menu) during the drag, its EndEdit and an Undo
		// of it: the drag stays open, the scene held (a request ends the gesture of its own document only).
		set(session, *menu, box, "position.left", int64_t(112));
		TEST_EXPECT(session.outcome().done() && menu->dirty());
		session.handle(request::end_edit(path));
		session.handle(request::undo(path));
		devices.sync(session);
		TEST_EXPECT(!menu->dirty() && view.documents.gesture_in(document->path()).open() && model->held() &&
				count(start, ViewportAction::Rebuild) == 0);
		session.handle(request::end_edit(document->path()));
		TEST_EXPECT(!view.documents.gesture_in(document->path()).open() && count(start, ViewportAction::Rebuild) == 0);
		devices.sync(session);
		TEST_EXPECT(!model->held() && count(start, ViewportAction::Rebuild) == 1 && model->builds() == builds + 1);
		devices.sync(session);
		TEST_EXPECT(count(start, ViewportAction::Rebuild) == 1);
		session.handle(request::undo(document->path()));
		devices.sync(session);
		TEST_EXPECT(!document->dirty() && !document->can_undo());
	}

	// A user point's drag: each sample an Update (the overlays' alone), no Rebuild, held or not.
	const NodeAddress point{ row.id, node_kind(ModelKind::UserPoint), row.ids.lists[3][0].id };
	session.handle(request::select_record(document->path(), point));
	devices.sync(session);
	float x = 0.0f, y = 0.0f;
	bool projected = false;
	for (const ModelOverlay &overlay : model->overlays(session.viewports().clock()))
		if (overlay.kind == ModelOverlayKind::UserPoint && overlay.index == 0)
			projected = model->camera().project(overlay.at, model->size().width, model->size().height, x, y);
	TEST_EXPECT(projected);
	const size_t model_from = model_device->taken.size();
	const uint64_t reads = model->reads(), builds = model->builds();
	CanvasRig model_rig{ session, devices, "models/armory.3di", ViewportKind::Model };
	CanvasKeys alt;
	alt.alt = true;
	model_rig.drag(x, y, 24.0f, -12.0f, alt);
	devices.sync(session);
	size_t updates = 0, model_rebuilds = 0;
	for (size_t i = model_from; i < model_device->taken.size(); ++i) {
		updates += model_device->taken[i] == ViewportAction::Update ? 1 : 0;
		model_rebuilds += model_device->taken[i] == ViewportAction::Rebuild ? 1 : 0;
	}
	TEST_EXPECT(!model_rig.out.refused && model_rig.out.count(EditorRequestKind::EditRecord) >= 2);
	TEST_EXPECT(updates >= 2 && model_rebuilds == 0 && model->builds() == builds && model->reads() == reads &&
			model->patches() >= 2 && document->dirty());
	std::printf("test_gesture_holds_rebuild passed\n");
	return 0;
}

// S13 V8 (the review): a Rebuild is held for a gesture only over a picture the device holds, with
// nothing else to take. A viewport of the model's kind, its follow scripted, a gesture open in its
// document: a sample's Rebuild held; a Clear mid-gesture drops the picture (and what was held), so the
// next sample's Rebuild is made at once, and the one after held again over it; a Clear and a Rebuild
// in one follow's turn make the picture (no Clear held over); the gesture's end issues the held
// Rebuild once. A gesture in another document holds nothing.
static int test_hold_needs_a_picture() {
	SessionView view;
	PreviewClock clock;
	ScriptedViewport viewport;
	const ViewportInput input{ view, clock, nullptr, ChangeClass::Changed };
	const auto follow = [&](ViewportAction next) {
		viewport.next = next;
		viewport.follow(input, clock);
		return viewport.take_action();
	};
	viewport.attach();
	TEST_EXPECT(viewport.take_action() == ViewportAction::Rebuild);
	// A gesture in another document: nothing held.
	view.documents.gestures.push_back(OpenGesture{ "models/other.3di", 4 });
	TEST_EXPECT(follow(ViewportAction::Rebuild) == ViewportAction::Rebuild && !viewport.held());
	view.documents.gestures.push_back(OpenGesture{ viewport.path(), 5 });
	TEST_EXPECT(follow(ViewportAction::Rebuild) == ViewportAction::Keep && viewport.held());
	TEST_EXPECT(follow(ViewportAction::Update) == ViewportAction::Update && viewport.held());
	// Cleared mid-gesture: the device holds no picture, so a sample's Rebuild makes one now.
	TEST_EXPECT(follow(ViewportAction::Clear) == ViewportAction::Clear && !viewport.held());
	TEST_EXPECT(follow(ViewportAction::Rebuild) == ViewportAction::Rebuild && !viewport.held());
	TEST_EXPECT(follow(ViewportAction::Rebuild) == ViewportAction::Keep && viewport.held());
	// A Clear, then a Rebuild before the device takes it: the picture made (the Clear not left over).
	viewport.next = ViewportAction::Clear;
	viewport.follow(input, clock);
	TEST_EXPECT(!viewport.held());
	TEST_EXPECT(follow(ViewportAction::Rebuild) == ViewportAction::Rebuild && !viewport.held());
	TEST_EXPECT(follow(ViewportAction::Rebuild) == ViewportAction::Keep && viewport.held());
	// The gesture ends: the held Rebuild once, then nothing.
	view.documents.gestures.pop_back();
	const uint64_t builds = viewport.builds();
	TEST_EXPECT(follow(ViewportAction::Keep) == ViewportAction::Rebuild && !viewport.held() &&
			viewport.builds() == builds + 1);
	TEST_EXPECT(follow(ViewportAction::Keep) == ViewportAction::Keep);
	std::printf("test_hold_needs_a_picture passed\n");
	return 0;
}

// S13 V8: a menu's clock is the preview clock, with no configure: the frame's clock is the preview
// clock's milliseconds (menu_frame_time), so a focused edit box's caret is drawn while the clock's
// (milliseconds & 0x3FF) are past 0x200 and not before; the device ticked as the clock runs takes no
// action (nothing is configured or made again), and sets its frame's clock (a compile of its frame)
// only as the caret's half of the blink changes: twice a blink at most, never once a frame, never
// with no edit box focused, never while the clock stands.
static int test_menu_clock() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_menu_clock");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	FakeDevices devices;
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Menu Clock"));
	session.run_operations();
	editor_test::create_missing_files(session);
	TEST_EXPECT(editor_test::write_text(view.project.root + "/two.mnu", kTwoScreens));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("two.mnu"));
	auto *menu = dynamic_cast<MnuDocument *>(session.document_for("two.mnu"));
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	const std::string path = menu->path();
	const NodeAddress edit = named(*menu, "EDIT");
	session.handle(request::set_viewport(path, R"({"options": {"force_id": )" + std::to_string(edit.child) +
			R"(, "focus": true}})"));
	TEST_EXPECT(session.outcome().done());
	devices.sync(session);
	const auto *shown = viewport_of<MenuViewport>(session, path, ViewportKind::Menu);
	FakeDevice *device = devices.held(path, ViewportKind::Menu);
	TEST_EXPECT(shown && device && shown->status() == ViewportStatus::Ready && shown->forced_index() >= 0);
	if (!shown || !device) return 1;

	// The device's frame at two clocks: its caret drawn in the blink's shown half alone, with no
	// configure between (one compile of the screen, its state's clock set).
	MenuScreenRender render;
	TEST_EXPECT(render.configure(*menu, shown->screen_row(), *view.findings.assets, shown->style_vars()) ==
			MenuScreenStatus::Ready);
	opennova::menu::MenuFrameState state = render.state();
	apply_menu_options(shown->options(), shown->forced_index(), render.compiler(), state);
	const auto glyphs_at = [&](uint32_t ms) {
		PreviewClock clock;
		clock.seek_ms(ms);
		state.time_ms = menu_frame_time(clock);
		render.set_state(state);
		return render.compile(1.0f, 1.0f).glyphs.size();
	};
	const size_t hidden = glyphs_at(0x100), drawn = glyphs_at(0x300), hidden_again = glyphs_at(0x500);
	TEST_EXPECT(drawn > hidden && hidden_again == hidden);

	// The session's clock runs a second, its devices ticked each frame as the Shell's are: nothing
	// taken, and the frame's clock set only where the caret's half of the blink changed (a 1,024 ms
	// cycle: one to three times in a second's thirty frames), each set the other half.
	const auto run = [&](int frames) {
		for (int frame = 0; frame < frames; ++frame) {
			session.advance(1.0 / 30.0);
			devices.sync(session);
			devices.cache.tick(session.viewports());
		}
	};
	const size_t from = device->taken.size();
	const size_t configures = shown->configures();
	const uint32_t was = menu_frame_time(session.viewports().clock());
	size_t ticks = device->ticks, sets = device->clock_sets.size();
	run(30);
	TEST_EXPECT(device->since(from).empty() && shown->configures() == configures);
	TEST_EXPECT(menu_frame_time(session.viewports().clock()) - was >= 990 && device->ticks - ticks == 30);
	const size_t blinked = device->clock_sets.size() - sets;
	TEST_EXPECT(blinked >= 1 && blinked <= 3);
	for (size_t i = sets + 1; i < device->clock_sets.size(); ++i)
		TEST_EXPECT(opennova::menu::menu_caret_shown(device->clock_sets[i]) !=
				opennova::menu::menu_caret_shown(device->clock_sets[i - 1]));
	TEST_EXPECT(!device->clock_sets.empty() &&
			opennova::menu::menu_caret_shown(device->frame_ms) ==
					opennova::menu::menu_caret_shown(menu_frame_time(session.viewports().clock())));
	// The clock paused: no set, however many frames pass.
	session.handle(request::set_viewport(path, R"({"clock": {"playing": false}})"));
	TEST_EXPECT(session.outcome().done());
	sets = device->clock_sets.size();
	run(30);
	TEST_EXPECT(device->clock_sets.size() == sets);
	// The edit box let go (focus off): the clock reads nothing on the screen, no set as it plays.
	session.handle(request::set_viewport(path, R"({"clock": {"playing": true}, "options": {"focus": false}})"));
	TEST_EXPECT(session.outcome().done());
	sets = device->clock_sets.size();
	ticks = device->ticks;
	run(60);
	TEST_EXPECT(device->ticks - ticks == 60 && device->clock_sets.size() == sets);
	std::printf("test_menu_clock passed\n");
	return 0;
}

namespace {

// The files one mount of the packed install serves, decoded.
class MountedFiles : public opennova::FileSource {
public:
	explicit MountedFiles(const opennova::Vfs &vfs) : vfs_(vfs) {}
	bool read(const std::string &name, std::vector<uint8_t> &out) const override { return vfs_.read_file(name, out); }
	uint64_t stamp(const std::string &name) const override { return vfs_.has_file(name) ? 1 : 0; }

private:
	const opennova::Vfs &vfs_;
};

} // namespace

// The retail leg (OPENNOVA_JO_DIR): a shipped screen's edit box blinks its caret on the preview
// clock. The .mnu files the packed install serves, read with the files that mount serves, until a
// screen holding an edit box whose caret draws (a font that loads, not read only), which is mp.mnu's
// MULTI_PLAYER_HOST and its GAME_NAME box: held focused as a viewport's options hold it, its frame
// configured once and compiled at the clock's every quarter of the blink, 198 glyphs in the blink's
// hidden half and 199 (the caret) in its shown half.
static int test_retail_animated_menu() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (a shipped screen's edit box blinking its caret on the preview clock)");
		return 0;
	}
	opennova::Vfs vfs;
	if (!vfs.mount_game(install, "", opennova::VfsMountMode::Packed)) {
		std::printf("  FAIL mount_game: %s\n", vfs.last_error().c_str());
		return 1;
	}
	const MountedFiles files(vfs);
	opennova::menu::MenuStyleSource style;
	const std::map<std::string, std::string> &vars = style.vars(files);
	editor_test::TempProjectDir scratch("opennova_viewport_retail_clock");
	size_t menus = 0, screens = 0;
	for (const auto &location : vfs.list_files()) {
		const std::string &name = location.logical_name;
		if (name.size() < 4 || retail::lower_ascii(name.substr(name.size() - 4)) != ".mnu") continue;
		std::vector<uint8_t> stored;
		const std::string at = scratch.file(name.c_str());
		if (!vfs.read_file_raw(name, stored) || !editor_test::write_bytes(at, stored)) continue;
		MnuDocument document;
		Diagnostic error;
		if (!document.load(at, name, AssetKind::Menu, "jo", error) || document.blocked()) continue;
		++menus;
		for (const auto &row : document.rows()) {
			++screens;
			MenuScreenRender render;
			if (render.configure(document, row->id, files, vars) != MenuScreenStatus::Ready) continue;
			const opennova::menu::MenuFrameCompiler &compiler = render.compiler();
			for (int index = 0; index < compiler.widget_count(); ++index) {
				const int type = compiler.widget_kind(index);
				if (type != int(opennova::mnu::WindowType::Edit) && type != int(opennova::mnu::WindowType::MultilineEdit))
					continue;
				MenuViewportOptions options;
				options.force_window = document.window_at(*row, size_t(index));
				options.focused = true;
				opennova::menu::MenuFrameState state = render.state();
				apply_menu_options(options, index, compiler, state);
				// The glyphs drawn at each quarter of the blink's 1,024 milliseconds.
				size_t glyphs[4] = {};
				for (uint32_t quarter = 0; quarter < 4; ++quarter) {
					PreviewClock clock;
					clock.seek_ms(quarter * 0x100u + 0x80u);
					state.time_ms = menu_frame_time(clock);
					render.set_state(state);
					glyphs[quarter] = render.compile(1.0f, 1.0f).glyphs.size();
				}
				if (!(glyphs[2] > glyphs[0])) continue; // no caret drawn (no font, read only)
				// The first is mp.mnu's MULTI_PLAYER_HOST, its GAME_NAME box: 198 glyphs, the caret one more.
				TEST_EXPECT(retail::lower_ascii(name) == "mp.mnu" && row->name() == "MULTI_PLAYER_HOST" &&
						compiler.widget_name(index) == "GAME_NAME");
				TEST_EXPECT(glyphs[0] == 198 && glyphs[1] == 198 && glyphs[2] == 199 && glyphs[3] == 199);
				return 0;
			}
		}
	}
	std::printf("  FAIL no shipped screen holds an edit box whose caret draws (%zu menus, %zu screens)\n", menus, screens);
	return 1;
}

// S13 V6: devices that build their pictures over the Shell's frames, a unit a step (FakeDevice with
// `units`), through a real session. A build of four units: its viewport loading for three frames
// (the envelope's status, reason and progress, nothing of it drawn), ready after the fourth, one
// Rebuild across its frames, each unit moving the Viewports concern and built frames moving nothing; a
// Rebuild while one runs dropping it and beginning the newer generation from its first unit, the last
// picture drawn until it ends; an Update while one runs folded into it (never taken, the build ending
// with the state as it then is), one after it taken; a Clear while one runs dropping it with the
// picture; a failure at a unit keeping the last picture (failed, its message), nothing asked again and
// an Update folded until the document moves; the frame's budget (two units a frame), and two builds in
// one frame sharing it, the most recently used device's first, one unit a frame in all.
static int test_builds() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_builds");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	FakeDevices devices;
	devices.units = 4;
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Builds"));
	session.run_operations();
	editor_test::create_missing_files(session);
	TEST_EXPECT(editor_test::write_bytes(view.project.root + "/models/armory.3di", test_io::read_file(synth("armory.3di"))));
	session.handle(request::rescan());
	session.run_operations();
	const std::string path = "models/armory.3di";
	session.handle(request::open_document(path));
	auto *document = dynamic_cast<ModelDocument *>(session.document_for(path));
	const auto *model = viewport_of<ModelViewport>(session, path, ViewportKind::Model);
	TEST_EXPECT(document && document->model_row() && model);
	if (!document || !document->model_row() || !model) return 1;
	const ModelRow &row = *document->model_row();
	TEST_EXPECT(!row.ids.lists[kModelLights].empty());
	if (row.ids.lists[kModelLights].empty()) return 1;
	const NodeAddress light{ row.id, node_kind(ModelKind::Light), row.ids.lists[kModelLights][0].id };
	const auto envelope = [&] { return viewport_to_json(view, *model, JsonPage()); };
	const auto moves = [&] { return view.revisions.of(ViewConcern::Viewports); };
	const auto count = [](const std::vector<ViewportAction> &actions, ViewportAction action) {
		size_t n = 0;
		for (const ViewportAction taken : actions) n += taken == action ? 1 : 0;
		return n;
	};

	// Loading for three frames, a unit a frame, nothing of it drawn; ready after the fourth, the
	// picture of generation 1 drawn. One Rebuild across the build's frames.
	for (uint64_t frame = 1; frame <= 4; ++frame) {
		const uint64_t at = moves();
		devices.frame(session);
		const FakeDevice *held = devices.held(path, ViewportKind::Model);
		TEST_EXPECT(held != nullptr);
		if (!held) return 1;
		const JsonValue json = envelope();
		TEST_EXPECT(moves() > at && model->status() == ViewportStatus::Ready);
		if (frame < 4) {
			TEST_EXPECT(model->picture_status() == ViewportStatus::Loading && json.get_string("status", "") == "loading" &&
					json.get_string("reason", "") == "loading");
			const JsonValue *progress = json.get("progress");
			TEST_EXPECT(progress && progress->get_number("generation", 0) == 1.0 &&
					progress->get_number("done", -1) == double(frame) && progress->get_number("total", -1) == 4.0 &&
					progress->get_string("unit", "") == "steps" && progress->get_string("label", "") == "units");
			TEST_EXPECT(held->shown == 0 && json.get("device")->get("build")->get_bool("loading", false));
		} else {
			TEST_EXPECT(model->picture_status() == ViewportStatus::Ready && json.get_string("status", "") == "ready" &&
					json.get_string("reason", "") == "ready" && json.get("progress")->is_null() && held->shown == 1);
			const JsonValue &build = *json.get("device")->get("build");
			TEST_EXPECT(build.get_number("generation", 0) == 1.0 && !build.get_bool("loading", true) &&
					build.get_number("done", 0) == 4.0 && build.get_number("total", 0) == 4.0);
		}
		TEST_EXPECT(model->builds() == 1 && count(held->taken, ViewportAction::Rebuild) == 1);
	}
	FakeDevice *device = devices.held(path, ViewportKind::Model);
	if (!device) return 1;
	// Built: frames with nothing asked step nothing and move nothing.
	uint64_t at = moves();
	const int steps = device->steps;
	devices.frame(session);
	devices.frame(session);
	TEST_EXPECT(moves() == at && device->steps == steps && device->last() == ViewportAction::Keep);

	// A Rebuild while one runs (a light's colour, which the scene draws, at its second unit):
	// generation 2's build dropped and generation 3's begun from its first unit, the picture of
	// generation 1 drawn until it ends, generation 2's never.
	set(session, *document, light, "start.r", int64_t(12));
	devices.frame(session);
	devices.frame(session);
	TEST_EXPECT(model->builds() == 2 && device->built.generation == 2 && device->built.progress.done == 2 &&
			device->shown == 1);
	device->taken_building.clear();
	set(session, *document, light, "start.r", int64_t(13));
	devices.frame(session);
	TEST_EXPECT(model->builds() == 3 && device->built.generation == 3 && device->built.progress.done == 1 &&
			device->shown == 1 && model->picture_status() == ViewportStatus::Loading);
	// The progress is the newer generation's, begun again (A1's never goes back within one).
	TEST_EXPECT(envelope().get("progress")->get_number("generation", 0) == 3.0 &&
			envelope().get("progress")->get_number("done", 0) == 1.0);
	TEST_EXPECT(count(device->taken_building, ViewportAction::Rebuild) == 1);
	for (int frame = 0; frame < 3; ++frame) devices.frame(session);
	TEST_EXPECT(device->shown == 3 && model->picture_status() == ViewportStatus::Ready &&
			count(device->taken, ViewportAction::Rebuild) == 3);
	TEST_EXPECT(envelope().get("device")->get("build")->get_number("generation", 0) == 3.0 &&
			envelope().get_number("builds", 0) == 3.0);

	// An Update while one runs (a level held): folded into it, taken neither while it runs nor after;
	// the build ends with the level as it then is. One after the build ends is taken.
	device->taken_building.clear();
	set(session, *document, light, "start.r", int64_t(14));
	devices.frame(session);
	session.handle(request::set_viewport(path, R"({"options": {"lod": 0}})"));
	TEST_EXPECT(session.outcome().done());
	for (int frame = 0; frame < 4; ++frame) devices.frame(session);
	TEST_EXPECT(device->shown == 4 && device->ended_lod == 0 && model->options().lod == 0);
	TEST_EXPECT(count(device->taken_building, ViewportAction::Update) == 0 && count(device->taken, ViewportAction::Update) == 0);
	session.handle(request::set_viewport(path, R"({"options": {"lod": "auto"}})"));
	size_t from = device->taken.size();
	devices.frame(session);
	TEST_EXPECT(device->since(from) == (Actions{ ViewportAction::Update }) && model->builds() == 4);

	// A failure at the second unit: the last picture kept (generation 4's), the envelope failed with
	// the device's message; nothing asked again while nothing moves and an Update folded (no picture
	// of the newest generation to apply it to); the document moved, the next generation's picture.
	device->fail_at = 2;
	set(session, *document, light, "start.r", int64_t(15));
	devices.frame(session);
	devices.frame(session);
	const JsonValue failed = envelope();
	TEST_EXPECT(model->picture_status() == ViewportStatus::Failed && model->status() == ViewportStatus::Ready &&
			device->shown == 4);
	TEST_EXPECT(failed.get_string("status", "") == "failed" && failed.get_string("reason", "") == "build_failed" &&
			failed.get_string("message", "") == "Unit 2 failed." && failed.get("progress")->is_null() &&
			failed.get("device")->get("build")->get_bool("failed", false));
	from = device->taken.size();
	session.handle(request::set_viewport(path, R"({"options": {"lod": 0}})"));
	for (int frame = 0; frame < 3; ++frame) devices.frame(session);
	TEST_EXPECT(device->since(from).empty() && model->builds() == 5 && device->steps == steps + 12);
	device->fail_at = 0;
	set(session, *document, light, "start.r", int64_t(16));
	for (int frame = 0; frame < 4; ++frame) devices.frame(session);
	TEST_EXPECT(device->shown == 6 && model->picture_status() == ViewportStatus::Ready && device->ended_lod == 0);

	// The frame's budget: two units a frame, a build of four drawn after its second frame.
	set(session, *document, light, "start.r", int64_t(17));
	devices.frame(session, 2);
	TEST_EXPECT(device->built.progress.done == 2 && device->shown == 6);
	devices.frame(session, 2);
	TEST_EXPECT(device->shown == 7 && model->picture_status() == ViewportStatus::Ready);

	// Two builds in one frame share its budget, the most recently used device's first: one unit a
	// frame in all, the model's (the pump uses the Preview's targets in kind order, the model's last);
	// the menu's once a canvas asked for its device after the pump (as a Shell's draw does); four units
	// end the model's build and give the menu's the unit left. A Clear while one runs: the menu's
	// device (three units a build) building after an edit, the menu made one the game could not read:
	// the build dropped with the picture; undone, the next generation's picture.
	devices.units = 3;
	session.handle(request::open_document("main.mnu"));
	Document *menu = session.document_for("main.mnu");
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	const std::string menu_path = menu->path();
	const auto *shown_menu = viewport_of<MenuViewport>(session, menu_path, ViewportKind::Menu);
	for (int frame = 0; frame < 3; ++frame) devices.frame(session);
	FakeDevice *menu_device = devices.held(menu_path, ViewportKind::Menu);
	TEST_EXPECT(menu_device && shown_menu && menu_device->shown == 1 &&
			shown_menu->picture_status() == ViewportStatus::Ready);
	if (!menu_device || !shown_menu) return 1;
	const NodeAddress title = named(*menu, "TITLE"), exit = named(*menu, "EXIT");
	set(session, *menu, title, "position.left", int64_t(8));
	set(session, *document, light, "start.r", int64_t(18));
	devices.frame(session);
	TEST_EXPECT(menu_device->built.loading && menu_device->built.progress.done == 0 && device->built.loading &&
			device->built.progress.done == 1);
	devices.sync(session);
	TEST_EXPECT(devices.cache.device(menu_path, ViewportKind::Menu) == menu_device);
	int left = 1;
	devices.cache.step(session.viewports(), [&left] { return --left > 0; });
	TEST_EXPECT(menu_device->built.progress.done == 1 && device->built.progress.done == 1);
	devices.frame(session, 4);
	TEST_EXPECT(!device->built.loading && menu_device->built.loading && menu_device->built.progress.done == 2);
	menu_device->taken_building.clear();
	set(session, *menu, exit, "string.justify", std::string("CEN\"TER"));
	devices.frame(session);
	TEST_EXPECT(menu_device->taken_building == (Actions{ ViewportAction::Clear }) && !menu_device->built.loading &&
			menu_device->shown == 0);
	TEST_EXPECT(shown_menu->status() == ViewportStatus::Failed && shown_menu->picture_status() == ViewportStatus::Failed);
	session.handle(request::undo(menu->path()));
	for (int frame = 0; frame < 3; ++frame) devices.frame(session);
	TEST_EXPECT(menu_device->shown == shown_menu->builds() && shown_menu->builds() == 3 &&
			shown_menu->picture_status() == ViewportStatus::Ready);
	std::printf("test_builds passed\n");
	return 0;
}

// S13 V6: a file the device read after the follow last compared the stamps (a build's unit, between
// two pumps) is compared at the next follow, though the files' generation stands: one written again
// after its read and before its report is seen; a file read again adds nothing to compare.
static int test_follow_reads() {
	struct Stamps final : opennova::FileSource {
		std::vector<std::pair<std::string, uint64_t>> stamps;
		bool read(const std::string &, std::vector<uint8_t> &) const override { return false; }
		uint64_t stamp(const std::string &name) const override {
			for (const auto &entry : stamps)
				if (entry.first == name) return entry.second;
			return 0;
		}
	};
	Stamps files;
	files.stamps.push_back({ "wall.tga", 1 });
	const PreviewFollow::Key key;
	PreviewFollow follow;
	follow.show(key, 7);
	follow.built(FileStamps());
	TEST_EXPECT(follow.follow(key, false, files, 7) == PreviewFollow::Found::Same);
	// The texture read at stamp 1 by a unit, then written again (2), the generation moving to 8 before
	// the device reported its read: compared once reported, though the generation stands at 8.
	files.stamps[0].second = 2;
	TEST_EXPECT(follow.follow(key, false, files, 8) == PreviewFollow::Found::Same);
	FileStamps read;
	read.note("wall.tga", 1);
	follow.read(read);
	TEST_EXPECT(follow.files_moved(files, 8));
	TEST_EXPECT(follow.follow(key, false, files, 8) == PreviewFollow::Found::Files);
	// Made again over the file as it stands: read at 2, compared once, then nothing until it moves.
	follow.built(FileStamps());
	read.clear();
	read.note("wall.tga", 2);
	follow.read(read);
	TEST_EXPECT(follow.follow(key, false, files, 8) == PreviewFollow::Found::Same);
	follow.read(read);
	TEST_EXPECT(!follow.files_moved(files, 8) && follow.follow(key, false, files, 8) == PreviewFollow::Found::Same);
	std::printf("test_follow_reads passed\n");
	return 0;
}

// ADR 0046 S14: a kind may keep fewer devices than the cache (ViewportKindRow::devices; here a test's
// limit of two menus): the third menu given a device gives up the least recently used menu's, the
// model's device untouched and the capacity not reached; most_recently_used is the device used last;
// a device holds no picture until its first build ends.
static int test_kind_limit() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_kind_limit");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	const SessionView &view = session.view();
	std::vector<editor_test::FakeDevice *> made;
	ViewportDeviceCache cache(
			[&made](ViewportKind) {
				auto device = std::make_unique<editor_test::FakeDevice>();
				made.push_back(device.get());
				return std::unique_ptr<ViewportDevice>(std::move(device));
			},
			4, [](ViewportKind kind) { return kind == ViewportKind::Menu ? size_t(2) : size_t(0); });
	const auto sync = [&] { cache.sync(session.viewports(), view); };
	session.handle(request::new_project(dir.file("project"), "Kind Limit"));
	session.run_operations();
	editor_test::create_missing_files(session);
	TEST_EXPECT(editor_test::write_bytes(view.project.root + "/models/armory.3di", test_io::read_file(synth("armory.3di"))));
	session.handle(request::rescan());
	session.run_operations();
	editor_test::FakeDevice fresh;
	TEST_EXPECT(!fresh.holds_picture() && cache.most_recently_used() == nullptr);
	session.handle(request::open_document("models/armory.3di"));
	sync();
	TEST_EXPECT(cache.size() == 1 && made.size() == 1 && cache.most_recently_used() == made[0] && made[0]->holds_picture());
	session.handle(request::open_document("main.mnu"));
	sync();
	const std::string main_path = session.document_for("main.mnu")->path();
	TEST_EXPECT(cache.size() == 2 && cache.held(main_path, ViewportKind::Menu) == made[1] && cache.most_recently_used() == made[1]);
	session.handle(request::create_file("a.mnu", "menu"));
	sync();
	const std::string a_path = session.document_for("a.mnu")->path();
	TEST_EXPECT(cache.size() == 3 && cache.held(a_path, ViewportKind::Menu) && cache.held(main_path, ViewportKind::Menu));
	// A third menu: the menu's limit of two gives up the least recently used menu (main's), though the
	// cache holds three of its four.
	session.handle(request::create_file("b.mnu", "menu"));
	sync();
	const std::string b_path = session.document_for("b.mnu")->path();
	TEST_EXPECT(cache.size() == 3 && made.size() == 4);
	TEST_EXPECT(cache.held(b_path, ViewportKind::Menu) == made[3] && cache.held(a_path, ViewportKind::Menu) &&
			!cache.held(main_path, ViewportKind::Menu) && cache.held("models/armory.3di", ViewportKind::Model) == made[0]);
	TEST_EXPECT(!session.viewports().find(main_path, ViewportKind::Menu)->attached());
	TEST_EXPECT(cache.most_recently_used() == made[3]);
	// The model's device asked for: now the most recently used; the menus' limit stands as main's view
	// asks for its device back (a's, the least recently used menu, given up at the next sync).
	TEST_EXPECT(cache.device("models/armory.3di", ViewportKind::Model) == made[0] && cache.most_recently_used() == made[0]);
	TEST_EXPECT(cache.device(main_path, ViewportKind::Menu) == nullptr);
	sync();
	TEST_EXPECT(cache.size() == 3 && made.size() == 5 && cache.held(main_path, ViewportKind::Menu) == made[4] &&
			!cache.held(a_path, ViewportKind::Menu) && cache.held(b_path, ViewportKind::Menu) == made[3]);
	std::printf("test_kind_limit passed\n");
	return 0;
}

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	TEST_EXPECT(test_kind_limit() == 0);
	TEST_EXPECT(test_actions() == 0);
	TEST_EXPECT(test_clock() == 0);
	TEST_EXPECT(test_two_menus() == 0);
	TEST_EXPECT(test_device_cache() == 0);
	TEST_EXPECT(test_cache_rounds() == 0);
	TEST_EXPECT(test_state_moves() == 0);
	TEST_EXPECT(test_envelope() == 0);
	TEST_EXPECT(test_requests() == 0);
	TEST_EXPECT(test_gestures() == 0);
	TEST_EXPECT(test_menu_change_sets() == 0);
	TEST_EXPECT(test_menu_shared_texture() == 0);
	TEST_EXPECT(test_gesture_holds_rebuild() == 0);
	TEST_EXPECT(test_hold_needs_a_picture() == 0);
	TEST_EXPECT(test_menu_clock() == 0);
	TEST_EXPECT(test_retail_animated_menu() == 0);
	TEST_EXPECT(test_builds() == 0);
	TEST_EXPECT(test_follow_reads() == 0);
	std::printf("editor_viewport: all tests passed\n");
	return 0;
}
