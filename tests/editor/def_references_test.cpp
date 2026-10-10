// Definition fields complete their references (ADR 0046 DI-09): every def field the game resolves by name
// is a reference of the right kind, cited to the reader that resolves it, over a real session. An item's
// weapon points, launch points and a mounted gun's point name user points of its graphic, every point
// scanned; its particle slots its graphic's first 16 alone (their section of the model's scope); its cockpit
// model and the driver's eye in it; its one-shot sounds by the hour sets. A weapon's launch point names a
// point of its third-person model, an action's effect point one of each of its two models (two edges), an
// action's anim a row of the weapon's map by its slot's key (a new kind, AnimationKey), a missing one in the
// game's words; sameas a weapon; its commander reticle and slot bar icon HUD art. An ammo's tracer ids name
// items by their type id (the items.def id less 100000): the graph reaches the item, the picker lists the
// type ids, a rename of the item's id writes the type id back; its blast-victim effect a particle; an effects
// row's surface a choice of the game's 27 tags. A missing effect is tolerated, in the game's words. hudpos.def's
// vehicle panels name items by their alias (items.def's sid, else S%06i of the id), Avatars.def's combinations
// their parts by name and kind. The
// complete query: the names a typed text begins first, as the lookup compares them, each with whether the
// field holds it whole and its preview; the typed name's status, Go to and Add it there.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/def_table.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/display_names.h>
#include <editor/graph/reference_queries.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/rtxt/rtxt.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "fixtures/minimal_3di_builder.h"

using namespace opennova;
using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

using editor_test::NoProcess;

// A box model holding the named user points, in order.
std::vector<uint8_t> model_with_points(const char *name, const std::vector<std::string> &points) {
	synth3di::Model m;
	m.name = name;
	const int lod = m.add_lod();
	const int paint = m.add_material("FF_ST_OP", "paint.tga");
	const int part = m.add_part(lod, 0, synth3di::Vec3{});
	m.add_box(lod, part, paint, synth3di::Box{{-0.5, -0.5, 0.0}, {0.5, 0.5, 1.0}});
	m.add_panm(lod, part, 0);
	for (const std::string &point : points)
		m.add_user_point(point.c_str(), synth3di::Vec3{0.0, 0.0, 0.5}, synth3di::Vec3{0.0, 0.0, 1.0}, 0,
		                 synth3di::kUserPointGameplay);
	std::vector<uint8_t> out;
	if (!synth3di::mint(m, out)) out.clear();
	return out;
}

struct Project {
	editor_test::TempProjectDir dir{"opennova_editor_def_references"};
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string root;

	Project() {
		editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Def references"));
		editor_test::set_missions(session, true); // ammo.def is a mission's
		editor_test::create_missing_files(session);
		root = session.view().project.root;
	}
	const AssetGraph &graph() const { return *session.view().findings.graph; }
	std::string path(const char *name) const {
		const AssetEntry *entry = session.view().project.scan->find(name);
		return entry ? entry->relative_path : std::string();
	}
	// A text as the game's readers take it, each line ending CR LF (their reader ends a line there alone).
	bool write(const std::string &relative, const std::string &text) {
		std::string lines;
		for (const char c : text) lines += c == '\n' ? std::string("\r\n") : std::string(1, c);
		return editor_test::write_text(root + "/" + relative, lines);
	}
	bool write(const std::string &relative, const std::vector<uint8_t> &bytes) {
		return editor_test::write_bytes(root + "/" + relative, bytes);
	}
	void rescan() { editor_test::handle_to_end(session, request::rescan()); }
	JsonValue query(const char *name, const std::string &args) {
		JsonValue parsed;
		std::string error;
		io::json_parse(args, parsed, error);
		error.clear();
		JsonValue out = session.query(name, parsed, error);
		if (!error.empty()) std::printf("  query error: %s\n", error.c_str());
		return out;
	}
};

