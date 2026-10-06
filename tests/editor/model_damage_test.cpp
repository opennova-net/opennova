// A model's damage states, named and played (DI-10; editor/preview/model_damage, the model viewport's
// damage state, documents/model_ctrl_words). Headless (no device follows the viewport: a read of it, or the
// session's advance, follows it), over minted fixtures: the synthetic crate and armory models, an items.def
// naming them, a bank minted through lwf::encode_lwf and a short wave.
//
// Pinned: every CTRL register's words (a group, a label, its writer, the destroy fade's six as shares over
// 0..0x10000 cited to the game's publisher); the items naming a model and the role each gives it; a gnrc
// item's death as the game runs it, in order (the husk swap four ticks on, the flash, the pieces, the death
// sound, the Dead bank, the blast, then the fade after the item's delay); the picture: the crate until the
// swap, the armory from it (not the document's picture), the sections that always leave hidden, the six
// destroy registers at the fade's values over the held ones; the husk's own viewport driven over itself; the
// death sound heard as the clock runs from the death (Play destroy), never over a seek; the envelope's
// `damage`, the `registers` rows' words; the option's wire and its refusals; a class whose death lands no
// husk (null), a gnrl item's tail, and one with no husk model (its death plays, the graphic kept); the retail
// leg (--retail, OPENNOVA_JO_ASSETS): JO's damageable items
// whose models the tree holds, each planned and its husk swapped in.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/documents/model_ctrl_words.h>
#include <editor/preview/model_damage.h>
#include <editor/preview/model_viewport.h>
#include <editor/preview/viewport_json.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/def/def.h>
#include <formats/lwf/lwf.h>
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <runtime/world/destruction.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

std::string repo() { return test_paths_repo_root(__FILE__); }
std::string synth(const char *name) { return repo() + "/fixtures/threedi/synth/" + name; }

