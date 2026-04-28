#pragma once

#include <cstddef>
#include <istream>
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

} // namespace opennova::particle