// The edge of `field` in `source` naming `value` in `scope` ("" any).
const GraphEdge *edge_of(const AssetGraph &graph, const std::string &source, const char *field, const std::string &value,
                         const std::string &scope = std::string()) {
	for (const GraphEdge *edge : graph.references_of(source))
		if (edge->field == field && edge->value == value && (scope.empty() || edge->scope == scope)) return edge;
	return nullptr;
}

std::string missing_message(const SessionView &view, const std::string &file, const char *field, const std::string &value) {
	for (const Diagnostic &d : view.findings.diagnostics)
		if (d.code() == "reference.missing" && d.asset == file && d.field == field && d.message.find("'" + value + "'") != std::string::npos)
			return d.message;
	return std::string();
}

// The first row the Null marker the shipped items.def begins with (a tracer's lookup by type id never reaches
// the first row).
const char *kItems = "begin \"Null\"\nid 100000\ntype marker\nend\n"
                     "begin \"Tank\"\nid 100500\ntype vehicle\ngraphic tank\ntextid TANK_TEXT\nweaplbup Late\n"
                     "launchups_rocket Nowhere\n"
                     "particlefx Effect_x Late\nparticlefxs Effect_x P03\nvirtualdisplay cockpit camera\n"
                     "addeweap Mount 100501\nend\n"
                     "begin \"Turret\"\nid 100501\ntype vehicle\ngraphic tank\nend\n"
                     "begin \"Soldier\"\nid 100502\ntype person\ngraphic chars\\soldier.3di\nend\n"
                     "begin \"Soldier again\"\nid 100502\ntype person\ngraphic medic\nend\n";

const char *kWeapons = "weapon \"WPN_T\"\n\tanimadm gun_1st\n\tgfx1 fpgun\n\tgfx3 tank\n\tlaunchuserpoint Late\n"
                       "\tsameas WPN_NONE\n\tloadout_menu_ttdesc TT_WPN_T\n"
                       "\taction \"fire\"\n\t\tanim anim_wpn_fire\n\t\tparticleuserpoint Muzzle\n\t\ttexttoken FIRE_TEXT\n\tend\n"
                       "\taction \"reload\"\n\t\tanim anim_wpn_reload\n\tend\n"
                       "\taction \"idle\"\n\t\tanim anim_bogus\n\tend\n"
                       "end\n";

const char *kAmmo = "ammo AMMO_T\n\tfrndlyTrcrID 500\n\tfoeTrcrID 777\n\tsecondary_effect Effect_gone\n"
                    "\teffects_table\n\t\tdirt none none 0\n\tend\nend\n";

bool setup(Project &project) {
	std::vector<std::string> points;
	for (int i = 1; i <= 16; ++i) points.push_back((i < 10 ? "P0" : "P") + std::to_string(i));
	points.push_back("Late");
	points.push_back("Mount");
	bool ok = project.write("models/tank.3di", model_with_points("tank", points)) &&
	          project.write("models/fpgun.3di", model_with_points("fpgun", {"Muzzle"})) &&
	          project.write("anims/gun_1st.adm", "anim_reset \"gun_rst\"\nanim_wpn_fire \"gun_f\"\n");
	project.rescan();
	ok = ok && project.write(project.path("items.def"), kItems) && project.write(project.path("weapon.def"), kWeapons) &&
	     project.write(project.path("ammo.def"), kAmmo);
	project.rescan();
	return ok;
}

} // namespace