// A bank of one set per name, one layer heard in either view playing its one wave (the name lower case).
std::vector<uint8_t> bank_of(const std::vector<std::string> &sets) {
	lwf::File bank;
	for (size_t i = 0; i < sets.size(); ++i) {
		lwf::Single single;
		single.name = sets[i];
		single.path = strutil::to_lower(sets[i]) + ".wav";
		single.value_hi = 0xD200;
		bank.singles.push_back(single);
		lwf::Multi set;
		set.name = sets[i];
		set.pitch_base = lwf::kAuthoredSetPitchBase;
		set.target_id = 100;
		set.playlist_indices.push_back(uint32_t(i));
		bank.multis.push_back(set);
		lwf::Playlist layer;
		layer.falloff_radius = 100;
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

// The pump: a gnrc item drawn by the crate, its husk the armory (four parts: the hull and three pieces,
// one a WHEEL's 50% roll, two that always leave), half a second's delay, a second's fade, a quarter
// second's stagger; a gnrl one drawn by the crate too; a null one whose husk is the armory.
const char *kItems =
		"begin \"Pump station\"\nid 100500\ntype object\ngraphic crate\nai_function gnrc\nhusk armory\n"
		"husk_sub_part_types 01_HULL 02_WHEEL 03_CHUNK_M 04_CHUNK_S\ndestroy_timing 0.5 1.0 0.25\n"
		"sounddeath EXPLO_PUMP\nparticledeath fx_pump_dead\nparticlefire fx_pump_fire\nend\n"
		"begin \"Crate stack\"\nid 100501\ntype decoration\ngraphic crate\nai_function gnrl\nhusk armory\n"
		"sounddeath EXPLO_PUMP\nparticledeath fx_crate_dead\nend\n"
		"begin \"Statue\"\nid 100502\ntype decoration\ngraphic house\nhusk armory\nend\n"
		"begin \"Shed sign\"\nid 100503\ntype decoration\ngraphic house\nai_function gnrl\nsounddeath EXPLO_PUMP\nend\n";

struct DamageProject {
	editor_test::TempProjectDir dir{"opennova_editor_model_damage"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string root;
	std::string path;
	bool made = false;

	DamageProject() {
		session.handle(request::new_project(dir.file("project"), "Model Damage"));
		session.run_operations();
		editor_test::create_missing_files(session);
		root = session.view().project.root;
		bool ok = editor_test::write_bytes(root + "/models/crate.3di", test_io::read_file(synth("crate.3di"))) &&
		          editor_test::write_bytes(root + "/models/armory.3di", test_io::read_file(synth("armory.3di"))) &&
		          editor_test::write_bytes(root + "/models/house.3di", test_io::read_file(synth("house.3di"))) &&
		          editor_test::write_bytes(root + "/sounds/game.lwf", bank_of({"EXPLO_PUMP"})) &&
		          editor_test::write_bytes(root + "/sounds/explo_pump.wav", test_io::read_file(repo() + "/fixtures/lwf/tone.wav")) &&
		          editor_test::write_text(root + "/defs/items.def", editor_test::crlf(kItems));
		editor_test::handle_to_end(session, request::rescan());
		while (session.view().activity.validation.running) session.poll();
		made = ok && open("models/crate.3di");
	}
	bool open(const std::string &file) {
		path = file;
		session.handle(request::open_document(path));
		const DocumentBase *document = session.document_for(path);
		if (document) path = document->path();
		return document && session.view().documents.previews[ViewportKind::Model].path == path;
	}
	const ModelViewport *viewport() {
		ViewportModel *model = session.viewports().follow_one(session.view(), path, ViewportKind::Model);
		return dynamic_cast<const ModelViewport *>(model);
	}
	bool set(const std::string &change) {
		session.handle(request::set_viewport(path, change));
		return session.outcome().done();
	}
	std::string refusal(const std::string &change) {
		session.handle(request::set_viewport(path, change));
		return session.outcome().done() || session.outcome().findings.empty() ? std::string()
		                                                                       : session.outcome().findings.front().message;
	}
	bool hold(int32_t ticks) { return set(R"({"clock": {"ticks": )" + std::to_string(ticks) + R"(, "playing": false}})"); }
	JsonValue json() {
		const ModelViewport *model = viewport();
		return model ? viewport_to_json(session.view(), *model, JsonPage()) : JsonValue();
	}
	const JsonValue *damage(JsonValue &shown) { return shown.get("body") ? shown.get("body")->get("damage") : nullptr; }
	const PreviewClock &clock() { return session.viewports().clock(); }
	// From tick 0, the clock run to `ticks`: the sounds the death fired on the way.
	std::vector<ClipSoundFired> run(int32_t ticks) {
		set(R"({"clock": {"ticks": 0, "playing": true, "rate": 1}})");
		const uint64_t before = session.viewports().clip_sound_seq();
		while (session.viewports().clock().ticks() < ticks) session.advance(0.5 / 62.5);
		std::vector<ClipSoundFired> out;
		if (const ModelViewport *model = viewport())
			for (const ClipSoundFired &fired : model->sounds_fired())
				if (fired.seq > before) out.push_back(fired);
		return out;
	}
};

std::vector<std::string> leg_kinds(const DamagePlan &plan) {
	std::vector<std::string> out;
	for (const DamageLeg &leg : plan.legs) out.push_back(std::to_string(leg.tick) + " " + leg.kind);
	return out;
}

} // namespace

// Every register's words: a group the popup lists, a label, a writer; the destroy fade's six as shares.
static int test_ctrl_words() {
	std::set<std::string> groups;
	for (const CtrlRegisterGroup &group : ctrl_register_groups()) groups.insert(group.token);
	for (int ordinal = 0; ordinal < threedi::THREEDI_CTRL_REGISTER_COUNT; ++ordinal) {
		const CtrlRegisterWords &words = ctrl_register_words(ordinal);
		TEST_EXPECT(groups.count(words.group) == 1);
		TEST_EXPECT(*words.label != '\0' && *words.driven != '\0' && std::string(words.cite).rfind("[orig: ", 0) == 0);
		TEST_EXPECT(words.min < words.max);
	}
	TEST_EXPECT(*ctrl_register_words(-1).label == '\0' && *ctrl_register_words(96).label == '\0');
	for (int i = 0; i < 6; ++i) {
		const int ordinal = threedi::THREEDI_CTRL_OBJECT_DESTROY + i;
		const CtrlRegisterWords &words = ctrl_register_words(ordinal);
		TEST_EXPECT(ctrl_register_is_destroy_phase(ordinal));
		TEST_EXPECT(std::string(words.group) == "destruction" && words.share && words.min == 0 && words.max == 65536);
		TEST_EXPECT(words.writer == CtrlWriter::Ported &&
		            std::string(words.cite) == "[orig: Entity_PublishSwapFadePhases @ 0x5C3F40]");
	}
	TEST_EXPECT(!ctrl_register_is_destroy_phase(threedi::THREEDI_CTRL_VEHICLE_SPECIAL1));
	TEST_EXPECT(std::string(ctrl_register_words(threedi::THREEDI_CTRL_OBJECT_DESTROY).label) == "Destroy fade" &&
	            std::string(ctrl_register_words(threedi::THREEDI_CTRL_OBJECT_DESTROY03).label) == "Destroy phase 3");
	// The census: no writer reaches LOD_FRAC or the trigger; the HUD's health has one not ported.
	TEST_EXPECT(ctrl_register_words(threedi::THREEDI_CTRL_LOD_FRAC).writer == CtrlWriter::None);
	TEST_EXPECT(ctrl_register_words(threedi::THREEDI_CTRL_WPN_TRIGGER).writer == CtrlWriter::None);
	TEST_EXPECT(ctrl_register_words(threedi::THREEDI_CTRL_HUD_HEALTH).writer == CtrlWriter::Open);
	TEST_EXPECT(ctrl_register_words(threedi::THREEDI_CTRL_DOOR_07).writer == CtrlWriter::Ported &&
	            std::string(ctrl_register_words(threedi::THREEDI_CTRL_DOOR_07).label) == "Door 7");
	TEST_EXPECT(std::string(ctrl_writer_token(CtrlWriter::Open)) == "open");
	std::printf("test_ctrl_words passed\n");
	return 0;
}

// A gnrc item's death as the game runs it, planned from its catalog and its husk models; the frame at a
// tick; the option's wire.
static int test_plan_and_frame() {
	DamageProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	const ModelViewport *model = project.viewport();
	TEST_EXPECT(model && model->view_status() == ModelViewStatus::Ready);
	if (!model) return 1;
	// The crate is the pump's graphic and the crate stack's; the house's statue is no use of the crate.
	TEST_EXPECT(model->damage_uses().size() == 2 && model->damage_uses()[0].item == "Pump station" &&
	            model->damage_uses()[0].role == DamageRole::Graphic && model->damage_uses()[0].field == "graphic" &&
	            model->damage_uses()[1].item == "Crate stack");
	const DamageItem &item = model->damage_item();
	TEST_EXPECT(item.found && item.death_class == world::ItemDeathClass::kGnrc && item.husk == "armory" &&
	            item.destroy_timing_ticks[0] == 31 && item.destroy_timing_ticks[1] == 62 &&
	            item.destroy_timing_ticks[2] == 15 && item.sounddeath == "EXPLO_PUMP");
	const DamagePlan &plan = model->damage_plan();
	TEST_EXPECT(plan.swaps && plan.swap_tick == 4 && plan.husk == "armory" && plan.pieces_from == "armory");
	TEST_EXPECT(plan.class_words.rfind("gnrc (gnrc): dead at once", 0) == 0);
	// The armory's four parts: the hull stays, the WHEEL rolls its chance, the two chunks always leave.
	TEST_EXPECT(plan.pieces.size() == 3 && plan.pieces[0].section == 1 && plan.pieces[0].type == "WHEEL" &&
	            plan.pieces[0].chance == 0.5f && plan.pieces[1].type == "CHUNK_M" && plan.pieces[2].type == "CHUNK_S");
	TEST_EXPECT(plan.hidden_sections == 0xCu);
	// In order: the swap, the flash (an object, no decoration), the pieces, the sound, the Dead bank (the
	// armory names no Dead point: once at the item; no Fire point: no Fire bank), the blast; the fade after
	// the delay.
	TEST_EXPECT(leg_kinds(plan) == std::vector<std::string>({"4 swap", "4 flash", "4 pieces", "4 sound", "4 effect",
	                                                         "4 blast", "31 fade"}));
	if (plan.legs.size() == 7) {
		TEST_EXPECT(plan.legs[0].words == "The husk swap: armory is drawn in the item's place from here on." &&
		            plan.legs[0].cite == "[orig: sub_407020 @ 0x407020 -> Entity_UpdateDeathTransforms @ 0x494660]");
		TEST_EXPECT(plan.legs[2].words.find("1 WHEEL (a 50% chance: kept here), 2 CHUNK_M, 3 CHUNK_S") != std::string::npos);
		TEST_EXPECT(plan.legs[3].name == "EXPLO_PUMP" && plan.legs[3].shown == "played");
		TEST_EXPECT(plan.legs[4].name == "fx_pump_dead" && plan.legs[4].shown == "named" &&
		            plan.legs[4].words.find("once at the item") != std::string::npos);
		TEST_EXPECT(plan.legs[5].name == "kz_OrganicBlast" &&
		            plan.legs[5].words.find("one kz_OrganicBlast at the item") != std::string::npos);
	}
	// The frame: intact before the swap; husked from it, the fade waiting out the delay; then the phases.
	TEST_EXPECT(!damage_frame(plan, item, 3).husked && damage_frame(plan, item, 3).fade_elapsed == -1);
	const DamageFrame at_swap = damage_frame(plan, item, 4);
	TEST_EXPECT(at_swap.husked && at_swap.hidden_sections == 0xCu && at_swap.fade_elapsed == -1 &&
	            at_swap.fade.phases_q16 == (std::array<int32_t, 6>{}));
	const DamageFrame later = damage_frame(plan, item, 93);
	TEST_EXPECT(later.fade_elapsed == 62);
	TEST_EXPECT(later.fade.phases_q16 == world::destroy_fade_phases(62, item.destroy_timing_ticks).phases_q16);
	TEST_EXPECT(later.fade.phases_q16[1] == 65536 && later.fade.phases_q16[2] == int32_t(47.0 / 62.0 * 65536.0));
	TEST_EXPECT(damage_fade_end_tick(plan, item) == 31 + 62 + 60);

	// The option on the wire, and its refusals.
	TEST_EXPECT(project.refusal(R"({"options": {"damage": {"state": "burnt"}}})") ==
	            "options.damage.state is \"intact\" or \"destroyed\".");
	TEST_EXPECT(project.refusal(R"({"options": {"damage": {"when": 3}}})") ==
	            "Unknown damage option \"when\" (it takes state, item).");
	TEST_EXPECT(project.refusal(R"({"options": {"damage": 1}})") == "options.damage is an object {state, item}.");
	JsonValue shown = project.json();
	const JsonValue *options = shown.get("options") ? shown.get("options")->get("damage") : nullptr;
	TEST_EXPECT(options && options->get_string("state", "") == "intact" && options->get_string("item", "x").empty());
	std::printf("test_plan_and_frame passed\n");
	return 0;
}

// The picture: the crate until the swap, the armory from it (not the document's picture); the hidden
// sections and the six destroy registers over the held ones; Intact again.
static int test_destroyed_picture() {
	DamageProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	// A held register of the destroy fade's and another: the state drives the first, keeps the second.
	TEST_EXPECT(project.set(R"({"options": {"ctrl": {"OBJECT_DESTROY02": 1000, "FLICKER": 77}}})"));
	TEST_EXPECT(project.set(R"({"options": {"damage": {"state": "destroyed"}}})"));
	TEST_EXPECT(project.hold(2));
	const ModelViewport *model = project.viewport();
	TEST_EXPECT(model && model->damage_driven() && model->damage_drawn().empty() && model->model() &&
	            model->model()->lod_count > 0 && model->model()->lods[0].render_object_count == 1);
	TEST_EXPECT(project.json().get_bool("current", false));
	std::map<std::string, int64_t> ctrl = model->ctrl_at(project.clock());
	TEST_EXPECT(ctrl == (std::map<std::string, int64_t>{{"FLICKER", 77}}) && model->hidden_sections_at(project.clock()) == 0);
	// The swap: the armory drawn in the crate's place, not the document's picture.
	TEST_EXPECT(project.hold(4));
	model = project.viewport();
	TEST_EXPECT(model && model->damage_drawn() == "armory.3di" && model->model() &&
	            model->model()->lods[0].render_object_count == 4);
	TEST_EXPECT(!project.json().get_bool("current", true));
	TEST_EXPECT(model && model->hidden_sections_at(project.clock()) == 0xCu);
	// The fade on the clock: its values in the destroy registers' place, the held ones kept.
	TEST_EXPECT(project.hold(93));
	model = project.viewport();
	const std::array<int32_t, 6> phases = world::destroy_fade_phases(62, model->damage_item().destroy_timing_ticks).phases_q16;
	ctrl = model->ctrl_at(project.clock());
	TEST_EXPECT(ctrl.size() == 7 && ctrl["FLICKER"] == 77 && ctrl["OBJECT_DESTROY"] == phases[0] &&
	            ctrl["OBJECT_DESTROY01"] == 65536 && ctrl["OBJECT_DESTROY02"] == phases[2] &&
	            ctrl["OBJECT_DESTROY05"] == phases[5]);
	// The envelope: the state at the clock, the legs due, the registers' words and who drives them.
	JsonValue shown = project.json();
	const JsonValue *damage = project.damage(shown);
	TEST_EXPECT(damage && damage->get_string("state", "") == "destroyed" && damage->get_string("item", "") == "Pump station" &&
	            damage->get_string("role", "") == "graphic" && damage->get_bool("driven", false) &&
	            damage->get_string("drawn", "") == "armory.3di" && damage->get_string("husk_file", "") == "armory.3di");
	const JsonValue *frame = damage ? damage->get("frame") : nullptr;
	TEST_EXPECT(frame && frame->get("ticks")->number == 93 && frame->get_bool("husked", false) &&
	            frame->get("fade_elapsed")->number == 62 && frame->get("hidden_sections")->number == 12 &&
	            frame->get("phases")->array.size() == 6 && frame->get("phases")->array[1].number == 65536);
	const JsonValue *legs = damage ? damage->get("legs") : nullptr;
	TEST_EXPECT(legs && legs->array.size() == 7 && legs->array[0].get_string("kind", "") == "swap" &&
	            legs->array[0].get_bool("due", false) && legs->array[6].get_string("kind", "") == "fade" &&
	            legs->array[6].get_bool("due", false));
	const JsonValue *uses = damage ? damage->get("uses") : nullptr;
	TEST_EXPECT(uses && uses->array.size() == 2 && uses->array[0].get_bool("chosen", false) &&
	            uses->array[0].get_string("file", "") == "defs/items.def" && !uses->array[0].get_string("locator", "").empty());
	const JsonValue *pieces = damage ? damage->get("pieces") : nullptr;
	TEST_EXPECT(pieces && pieces->array.size() == 3 && pieces->array[0].get("chance")->number == 0.5);
	// The registers: the armory's FLICKER, by its group and its writer.
	const JsonValue *registers = shown.get("body") ? shown.get("body")->get("registers") : nullptr;
	TEST_EXPECT(registers && registers->array.size() == 1 && registers->array[0].get_string("name", "") == "FLICKER" &&
	            registers->array[0].get_string("group", "") == "light" && registers->array[0].get("value")->number == 77 &&
	            registers->array[0].get_string("writer", "") == "ported" && !registers->array[0].get_bool("damage", true));
	// Back to the start of the death: the crate, and the document's picture again.
	TEST_EXPECT(project.hold(0));
	model = project.viewport();
	TEST_EXPECT(model && model->damage_drawn().empty() && model->model()->lods[0].render_object_count == 1);
	// Another item's death: the crate stack's gnrl tail, husked at once.
	TEST_EXPECT(project.set(R"({"options": {"damage": {"item": "Crate stack"}}})"));
	model = project.viewport();
	TEST_EXPECT(model && model->damage_item().name == "Crate stack" && model->damage_plan().swap_tick == 0 &&
	            leg_kinds(model->damage_plan()) == std::vector<std::string>({"0 swap", "0 sound", "0 effect", "0 fade"}) &&
	            model->damage_drawn() == "armory.3di");
	TEST_EXPECT(model && model->damage_plan().legs.size() == 4 &&
	            model->damage_plan().legs[2].words == "The death effect fx_crate_dead, once at the item.");
	// Intact: the document's picture, the held registers alone.
	TEST_EXPECT(project.set(R"({"options": {"damage": {"state": "intact"}}})"));
	model = project.viewport();
	TEST_EXPECT(model && !model->damage_driven() && model->damage_drawn().empty() &&
	            model->ctrl_at(project.clock()).at("OBJECT_DESTROY02") == 1000);
	std::printf("test_destroyed_picture passed\n");
	return 0;
}

// The husk's own viewport: named as the husk, driven over itself; the statue's null class lands none.
static int test_husk_viewport() {
	DamageProject project;
	TEST_EXPECT(project.made && project.open("models/armory.3di"));
	const ModelViewport *model = project.viewport();
	TEST_EXPECT(model && model->damage_uses().size() == 3 && model->damage_uses()[0].role == DamageRole::Husk);
	TEST_EXPECT(project.set(R"({"options": {"damage": {"state": "destroyed"}}})"));
	TEST_EXPECT(project.hold(10));
	model = project.viewport();
	TEST_EXPECT(model && model->damage_driven() && model->damage_drawn().empty() &&
	            model->hidden_sections_at(project.clock()) == 0xCu &&
	            model->model()->lods[0].render_object_count == 4);
	// The statue authors no ai_function: the null row, which never dies.
	TEST_EXPECT(project.set(R"({"options": {"damage": {"item": "Statue"}}})"));
	model = project.viewport();
	TEST_EXPECT(model && model->damage_item().death_class == world::ItemDeathClass::kNull && !model->damage_plan().swaps &&
	            !model->damage_driven() && model->damage_plan().legs.empty());
	TEST_EXPECT(model && model->damage_note().rfind("null (null): its callback only re-arms its think", 0) == 0);
	std::printf("test_husk_viewport passed\n");
	return 0;
}

// The death sound heard as the clock runs from the death (Play destroy), once, at its tick; nothing over a
// seek; nothing while intact.
static int test_death_sound() {
	DamageProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	TEST_EXPECT(project.run(12).empty()); // intact: no death
	TEST_EXPECT(project.set(R"({"options": {"damage": {"state": "destroyed"}}})"));
	std::vector<ClipSoundFired> fired = project.run(12);
	TEST_EXPECT(fired.size() == 1);
	if (fired.size() == 1) {
		TEST_EXPECT(fired[0].tick == 4 && fired[0].set == "EXPLO_PUMP" && fired[0].bank == "game.lwf" &&
		            fired[0].state == "played" && fired[0].slot == -1 && fired[0].voices.size() == 1 &&
		            fired[0].voices[0].path == "sounds/explo_pump.wav");
		TEST_EXPECT(fired[0].words.rfind("Tick 4 (the death sound): ", 0) == 0);
	}
	// A seek past it and on: nothing.
	TEST_EXPECT(project.set(R"({"clock": {"ticks": 8, "playing": true}})"));
	const uint64_t before = project.session.viewports().clip_sound_seq();
	while (project.clock().ticks() < 20) project.session.advance(0.5 / 62.5);
	TEST_EXPECT(project.session.viewports().clip_sound_seq() == before);
	// The gnrl tail's sound on the death tick itself: heard as the clock leaves it.
	TEST_EXPECT(project.set(R"({"options": {"damage": {"item": "Crate stack"}}})"));
	fired = project.run(6);
	TEST_EXPECT(fired.size() == 1 && fired[0].tick == 0 && fired[0].set == "EXPLO_PUMP");
	// The Shell is handed it.
	const std::vector<ClipSoundPlay> plays = project.session.clip_sounds_since(fired.empty() ? 0 : fired[0].seq - 1);
	TEST_EXPECT(plays.size() == 1 && plays[0].voices.size() == 1 && plays[0].voices[0].path == "sounds/explo_pump.wav");
	// The envelope's sounds_fired.
	JsonValue shown = project.json();
	const JsonValue *damage = project.damage(shown);
	const JsonValue *wire = damage ? damage->get("sounds_fired") : nullptr;
	TEST_EXPECT(wire && !wire->array.empty() && wire->array.back().get_string("set", "") == "EXPLO_PUMP" &&
	            wire->array.back().get_string("keyword", "x").empty());
	TEST_EXPECT(damage && damage->get_bool("playing", false) && damage->get_bool("driven", false));
	// A gnrl item with no husk: its death plays (its sound on the death tick), the graphic kept, nothing driven.
	TEST_EXPECT(project.open("models/house.3di"));
	TEST_EXPECT(project.set(R"({"options": {"damage": {"state": "destroyed", "item": "Shed sign"}}})"));
	const ModelViewport *house = project.viewport();
	TEST_EXPECT(house && house->damage_playing() && !house->damage_driven() && house->damage_drawn().empty() &&
	            house->damage_note() == "Shed sign authors no husk model: the preview keeps drawing its graphic.");
	const std::vector<ClipSoundFired> sign = project.run(4);
	TEST_EXPECT(sign.size() == 1 && sign[0].tick == 0 && sign[0].set == "EXPLO_PUMP");
	std::printf("test_death_sound passed\n");
	return 0;
}

// The retail leg (--retail, OPENNOVA_JO_ASSETS): JO's ITEMS.DEF and every model its damageable items name
// that the tree holds, written into one project; each damageable item's graphic opened destroyed past its
// swap: the husk the game draws swapped in, its pieces those of the debris rows its def names.
static int test_retail() {
	const std::string items = retail::asset_file("ITEMS.DEF");
	if (items.empty()) return retail::skip_leg("OPENNOVA_JO_ASSETS (ITEMS.DEF and the models it names)");
	DamageProject project;
	TEST_EXPECT(project.made);
	const std::vector<uint8_t> bytes = test_io::read_file(items);
	def::DefItemsFile parsed{};
	TEST_EXPECT(def::def_parse_items_memory(bytes.data(), bytes.size(), &parsed) == 0);
	struct Damageable {
		std::string item, graphic, husk;
	};
	std::vector<Damageable> damageable;
	std::set<std::string> models;
	for (size_t i = 0; i < parsed.count; ++i) {
		const def::DefItemDef &def = parsed.entries[i];
		if (def.husk[0] == '\0' || def.graphic[0] == '\0') continue;
		const std::string graphic = retail::asset_file((std::string(def.graphic) + ".3di").c_str());
		const std::string husk = retail::asset_file((std::string(def.husk) + ".3di").c_str());
		if (graphic.empty() || husk.empty()) continue;
		damageable.push_back({def.display_name, std::filesystem::path(graphic).filename().string(),
		                      std::filesystem::path(husk).filename().string()});
		models.insert(graphic);
		models.insert(husk);
		if (def.huskfinal[0] != '\0') {
			const std::string final_husk = retail::asset_file((std::string(def.huskfinal) + ".3di").c_str());
			if (!final_husk.empty()) models.insert(final_husk);
		}
	}
	def::def_free_items(&parsed);
	if (damageable.empty()) return retail::skip_leg("OPENNOVA_JO_ASSETS with the models JO's damageable items name");
	for (const std::string &model : models)
		TEST_EXPECT(editor_test::write_bytes(project.root + "/models/" + std::filesystem::path(model).filename().string(),
		                                     test_io::read_file(model)));
	TEST_EXPECT(editor_test::write_bytes(project.root + "/defs/items.def", bytes));
	editor_test::handle_to_end(project.session, request::rescan());
	while (project.session.view().activity.validation.running) project.session.poll();
	size_t swapped = 0, pieces = 0, gnrc = 0, brains = 0, tails = 0;
	std::set<std::string> opened;
	for (const Damageable &one : damageable) {
		if (strutil::iequals(one.graphic, one.husk) || !opened.insert(strutil::to_lower(one.graphic)).second) continue;
		TEST_EXPECT(project.open("models/" + one.graphic));
		TEST_EXPECT(project.set(R"({"options": {"damage": {"state": "destroyed", "item": )" +
		                        io::json_write(io::json_string(one.item)) + "}}}"));
		TEST_EXPECT(project.hold(64));
		const ModelViewport *model = project.viewport();
		TEST_EXPECT(model && model->view_status() == ModelViewStatus::Ready);
		if (!model || !model->damage_item().found) continue;
		const DamagePlan &plan = model->damage_plan();
		if (!plan.swaps) continue;
		TEST_EXPECT(strutil::iequals(model->damage_drawn(), one.husk));
		if (strutil::iequals(model->damage_drawn(), one.husk)) ++swapped;
		pieces += plan.pieces.size();
		if (model->damage_item().ai_class) ++brains;
		else if (model->damage_item().death_class == world::ItemDeathClass::kGnrc) ++gnrc;
		else ++tails;
		TEST_EXPECT(!plan.legs.empty() && plan.legs.front().kind == "swap");
	}
	TEST_EXPECT(swapped > 0);
	std::printf("retail: %zu damageable graphics destroyed (%zu gnrc, %zu brains, %zu class tails), their husks "
	            "swapped in, %zu death pieces named\n",
	            swapped, gnrc, brains, tails, pieces);
	std::printf("test_retail passed\n");
	return 0;
}

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	TEST_EXPECT(test_ctrl_words() == 0);
	TEST_EXPECT(test_plan_and_frame() == 0);
	TEST_EXPECT(test_destroyed_picture() == 0);
	TEST_EXPECT(test_husk_viewport() == 0);
	TEST_EXPECT(test_death_sound() == 0);
	TEST_EXPECT(test_retail() == 0);
	std::printf("editor_model_damage OK\n");
	return 0;
}
