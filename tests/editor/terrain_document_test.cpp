// The terrain document (the deep-integration plan's DI-30; editor/documents/terrain_document.h): a .trn
// through the engine's own reader and writer, its fields the game's keys in the units the file writes them,
// its grid rows (at most 16) and foliage definitions (at most four) lists, what the readers read otherwise
// than written as the file's source findings, the gate's refusal and the inert definitions as findings with
// fixes; its references the graph's edges through the document; the missions that run on it and the import
// that makes it (session/terrain_uses, the terrain_uses query); an authored terrain edited, followed, undone
// and saved; an import's terrain opened and every edit of it refused (document.imported), its Reimport. The
// retail leg (OPENNOVA_JO_DIR): every .trn of the install read through the document and written again reads
// back as the same terrain, field for field, and a second save writes the same bytes.
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
#include <editor/documents/line_ends.h>
#include <editor/documents/terrain_document.h>
#include <editor/documents/texture_roles.h>
#include <editor/graph/asset_graph.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/terrain_uses.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/png/png_encode.h>
#include <formats/trn/trn_io.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
namespace renderer = opennova::renderer;
using opennova::FoliageDef;
using opennova::TrnConfig;
using opennova::png::encode_png_rgba;

namespace {

constexpr NodeKind kTerrain = node_kind(TerrainKind::Terrain);
constexpr NodeKind kSectorRow = node_kind(TerrainKind::SectorRow);
constexpr NodeKind kFoliage = node_kind(TerrainKind::Foliage);

std::vector<uint8_t> bytes_of(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

// A terrain written as the shipped ones are (Dvxi5.trn's layout, its own names).
const char *const kShipped =
		"terrain_name     \"Isle\"\r\n"
		"terrain_creator  \"Someone\"\r\n"
		"\r\n"
		";terrain values\r\n"
		"water_height     21         ;default, if zero will take from mission\r\n"
		"polytrn_colormap         isle_c.tga\r\n"
		"polytrn_detailmap        det_d1.tga\r\n"
		"polytrn_detailmap_c1     det_dg1.tga\r\n"
		"polytrn_detailmap_c2     det_ds2.tga\r\n"
		"polytrn_detailmap_c3     det_mr2.tga\r\n"
		"polytrn_detailmap2       det_d1d.tga\r\n"
		"polytrn_detailmapdist    detgrsd2.tga\r\n"
		"polytrn_polydata         isle.cpt\r\n"
		"polytrn_tilestrip        tiles.tga\r\n"
		"polytrn_charmap          isle_m.pcx\r\n"
		"polytrn_foliagemap       isle_f.pcx\r\n"
		"polytrn_detailblendmap   isle_d1.tga\r\n"
		"water_rgb \t\t 108,81,48\r\n"
		"water_murk  \t\t .3\r\n"
		"polytrn_detaildensity           128\r\n"
		"polytrn_detaildensity2          8\r\n"
		"polytrn_sectorcount\t\t4                   ; width of sectors\r\n"
		"polytrn_wrapx\t\t\t0\r\n"
		"polytrn_wrapy\t\t\t0\r\n"
		"polytrn_origin\t\t\t-2\t-2\r\n"
		"polytrn_sectors\t\t\t0\t0\t0\t0\r\n"
		"polytrn_sectors\t\t\t0\t1\t3\t0\r\n"
		"polytrn_sectors\t\t\t0\t2\t4\t0\r\n"
		"polytrn_sectors\t\t\t0\t0\t0\t0\r\n"
		"lock_topleft      0 0\r\n"
		"lock_topright     1 0\r\n"
		"lock_bottomleft   0 1\r\n"
		"lock_bottomright  0 0\r\n"
		"foliage\r\n"
		"  graphic         mveg5b.3di ; <=20 verts\r\n"
		"  color_lower     0\r\n"
		"  color_upper     2\r\n"
		"  match           254\r\n"
		"  attrib\t  shadow\r\n"
		"end\r\n"
		"foliage\r\n"
		"  graphic         mveg5.3di\r\n"
		"  color_lower     0\r\n"
		"  color_upper     2\r\n"
		"  match           253 252\r\n"
		"  attrib\t  shadow forceon\r\n"
		"end\r\n";

std::unique_ptr<TerrainDocument> load(const std::string &text, const std::string &name = "isle.trn") {
	auto document = std::make_unique<TerrainDocument>();
	Diagnostic error;
	if (!document->load_bytes(bytes_of(text), name, AssetKind::Terrain, "jo", error)) {
		std::fprintf(stderr, "load %s: %s\n", name.c_str(), error.message.c_str());
		return nullptr;
	}
	return document;
}

NodeAddress row_address(const TerrainDocument &document) {
	const TerrainRow *row = document.terrain_row();
	return row ? NodeAddress{row->id, kTerrain, 0} : NodeAddress{};
}

NodeAddress child(const TerrainDocument &document, NodeKind kind, size_t index) {
	for (const Document::Collection &list : document.collections_of(row_address(document)))
		if (list.spec.kind == kind && index < list.ids.size()) return {row_address(document).row, kind, list.ids[index]};
	return {};
}

size_t count_of(const TerrainDocument &document, NodeKind kind) {
	for (const Document::Collection &list : document.collections_of(row_address(document)))
		if (list.spec.kind == kind) return list.ids.size();
	return 0;
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

bool add(Document &document, const NodeAddress &row, NodeKind kind, std::string *why = nullptr) {
	Edit edit;
	edit.operation = EditOperation::Add;
	edit.address = {row.row, kind, 0};
	edit.position = SIZE_MAX;
	Diagnostic error;
	const bool applied = document.apply(edit, error);
	if (why) *why = error.message;
	return applied;
}

TrnConfig parsed(const std::string &text) {
	std::istringstream input(text);
	TrnConfig config;
	std::string error;
	opennova::load_trn(input, config, error);
	return config;
}

bool same_def(const FoliageDef &a, const FoliageDef &b) {
	return a.graphic == b.graphic && a.color_lower == b.color_lower && a.color_upper == b.color_upper &&
	       a.match == b.match && a.attrib_flags == b.attrib_flags;
}

// Every field the reader fills, compared as the game holds it; "" when alike, else the first that differs.
std::string config_difference(const TrnConfig &a, const TrnConfig &b) {
	if (a.name != b.name) return "terrain_name";
	if (a.creator != b.creator) return "terrain_creator";
	if (a.colormap != b.colormap || a.detailmap != b.detailmap || a.detailmap_c1 != b.detailmap_c1 ||
	    a.detailmap_c2 != b.detailmap_c2 || a.detailmap_c3 != b.detailmap_c3 || a.detailblendmap != b.detailblendmap ||
	    a.detailmap2 != b.detailmap2 || a.detailmapdist != b.detailmapdist || a.detailmapdist2 != b.detailmapdist2)
		return "maps";
	if (a.polydata != b.polydata || a.charmap != b.charmap || a.foliagemap != b.foliagemap || a.tilestrip != b.tilestrip ||
	    a.tileinfo != b.tileinfo)
		return "names";
	if (a.detail_density != b.detail_density || a.detail_density2 != b.detail_density2) return "densities";
	if (a.sector_count != b.sector_count || a.sector_rows != b.sector_rows) return "grid size";
	if (a.origin_x != b.origin_x || a.origin_y != b.origin_y) return "origin";
	if (a.wrap_x != b.wrap_x || a.wrap_y != b.wrap_y) return "wraps";
	const auto lock = [](const opennova::TerrainLockCoord &l) { return l.x * 1000 + l.y; };
	if (lock(a.lock_topleft) != lock(b.lock_topleft) || lock(a.lock_topright) != lock(b.lock_topright) ||
	    lock(a.lock_bottomleft) != lock(b.lock_bottomleft) || lock(a.lock_bottomright) != lock(b.lock_bottomright))
		return "locks";
	if (a.water_height != b.water_height) return "water_height";
	if (a.water_rgb_set != b.water_rgb_set || (a.water_rgb_set && a.water_rgb != b.water_rgb)) return "water_rgb";
	if (a.water_murk_set != b.water_murk_set || (a.water_murk_set && a.water_murk != b.water_murk)) return "water_murk";
	// The grid as the file writes it (the rows, each the width's cells): the rest the game's extension derives.
	for (int r = 0; r < std::max(1, a.sector_rows); ++r)
		for (int c = 0; c < std::min(a.sector_count, opennova::kTerrainGridSide); ++c)
			if (a.sector_grid[r][c] != b.sector_grid[r][c]) return "sector " + std::to_string(r) + "," + std::to_string(c);
	if (a.foliage_defs.size() != b.foliage_defs.size()) return "foliage count";
	for (size_t i = 0; i < a.foliage_defs.size(); ++i)
		if (!same_def(a.foliage_defs[i], b.foliage_defs[i])) return "foliage " + std::to_string(i + 1);
	return std::string();
}

bool has_issue(const Document &document, size_t line, bool blocks, const std::string &words) {
	for (const SourceIssue &issue : document.issues())
		if (issue.line == line && issue.blocks == blocks && issue.message.find(words) != std::string::npos) return true;
	std::fprintf(stderr, "no issue on line %zu (%s) saying '%s'; the issues:\n", line, blocks ? "blocks" : "ignored",
	             words.c_str());
	for (const SourceIssue &issue : document.issues())
		std::fprintf(stderr, "  %zu %s: %s\n", issue.line, issue.blocks ? "blocks" : "ignored", issue.message.c_str());
	return false;
}

// The type's row, the kind's, the table, the codes.
int test_type() {
	const DocumentType *type = document_type_for(AssetKind::Terrain);
	TEST_EXPECT(type && type->id == DocumentTypeId::Terrain && std::string(type->name) == "terrain");
	TEST_EXPECT(document_content(*type) == DocumentContent::Records);
	TEST_EXPECT(terrain_table().well_formed());
	const std::vector<FieldSchema> &fields = TerrainDocument::schema(kTerrain);
	const auto field = [&](const char *id) -> const FieldSchema * {
		for (const FieldSchema &f : fields)
			if (f.id == id) return &f;
		return nullptr;
	};
	TEST_EXPECT(field("polytrn_colormap") && field("polytrn_colormap")->reference == ReferenceKind::Texture);
	TEST_EXPECT(field("polytrn_polydata") && field("polytrn_polydata")->reference == ReferenceKind::TerrainData);
	TEST_EXPECT(field("polytrn_tileinfo") && field("polytrn_tileinfo")->reference == ReferenceKind::TilePlacement);
	TEST_EXPECT(field("terrain_name") && field("terrain_name")->applies == Applicability::Ignored);
	TEST_EXPECT(field("water_height") && field("water_height")->unit == "half m" && field("water_height")->max == 65535.0);
	TEST_EXPECT(field("water_murk") && field("water_murk")->optional && field("water_murk")->max == 0.99);
	TEST_EXPECT(field("polytrn_sectorcount") && field("polytrn_sectorcount")->unit == "sectors");
	TEST_EXPECT(TerrainDocument::schema(kSectorRow).size() == 16);
	TEST_EXPECT(TerrainDocument::schema(kFoliage).size() == 8); // graphic, four codes, two colours, attrib
	const FindingTable codes = terrain_finding_codes();
	TEST_EXPECT(codes.count == 5 && std::string(codes.rows[2].token) == "terrain.refused" && codes.rows[2].gates_build &&
	            !codes.rows[3].gates_build && codes.rows[4].fixes == FindingFix::EditRecord);
	std::printf("type: the terrain's row, its table and its codes\n");
	return 0;
}

// The fields in the game's units, the lists, the rules a value keeps, the writer.
int test_document() {
	auto document = load(kShipped);
	TEST_EXPECT(document && !document->blocked() && document->issues().empty());
	if (!document) return 1;
	for (const SourceIssue &issue : document->issues()) std::fprintf(stderr, "  issue %zu: %s\n", issue.line, issue.message.c_str());
	const NodeAddress row = row_address(*document);
	TEST_EXPECT(read(*document, row, "terrain_name") == Value(std::string("Isle")) &&
	            read(*document, row, "terrain_creator") == Value(std::string("Someone")));
	TEST_EXPECT(read(*document, row, "polytrn_colormap") == Value(std::string("isle_c.tga")) &&
	            read(*document, row, "polytrn_polydata") == Value(std::string("isle.cpt")));
	TEST_EXPECT(read(*document, row, "water_height") == Value(int64_t(21)) &&
	            read(*document, row, "water_rgb_g") == Value(int64_t(81)) && document->present(row, "water_rgb_g"));
	TEST_EXPECT(read(*document, row, "polytrn_sectorcount") == Value(int64_t(4)) &&
	            read(*document, row, "origin_x") == Value(int64_t(-2)) && read(*document, row, "origin_y") == Value(int64_t(-2)));
	TEST_EXPECT(read(*document, row, "lock_topright_x") == Value(int64_t(1)) &&
	            read(*document, row, "lock_bottomleft_y") == Value(int64_t(1)));
	{
		const Value murk = read(*document, row, "water_murk");
		TEST_EXPECT(std::holds_alternative<double>(murk) && float(std::get<double>(murk)) == 0.3f);
	}
	// The grid's rows and the foliage definitions.
	TEST_EXPECT(count_of(*document, kSectorRow) == 4 && count_of(*document, kFoliage) == 2);
	const NodeAddress second_row = child(*document, kSectorRow, 1);
	TEST_EXPECT(read(*document, second_row, "sector_2") == Value(int64_t(1)) &&
	            read(*document, second_row, "sector_3") == Value(int64_t(3)));
	TEST_EXPECT(document->record_title(second_row) == "Row 2");
	// A cell past the width is read by no row's arm.
	TEST_EXPECT(document->field_on(second_row, TerrainDocument::schema(kSectorRow)[3]).applies == Applicability::Reads &&
	            document->field_on(second_row, TerrainDocument::schema(kSectorRow)[4]).applies == Applicability::Ignored);
	const NodeAddress first_foliage = child(*document, kFoliage, 0), second_foliage = child(*document, kFoliage, 1);
	TEST_EXPECT(document->record_title(first_foliage) == "Foliage 1: mveg5b.3di");
	TEST_EXPECT(read(*document, first_foliage, "match_1") == Value(int64_t(254)) && read(*document, first_foliage, "match_2") == Value(int64_t(0)));
	TEST_EXPECT(read(*document, second_foliage, "match_2") == Value(int64_t(252)) &&
	            read(*document, second_foliage, "attrib") == Value(int64_t(3)));
	// Each map by its role's loader, the colour map's and the blend map's gating.
	{
		const auto loader = [&](const char *id) {
			for (const FieldSchema &f : TerrainDocument::schema(kTerrain))
				if (f.id == id) return document->field_on(row, f).loader_arg;
			return -2;
		};
		TEST_EXPECT(loader("polytrn_colormap") == texture_role_arg(renderer::TextureRoleId::TerrainColourMap, kTextureArgGates));
		TEST_EXPECT(loader("polytrn_detailblendmap") == texture_role_arg(renderer::TextureRoleId::TerrainBlendMap, kTextureArgGates));
		TEST_EXPECT(loader("polytrn_charmap") == texture_role_arg(renderer::TextureRoleId::TerrainCharMap));
		TEST_EXPECT(loader("polytrn_tilestrip") == texture_role_arg(renderer::TextureRoleId::TerrainTileAtlas));
	}
	// The rules a value keeps.
	std::string why;
	TEST_EXPECT(!set(*document, row, "polytrn_sectorcount", int64_t(6), &why) && why.find("power of two") != std::string::npos);
	TEST_EXPECT(!set(*document, row, "polytrn_sectorcount", int64_t(32), &why));
	TEST_EXPECT(!set(*document, row, "water_murk", 1.0, &why) && why.find("0.99") != std::string::npos);
	TEST_EXPECT(!set(*document, row, "water_rgb_r", int64_t(300), &why));
	TEST_EXPECT(!set(*document, row, "water_height", int64_t(70000), &why) && why.find("half metres") != std::string::npos);
	TEST_EXPECT(!set(*document, row, "polytrn_colormap", std::string("my map.tga"), &why) && why.find("space") != std::string::npos);
	TEST_EXPECT(!set(*document, first_foliage, "match_1", int64_t(300), &why));
	TEST_EXPECT(!set(*document, first_foliage, "attrib", int64_t(4), &why) && why.find("forceon") != std::string::npos);
	TEST_EXPECT(set(*document, row, "polytrn_sectorcount", int64_t(8)) && set(*document, row, "water_murk", 0.5));
	// The codes in order, as the one match line writes them: a code past the next is refused; one set to 0 (none)
	// leaves the line, the codes after it moving up.
	TEST_EXPECT(!set(*document, first_foliage, "match_3", int64_t(251), &why) && why.find("give code 2 first") != std::string::npos);
	TEST_EXPECT(set(*document, first_foliage, "match_2", int64_t(251)) && set(*document, first_foliage, "match_3", int64_t(250)));
	TEST_EXPECT(set(*document, first_foliage, "match_2", int64_t(0)) &&
	            read(*document, first_foliage, "match_2") == Value(int64_t(250)) &&
	            read(*document, first_foliage, "match_3") == Value(int64_t(0)));
	TEST_EXPECT(set(*document, first_foliage, "match_2", int64_t(251)));
	// A new grid row is the last one again (what the game's extension reads past the rows); a fifth definition
	// is refused.
	TEST_EXPECT(set(*document, child(*document, kSectorRow, 3), "sector_1", int64_t(2)));
	TEST_EXPECT(add(*document, row, kSectorRow) && count_of(*document, kSectorRow) == 5 &&
	            read(*document, child(*document, kSectorRow, 4), "sector_1") == Value(int64_t(2)));
	TEST_EXPECT(add(*document, row, kFoliage) && add(*document, row, kFoliage) && !add(*document, row, kFoliage, &why) &&
	            why.find("four") != std::string::npos);
	// Written through the engine's writer, read back through its reader.
	const SerializeResult written = document->serialize();
	TEST_EXPECT(written.ok());
	const TrnConfig back = parsed(written.text);
	TEST_EXPECT(back.sector_count == 8 && back.sector_rows == 5 && back.sector_grid[4][0] == 2 && back.water_murk == 0.5f &&
	            back.creator == "Someone" && back.water_rgb_set && back.water_rgb[1] == 81);
	TEST_EXPECT(back.foliage_defs.size() == 4 && back.foliage_defs[0].match[1] == 251 &&
	            back.foliage_defs[3].color_upper == 2 && back.foliage_defs[3].attrib_flags == opennova::FOLIAGE_ATTRIB_SHADOW);
	const std::string differs = config_difference(*document->config(), back);
	if (!differs.empty()) std::fprintf(stderr, "differs: %s\n", differs.c_str());
	TEST_EXPECT(differs.empty());
	std::printf("document: the keys in the game's units, the grid rows and foliage, the rules, written and read back\n");
	return 0;
}

// What the readers read otherwise than written.
int test_source_issues() {
	const std::string head = "polytrn_colormap c.tga\r\npolytrn_detailmap d.tga\r\npolytrn_polydata h.cpt\r\n";
	auto document = load(head +
	                     "polytrn_sectors 1 2\r\n"            // 4: before the width
	                     "polytrn_sectorcount 2\r\n"          // 5
	                     "polytrn_sectors 1\r\n"              // 6: short
	                     "polytrn_sectors 1 2 3\r\n"          // 7: wide
	                     "polytrn_scaled 3\r\n"               // 8: skipped
	                     "water_murk 1.0\r\n"                 // 9: past 0.99
	                     "polytrn_detaildensity 64\r\n"       // 10: written again on 11
	                     "polytrn_detaildensity 96\r\n"       // 11
	                     "foliage\r\n"                        // 12
	                     "  graphic a.3di\r\n"                // 13
	                     "  match 1 2 3 4 5\r\n"              // 14: past four
	                     "  colour 3\r\n"                     // 15: no such arm in a block
	                     "end\r\n");
	TEST_EXPECT(document && !document->blocked());
	if (!document) return 1;
	TEST_EXPECT(has_issue(*document, 4, false, "before the grid's width"));
	TEST_EXPECT(has_issue(*document, 6, false, "1 of the width's 2"));
	TEST_EXPECT(has_issue(*document, 7, false, "past the width's 2"));
	TEST_EXPECT(has_issue(*document, 8, false, "skip 'polytrn_scaled'"));
	TEST_EXPECT(has_issue(*document, 9, false, "0.99"));
	TEST_EXPECT(has_issue(*document, 10, false, "written again on line 11"));
	TEST_EXPECT(has_issue(*document, 14, false, "first four"));
	TEST_EXPECT(has_issue(*document, 15, false, "skips 'colour'"));
	TEST_EXPECT(read(*document, row_address(*document), "polytrn_detaildensity") == Value(int64_t(96)));
	const std::vector<Diagnostic> findings = validate_terrain_file(*document);
	TEST_EXPECT(std::count_if(findings.begin(), findings.end(), [](const Diagnostic &d) {
		            return d.code() == "terrain.ignored_input" && d.severity == DiagnosticSeverity::Warning;
	            }) == 8);
	TEST_EXPECT(document->rewrite_need() == DocumentBase::RewriteNeed::Rewrite);
	// A fifth foliage block: from it on the file is read by no arm.
	{
		std::string blocks;
		for (int i = 0; i < 5; ++i) blocks += "foliage\r\n  graphic g" + std::to_string(i) + ".3di\r\n  match 9\r\nend\r\n";
		auto five = load(head + blocks + "polytrn_charmap m.pcx\r\n");
		TEST_EXPECT(five && has_issue(*five, 20, false, "fifth") && five->config()->foliage_defs.size() == 4 &&
		            five->config()->charmap.empty());
	}
	// A last line no CR LF ends: "end" read "en", the block kept, its end written.
	{
		auto cut = load(head + "foliage\r\n  graphic a.3di\r\n  match 9\r\nend");
		TEST_EXPECT(cut && has_issue(*cut, 7, false, "no line end") && cut->config()->foliage_defs.size() == 1);
		TEST_EXPECT(cut && cut->serialize().text.find("end\r\n") != std::string::npos);
	}
	// A file of LF line ends: one line to the game's walk, which reads its first key alone (the line-ends rule:
	// the terrain's reader is the ASCII walk); restored, its height data and its definition read.
	{
		std::string lf;
		for (const char c : head + "foliage\r\n  graphic a.3di\r\n  match 9\r\nend\r\n")
			if (c != '\r') lf += c;
		auto odd = load(lf);
		TEST_EXPECT(odd && odd->config() && odd->config()->polydata.empty() && odd->config()->foliage_defs.empty());
		const std::vector<Diagnostic> ends = odd ? line_end_findings(*odd, "jo") : std::vector<Diagnostic>();
		TEST_EXPECT(ends.size() == 1 && ends[0].code() == "document.line_ends" && ends[0].planned.size() == 1);
		Diagnostic error;
		TEST_EXPECT(ends.size() == 1 && ends[0].planned.size() == 1 && odd->apply(ends[0].planned[0].edits, error) &&
		            odd->config()->polydata == "h.cpt" && odd->config()->foliage_defs.size() == 1);
		TEST_EXPECT(odd && line_end_findings(*odd, "jo").empty());
		// Undo gives the one-line reading back.
		if (odd) odd->undo();
		TEST_EXPECT(odd && odd->config()->polydata.empty() && line_end_findings(*odd, "jo").size() == 1);
	}
	// An environment keyword the record does not hold, polytrn_scale: the file blocks.
	{
		auto fog = load(head + "fog_level 600\r\n");
		TEST_EXPECT(fog && fog->blocked() && has_issue(*fog, 4, true, "environment keyword"));
		const std::vector<Diagnostic> blocking = validate_terrain_file(*fog);
		TEST_EXPECT(blocking.size() == 1 && blocking[0].code() == "terrain.invalid_input" &&
		            blocking[0].severity == DiagnosticSeverity::Error);
		auto scale = load(head + "polytrn_scale 128\r\n");
		TEST_EXPECT(scale && scale->blocked() && has_issue(*scale, 4, true, "multiplayer"));
		// polytrn_depthmap: an arm reads it (formats/trn trn_parser_key), the record does not hold it.
		auto depth = load(head + "polytrn_depthmap d.raw\r\n");
		TEST_EXPECT(depth && depth->blocked() && has_issue(*depth, 4, true, "polytrn_depthmap"));
		// horizon: no arm reads it, the record keeps it (save_trn writes it): no issue.
		auto horizon = load(head + "horizon 0\r\n");
		TEST_EXPECT(horizon && !horizon->blocked() && horizon->issues().empty());
		auto tod = load(head + "tod_begin 0600\r\n");
		TEST_EXPECT(tod && tod->blocked());
	}
	std::printf("source issues: a row before the width, short, wide; a key skipped, written again; a murk past 0.99; a "
	            "block's codes past four, a line no arm reads; a fifth block; a cut last line; LF line ends restored; an "
	            "environment keyword, polytrn_scale, polytrn_depthmap; horizon kept\n");
	return 0;
}

const Diagnostic *finding(const std::vector<Diagnostic> &findings, const char *code) {
	for (const Diagnostic &d : findings)
		if (d.code() == code) return &d;
	return nullptr;
}

// The gate's refusal, a width of 0, a definition that grows nothing: each a finding, each fix applied.
int test_findings() {
	const std::string maps = "polytrn_colormap c.tga\r\npolytrn_detailmap d.tga\r\npolytrn_polydata h.cpt\r\n";
	{
		auto unnamed = load("polytrn_detailmap d.tga\r\npolytrn_polydata h.cpt\r\npolytrn_sectorcount 1\r\npolytrn_sectors 1\r\n");
		TEST_EXPECT(unnamed && unnamed->refused().find("colour map") != std::string::npos);
		const std::vector<Diagnostic> found = unnamed ? validate_terrain_file(*unnamed) : std::vector<Diagnostic>();
		const Diagnostic *d = finding(found, "terrain.refused");
		TEST_EXPECT(d && d->severity == DiagnosticSeverity::Error && d->field == "polytrn_colormap" && d->planned.empty());
	}
	// A width of 6: Set the width to 8.
	{
		auto wide = load(maps + "polytrn_sectorcount 6\r\npolytrn_sectors 1 2 3 4 1 2\r\n");
		TEST_EXPECT(wide && wide->issues().empty());
		const std::vector<Diagnostic> found = wide ? validate_terrain_file(*wide) : std::vector<Diagnostic>();
		const Diagnostic *d = finding(found, "terrain.refused");
		TEST_EXPECT(d && d->field == "polytrn_sectorcount" && d->planned.size() == 1 &&
		            d->planned[0].label == "Set the width to 8");
		if (d && !d->planned.empty()) {
			Diagnostic error;
			for (const Edit &edit : d->planned[0].edits) TEST_EXPECT(wide->apply(edit, error));
			TEST_EXPECT(wide->refused().empty() && !finding(validate_terrain_file(*wide), "terrain.refused"));
		}
	}
	// Three rows: Fill the grid to 4 rows, the last row again.
	{
		auto rows = load(maps + "polytrn_sectorcount 2\r\npolytrn_sectors 1 3\r\npolytrn_sectors 2 4\r\npolytrn_sectors 0 4\r\n");
		const std::vector<Diagnostic> found = rows ? validate_terrain_file(*rows) : std::vector<Diagnostic>();
		const Diagnostic *d = finding(found, "terrain.refused");
		TEST_EXPECT(d && d->planned.size() == 1 && d->planned[0].label == "Fill the grid to 4 rows");
		if (rows && d && !d->planned.empty()) {
			Diagnostic error;
			for (const Edit &edit : d->planned[0].edits) TEST_EXPECT(rows->apply(edit, error));
			TEST_EXPECT(rows->refused().empty() && count_of(*rows, kSectorRow) == 4 &&
			            read(*rows, child(*rows, kSectorRow, 3), "sector_2") == Value(int64_t(4)));
		}
	}
	// No width: Set the width to 1.
	{
		auto none = load(maps + "polytrn_sectors 1\r\n");
		const std::vector<Diagnostic> found = none ? validate_terrain_file(*none) : std::vector<Diagnostic>();
		const Diagnostic *d = finding(found, "terrain.no_width");
		TEST_EXPECT(d && d->severity == DiagnosticSeverity::Warning && d->planned.size() == 1);
	}
	// A definition with no model, one with code 0 alone: each grows nothing, Remove it.
	{
		auto inert = load(maps + "polytrn_sectorcount 1\r\npolytrn_sectors 1\r\nfoliage\r\n  match 9\r\nend\r\nfoliage\r\n"
		                         "  graphic a.3di\r\n  match 0\r\nend\r\nfoliage\r\n  graphic b.3di\r\n  match 7\r\nend\r\n");
		std::vector<Diagnostic> found = inert ? validate_terrain_file(*inert) : std::vector<Diagnostic>();
		TEST_EXPECT(std::count_if(found.begin(), found.end(), [](const Diagnostic &d) {
			            return d.code() == "terrain.foliage_inert";
		            }) == 2);
		const Diagnostic *d = finding(found, "terrain.foliage_inert");
		TEST_EXPECT(d && d->field == "graphic" && d->planned.size() == 1 && d->planned[0].label == "Remove it" && d->child_id);
		if (inert && d && !d->planned.empty()) {
			Diagnostic error;
			TEST_EXPECT(inert->apply(d->planned[0].edits[0], error) && count_of(*inert, kFoliage) == 2);
		}
	}
	std::printf("findings: the gate's refusal with its fixes, no width, an inert definition removed\n");
	return 0;
}

const opennova::io::JsonValue *member(const opennova::io::JsonValue &object, const char *name) {
	return object.is_object() ? object.get(name) : nullptr;
}

std::string text_of(const opennova::io::JsonValue *value) { return value && value->is_string() ? value->string : std::string(); }

std::vector<uint8_t> grey_png(int side) {
	std::vector<uint8_t> rgba(size_t(side) * side * 4);
	for (int y = 0; y < side; ++y)
		for (int x = 0; x < side; ++x) {
			uint8_t *p = &rgba[(size_t(y) * side + x) * 4];
			p[0] = p[1] = p[2] = uint8_t((x + y) * 255 / (2 * side));
			p[3] = 255;
		}
	return encode_png_rgba(rgba.data(), uint32_t(side), uint32_t(side));
}

// In a session: an authored terrain's references through the document, the missions on it, an edit followed,
// undone, saved; an imported terrain opened and its edits refused, its import read and reimported.
int test_session() {
	editor_test::TempProjectDir dir("opennova_terrain_document");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Terrain"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	TEST_EXPECT(editor_test::write_text(root + "/isle.trn", kShipped));
	TEST_EXPECT(editor_test::write_text(root + "/isle_c.tga", "x"));
	// The mission's .env and overcast.def each set a terrain key, which the game's terrain reader takes after the
	// .trn (D-TERRAIN-18): the terrain's uses say where each comes from.
	TEST_EXPECT(editor_test::write_text(root + "/day.env", "sky_height 175\r\npolytrn_detaildensity 64\r\n"));
	TEST_EXPECT(editor_test::write_text(root + "/overcast.def", "polytrn_wrapx 1\r\n"));
	{
		opennova::bms::File file;
		opennova::mission::make_default(file);
		std::string why;
		TEST_EXPECT(opennova::mission::set_header_string(file, "terrain", "isle", why) &&
		            opennova::mission::set_header_string(file, "environment", "day", why) &&
		            opennova::mission::set_header_string(file, "terrain_tile", "rock.tga", why));
		std::vector<uint8_t> bytes;
		TEST_EXPECT(opennova::bms::write(file, bytes, why));
		TEST_EXPECT(editor_test::write_bytes(root + "/landing.bms", bytes));
		TEST_EXPECT(editor_test::write_text(root + "/landing.til", "0lit"));
	}
	editor_test::handle_to_end(session, request::rescan());
	const AssetGraph &graph = *view.findings.graph;
	// The references are the document's fields: rewritable, on the terrain's row, each map by its role.
	{
		const std::vector<const GraphEdge *> refs = graph.references_of("isle.trn");
		const auto find = [&](const char *value) -> const GraphEdge * {
			for (const GraphEdge *e : refs)
				if (e->value == value) return e;
			return nullptr;
		};
		const GraphEdge *colour = find("isle_c.tga");
		TEST_EXPECT(colour && colour->field == "polytrn_colormap" && colour->rewritable && colour->record == "Terrain" &&
		            colour->loader_arg == texture_role_arg(renderer::TextureRoleId::TerrainColourMap, kTextureArgGates) &&
		            graph.resolve(*colour) == ReferenceStatus::Present);
		const GraphEdge *heights = find("isle.cpt");
		TEST_EXPECT(heights && heights->kind == ReferenceKind::TerrainData && heights->field == "polytrn_polydata");
		const GraphEdge *grass = find("mveg5.3di");
		TEST_EXPECT(grass && grass->kind == ReferenceKind::Model && grass->record == "Terrain/Foliage 2" &&
		            grass->field == "graphic");
	}
	// The missions that run on it.
	{
		const TerrainUses uses = terrain_uses(view, "isle.trn");
		TEST_EXPECT(uses.found && !uses.import.imported && uses.missions.size() == 1);
		if (uses.missions.size() == 1) {
			const TerrainMissionUse &use = uses.missions[0];
			TEST_EXPECT(use.read && use.name == "landing" && use.environment == "day" && use.environment_file == "day.env" &&
			            use.tile_set == "rock.tga" && use.tiles && use.edge && use.edge->field == "terrain");
			TEST_EXPECT(uses.overcast_file == "overcast.def" && use.later.size() == 2);
			if (use.later.size() == 2) {
				TEST_EXPECT(use.later[0].file == opennova::TrnLaterLine::File::Overcast && use.later[0].key == "polytrn_wrapx" &&
				            use.later[0].value == "1" && use.later[0].line == 1);
				TEST_EXPECT(use.later[1].file == opennova::TrnLaterLine::File::Environment &&
				            use.later[1].key == "polytrn_detaildensity" && use.later[1].value == "64" && use.later[1].line == 2);
				// Each the first terrain line of its file: the environment's its first terrain key, where a Go to opens
				// it (its document's locator).
				TEST_EXPECT(use.later[0].index == 0 && use.later[1].index == 0 && terrain_key_locator(0) == "0/terrain_key:0");
			}
		}
		opennova::io::JsonValue args = opennova::io::JsonValue::make_object();
		args.set("path", opennova::io::json_string("isle.trn"));
		std::string why;
		const opennova::io::JsonValue answer = session.query("terrain_uses", args, why);
		const opennova::io::JsonValue *missions = member(answer, "missions");
		TEST_EXPECT(why.empty() && missions && missions->is_array() && missions->array.size() == 1);
		const opennova::io::JsonValue *later =
				missions && missions->is_array() && missions->array.size() == 1 ? member(missions->array[0], "later") : nullptr;
		TEST_EXPECT(later && later->is_array() && later->array.size() == 2);
		TEST_EXPECT(member(answer, "import") && member(answer, "import")->is_null());
		args.set("path", opennova::io::json_string("day.env"));
		session.query("terrain_uses", args, why);
		TEST_EXPECT(why.find("not a terrain") != std::string::npos);
	}
	// Opened, edited: the graph follows the unsaved edit; undone; saved through the writer.
	editor_test::handle_to_end(session, request::open_document("isle.trn"));
	Document *open = session.document_for("isle.trn");
	TEST_EXPECT(open && dynamic_cast<TerrainDocument *>(open) != nullptr);
	if (!open) return 1;
	const NodeAddress row{open->rows().front()->id, kTerrain, 0};
	Edit rename;
	rename.address = row;
	rename.field = "polytrn_colormap";
	rename.value = std::string("dusk_c.tga");
	editor_test::handle_to_end(session, request::edit_record("isle.trn", rename));
	const auto names = [&](const char *value) {
		const std::vector<const GraphEdge *> refs = view.findings.graph->references_of("isle.trn");
		return std::any_of(refs.begin(), refs.end(), [&](const GraphEdge *e) { return e->value == value; });
	};
	TEST_EXPECT(names("dusk_c.tga") && !names("isle_c.tga"));
	editor_test::handle_to_end(session, request::undo("isle.trn"));
	TEST_EXPECT(names("isle_c.tga") && !names("dusk_c.tga"));
	Edit water;
	water.address = row;
	water.field = "water_height";
	water.value = int64_t(30);
	editor_test::handle_to_end(session, request::edit_record("isle.trn", water));
	editor_test::handle_to_end(session, request::save("isle.trn"));
	{
		const std::vector<uint8_t> saved = test_io::read_file(root + "/isle.trn");
		const std::string saved_text(saved.begin(), saved.end());
		TEST_EXPECT(parsed(saved_text).water_height == 30 && parsed(saved_text).creator == "Someone");
	}

	// A terrain an import makes: opened to be read, no edit taken, its import read and imported again.
	const std::string images = dir.file("images");
	TEST_EXPECT(editor_test::write_bytes(images + "/height.png", grey_png(1024)));
	TEST_EXPECT(editor_test::write_bytes(images + "/colour.png", grey_png(1024)));
	ActionOutcome outcome = editor_test::handle_to_end(session, request::new_terrain("reef",
	        {{"heightmap", images + "/height.png"}, {"colormap", images + "/colour.png"}, {"water", "6"}}));
	TEST_EXPECT(!outcome.refused);
	const AssetEntry *made = view.project.scan->find("reef.trn");
	TEST_EXPECT(made && made->imported_from == "art/terrain/reef.tset");
	if (!made) return 1;
	const std::string made_path = made->relative_path;
	{
		const TerrainUses uses = terrain_uses(view, "reef.trn");
		TEST_EXPECT(uses.import.imported && uses.import.source == "art/terrain/reef.tset" && uses.import.error.empty());
		TEST_EXPECT(uses.import.images.size() == 2 && uses.import.images[0].key == "heightmap" &&
		            uses.import.images[0].file == "art/terrain/reef_heightmap.png");
		const auto water_option = std::find_if(uses.import.options.begin(), uses.import.options.end(),
		                                       [](const TerrainImportOption &o) { return o.key == "water"; });
		TEST_EXPECT(water_option != uses.import.options.end() && water_option->value == "6" && water_option->set);
		TEST_EXPECT(std::find(uses.import.outputs.begin(), uses.import.outputs.end(), made_path) != uses.import.outputs.end());
		const opennova::io::JsonValue json = terrain_uses_json(uses);
		const opennova::io::JsonValue *import = member(json, "import");
		const opennova::io::JsonValue *reimport = import ? member(*import, "reimport") : nullptr;
		TEST_EXPECT(reimport && text_of(member(*reimport, "kind")) == "reimport" &&
		            text_of(member(*reimport, "path")) == "art/terrain/reef.tset");
	}
	editor_test::handle_to_end(session, request::open_document(made_path));
	Document *imported = session.document_for(made_path);
	TEST_EXPECT(imported && dynamic_cast<TerrainDocument *>(imported) != nullptr);
	if (!imported) return 1;
	Edit wet;
	wet.address = {imported->rows().front()->id, kTerrain, 0};
	wet.field = "water_height";
	wet.value = int64_t(40);
	outcome = editor_test::handle_to_end(session, request::edit_record(made_path, wet));
	TEST_EXPECT(outcome.refused && !outcome.findings.empty() && outcome.findings.back().code() == "document.imported" &&
	            outcome.findings.back().message.find("reef.tset") != std::string::npos);
	TEST_EXPECT(!imported->dirty() && read(*imported, wet.address, "water_height") == Value(int64_t(12)));
	outcome = editor_test::handle_to_end(session, request::reimport("art/terrain/reef.tset", true));
	TEST_EXPECT(!outcome.refused && view.project.scan->find("reef.trn"));
	std::printf("session: an authored terrain's references through the document, its mission, an edit followed, undone, "
	            "saved; an imported terrain's edits refused, its import read and reimported\n");
	return 0;
}

// Every .trn of the install through the document and written again: the same terrain, field for field, and a
// second save writes the same bytes.
int test_retail() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (every retail .trn through the terrain document)");
		return 0;
	}
	opennova::Vfs vfs;
	TEST_EXPECT(vfs.mount_game(install.c_str(), std::string(), opennova::VfsMountMode::Packed));
	size_t files = 0, issues = 0, blocked = 0, refused = 0, inert = 0;
	for (const auto &location : vfs.list_files()) {
		const std::string &name = location.logical_name;
		if (name.size() < 4) continue;
		std::string extension = name.substr(name.size() - 4);
		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
		if (extension != ".trn") continue;
		std::vector<uint8_t> bytes;
		if (!vfs.read_file_raw(name, bytes) || bytes.empty()) continue;
		++files;
		auto document = std::make_unique<TerrainDocument>();
		Diagnostic error;
		TEST_EXPECT(document->load_bytes(bytes, name, AssetKind::Terrain, "jo", error));
		issues += document->issues().size();
		for (const SourceIssue &issue : document->issues())
			std::printf("  %s line %zu: %s\n", name.c_str(), issue.line, issue.message.c_str());
		if (document->blocked()) {
			++blocked;
			continue;
		}
		refused += !document->refused().empty();
		for (const Diagnostic &d : validate_terrain_file(*document)) inert += d.code() == "terrain.foliage_inert";
		const TrnConfig *held = document->config();
		TEST_EXPECT(held != nullptr);
		if (!held) continue;
		const SerializeResult written = document->serialize();
		TEST_EXPECT(written.ok());
		// What the game reads of the original, the grid extended as its gate's last leg extends it.
		const std::string original(bytes.begin(), bytes.end());
		const std::string difference = config_difference(parsed(original), parsed(written.text));
		if (!difference.empty()) std::fprintf(stderr, "FAIL: %s: %s reads back otherwise\n", name.c_str(), difference.c_str());
		TEST_EXPECT(difference.empty());
		auto again = std::make_unique<TerrainDocument>();
		TEST_EXPECT(again->load_bytes(bytes_of(written.text), name, AssetKind::Terrain, "jo", error) &&
		            again->serialize().text == written.text && again->issues().empty());
	}
	std::printf("retail: %zu terrains read and written again field for field (%zu source issues, %zu blocked, %zu "
	            "refused, %zu inert definitions)\n",
	            files, issues, blocked, refused, inert);
	TEST_EXPECT(files >= 30 && blocked == 0 && refused == 0);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = test_type();
	failures += test_document();
	failures += test_source_issues();
	failures += test_findings();
	failures += test_session();
	failures += test_retail();
	if (failures == 0) std::printf("editor_terrain_document: all passed\n");
	return failures == 0 ? 0 : 1;
}