// Every name an item, a weapon and an ammo name is a reference of its kind in the scope its reader reads.
static int test_fields_are_references() {
	Project project;
	TEST_EXPECT(setup(project));
	const AssetGraph &graph = project.graph();
	const SessionView &view = project.session.view();
	const std::string items = project.path("items.def"), weapons = project.path("weapon.def"), ammo = project.path("ammo.def");
	// The weapon points scan every point of the graphic: the 17th is found.
	const GraphEdge *weapon_point = edge_of(graph, items, "weapon_userpoints[0]", "Late");
	TEST_EXPECT(weapon_point && weapon_point->kind == ReferenceKind::UserPoint && weapon_point->scope == "TANK.3DI" &&
	            graph.resolve(*weapon_point) == ReferenceStatus::Present);
	// A particle slot reads the first 16 alone: the 17th is no point of its lookup.
	const GraphEdge *slot = edge_of(graph, items, "particlefx.userpoint", "Late");
	TEST_EXPECT(slot && slot->scope == "TANK.3DI/FIRST16" && graph.resolve(*slot) == ReferenceStatus::Missing);
	TEST_EXPECT(missing_message(view, items, "particlefx.userpoint", "Late").find("first 16") != std::string::npos);
	const GraphEdge *early = edge_of(graph, items, "particlefxs.userpoint", "P03");
	TEST_EXPECT(early && graph.resolve(*early) == ReferenceStatus::Present);
	// A launch point the graphic lacks: tolerated, in the lookup's words.
	TEST_EXPECT(edge_of(graph, items, "launchups_rocket", "Nowhere"));
	const std::string launch = missing_message(view, items, "launchups_rocket", "Nowhere");
	TEST_EXPECT(launch.find("TANK.3DI does not have") != std::string::npos);
	// A mounted gun's point; the cockpit model and its eye.
	const GraphEdge *mount = edge_of(graph, items, "userpoint", "Mount");
	TEST_EXPECT(mount && mount->scope == "TANK.3DI" && graph.resolve(*mount) == ReferenceStatus::Present);
	const GraphEdge *cockpit = edge_of(graph, items, "virtual_display", "cockpit");
	TEST_EXPECT(cockpit && cockpit->kind == ReferenceKind::Model);
	const GraphEdge *eye = edge_of(graph, items, "virtual_display_userpoint", "camera");
	TEST_EXPECT(eye && eye->scope == "COCKPIT.3DI");
	// A weapon's launch point on its third-person model; an action's effect point on each model.
	const GraphEdge *launch_point = edge_of(graph, weapons, "launch_user_point", "Late");
	TEST_EXPECT(launch_point && launch_point->scope == "TANK.3DI" && graph.resolve(*launch_point) == ReferenceStatus::Present);
	const GraphEdge *third = edge_of(graph, weapons, "particleuserpoint", "Muzzle", "TANK.3DI");
	const GraphEdge *first = edge_of(graph, weapons, "particleuserpoint", "Muzzle", "FPGUN.3DI");
	TEST_EXPECT(third && first && graph.resolve(*third) == ReferenceStatus::Missing &&
	            graph.resolve(*first) == ReferenceStatus::Present && first->rewritable);
	// An action's slot in its weapon's map: the fire row there, a reload the map has no row for, a key naming
	// no slot.
	const GraphEdge *fire = edge_of(graph, weapons, "anim", "anim_wpn_fire");
	TEST_EXPECT(fire && fire->kind == ReferenceKind::AnimationKey && fire->scope == "GUN_1ST.ADM" &&
	            graph.resolve(*fire) == ReferenceStatus::Present);
	TEST_EXPECT(graph.resolve(ReferenceKind::AnimationKey, "ANIM_WPN_FIRE", "GUN_1ST.ADM") == ReferenceStatus::Present);
	const GraphSymbol *row = graph.resolve_symbol(ReferenceKind::AnimationKey, "anim_wpn_fire", "GUN_1ST.ADM");
	TEST_EXPECT(row && row->file == "anims/gun_1st.adm" && graph.users_of(*row).size() == 1);
	TEST_EXPECT(missing_message(view, weapons, "anim", "anim_wpn_reload").find("has no row for") != std::string::npos);
	TEST_EXPECT(missing_message(view, weapons, "anim", "anim_bogus").find("252 animation slots") != std::string::npos);
	// The text keys as the game reads each: an item's text a key of gametext.bin's item section (its
	// text_default shown for a key it lacks); an action's text token any table's, the playing mission's then
	// gametext.bin's (an empty text for none); the loadout tooltip, which nothing reads, no reference.
	const GraphEdge *item_text = edge_of(graph, items, "text_id", "TANK_TEXT");
	TEST_EXPECT(item_text && item_text->kind == ReferenceKind::TextId && item_text->scope == "GAMETEXT.BIN/item" &&
	            graph.resolve(*item_text) == ReferenceStatus::Missing);
	TEST_EXPECT(missing_message(view, items, "text_id", "TANK_TEXT").find("text_default") != std::string::npos);
	const GraphEdge *token = edge_of(graph, weapons, "text_token", "FIRE_TEXT");
	TEST_EXPECT(token && token->kind == ReferenceKind::TextId && token->scope == "MEDMSSN.BIN" &&
	            token->scopes_after == std::vector<std::string>{"GAMETEXT.BIN"});
	TEST_EXPECT(missing_message(view, weapons, "text_token", "FIRE_TEXT").find("the action's text is empty") !=
	            std::string::npos);
	bool tooltip = false;
	for (const GraphEdge *edge : graph.references_of(weapons)) tooltip = tooltip || edge->field == "loadout_menu_ttdesc";
	TEST_EXPECT(!tooltip);
	// A person's face: the .GRM its model's name makes, of the first row of its id alone; the name derived, so no
	// rename rewrites it, and a face the project lacks no finding (most people have none).
	const GraphEdge *face = edge_of(graph, items, "graphic", "soldier.GRM");
	TEST_EXPECT(face && face->kind == ReferenceKind::FaceAnimation && !face->rewritable &&
	            graph.resolve(*face) == ReferenceStatus::Missing && missing_message(view, items, "graphic", "soldier.GRM").empty());
	TEST_EXPECT(!edge_of(graph, items, "graphic", "medic.GRM") && !edge_of(graph, items, "graphic", "tank.GRM"));
	// sameas names a weapon.
	const GraphEdge *sameas = edge_of(graph, weapons, "sameas", "WPN_NONE");
	TEST_EXPECT(sameas && sameas->kind == ReferenceKind::Weapon);
	// The tracer ids reach the items their type ids name: 500 the item 100500.
	const GraphEdge *friendly = edge_of(graph, ammo, "frndly_trcr_type_id", "100500");
	TEST_EXPECT(friendly && friendly->kind == ReferenceKind::Item && friendly->name_offset == 100000 &&
	            graph.resolve(*friendly) == ReferenceStatus::Present);
	const GraphEdge *foe = edge_of(graph, ammo, "foe_trcr_type_id", "100777");
	TEST_EXPECT(foe && graph.resolve(*foe) == ReferenceStatus::Missing);
	TEST_EXPECT(missing_message(view, ammo, "foe_trcr_type_id", "100777").find("(type id 777)") != std::string::npos);
	// Where its type id finds no item, the tracer is the item named as the ammo (ItemName, the edge's fallback):
	// none named AMMO_T yet, which the finding says.
	TEST_EXPECT(foe && foe->fallback == "AMMO_T" && foe->fallback_kind == ReferenceKind::ItemName && !friendly->fallback.empty());
	TEST_EXPECT(missing_message(view, ammo, "foe_trcr_type_id", "100777").find("nor an item named as the ammo ('AMMO_T')") !=
	            std::string::npos);
	// A missing effect is tolerated: the game plays stockeffect's copy.
	const GraphEdge *victim = edge_of(graph, ammo, "secondary_effect", "Effect_gone");
	TEST_EXPECT(victim && victim->kind == ReferenceKind::Particle);
	bool warned = false;
	for (const Diagnostic &d : view.findings.diagnostics)
		warned = warned || (d.asset == ammo && d.field == "secondary_effect" && d.severity == DiagnosticSeverity::Warning &&
		                    d.message.find("stockeffect") != std::string::npos);
	TEST_EXPECT(warned);
	return 0;
}

