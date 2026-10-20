#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <ostream>
#include <string>

#include <formats/particle/particle.h>

namespace opennova::particle {

struct ParseError {
	std::string message;
	int line = 0;     // 1-based; 0 means error has no source location
	int column = 0;   // 1-based; 0 means error refers to whole line
};

// What the game's effect readers keep between the lines they read, and between the files the effect
// system reads in turn: retail keeps all of it in process globals, so a line can carry on what an
// earlier line, an earlier block or an earlier file left [orig: CEffectWorld_ParseSectionCallback @
// 0x5ecb40, the section flags byte_2C06548/49/4A; CParticleTableDef_ParseScriptLine @ 0x5e92b0, the
// effect name byte_2C06448 and the effect it builds dword_2C06558; CParticleDef_ParseProperties @
// 0x5ea320, the parse scratch g_ParticleSystemDefParseScratch @ 0x2c06588, the definition it fills
// dword_2C06550, and the colour channels dword_2C06560/68/64; CParticleDefEntry_ParseGraphicProperty
// @ 0x5e3550, the graphic index dword_2C0654C; CEffectTableDef_ParseCallback @ 0x5e4010, the table
// it fills dword_2C06554, its row cursor dword_2C0655C and its row count dword_2C0656C]. A reader of
// one file alone starts from a fresh state; the effect system's load passes one state to every file
// in its order (the reads below that take one).
struct ParticleReadState {
	// The section a line is dispatched to: the last section header read, none before the first.
	enum class Section : std::uint8_t { None, Effect, Particle, Table };
	Section section = Section::None;

	// The effect reader: the name the last `id` line copied (it outlives its block), where that line
	// wrote it (0 once another file is read), and the effect a `pdefs` line built, registered at the
	// next `}` of an effect section.
	std::string effect_name;
	int effect_name_line = 0;
	int effect_name_column = 0;
	bool effect_open = false;
	EffectDef effect;

	// The particle reader: the scratch an `id` line resets to the game's defaults and a `}` copies
	// into the definition that `id` opened, the scratch's graphic count, the graphic index (outside
	// the scratch, so it outlives its block), and the colour channels every colour line shares.
	ParticleDef scratch;
	int graphic_count = 0;
	bool particle_open = false;
	int graphic_index = 0;
	std::int32_t red = 0;
	std::int32_t green = 0;
	std::int32_t blue = 0;

	// The table reader: the table a `{` opened (registered at its `}`), the row count (reset only by
	// an `id` line), and the row cursor: a byte offset into the table the last `{` opened, which keeps
	// pointing into it once it is registered.
	bool table_open = false;
	TableDef table;
	std::int32_t row_count = 0;
	enum class Cursor : std::uint8_t { None, Open, Registered, Elsewhere };
	Cursor cursor = Cursor::None;
	std::size_t cursor_offset = 0;
	std::size_t cursor_table = 0; // the registered table's index in the file being read (Cursor::Registered)
};

// Reads a NovaLogic .ptl text as the game's effect system does: the shared ASCII walk (lines cut at
// CR LF, each tokenized, a line of no token or whose first starts with '/' skipped; base/io/
// ascii_config.h [orig: File_ParseASCIIFile @ 0x53d810; Config_TokenizeLine @ 0x53cb60]), each line
// handed to the section callback, which a `[tabledef_edithandles]` header ends [orig:
// CEffectWorld_ParseSectionCallback @ 0x5ecb40], and its three line readers:
//   CParticleTableDef_ParseScriptLine @ 0x5e92b0  the [effectdef] reader (its IDB name notwithstanding)
//   CParticleDef_ParseProperties      @ 0x5ea320  the [particledef] reader, with
//     CParticleDefEntry_ParseGraphicProperty @ 0x5e3550 for its graphic lines
//   CEffectTableDef_ParseCallback     @ 0x5e4010  the [tabledef] reader
// The game refuses no text: these return true for every text (load_particles_from_file false only
// for a file it cannot open). After the walk ends the reader goes on for the original editor's
// [tabledef_edithandles] blocks alone (ParticleFile::table_handles), which the game never reads.
// The config-map hydrator, CParticleDef_ParseFromConfigMap @ 0x5ed210, is on no load's path (D-PTL-38).
bool load_particles(std::istream &input, ParticleFile &out, ParseError &error);
bool load_particles_from_file(const std::string &path, ParticleFile &out, ParseError &error);
bool load_particles_from_buffer(const char *data, std::size_t size, ParticleFile &out, ParseError &error);
bool load_particles_from_buffer(const char *data, std::size_t size, ParticleFile &out, ParticleReadState &state,
                                ParseError &error);
// The same read, and where the text writes each block and key (ParticlePlaces): what the editor asks for alone.
bool load_particles_with_places(const char *data, std::size_t size, ParticleFile &out, ParticlePlaces &places,
                                ParseError &error);
bool load_particles_with_places(const char *data, std::size_t size, ParticleFile &out, ParticlePlaces &places,
                                ParticleReadState &state, ParseError &error);

// Serializes a ParticleFile back to text. Field ordering, leading whitespace,
// numeric format ("%5.3f"), pdefs separator (", ") and the duplicated `emit_dur`
// line all mirror the engine writers:
//   CParticleEffectDef_WriteToFile @ 0x5e0fe0
//   CParticleDef_SaveToFile        @ 0x5e4d70
//   CParticleTableDef_WriteToFile  @ 0x5e27e0
// CR LF line ends: the engine's fprintf "\n" through a text-mode stream, the line end the game's
// reader cuts at.
bool save_particles(std::ostream &output, const ParticleFile &file, std::string &error);

} // namespace opennova::particle
