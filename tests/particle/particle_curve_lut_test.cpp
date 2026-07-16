// Curve LUT bake parity — exercises bake_curve_lut + bake_particle_def_curves.
// Engine reference: CEffectDef_ResolveTblDefReference @ 0x5e9630, which writes
// `entry+68 = TableDefByName + 328`. The resolved LUT is the tabledef's 32 × 8
// byte buffer read row-major as a flat 256-byte array. This test pins the
// row-major layout and the reverse/inverse modifiers from
// CParticleDef_ParseProperties (`scale_func @ 0x5eafdd`, etc.) which set bits
// 0x02 (reverse) / 0x01 (inverse) on each curve.

#include <particle/parser.h>
#include <particle/particle.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

opennova::particle::TableDef ramp_table(const char *id) {
	opennova::particle::TableDef table;
	table.id = id;
	table.rows.reserve(32);
	for (int r = 0; r < 32; ++r) {
		std::array<std::uint8_t, 8> row{};
		for (int c = 0; c < 8; ++c) {
			row[static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(r * 8 + c);
		}
		table.rows.push_back(row);
	}
	return table;
}

opennova::particle::TableDef constant_table(const char *id,
		std::uint8_t value) {
	opennova::particle::TableDef table;
	table.id = id;
	table.rows.assign(32, std::array<std::uint8_t, 8>{});
	for (auto &row : table.rows) {
		row.fill(value);
	}
	return table;
}

bool test_row_major_flatten() {
	// rows[r][c] = r*8 + c → flat lut[i] = i. This pins the row-major order
	// the engine uses (CEffectDef_ResolveTblDefReference @ 0x5e9630 stores
	// `TableDefByName + 328` directly; renderer reads `*(uint8_t*)(lut + i)`).
	using namespace opennova::particle;
	const TableDef table = ramp_table("ramp");
	std::array<std::uint8_t, 256> lut{};
	bake_curve_lut(table, false, false, lut);
	for (int i = 0; i < 256; ++i) {
		if (!expect(lut[i] == static_cast<std::uint8_t>(i), "row-major identity ramp")) {
			std::fprintf(stderr, "  i=%d got=%u\n", i, static_cast<unsigned>(lut[i]));
			return false;
		}
	}
	return true;
}

bool test_reverse() {
	// "reverse" (curve.reverse, parser bit 0x02) reads the 256 source bytes in
	// reverse order: lut[i] = src[255 - i]. Engine: scale_func "reverse" trail.
	using namespace opennova::particle;
	const TableDef table = ramp_table("ramp");
	std::array<std::uint8_t, 256> lut{};
	bake_curve_lut(table, true, false, lut);
	for (int i = 0; i < 256; ++i) {
		const std::uint8_t want = static_cast<std::uint8_t>(255 - i);
		if (!expect(lut[i] == want, "reverse swap")) {
			std::fprintf(stderr, "  i=%d got=%u want=%u\n", i,
					static_cast<unsigned>(lut[i]), static_cast<unsigned>(want));
			return false;
		}
	}
	return true;
}

bool test_inverse() {
	// "inverse" (curve.inverse, parser bit 0x01) flips byte values: lut[i] =
	// 255 - src[i]. Without reverse, source order is preserved.
	using namespace opennova::particle;
	const TableDef table = ramp_table("ramp");
	std::array<std::uint8_t, 256> lut{};
	bake_curve_lut(table, false, true, lut);
	for (int i = 0; i < 256; ++i) {
		const std::uint8_t want = static_cast<std::uint8_t>(255 - i);
		if (!expect(lut[i] == want, "inverse byte flip")) {
			std::fprintf(stderr, "  i=%d got=%u want=%u\n", i,
					static_cast<unsigned>(lut[i]), static_cast<unsigned>(want));
			return false;
		}
	}
	return true;
}

bool test_reverse_and_inverse_compose() {
	// Both modifiers: read in reverse AND flip bytes. With identity ramp, the
	// composed result is `255 - (255 - i) = i`.
	using namespace opennova::particle;
	const TableDef table = ramp_table("ramp");
	std::array<std::uint8_t, 256> lut{};
	bake_curve_lut(table, true, true, lut);
	for (int i = 0; i < 256; ++i) {
		if (!expect(lut[i] == static_cast<std::uint8_t>(i), "reverse + inverse composes to identity on ramp")) {
			std::fprintf(stderr, "  i=%d got=%u\n", i, static_cast<unsigned>(lut[i]));
			return false;
		}
	}
	return true;
}

bool test_short_table_zero_pad() {
	// A tabledef with fewer than 32 rows zero-pads — corpus invariant is 32×8
	// (smoke test confirms 77/77) but the bake must not undef-read.
	using namespace opennova::particle;
	TableDef table;
	table.id = "short";
	table.rows.push_back({{1, 2, 3, 4, 5, 6, 7, 8}});
	std::array<std::uint8_t, 256> lut{};
	bake_curve_lut(table, false, false, lut);
	for (int i = 0; i < 8; ++i) {
		if (!expect(lut[i] == static_cast<std::uint8_t>(i + 1), "first row preserved")) return false;
	}
	for (int i = 8; i < 256; ++i) {
		if (!expect(lut[i] == 0, "rows beyond row_count read as zero")) return false;
	}
	return true;
}

bool test_table_rows_follow_tl_indices_and_last_write_wins() {
	const char *source = R"PTL(
[tabledef]
{
	id = shuffled;
	tl2 = 2, 2, 2, 2, 2, 2, 2, 2;
	tl1 = 1, 1, 1, 1, 1, 1, 1, 1;
	tl2 = 9, 9, 9, 9, 9, 9, 9, 9;
	tl32 = 32, 32, 32, 32, 32, 32, 32, 32;
	tl0 = 77, 77, 77, 77, 77, 77, 77, 77;
	tlfoo = 88, 88, 88, 88, 88, 88, 88, 88;
}
)PTL";
	std::istringstream stream(source);
	opennova::particle::ParticleFile file;
	opennova::particle::ParseError error;
	if (!opennova::particle::load_particles(stream, file, error)) return false;
	if (!expect(file.tables.size() == 1, "shuffled table parses")) return false;
	const auto &rows = file.tables[0].rows;
	if (!expect(rows.size() == 32, "tl32 establishes indexed row 32")) return false;
	if (!expect(rows[0][0] == 1, "tl1 maps to row zero")) return false;
	if (!expect(rows[1][0] == 9, "duplicate tl2 is last-wins")) return false;
	if (!expect(rows[2][0] == 0, "missing tl3 stays zero")) return false;
	return expect(rows[31][0] == 32, "tl32 maps to final row");
}

