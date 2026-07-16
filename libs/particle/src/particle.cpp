#include "particle/particle.h"

#include "io/strutil.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <sstream>
#include <utility>

namespace opennova::particle {

const ParticleDef *ParticleFile::find_particle(std::string_view id) const noexcept {
	// Particle-def names resolve case-insensitively like every by-name walk in
	// the effect system: the EFFDEF→PARDEF member resolve compares with _stricmp
	// [orig: CEffectWorld_FindParticleDefByName @ 0x5e41d0 → _stricmp @ 0x5e420c,
	// called from the pdefs resolve @ 0x5e4920].
	for (const ParticleDef &particle : particles) {
		if (strutil::iequals(particle.id, id)) {
			return &particle;
		}
	}
	return nullptr;
}

const TableDef *ParticleFile::find_table(std::string_view id) const noexcept {
	// Table names resolve case-insensitively: the engine's list walk compares
	// with _stricmp — shipped data relies on it (ambfx.ptl authors
	// `green_func = Table11Alt` against `id = table11Alt`)
	// [orig: table find @ 0x5e9540 → _stricmp @ 0x76fdf6].
	for (const TableDef &table : tables) {
		if (strutil::iequals(table.id, id)) {
			return &table;
		}
	}
	return nullptr;
}

namespace {

bool icontains(std::string_view haystack, std::string_view needle) noexcept {
	if (needle.empty()) {
		return true;
	}
	if (needle.size() > haystack.size()) {
		return false;
	}
	for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
		bool ok = true;
		for (std::size_t j = 0; j < needle.size(); ++j) {
			const unsigned char a = static_cast<unsigned char>(haystack[i + j]);
			const unsigned char b = static_cast<unsigned char>(needle[j]);
			if (std::tolower(a) != std::tolower(b)) {
				ok = false;
				break;
			}
		}
		if (ok) {
			return true;
		}
	}
	return false;
}

// [orig: g_ParticleFlagTable @ 0x5ba500 (ParticleEdit_v1_1.exe, cnt dword_5BC2E8=29); JO @ 0x846A18]
// 29 entries in engine table order (the write order). HAZE (idx 9), BELOWH20
// (idx 27), ABOVEH20 (idx 28) added per the ParticleEdit cross-witness grill
// (D5) — fixes the HAZE round-trip drop (fire.ptl) and the GLOBALWIND..
// AMBIENTCOLOR bit-shift. See notes/ida_particle_witness.md.
constexpr std::array<std::pair<const char *, std::uint32_t>, 29> kParticleFlagEntries = {{
	{"NOVISNOUPDATE",        particle_flag::NoVisNoUpdate},
	{"INITIALYCLIP",         particle_flag::InitialClip},
	{"NEVERAGE",             particle_flag::NeverAge},
	{"TOPALIGN",             particle_flag::TopAlign},
	{"ONMYDEATH",            particle_flag::OnMyDeath},
	{"USEPARENTSCALE",       particle_flag::UseParentScale},
	{"USEPARENTCOLOR",       particle_flag::UseParentColor},
	{"USEPARENTALPHA",       particle_flag::UseParentAlpha},
	{"YAWANDPITCH",          particle_flag::YawAndPitch},
	{"HAZE",                 particle_flag::Haze},
	{"GLOBALWIND",           particle_flag::GlobalWind},
	{"FOCALWIND",            particle_flag::FocalWind},
	{"FOCALWINDFORCEAGING",  particle_flag::FocalWindForceAging},
	{"COLLIDEBOUNCE",        particle_flag::CollideBounce},
	{"COLLIDESLIDE",         particle_flag::CollideSlide},
	{"COLLIDEKILL",          particle_flag::CollideKill},
	{"EMITVECTOR",           particle_flag::EmitVector},
	{"POSITIONINTERPOLATE",  particle_flag::PositionInterpolate},
	{"FOREVEREMIT",          particle_flag::ForeverEmit},
	{"POSITIONRELATIVE",     particle_flag::PositionRelative},
	{"USEPARENTROTATIONS",   particle_flag::UseParentRotations},
	{"GFXFLIPRAND",          particle_flag::GfxFlipRand},
	{"CONTROLEDALLIGNMENT",  particle_flag::ControledAlignment},
	{"SIGNEDROTATIONS",      particle_flag::SignedRotations},
	{"ONEFRAME",             particle_flag::OneFrame},
	{"BURSTDISTRIBUTE",      particle_flag::BurstDistribute},
	{"AMBIENTCOLOR",         particle_flag::AmbientColor},
	{"BELOWH20",             particle_flag::BelowH2O},
	{"ABOVEH20",             particle_flag::AboveH2O},
}};

// Engine move table @ 0x848800 — same convention.
constexpr std::array<std::pair<const char *, std::uint32_t>, 5> kMoveEntries = {{
	{"NORMAL",    move_flag::Normal},
	{"GRAVITATE", move_flag::Gravitate},
	{"WANDER",    move_flag::Wander},
	{"BUBBLE",    move_flag::Bubble},
	{"ORBIT",     move_flag::Orbit},
}};

template <std::size_t N>
std::uint32_t parse_flag_table(std::string_view raw,
		const std::array<std::pair<const char *, std::uint32_t>, N> &entries) noexcept {
	std::uint32_t bits = 0;
	for (const auto &entry : entries) {
		if (icontains(raw, entry.first)) {
			bits |= entry.second;
		}
	}
	return bits;
}

template <std::size_t N>
std::string format_flag_table(std::uint32_t bits,
		const std::array<std::pair<const char *, std::uint32_t>, N> &entries) {
	// [orig: FlagTable_BuildString @ 0x428fe0 (ParticleEdit_v1_1.exe); JO sub_5DF9C0 @ 0x5df9c0]
	// DIVERGENCE D1 (see notes/ida_particle_witness.md): the engine seeds the
	// buffer with a LEADING space (*(WORD*)buf = 0x20) before appending names, so
	// the value is " NAME1 NAME2 " and a line reads "flags\t=  NAME1 NAME2 ;" (two
	// spaces after '='). Corpus confirms. We emit no leading space (single space).
	// Engine emits names in table order. FlagTable_BuildString seeds the buffer
	// with a LEADING space, then appends "<name> " per matched bit — so the value
	// is " NAME1 NAME2 " (leading + single-separator + trailing space) and a line
	// reads "flags\t=  NAME1 NAME2 ;" (two spaces after '='). Reproduce that by
	// prefixing each name with a space; gives "" for the (never-written) empty case.
	std::string out;
	for (const auto &entry : entries) {
		if ((bits & entry.second) != 0) {
			out.push_back(' ');
			out.append(entry.first);
		}
	}
	if (!out.empty()) {
		out.push_back(' ');
	}
	return out;
}

} // namespace

