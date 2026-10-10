// The HUD preview over hudpos.def (the editor deep-integration plan's DI-20): the HUD layout type
// (hudpos.def held as its text, its line ends the game's reader's), and the HUD viewport through a
// session: hudpos.def opened shows its HUD in the Preview window, its device asked to make the picture and made to again when the text
// changes; the viewport query's state (the screen, the HUD font at its width, the weapons weapon.def
// holds, the stances' names); a SetViewport of its options and a refused one; the elements the device
// said it drew named by a point (the smallest box first), each with the hudpos.def lines that place it
// and the textures it draws, a click picking one; the graph's names of the layout following its text
// as it stands; an LF alone the line-ends rule's finding. The canvas: what a hover rings and says, what a
// click and Esc raise.
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <base/gameprofile/game_type.h>
#include <base/io/json.h>
#include <editor/documents/document_types.h>
#include <editor/documents/line_ends.h>
#include <editor/model/text_document.h>
#include <editor/preview/hud_canvas.h>
#include <editor/preview/hud_viewport.h>
#include <editor/preview/viewport_kinds.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_json.h>
#include <editor/session/view/session_view.h>
#include <formats/def/def.h>
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
	// Its lines as the parser is handed them, and the line the game takes of a key, are the engine's
	// (formats/def/def_hudpos_text.h, tests/def/def_hudpos_text_test).
	const std::string text = kLayout;
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
	std::printf("layout type: its text as the file holds it, an LF alone a finding\n");
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

// The layout's model as the game's parser reads a text.
struct Model {
	opennova::def::DefHudPosFile file{};
	explicit Model(const std::string &text) {
		opennova::def::def_parse_hudpos_memory(reinterpret_cast<const uint8_t *>(text.data()), text.size(), &file);
	}
	~Model() { opennova::def::def_free_hudpos(&file); }
};

std::unique_ptr<DocumentBase> layout_document(const std::string &text) {
	std::unique_ptr<DocumentBase> document = document_type_for(AssetKind::HudPosDefs)->make();
	Diagnostic error;
	document->load_bytes(std::vector<uint8_t>(text.begin(), text.end()), "hudpos.def", AssetKind::HudPosDefs, "jo", error);
	return document;
}

// The changes applied to a layout's text through its document: the text after them.
std::string edited(const std::string &text, const std::vector<HudValueChange> &changes) {
	std::unique_ptr<DocumentBase> document = layout_document(text);
	Model model(text);
	std::vector<Edit> edits;
	std::string error;
	if (!hud_layout_edits(*text_of(*document), model.file.hud, changes, 0, edits, error)) return "refused: " + error;
	Diagnostic failed;
	if (!edits.empty() && !document->apply(edits, failed)) return "not applied";
	return text_of(*document)->text();
}