// The picker, the completion and the rename of a type id: the item's id less 100000.
static int test_type_ids() {
	Project project;
	TEST_EXPECT(setup(project));
	const std::string ammo = project.path("ammo.def"), items = project.path("items.def");
	editor_test::handle_to_end(project.session, request::open_document(ammo));
	const Document *document = project.session.document_for(ammo);
	TEST_EXPECT(document != nullptr);
	if (!document) return 1;
	NodeAddress round;
	TEST_EXPECT(find_definition(project.graph(), *document, "AMMO_T", round));
	FieldUse field;
	for (const FieldSchema &schema : document->fields(round.kind))
		if (schema.id == "frndly_trcr_type_id") field = document->field_on(round, schema);
	TEST_EXPECT(field.schema && field.reference == ReferenceKind::Item && field.name_offset == 100000);
	if (!field.schema) return 1;
	const GraphNameSource names(project.graph());
	const std::vector<ReferenceChoice> choices = picker_choices(&project.graph(), *document, round, field, &names);
	const auto tank = std::find_if(choices.begin(), choices.end(), [](const ReferenceChoice &c) { return c.name == "500"; });
	TEST_EXPECT(tank != choices.end() && tank->symbol == "100500" && tank->label == "Tank");
	// The words of the value: the item's name.
	const DisplayName words = value_display(*document, round, field, Value(int64_t(500)), &names);
	TEST_EXPECT(words.text == "Tank" && !words.dangling);
	// The complete query over the wire: the type ids the typed digits begin, as written.
	const JsonValue answer = project.query("complete",
	                                       "{\"path\": \"" + ammo + "\", \"locator\": \"" + document->locator(round) +
	                                               "\", \"field\": \"frndly_trcr_type_id\", \"prefix\": \"50\"}");
	const JsonValue *listed = answer.get("choices");
	TEST_EXPECT(listed && listed->is_array() && !listed->array.empty() && listed->array[0].get_string("name", "") == "500" &&
	            listed->array[0].get_bool("prefix", false));
	TEST_EXPECT(answer.get("typed") && answer.get("typed")->get_string("status", "") == "missing"); // 100050 is no item
	// A rename of the item's id writes the type id that names it.
	const std::vector<const GraphSymbol *> defined = project.graph().symbols_named(ReferenceKind::Item, "100500");
	TEST_EXPECT(defined.size() == 1);
	if (defined.empty()) return 1;
	const GraphSymbol symbol = *defined.front();
	TEST_EXPECT(editor_test::handle_to_end(project.session,
	                                       request::rename_symbol(symbol.file, symbol.locator, symbol.field, "100600"))
	                    .done());
	TEST_EXPECT(edge_of(project.graph(), ammo, "frndly_trcr_type_id", "100600"));
	Value held;
	const Document *reopened = project.session.document_for(ammo);
	NodeAddress again;
	TEST_EXPECT(reopened && find_definition(project.graph(), *reopened, "AMMO_T", again) &&
	            reopened->get(again, "frndly_trcr_type_id", held) && std::get<int64_t>(held) == 600);
	(void)items;
	return 0;
}

