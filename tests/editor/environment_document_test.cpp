// The environment document (the deep-integration plan's DI-19a; editor/documents/environment_document.h):
// a .env through the engine's own reader and writer, its fields the game's keywords in the units the file
// writes them, its keyframes a list of at most 16, what the reader reads otherwise than written as the
// file's source findings; its references the graph's edges; the missions that run on it, each with its
// terrain, its header's overrides and its water plane (session/environment_uses, the environment_uses
// query); an edit followed by the graph, undone, saved. The retail leg (OPENNOVA_JO_DIR): every .env of
// the install read through the document and written again reads back as the same environment, field for
// field.
#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/vfs/vfs.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/document_types.h>
#include <editor/documents/environment_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/project_build/build_run.h>
#include <editor/session/environment_uses.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/env/env.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/tga/tga.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::env::Config;
using opennova::env::Keyframe;
using opennova::env::Rgb;

namespace {

constexpr NodeKind kEnvironment = node_kind(EnvironmentKind::Environment);
constexpr NodeKind kKeyframe = node_kind(EnvironmentKind::Keyframe);

std::vector<uint8_t> bytes_of(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

std::unique_ptr<EnvironmentDocument> load(const std::string &text, const std::string &name = "day.env") {
	auto document = std::make_unique<EnvironmentDocument>();
	Diagnostic error;
	if (!document->load_bytes(bytes_of(text), name, AssetKind::Environment, "jo", error)) {
		std::fprintf(stderr, "load %s: %s\n", name.c_str(), error.message.c_str());
		return nullptr;
	}
	return document;
}

NodeAddress row_address(const EnvironmentDocument &document) {
	const EnvironmentRow *row = document.environment_row();
	return row ? NodeAddress{row->id, kEnvironment, 0} : NodeAddress{};
}

NodeAddress keyframe_address(const EnvironmentDocument &document, size_t index) {
	const std::vector<Document::Collection> lists = document.collections_of(row_address(document));
	if (lists.empty() || index >= lists[0].ids.size()) return {};
	return {row_address(document).row, kKeyframe, lists[0].ids[index]};
}

Value read(const Document &document, const NodeAddress &address, const std::string &field) {
	Value value;
	document.get(address, field, value);
	return value;
}

bool set(Document &document, const NodeAddress &address, const std::string &field, Value value, std::string *why = nullptr) {
	Edit edit;
	edit.address = address;
	edit.field = field;
	edit.value = std::move(value);
	Diagnostic error;
	const bool applied = document.apply(edit, error);
	if (why) *why = error.message;
	return applied;
}

bool presence(Document &document, const NodeAddress &address, const std::string &field, bool written) {
	Edit edit;
	edit.operation = written ? EditOperation::Write : EditOperation::Clear;
	edit.address = address;
	edit.field = field;
	Diagnostic error;
	return document.apply(edit, error);
}

bool add_keyframe(Document &document, const NodeAddress &row, size_t position = SIZE_MAX) {
	Edit edit;
	edit.operation = EditOperation::Add;
	edit.address = {row.row, kKeyframe, 0};
	edit.position = position;
	Diagnostic error;
	return document.apply(edit, error);
}

uint8_t byte(float channel) { return uint8_t(std::max(0, std::min(255, int(channel * 255.0f + 0.5f)))); }

bool same_rgb(const Rgb &a, const Rgb &b) {
	return byte(a.r) == byte(b.r) && byte(a.g) == byte(b.g) && byte(a.b) == byte(b.b);
}

bool same_keyframe(const Keyframe &a, const Keyframe &b) {
	return a.time == b.time && same_rgb(a.sun, b.sun) && same_rgb(a.moon, b.moon) && same_rgb(a.sky, b.sky) &&
	       same_rgb(a.ground, b.ground) && same_rgb(a.fog, b.fog) && same_rgb(a.skyfog, b.skyfog) &&
	       same_rgb(a.skybase, b.skybase) && same_rgb(a.skybright, b.skybright) &&
	       same_rgb(a.skyhighlight, b.skyhighlight) && same_rgb(a.cloudbase, b.cloudbase) &&
	       same_rgb(a.cloudhighlight, b.cloudhighlight) && same_rgb(a.cloudedge, b.cloudedge);
}

// Every field the reader fills, compared as the game holds it: names and words as written, numbers
// exactly, colours by their bytes; "" when alike, else the first field that differs.
std::string config_difference(const Config &a, const Config &b) {
	if (a.name != b.name) return "enviro_name";
	if (a.timeofday != b.timeofday) return "timeofday";
	if (a.envscale != b.envscale) return "envscale";
	if (a.curtime != b.curtime) return "curtime";
	if (a.fog_level != b.fog_level) return "fog_level";
	if (a.fog_type != b.fog_type) return "fog_type";
	if (!same_rgb(a.terrain_rgb, b.terrain_rgb)) return "terrain_rgb";
	if (!same_rgb(a.water_rgb, b.water_rgb)) return "water_rgb";
	if (a.water_height_set != b.water_height_set || (a.water_height_set && a.water_height != b.water_height))
		return "water_height";
	if (!same_rgb(a.cloud_rgb, b.cloud_rgb)) return "cloud_rgb";
	if (!same_rgb(a.vertex_rgb, b.vertex_rgb)) return "vertex_rgb";
	if (!same_rgb(a.lightning_rgb, b.lightning_rgb)) return "lightning_rgb";
	if (!same_rgb(a.ceiling_rgb, b.ceiling_rgb)) return "ceiling_rgb";
	if (!same_rgb(a.floor_rgb, b.floor_rgb)) return "floor_rgb";
	if (a.sky_speed != b.sky_speed) return "sky_speed";
	if (a.sky_height != b.sky_height) return "sky_height";
	if (a.sky_map1 != b.sky_map1) return "sky_map1";
	if (a.sky_map2 != b.sky_map2) return "sky_map2";
	if (a.sun_3di != b.sun_3di) return "sun_3di";
	if (a.moon_3di != b.moon_3di) return "moon_3di";
	if (a.glare_3di != b.glare_3di) return "glare_3di";
	if (a.star_3di != b.star_3di) return "star_3di";
	if (a.iris_percent != b.iris_percent) return "iris_percent";
	if (a.iris_center != b.iris_center) return "iris_center";
	if (a.water_murk != b.water_murk) return "water_murk";
	if (a.advanced_clouds != b.advanced_clouds) return "advanced_clouds";
	if (a.tod_rate_set != b.tod_rate_set || (a.tod_rate_set && a.tod_rate != b.tod_rate)) return "tod_rate";
	if (!same_keyframe(a.scratch, b.scratch)) return "the colours with no keyframe";
	if (a.keyframes.size() != b.keyframes.size()) return "the keyframe count";
	for (size_t i = 0; i < a.keyframes.size(); ++i)
		if (!same_keyframe(a.keyframes[i], b.keyframes[i])) return "keyframe " + std::to_string(i + 1);
	return std::string();
}

Config parsed(const std::string &text) {
	Config out;
	std::istringstream input(text);
	std::string error;
	opennova::env::load_env(input, out, error);
	return out;
}

bool has_issue(const DocumentBase &document, size_t line, bool blocks, const std::string &words) {
	for (const SourceIssue &issue : document.issues())
		if (issue.line == line && issue.blocks == blocks && issue.message.find(words) != std::string::npos) return true;
	return false;
}

// The type, its kind's row, its graph reading.
int test_type() {
	const DocumentType *type = document_type_for(AssetKind::Environment);
	TEST_EXPECT(type && type->id == DocumentTypeId::Environment && std::string(type->name) == "environment");
	TEST_EXPECT(type && document_content(*type) == DocumentContent::Records);
	TEST_EXPECT(is_editable_kind(AssetKind::Environment) && graph_reads_kind(AssetKind::Environment));
	TEST_EXPECT(environment_table().well_formed());
	return 0;
}

// The fields in the file's units, their rules, the keyframes' list, the optional lines, a save.
int test_document(const std::string &repo) {
	const std::vector<uint8_t> fixture = test_io::read_file(repo + "/fixtures/env/synth_full.env");
	TEST_EXPECT(!fixture.empty());
	auto document = load(std::string(fixture.begin(), fixture.end()), "synth_full.env");
	TEST_EXPECT(document != nullptr);
	if (!document) return 1;
	EnvironmentDocument &env = *document;
	const NodeAddress row = row_address(env);
	TEST_EXPECT(env.rows().size() == 1 && env.issues().empty() && !env.blocked());
	TEST_EXPECT(read(env, row, "curtime") == Value(int64_t(1200)) && read(env, row, "fog_level") == Value(int64_t(1000)) &&
	            read(env, row, "fog_type") == Value(int64_t(2)) && read(env, row, "sky_map1") == Value(std::string("Cloud01.pcx")) &&
	            read(env, row, "water_rgb_r") == Value(int64_t(48)) && read(env, row, "sky_height") == Value(int64_t(160)) &&
	            read(env, row, "envscale") == Value(1.0));
	TEST_EXPECT(env.present(row, "sky_height") && !env.present(row, "water_height") && !env.present(row, "tod_rate"));
	// The keyframes, then the terrain keys (none here).
	TEST_EXPECT(env.collections_of(row).size() == 2 && env.collections_of(row)[0].ids.size() == 10 &&
	            env.collections_of(row)[1].ids.empty());
	TEST_EXPECT(env.record_title(keyframe_address(env, 0)) == "Keyframe 02:00" && env.record_title(row) == "Environment");
	// The colours with no keyframe are not read while the file has keyframes; the name and the vertex tint
	// never are.
	{
		const FieldSchema *scratch = nullptr, *name = nullptr;
		for (const FieldSchema &field : env.fields(kEnvironment)) {
			if (field.id == "static_sun_rgb_r") scratch = &field;
			if (field.id == "enviro_name") name = &field;
		}
		TEST_EXPECT(scratch && env.field_on(row, *scratch).applies == Applicability::Ignored);
		TEST_EXPECT(name && env.field_on(row, *name).applies == Applicability::Ignored);
	}
	// A cloud layer names a texture through the sky's loader; the sun a model.
	{
		const FieldSchema *sky = nullptr, *sun = nullptr;
		for (const FieldSchema &field : env.fields(kEnvironment)) {
			if (field.id == "sky_map1") sky = &field;
			if (field.id == "sun_3di") sun = &field;
		}
		TEST_EXPECT(sky && env.field_on(row, *sky).reference == ReferenceKind::Texture &&
		            env.field_on(row, *sky).loader_arg >= 0);
		TEST_EXPECT(sun && env.field_on(row, *sun).reference == ReferenceKind::Model);
	}
	// The game's rules: a fog distance the 16.16 store holds, an HHMM time, a murk the game reads, a colour
	// byte, a name a line carries.
	std::string why;
	TEST_EXPECT(set(env, row, "fog_level", int64_t(800)) && read(env, row, "fog_level") == Value(int64_t(800)));
	TEST_EXPECT(!set(env, row, "fog_level", int64_t(40000), &why) && why.find("16.16") != std::string::npos);
	TEST_EXPECT(!set(env, row, "curtime", int64_t(1275), &why) && why.find("HHMM") != std::string::npos);
	TEST_EXPECT(set(env, row, "curtime", int64_t(30)));
	TEST_EXPECT(!set(env, row, "water_murk", 1.0, &why) && why.find("0.99") != std::string::npos);
	TEST_EXPECT(!set(env, row, "water_rgb_g", int64_t(256)));
	TEST_EXPECT(!set(env, row, "sun_3di", std::string("a\"b.3di")));
	TEST_EXPECT(set(env, row, "sky_map2", std::string("my clouds.pcx")));
	// The optional lines: a water height Set is written, Cleared left out; the sky height left out keeps the
	// engine's default and its own value apart.
	TEST_EXPECT(set(env, row, "water_height", int64_t(60)) && env.present(row, "water_height"));
	{
		const std::string text = env.serialize().text;
		TEST_EXPECT(text.find("water_height 60\r\n") != std::string::npos && text.find("curtime 0030\r\n") != std::string::npos &&
		            text.find("fog_level 800\r\n") != std::string::npos &&
		            text.find("sky_map2 \"my clouds.pcx\"\r\n") != std::string::npos);
	}
	TEST_EXPECT(presence(env, row, "water_height", false) && env.serialize().text.find("water_height") == std::string::npos);
	TEST_EXPECT(presence(env, row, "sky_height", false) && !env.present(row, "sky_height") &&
	            read(env, row, "sky_height") == Value(int64_t(160)) &&
	            env.serialize().text.find("sky_height") == std::string::npos);
	{
		const std::vector<Diagnostic> findings = validate_environment_file(env);
		TEST_EXPECT(std::any_of(findings.begin(), findings.end(), [](const Diagnostic &d) {
			return d.code() == "environment.sky_height_default" && d.field == "sky_height" &&
			       d.severity == DiagnosticSeverity::Warning;
		}));
	}
	TEST_EXPECT(presence(env, row, "sky_height", true) && env.serialize().text.find("sky_height 160\r\n") != std::string::npos);
	TEST_EXPECT(validate_environment_file(env).empty());
	// The keyframes: up to 16, a new one between its neighbours with the one before's colours.
	TEST_EXPECT(add_keyframe(env, row, 1));
	TEST_EXPECT(env.collections_of(row)[0].ids.size() == 11 &&
	            read(env, keyframe_address(env, 1), "time") == Value(int64_t(300)) &&
	            read(env, keyframe_address(env, 1), "moon_rgb_b") == read(env, keyframe_address(env, 0), "moon_rgb_b"));
	while (env.collections_of(row)[0].ids.size() < 16) TEST_EXPECT(add_keyframe(env, row));
	TEST_EXPECT(!add_keyframe(env, row));
	// A keyframe moved out of time order is written in time order, and the save says so.
	TEST_EXPECT(set(env, keyframe_address(env, 0), "time", int64_t(2300)));
	{
		const SerializeResult saved = env.serialize();
		TEST_EXPECT(saved.ok() && saved.notes.size() == 1 && saved.notes[0].find("time order") != std::string::npos);
		const Config back = parsed(saved.text);
		TEST_EXPECT(back.keyframes.size() == 16 &&
		            std::is_sorted(back.keyframes.begin(), back.keyframes.end(),
		                           [](const Keyframe &a, const Keyframe &b) { return a.time < b.time; }));
	}
	// Undo all the way back; the document writes what it read.
	while (env.can_undo()) env.undo();
	TEST_EXPECT(!env.dirty() && config_difference(parsed(env.serialize().text), parsed(std::string(fixture.begin(), fixture.end())))
	                                   .empty());
	// An environment keeps its one row.
	{
		Edit add;
		add.operation = EditOperation::Add;
		add.address = {0, kEnvironment, 0};
		Diagnostic error;
		TEST_EXPECT(!env.apply(add, error));
	}
	std::printf("document: the fields in the file's units, the rules, the keyframes, the optional lines\n");
	return 0;
}

// What the reader reads otherwise than written: each a source finding of its line.
int test_source_issues() {
	const std::string text = "enviro_name \"x\"\r\n"           // 1
	                         "fog_level 500\r\n"                // 2
	                         "fog_level 600\r\n"                // 3: read over line 2
	                         "speling 3\r\n"                    // 4: skipped
	                         "water_rgb 10,20\r\n"              // 5: two of three values
	                         "curtime 2400\r\n"                 // 6: reads 23:00
	                         "sky_height 175\r\n";              // 7
	auto document = load(text);
	TEST_EXPECT(document && !document->blocked());
	if (!document) return 1;
	TEST_EXPECT(has_issue(*document, 2, false, "written again on line 3"));
	TEST_EXPECT(has_issue(*document, 4, false, "skips 'speling'"));
	TEST_EXPECT(has_issue(*document, 5, false, "2 of its three values"));
	TEST_EXPECT(has_issue(*document, 6, false, "23:00"));
	TEST_EXPECT(read(*document, row_address(*document), "fog_level") == Value(int64_t(600)) &&
	            read(*document, row_address(*document), "curtime") == Value(int64_t(2300)));
	const std::vector<Diagnostic> findings = validate_environment_file(*document);
	TEST_EXPECT(findings.size() == 4 && std::all_of(findings.begin(), findings.end(), [](const Diagnostic &d) {
		            return d.code() == "environment.ignored_input" && d.severity == DiagnosticSeverity::Warning;
	            }));
	TEST_EXPECT(document->rewrite_need() == DocumentBase::RewriteNeed::Rewrite);
	// A last line no CR LF ends loses its last byte (retail's FULL_03.ENV ends on "tod_en").
	{
		auto cut = load("sky_height 175\r\ncurtime 1200");
		TEST_EXPECT(cut && has_issue(*cut, 2, false, "no line end") &&
		            read(*cut, row_address(*cut), "curtime") == Value(int64_t(120)));
		TEST_EXPECT(cut && cut->serialize().text.find("curtime 0120\r\n") != std::string::npos);
	}
	// A 17th tod_begin takes no slot: its colours land where the slot pointer is.
	{
		std::string many;
		for (int i = 0; i < 17; ++i)
			many += "tod_begin " + std::to_string(100 * i) + "\r\n    sun_rgb 1,1,1\r\ntod_end\r\n";
		auto blocks = load("sky_height 175\r\n" + many);
		TEST_EXPECT(blocks && has_issue(*blocks, 50, false, "the colours with no keyframe"));
	}
	// An envscale after a colour it would scale otherwise: the record does not hold it, so the file blocks. A terrain
	// keyword is the row's terrain key (D-TERRAIN-18): kept, no issue.
	{
		auto scaled = load("water_rgb 100,100,100\r\nenvscale 2\r\nsky_height 175\r\n");
		TEST_EXPECT(scaled && scaled->blocked() && has_issue(*scaled, 1, true, "envscale"));
		const std::vector<Diagnostic> blocking = validate_environment_file(*scaled);
		TEST_EXPECT(blocking.size() == 1 && blocking[0].code() == "environment.invalid_input" &&
		            blocking[0].severity == DiagnosticSeverity::Error && blocking[0].line == 1);
		auto same = load("envscale 2\r\nwater_rgb 100,100,100\r\nsky_height 175\r\n");
		TEST_EXPECT(same && !same->blocked() && same->issues().empty());
		auto terrain = load("polytrn_colormap map.tga\r\nsky_height 175\r\n");
		TEST_EXPECT(terrain && !terrain->blocked() && terrain->issues().empty() &&
		            terrain->environment_row()->terrain_keys.size() == 1);
		// horizon is read by no arm of either reader (formats/trn trn_parser_key): a skipped line.
		auto skipped = load("horizon 0\r\nsky_height 175\r\n");
		TEST_EXPECT(skipped && !skipped->blocked() && has_issue(*skipped, 1, false, "skips"));
	}
	std::printf("source issues: a line read over, skipped, short, a time read as another, a 17th block, an envscale "
	            "after a colour, a terrain keyword kept\n");
	return 0;
}

NodeAddress terrain_key_address(const EnvironmentDocument &document, size_t index) { return document.terrain_key_address(index); }

const FieldSchema *field_of(const Document &document, NodeKind kind, const std::string &id) {
	for (const FieldSchema &field : document.fields(kind))
		if (field.id == id) return &field;
	return nullptr;
}

// The game's terrain configuration over the Tmap fixture with `environment` read after it (formats/trn
// load_mission_trn), as save_trn writes it: what a mission on that terrain and this environment loads.
std::string terrain_under(const std::string &repo, const std::string &environment) {
	const std::vector<uint8_t> trn = test_io::read_file(repo + "/fixtures/terrain/tmap/Tmap.trn");
	opennova::TrnLaterTexts later;
	later.environment = &environment;
	opennova::TrnConfig config;
	std::string error;
	if (!opennova::load_mission_trn(std::string(trn.begin(), trn.end()), later, config, error)) return "refused: " + error;
	std::ostringstream out;
	return opennova::save_trn(out, config, error) ? out.str() : "unwritable: " + error;
}

// The terrain keys (D-TERRAIN-18): the lines the terrain's parser takes after the mission's .trn, kept in the file's
// order through the read, an edit and the write (the line from scratch), each as the terrain's field of its keyword;
// those the parser reads otherwise than they say a finding of their record.
int test_terrain_keys(const std::string &repo) {
	constexpr NodeKind kTerrainKey = node_kind(EnvironmentKind::TerrainKey);
	const std::string text = "sky_height 175\r\n"                          // 1
	                         "POLYTRN_COLORMAP \"red map.tga\" ; tinted\r\n" // 2: a keyword without case, a quoted name
	                         "fog_level 900\r\n"                           // 3
	                         "lock_topleft 3,4\r\n"                        // 4
	                         "foliage\r\n"                                 // 5
	                         "graphic tree.3di\r\n"                        // 6
	                         "fog_type 2\r\n"                              // 7: the environment's, inside the block
	                         "color_lower 2\r\n"                           // 8
	                         "end\r\n"                                     // 9
	                         "polytrn_detaildensity 64\r\n"                // 10
	                         "terrain_name \"x\"\r\n";                     // 11: no arm of either reader
	auto document = load(text);
	TEST_EXPECT(document && !document->blocked());
	if (!document) return 1;
	EnvironmentDocument &env = *document;
	const EnvironmentRow *row = env.environment_row();
	using opennova::TrnKeyLine;
	const std::vector<TrnKeyLine> expected = {{"polytrn_colormap", {"red map.tga"}},
	                                          {"lock_topleft", {"3", "4"}},
	                                          {"foliage", {}},
	                                          {"graphic", {"tree.3di"}},
	                                          {"color_lower", {"2"}},
	                                          {"end", {}},
	                                          {"polytrn_detaildensity", {"64"}}};
	TEST_EXPECT(row && row->terrain_keys == expected);
	// The skipped terrain_name is the one source issue; the environment's own lines read as before.
	TEST_EXPECT(env.issues().size() == 1 && has_issue(env, 11, false, "skips 'terrain_name'"));
	TEST_EXPECT(env.config()->fog_level == 900.0f && env.config()->fog_type == 2);
	TEST_EXPECT(env.collections_of(row_address(env)).size() == 2 && env.collections_of(row_address(env))[1].ids.size() == 7);
	TEST_EXPECT(env.record_title(terrain_key_address(env, 0)) == "polytrn_colormap \"red map.tga\"" &&
	            env.record_title(terrain_key_address(env, 2)) == "foliage");
	// A key's place is where a Go to opens it, the file open or not (the terrain's Inspector's line from it).
	TEST_EXPECT(env.locator(terrain_key_address(env, 4)) == terrain_key_locator(4) &&
	            env.address_at(terrain_key_locator(4)) == terrain_key_address(env, 4));
	// Written again: the environment's keywords in the editor's layout, then the terrain keys in their order, each line
	// from scratch; read back, the same keys; written again, the same bytes.
	const SerializeResult saved = env.serialize();
	TEST_EXPECT(saved.ok());
	const std::string tail = "polytrn_colormap \"red map.tga\"\r\nlock_topleft 3 4\r\nfoliage\r\ngraphic tree.3di\r\n"
	                         "color_lower 2\r\nend\r\npolytrn_detaildensity 64\r\n";
	const size_t first = saved.text.find(tail);
	TEST_EXPECT(first != std::string::npos && first + tail.size() == saved.text.size() &&
	            saved.text.find("fog_level 900\r\n") < first && saved.text.find("terrain_name") == std::string::npos);
	{
		auto again = load(saved.text);
		TEST_EXPECT(again && again->issues().empty() && again->environment_row()->terrain_keys == expected &&
		            config_difference(*again->config(), *env.config()).empty() && again->serialize().text == saved.text);
	}
	// The game's terrain reads the saved file as it read the original: the same configuration over the fixture.
	TEST_EXPECT(terrain_under(repo, saved.text) == terrain_under(repo, text) &&
	            terrain_under(repo, text) != terrain_under(repo, "sky_height 175\r\n"));
	// The one finding the skipped terrain_name's: every terrain key reads as its line says.
	{
		const std::vector<Diagnostic> findings = validate_environment_file(env);
		TEST_EXPECT(findings.size() == 1 && findings[0].code() == "environment.ignored_input" && findings[0].line == 11);
	}
	// Each key's value is the terrain's field of its keyword: the colour map a texture by its role's loader; a name
	// reads its first value alone, a lock two.
	{
		const FieldSchema *value = field_of(env, kTerrainKey, "value"), *more = field_of(env, kTerrainKey, "more");
		TEST_EXPECT(value && more);
		if (!value || !more) return 1;
		const FieldUse map = env.field_on(terrain_key_address(env, 0), *value);
		TEST_EXPECT(map.reference == ReferenceKind::Texture && map.loader_arg >= 0 && map.label &&
		            std::string(map.label) == "Colour map");
		TEST_EXPECT(env.field_on(terrain_key_address(env, 0), *more).applies == Applicability::Ignored &&
		            env.field_on(terrain_key_address(env, 1), *more).applies == Applicability::Reads &&
		            env.field_on(terrain_key_address(env, 2), *value).applies == Applicability::Ignored);
		TEST_EXPECT(env.field_on(terrain_key_address(env, 3), *value).reference == ReferenceKind::Model);
	}
	// Edited: a value, the values after it, the keyword; what no line carries, refused.
	std::string why;
	TEST_EXPECT(set(env, terrain_key_address(env, 0), "value", std::string("blue_c.tga")) &&
	            set(env, terrain_key_address(env, 1), "more", std::string("7")) &&
	            set(env, terrain_key_address(env, 6), "key", std::string("polytrn_detaildensity2")));
	TEST_EXPECT(!set(env, terrain_key_address(env, 6), "key", std::string("fog_level"), &why) &&
	            why.find("terrain's reader") != std::string::npos);
	TEST_EXPECT(!set(env, terrain_key_address(env, 0), "value", std::string("a\"b"), &why) && why.find("'\"'") != std::string::npos);
	TEST_EXPECT(!set(env, terrain_key_address(env, 1), "more", std::string("5 ; six"), &why) && why.find("comment") != std::string::npos);
	TEST_EXPECT(!set(env, terrain_key_address(env, 1), "value", std::string(""), &why));
	TEST_EXPECT(set(env, terrain_key_address(env, 1), "more", std::string("\"7 8\"")) &&
	            read(env, terrain_key_address(env, 1), "more") == Value(std::string("\"7 8\"")));
	{
		const std::string edited = env.serialize().text;
		TEST_EXPECT(edited.find("polytrn_colormap blue_c.tga\r\nlock_topleft 3 \"7 8\"\r\n") != std::string::npos &&
		            edited.find("polytrn_detaildensity2 64\r\n") != std::string::npos);
		auto back = load(edited);
		TEST_EXPECT(back && back->environment_row()->terrain_keys == env.environment_row()->terrain_keys);
	}
	// A key added (the load's own detail density, 128), then removed; undone back to the file as read.
	{
		Edit add;
		add.operation = EditOperation::Add;
		add.address = {row_address(env).row, kTerrainKey, 0};
		Diagnostic error;
		TEST_EXPECT(env.apply(add, error) && env.environment_row()->terrain_keys.size() == 8 &&
		            env.environment_row()->terrain_keys.back() == TrnKeyLine({"polytrn_detaildensity", {"128"}}));
		Edit remove;
		remove.operation = EditOperation::Remove;
		remove.address = terrain_key_address(env, 0);
		TEST_EXPECT(env.apply(remove, error) && env.environment_row()->terrain_keys.size() == 7);
	}
	while (env.can_undo()) env.undo();
	TEST_EXPECT(!env.dirty() && env.environment_row()->terrain_keys == expected && env.serialize().text == saved.text);
	// What the terrain's parser reads otherwise than a line says: a grid row adds a row after the terrain's; a block's
	// key with no block open; a line inside a block the file opens; past a fifth block, nothing. Each a listed finding
	// of its record, the line kept.
	{
		std::string blocks;
		for (int i = 0; i < 5; ++i) blocks += "foliage\r\ngraphic g" + std::to_string(i) + ".3di\r\nend\r\n";
		auto odd = load("sky_height 175\r\npolytrn_sectors 1 1 1 1\r\ngraphic lone.3di\r\nfoliage\r\npolytrn_wrapx 1\r\n"
		                "end\r\n" + blocks);
		TEST_EXPECT(odd && !odd->blocked() && odd->environment_row()->terrain_keys.size() == 20);
		if (!odd) return 1;
		const std::vector<Diagnostic> findings = validate_environment_file(*odd);
		std::vector<NodeId> at;
		for (const Diagnostic &d : findings) {
			TEST_EXPECT(d.code() == "environment.terrain_key" && d.severity == DiagnosticSeverity::Warning &&
			            d.record_kind == kTerrainKey && d.message.find("[orig: ") != std::string::npos);
			at.push_back(d.child_id);
		}
		// The grid row (0), the lone graphic (1), the wrap inside the block (3), and from the file's fifth block on
		// (14: the blocks at 2..4, 5..7, 8..10 and 11..13 close four) every line.
		std::vector<NodeId> expected_at;
		for (size_t i : {0, 1, 3, 14, 15, 16, 17, 18, 19}) expected_at.push_back(odd->terrain_key_address(i).child);
		TEST_EXPECT(at == expected_at);
		TEST_EXPECT(findings.size() == 9 && findings[0].message.find("row after the terrain's") != std::string::npos &&
		            findings[1].message.find("only inside a block") != std::string::npos &&
		            findings[2].message.find("inside the foliage block") != std::string::npos &&
		            findings[3].message.find("A fifth foliage block") != std::string::npos &&
		            findings[4].message.find("inside a fifth foliage block") != std::string::npos);
		TEST_EXPECT(odd->serialize().ok());
	}
	// A line the editor cannot write back as the game reads it (its 30th token runs to the line's end, a separator
	// in it) blocks: the row cannot keep it.
	{
		std::string line = "polytrn_sectors";
		for (int i = 0; i < 28; ++i) line += " 1";
		auto cut = load("sky_height 175\r\n" + line + " x y\r\n");
		TEST_EXPECT(cut && cut->blocked() && has_issue(*cut, 2, true, "cannot be written again"));
	}
	std::printf("terrain keys: kept in order through the read, an edit and the write, the game's terrain the same; each "
	            "the terrain's field of its keyword; those read otherwise a finding\n");
	return 0;
}

const opennova::io::JsonValue *member(const opennova::io::JsonValue &object, const char *name) {
	return object.is_object() ? object.get(name) : nullptr;
}

std::string text_of(const opennova::io::JsonValue *value) { return value && value->is_string() ? value->string : std::string(); }

// In a session: the graph reads the environment's references through the document; the missions that run
// on it, with their terrain, overrides and water plane; an edit the graph follows, undone, saved.
int test_session() {
	editor_test::TempProjectDir dir("opennova_environment_document");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Environment"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	Config config;
	config.sky_map1 = "near.pcx";
	config.sky_map2 = "far.pcx";
	config.sun_3di = "sun.3di";
	config.sky_height = 175.0f;
	config.water_height = 4.0f;
	config.water_height_set = true;
	std::ostringstream env_text;
	std::string error;
	TEST_EXPECT(opennova::env::save_env(env_text, config, error));
	TEST_EXPECT(editor_test::write_text(root + "/day.env", env_text.str()));
	TEST_EXPECT(editor_test::write_text(root + "/island.trn",
	                                    "polytrn_colormap map.tga\r\npolytrn_detailmap detail.tga\r\npolytrn_polydata island.cpt\r\n"
	                                    "polytrn_sectorcount 1\r\npolytrn_sectors 0\r\nwater_height 25\r\n"));
	TEST_EXPECT(editor_test::write_text(root + "/near.pcx", "x"));
	// Two missions on it: one fogging closer, one setting its own water height.
	const auto mission = [&](const char *name, int attrib, int fog, int water, int start) {
		opennova::bms::File file;
		opennova::mission::make_default(file);
		std::string why;
		TEST_EXPECT(opennova::mission::set_header_string(file, "terrain", "island", why) &&
		            opennova::mission::set_header_string(file, "environment", "day", why) &&
		            opennova::mission::set_header_int(file, "attrib_flags", attrib, why) &&
		            opennova::mission::set_header_int(file, "fog_override", fog, why) &&
		            opennova::mission::set_header_int(file, "water_override", water, why) &&
		            opennova::mission::set_header_int(file, "start_time", start, why) &&
		            opennova::mission::set_header_int(file, "minutes_per_day", 30, why));
		std::vector<uint8_t> bytes;
		TEST_EXPECT(opennova::bms::write(file, bytes, why));
		TEST_EXPECT(editor_test::write_bytes(root + "/" + name, bytes));
		return 0;
	};
	TEST_EXPECT(mission("fogged.bms", 0x2, 800, 0, 6 << 8) == 0);
	TEST_EXPECT(mission("flooded.bms", 0x1, 0, 30, 18 << 8) == 0);
	editor_test::handle_to_end(session, request::rescan());
	const AssetGraph &graph = *view.findings.graph;
	// The references are the document's fields: rewritable, on the environment row.
	{
		const std::vector<const GraphEdge *> refs = graph.references_of("day.env");
		const auto near = std::find_if(refs.begin(), refs.end(), [](const GraphEdge *e) { return e->value == "near.pcx"; });
		TEST_EXPECT(near != refs.end() && (*near)->field == "sky_map1" && (*near)->rewritable &&
		            (*near)->record == "Environment" && graph.resolve(**near) == ReferenceStatus::Present);
		TEST_EXPECT(std::any_of(refs.begin(), refs.end(), [](const GraphEdge *e) {
			return e->kind == ReferenceKind::Model && e->value == "sun.3di" && e->field == "sun_3di";
		}));
	}
	// The missions that run on it.
	{
		const EnvironmentUses uses = environment_uses(view, "day.env");
		TEST_EXPECT(uses.found && uses.missions.size() == 2);
		for (const EnvironmentMissionUse &use : uses.missions) {
			TEST_EXPECT(use.read && use.terrain_file == "island.trn" && use.terrain_read && use.terrain_water == 25);
			TEST_EXPECT(use.edge && use.edge->field == "environment");
			const std::vector<EnvironmentOverride> overrides = environment_overrides(use);
			if (use.mission == "fogged.bms") {
				TEST_EXPECT(overrides.size() == 1 && std::string(overrides[0].field) == "fog_override" &&
				            overrides[0].words == "fog distance 800 m");
				// The environment's water height (4 half metres) writes after the terrain's (25), so it is the
				// mission's (env #44).
				TEST_EXPECT(use.water_from == opennova::env::WaterRung::Environment && use.water_height == 2.0f);
				TEST_EXPECT(mission_clock_words(use) == "starts at 06:00, a day of 60 min");
			} else {
				TEST_EXPECT(overrides.size() == 1 && std::string(overrides[0].field) == "water_override" &&
				            overrides[0].words == "water height 15 m");
				TEST_EXPECT(use.water_from == opennova::env::WaterRung::Mission && use.water_height == 15.0f);
			}
		}
		opennova::io::JsonValue args = opennova::io::JsonValue::make_object();
		args.set("path", opennova::io::json_string("day.env"));
		std::string why;
		const opennova::io::JsonValue answer = session.query("environment_uses", args, why);
		const opennova::io::JsonValue *missions = member(answer, "missions");
		TEST_EXPECT(why.empty() && missions && missions->is_array() && missions->array.size() == 2);
		if (missions && missions->array.size() == 2) {
			const opennova::io::JsonValue &first = missions->array[0];
			TEST_EXPECT(!text_of(member(first, "locator")).empty() && text_of(member(first, "field")) == "environment");
			const opennova::io::JsonValue *water = member(first, "water");
			TEST_EXPECT(water && !text_of(member(*water, "from")).empty());
		}
		args.set("path", opennova::io::json_string("island.trn"));
		session.query("environment_uses", args, why);
		TEST_EXPECT(why.find("not an environment") != std::string::npos);
	}
	// Opened, edited: the graph follows the unsaved edit; undone; saved through the writer.
	editor_test::handle_to_end(session, request::open_document("day.env"));
	Document *open = session.document_for("day.env");
	TEST_EXPECT(open && dynamic_cast<EnvironmentDocument *>(open) != nullptr);
	if (!open) return 1;
	const NodeAddress row{open->rows().front()->id, kEnvironment, 0};
	Edit rename;
	rename.address = row;
	rename.field = "sky_map1";
	rename.value = std::string("haze.pcx");
	editor_test::handle_to_end(session, request::edit_record("day.env", rename));
	const auto names = [&](const char *value) {
		const std::vector<const GraphEdge *> refs = view.findings.graph->references_of("day.env");
		return std::any_of(refs.begin(), refs.end(), [&](const GraphEdge *e) { return e->value == value; });
	};
	TEST_EXPECT(names("haze.pcx") && !names("near.pcx"));
	editor_test::handle_to_end(session, request::undo("day.env"));
	TEST_EXPECT(names("near.pcx") && !names("haze.pcx"));
	Edit fog;
	fog.address = row;
	fog.field = "fog_level";
	fog.value = int64_t(640);
	editor_test::handle_to_end(session, request::edit_record("day.env", fog));
	editor_test::handle_to_end(session, request::save("day.env"));
	const std::vector<uint8_t> saved = test_io::read_file(root + "/day.env");
	const std::string saved_text(saved.begin(), saved.end());
	TEST_EXPECT(saved_text.find("fog_level 640\r\n") != std::string::npos && parsed(saved_text).fog_level == 640.0f);
	// A terrain key in an environment is its missions' terrain's, after the .trn's (D-TERRAIN-18): its uses list the
	// line each mission's terrain takes.
	{
		TEST_EXPECT(editor_test::write_text(root + "/dusk.env", "sky_height 175\r\npolytrn_detaildensity 64\r\n"));
		opennova::bms::File file;
		opennova::mission::make_default(file);
		std::string why;
		TEST_EXPECT(opennova::mission::set_header_string(file, "terrain", "island", why) &&
		            opennova::mission::set_header_string(file, "environment", "dusk", why));
		std::vector<uint8_t> bytes;
		TEST_EXPECT(opennova::bms::write(file, bytes, why));
		TEST_EXPECT(editor_test::write_bytes(root + "/late.bms", bytes));
		editor_test::handle_to_end(session, request::rescan());
		const EnvironmentUses uses = environment_uses(view, "dusk.env");
		TEST_EXPECT(uses.missions.size() == 1 && uses.missions[0].terrain_keys.size() == 1);
		if (uses.missions.size() == 1 && uses.missions[0].terrain_keys.size() == 1) {
			const EnvironmentTerrainKey &line = uses.missions[0].terrain_keys[0];
			TEST_EXPECT(line.key == "polytrn_detaildensity" && line.value == "64" && line.line == 2);
			// The environment's first terrain key, over the load's own default (island.trn writes none).
			TEST_EXPECT(line.index == 0 && line.over == "128" && line.over_file.empty());
		}
		TEST_EXPECT(environment_uses(view, "day.env").missions.size() == 2 &&
		            environment_uses(view, "day.env").missions[0].terrain_keys.empty());
		// The file is no Problem: the build ships it with its terrain key, and the game reads it from there.
		const auto built_text = [&](const char *name) {
			const SessionView &v = session.view();
			if (!v.activity.has_build || !v.activity.last_build->ok) return std::string("(no build)");
			opennova::Vfs vfs;
			std::vector<uint8_t> bytes;
			if (!vfs.mount_game(v.activity.last_build->build_dir, std::string(), opennova::VfsMountMode::Packed,
			                    opennova::VfsArchiveDiscovery::RetailTable) ||
			    !vfs.read_file_raw(name, bytes))
				return std::string("(not in the build)");
			return std::string(bytes.begin(), bytes.end());
		};
		for (const Diagnostic &d : view.findings.diagnostics)
			TEST_EXPECT(!(d.asset == "dusk.env" && d.code().rfind("environment.", 0) == 0));
		// (The terrain's colour map, whose absence aborts the mission, refuses a build: the project gets one.)
		{
			const std::vector<uint8_t> rgba(size_t(1024) * 1024 * 4, 200); // the game reads 1024 x 1024 texels
			std::vector<uint8_t> tga;
			TEST_EXPECT(opennova::tga::tga_write_rgba32(rgba.data(), 1024, 1024, tga, why) &&
			            editor_test::write_bytes(root + "/map.tga", tga));
		}
		editor_test::handle_to_end(session, request::rescan());
		editor_test::handle_to_end(session, request::build());
		for (const Diagnostic &d : view.activity.last_build ? view.activity.last_build->diagnostics : std::vector<Diagnostic>())
			if (d.severity == DiagnosticSeverity::Error) std::fprintf(stderr, "  build: %s %s\n", d.code().c_str(), d.message.c_str());
		TEST_EXPECT(built_text("dusk.env").find("polytrn_detaildensity 64\r\n") != std::string::npos);
		// Opened, the key edited in its record, saved, built: the build ships the edit.
		editor_test::handle_to_end(session, request::open_document("dusk.env"));
		auto *dusk = dynamic_cast<EnvironmentDocument *>(session.document_for("dusk.env"));
		TEST_EXPECT(dusk && !dusk->blocked() && dusk->environment_row()->terrain_keys.size() == 1);
		if (!dusk) return 1;
		Edit density;
		density.address = dusk->terrain_key_address(0);
		density.field = "value";
		density.value = std::string("96");
		editor_test::handle_to_end(session, request::edit_record("dusk.env", density));
		{
			const EnvironmentUses edited = environment_uses(view, "dusk.env");
			TEST_EXPECT(edited.missions.size() == 1 && edited.missions[0].terrain_keys.size() == 1 &&
			            edited.missions[0].terrain_keys[0].value == "96" && edited.missions[0].terrain_keys[0].index == 0);
		}
		editor_test::handle_to_end(session, request::save("dusk.env"));
		editor_test::handle_to_end(session, request::build());
		const std::string shipped = built_text("dusk.env");
		TEST_EXPECT(shipped.find("polytrn_detaildensity 96\r\n") != std::string::npos &&
		            shipped.find("sky_height 175\r\n") != std::string::npos);
		// overcast.def is a text the editor keeps whole (the text type): a terrain key there is kept, no Problem, and
		// shipped; the environment's key now sets over overcast.def's.
		const AssetEntry *overcast_entry = view.project.scan->find("overcast.def");
		const std::string overcast = overcast_entry ? overcast_entry->relative_path : std::string("overcast.def");
		TEST_EXPECT(editor_test::write_text(root + "/" + overcast, "tod_begin 1200\r\nsun_rgb 1,2,3\r\ntod_end\r\n"
		                                                           "polytrn_detaildensity 32\r\n"));
		editor_test::handle_to_end(session, request::rescan());
		{
			const EnvironmentUses over = environment_uses(view, "dusk.env");
			TEST_EXPECT(over.missions.size() == 1 && over.missions[0].terrain_keys.size() == 1 &&
			            over.missions[0].terrain_keys[0].over == "32" && over.missions[0].terrain_keys[0].over_file == overcast);
		}
		editor_test::handle_to_end(session, request::open_document(overcast));
		const DocumentBase *text = nullptr;
		for (const auto &open : view.documents.open)
			if (open && open->path() == overcast) text = open.get();
		TEST_EXPECT(text && !text->blocked() && text->issues().empty());
		for (const Diagnostic &d : view.findings.diagnostics) TEST_EXPECT(!(d.asset == overcast && d.severity == DiagnosticSeverity::Error));
		editor_test::handle_to_end(session, request::build());
		TEST_EXPECT(built_text("overcast.def").find("polytrn_detaildensity 32\r\n") != std::string::npos);
	}
	std::printf("session: the references through the document, the missions with their terrain, overrides and water, an "
	            "edit followed and undone, a save, a terrain key's missions, built and edited\n");
	return 0;
}

// Every .env of the install through the document and written again: the same environment, field for field.
int test_retail() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (every retail .env through the environment document)");
		return 0;
	}
	opennova::Vfs vfs;
	TEST_EXPECT(vfs.mount_game(install.c_str(), std::string(), opennova::VfsMountMode::Packed));
	size_t files = 0, issues = 0, blocked = 0, rates = 0;
	for (const auto &location : vfs.list_files()) {
		const std::string &name = location.logical_name;
		if (name.size() < 4) continue;
		std::string extension = name.substr(name.size() - 4);
		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
		if (extension != ".env") continue;
		std::vector<uint8_t> bytes;
		if (!vfs.read_file_raw(name, bytes) || bytes.empty()) continue;
		++files;
		auto document = std::make_unique<EnvironmentDocument>();
		Diagnostic error;
		TEST_EXPECT(document->load_bytes(bytes, name, AssetKind::Environment, "jo", error));
		issues += document->issues().size();
		for (const SourceIssue &issue : document->issues())
			std::printf("  %s line %zu: %s\n", name.c_str(), issue.line, issue.message.c_str());
		if (document->blocked()) {
			++blocked;
			continue;
		}
		const Config *held = document->config();
		TEST_EXPECT(held != nullptr);
		if (!held) continue;
		rates += held->tod_rate_set;
		const SerializeResult written = document->serialize();
		TEST_EXPECT(written.ok());
		const std::string difference = config_difference(*held, parsed(written.text));
		if (!difference.empty()) std::fprintf(stderr, "FAIL: %s: %s reads back otherwise\n", name.c_str(), difference.c_str());
		TEST_EXPECT(difference.empty());
		// Written again from what it wrote, it writes the same bytes.
		auto again = std::make_unique<EnvironmentDocument>();
		TEST_EXPECT(again->load_bytes(bytes_of(written.text), name, AssetKind::Environment, "jo", error) &&
		            again->serialize().text == written.text && again->issues().empty());
	}
	std::printf("retail: %zu environments read and written again field for field (%zu source issues, %zu blocked, %zu "
	            "with a tod_rate)\n",
	            files, issues, blocked, rates);
	TEST_EXPECT(files >= 10 && blocked == 0);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	const std::string repo = test_paths_repo_root(__FILE__);
	int failures = test_type();
	failures += test_document(repo);
	failures += test_source_issues();
	failures += test_terrain_keys(repo);
	failures += test_session();
	failures += test_retail();
	if (failures == 0) std::printf("editor_environment_document: all passed\n");
	return failures == 0 ? 0 : 1;
}
