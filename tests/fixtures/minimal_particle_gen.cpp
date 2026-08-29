// Generator + guard for fixtures/particle/synth_*.ptl: five synthetic particle
// files written by save_particles (the writer shaped after
// CParticleDef_SaveToFile @ 0x5e4d70 et al.) from integer/float data, in the
// shapes the focused particle tests key on:
//
//   synth_smallest.ptl       one [tabledef] and nothing else
//   synth_table_handles.ptl  a [tabledef] plus its [tabledef_edithandles]
//   synth_multi_section.ptl  seven [effectdef]s over one "blank" particle
//                            (an empty graphic texture) and one textured one
//   synth_multi_layer.ptl    four effects; a three-layer particle with per-layer
//                            colour overrides and curves, and a one-layer one
//   synth_minimal_effect.ptl the Buildup effect over the "Buildup dots"
//                            particle (a gravitating top-aligned burst) that
//                            the authored gorehit.ptu also references
//
// No retail file is carried: the shipped .ptl corpus is the OPENNOVA_JO_ASSETS
// leg of particle_smoke_all_fixtures.
//
// Default: rebuild in memory and byte-compare the committed files. `--write`
// (re)writes them.
#include "common/test_paths.h"

#include <formats/particle/parser.h>
#include <formats/particle/particle.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "common/file_io.h"

using namespace opennova::particle;

namespace {

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

Color3 rgb(int r, int g, int b) {
	return {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b)};
}

CurveRef curve(const char *name, bool reverse = false, bool inverse = false) {
	CurveRef c;
	c.name = name;
	c.reverse = reverse;
	c.inverse = inverse;
	c.present = true;
	return c;
}

// A 32 x 8 table: row i, byte j = 255 - 8 i - j, floored at 1 (a descending
// ramp whose corners the smallest test pins: 255/248 on the first row, 7/1 on
// the last).
TableDef ramp_table(const char *id) {
	TableDef t;
	t.id = id;
	for (int i = 0; i < 32; ++i) {
		std::array<std::uint8_t, 8> row{};
		for (int j = 0; j < 8; ++j) {
			const int v = 255 - 8 * i - j;
			row[static_cast<size_t>(j)] = static_cast<std::uint8_t>(v < 1 ? 1 : v);
		}
		t.rows.push_back(row);
	}
	return t;
}

// A 32 x 8 table: a rising ramp (row i, byte j = 8 i + j, capped at 255).
TableDef rise_table(const char *id) {
	TableDef t;
	t.id = id;
	for (int i = 0; i < 32; ++i) {
		std::array<std::uint8_t, 8> row{};
		for (int j = 0; j < 8; ++j) {
			const int v = 8 * i + j;
			row[static_cast<size_t>(j)] = static_cast<std::uint8_t>(v > 255 ? 255 : v);
		}
		t.rows.push_back(row);
	}
	return t;
}

EffectDef effect(const char *id, std::vector<std::string> pdefs) {
	EffectDef e;
	e.id = id;
	e.pdefs = std::move(pdefs);
	return e;
}

// A particle with the retail-shaped baseline every shipped particledef
// carries (the writer emits every key): one second of emission, a short life,
// grey colours, no motion.
ParticleDef base_particle(const char *id) {
	ParticleDef p;
	p.id = id;
	p.flags = 0;
	p.move = move_flag::Normal;
	p.emit_dur = 1.0f;
	p.emit_rate = 20.0f;
	p.emit_burst = 1;
	p.age = 0.5f;
	p.alpha = 1.0f;
	p.scale = 1.0f;
	p.color1 = p.color2 = p.color3 = p.color4 = rgb(127, 127, 127);
	p.spread = 30.0f;
	return p;
}

GraphicLayer &layer(ParticleDef &p, int index, const char *texture, BlendMode mode) {
	GraphicLayer &g = p.graphics[static_cast<size_t>(index - 1)];
	g.index = index;
	g.present = true;
	g.texture = texture;
	g.blend_mode = mode;
	g.blend_mode_raw = blend_mode_name(mode);
	g.flip_frames = 1;
	g.flip_rate = 8;
	g.alpha = p.alpha;
	g.scale = p.scale;
	g.scale_adj = p.scale_adj;
	g.color1 = p.color1;
	g.color2 = p.color2;
	g.color3 = p.color3;
	g.color4 = p.color4;
	return g;
}