const char *blend_mode_name(BlendMode mode) noexcept {
	// Engine writer at 0x5e5414 switch — same order, identical names. Branch
	// for value 4 references off_7DCBA8 ("mod"); we hardcode the literal.
	switch (mode) {
		case BlendMode::Additive: return "additive";
		case BlendMode::Blend:    return "blend";
		case BlendMode::Premult:  return "premult";
		case BlendMode::Bump:     return "bump";
		case BlendMode::Mod:      return "mod";
		case BlendMode::Mod2x:    return "mod2x";
		case BlendMode::Bumpadd:  return "bumpadd";
		case BlendMode::Distort:  return "distort";
	}
	return "blend";
}

BlendMode parse_blend_mode(std::string_view raw) noexcept {
	// [orig: ParseBlendMode @ 0x42c080 (ParticleEdit_v1_1.exe); JO CParticleDefEntry_ParseBlendMode @ 0x5e29f0]
	// ParticleEdit confirms the chained-strstr order exactly (no distort case;
	// distort=7 is a Jointops-era addition). CParticleDefEntry_ParseBlendMode uses chained strstr — first
	// match wins. Order matters because "bump" is a substring of "bumpadd";
	// engine checks "bump" first and would return Bump for "bumpadd". Match
	// the engine exactly so corpus parses identically.
	if (icontains(raw, "additive")) return BlendMode::Additive;
	if (icontains(raw, "blend"))    return BlendMode::Blend;
	if (icontains(raw, "premult"))  return BlendMode::Premult;
	if (icontains(raw, "bump"))     return BlendMode::Bump;
	if (icontains(raw, "mod"))      return BlendMode::Mod;
	if (icontains(raw, "mod2x"))    return BlendMode::Mod2x;     // unreachable per engine; kept for completeness
	if (icontains(raw, "bumpadd"))  return BlendMode::Bumpadd;   // ditto
	if (icontains(raw, "distort"))  return BlendMode::Distort;
	return BlendMode::Blend;
}