// The complete query over a text reference: the names its prefix begins first, without case, each with its
// preview and whether the field holds it; the typed name's status, its Go to and its Add it there.
static int test_complete_query() {
	Project project;
	TEST_EXPECT(setup(project));
	const std::string items = project.path("items.def");
	editor_test::handle_to_end(project.session, request::open_document(items));
	const Document *document = project.session.document_for(items);
	NodeAddress tank;
	TEST_EXPECT(document && find_definition(project.graph(), *document, "100500", tank));
	if (!document) return 1;
	const std::string where = "{\"path\": \"" + items + "\", \"locator\": \"" + document->locator(tank) + "\", ";
	// The weapon point's names: the graphic's every point, those "p1" begins first (P10..P16, then P01 holding it
	// nowhere: only the prefix ones and those holding it).
	JsonValue points = project.query("complete", where + "\"field\": \"weapon_userpoints[0]\", \"prefix\": \"p1\"}");
	const JsonValue *listed = points.get("choices");
	TEST_EXPECT(points.get_string("reference", "") == "user_point" && points.get_string("scope", "") == "TANK.3DI");
	TEST_EXPECT(listed && listed->array.size() >= 7 && listed->array[0].get_string("name", "") == "P10" &&
	            listed->array[0].get_bool("prefix", false) && listed->array[0].get_bool("fits", false));
	TEST_EXPECT(points.get("limit") && points.get("limit")->number == 15.0);
	// The particle slot's: the first 16 alone, so Late is not offered.
	JsonValue slot = project.query("complete", where + "\"field\": \"particlefx.userpoint\", \"prefix\": \"la\"}");
	TEST_EXPECT(slot.get("choices") && slot.get("choices")->array.empty());
	TEST_EXPECT(slot.get("typed") && slot.get("typed")->get_string("status", "") == "missing");
	// A model's name: its preview opens it; the typed name found and its Go to.
	JsonValue graphic = project.query("complete", where + "\"field\": \"graphic\", \"prefix\": \"TA\"}");
	const JsonValue *models = graphic.get("choices");
	TEST_EXPECT(models && !models->array.empty() && models->array[0].get("preview") &&
	            models->array[0].get("preview")->get("open"));
	JsonValue typed = project.query("complete", where + "\"field\": \"graphic\", \"prefix\": \"tank\"}");
	TEST_EXPECT(typed.get("typed") && typed.get("typed")->get_string("status", "") == "present" &&
	            typed.get("typed")->get("targets") && !typed.get("typed")->get("targets")->array.empty());
	// A powerup row no file defines: its Add it there.
	JsonValue powerup = project.query("complete", where + "\"field\": \"powerup_def\", \"prefix\": \"PU_NEW\"}");
	const JsonValue *fixes = powerup.get("typed") ? powerup.get("typed")->get("fixes") : nullptr;
	TEST_EXPECT(fixes && !fixes->array.empty() && fixes->array[0].get_string("label", "").find("Add PU_NEW") == 0);
	// The record by identity or locator, one of them.
	JsonValue args;
	std::string error;
	io::json_parse("{\"path\": \"" + items + "\", \"field\": \"graphic\"}", args, error);
	error.clear();
	project.session.query("complete", args, error);
	TEST_EXPECT(error.find("one of them") != std::string::npos);
	// The completion as a function: the length limit said, the kind's comparison without case.
	FieldUse field;
	for (const FieldSchema &schema : document->fields(tank.kind))
		if (schema.id == "soundloops[0]") field = document->field_on(tank, schema);
	TEST_EXPECT(field.schema && field_name_limit(field) == 24);
	ReferenceChoice long_set;
	long_set.name = "A_SET_NAME_LONGER_THAN_24_CHARS";
	long_set.kind = ReferenceKind::Sound;
	ReferenceChoice short_set;
	short_set.name = "a_set";
	short_set.kind = ReferenceKind::Sound;
	const std::vector<ReferenceCompletion> completed = complete_reference({long_set, short_set}, field, "A_S");
	TEST_EXPECT(completed.size() == 2 && !completed[0].fits && completed[1].fits && completed[1].prefix);
	return 0;
}

