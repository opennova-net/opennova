#include "particle/parser.h"

#include <cstdio>
#include <fstream>
#include <ostream>
#include <string>

#include "particle/particle.h"

namespace opennova::particle {

namespace {

// Engine writers (CParticleDef_SaveToFile @ 0x5e4d70 et al.) use plain "\n" in
// fprintf format strings; the OS may translate to CRLF in text-mode stdio.
// We emit LF only — consumers can convert if they need CRLF for parity with
// captured corpus.
constexpr const char *NL = "\n";

std::string format_float(float value) {
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%5.3f", value);
	return buffer;
}

std::string format_int(int value) {
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%d", value);
	return buffer;
}

void write_color(std::ostream &out, const Color3 &c) {
	// Engine writer at 0x5e5039 emits "%d, %d, %d" with a single space after
	// each comma. Mirror exactly.
	out << static_cast<int>(c.r) << ", " << static_cast<int>(c.g) << ", " << static_cast<int>(c.b);
}

void write_vec3(std::ostream &out, const Vec3 &v) {
	out << format_float(v.x) << ", " << format_float(v.y) << ", " << format_float(v.z);
}

// [orig: CurveRef_ModifierSuffix @ 0x42bf60 (ParticleEdit_v1_1.exe); JO suffix writer in CParticleDef_SaveToFile @ 0x5e4d70]
// The engine writer maps modifier bit 0x01 -> " invert", bit 0x02 ->
// " reverse", and both -> " invert reverse". Its parser spells the first token
// "inverse" instead; our parser accepts both spellings so retail writer output
// remains round-trippable.
std::string format_curve_suffix(const CurveRef &ref) {
	std::string out;
	if (ref.inverse) out += " invert";
	if (ref.reverse) out += " reverse";
	return out;
}

void write_curve_line(std::ostream &out, const char *prefix, const char *key,
		const CurveRef &ref) {
	if (!ref.present) {
		return;
	}
	out << prefix << key << "\t= " << ref.name << format_curve_suffix(ref) << ";" << NL;
}

void write_effect(std::ostream &out, const EffectDef &effect) {
	// CParticleEffectDef_WriteToFile @ 0x5e0fe0
	out << NL << "[effectdef]" << NL << "{" << NL;
	out << "\tid = " << effect.id << ";" << NL;
	if (!effect.pdefs.empty()) {
		out << "\tpdefs = ";
		for (std::size_t i = 0; i < effect.pdefs.size(); ++i) {
			if (i > 0) {
				out << ", ";
			}
			out << effect.pdefs[i];
		}
		out << ";" << NL;
	}
	out << NL << "}" << NL;
}

void write_table(std::ostream &out, const TableDef &table) {
	// CParticleTableDef_WriteToFile @ 0x5e27e0 — always emits exactly 32 rows
	// of 8 unsigned values; we mirror that even if the parsed table has fewer
	// rows (zero-fill to match the engine's fixed shape).
	out << NL << "[tabledef]" << NL << "{" << NL;
	out << "\tid = " << table.id << ";" << NL;
	for (int row_index = 0; row_index < 32; ++row_index) {
		out << "\ttl" << (row_index + 1) << " = ";
		for (int i = 0; i < 8; ++i) {
			if (i > 0) {
				out << ", ";
			}
			const std::array<std::uint8_t, 8> &row = static_cast<std::size_t>(row_index) < table.rows.size()
					? table.rows[static_cast<std::size_t>(row_index)]
					: std::array<std::uint8_t, 8>{};
			out << static_cast<unsigned>(row[static_cast<std::size_t>(i)]);
		}
		out << ";" << NL;
	}
	out << NL << "}" << NL;
}

void write_handles(std::ostream &out, const TableEditHandles &handles) {
	// No engine writer was decoded for this section; the format mirrors the
	// observed corpus (boatwake.ptl:40-45) which closes the block with `};`.
	out << NL << "[tabledef_edithandles]" << NL << "{" << NL;
	out << "\thandlecount = " << handles.handlecount << ";" << NL;
	out << "\ttableid = " << handles.table_id << ";" << NL;
	out << "\ttightness = " << handles.tightness << ";" << NL;
	out << "}" << NL;
}

void write_graphic(std::ostream &out, const GraphicLayer &layer, int slot) {
	// Per CParticleDef_SaveToFile @ 0x5e4d70 graphic loop (0x5e540b..0x5e56ee).
	// Header line uses tab, space-equals, tab, then "<texture>, <blend>;".
	out << "\tgraphic" << slot << " =\t" << layer.texture << ", "
			<< blend_mode_name(layer.blend_mode) << ";" << NL;

	const std::string prefix = std::string("\tg") + std::to_string(slot) + "_";

	out << prefix << "flip_frames\t= " << layer.flip_frames << ";" << NL;
	out << prefix << "flip_rate\t= " << layer.flip_rate << ";" << NL;

	out << prefix << "color1\t= "; write_color(out, layer.color1); out << ";" << NL;
	out << prefix << "color2\t= "; write_color(out, layer.color2); out << ";" << NL;
	out << prefix << "color3\t= "; write_color(out, layer.color3); out << ";" << NL;
	out << prefix << "color4\t= "; write_color(out, layer.color4); out << ";" << NL;
	out << prefix << "alpha\t= " << format_float(layer.alpha) << ";" << NL;
	out << prefix << "scale\t= " << format_float(layer.scale) << ";" << NL;
	out << prefix << "scale_adj\t= " << format_float(layer.scale_adj) << ";" << NL;

	// CurveRef::present is the serialization gate. A graphic can author a
	// curve without a particle-level fallback, so each layer is independent.
	write_curve_line(out, prefix.c_str(), "scale_func", layer.scale_func);
	write_curve_line(out, prefix.c_str(), "alpha_func", layer.alpha_func);
	write_curve_line(out, prefix.c_str(), "red_func", layer.red_func);
	write_curve_line(out, prefix.c_str(), "green_func", layer.green_func);
	write_curve_line(out, prefix.c_str(), "blue_func", layer.blue_func);
}

void write_particle(std::ostream &out, const ParticleDef &p) {
	// [orig: ParticleDef_Write @ 0x42ec20 (ParticleEdit_v1_1.exe); JO CParticleDef_SaveToFile @ 0x5e4d70]
	// Field order/whitespace/duplicate emit_dur/unconditional lod/BGR colors all
	// byte-match ParticleEdit's fprintf format strings — exact field ordering and whitespace.
	out << NL << "[particledef]" << NL << "{" << NL;

	out << "id\t= " << p.id << ";" << NL;
	if (!p.child_id.empty()) {
		out << "child_id\t= " << p.child_id << ";" << NL;
	}
	if (p.flags != 0) {
		out << "flags\t= " << format_particle_flags(p.flags) << ";" << NL;
	}
	if (p.move != 0) {
		out << "move\t= " << format_move_bits(p.move) << ";" << NL;
	}
	out << "lod\t= " << format_float(p.lod) << ";" << NL;
	out << NL;

	out << "\temit_dur\t= "          << format_float(p.emit_dur) << ";" << NL;
	out << "\temit_dur_adj\t= "      << format_float(p.emit_dur_adj) << ";" << NL;
	out << "\temit_rate\t= "         << format_float(p.emit_rate) << ";" << NL;
	out << "\temit_rate_adj\t= "     << format_float(p.emit_rate_adj) << ";" << NL;
	if (p.emit_rate_func.present) {
		write_curve_line(out, "\t", "emit_rate_func", p.emit_rate_func);
	}
	out << "\temit_delay\t= "        << format_float(p.emit_delay) << ";" << NL;
	out << "\temit_burst\t= "        << p.emit_burst << ";" << NL;
	out << "\temit_maxoverride\t= "  << p.emit_maxoverride << ";" << NL;
	// Engine emits emit_dur a second time at 0x5e4f30 (same field +906) — replicate.
	out << "\temit_dur\t= "          << format_float(p.emit_dur) << ";" << NL;
	out << "\ty_offset\t= "          << format_float(p.y_offset) << ";" << NL;
	out << "\tz_offset\t= "          << format_float(p.z_offset) << ";" << NL;
	out << "\temit_shape\t= "        << p.emit_shape << ";" << NL;
	out << "\temit_shape_size\t= ";        write_vec3(out, p.emit_shape_size);      out << ";" << NL;
	out << "\temit_shape_size_skip\t= ";   write_vec3(out, p.emit_shape_size_skip); out << ";" << NL;
	out << NL;

	out << "\tage\t= "         << format_float(p.age) << ";" << NL;
	out << "\tage_adj\t= "     << format_float(p.age_adj) << ";" << NL;
	out << "\talpha\t= "       << format_float(p.alpha) << ";" << NL;
	out << "\tcolor1\t= "; write_color(out, p.color1); out << ";" << NL;
	out << "\tcolor2\t= "; write_color(out, p.color2); out << ";" << NL;
	out << "\tcolor3\t= "; write_color(out, p.color3); out << ";" << NL;
	out << "\tcolor4\t= "; write_color(out, p.color4); out << ";" << NL;
	out << "\tbump_scale\t= " << format_float(p.bump_scale) << ";" << NL;
	out << "\tscale\t= "      << format_float(p.scale) << ";" << NL;
	out << "\tscale_adj\t= "  << format_float(p.scale_adj) << ";" << NL;
	if (p.scale_func.present) write_curve_line(out, "\t", "scale_func", p.scale_func);
	if (p.alpha_func.present) write_curve_line(out, "\t", "alpha_func", p.alpha_func);
	if (p.red_func.present)   write_curve_line(out, "\t", "red_func",   p.red_func);
	if (p.green_func.present) write_curve_line(out, "\t", "green_func", p.green_func);
	if (p.blue_func.present)  write_curve_line(out, "\t", "blue_func",  p.blue_func);
	out << NL;

	out << "\torientation\t= ";    write_vec3(out, p.orientation);    out << ";" << NL;
	out << "\torientationadj\t= "; write_vec3(out, p.orientationadj); out << ";" << NL;
	out << "\tyaw_rot\t= "          << format_float(p.yaw_rot) << ";" << NL;
	out << "\tyaw_rot_adj\t= "      << format_float(p.yaw_rot_adj) << ";" << NL;
	out << "\tpitch_rot\t= "        << format_float(p.pitch_rot) << ";" << NL;
	out << "\tpitch_rot_adj\t= "    << format_float(p.pitch_rot_adj) << ";" << NL;
	out << "\troll_rot\t= "         << format_float(p.roll_rot) << ";" << NL;
	out << "\troll_rot_adj\t= "     << format_float(p.roll_rot_adj) << ";" << NL;
	out << "\tspeed\t= "            << format_float(p.speed) << ";" << NL;
	out << "\tspeed_adj\t= "        << format_float(p.speed_adj) << ";" << NL;
	out << "\telastic\t= "          << format_float(p.elastic) << ";" << NL;
	out << "\tgravity\t= "          << format_float(p.gravity) << ";" << NL;
	out << "\tgravity_mask\t= ";    write_vec3(out, p.gravity_mask);  out << ";" << NL;
	out << "\tdrag\t= "             << format_float(p.drag) << ";" << NL;
	out << "\tspread\t= "           << format_float(p.spread) << ";" << NL;
	out << "\tspread_skip\t= "      << format_float(p.spread_skip) << ";" << NL;
	out << "\torbitalspeed\t= "     << format_float(p.orbitalspeed) << ";" << NL;
	out << "\torbitalspeed_adj\t= " << format_float(p.orbitalspeed_adj) << ";" << NL;
	out << "\torbital_axis\t= ";    write_vec3(out, p.orbital_axis);  out << ";" << NL;

	for (std::size_t i = 0; i < p.graphics.size(); ++i) {
		if (p.graphics[i].present) {
			write_graphic(out, p.graphics[i], static_cast<int>(i + 1));
		}
	}

	for (std::size_t i = 0; i < p.collide_sounds.size(); ++i) {
		if (!p.collide_sounds[i].empty()) {
			out << "collide_sound" << i << "\t= " << p.collide_sounds[i] << ";" << NL;
		}
	}
	for (const auto &entry : p.unknown_keys) {
		out << '\t' << entry.first << "\t= " << entry.second << ";" << NL;
	}

	out << NL << "}" << NL;
}

} // namespace

bool save_particles(std::ostream &output, const ParticleFile &file, std::string &error) {
	error.clear();
	for (const EffectDef &effect : file.effects) {
		write_effect(output, effect);
	}
	for (const ParticleDef &particle : file.particles) {
		write_particle(output, particle);
	}
	for (const TableDef &table : file.tables) {
		write_table(output, table);
	}
	for (const TableEditHandles &handles : file.table_handles) {
		write_handles(output, handles);
	}
	if (!output.good()) {
		error = "Stream write error";
		return false;
	}
	return true;
}

bool save_particles_to_file(const std::string &path, const ParticleFile &file, std::string &error) {
	std::ofstream stream(path, std::ios::binary);
	if (!stream) {
		error = "Cannot open " + path;
		return false;
	}
	return save_particles(stream, file, error);
}

} // namespace opennova::particle
