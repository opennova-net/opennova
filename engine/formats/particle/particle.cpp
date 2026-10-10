#include <formats/particle/particle.h>

#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace opennova::particle {

const EffectDef *ParticleFile::find_effect(std::string_view id) const noexcept {
	// The first case-insensitive effect-name match is the catalog definition.
	// [orig: CEffectWorld_FindEffectDefByName @ 0x5e34f0 ->
	// _stricmp @ 0x5e352c, see docs/particles/ptl-format-re.md].
	for (const EffectDef &effect : effects) {
		if (strutil::iequals(effect.id, id)) return &effect;
	}
	return nullptr;
}

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

size_t ParticleFile::effect_at_line(size_t line) const noexcept {
	for (size_t i = 0; i < effects.size(); ++i) {
		const EffectDef &effect = effects[i];
		if (effect.first_line > 0 && line >= size_t(effect.first_line) &&
				(effect.last_line <= 0 || line <= size_t(effect.last_line)))
			return i;
	}
	return std::string::npos;
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
	return strutil::ifind(haystack, needle) != std::string_view::npos;
}

// [orig: g_ParticleFlagTable @ 0x5ba500 (ParticleEdit_v1_1.exe, cnt dword_5BC2E8=29); JO @ 0x846A18]
// 29 entries in engine table order (the write order). HAZE (idx 9), BELOWH20
// (idx 27), ABOVEH20 (idx 28) added per the ParticleEdit cross-witness grill
// (D5) — fixes the HAZE round-trip drop (fire.ptl) and the GLOBALWIND..
// AMBIENTCOLOR bit-shift. See docs/particles/ptl-format-re.md §1.
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
	// [orig: FlagTable_BuildString @ 0x428fe0 (ParticleEdit_v1_1.exe); JO FlagTable_BuildString @ 0x5df9c0 (ex sub_5DF9C0)]
	// DIVERGENCE D1 (docs/particles/ptl-format-re.md §9): the engine seeds the
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

// ------------------------------------------------------------ the reader's keys

namespace {

using K = KeyValueKind;
using B = BlockKind;

KeyRow row(B block, const char *key, K value) {
	KeyRow r;
	r.block = block;
	r.key = key;
	r.value = value;
	return r;
}

KeyRow clamped(B block, const char *key, int min, int max) {
	KeyRow r = row(block, key, K::Whole);
	r.clamped = true;
	r.min = min;
	r.max = max;
	return r;
}

// One '#' a graphic layer's digit (1..4), one trailing '*' a collision sound's slot (what follows through atol,
// under 20): the patterns key_rows writes.
bool key_matches(std::string_view pattern, std::string_view key) {
	size_t p = 0, k = 0;
	while (p < pattern.size()) {
		const char c = pattern[p];
		if (c == '#') {
			if (k >= key.size() || key[k] < '1' || key[k] > '4') return false;
			++p;
			++k;
		} else if (c == '*') {
			// The slot: atol of the rest, under 20 unsigned (none or a word first is 0, a minus none) [orig:
			// CParticleDef_ParseProperties @ 0x5ebb96..0x5ebba7; the particle's 20 slots @ +3952].
			const int32_t slot = io::retail_atol(std::string(key.substr(k)).c_str());
			return static_cast<uint32_t>(slot) < 20u;
		} else {
			if (k >= key.size() || std::tolower(static_cast<unsigned char>(key[k])) != c) return false;
			++p;
			++k;
		}
	}
	return k == key.size();
}

} // namespace