// A tracer whose type id finds no item is the item named as the ammo, the first of the name without case [orig:
// AmmoDef_ParseProperty @ 0x40A646..0x40A668 -> ItemList_FindIndexByPrimaryName @ 0x49E010]: the foe tracer 777
// reaches the item "ammo_t", whose uses are the ammo's field; a later item of the name none.
static int test_tracer_by_name() {
	Project project;
	TEST_EXPECT(setup(project));
	TEST_EXPECT(project.write(project.path("items.def"), std::string(kItems) +
	                                                         "begin \"ammo_t\"\nid 100900\ntype building\nend\n"
	                                                         "begin \"AMMO_T\"\nid 100901\ntype building\nend\n"));
	project.rescan();
	const AssetGraph &graph = project.graph();
	const std::string ammo = project.path("ammo.def");
	const GraphEdge *foe = edge_of(graph, ammo, "foe_trcr_type_id", "100777");
	TEST_EXPECT(foe && graph.resolve(*foe) == ReferenceStatus::Present);
	const GraphSymbol *item = foe ? graph.symbol_reached(*foe) : nullptr;
	TEST_EXPECT(item && item->kind == ReferenceKind::ItemName && item->display == "ammo_t");
	if (item) {
		const std::vector<const GraphEdge *> users = graph.users_of(*item);
		TEST_EXPECT(users.size() == 1 && users[0] == foe);
	}
	TEST_EXPECT(missing_message(project.session.view(), ammo, "foe_trcr_type_id", "100777").empty());
	const std::vector<const GraphSymbol *> named = graph.symbols_named(ReferenceKind::ItemName, "AMMO_T");
	TEST_EXPECT(named.size() == 2 && graph.users_of(*named[1]).empty());
	return 0;
}