int test_layout_edit() {
	using opennova::def::DefHudPosDef;
	const std::string text = kLayout;
	Model model(text);
	const DefHudPosDef &hud = model.file.hud;
	// A move of the ammo count: its two numbers' tokens rewritten in place, the rest of its line as written.
	HudDragStart start;
	TEST_EXPECT(hud_drag_start(HudElement::AmmoCount, hud, start) && start.values == std::vector<int>({ 168, 616 }));
	std::vector<HudValueChange> changes;
	std::string error;
	TEST_EXPECT(hud_drag_changes(start, HudHandle::Move, 10.4f, -6.6f, 1, changes, error) && changes.size() == 2 &&
	            changes[0].value == "178" && changes[1].value == "609");
	std::string after = edited(text, changes);
	TEST_EXPECT(after.find("AMMOCOUNTPOS\t178,609,0,right\r\n") != std::string::npos);
	TEST_EXPECT(after.size() == text.size());
	// On a grid of 8 the lead lands on it: 168 + 10 -> 176, 616 - 6 -> 608.
	TEST_EXPECT(hud_drag_changes(start, HudHandle::Move, 10.0f, -6.0f, 8, changes, error) && changes[0].value == "176" &&
	            changes[1].value == "608");
	// The health bar's corners: a resize of its bottom right moves its far edges alone, a move all four.
	TEST_EXPECT(hud_drag_start(HudElement::Health, hud, start));
	TEST_EXPECT(hud_drag_changes(start, HudHandle::BottomRight, 20.0f, 4.0f, 1, changes, error));
	TEST_EXPECT(edited(text, changes).find("HUDHEALTH\t25,741,197,755\r\n") != std::string::npos);
	TEST_EXPECT(hud_drag_changes(start, HudHandle::TopLeft, 500.0f, 0.0f, 1, changes, error));
	TEST_EXPECT(edited(text, changes).find("HUDHEALTH\t176,741,177,751\r\n") != std::string::npos);
	TEST_EXPECT(!hud_drag_changes(start, HudHandle::Move, 0.0f, 0.0f, 1, changes, error) ||
	            edited(text, changes) == text);
	// The ammo count has no size: a corner of it is refused.
	TEST_EXPECT(hud_drag_start(HudElement::AmmoCount, hud, start) &&
	            !hud_drag_changes(start, HudHandle::BottomRight, 1.0f, 1.0f, 1, changes, error));
	// A power bar is x, y, width, height: its top left corner moves its place and keeps its far edges.
	TEST_EXPECT(hud_drag_start(HudElement::Power, hud, start) && start.values == std::vector<int>({ 0, 0, 0, 0 }));
	const std::string power = "HUDPOWERBAR\t20,720,72,11\r\n";
	Model power_model(power);
	TEST_EXPECT(hud_drag_start(HudElement::Power, power_model.file.hud, start) &&
	            hud_drag_changes(start, HudHandle::TopLeft, -4.0f, 2.0f, 1, changes, error));
	TEST_EXPECT(edited(power, changes) == "HUDPOWERBAR\t16,722,76,9\r\n");
	// A key the file lacks: a line of its own at the end, as the writer writes it.
	TEST_EXPECT(hud_drag_start(HudElement::Heat, hud, start) &&
	            hud_drag_changes(start, HudHandle::BottomRight, 4.0f, 70.0f, 1, changes, error));
	after = edited(text, changes);
	TEST_EXPECT(after.size() > text.size() && after.compare(text.size(), std::string::npos, "HUDHEAT\t0,0,4,70\r\n") == 0);
	// A line shorter than the value set: the values it lacks added after its last.
	const std::string short_line = "HUDWEAPONNAME 14,588 // the name\r\n";
	Model short_model(short_line);
	HudValueChange align;
	TEST_EXPECT(hud_field_change(HudElement::WeaponName, short_model.file.hud, "hudweaponname.align", "Center", align, error));
	TEST_EXPECT(edited(short_line, { align }) == "HUDWEAPONNAME 14,588,0,center // the name\r\n");

	// The fields: the place, the hidden value and the anchor, the fonts, the colour, the detail level.
	const std::vector<HudField> fields = hud_element_fields(HudElement::AmmoCount, hud);
	std::vector<std::string> ids;
	for (const HudField &field : fields) ids.push_back(field.id);
	TEST_EXPECT(ids == std::vector<std::string>({ "ammocountpos.x", "ammocountpos.y", "ammocountpos.hidden",
	                                              "ammocountpos.align", "fonthud1_hi", "fonthud1_lo", "weapon_textcolor.r",
	                                              "weapon_textcolor.g", "weapon_textcolor.b", "huddeclut_wpngrp.level0",
	                                              "huddeclut_wpngrp.level1", "huddeclut_wpngrp.level2",
	                                              "huddeclut_wpngrp.level3" }));
	TEST_EXPECT(fields[0].value == "168" && fields[0].most == 1024 && fields[1].most == 768 && fields[3].value == "right" &&
	            fields[4].value == "onhudb18.fnt" && fields[0].cite.find("0x59FC31") != std::string::npos);
	const std::vector<HudField> health = hud_element_fields(HudElement::Health, hud);
	TEST_EXPECT(health.size() >= 8 && health[2].id == "hudhealth.right" && health[4].id == "hudhealthborder.a");
	// A set checked against the field's kind and range.
	HudValueChange set;
	TEST_EXPECT(!hud_field_change(HudElement::AmmoCount, hud, "ammocountpos.x", "2000", set, error));
	TEST_EXPECT(!hud_field_change(HudElement::AmmoCount, hud, "ammocountpos.align", "middle", set, error));
	TEST_EXPECT(!hud_field_change(HudElement::AmmoCount, hud, "nosuch.field", "1", set, error) &&
	            error.find("ammocountpos.x") != std::string::npos);
	TEST_EXPECT(hud_field_change(HudElement::AmmoCount, hud, "fonthud1_lo", "my font.fnt", set, error));
	TEST_EXPECT(edited(text, { set }).find("fonthud1_lo\t\"my font.fnt\"\r\n") != std::string::npos);
	TEST_EXPECT(hud_field_change(HudElement::AmmoCount, hud, "huddeclut_wpngrp.level2", "1", set, error));
	TEST_EXPECT(edited(text, { set }).find("HUDDECLUT_WPNGRP\t0,0,1,0\r\n") != std::string::npos);
	// An element no line places has no field and no drag.
	TEST_EXPECT(hud_element_fields(HudElement::Scoreboard, hud).empty() &&
	            !hud_drag_start(HudElement::Scoreboard, hud, start));
	std::printf("layout edit: a move, a corner and a set written to the element's lines\n");
	return 0;
}

