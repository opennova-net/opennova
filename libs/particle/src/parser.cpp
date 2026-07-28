#include "particle/parser.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <istream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <io/strutil.h>

namespace opennova::particle {

namespace {

// ------------------------------------------------------------ string helpers

bool is_space(char c) noexcept {
	return std::isspace(static_cast<unsigned char>(c)) != 0;
}

std::string_view trim(std::string_view value) noexcept {
	std::size_t begin = 0;
	while (begin < value.size() && is_space(value[begin])) {
		++begin;
	}
	std::size_t end = value.size();
	while (end > begin && is_space(value[end - 1])) {
		--end;
	}
	return value.substr(begin, end - begin);
}

std::string trim_str(std::string_view value) { return opennova::strutil::trim(value); }

std::string lowercase(std::string_view value) { return opennova::strutil::to_lower(value); }

// Split on a single delimiter, trimming each part. Empty parts are kept (an
// empty texture in `graphic1 = , additive;` is a valid value, not a missing one).
std::vector<std::string> split(std::string_view value, char delim) {
	std::vector<std::string> out;
	std::size_t pos = 0;
	while (pos <= value.size()) {
		const std::size_t next = value.find(delim, pos);
		const std::size_t end = next == std::string_view::npos ? value.size() : next;
		out.emplace_back(trim_str(value.substr(pos, end - pos)));
		if (next == std::string_view::npos) {
			break;
		}
		pos = next + 1;
	}
	return out;
}

// ------------------------------------------------------------ scalar parsers

float parse_float(const std::string &text) {
	return text.empty() ? 0.0f : static_cast<float>(std::atof(text.c_str()));
}

int parse_int(const std::string &text) {
	return text.empty() ? 0 : std::atoi(text.c_str());
}

std::uint8_t parse_byte(const std::string &text) {
	const int value = parse_int(text);
	if (value < 0) {
		return 0;
	}
	if (value > 255) {
		return 255;
	}
	return static_cast<std::uint8_t>(value);
}

bool parse_color3(const std::vector<std::string> &values, Color3 &out) {
	if (values.size() < 3) {
		return false;
	}
	out.r = parse_byte(values[0]);
	out.g = parse_byte(values[1]);
	out.b = parse_byte(values[2]);
	return true;
}

bool parse_vec3(const std::vector<std::string> &values, Vec3 &out) {
	if (values.size() < 3) {
		return false;
	}
	out.x = parse_float(values[0]);
	out.y = parse_float(values[1]);
	out.z = parse_float(values[2]);
	return true;
}

// "table12", "table12 reverse", "table12 inverse", "table12 invert reverse"
// (modifiers in either order). CParticleDef_ParseProperties @ 0x5ea320 sets
// reverse on bit 0x02 and inverse on bit 0x01 — see e.g. scale_func @ 0x5eafdd.
// The engine only checks one trailing token and spells bit 0x01 "inverse", while
// CurveRef_ModifierSuffix @ 0x42bf60 writes "invert". Our parser accepts both
// spellings and both modifiers so it can read retail writer output.
CurveRef parse_curve_ref(const std::string &raw) {
	CurveRef out;
	out.present = true;
	std::vector<std::string> tokens;
	std::istringstream stream(raw);
	std::string token;
	while (stream >> token) {
		tokens.push_back(token);
	}
	if (tokens.empty()) {
		return out;
	}
	while (tokens.size() >= 2) {
		const std::string trailing = lowercase(tokens.back());
		if (trailing == "reverse") {
			out.reverse = true;
		} else if (trailing == "inverse" || trailing == "invert") {
			out.inverse = true;
		} else {
			break;
		}
		tokens.pop_back();
	}
	std::ostringstream rebuilt;
	for (std::size_t i = 0; i < tokens.size(); ++i) {
		if (i > 0) {
			rebuilt << ' ';
		}
		rebuilt << tokens[i];
	}
	out.name = rebuilt.str();
	return out;
}

// ----------------------------------------------------------- per-graphic dispatch

// Returns true if `key` matches `gN_<remainder>` for N in 1..4, fills out_index
// (0..3) and out_remainder. False otherwise.
bool match_graphic_key(const std::string &key, int &out_index, std::string &out_remainder) {
	const std::string folded_key = lowercase(key);
	if (folded_key.size() < 3 || folded_key[0] != 'g') {
		return false;
	}
	const char digit = folded_key[1];
	if (digit < '1' || digit > '4') {
		return false;
	}
	if (folded_key[2] != '_') {
		return false;
	}
	out_index = digit - '1';
	out_remainder = folded_key.substr(3);
	return true;
}

// Returns layer index (0..3) if `key` matches `graphicN`, -1 otherwise.
int match_graphic_decl(const std::string &key) {
	const std::string folded_key = lowercase(key);
	if (folded_key.size() != 8) {
		return -1;
	}
	if (folded_key.compare(0, 7, "graphic") != 0) {
		return -1;
	}
	const char digit = folded_key[7];
	if (digit < '1' || digit > '4') {
		return -1;
	}
	return digit - '1';
}

bool apply_graphic_field(GraphicLayer &layer, const std::string &key,
		const std::string &raw_value, const std::vector<std::string> &values) {
	if (key == "flip_frames") {
		layer.flip_frames = std::clamp(
				parse_int(raw_value), 1, kMaxParticleFlipFrames);
	} else if (key == "flip_rate") {
		layer.flip_rate = parse_int(raw_value);
	} else if (key == "color1") {
		layer.color_overrides_set |= parse_color3(values, layer.color1);
	} else if (key == "color2") {
		layer.color_overrides_set |= parse_color3(values, layer.color2);
	} else if (key == "color3") {
		layer.color_overrides_set |= parse_color3(values, layer.color3);
	} else if (key == "color4") {
		layer.color_overrides_set |= parse_color3(values, layer.color4);
	} else if (key == "alpha") {
		layer.alpha = parse_float(raw_value);
	} else if (key == "scale") {
		layer.scale = parse_float(raw_value);
	} else if (key == "scale_adj") {
		layer.scale_adj = parse_float(raw_value);
	} else if (key == "scale_func") {
		layer.scale_func = parse_curve_ref(raw_value);
	} else if (key == "alpha_func") {
		layer.alpha_func = parse_curve_ref(raw_value);
	} else if (key == "red_func") {
		layer.red_func = parse_curve_ref(raw_value);
	} else if (key == "green_func") {
		layer.green_func = parse_curve_ref(raw_value);
	} else if (key == "blue_func") {
		layer.blue_func = parse_curve_ref(raw_value);
	} else {
		return false;
	}
	return true;
}

// ------------------------------------------------------------ per-section apply

void apply_effect_key(EffectDef &effect, const std::string &authored_key,
		const std::string &raw_value, const std::vector<std::string> &values,
		std::vector<std::pair<std::string, std::string>> &unknown) {
	const std::string key = lowercase(authored_key);
	if (key == "id") {
		effect.id = raw_value;
	} else if (key == "pdefs") {
		effect.pdefs.clear();
		for (const std::string &value : values) {
			if (!value.empty()) {
				effect.pdefs.push_back(value);
			}
		}
	} else {
		unknown.emplace_back(authored_key, raw_value);
	}
}

// [orig: ParticleDef_ParseField @ 0x433e20 (ParticleEdit_v1_1.exe); JO CParticleDef_ParseProperties @ 0x5ea320]
// ParticleEdit confirms: graphic color keys are REMAPPED (g2_color1->color2,
// g3_color1/2->color3, g4_color1/2->color4); we deliberately map 1:1. See
// notes/ida_particle_witness.md "ParticleEdit cross-witness grill" D4.
void apply_particle_key(ParticleDef &particle, const std::string &authored_key,
		const std::string &raw_value, const std::vector<std::string> &values) {
	// JO dispatches every property through _stricmp [orig:
	// CParticleDef_ParseProperties @ 0x5ea320]. Preserve authored spelling
	// only for unknown-field round trips.
	const std::string key = lowercase(authored_key);
	// Per-graphic header: `graphic1 = mbFlash2.tga, additive;`
	const int decl_index = match_graphic_decl(key);
	if (decl_index >= 0) {
		GraphicLayer &layer = particle.graphics[decl_index];
		layer.present = true;
		layer.index = decl_index + 1;
		layer.texture = values.empty() ? std::string() : values[0];
		layer.blend_mode_raw = values.size() >= 2 ? lowercase(values[1]) : std::string();
		layer.blend_mode = parse_blend_mode(layer.blend_mode_raw);
		// Initialize per-layer color/scale/alpha defaults from the particle level
		// (qmemcpy at 0x5ee9d0..0x5eea3d in CParticleDef_ParseFromConfigMap).
		layer.color1 = particle.color1;
		layer.color2 = particle.color2;
		layer.color3 = particle.color3;
		layer.color4 = particle.color4;
		layer.alpha = particle.alpha;
		layer.scale = particle.scale;
		layer.scale_adj = particle.scale_adj;
		return;
	}

	// Per-graphic field: `g1_color1 = ...;`
	int graphic_index = -1;
	std::string graphic_remainder;
	if (match_graphic_key(key, graphic_index, graphic_remainder)) {
		if (!apply_graphic_field(
				particle.graphics[graphic_index], graphic_remainder, raw_value, values)) {
			particle.unknown_keys.emplace_back(authored_key, raw_value);
		}
		return;
	}

	// Particle-level fields. Order matches CParticleDef_ParseFromConfigMap @ 0x5ed210.
	if (key == "id") {
		particle.id = raw_value;
	} else if (key == "child_id") {
		particle.child_id = raw_value;
	} else if (key == "flags") {
		particle.flags_raw = raw_value;
		particle.flags = parse_particle_flags(raw_value);
	} else if (key == "move") {
		particle.move_raw = raw_value;
		particle.move = parse_move_bits(raw_value);
	} else if (key == "lod") {
		particle.lod = parse_float(raw_value);
	} else if (key == "emit_dur") {
		particle.emit_dur = parse_float(raw_value);
	} else if (key == "emit_dur_adj") {
		particle.emit_dur_adj = parse_float(raw_value);
	} else if (key == "emit_rate") {
		particle.emit_rate = parse_float(raw_value);
	} else if (key == "emit_rate_adj") {
		particle.emit_rate_adj = parse_float(raw_value);
	} else if (key == "emit_rate_func") {
		particle.emit_rate_func = parse_curve_ref(raw_value);
	} else if (key == "emit_delay") {
		particle.emit_delay = parse_float(raw_value);
	} else if (key == "emit_burst") {
		particle.emit_burst = parse_int(raw_value);
	} else if (key == "emit_maxoverride") {
		particle.emit_maxoverride = parse_int(raw_value);
	} else if (key == "emit_shape") {
		particle.emit_shape = parse_int(raw_value);
	} else if (key == "emit_shape_size") {
		parse_vec3(values, particle.emit_shape_size);
	} else if (key == "emit_shape_size_skip") {
		parse_vec3(values, particle.emit_shape_size_skip);
	} else if (key == "y_offset") {
		particle.y_offset = parse_float(raw_value);
	} else if (key == "z_offset") {
		particle.z_offset = parse_float(raw_value);
	} else if (key == "age") {
		particle.age = parse_float(raw_value);
	} else if (key == "age_adj") {
		particle.age_adj = parse_float(raw_value);
	} else if (key == "scale") {
		particle.scale = parse_float(raw_value);
	} else if (key == "scale_adj") {
		particle.scale_adj = parse_float(raw_value);
	} else if (key == "scale_func") {
		particle.scale_func = parse_curve_ref(raw_value);
	} else if (key == "alpha") {
		particle.alpha = parse_float(raw_value);
	} else if (key == "alpha_func") {
		particle.alpha_func = parse_curve_ref(raw_value);
	} else if (key == "red_func") {
		particle.red_func = parse_curve_ref(raw_value);
	} else if (key == "green_func") {
		particle.green_func = parse_curve_ref(raw_value);
	} else if (key == "blue_func") {
		particle.blue_func = parse_curve_ref(raw_value);
	} else if (key == "color1") {
		parse_color3(values, particle.color1);
	} else if (key == "color2") {
		parse_color3(values, particle.color2);
	} else if (key == "color3") {
		parse_color3(values, particle.color3);
	} else if (key == "color4") {
		parse_color3(values, particle.color4);
	} else if (key == "bump_scale") {
		particle.bump_scale = parse_float(raw_value);
	} else if (key == "orientation") {
		parse_vec3(values, particle.orientation);
	} else if (key == "orientationadj") {
		parse_vec3(values, particle.orientationadj);
	} else if (key == "yaw_rot") {
		particle.yaw_rot = parse_float(raw_value);
	} else if (key == "yaw_rot_adj") {
		particle.yaw_rot_adj = parse_float(raw_value);
	} else if (key == "pitch_rot") {
		particle.pitch_rot = parse_float(raw_value);
	} else if (key == "pitch_rot_adj") {
		particle.pitch_rot_adj = parse_float(raw_value);
	} else if (key == "roll_rot") {
		particle.roll_rot = parse_float(raw_value);
	} else if (key == "roll_rot_adj") {
		particle.roll_rot_adj = parse_float(raw_value);
	} else if (key == "speed") {
		particle.speed = parse_float(raw_value);
	} else if (key == "speed_adj") {
		particle.speed_adj = parse_float(raw_value);
	} else if (key == "elastic") {
		particle.elastic = parse_float(raw_value);
	} else if (key == "gravity") {
		particle.gravity = parse_float(raw_value);
	} else if (key == "gravity_mask") {
		parse_vec3(values, particle.gravity_mask);
	} else if (key == "drag") {
		particle.drag = parse_float(raw_value);
	} else if (key == "spread") {
		particle.spread = parse_float(raw_value);
	} else if (key == "spread_skip") {
		particle.spread_skip = parse_float(raw_value);
	} else if (key == "orbitalspeed") {
		particle.orbitalspeed = parse_float(raw_value);
	} else if (key == "orbitalspeed_adj") {
		particle.orbitalspeed_adj = parse_float(raw_value);
	} else if (key == "orbital_axis") {
		parse_vec3(values, particle.orbital_axis);
	} else if (key.size() > 13 && key.compare(0, 13, "collide_sound") == 0) {
		const std::string suffix = key.substr(13);
		bool digits_only = !suffix.empty();
		for (char c : suffix) {
			if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
				digits_only = false;
				break;
			}
		}
		const int slot = digits_only ? std::atoi(suffix.c_str()) : -1;
		if (slot >= 0 && static_cast<std::size_t>(slot) < particle.collide_sounds.size()) {
			particle.collide_sounds[static_cast<std::size_t>(slot)] = raw_value;
		} else {
			particle.unknown_keys.emplace_back(authored_key, raw_value);
		}
	} else {
		particle.unknown_keys.emplace_back(authored_key, raw_value);
	}
}

struct PendingParticleStatement {
	std::string key;
	std::string raw_value;
	std::vector<std::string> values;
};

void hydrate_particle(ParticleDef &particle,
		const std::vector<PendingParticleStatement> &statements) {
	// The retail config-map hydrator is source-order independent: particle
	// defaults are established first, then graphic declarations inherit them,
	// then gN fields override the inherited values.
	for (const PendingParticleStatement &statement : statements) {
		int graphic_index = -1;
		std::string graphic_remainder;
		if (match_graphic_decl(statement.key) < 0 &&
				!match_graphic_key(statement.key, graphic_index, graphic_remainder)) {
			apply_particle_key(particle, statement.key, statement.raw_value, statement.values);
		}
	}
	for (const PendingParticleStatement &statement : statements) {
		if (match_graphic_decl(statement.key) >= 0) {
			apply_particle_key(particle, statement.key, statement.raw_value, statement.values);
		}
	}
	for (const PendingParticleStatement &statement : statements) {
		int graphic_index = -1;
		std::string graphic_remainder;
		if (match_graphic_key(statement.key, graphic_index, graphic_remainder)) {
			apply_particle_key(particle, statement.key, statement.raw_value, statement.values);
		}
	}
}

void apply_table_key(TableDef &table, const std::string &authored_key,
		const std::string &raw_value, const std::vector<std::string> &values,
		std::vector<std::pair<std::string, std::string>> &unknown) {
	const std::string key = lowercase(authored_key);
	if (key == "id") {
		table.id = raw_value;
		return;
	}
	if (key.size() > 2 && key[0] == 't' && key[1] == 'l') {
		int row_number = 0;
		bool valid_row_key = true;
		for (std::size_t i = 2; i < key.size(); ++i) {
			const unsigned char c = static_cast<unsigned char>(key[i]);
			if (std::isdigit(c) == 0) {
				valid_row_key = false;
				break;
			}
			row_number = row_number * 10 + (key[i] - '0');
			if (row_number > 32) {
				valid_row_key = false;
				break;
			}
		}
		if (!valid_row_key || row_number < 1 || values.size() < 8) {
			unknown.emplace_back(authored_key, raw_value);
			return;
		}
		// tlN names are indexed config-map fields, not append records. Source
		// order is irrelevant and duplicate keys are last-wins.
		std::array<std::uint8_t, 8> row{};
		for (std::size_t i = 0; i < row.size(); ++i) {
			row[i] = parse_byte(values[i]);
		}
		if (table.rows.size() < static_cast<std::size_t>(row_number)) {
			table.rows.resize(static_cast<std::size_t>(row_number));
		}
		table.rows[static_cast<std::size_t>(row_number - 1)] = row;
		return;
	}
	unknown.emplace_back(authored_key, raw_value);
}

void apply_handles_key(TableEditHandles &handles, const std::string &authored_key,
		const std::string &raw_value,
		std::vector<std::pair<std::string, std::string>> &unknown) {
	const std::string key = lowercase(authored_key);
	if (key == "tableid") {
		handles.table_id = raw_value;
	} else if (key == "handlecount") {
		handles.handlecount = parse_int(raw_value);
	} else if (key == "tightness") {
		handles.tightness = parse_int(raw_value);
	} else {
		unknown.emplace_back(authored_key, raw_value);
	}
}

// ------------------------------------------------------------ driver

enum class Section {
	None,
	Effect,
	Particle,
	Table,
	Handles,
};

enum class State {
	TopLevel,
	ExpectOpen,
	InBlock,
};

bool match_section(std::string_view line, Section &out) {
	// The top-level dispatcher uses _stricmp for every section token
	// [orig: CEffectWorld_ParseSectionCallback @ 0x5ecb40].
	const std::string folded_line = lowercase(line);
	if (folded_line == "[effectdef]") {
		out = Section::Effect;
		return true;
	}
	if (folded_line == "[particledef]") {
		out = Section::Particle;
		return true;
	}
	if (folded_line == "[tabledef]") {
		out = Section::Table;
		return true;
	}
	if (folded_line == "[tabledef_edithandles]") {
		out = Section::Handles;
		return true;
	}
	return false;
}

} // namespace