ParticleFile make_smallest() {
	ParticleFile f;
	f.tables.push_back(ramp_table("synth_ramp"));
	return f;
}

ParticleFile make_table_handles() {
	ParticleFile f;
	f.tables.push_back(rise_table("synth_wake"));
	TableEditHandles h;
	h.table_id = "synth_wake";
	h.handlecount = 0;
	h.tightness = 0;
	f.table_handles.push_back(h);
	return f;
}

ParticleFile make_multi_section() {
	ParticleFile f;
	// "stockeffect" is the literal name the engine clones for an unknown effect
	// [orig: CEffectWorld_InternEffectHandle @ 0x5f7310], so the catalog carries it.
	static const char *const kEffects[] = {"stockeffect", "synth_burst", "synth_ring", "synth_dust",
	                                       "synth_spray", "synth_wake",  "synth_smoke"};
	for (const char *id : kEffects) f.effects.push_back(effect(id, {"blank"}));
	// "blank": a declared graphic with no texture, the default blend.
	ParticleDef blank = base_particle("blank");
	layer(blank, 1, "", BlendMode::Blend);
	f.particles.push_back(blank);
	ParticleDef flare = base_particle("synth_flare");
	flare.age = 0.25f;
	flare.color1 = flare.color2 = flare.color3 = flare.color4 = rgb(255, 200, 120);
	layer(flare, 1, "flare.tga", BlendMode::Additive);
	f.particles.push_back(flare);
	return f;
}

ParticleFile make_multi_layer() {
	ParticleFile f;
	f.effects.push_back(effect("synth_dirt_hit", {"synth_fol_puf", "synth_flash"}));
	f.effects.push_back(effect("synth_fol_hit", {"synth_fol_puf"}));
	f.effects.push_back(effect("synth_metal_hit", {"synth_flash", "synth_spark"}));
	f.effects.push_back(effect("synth_wood_hit", {"synth_fol_puf", "synth_spark"}));

	// Three layers, per-layer colour overrides on the first, two curves.
	ParticleDef puf = base_particle("synth_fol_puf");
	puf.flags = particle_flag::EmitVector | particle_flag::AmbientColor;
	puf.move = move_flag::Normal;
	puf.emit_dur = 0.1f;
	puf.emit_rate = 30.0f;
	puf.emit_burst = 3;
	puf.age = 1.5f;
	puf.age_adj = 0.5f;
	puf.scale = 3.0f;
	puf.scale_adj = 1.0f;
	puf.alpha_func = curve("synth_fade");
	puf.scale_func = curve("synth_grow", true);
	puf.speed = 4.0f;
	puf.speed_adj = 2.0f;
	puf.gravity = 40.0f;
	puf.spread = 45.0f;
	puf.color1 = rgb(180, 150, 110);
	puf.color2 = rgb(160, 130, 100);
	puf.color3 = rgb(140, 110, 90);
	puf.color4 = rgb(120, 100, 80);
	GraphicLayer &g1 = layer(puf, 1, "puf_a.tga", BlendMode::Blend);
	g1.color1 = rgb(200, 160, 120);
	g1.color_overrides_set = true;
	g1.alpha_func = curve("synth_fade");
	g1.scale_func = curve("synth_grow", true);
	layer(puf, 2, "puf_b.tga", BlendMode::Blend);
	GraphicLayer &g3 = layer(puf, 3, "PufC.tga", BlendMode::Blend);
	g3.alpha = 0.5f;
	f.particles.push_back(puf);

	ParticleDef flash = base_particle("synth_flash");
	flash.emit_dur = 0.05f;
	flash.emit_rate = 1.0f;
	flash.age = 0.1f;
	flash.scale = 2.0f;
	flash.color1 = flash.color2 = flash.color3 = flash.color4 = rgb(255, 240, 200);
	layer(flash, 1, "flash.tga", BlendMode::Additive);
	f.particles.push_back(flash);

	ParticleDef spark = base_particle("synth_spark");
	spark.move = move_flag::Gravitate;
	spark.emit_dur = 0.05f;
	spark.emit_rate = 60.0f;
	spark.age = 0.8f;
	spark.speed = 12.0f;
	spark.speed_adj = 6.0f;
	spark.gravity = 300.0f;
	spark.elastic = 0.4f;
	spark.spread = 60.0f;
	spark.scale = 0.25f;
	spark.color1 = spark.color2 = spark.color3 = spark.color4 = rgb(255, 180, 60);
	layer(spark, 1, "spark.tga", BlendMode::Additive);
	f.particles.push_back(spark);

	f.tables.push_back(ramp_table("synth_fade"));
	f.tables.push_back(rise_table("synth_grow"));
	return f;
}