// The two turns of a tracer's lookup the game takes past items.def's names: a type id naming items.def's first item
// finds none, the lookup's 0 being its "none" too [orig: ItemList_FindIndexByTypeId @ 0x49E100; AmmoDef_ParseProperty
// @ 0x40A5DA, `if (index || ...)`]; and the names it searches are the items' gametext names where "Item Names" has
// their STR_ITM%04i key, which the boot copies over items.def's [orig: Item_LoadLocalizedNames @ 0x49E1B0]. A
// change of the table resolves the tracer again.
static int test_tracer_lookups() {
	Project project;
	TEST_EXPECT(setup(project));
	TEST_EXPECT(project.write(project.path("items.def"), "begin \"Lead\"\nid 100800\ntype building\nend\n"
	                                                     "begin \"ammo_t\"\nid 100900\ntype building\nend\n"
	                                                     "begin \"Other\"\nid 100902\ntype building\nend\n") &&
	            project.write(project.path("ammo.def"), "ammo AMMO_T\n\tfrndlyTrcrID 800\n\tfoeTrcrID 902\nend\n"));
	project.rescan();
	const std::string ammo = project.path("ammo.def");
	const auto reached = [&](const char *field, const char *value) -> std::string {
		const GraphEdge *edge = edge_of(project.graph(), ammo, field, value);
		const GraphSymbol *symbol = edge ? project.graph().symbol_reached(*edge) : nullptr;
		return symbol ? symbol->display : std::string();
	};
	// 800 names the first item: the tracer is the item named as the ammo; 902 its own item.
	TEST_EXPECT(reached("frndly_trcr_type_id", "100800") == "ammo_t" && reached("foe_trcr_type_id", "100902") == "100902");
	// gametext names item 100900 otherwise and 100902 as the ammo: the lookup by name finds Other.
	const std::string table = project.path("gametext.bin");
	rtxt::File strings;
	strings.sections = {{"Item Names", 2}};
	rtxt::Entry renamed, as_ammo;
	renamed.key = "STR_ITM100900";
	renamed.text = "Tracer round";
	as_ammo.key = "STR_ITM100902";
	as_ammo.text = "Ammo_T";
	strings.entries = {renamed, as_ammo};
	std::vector<uint8_t> bytes;
	std::string io_error;
	TEST_EXPECT(!table.empty() && rtxt::write(strings, bytes, io_error) && project.write(table, bytes));
	project.rescan();
	TEST_EXPECT(reached("frndly_trcr_type_id", "100800") == "Other");
	const GraphEdge *friendly = edge_of(project.graph(), ammo, "frndly_trcr_type_id", "100800");
	const std::vector<const GraphSymbol *> other = project.graph().symbols_named(ReferenceKind::ItemName, "OTHER");
	TEST_EXPECT(friendly && other.size() == 1 && project.graph().users_of(*other[0]).size() == 1 &&
	            project.graph().users_of(*other[0])[0] == friendly);
	// No item named as the ammo any more: the finding says the type id names the first item.
	strings.entries = {renamed};
	strings.sections = {{"Item Names", 1}};
	TEST_EXPECT(rtxt::write(strings, bytes, io_error) && project.write(table, bytes));
	project.rescan();
	friendly = edge_of(project.graph(), ammo, "frndly_trcr_type_id", "100800");
	TEST_EXPECT(friendly && project.graph().resolve(*friendly) == ReferenceStatus::Missing);
	TEST_EXPECT(missing_message(project.session.view(), ammo, "frndly_trcr_type_id", "100800").find("first item") !=
	            std::string::npos);
	std::printf("tracer: a type id naming the first item finds none; the names searched are gametext's\n");
	return 0;
}