const opennova::editor::TextDocument *layout_text(Rig &rig) {
	for (const auto &open : rig.view().documents.open)
		if (open && open->path() == rig.layout) return text_of(*open);
	return nullptr;
}

int test_commands() {
	Rig rig;
	editor_test::handle_to_end(rig.session, request::new_project(rig.root(), "HUD"));
	editor_test::create_missing_files(rig.session);
	TEST_EXPECT(editor_test::write_text(rig.root() + "/" + rig.layout, kLayout));
	editor_test::handle_to_end(rig.session, request::rescan());
	editor_test::handle_to_end(rig.session, request::open_document(rig.layout));
	rig.pump();
	const HudViewport *viewport = rig.viewport();
	editor_test::FakeDevice *device = rig.devices.held(rig.layout, ViewportKind::Hud);
	TEST_EXPECT(viewport && device);
	if (!viewport || !device) return 1;
	device->placed.assign(opennova::hud::kHudElementCount, ViewportDeviceReport::Rect());
	device->placed[size_t(HudElement::AmmoCount)] = rect(130, 616, 168, 632);
	device->placed[size_t(HudElement::Health)] = rect(25, 741, 177, 751);
	rig.pump();
	// The items say what moves and what sizes, and each one's fields.
	JsonValue items = rig.query("viewport", "{\"path\":\"" + rig.layout + "\",\"op\":\"items\"}");
	bool ammo_fields = false, health_sizes = false;
	if (const JsonValue *rows = items.get("items"))
		for (const JsonValue &row : rows->array) {
			if (row.get_string("element", "") == "ammo_count")
				ammo_fields = row.get_bool("movable", false) && !row.get_bool("resizable", true) && row.get("fields") &&
				              row.get("fields")->array.size() == 13 &&
				              row.get("fields")->array[3].get_string("kind", "") == "align";
			if (row.get_string("element", "") == "health") health_sizes = row.get_bool("resizable", false);
		}
	TEST_EXPECT(ammo_fields && health_sizes);
	// move by an item: one batch, one undo step, the line rewritten in place.
	ViewportCommand move;
	move.name = "move";
	move.item = "ammo_count";
	move.by = { -8.0, 4.0 };
	editor_test::handle_to_end(rig.session, request::edit_in_viewport(rig.layout, move));
	TEST_EXPECT(rig.session.outcome().done());
	const TextDocument *text = layout_text(rig);
	TEST_EXPECT(text && text->text().find("AMMOCOUNTPOS\t160,620,0,right") != std::string::npos);
	// at: its place there.
	move.by.clear();
	move.has_at = true;
	move.at_x = 500.0f;
	move.at_y = 300.0f;
	editor_test::handle_to_end(rig.session, request::edit_in_viewport(rig.layout, move));
	TEST_EXPECT(rig.session.outcome().done() && text && text->text().find("AMMOCOUNTPOS\t500,300,0,right") != std::string::npos);
	// resize by the picked element (none picked: refused; picked: its bottom right).
	ViewportCommand resize;
	resize.name = "resize";
	resize.by = { 10.0, 2.0 };
	editor_test::handle_to_end(rig.session, request::edit_in_viewport(rig.layout, resize));
	TEST_EXPECT(!rig.session.outcome().done());
	editor_test::handle_to_end(rig.session,
	        request::set_viewport(rig.layout, "{\"kind\":\"hud\",\"options\":{\"picked\":\"health\"}}"));
	rig.pump();
	editor_test::handle_to_end(rig.session, request::edit_in_viewport(rig.layout, resize));
	TEST_EXPECT(rig.session.outcome().done() && text && text->text().find("HUDHEALTH\t25,741,187,753") != std::string::npos);
	resize.handle = "move";
	editor_test::handle_to_end(rig.session, request::edit_in_viewport(rig.layout, resize));
	TEST_EXPECT(!rig.session.outcome().done());
	resize.item = "ammo_count";
	resize.handle = "top_left";
	editor_test::handle_to_end(rig.session, request::edit_in_viewport(rig.layout, resize));
	TEST_EXPECT(!rig.session.outcome().done());
	// set: a field by its id, refused out of its range.
	ViewportCommand set;
	set.name = "set";
	set.item = "health";
	set.field = "hudhealthborder.a";
	set.value = "128";
	editor_test::handle_to_end(rig.session, request::edit_in_viewport(rig.layout, set));
	TEST_EXPECT(rig.session.outcome().done() && text && text->text().find("HUDHEALTHBORDER\t128,0,0,0\r\n") != std::string::npos);
	set.value = "300";
	editor_test::handle_to_end(rig.session, request::edit_in_viewport(rig.layout, set));
	TEST_EXPECT(!rig.session.outcome().done());
	// Each command one undo step: four undone, the text as it was read.
	for (int i = 0; i < 4; ++i) editor_test::handle_to_end(rig.session, request::undo(rig.layout));
	TEST_EXPECT(text && text->text() == kLayout);
	// The wire's form of a command: its item, handle, field and value.
	JsonValue wire;
	std::string error;
	TEST_EXPECT(opennova::io::json_parse(
	        "{\"kind\":\"edit_in_viewport\",\"path\":\"" + rig.layout +
	                "\",\"command\":{\"name\":\"set\",\"item\":\"ammo_count\",\"field\":\"ammocountpos.x\",\"value\":40}}",
	        wire, error));
	EditorRequest parsed;
	TEST_EXPECT(editor_request_from_json(wire, parsed, error) && parsed.command.item == "ammo_count" &&
	            parsed.command.field == "ammocountpos.x" && parsed.command.value == "40");
	// Another kind takes none of them.
	std::printf("commands: move, resize and set, each one undo step\n");
	return 0;
}