std::uint32_t parse_move_bits(std::string_view raw) noexcept {
	return parse_flag_table(raw, kMoveEntries);
}

std::string format_move_bits(std::uint32_t bits) {
	return format_flag_table(bits, kMoveEntries);
}

std::uint32_t parse_particle_flags(std::string_view raw) noexcept {
	return parse_flag_table(raw, kParticleFlagEntries);
}

std::string format_particle_flags(std::uint32_t bits) {
	return format_flag_table(bits, kParticleFlagEntries);
}

const std::vector<std::pair<std::string, std::uint32_t>> &particle_flag_entries() {
	static const std::vector<std::pair<std::string, std::uint32_t>> entries = [] {
		std::vector<std::pair<std::string, std::uint32_t>> v;
		v.reserve(kParticleFlagEntries.size());
		for (const auto &e : kParticleFlagEntries) {
			v.emplace_back(e.first, e.second);
		}
		return v;
	}();
	return entries;
}

const std::vector<std::pair<std::string, std::uint32_t>> &move_flag_entries() {
	static const std::vector<std::pair<std::string, std::uint32_t>> entries = [] {
		std::vector<std::pair<std::string, std::uint32_t>> v;
		v.reserve(kMoveEntries.size());
		for (const auto &e : kMoveEntries) {
			v.emplace_back(e.first, e.second);
		}
		return v;
	}();
	return entries;
}

const std::vector<std::string> &blend_mode_names() {
	static const std::vector<std::string> names = [] {
		std::vector<std::string> v;
		for (int i = 0; i <= 7; ++i) {
			v.emplace_back(blend_mode_name(static_cast<BlendMode>(i)));
		}
		return v;
	}();
	return names;
}

void bake_curve_lut(const TableDef &table, bool reverse, bool inverse,
		std::array<std::uint8_t, 256> &out) noexcept {
	// Engine: CEffectDef_ResolveTblDefReference @ 0x5e9630. The 32 × 8 byte
	// rows are read row-major into a flat 256-byte buffer (TableDef +328 in
	// the engine heap layout). We mirror that by walking row*8 + col.
	std::array<std::uint8_t, 256> flat{};
	const std::size_t row_count = table.rows.size();
	for (std::size_t r = 0; r < 32 && r < row_count; ++r) {
		for (std::size_t c = 0; c < 8; ++c) {
			flat[r * 8 + c] = table.rows[r][c];
		}
	}
	for (std::size_t i = 0; i < 256; ++i) {
		const std::size_t src = reverse ? (255 - i) : i;
		const std::uint8_t v = flat[src];
		out[i] = inverse ? static_cast<std::uint8_t>(255 - v) : v;
	}
}

namespace {

void bake_one_curve(CurveRef &curve, const std::vector<TableDef> &tables) noexcept {
	curve.baked = false;
	if (!curve.present || curve.name.empty()) {
		return;
	}
	// The engine's resolve walks the table list comparing names with _stricmp
	// [orig: CEffectDef_ResolveTblDefReference @ 0x5e9630 → table find
	// @ 0x5e9540 → _stricmp @ 0x76fdf6]. Shipped data depends on the fold:
	// ambfx.ptl's Wood_AmbFB authors `green_func = Table11Alt` against
	// `id = table11Alt` — an exact compare leaves green unbaked (constant 255)
	// while red/alpha fade, tinting aging fire sprites green.
	// TableDef+0x248 is the transform mask (inverse=1, reverse=2), not an
	// owner. An unmodified reference returns the FIRST base match. A modified
	// reference scans every base match, remembers the LAST one, then clones
	// and transforms it [orig: @ 0x5e95b8..0x5e960b; transform @ 0x5e2700].
	const bool modified = curve.reverse || curve.inverse;
	const TableDef *base_match = nullptr;
	for (const TableDef &table : tables) {
		if (strutil::iequals(table.id, curve.name)) {
			if (!modified) {
				bake_curve_lut(table, false, false, curve.baked_lut);
				curve.baked = true;
				return;
			}
			base_match = &table;
		}
	}
	if (base_match != nullptr) {
		bake_curve_lut(*base_match, curve.reverse, curve.inverse,
				curve.baked_lut);
		curve.baked = true;
	}
}

} // namespace