// [orig: CParticleDef_ParseProperties @ 0x5ea320, the particle's and its graphic layers' keys;
//  CParticleTableDef_ParseScriptLine @ 0x5e92b0, the effect's ([effectdef] routes there, its IDB name
//  notwithstanding); CEffectTableDef_ParseCallback @ 0x5e4010, a table's; the original editor's
//  [tabledef_edithandles] its own, which the game never reads]
const std::vector<KeyRow> &key_rows() {
	static const std::vector<KeyRow> rows = [] {
		std::vector<KeyRow> r;
		r.push_back(row(B::Effect, "id", K::Text));
		r.push_back(row(B::Effect, "pdefs", K::Members));
		r.push_back(row(B::Particle, "id", K::Text));
		r.push_back(row(B::Particle, "child_id", K::Text));
		r.push_back(row(B::Particle, "flags", K::Flags));
		r.push_back(row(B::Particle, "move", K::Move));
		KeyRow lod = row(B::Particle, "lod", K::Real);
		lod.read = false;
		r.push_back(lod);
		for (const char *key : {"emit_dur", "emit_dur_adj", "emit_rate", "emit_rate_adj"})
			r.push_back(row(B::Particle, key, K::Real));
		r.push_back(row(B::Particle, "emit_rate_func", K::Curve));
		r.push_back(row(B::Particle, "emit_delay", K::Real));
		// [orig: CParticleDef_ParseProperties @ 0x5ea86c..0x5ea87a, a burst under 1 taken as 1]
		KeyRow burst = clamped(B::Particle, "emit_burst", 1, 0x7FFFFFFF);
		r.push_back(burst);
		r.push_back(row(B::Particle, "emit_maxoverride", K::Whole));
		r.push_back(row(B::Particle, "emit_shape", K::Whole));
		r.push_back(row(B::Particle, "emit_shape_size", K::Vector));
		r.push_back(row(B::Particle, "emit_shape_size_skip", K::Vector));
		for (const char *key : {"y_offset", "z_offset", "age", "age_adj", "scale", "scale_adj"})
			r.push_back(row(B::Particle, key, K::Real));
		r.push_back(row(B::Particle, "scale_func", K::Curve));
		r.push_back(row(B::Particle, "alpha", K::Real));
		for (const char *key : {"alpha_func", "red_func", "green_func", "blue_func"})
			r.push_back(row(B::Particle, key, K::Curve));
		for (const char *key : {"color1", "color2", "color3", "color4"})
			r.push_back(row(B::Particle, key, K::Color));
		r.push_back(row(B::Particle, "bump_scale", K::Real));
		r.push_back(row(B::Particle, "orientation", K::Vector));
		r.push_back(row(B::Particle, "orientationadj", K::Vector));
		for (const char *key : {"yaw_rot", "yaw_rot_adj", "pitch_rot", "pitch_rot_adj", "roll_rot", "roll_rot_adj",
		                        "speed", "speed_adj", "elastic", "gravity"})
			r.push_back(row(B::Particle, key, K::Real));
		r.push_back(row(B::Particle, "gravity_mask", K::Vector));
		for (const char *key : {"drag", "spread", "spread_skip", "orbitalspeed", "orbitalspeed_adj"})
			r.push_back(row(B::Particle, key, K::Real));
		r.push_back(row(B::Particle, "orbital_axis", K::Vector));
		r.push_back(row(B::Particle, "collide_sound*", K::Text));
		// A graphic layer's: its declaration, then its own keys over the particle's [orig:
		// CParticleDefEntry_ParseGraphicProperty @ 0x5e3550].
		r.push_back(row(B::Particle, "graphic#", K::Graphic));
		// The game stores atol's value as it is; the bound is OpenNova's (D-PTL-19).
		KeyRow frames = clamped(B::Particle, "g#_flip_frames", 1, kMaxParticleFlipFrames);
		frames.port_bound = "D-PTL-19";
		r.push_back(frames);
		r.push_back(row(B::Particle, "g#_flip_rate", K::Whole));
		for (const char *key : {"g#_color1", "g#_color2", "g#_color3", "g#_color4"})
			r.push_back(row(B::Particle, key, K::Color));
		for (const char *key : {"g#_alpha", "g#_scale", "g#_scale_adj"}) r.push_back(row(B::Particle, key, K::Real));
		for (const char *key : {"g#_scale_func", "g#_alpha_func", "g#_red_func", "g#_green_func", "g#_blue_func"})
			r.push_back(row(B::Particle, key, K::Curve));
		r.push_back(row(B::Table, "id", K::Text));
		// A row: the key as the writer writes it (tl1..tl32); the game takes any key holding a 't' (key_row).
		r.push_back(row(B::Table, "tl*", K::TableRow));
		// The original editor's handles, which the game never reaches (their header ends its walk): its table, its
		// count of handles, its smoothing, and each handle (handle0, handle1, ..., two numbers).
		for (const auto &[key, value] : {std::pair{"tableid", K::Text}, std::pair{"handlecount", K::Whole},
		                                 std::pair{"tightness", K::Whole}, std::pair{"handle*", K::Text}}) {
			KeyRow handle = row(B::Handles, key, value);
			handle.read = false;
			r.push_back(handle);
		}
		return r;
	}();
	return rows;
}

const KeyRow *key_row(BlockKind block, std::string_view key) {
	const std::vector<KeyRow> &rows = key_rows();
	if (block == B::Table) {
		// `id` without case, then any key holding a 't' the next row [orig: CEffectTableDef_ParseCallback @
		// 0x5e40c2 stricmp "id"; @ 0x5e4120 strstr "t"].
		const auto row_named = [&rows](const char *name) -> const KeyRow * {
			for (const KeyRow &r : rows)
				if (r.block == B::Table && std::string_view(r.key) == name) return &r;
			return nullptr;
		};
		if (strutil::iequals(key, "id")) return row_named("id");
		return key.find('t') != std::string_view::npos ? row_named("tl*") : nullptr;
	}
	if (block == B::Handles) {
		// A handle by its number (any digits after "handle"), the others by their keys.
		const std::string_view prefix = "handle";
		const bool numbered = key.size() > prefix.size() && strutil::iequals(key.substr(0, prefix.size()), prefix) &&
		                      key.find_first_not_of("0123456789", prefix.size()) == std::string_view::npos;
		for (const KeyRow &r : rows)
			if (r.block == B::Handles &&
			    (numbered ? std::string_view(r.key) == "handle*" : std::string_view(r.key) != "handle*" &&
			                                                          strutil::iequals(r.key, key)))
				return &r;
		return nullptr;
	}
	for (const KeyRow &r : rows)
		if (r.block == block && key_matches(r.key, key)) return &r;
	return nullptr;
}

} // namespace opennova::particle