ParticleFile make_minimal_effect() {
	ParticleFile f;
	f.effects.push_back(effect("Buildup", {"Buildup dots"}));
	// A top-aligned gravitating burst from a 10 x 10 box, curves on scale and
	// the colour channels, one additive layer at twice the base scale.
	ParticleDef dots = base_particle("Buildup dots");
	dots.flags = particle_flag::TopAlign;
	dots.move = move_flag::Gravitate;
	dots.emit_dur = 60.0f;
	dots.emit_rate = 400.0f;
	dots.emit_burst = 1;
	dots.emit_shape = 2;
	dots.emit_shape_size = {10.0f, 10.0f, 10.0f};
	dots.emit_shape_size_skip = {10.0f, 10.0f, 10.0f};
	dots.age = 0.6f;
	dots.scale = 2.0f;
	dots.scale_func = curve("synth_grow", true);
	dots.red_func = curve("synth_red");
	dots.green_func = curve("synth_green");
	dots.blue_func = curve("synth_blue");
	dots.gravity = 600.0f;
	dots.spread = 180.0f;
	GraphicLayer &g1 = layer(dots, 1, "spark.tga", BlendMode::Additive);
	g1.scale_func = curve("synth_grow", true);
	g1.red_func = curve("synth_red");
	g1.green_func = curve("synth_green");
	g1.blue_func = curve("synth_blue");
	f.particles.push_back(dots);
	return f;
}

// save_particles' text for a file, re-parsed once and re-written to prove the
// writer's output is its own fixed point.
bool build(const ParticleFile &file, std::vector<uint8_t> &bytes, std::string &err) {
	std::ostringstream out;
	if (!save_particles(out, file, err)) return false;
	const std::string text = out.str();
	std::istringstream in(text);
	ParticleFile back;
	ParseError perr;
	if (!load_particles(in, back, perr)) {
		err = "re-parse: line " + std::to_string(perr.line) + ": " + perr.message;
		return false;
	}
	if (back.effects.size() != file.effects.size() || back.particles.size() != file.particles.size() ||
	    back.tables.size() != file.tables.size() || back.table_handles.size() != file.table_handles.size()) {
		err = "the re-parsed file lost a section";
		return false;
	}
	std::ostringstream again;
	if (!save_particles(again, back, err)) return false;
	if (again.str() != text) {
		err = "save(load(save(file))) differs from save(file)";
		return false;
	}
	bytes.assign(text.begin(), text.end());
	return true;
}

using test_io::read_file;

int guard(const std::string &path, const ParticleFile &file, bool write_mode) {
	std::vector<uint8_t> bytes;
	std::string err;
	if (!expect(build(file, bytes, err), (path + ": " + err).c_str())) return 1;
	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		if (!expect(static_cast<bool>(o), ("cannot open for writing: " + path).c_str())) return 1;
		o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
		return 0;
	}
	std::vector<uint8_t> committed;
	if (!expect(read_file(path, committed), ("committed file missing; run with --write: " + path).c_str())) return 1;
	static const char kLfsSentinel[] = "version https://git-lfs";
	if (committed.size() >= sizeof(kLfsSentinel) - 1 &&
	    std::memcmp(committed.data(), kLfsSentinel, sizeof(kLfsSentinel) - 1) == 0) {
		std::printf("[skip] %s is an unpulled LFS pointer\n", path.c_str());
		return 0;
	}
	return expect(committed == bytes, ("differs from the generator output; regenerate with --write: " + path).c_str())
	           ? 0
	           : 1;
}

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string dir = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/particle";
	int failures = 0;
	failures += guard(dir + "/synth_smallest.ptl", make_smallest(), write_mode);
	failures += guard(dir + "/synth_table_handles.ptl", make_table_handles(), write_mode);
	failures += guard(dir + "/synth_multi_section.ptl", make_multi_section(), write_mode);
	failures += guard(dir + "/synth_multi_layer.ptl", make_multi_layer(), write_mode);
	failures += guard(dir + "/synth_minimal_effect.ptl", make_minimal_effect(), write_mode);
	if (failures == 0 && !write_mode) std::printf("OK: fixtures/particle synth_*.ptl byte-reproducible\n");
	return failures == 0 ? 0 : 1;
}