int test_canvas_drag() {
	Rig rig;
	editor_test::handle_to_end(rig.session, request::new_project(rig.root(), "HUD"));
	editor_test::create_missing_files(rig.session);
	TEST_EXPECT(editor_test::write_text(rig.root() + "/" + rig.layout, kLayout));
	editor_test::handle_to_end(rig.session, request::rescan());
	editor_test::handle_to_end(rig.session, request::open_document(rig.layout));
	editor_test::handle_to_end(rig.session, request::set_viewport(rig.layout,
	        "{\"kind\":\"hud\",\"options\":{\"width\":1600,\"height\":1200}}"));
	rig.pump();
	editor_test::FakeDevice *device = rig.devices.held(rig.layout, ViewportKind::Hud);
	const HudViewport *viewport = rig.viewport();
	TEST_EXPECT(device && viewport);
	if (!device || !viewport) return 1;
	// At 1600 x 1200 the health bar draws at its design corners scaled by 1.5625.
	device->placed.assign(opennova::hud::kHudElementCount, ViewportDeviceReport::Rect());
	device->placed[size_t(HudElement::Health)] = rect(39, 1158, 277, 1173);
	rig.pump();
	// The canvas shows the 1600 x 1200 screen in an 800 x 600 picture: a picture pixel is 1.28 design units.
	HudCanvas canvas;
	editor_test::Gathered out;
	// One frame of the canvas: the viewport as the session holds it now, followed, then what the frame does.
	auto frame = [&](const std::function<void(const ViewportContext &)> &body) {
		const ViewportContext context = viewport_context(rig.view(), *viewport);
		canvas.follow(*viewport, context, out);
		body(context);
	};
	CanvasInput in;
	in.width = 800;
	in.height = 600;
	in.hovered = true;
	in.mouse = in.screen = CanvasPoint{ 60.0f, 582.0f };
	CanvasCursor cursor = CanvasCursor::Default;
	frame([&](const ViewportContext &context) {
		cursor = canvas.cursor(context, in);
		in.pressed = in.down = true;
		canvas.input(context, in, out);
		in.pressed = false;
	});
	TEST_EXPECT(cursor == CanvasCursor::Move);
	// Three samples of one drag, each served as the editor serves it, the picture following each.
	const float travels[] = { 4.0f, 10.0f, 20.0f };
	for (const float travel : travels) {
		in.mouse = in.screen = CanvasPoint{ 60.0f + travel, 582.0f - travel / 2.0f };
		frame([&](const ViewportContext &context) { canvas.input(context, in, out); });
		TEST_EXPECT(editor_test::serve(rig.session, out.requests));
		out.requests.clear();
		rig.pump();
	}
	// Let go: the gesture's end.
	in.down = false;
	frame([&](const ViewportContext &context) { canvas.input(context, in, out); });
	TEST_EXPECT(out.requests.size() == 1 && out.requests[0].kind == EditorRequestKind::EndEdit);
	TEST_EXPECT(editor_test::serve(rig.session, out.requests));
	out.requests.clear();
	// 20 picture pixels right and 10 up: 25.6 and 12.8 design units, whole on the design grid.
	const TextDocument *text = layout_text(rig);
	TEST_EXPECT(text && text->text().find("HUDHEALTH\t51,728,203,738\r\n") != std::string::npos);
	TEST_EXPECT(viewport->options().picked == "health");
	// The drag is one undo step.
	editor_test::handle_to_end(rig.session, request::undo(rig.layout));
	TEST_EXPECT(text && text->text() == kLayout);
	editor_test::handle_to_end(rig.session, request::redo(rig.layout));
	rig.pump();
	// The picked element's corner: its handle resizes it.
	device->placed[size_t(HudElement::Health)] = rect(80, 1138, 317, 1153);
	rig.pump();
	in.mouse = in.screen = CanvasPoint{ 158.5f, 576.5f };
	size_t handles = 0;
	frame([&](const ViewportContext &context) {
		cursor = canvas.cursor(context, in);
		const OverlayList shapes = canvas.shapes(context, in);
		for (const OverlayShape &shape : shapes.shapes) handles += shape.kind == OverlayKind::Marker ? 1 : 0;
		in.pressed = in.down = true;
		canvas.input(context, in, out);
		in.pressed = false;
	});
	TEST_EXPECT(cursor == CanvasCursor::ResizeNWSE && handles == 4);
	in.mouse = in.screen = CanvasPoint{ 168.5f, 576.5f };
	frame([&](const ViewportContext &context) { canvas.input(context, in, out); });
	TEST_EXPECT(editor_test::serve(rig.session, out.requests));
	out.requests.clear();
	TEST_EXPECT(text && text->text().find("HUDHEALTH\t51,728,216,738\r\n") != std::string::npos);
	in.down = false;
	frame([&](const ViewportContext &context) { canvas.input(context, in, out); });
	TEST_EXPECT(editor_test::serve(rig.session, out.requests));
	out.requests.clear();
	// The arrows nudge it a design unit while held, one undo step.
	in.keyboard.focused = true;
	in.keyboard.arrow_x = 1;
	in.keyboard.arrow_held = true;
	for (int step = 0; step < 3; ++step) {
		if (step == 2) {
			in.keyboard.arrow_x = 0;
			in.keyboard.arrow_held = false;
		}
		rig.pump();
		frame([&](const ViewportContext &context) { canvas.input(context, in, out); });
		TEST_EXPECT(editor_test::serve(rig.session, out.requests));
		out.requests.clear();
	}
	TEST_EXPECT(text && text->text().find("HUDHEALTH\t53,728,218,738\r\n") != std::string::npos);
	std::printf("canvas drag: a move and a corner on the design grid at 1600 x 1200, one undo step each; a nudge\n");
	return 0;
}

