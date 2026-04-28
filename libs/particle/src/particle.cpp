#include "particle/particle.h"

#include <array>
#include <cctype>
#include <cstring>
#include <sstream>
#include <utility>

namespace opennova::particle {

const ParticleDef *ParticleFile::find_particle(std::string_view id) const noexcept {
	for (const ParticleDef &particle : particles) {
		if (particle.id == id) {
			return &particle;
		}
	}
	return nullptr;
}

const TableDef *ParticleFile::find_table(std::string_view id) const noexcept {
	for (const TableDef &table : tables) {
		if (table.id == id) {
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

// Engine flag table @ 0x846A18 — order matches CParticleDef_ParseProperties
// dispatch and FlagTable_ParseFromString iteration.
constexpr std::array<std::pair<const char *, std::uint32_t>, 26> kParticleFlagEntries = {{
	{"NOVISNOUPDATE",        particle_flag::NoVisNoUpdate},
	{"INITIALYCLIP",         particle_flag::InitialClip},
	{"NEVERAGE",             particle_flag::NeverAge},
	{"TOPALIGN",             particle_flag::TopAlign},
	{"ONMYDEATH",            particle_flag::OnMyDeath},
	{"USEPARENTSCALE",       particle_flag::UseParentScale},
	{"USEPARENTCOLOR",       particle_flag::UseParentColor},
	{"USEPARENTALPHA",       particle_flag::UseParentAlpha},
	{"YAWANDPITCH",          particle_flag::YawAndPitch},
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
	// Engine sub_5DF9C0 @ 0x5df9c0 emits names in table order, separated by
	// single spaces, with one trailing space — replicate exactly so round-trip
	// matches what the runtime would write.
	std::string out;
	for (const auto &entry : entries) {
		if ((bits & entry.second) != 0) {
			if (!out.empty()) {
				out.push_back(' ');
			}
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
	// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 uses chained strstr — first
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

} // namespace opennova::particle
