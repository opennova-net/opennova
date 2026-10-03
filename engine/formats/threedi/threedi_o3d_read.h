// The `.o3d` model scene text (docs/threedi/o3d-scene-format.md) read into the
// engine's construction API (threedi_build.h) and minted through the parity
// writer, so a model authored in a DCC is written by the same writer every
// fixture is (ADR 0003, ADR 0047 d1). The text carries geometry in MISSION axes
// (x forward, y left, z up); the model-axis conversion is threedi_build's.
// opennova-3di `build` and the editor's `.o3d` import both read through here.
//
// The reader is strict: a field that does not parse, a value its word cannot
// hold, or a token past the record's fields is an error naming its line, so
// nothing an exporter writes is silently wrapped or dropped. Authoring text,
// not a port: the rules it enforces cite the retail loader where it has one.
#pragma once

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

#include <formats/threedi/scene_text.h>
#include <formats/threedi/threedi_build.h>

namespace opennova::threedi {

// Whether the engine's shader table knows `tag`, and whether that shader reads
// the TANGENT semantic. The table is the renderer's
// (runtime/renderer/material_descriptor.h, material_descriptor_tangent_lookup),
// which a format library may not include, so the caller passes it.
using ThreediShaderLookup = bool (*)(const char *tag, bool &reads_tangents);

// Whether the game's texture loader reads a texture row (its name and authored
// type) only as the `.dds` of its stem: true with `opens` the file it opens and
// `loads` that `.dds`; false for a row it decodes itself, one another loader
// reads, or an empty name. The rule is the renderer's
// (runtime/renderer/material_texture.h, material_texture_dds_only), which a
// format library may not include, so the caller passes it.
using ThreediTextureLookup = bool (*)(const char *name, uint8_t type, std::string &opens, std::string &loads);

// Read `.o3d` text into `model`. Every error and note lands in `findings`; true
// when none is an error, and `model` is then complete and passes the
// whole-model checks retail imposes.
bool threedi_o3d_read(std::istream &text, ThreediShaderLookup shaders, ThreediTextureLookup textures,
		ThreediBuildModel &model, std::vector<SceneFinding> &findings);

// Read, mint (threedi_build_mint) and read the minted bytes back through the
// retail-shape reader. True with `out` the model's bytes; false with why in
// `findings` (a chunk too large for its length field named with its size).
bool threedi_o3d_build(std::istream &text, ThreediShaderLookup shaders, ThreediTextureLookup textures,
		std::vector<uint8_t> &out, std::vector<SceneFinding> &findings);

} // namespace opennova::threedi