bool test_def_bake_resolves_named_curves() {
	// bake_particle_def_curves walks all 5 particle-level + 5 per-graphic
	// CurveRefs, looks up each by name in the supplied tables vector, sets
	// `baked = true` when resolved. CurveRefs that have `present == false`
	// stay un-baked even if a name happens to match.
	using namespace opennova::particle;
	std::vector<TableDef> tables;
	tables.push_back(ramp_table("alpha_ramp"));
	tables.push_back(ramp_table("scale_ramp"));

	ParticleDef def;
	def.id = "test";
	def.alpha_func.name = "alpha_ramp";
	def.alpha_func.present = true;
	def.scale_func.name = "scale_ramp";
	def.scale_func.present = true;
	def.scale_func.reverse = true;
	def.red_func.name = "missing_table";
	def.red_func.present = true;
	def.green_func.name = "alpha_ramp";   // name matches but not authored
	def.green_func.present = false;

	bake_particle_def_curves(def, tables);

	if (!expect(def.alpha_func.baked, "alpha_func baked when name resolves")) return false;
	if (!expect(def.alpha_func.baked_lut[0] == 0 && def.alpha_func.baked_lut[255] == 255,
			"alpha_func bake matches identity ramp")) return false;
	if (!expect(def.scale_func.baked, "scale_func baked")) return false;
	if (!expect(def.scale_func.baked_lut[0] == 255 && def.scale_func.baked_lut[255] == 0,
			"scale_func reverse modifier applied")) return false;
	if (!expect(!def.red_func.baked, "red_func not baked when name does not resolve")) return false;
	if (!expect(!def.green_func.baked, "green_func not baked when present == false")) return false;
	return true;
}

