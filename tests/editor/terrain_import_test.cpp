// A terrain made from images (ADR 0046 S20): the terrain set's text; a heightmap read at every depth
// it takes (an 8-bit PNG TrnGen's way, a 16-bit one scaled by `top`, TrnGen's own .raw and the game's
// raw16) and refused at any other size; the new_terrain request over a project (the images copied
// into art/terrain/ as the set's inputs, the set and its record written, the import making the
// terrain's files under the cache, which the scan types and the build packs while the images are
// never packed); the outputs read back through the engine's readers as the game reads them (the .trn
// past the admission gate, the .cpt's CDEP and 341 tiles, the colour map 1024 x 1024, the blend map,
// the empty .til); the runtime's own terrain load over them (the field store's heights, the frame
// compiler's patches from afar, and near an eye standing on the slope the finest tile under it drawn
// at the ground's height whichever way it looks); the import rerun-stable (forced again, the same bytes); the blank environment
// a mission can be made under, and a blank mission made on the new terrain; and the refusals that
// write nothing (a name taken, a colour map of another size, a value of no key).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <base/resource_index/resource_index.h>
#include <editor/assets/asset_registry.h>
#include <editor/blank/blank_factory.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/project_validation.h>
#include <editor/import/import_run.h>
#include <editor/import/png_encode.h>
#include <editor/import/sidecar.h>
#include <editor/import/terrain_import.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_plan.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <formats/cpt/cpt_io.h>
#include <formats/env/env.h>
#include <formats/tga/tga.h>
#include <formats/til/til_io.h>
#include <formats/trn/trn_io.h>
#include <runtime/terrain/terrain_frame.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/terrain_query/terrain_field_build.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/png_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
namespace fs = std::filesystem;
using editor_test::NoProcess;
using editor_test::PngSpec;
using editor_test::make_png;