// A vehicle panel names its items by their alias; a combination its parts by name, of their kind, in its file.
static int test_aliases_and_parts() {
	Project project;
	TEST_EXPECT(setup(project));
	TEST_EXPECT(project.write(project.path("items.def"), std::string(kItems) +
	                                                         "begin \"Buggy\"\nid 100502\nsid buggy1\ntype vehicle\nend\n"));
	TEST_EXPECT(project.write("defs/hudpos.def", "VEHICLE_HUD\n  sid BUGGY1\n  interface h_buggy.tga\nVEHICLE_END\n"
	                                             "VEHICLE_HUD\n  sid S100500\nVEHICLE_END\n"
	                                             "VEHICLE_HUD\n  sid gone\nVEHICLE_END\n"));
	TEST_EXPECT(project.write("defs/Avatars.def",
	                          "define head H1\n{\n\tname AV_H\n\tgraphic tank.3di\n}\n"
	                          "define body B1\n{\n\tname AV_B\n\tgraphic tank.3di\n}\n"
	                          "define arms A1\n{\n\tname AV_A\n\tgraphic fpgun.3di\n}\n"
	                          "nationality 0 AV_N\n{\n\talignment good\n\tdivision 0 AV_D\n\t{\n\t\tcombo 1 h1 B1 A1\n\t}\n}\n"));
	project.rescan();
	const AssetGraph &graph = project.graph();
	const std::string hudpos = project.path("hudpos.def"), avatars = project.path("Avatars.def");
	TEST_EXPECT(!hudpos.empty() && !avatars.empty());
	// The authored alias, without case, and the alias an item with none takes (S%06i of its id).
	const GraphEdge *buggy = edge_of(graph, hudpos, "sid", "BUGGY1");
	const GraphEdge *tank = edge_of(graph, hudpos, "sid", "S100500");
	const GraphEdge *gone = edge_of(graph, hudpos, "sid", "gone");
	TEST_EXPECT(buggy && buggy->kind == ReferenceKind::ItemAlias && graph.resolve(*buggy) == ReferenceStatus::Present);
	TEST_EXPECT(tank && graph.resolve(*tank) == ReferenceStatus::Present);
	TEST_EXPECT(gone && graph.resolve(*gone) == ReferenceStatus::Missing);
	// A combination's parts, each of its kind.
	const GraphEdge *head = edge_of(graph, avatars, "head", "h1");
	TEST_EXPECT(head && head->kind == ReferenceKind::AvatarPart && head->scope == "AVATARS.DEF/HEAD" &&
	            graph.resolve(*head) == ReferenceStatus::Present);
	const GraphSymbol *body = graph.resolve_symbol(ReferenceKind::AvatarPart, "B1", "AVATARS.DEF/BODY");
	TEST_EXPECT(body && graph.users_of(*body).size() == 1);
	TEST_EXPECT(graph.resolve(ReferenceKind::AvatarPart, "B1", "AVATARS.DEF/HEAD") == ReferenceStatus::Missing);
	TEST_EXPECT(edge_of(graph, avatars, "arms", "A1"));
	return 0;
}

// An effects row's surface: one of the game's 27 tags.
static int test_surface_choices() {
	const TableKind *effect = catalog_table().kind(node_kind(def::DefRecordKind::Effect));
	TEST_EXPECT(effect != nullptr);
	if (!effect) return 1;
	const size_t place = effect->find("surface_type");
	TEST_EXPECT(place != TableKind::npos);
	if (place == TableKind::npos) return 1;
	const FieldSchema &surface = effect->fields()[place];
	TEST_EXPECT(surface.choices.size() == 27 && surface.choices.front().name == "move" && surface.choices.back().name == "uwatersurface" &&
	            !surface.open_choices);
	return 0;
}

int main() {
	int failures = 0;
	failures += test_fields_are_references();
	failures += test_type_ids();
	failures += test_complete_query();
	failures += test_aliases_and_parts();
	failures += test_tracer_by_name();
	failures += test_tracer_lookups();
	failures += test_surface_choices();
	return failures;
}