bool test_def_bake_resolves_names_case_insensitively() {
	// The engine resolves table names with _stricmp [orig:
	// CEffectDef_ResolveTblDefReference @ 0x5e9630 → table find @ 0x5e9540 →
	// _stricmp @ 0x76fdf6], and shipped data relies on it: ambfx.ptl's
	// Wood_AmbFB (the 00TR* fire-barrel flame) authors
	// `green_func = Table11Alt` against `id = table11Alt`. An exact compare
	// leaves green unbaked (constant 255) while red/alpha fade with the
	// resolved table — aging flame sprites tint green.
	using namespace opennova::particle;
	std::vector<TableDef> tables;
	tables.push_back(ramp_table("table11Alt"));

	ParticleDef def;
	def.id = "Wood_AmbFB";
	def.red_func.name = "table11Alt";
	def.red_func.present = true;
	def.green_func.name = "Table11Alt";
	def.green_func.present = true;
	def.blue_func.name = "TABLE11ALT";
	def.blue_func.present = true;

	bake_particle_def_curves(def, tables);

	if (!expect(def.red_func.baked, "exact-case name resolves")) return false;
	if (!expect(def.green_func.baked, "mixed-case name resolves (Table11Alt)")) return false;
	if (!expect(def.blue_func.baked, "upper-case name resolves (TABLE11ALT)")) return false;
	if (!expect(def.green_func.baked_lut == def.red_func.baked_lut,
			"case-folded resolve bakes the same LUT")) return false;

	ParticleFile file;
	file.tables = tables;
	if (!expect(file.find_table("tAbLe11aLt") != nullptr,
			"ParticleFile::find_table folds case")) return false;
	return true;
}

bool test_duplicate_selection_matches_retail_transform_cache() {
	// CParticleManager_FindTableDefByName @ 0x5e9540 returns the first base
	// table for flags=0. For inverse/reverse flags it remembers the last base
	// match, clones that table, and applies the transform @ 0x5e2700.
	using namespace opennova::particle;
	std::vector<TableDef> tables;
	tables.push_back(constant_table("Duplicate", 17));
	tables.push_back(ramp_table("duplicate"));

	ParticleDef def;
	def.alpha_func = {"DUPLICATE", false, false, true};
	def.red_func = {"Duplicate", true, false, true};
	def.green_func = {"duplicate", false, true, true};
	def.blue_func = {"duplicate", true, true, true};
	bake_particle_def_curves(def, tables);

	if (!expect(def.alpha_func.baked_lut[0] == 17 &&
			def.alpha_func.baked_lut[255] == 17,
			"unmodified duplicate resolves the first base table")) return false;
	if (!expect(def.red_func.baked_lut[0] == 255 &&
			def.red_func.baked_lut[255] == 0,
			"reverse duplicate transforms the last base table")) return false;
	if (!expect(def.green_func.baked_lut[0] == 255 &&
			def.green_func.baked_lut[255] == 0,
			"inverse duplicate transforms the last base table")) return false;
	return expect(def.blue_func.baked_lut[0] == 0 &&
			def.blue_func.baked_lut[255] == 255,
			"combined modifiers transform the last base table");
}

bool test_uv_rect_bake_horizontal_strip_default() {
	// `bake_graphic_uv_rects` fills `baked_uv_rects` with horizontal-strip
	// UVs: frame N spans u in [N/count, (N+1)/count], v in [0, 1]. This
	// matches the renderer's prior on-the-fly math so existing rendering
	// is regression-safe. Engine equivalent: rects from the atlas-bake at
	// graphic+724.
	using namespace opennova::particle;
	GraphicLayer layer;
	layer.flip_frames = 4;
	bake_graphic_uv_rects(layer);

	if (!expect(layer.baked_uv_rects.size() == 4, "rect count matches flip_frames")) {
		std::fprintf(stderr, "  got %zu rects\n", layer.baked_uv_rects.size());
		return false;
	}
	for (int i = 0; i < 4; ++i) {
		const UvRect &rect = layer.baked_uv_rects[static_cast<std::size_t>(i)];
		const float expected_u_min = static_cast<float>(i) * 0.25f;
		const float expected_u_max = static_cast<float>(i + 1) * 0.25f;
		if (std::fabs(rect.u_min - expected_u_min) > 0.001f ||
				std::fabs(rect.u_max - expected_u_max) > 0.001f) {
			std::fprintf(stderr, "FAIL: frame %d u=(%f, %f) want=(%f, %f)\n",
					i, rect.u_min, rect.u_max, expected_u_min, expected_u_max);
			return false;
		}
		if (std::fabs(rect.v_min) > 0.001f || std::fabs(rect.v_max - 1.0f) > 0.001f) {
			std::fprintf(stderr, "FAIL: frame %d v=(%f, %f) want=(0, 1)\n",
					i, rect.v_min, rect.v_max);
			return false;
		}
		if (std::fabs(rect.inset) > 0.001f) {
			std::fprintf(stderr, "FAIL: frame %d inset=%f want 0\n", i, rect.inset);
			return false;
		}
	}
	return true;
}