namespace {

constexpr int kSide = 1024;

// A 16-bit grey PNG of an island: a cone from the centre, its top `peak` of 65535.
std::vector<uint8_t> island_png16(double peak = 0.5) {
	PngSpec spec;
	spec.width = kSide;
	spec.height = kSide;
	spec.depth = 16;
	spec.color_type = 0;
	for (int y = 0; y < kSide; ++y) {
		spec.rows.push_back(0);
		for (int x = 0; x < kSide; ++x) {
			const double r = std::hypot(x - 512.0, y - 512.0) / 400.0;
			const uint16_t v = static_cast<uint16_t>(65535.0 * peak * std::max(0.0, 1.0 - r));
			spec.rows.push_back(uint8_t(v >> 8));
			spec.rows.push_back(uint8_t(v & 0xFF));
		}
	}
	return make_png(spec);
}

// An 8-bit grey PNG of `side` texels, a ramp.
std::vector<uint8_t> ramp_png8(int side) {
	PngSpec spec;
	spec.width = side;
	spec.height = side;
	spec.depth = 8;
	spec.color_type = 0;
	for (int y = 0; y < side; ++y) {
		spec.rows.push_back(0);
		for (int x = 0; x < side; ++x) spec.rows.push_back(uint8_t(x * 255 / std::max(1, side - 1)));
	}
	return make_png(spec);
}

// An RGBA image of `side` texels, green fading to sand.
std::vector<uint8_t> colour_png(int side) {
	std::vector<uint8_t> rgba(size_t(side) * side * 4);
	for (int y = 0; y < side; ++y)
		for (int x = 0; x < side; ++x) {
			uint8_t *p = &rgba[(size_t(y) * side + x) * 4];
			p[0] = uint8_t(60 + x * 120 / side);
			p[1] = uint8_t(140 - y * 40 / side);
			p[2] = 60;
			p[3] = 255;
		}
	return encode_png_rgba(rgba.data(), uint32_t(side), uint32_t(side));
}

std::vector<uint8_t> read(const std::string &path) {
	std::vector<uint8_t> bytes;
	std::string message;
	read_file_bytes(path, bytes, message);
	return bytes;
}

// A camera at `eye` looking along `fwd` (normalized here): the column-major world-to-view matrix the
// frame compiler culls with, -Z forward, +Y up.
opennova::TerrainViewInput camera_at(const float eye[3], float fwd[3]) {
	opennova::TerrainViewInput camera;
	camera.cam_x = eye[0];
	camera.cam_y = eye[1];
	camera.cam_z = eye[2];
	const float length = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
	for (int i = 0; i < 3; ++i) fwd[i] /= length;
	float right[3] = {-fwd[2], 0.0f, fwd[0]}; // fwd x up
	const float side = std::sqrt(right[0] * right[0] + right[2] * right[2]);
	for (float &r : right) r /= side;
	const float up[3] = {right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2],
	                     right[0] * fwd[1] - right[1] * fwd[0]};
	const auto dot = [](const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
	float *m = camera.view;
	m[0] = right[0]; m[4] = right[1]; m[8] = right[2]; m[12] = -dot(right, eye);
	m[1] = up[0]; m[5] = up[1]; m[9] = up[2]; m[13] = -dot(up, eye);
	m[2] = -fwd[0]; m[6] = -fwd[1]; m[10] = -fwd[2]; m[14] = dot(fwd, eye);
	m[3] = 0.0f; m[7] = 0.0f; m[11] = 0.0f; m[15] = 1.0f;
	return camera;
}

// The drawn surface's height at sector-local (x, z): the triangle of the tile's LOD list (strip or
// list, as the embedder converts it) that holds the point, interpolated; NAN when none does.
float drawn_height(const std::vector<opennova::TerrainTileVertex> &vertices, const opennova::CptTileLOD &lod, float x,
                   float z) {
	const auto at = [&](uint16_t a, uint16_t b, uint16_t c) {
		if (a == b || b == c || a == c || std::max({a, b, c}) >= vertices.size()) return NAN;
		const auto &p0 = vertices[a].position, &p1 = vertices[b].position, &p2 = vertices[c].position;
		const float det = (p1[0] - p0[0]) * (p2[2] - p0[2]) - (p2[0] - p0[0]) * (p1[2] - p0[2]);
		if (det == 0.0f) return NAN;
		const float l1 = ((x - p0[0]) * (p2[2] - p0[2]) - (p2[0] - p0[0]) * (z - p0[2])) / det;
		const float l2 = ((p1[0] - p0[0]) * (z - p0[2]) - (x - p0[0]) * (p1[2] - p0[2])) / det;
		const float l0 = 1.0f - l1 - l2;
		if (l0 < -1e-4f || l1 < -1e-4f || l2 < -1e-4f) return NAN;
		return l0 * p0[1] + l1 * p1[1] + l2 * p2[1];
	};
	const std::vector<uint16_t> &idx = lod.indices;
	for (size_t i = 2; i < idx.size(); i += lod.is_strip ? 1 : 3) {
		const float h = at(idx[i - 2], idx[i - 1], idx[i]);
		if (!std::isnan(h)) return h;
	}
	return NAN;
}

int test_set_text() {
	TerrainSet set;
	set.heightmap = "isle_heightmap.png";
	set.colormap = "isle_colormap.png";
	set.tiles = "../shared/tiles.tga";
	TerrainSet back;
	std::string why;
	TEST_EXPECT(parse_terrain_set(write_terrain_set(set), back, why));
	TEST_EXPECT(back.heightmap == set.heightmap && back.colormap == set.colormap && back.detail.empty() &&
	            back.tiles == set.tiles);
	const std::string text = "; a comment\r\nheightmap h.png ; the heights\r\ncolormap  c.png\r\n";
	TEST_EXPECT(parse_terrain_set(std::vector<uint8_t>(text.begin(), text.end()), back, why) && back.heightmap == "h.png" &&
	            back.colormap == "c.png");
	const std::string wrong = "heightmap h.png\r\nfoliage f.pcx\r\n";
	TEST_EXPECT(!parse_terrain_set(std::vector<uint8_t>(wrong.begin(), wrong.end()), back, why) &&
	            why.find("foliage") != std::string::npos);
	const std::string half = "heightmap h.png\r\n";
	TEST_EXPECT(!parse_terrain_set(std::vector<uint8_t>(half.begin(), half.end()), back, why) &&
	            why.find("colormap") != std::string::npos);
	TEST_EXPECT(terrain_stem_fits("island", why) && !terrain_stem_fits("islandlong", why) && !terrain_stem_fits("is-le", why) &&
	            !terrain_stem_fits("", why));
	// The options: a row each, the fallbacks, a value no row takes refused with its key.
	TerrainImportSettings settings;
	std::string field;
	TEST_EXPECT(terrain_import_settings({}, settings, why, field) && settings.top == 127.5 && settings.water == 0.0 &&
	            settings.layout == "island");
	TEST_EXPECT(terrain_import_settings({{"top", "64"}, {"water", "10.5"}, {"layout", "Tiled"}}, settings, why, field) &&
	            settings.top == 64.0 && settings.water == 10.5 && settings.layout == "tiled");
	TEST_EXPECT(!terrain_import_settings({{"top", "300"}}, settings, why, field) && field == "top");
	TEST_EXPECT(!terrain_import_settings({{"layout", "ring"}}, settings, why, field) && field == "layout");
	TEST_EXPECT(!terrain_import_settings({{"depth", "1"}}, settings, why, field) && field == "depth");
	return 0;
}

int test_heightmap_depths() {
	TerrainHeights heights;
	std::string why;
	// 16 bits: 0..65535 over 0..top, 256 raw a world unit.
	TEST_EXPECT(decode_terrain_heightmap("h.png", island_png16(1.0), 127.5, heights, why));
	TEST_EXPECT(heights.depth8.empty() && heights.depth16.size() == size_t(kSide) * kSide);
	TEST_EXPECT(heights.depth16[512 * kSide + 512] == 32640 && heights.depth16[0] == 0);
	TEST_EXPECT(decode_terrain_heightmap("h.png", island_png16(1.0), 200.0, heights, why) &&
	            heights.depth16[512 * kSide + 512] == 51200);
	// 8 bits at TrnGen's own scale go to the bake as they are; at another, TrnGen's smoothing then the scale.
	TEST_EXPECT(decode_terrain_heightmap("h.png", ramp_png8(kSide), 127.5, heights, why) && heights.depth16.empty() &&
	            heights.depth8.size() == size_t(kSide) * kSide && heights.depth8[kSide - 1] == 255);
	TEST_EXPECT(decode_terrain_heightmap("h.png", ramp_png8(kSide), 63.75, heights, why) && heights.depth8.empty() &&
	            heights.depth16.size() == size_t(kSide) * kSide);
	// TrnGen's .raw (1 MiB, 8 bits) and the game's raw16 (2 MiB), taken as they are.
	TEST_EXPECT(decode_terrain_heightmap("h.raw", std::vector<uint8_t>(size_t(kSide) * kSide, 7), 127.5, heights, why) &&
	            heights.depth8[0] == 7);
	std::vector<uint8_t> raw16(size_t(kSide) * kSide * 2, 0);
	raw16[0] = 0x34;
	raw16[1] = 0x12;
	TEST_EXPECT(decode_terrain_heightmap("h.raw", raw16, 10.0, heights, why) && heights.depth16[0] == 0x1234);
	// Any other size refused in words.
	TEST_EXPECT(!decode_terrain_heightmap("h.png", ramp_png8(512), 127.5, heights, why) &&
	            why.find("1024 x 1024") != std::string::npos);
	TEST_EXPECT(!decode_terrain_heightmap("h.raw", std::vector<uint8_t>(100), 127.5, heights, why));
	// The steep check: a 256-texel stretch spanning more than 128 units.
	std::vector<uint16_t> steep(1024, 0);
	steep[300] = 40000;
	TEST_EXPECT(terrain_steep_blocks(steep) == 1);
	return 0;
}

struct Project {
	editor_test::TempProjectDir dir;
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session;
	std::string root;
	explicit Project(const char *name) : dir(name), session(platform, preferences) {
		editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Terrains"));
		root = session.view().project.root;
	}
	std::string at(const std::string &relative) const { return root + "/" + relative; }
	const AssetEntry *find(const std::string &name) const { return session.view().project.scan->find(name); }
	std::string output(const std::string &name) const {
		const AssetEntry *entry = find(name);
		return entry ? at(entry->relative_path) : std::string();
	}
};

int test_new_terrain() {
	Project project("opennova_editor_terrain_import");
	const std::string images = project.dir.file("images");
	TEST_EXPECT(editor_test::write_bytes(images + "/height.png", island_png16()));
	TEST_EXPECT(editor_test::write_bytes(images + "/colour.png", colour_png(kSide)));
	TEST_EXPECT(editor_test::write_bytes(images + "/small.png", colour_png(256)));
	const ProjectSession &session = project.session;

	// Refused, nothing written: a colour map of another size, a value of no key, a name too long.
	ActionOutcome outcome = editor_test::handle_to_end(project.session,
	        request::new_terrain("isle", {{"heightmap", images + "/height.png"}, {"colormap", images + "/small.png"}}));
	TEST_EXPECT(outcome.refused && !outcome.findings.empty() && outcome.findings.back().code() == "import.terrain");
	TEST_EXPECT(outcome.findings.back().message.find("1024 x 1024") != std::string::npos);
	TEST_EXPECT(!fs::exists(system_path(project.at("art/terrain"))));
	outcome = editor_test::handle_to_end(project.session, request::new_terrain("isle",
	        {{"heightmap", images + "/height.png"}, {"colormap", images + "/colour.png"}, {"sea", "4"}}));
	TEST_EXPECT(outcome.refused && outcome.findings.back().message.find("'sea'") != std::string::npos);
	outcome = editor_test::handle_to_end(project.session, request::new_terrain("averylongname",
	        {{"heightmap", images + "/height.png"}, {"colormap", images + "/colour.png"}}));
	TEST_EXPECT(outcome.refused && !fs::exists(system_path(project.at("art/terrain"))));

	// Made: the images copied in as the set's inputs, the set and its record, then imported.
	outcome = editor_test::handle_to_end(project.session, request::new_terrain("isle",
	        {{"heightmap", images + "/height.png"}, {"colormap", images + "/colour.png"}, {"water", "12"}, {"top", "100"}}));
	for (const Diagnostic &d : outcome.findings) std::fprintf(stderr, "  %s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(!outcome.refused);
	TerrainSet set;
	std::string why;
	TEST_EXPECT(parse_terrain_set(read(project.at("art/terrain/isle.tset")), set, why));
	TEST_EXPECT(set.heightmap == "isle_heightmap.png" && set.colormap == "isle_colormap.png" && set.detail.empty());
	TEST_EXPECT(read(project.at("art/terrain/isle_heightmap.png")) == island_png16());
	ImportSidecar record;
	Diagnostic error;
	TEST_EXPECT(load_import_sidecar(project.at("art/terrain/isle.tset.import"), record, error));
	TEST_EXPECT(record.importer == "terrain" && record.options.at("water") == "12" && record.options.at("top") == "100");
	TEST_EXPECT((record.inputs == std::vector<std::string>{"isle_heightmap.png", "isle_colormap.png"}));
	TEST_EXPECT(record.outputs == terrain_output_names("isle", false));

	// The scan: the outputs project files of their kinds, the set an import source, the images inputs.
	const SessionView &view = session.view();
	TEST_EXPECT(view.project.imports && view.project.imports->size() == 1 && (*view.project.imports)[0].ok);
	TEST_EXPECT(project.find("isle.trn") && project.find("isle.trn")->kind == AssetKind::Terrain);
	TEST_EXPECT(project.find("isle.cpt") && project.find("isle.cpt")->kind == AssetKind::TerrainPolyData);
	TEST_EXPECT(project.find("isle.til") && project.find("isle.til")->kind == AssetKind::TileInfo);
	for (const char *texture : {"isle_c.tga", "isle_dt.tga", "isle_dm.tga", "isle_d1.tga"})
		TEST_EXPECT(project.find(texture) && project.find(texture)->kind == AssetKind::Texture);
	TEST_EXPECT(project.find("isle.tset") && project.find("isle.tset")->kind == AssetKind::ImportSource);
	TEST_EXPECT(project.find("isle_heightmap.png") && project.find("isle_heightmap.png")->kind == AssetKind::ImportInput);
	TEST_EXPECT(project.find("isle_colormap.png") && project.find("isle_colormap.png")->kind == AssetKind::ImportInput);

	// The build packs the terrain's files, never its images or its set.
	AssetGraph graph;
	ValidationCache cache;
	const ProjectPaths paths = ProjectPaths::for_root(project.root);
	const BuildPlan plan = plan_build(paths, *view.project.scan, *view.project.requirements,
	                                  validate_project({paths, *view.project.document, *view.project.scan, view.documents.open},
	                                                   graph, cache));
	std::map<std::string, bool> packed;
	for (const BuildArchive &archive : plan.archives)
		for (const BuildEntry &entry : archive.entries) packed[entry.logical_name] = true;
	for (const BuildEntry &entry : plan.loose) packed[entry.logical_name] = true;
	TEST_EXPECT(packed["isle.trn"] && packed["isle.cpt"] && packed["isle_c.tga"] && packed["isle_d1.tga"] && packed["isle.til"]);
	TEST_EXPECT(!packed["isle_heightmap.png"] && !packed["isle_colormap.png"] && !packed["isle.tset"]);

	// The outputs, read back as the game reads them.
	opennova::TrnConfig trn;
	{
		const std::vector<uint8_t> text = read(project.output("isle.trn"));
		std::istringstream in(std::string(text.begin(), text.end()));
		TEST_EXPECT(opennova::load_trn(in, trn, why)); // the admission gate
	}
	TEST_EXPECT(trn.name == "isle" && trn.colormap == "isle_c.tga" && trn.polydata == "isle.cpt" && trn.detailmap == "isle_dm.tga");
	TEST_EXPECT(trn.detailmap_c1 == "isle_dt.tga" && trn.detailblendmap == "isle_d1.tga" && trn.tileinfo == "isle.til");
	TEST_EXPECT(trn.water_height == 24 && trn.sector_count == 8 && trn.origin_x == -4);
	TEST_EXPECT(trn.sector_grid[3][3] == 1 && trn.sector_grid[3][4] == 3 && trn.sector_grid[4][3] == 2 && trn.sector_grid[4][4] == 4);
	TEST_EXPECT(trn.sector_grid[0][0] == 0 && trn.wrap_x == 0 && trn.charmap.empty());
	const std::vector<uint8_t> cpt_bytes = read(project.output("isle.cpt"));
	opennova::CptFile cpt;
	TEST_EXPECT(opennova::load_cpt(cpt_bytes.data(), cpt_bytes.size(), cpt, why));
	TEST_EXPECT(cpt.depth_format == opennova::DepthFormat::CDEP && cpt.tiles.size() == 341 && cpt_bytes[6] == 5);
	// The peak: half of 65535 over 0..100 units is 50 units, within the mesh's rasterized error.
	const double peak = cpt.depth_buffer[512 * kSide + 512] / 256.0;
	TEST_EXPECT(peak > 48.0 && peak < 52.0 && cpt.depth_buffer[0] == 0);
	for (const char *texture : {"isle_c.tga", "isle_d1.tga"}) {
		const std::vector<uint8_t> tga = read(project.output(texture));
		uint32_t w = 0, h = 0;
		TEST_EXPECT(opennova::tga::tga_header_size(tga.data(), tga.size(), w, h) && w == 1024 && h == 1024);
	}
	TEST_EXPECT(read(project.output("isle_c.tga"))[16] == 24 && read(project.output("isle_d1.tga"))[16] == 32);
	opennova::TilFile til;
	const std::vector<uint8_t> til_bytes = read(project.output("isle.til"));
	TEST_EXPECT(til_bytes.size() == 16 && opennova::load_til(til_bytes.data(), til_bytes.size(), til, why) && til.empty());

	// The runtime's own load over the outputs: the heights the field store samples, the patches a
	// frame compiles over the island.
	{
		opennova::ResourceIndex index;
		const std::string dir = utf8_of(path_of(project.output("isle.trn")).parent_path());
		TEST_EXPECT(index.scan(dir));
		opennova::terrain::TerrainFieldStore store;
		TEST_EXPECT(opennova::terrain::terrain_field_store_load(store, index, "isle", why));
		TEST_EXPECT(store.valid());
		const float centre = opennova::terrain::height_field_height_world_bilinear(store.height_field(), 0.0f, 0.0f);
		const float shore = opennova::terrain::height_field_height_world_bilinear(store.height_field(), 450.0f, 0.0f);
		TEST_EXPECT(centre > 48.0f && centre < 52.0f && shore < 2.0f);
		const opennova::TerrainSceneSnapshot scene = opennova::build_terrain_scene_snapshot(cpt, trn);
		TEST_EXPECT(scene.valid());
		// From the island's south, looking at its centre.
		const float far_eye[3] = {0.0f, 200.0f, 600.0f};
		float at_centre[3] = {-far_eye[0], -far_eye[1], -far_eye[2]};
		opennova::TerrainFrameCompiler compiler;
		TEST_EXPECT(!compiler.compile(scene, camera_at(far_eye, at_centre)).patches.empty());

		// Near the eye: the finest tiles all carry vertices and every LOD list a triangle, every leaf
		// node the traversal reaches resolves to its tile, and an eye standing on the slope (1.7 over
		// the ground, the island's sector grid putting world (x, z) at texel (512 + x, 512 + z)) draws
		// the leaf tile under it, at the finest family, its surface there the ground's, whichever
		// way it looks.
		int leaves = 0;
		for (const opennova::CptTile &tile : cpt.tiles) {
			if (tile.tile_size != 64) continue;
			++leaves;
			TEST_EXPECT(tile.vertex_count > 0);
			for (const opennova::CptTileLOD &lod : tile.lods) TEST_EXPECT(lod.indices.size() >= 3);
		}
		TEST_EXPECT(leaves == 256);
		for (const auto &node : scene.quad_nodes)
			if (node.is_leaf) TEST_EXPECT(node.size == 64 && node.tile_index >= 0);
		const int eye_x = 60, eye_z = -40;
		const float ground = cpt.depth_buffer[(512 + eye_z) * kSide + 512 + eye_x] / 256.0f;
		TEST_EXPECT(ground > 35.0f && ground < 45.0f);
		const float eye[3] = {float(eye_x), ground + 1.7f, float(eye_z)};
		for (const float yaw : {0.0f, 90.0f, 180.0f, 270.0f}) {
			for (const float pitch : {0.0f, -40.0f}) {
				const float y = yaw * 3.14159265f / 180.0f, p = pitch * 3.14159265f / 180.0f;
				float fwd[3] = {std::sin(y) * std::cos(p), std::sin(p), -std::cos(y) * std::cos(p)};
				const opennova::TerrainDrawList &near = compiler.compile(scene, camera_at(eye, fwd));
				TEST_EXPECT(near.debug.traversal.budget_drops == 0);
				int under = 0;
				for (const opennova::TerrainPatchDraw &draw : near.patches) {
					const opennova::CptTile &tile = cpt.tiles[draw.tile_index];
					const float local_x = eye[0] - draw.sector_ox, local_z = eye[2] - draw.sector_oz;
					const int x0 = tile.tile_x & 0x1ff, z0 = tile.tile_y & 0x1ff;
					if (draw.zero_height || local_x < x0 || local_x > x0 + tile.tile_size || local_z < z0 ||
					    local_z > z0 + tile.tile_size)
						continue;
					++under;
					TEST_EXPECT(draw.page_lod_level == 4 && draw.lod_family == 0 && tile.tile_size == 64);
					const float drawn = drawn_height(
					        opennova::build_terrain_tile_vertices(cpt, trn, draw.tile_index, false),
					        tile.lods[draw.lod_family], local_x, local_z);
					TEST_EXPECT(std::fabs(drawn - ground) < 0.25f);
				}
				TEST_EXPECT(under == 1);
			}
		}
	}

	// Rerun-stable: imported again with nothing changed, the same bytes.
	std::map<std::string, std::vector<uint8_t>> before;
	for (const std::string &name : terrain_output_names("isle", false)) before[name] = read(project.output(name));
	outcome = editor_test::handle_to_end(project.session, request::reimport("art/terrain/isle.tset", true));
	TEST_EXPECT(!outcome.refused && (*session.view().project.imports)[0].reimported);
	for (const auto &[name, bytes] : before) TEST_EXPECT(!bytes.empty() && read(project.output(name)) == bytes);

	// An input changed imports it again: the colour map's first texel.
	std::vector<uint8_t> rgba(size_t(kSide) * kSide * 4, 200);
	TEST_EXPECT(editor_test::write_bytes(project.at("art/terrain/isle_colormap.png"), encode_png_rgba(rgba.data(), kSide, kSide)));
	outcome = editor_test::handle_to_end(project.session, request::reimport(std::string(), false));
	TEST_EXPECT(read(project.output("isle_c.tga")) != before["isle_c.tga"] && read(project.output("isle.cpt")) == before["isle.cpt"]);

	// A name taken: refused, nothing written over.
	outcome = editor_test::handle_to_end(project.session, request::new_terrain("isle",
	        {{"heightmap", images + "/height.png"}, {"colormap", images + "/colour.png"}}));
	TEST_EXPECT(outcome.refused && outcome.findings.back().message.find("already") != std::string::npos);

	// An environment, then a mission on the new terrain under it.
	outcome = editor_test::handle_to_end(project.session, request::create_file("isle.env", "environment"));
	for (const Diagnostic &d : outcome.findings) std::fprintf(stderr, "  %s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(!outcome.refused && fs::is_regular_file(system_path(project.at("terrain/isle.env"))));
	{
		const std::vector<uint8_t> text = read(project.at("terrain/isle.env"));
		std::istringstream in(std::string(text.begin(), text.end()));
		opennova::env::Config env;
		TEST_EXPECT(opennova::env::load_env(in, env, why) && env.keyframes.size() == 3 && env.name == "isle");
	}
	editor_test::set_missions(project.session, true);
	outcome = editor_test::handle_to_end(project.session,
	        request::create_file("isle1.bms", "mission", {{"terrain", "isle.trn"}, {"environment", "isle.env"}}));
	for (const Diagnostic &d : outcome.findings) std::fprintf(stderr, "  %s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(!outcome.refused && fs::is_regular_file(system_path(project.at("missions/isle1.bms"))));
	return 0;
}

// The blank environment: the writer's template, named after its file, CRLF lines the game parses.
int test_blank_environment() {
	BlankRequest blank;
	blank.logical_name = "dusk.env";
	std::vector<uint8_t> bytes;
	Diagnostic error;
	TEST_EXPECT(make_blank(blank, AssetKind::Environment, bytes, error));
	const std::string text(bytes.begin(), bytes.end());
	TEST_EXPECT(text.find("enviro_name \"dusk\"\r\n") == 0 && text.find("tod_begin") != std::string::npos);
	TEST_EXPECT(find_blank_factory_for_kind(AssetKind::Environment) != nullptr);
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_set_text();
	failures += test_heightmap_depths();
	failures += test_blank_environment();
	failures += test_new_terrain();
	if (failures == 0) std::printf("editor_terrain_import: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
