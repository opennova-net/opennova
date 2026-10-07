// The HUD preview over hudpos.def (the editor deep-integration plan's DI-20): the HUD layout type
// (hudpos.def held as its text, its line ends the game's reader's, the lines its parser is handed and
// the one the game takes of each key), and the HUD viewport through a session: hudpos.def opened shows
// its HUD in the Preview window, its device asked to make the picture and made to again when the text
// changes; the viewport query's state (the screen, the HUD font at its width, the weapons weapon.def
// holds, the stances' names); a SetViewport of its options and a refused one; the elements the device
// said it drew named by a point (the smallest box first), each with the hudpos.def lines that place it
// and the textures it draws, a click picking one; the graph's names of the layout following its text
// as it stands; an LF alone the line-ends rule's finding. The canvas: what a hover rings and says, what a
// click and Esc raise.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/document_types.h>
#include <editor/documents/hud_layout_type.h>
#include <editor/documents/line_ends.h>
#include <editor/model/text_document.h>
#include <editor/preview/hud_canvas.h>
#include <editor/preview/hud_viewport.h>
#include <editor/preview/viewport_kinds.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <runtime/hud/hud_elements.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova::editor;
using opennova::hud::HudElement;
using opennova::io::JsonValue;

namespace {

// A soldier panel's layout, every line CR LF as the game's reader ends them.
const char *const kLayout =
		"// The soldier panel\r\n"
		"fonthud1_hi\tonhudb18.fnt\r\n"
		"fonthud1_lo\tonhud14.fnt\r\n"
		"StaticFrame\told.tga\t0,0\r\n"
		"StaticFrame\tonhframe.tga\t6,586\r\n"
		"AMMOCOUNTPOS\t168,616,0,right\r\n"
		"HUDWEAPONNAME\t14,588,0,left\r\n"
		"HUDSTANCEPOS\t30,639\r\n"
		"HUDSTANCE 0\t0 0 onhstnc0.tga STAND\r\n"
		"HUDSTANCE 1\t0 0 onhstnc1.tga CROUCH\r\n"
		"HUDHEALTH\t25,741,177,751\r\n"
		"HUDCLIP\t14,648\r\n";

const char *const kWeapons =
		"weapon \"W_AR15\"\r\n"
		"\tclipsize 30\r\n"
		"\thudicon h_onar15.tga\r\n"
		"\thudclipgfx 0 0 h_onclip.tga\r\n"
		"\thudrndgfx 6 0 4 0 1 h_onrnd.tga\r\n"
		"end\r\n"
		"weapon \"W_PISTOL\"\r\n"
		"\tclipsize 7\r\n"
		"end\r\n";

int test_layout_type() {
	const DocumentType *type = document_type_for(AssetKind::HudPosDefs);
	TEST_EXPECT(type && type->id == DocumentTypeId::HudLayout && document_content(*type) == DocumentContent::Text);
	// Its lines as the parser is handed them: the comment none, each key's line where it starts.
	const std::string text = kLayout;
	const std::vector<HudLayoutLine> lines = hud_layout_lines(text);
	TEST_EXPECT(lines.size() == 11 && lines[0].key == "fonthud1_hi" && lines[0].first == "onhudb18.fnt");
	// The game takes a key's last line (two StaticFrames: the second), a HUDSTANCE by its id.
	const HudLayoutLine *frame = hud_layout_line(lines, "STATICFRAME");
	TEST_EXPECT(frame && frame->first == "onhframe.tga" && text.compare(frame->offset, 11, "StaticFrame") == 0);
	const HudLayoutLine *crouch = hud_layout_line(lines, "hudstance", "1");
	TEST_EXPECT(crouch && text.compare(crouch->offset, crouch->length, "HUDSTANCE 1\t0 0 onhstnc1.tga CROUCH") == 0);
	TEST_EXPECT(hud_layout_line(lines, "HUDSTANCE", "4") == nullptr && hud_layout_line(lines, "HUDHEAT") == nullptr);

	// The document: its text as the file holds it, no finding; an LF alone the line-ends rule's warning.
	std::unique_ptr<DocumentBase> document = type->make();
	Diagnostic error;
	TEST_EXPECT(document->load_bytes(std::vector<uint8_t>(text.begin(), text.end()), "hudpos.def", AssetKind::HudPosDefs, "jo",
	                                 error));
	TEST_EXPECT(text_of(*document) && text_of(*document)->text() == text && type->validate_file(*document).empty());
	const std::string loose = "HUDHEALTH 25,741,177,751\nHUDCLIP 14,648\r\n";
	std::unique_ptr<DocumentBase> lf = type->make();
	TEST_EXPECT(lf->load_bytes(std::vector<uint8_t>(loose.begin(), loose.end()), "hudpos.def", AssetKind::HudPosDefs, "jo",
	                           error));
	// The line-ends rule's (documents/line_ends.h): the type's own validator makes none.
	TEST_EXPECT(type->validate_file(*lf).empty());
	const std::vector<Diagnostic> findings = line_end_findings(*lf, "jo");
	TEST_EXPECT(findings.size() == 1 && findings[0].code() == "document.line_ends" && findings[0].line == 1);
	// Save writes it CR LF.
	const SerializeResult written = lf->serialize();
	TEST_EXPECT(written.ok() && written.text == "HUDHEALTH 25,741,177,751\r\nHUDCLIP 14,648\r\n");
	std::printf("layout type: its lines, the line the game takes, an LF alone a finding\n");
	return 0;
}

struct Rig {
	editor_test::TempProjectDir dir{"opennova_editor_hud_viewport"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::FakeDevices devices;
	const std::string layout = "defs/hudpos.def";
	std::string root() const { return dir.file("project"); }
	const SessionView &view() const { return session.view(); }
	const HudViewport *viewport() {
		return static_cast<const HudViewport *>(session.viewports().find(layout, ViewportKind::Hud));
	}
	void pump() {
		session.run_operations();
		devices.sync(session);
	}
	JsonValue query(const std::string &name, const std::string &args_json) {
		JsonValue args;
		std::string error;
		opennova::io::json_parse(args_json, args, error);
		return session.query(name, args, error);
	}
};

ViewportDeviceReport::Rect rect(int left, int top, int right, int bottom) {
	return ViewportDeviceReport::Rect{ true, left, top, right, bottom };
}

int test_session() {
	Rig rig;
	editor_test::handle_to_end(rig.session, request::new_project(rig.root(), "HUD"));
	editor_test::create_missing_files(rig.session);
	TEST_EXPECT(editor_test::write_text(rig.root() + "/" + rig.layout, kLayout) &&
	            editor_test::write_text(rig.root() + "/defs/weapon.def", kWeapons) &&
	            editor_test::write_text(rig.root() + "/textures/onhframe.tga", "not read here") &&
	            editor_test::write_text(rig.root() + "/textures/h_onclip.tga", "not read here"));
	editor_test::handle_to_end(rig.session, request::rescan());
	rig.pump();

	// Opened: its text in the Document tab, its HUD in the Preview window.
	editor_test::handle_to_end(rig.session, request::open_document(rig.layout));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	TEST_EXPECT(rig.view().documents.preview_shown == ViewportKind::Hud &&
	            rig.view().documents.previews[ViewportKind::Hud].path == rig.layout);
	TEST_EXPECT(main_viewport_kind(DocumentTypeId::HudLayout) == ViewportKind::Script);
	const HudViewport *viewport = rig.viewport();
	TEST_EXPECT(viewport && viewport->status() == ViewportStatus::Ready);
	editor_test::FakeDevice *device = rig.devices.held(rig.layout, ViewportKind::Hud);
	TEST_EXPECT(device && device->since(0) == std::vector<ViewportAction>{ ViewportAction::Rebuild });
	rig.pump();
	TEST_EXPECT(device->last() == ViewportAction::Keep);

	// Its state: the design screen, the HUD font at 1024 (FONTHUD1_HI), weapon.def's weapons (the first
	// shown by default), the stances' names, the frame the layout's last StaticFrame names.
	JsonValue state = rig.query("viewport", "{\"path\":\"" + rig.layout + "\",\"op\":\"state\"}");
	TEST_EXPECT(state.get_string("kind", "") == "hud" && state.get_string("status", "") == "ready" &&
	            state.get_string("units", "") == "pixels");
	const JsonValue *body = state.get("body");
	TEST_EXPECT(body && body->get_string("hud_font", "") == "onhudb18.fnt" &&
	            body->get_string("static_frame", "") == "onhframe.tga" && body->get_string("weapon_shown", "") == "W_AR15");
	const JsonValue *weapons = body ? body->get("weapons") : nullptr;
	TEST_EXPECT(weapons && weapons->array.size() == 2 && weapons->array[0].get_string("clip_art", "") == "h_onclip.tga" &&
	            weapons->array[0].get_string("hudicon", "") == "h_onar15.tga");
	const JsonValue *stances = body ? body->get("stances") : nullptr;
	TEST_EXPECT(stances && stances->array.size() == 6 && stances->array[1].get_string("name", "") == "CROUCH" &&
	            stances->array[1].get_string("texture", "") == "onhstnc1.tga");

	// The device says where the HUD's walk drew: the frame, the health bar, the ammo count over the frame.
	device->placed.assign(opennova::hud::kHudElementCount, ViewportDeviceReport::Rect());
	device->placed[size_t(HudElement::Frame)] = rect(6, 586, 134, 650);
	device->placed[size_t(HudElement::Health)] = rect(25, 741, 177, 751);
	device->placed[size_t(HudElement::AmmoCount)] = rect(130, 616, 168, 632);
	device->placed[size_t(HudElement::ClipIndicator)] = rect(14, 648, 60, 660);
	rig.pump();
	TEST_EXPECT(viewport->elements().size() == 4);
	// A point names the smallest box that holds it: the ammo count inside the frame, the frame around it.
	JsonValue hit = rig.query("viewport", "{\"path\":\"" + rig.layout + "\",\"op\":\"hit\",\"x\":140,\"y\":620}");
	TEST_EXPECT(hit.get_string("kind", "") == "ammo_count" && hit.get_number("index", -1) == double(HudElement::AmmoCount) &&
	            hit.get_string("name", "").find("AMMOCOUNTPOS (line 6)") != std::string::npos);
	hit = rig.query("viewport", "{\"path\":\"" + rig.layout + "\",\"op\":\"hit\",\"x\":20,\"y\":600}");
	TEST_EXPECT(hit.get_string("kind", "") == "frame");
	hit = rig.query("viewport", "{\"path\":\"" + rig.layout + "\",\"op\":\"hit\",\"x\":500,\"y\":100}");
	TEST_EXPECT(hit.get_number("index", 0) == -1);
	// Each element's lines and textures: the frame's last StaticFrame line and its art, found in the
	// project; the clip's HUDCLIP and the weapon's two graphics (h_onrnd.tga the project lacks).
	JsonValue items = rig.query("viewport", "{\"path\":\"" + rig.layout + "\",\"op\":\"items\"}");
	const JsonValue *rows = items.get("items");
	const JsonValue *frame = nullptr, *clip = nullptr;
	static const std::vector<JsonValue> kNoRows;
	for (const JsonValue &row : rows ? rows->array : kNoRows) {
		if (row.get_string("element", "") == "frame") frame = &row;
		if (row.get_string("element", "") == "clip_indicator") clip = &row;
	}
	TEST_EXPECT(frame && frame->get("lines") && frame->get("lines")->array.size() == 1 &&
	            frame->get("lines")->array[0].get_string("locator", "") == "5:1" &&
	            frame->get("textures")->array.size() == 1 &&
	            frame->get("textures")->array[0].get_string("path", "") == "textures/onhframe.tga");
	TEST_EXPECT(clip && clip->get("lines")->array[0].get_string("key", "") == "HUDCLIP" &&
	            clip->get("textures")->array.size() == 2 &&
	            clip->get("textures")->array[0].get_string("path", "") == "textures/h_onclip.tga" &&
	            clip->get("textures")->array[1].get_string("name", "") == "h_onrnd.tga" &&
	            clip->get("textures")->array[1].get_string("path", "x").empty());

	// The state a SetViewport sets: the stance, no weapon, a smaller screen; a stance past 5 refused.
	editor_test::handle_to_end(rig.session, request::set_viewport(rig.layout,
	        "{\"kind\":\"hud\",\"options\":{\"stance\":1,\"weapon\":\"NONE\",\"width\":640,\"height\":480}}"));
	TEST_EXPECT(rig.session.outcome().done() && viewport->options().stance == 1 && viewport->options().width == 640 &&
	            viewport->weapon_shown() == nullptr && viewport->layout().design_width == 640);
	state = rig.query("viewport", "{\"path\":\"" + rig.layout + "\",\"op\":\"state\"}");
	TEST_EXPECT(state.get("body") && state.get("body")->get_string("hud_font", "") == "onhud14.fnt");
	editor_test::handle_to_end(rig.session, request::set_viewport(rig.layout, "{\"kind\":\"hud\",\"options\":{\"stance\":7}}"));
	TEST_EXPECT(!rig.session.outcome().done() && viewport->options().stance == 1);
	editor_test::handle_to_end(rig.session, request::set_viewport(rig.layout,
	        "{\"kind\":\"hud\",\"options\":{\"weapon\":\"W_PISTOL\",\"width\":1024,\"height\":768}}"));
	TEST_EXPECT(viewport->weapon_shown() && viewport->weapon_shown()->name == "W_PISTOL");
	rig.pump();

	// A click picks the element under it (the canvas's own click, driven through the wire).
	ViewportCommand click;
	click.name = "click";
	click.has_at = true;
	click.at_x = 140.0f;
	click.at_y = 620.0f;
	editor_test::handle_to_end(rig.session, request::edit_in_viewport(rig.layout, click));
	TEST_EXPECT(rig.session.outcome().done() && viewport->options().picked == "ammo_count" && viewport->picked() &&
	            viewport->picked()->element == HudElement::AmmoCount);
	click.mode = SelectMode::Add;
	editor_test::handle_to_end(rig.session, request::edit_in_viewport(rig.layout, click));
	TEST_EXPECT(!rig.session.outcome().done());
	// Nothing is dragged.
	ViewportDrag drag;
	drag.id = 1;
	drag.handle = "move";
	editor_test::handle_to_end(rig.session, request::edit_in_viewport(rig.layout, drag));
	TEST_EXPECT(!rig.session.outcome().done());

	// An edit of the text: the picture made again, and the graph's names follow the text as it stands.
	const TextDocument *text = nullptr;
	for (const auto &open : rig.view().documents.open)
		if (open && open->path() == rig.layout) text = text_of(*open);
	TEST_EXPECT(text != nullptr);
	if (!text) return 1;
	const size_t at = text->text().find("onhframe.tga");
	editor_test::handle_to_end(rig.session,
	        request::edit_record(rig.layout, TextDocument::replace(text->span_at(at, 8), "newframe")));
	TEST_EXPECT(rig.session.outcome().done());
	editor_test::handle_to_end(rig.session, request::end_edit(rig.layout));
	rig.pump();
	TEST_EXPECT(device->last() == ViewportAction::Rebuild && viewport->assets().static_frame == "newframe.tga");
	const JsonValue references = rig.query("references", "{\"path\":\"" + rig.layout + "\"}");
	bool newframe = false, oldframe = false;
	if (const JsonValue *list = references.get("edges"))
		for (const JsonValue &row : list->array) {
			newframe = newframe || row.get_string("value", "") == "newframe.tga";
			oldframe = oldframe || row.get_string("value", "") == "onhframe.tga";
		}
	TEST_EXPECT(newframe && !oldframe);
	std::printf("session: opened, its HUD previewed, its state set, an element named, picked, its text followed\n");
	return 0;
}

int test_canvas() {
	Rig rig;
	editor_test::handle_to_end(rig.session, request::new_project(rig.root(), "HUD"));
	editor_test::create_missing_files(rig.session);
	TEST_EXPECT(editor_test::write_text(rig.root() + "/" + rig.layout, kLayout));
	editor_test::handle_to_end(rig.session, request::rescan());
	editor_test::handle_to_end(rig.session, request::open_document(rig.layout));
	rig.pump();
	editor_test::FakeDevice *device = rig.devices.held(rig.layout, ViewportKind::Hud);
	TEST_EXPECT(device != nullptr);
	if (!device) return 1;
	device->placed.assign(opennova::hud::kHudElementCount, ViewportDeviceReport::Rect());
	device->placed[size_t(HudElement::Health)] = rect(25, 741, 177, 751);
	rig.pump();
	const HudViewport *viewport = rig.viewport();
	TEST_EXPECT(viewport && viewport->elements().size() == 1);
	if (!viewport) return 1;
	// The canvas fits the 1024 x 768 screen into a 512 x 384 picture: the health bar under (50, 373).
	HudCanvas canvas;
	editor_test::Gathered out;
	const ViewportContext context = viewport_context(rig.view(), *viewport);
	canvas.follow(*viewport, context, out);
	CanvasInput in;
	in.width = 512;
	in.height = 384;
	in.mouse = in.screen = CanvasPoint{ 50.0f, 373.0f };
	in.hovered = true;
	const std::string tip = canvas.hover_tip(context, in);
	TEST_EXPECT(tip.find("Health bar: HUDHEALTH (line 11)") == 0);
	const OverlayList shapes = canvas.shapes(context, in);
	TEST_EXPECT(shapes.shapes.size() == 1 && shapes.shapes[0].role == OverlayRole::Hover &&
	            shapes.shapes[0].points[0].x == 12.5f);
	// A click picks it.
	in.pressed = in.down = true;
	canvas.input(context, in, out);
	in.pressed = in.down = false;
	canvas.input(context, in, out);
	TEST_EXPECT(out.requests.size() == 1 && out.requests[0].kind == EditorRequestKind::SetViewport &&
	            out.requests[0].viewport.find("\"health\"") != std::string::npos);
	std::printf("canvas: a hover names and rings the element, a click picks it\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_layout_type();
	failures += test_session();
	failures += test_canvas();
	if (failures == 0) std::printf("editor_hud_viewport: all passed\n");
	return failures == 0 ? 0 : 1;
}