// S23 C: the sights and the Tab board. A scoped rifle with a rangefinder and an elevation readout, its sights up:
// the frame the game's own view leaves once its scope settles and the body's aim ray has found the wall the range
// option stands (the card, the readouts, the range the rangefinder reads, the zero, the magnification); refused
// under the binoculars and for a weapon with no sights, each in words. The board's stand-in rows for a team game
// and a free-for-all, and the options' wire with its refusals.
int test_sights_and_board() {
	Rig rig;
	editor_test::handle_to_end(rig.session, request::new_project(rig.root(), "HUD"));
	editor_test::create_missing_files(rig.session);
	const std::string weapons = std::string(kWeapons) +
	                            "weapon \"W_SCOPE\"\r\n\tclipsize 5\r\n\tstartrounds 10\r\n\tflags scoped\r\n"
	                            "\tflags showrange\r\n\tflags showelevation\r\n\tscope_max_mag 4 0\r\n"
	                            "\tscope_max_zero 10 100 200 1\r\n\tpos 25 -5 -145 0 0 0\r\n\ttpos -1 12 -138 0 0 0\r\nend\r\n";
	TEST_EXPECT(editor_test::write_text(rig.root() + "/" + rig.layout, kLayout) &&
	            editor_test::write_text(rig.root() + "/defs/weapon.def", weapons));
	editor_test::handle_to_end(rig.session, request::rescan());
	editor_test::handle_to_end(rig.session, request::open_document(rig.layout));
	rig.pump();
	const HudViewport *viewport = rig.viewport();
	TEST_EXPECT(viewport != nullptr);
	if (!viewport) return 1;
	TEST_EXPECT(!viewport->scope_frame() && viewport->scope_why().empty());

	// Up with the scoped rifle, the aim on a wall 300 metres away.
	editor_test::handle_to_end(rig.session, request::set_viewport(rig.layout,
	        "{\"kind\":\"hud\",\"options\":{\"weapon\":\"W_SCOPE\",\"sights\":true,\"range\":300}}"));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	const opennova::world::LocalPlayerViewFrame *frame = viewport->scope_frame();
	TEST_EXPECT(frame != nullptr);
	if (frame) {
		TEST_EXPECT(frame->scope_card_active && frame->frame_fx.scoped_selector && !frame->frame_fx.sighted_selector);
		TEST_EXPECT(frame->scope_details_active && frame->scope_details_scoped);
		const double range = double(frame->aim_range_q16) / 65536.0;
		TEST_EXPECT(range > 298.0 && range < 302.0);
		std::printf("sights: range %.2f m, zero word %d, magnification %d, fov %.2f\n", range,
		            int(frame->scope_zero_word), int(frame->scope_magnification), double(frame->fov_h_deg));
	}
	TEST_EXPECT(viewport->scope_weapon() != nullptr);
	// The aimed shot stamped as the game's input pack stamps it, the optical view up: the crosshair's gate shuts
	// (the HUD draws none over the card, as the game's frame shows) and the spread reads the aimed row.
	TEST_EXPECT(viewport->scope_weapon() && viewport->scope_weapon()->aimed_shot_available);
	JsonValue state = rig.query("viewport", "{\"path\":\"" + rig.layout + "\",\"op\":\"state\"}");
	const JsonValue *body = state.get("body");
	const JsonValue *sights = body ? body->get("sights") : nullptr;
	TEST_EXPECT(sights && sights->get_bool("up", false) && sights->get_bool("card", false) &&
	            sights->get_bool("readouts", false) && sights->get_number("range", 0) > 298.0);
	// A nearer wall: the run again, the rangefinder's reading with it.
	editor_test::handle_to_end(rig.session, request::set_viewport(rig.layout, "{\"kind\":\"hud\",\"options\":{\"range\":50}}"));
	rig.pump();
	frame = viewport->scope_frame();
	TEST_EXPECT(frame && double(frame->aim_range_q16) / 65536.0 > 48.0 && double(frame->aim_range_q16) / 65536.0 < 52.0);
	// The binoculars put them down; so does a weapon with no sights, the game's refusal in words.
	editor_test::handle_to_end(rig.session, request::set_viewport(rig.layout, "{\"kind\":\"hud\",\"options\":{\"view\":\"binoculars\"}}"));
	rig.pump();
	TEST_EXPECT(!viewport->scope_frame() && viewport->scope_why().find("binoculars") != std::string::npos);
	editor_test::handle_to_end(rig.session, request::set_viewport(rig.layout,
	        "{\"kind\":\"hud\",\"options\":{\"view\":\"normal\",\"weapon\":\"W_AR15\"}}"));
	rig.pump();
	TEST_EXPECT(!viewport->scope_frame() && viewport->scope_why().find("neither scoped nor sighted") != std::string::npos);

	// The board's stand-ins: a team game's players over its two sides, the score falling; a free-for-all's on none.
	HudViewportOptions options;
	options.game_type = opennova::game_type::kTeamDeathmatch;
	options.players = 5;
	const opennova::hud::GameTextLookup none = [](const char *, const char *, const char *fallback) {
		return std::string(fallback);
	};
	opennova::hud::HudScoreboardState board = hud_preview_board(options, none, false, none);
	TEST_EXPECT(board.game_type == options.game_type && board.team_count == 2 && board.rows.size() == 5 && board.local_team == 1);
	TEST_EXPECT(board.rows[0].name == "Player 1" && board.rows[0].score1 == 5 && board.rows[0].team == 1 &&
	            board.rows[1].team == 2 && board.rows[4].score1 == 1 && board.rows[4].team == 1 && board.rows[2].has_entity);
	TEST_EXPECT(board.title == "!Kill List" && board.players_line == " 5" && board.spectators_line.empty() &&
	            board.footer == "!PgUp and PgDn to change pages");
	options.game_type = opennova::game_type::kDeathmatch;
	board = hud_preview_board(options, none, false, none);
	TEST_EXPECT(board.team_count == 0 && board.rows[1].team == 0);
	// On the wire, and the refusals.
	editor_test::handle_to_end(rig.session, request::set_viewport(rig.layout,
	        "{\"kind\":\"hud\",\"options\":{\"board\":true,\"game_type\":\"ctf\",\"players\":12}}"));
	TEST_EXPECT(rig.session.outcome().done() && viewport->options().board &&
	            viewport->options().game_type == opennova::game_type::kCaptureTheFlag && viewport->options().players == 12);
	state = rig.query("viewport", "{\"path\":\"" + rig.layout + "\",\"op\":\"state\"}");
	const JsonValue *options_json = state.get("options");
	TEST_EXPECT(options_json && options_json->get_string("game_type", "") == "CTF" &&
	            options_json->get_number("players", 0) == 12.0 && options_json->get_bool("board", false));
	body = state.get("body");
	TEST_EXPECT(body && body->get("board") && body->get("board")->get_number("rows", 0) == 12.0 &&
	            body->get("board")->get_number("teams", 0) == 2.0);
	// The model's board, its strings composed headlessly over the project's tables (it has none: the literals).
	TEST_EXPECT(viewport->board().rows.size() == 12 && viewport->board().title == "!Kill List" &&
	            viewport->board().players_line == " 12" && viewport->board().game_type == opennova::game_type::kCaptureTheFlag);
	for (const char *refused : { "{\"game_type\":\"TAG\"}", "{\"players\":129}", "{\"range\":1}", "{\"sights\":1}" }) {
		editor_test::handle_to_end(rig.session, request::set_viewport(rig.layout,
		        std::string("{\"kind\":\"hud\",\"options\":") + refused + "}"));
		TEST_EXPECT(!rig.session.outcome().done());
	}
	TEST_EXPECT(hud_board_game_types().size() == 12 && std::string(hud_board_game_type_token(0x30020u)) == "COOP");
	std::printf("sights and board: the scope's frame at the range's wall, refused in words; the board's stand-ins\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_layout_type();
	failures += test_session();
	failures += test_sights_and_board();
	failures += test_canvas();
	failures += test_layout_edit();
	failures += test_commands();
	failures += test_canvas_drag();
	if (failures == 0) std::printf("editor_hud_viewport: all passed\n");
	return failures == 0 ? 0 : 1;
}