void bake_graphic_uv_rects(GraphicLayer &layer) noexcept {
	const int frames = std::clamp(layer.flip_frames, 1, kMaxParticleFlipFrames);
	layer.baked_uv_rects.assign(static_cast<std::size_t>(frames), UvRect{});
	const float inv_frames = 1.0f / static_cast<float>(frames);
	for (int i = 0; i < frames; ++i) {
		UvRect &rect = layer.baked_uv_rects[static_cast<std::size_t>(i)];
		rect.u_min = static_cast<float>(i) * inv_frames;
		rect.u_max = static_cast<float>(i + 1) * inv_frames;
		rect.v_min = 0.0f;
		rect.v_max = 1.0f;
		rect.inset = 0.0f;
	}
}

void bake_particle_def_curves(ParticleDef &def,
		const std::vector<TableDef> &tables) noexcept {
	bake_one_curve(def.scale_func, tables);
	bake_one_curve(def.alpha_func, tables);
	bake_one_curve(def.red_func, tables);
	bake_one_curve(def.green_func, tables);
	bake_one_curve(def.blue_func, tables);
	bake_one_curve(def.emit_rate_func, tables);
	for (GraphicLayer &layer : def.graphics) {
		bake_one_curve(layer.scale_func, tables);
		bake_one_curve(layer.alpha_func, tables);
		bake_one_curve(layer.red_func, tables);
		bake_one_curve(layer.green_func, tables);
		bake_one_curve(layer.blue_func, tables);
		bake_graphic_uv_rects(layer);
	}
}

AtlasLayout bake_atlas_layout(ParticleDef &def,
		const std::array<AtlasInputSize, 4> &sizes,
		AtlasBakeOptions options) noexcept {
	AtlasLayout layout{};
	const int gutter = std::max(options.gutter_pixels, 0);
	int total_width = 0;
	int max_height = 0;
	for (std::size_t i = 0; i < def.graphics.size(); ++i) {
		const bool present_for_atlas = def.graphics[i].present
				&& sizes[i].width > 0 && sizes[i].height > 0;
		if (!present_for_atlas) {
			continue;
		}
		layout.layer_x_offset[i] = total_width + gutter;
		total_width += sizes[i].width + gutter * 2;
		const int packed_height = sizes[i].height + gutter * 2;
		if (packed_height > max_height) {
			max_height = packed_height;
		}
	}
	layout.atlas_width = total_width;
	layout.atlas_height = max_height;

	for (std::size_t i = 0; i < def.graphics.size(); ++i) {
		GraphicLayer &layer = def.graphics[i];
		const bool present_for_atlas = layer.present
				&& sizes[i].width > 0 && sizes[i].height > 0
				&& total_width > 0 && max_height > 0;
		if (!present_for_atlas) {
			// Reset rects to horizontal-strip defaults so renderer's
			// fallback path (no atlas / missing texture) keeps working.
			bake_graphic_uv_rects(layer);
			continue;
		}

		const int frames = std::clamp(layer.flip_frames, 1, kMaxParticleFlipFrames);
		layer.baked_uv_rects.assign(static_cast<std::size_t>(frames), UvRect{});

		const float u_min_layer = static_cast<float>(layout.layer_x_offset[i])
				/ static_cast<float>(total_width);
		const float u_max_layer = static_cast<float>(layout.layer_x_offset[i] + sizes[i].width)
				/ static_cast<float>(total_width);
		const float v_min_layer = static_cast<float>(gutter)
				/ static_cast<float>(max_height);
		const float v_max_layer = static_cast<float>(gutter + sizes[i].height)
				/ static_cast<float>(max_height);
		const float frame_width = (u_max_layer - u_min_layer)
				/ static_cast<float>(frames);

		for (int j = 0; j < frames; ++j) {
			UvRect &rect = layer.baked_uv_rects[static_cast<std::size_t>(j)];
			rect.u_min = u_min_layer + static_cast<float>(j) * frame_width;
			rect.u_max = u_min_layer + static_cast<float>(j + 1) * frame_width;
			rect.v_min = v_min_layer;
			rect.v_max = v_max_layer;
			rect.inset = 0.0f;
		}
	}

	return layout;
}

} // namespace opennova::particle