bool test_uv_rect_bake_single_frame_full_quad() {
	// flip_frames = 1 => one rect spanning the full texture (0,0)-(1,1).
	using namespace opennova::particle;
	GraphicLayer layer;
	layer.flip_frames = 1;
	bake_graphic_uv_rects(layer);

	if (!expect(layer.baked_uv_rects.size() == 1, "single-frame layer bakes one rect")) return false;
	const UvRect &rect = layer.baked_uv_rects[0];
	if (!expect(std::fabs(rect.u_min) < 0.001f && std::fabs(rect.u_max - 1.0f) < 0.001f,
			"single frame spans u in [0, 1]")) return false;
	if (!expect(std::fabs(rect.v_min) < 0.001f && std::fabs(rect.v_max - 1.0f) < 0.001f,
			"single frame spans v in [0, 1]")) return false;
	return true;
}

bool test_uv_rect_bake_caps_programmatic_frame_count() {
	using namespace opennova::particle;
	GraphicLayer layer;
	layer.flip_frames = kMaxParticleFlipFrames + 100;
	bake_graphic_uv_rects(layer);

	return expect(layer.baked_uv_rects.size() ==
			static_cast<std::size_t>(kMaxParticleFlipFrames),
			"UV bake caps programmatic flip_frames at the shared runtime limit");
}

bool test_uv_rect_bake_called_from_def_bake() {
	// `bake_particle_def_curves` calls `bake_graphic_uv_rects` for every
	// graphic layer — verify the rects populate alongside the curve LUTs.
	using namespace opennova::particle;
	std::vector<TableDef> tables;
	ParticleDef def;
	def.graphics[0].present = true;
	def.graphics[0].flip_frames = 3;
	def.graphics[2].present = true;
	def.graphics[2].flip_frames = 8;

	bake_particle_def_curves(def, tables);

	if (!expect(def.graphics[0].baked_uv_rects.size() == 3,
			"graphic[0] baked 3 rects")) return false;
	if (!expect(def.graphics[2].baked_uv_rects.size() == 8,
			"graphic[2] baked 8 rects")) return false;
	// Frame indices fall in expected u-band: graphic[0] frame 1 → u in [0.333, 0.667).
	const UvRect &g0_f1 = def.graphics[0].baked_uv_rects[1];
	if (!expect(std::fabs(g0_f1.u_min - (1.0f / 3.0f)) < 0.001f,
			"graphic[0] frame 1 u_min ≈ 1/3")) return false;
	return true;
}

bool test_def_bake_handles_graphic_layers() {
	using namespace opennova::particle;
	std::vector<TableDef> tables;
	tables.push_back(ramp_table("graph_alpha"));

	ParticleDef def;
	def.graphics[1].present = true;
	def.graphics[1].alpha_func.name = "graph_alpha";
	def.graphics[1].alpha_func.present = true;
	def.graphics[1].alpha_func.inverse = true;

	bake_particle_def_curves(def, tables);
	if (!expect(def.graphics[1].alpha_func.baked, "per-graphic curve resolved")) return false;
	if (!expect(def.graphics[1].alpha_func.baked_lut[0] == 255, "inverse applied to graphic curve")) return false;
	return true;
}

} // namespace

int main() {
	int failures = 0;
	if (!test_row_major_flatten())              ++failures;
	if (!test_reverse())                        ++failures;
	if (!test_inverse())                        ++failures;
	if (!test_reverse_and_inverse_compose())    ++failures;
	if (!test_short_table_zero_pad())           ++failures;
	if (!test_table_rows_follow_tl_indices_and_last_write_wins()) ++failures;
	if (!test_def_bake_resolves_named_curves()) ++failures;
	if (!test_def_bake_resolves_names_case_insensitively()) ++failures;
	if (!test_duplicate_selection_matches_retail_transform_cache()) ++failures;
	if (!test_def_bake_handles_graphic_layers()) ++failures;
	if (!test_uv_rect_bake_horizontal_strip_default()) ++failures;
	if (!test_uv_rect_bake_single_frame_full_quad())  ++failures;
	if (!test_uv_rect_bake_caps_programmatic_frame_count()) ++failures;
	if (!test_uv_rect_bake_called_from_def_bake())    ++failures;
	if (failures != 0) {
		std::fprintf(stderr, "%d test(s) failed\n", failures);
		return 1;
	}
	return 0;
}