bool load_particles(std::istream &input, ParticleFile &out, ParseError &error) {
	out = ParticleFile();
	error = ParseError();

	// Slurp; trim trailing NUL bytes (observed in 30MM/airexp/ambfx/df_exp).
	std::string buffer{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	while (!buffer.empty() && buffer.back() == '\0') {
		buffer.pop_back();
	}

	State state = State::TopLevel;
	Section section = Section::None;

	EffectDef current_effect;
	ParticleDef current_particle;
	TableDef current_table;
	TableEditHandles current_handles;
	std::vector<PendingParticleStatement> pending_particle;
	std::vector<std::pair<std::string, std::string>> stray_unknown; // for [effectdef]/[tabledef]/[handles]

	std::size_t line_number = 0;
	std::size_t pos = 0;
	while (pos <= buffer.size()) {
		const std::size_t newline = buffer.find('\n', pos);
		const std::size_t end = newline == std::string::npos ? buffer.size() : newline;
		std::string_view raw = std::string_view(buffer).substr(pos, end - pos);
		if (!raw.empty() && raw.back() == '\r') {
			raw.remove_suffix(1);
		}
		++line_number;
		pos = newline == std::string::npos ? buffer.size() + 1 : newline + 1;

		const std::string_view line = trim(raw);
		if (line.empty()) {
			continue;
		}

		switch (state) {
			case State::TopLevel: {
				Section parsed = Section::None;
				if (!match_section(line, parsed)) {
					// Unrecognized top-level lines are IGNORED, never fatal —
					// retail skips anything its section matcher doesn't claim
					// (comment dividers like "//====" ship between blocks in
					// modded .ptl) [orig: CEffectWorld_ParseSectionCallback
					// @ 0x5ecb40]. Rejecting the file here lost the entire
					// Effect_AmHit* family to the stockeffect fallback (D-PTL-13).
					continue;
				}
				section = parsed;
				state = State::ExpectOpen;
				switch (section) {
					case Section::Effect:
						current_effect = EffectDef();
						break;
					case Section::Particle:
						current_particle = ParticleDef();
						pending_particle.clear();
						break;
					case Section::Table:
						current_table = TableDef();
						break;
					case Section::Handles:
						current_handles = TableEditHandles();
						break;
					case Section::None:
						break;
				}
				stray_unknown.clear();
				break;
			}

			case State::ExpectOpen: {
				if (line.front() != '{') {
					error.message = "Expected '{' after section header";
					error.line = static_cast<int>(line_number);
					return false;
				}
				state = State::InBlock;
				break;
			}

			case State::InBlock: {
				if (line.front() == '}') {
					switch (section) {
						case Section::Effect:
							out.effects.push_back(std::move(current_effect));
							break;
						case Section::Particle: {
							hydrate_particle(current_particle, pending_particle);
							// Engine clamps emit_burst < 1 to 1 (sub_5ed6c2);
							// we preserve the source value but mirror the clamp
							// so consumers see authoring intent.
							if (current_particle.emit_burst < 1) {
								current_particle.emit_burst = 1;
							}
							out.particles.push_back(std::move(current_particle));
							break;
						}
						case Section::Table:
							out.tables.push_back(std::move(current_table));
							break;
						case Section::Handles:
							out.table_handles.push_back(std::move(current_handles));
							break;
						case Section::None:
							break;
					}
					section = Section::None;
					state = State::TopLevel;
					break;
				}

				const std::size_t eq = line.find('=');
				if (eq == std::string_view::npos) {
					// Same leniency in-block: the witnessed per-section line
					// parsers skip lines they don't recognize instead of
					// failing the file [orig: CEffectWorld_ParseSectionCallback
					// @ 0x5ecb40].
					continue;
				}

				std::string key = trim_str(line.substr(0, eq));
				std::string_view value_view = line.substr(eq + 1);
				// Strip trailing ';' (one or more).
				while (!value_view.empty() && (value_view.back() == ';' || is_space(value_view.back()))) {
					value_view.remove_suffix(1);
				}
				const std::string raw_value = trim_str(value_view);
				const std::vector<std::string> values = split(raw_value, ',');

				switch (section) {
					case Section::Effect:
						apply_effect_key(current_effect, key, raw_value, values, stray_unknown);
						break;
					case Section::Particle:
						pending_particle.push_back({key, raw_value, values});
						break;
					case Section::Table:
						apply_table_key(current_table, key, raw_value, values, stray_unknown);
						break;
					case Section::Handles:
						apply_handles_key(current_handles, key, raw_value, stray_unknown);
						break;
					case Section::None:
						break;
				}
				break;
			}
		}
	}

	if (state != State::TopLevel) {
		error.message = state == State::InBlock ? "Unclosed '{'" : "Section header without body";
		error.line = static_cast<int>(line_number);
		return false;
	}

	return true;
}

bool load_particles_from_file(const std::string &path, ParticleFile &out, ParseError &error) {
	std::ifstream stream(path, std::ios::binary);
	if (!stream) {
		error = ParseError();
		error.message = "Cannot open " + path;
		return false;
	}
	return load_particles(stream, out, error);
}

bool load_particles_from_buffer(const char *data, std::size_t size, ParticleFile &out, ParseError &error) {
	std::stringstream stream;
	stream.write(data, static_cast<std::streamsize>(size));
	return load_particles(stream, out, error);
}

} // namespace opennova::particle
