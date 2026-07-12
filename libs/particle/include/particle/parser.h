#pragma once

#include <cstddef>
#include <istream>
#include <ostream>
#include <string>

#include "particle/particle.h"

namespace opennova::particle {

struct ParseError {
	std::string message;
	int line = 0;     // 1-based; 0 means error has no source location
	int column = 0;   // 1-based; 0 means error refers to whole line
};

// Parses a NovaLogic .ptl text file. Returns true on success and fills `out`;
// on failure returns false and fills `error` (message + best-effort source loc).
//
// Mirrors the engine pipeline:
//   CEffectWorld_ParseSectionCallback @ 0x5ecb40  — section tag dispatch
//   CParticleDef_ParseFromConfigMap   @ 0x5ed210  — [particledef] hydration
//   CParticleTableDef_ParseScriptLine @ 0x5e92b0  — [tabledef] line driver
bool load_particles(std::istream &input, ParticleFile &out, ParseError &error);
bool load_particles_from_file(const std::string &path, ParticleFile &out, ParseError &error);
bool load_particles_from_buffer(const char *data, std::size_t size, ParticleFile &out, ParseError &error);

// Serializes a ParticleFile back to text. Field ordering, leading whitespace,
// numeric format ("%5.3f"), pdefs separator (", ") and the duplicated `emit_dur`
// line all mirror the engine writers:
//   CParticleEffectDef_WriteToFile @ 0x5e0fe0
//   CParticleDef_SaveToFile        @ 0x5e4d70
//   CParticleTableDef_WriteToFile  @ 0x5e27e0
// LF line endings (engine uses bare "\n" in fprintf format strings; on Windows
// stdio in text mode the OS may translate to CRLF).
bool save_particles(std::ostream &output, const ParticleFile &file, std::string &error);
bool save_particles_to_file(const std::string &path, const ParticleFile &file, std::string &error);

} // namespace opennova::particle
