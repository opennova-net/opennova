// A BHD-era GP model migrated to 3DI3, the only model format OpenNova and JO load (ADR 0027 as
// amended for the BHD-era import). The rule for every field is the runtime one: given the
// 3DI3 this writes, JO's loader [orig: ThreediGp_LoadFromFile @ 0x5B5780 (Jointops)] builds the
// model BHD's loader built from the GP file [orig: GP_LoadModel @ 0x510E10 (dfbhd)]. Where JO
// has no counterpart for what BHD did (a mission region's texture, a flat-colour surface, a
// spinner part), the migration says so in a note and writes the nearest JO form; the map, its
// witnesses and the notes are docs/threedi/3di-gp-format-re.md, "GP to 3DI3".
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <formats/threedi_gp/threedi_gp.h>

namespace opennova::threedi_gp {

struct MigrateOptions {
	// The mission region whose textures a material takes (0..2): BHD picks a material's
	// texture by the region at load, JO has none [orig: GP_LoadRenderModel @ 0x515D16..0x515D27
	// (dfbhd)].
	int region = 0;
	// Whether a texture file of this name sits beside the model: a bump material's normal map
	// is BHD's `<texture>.mdt` when there is one, else its texture's alpha [orig:
	// Texture_LoadAsNormalMap @ 0x4FAEBD..0x4FAF5F (dfbhd)]. Unset: the alpha is taken.
	std::function<bool(const std::string &name)> texture_exists;
};

// MigrateOptions::texture_exists over the folder `file_path` (UTF-8) sits in: its files listed
// once, by name without case (a retail tree mixes FSUN.3DI and fsun.3di); a folder that cannot be
// listed, or stops listing, holds none past that point.
std::function<bool(const std::string &name)> names_beside(const std::string &file_path);

// What the migration could not carry exactly, one line a kind, with how many it met.
struct MigrateNote {
	std::string text;
	int count = 0;
};

// Migrate `gp` to the bytes of a 3DI3 file. False, with `error` saying why, when the model
// holds what no 3DI3 can (a record BHD's corpus never ships, a name past its field, a model
// past a chunk's 24-bit length).
bool migrate(const File &gp, std::vector<uint8_t> &out, std::vector<MigrateNote> &notes, std::string &error,
		const MigrateOptions &options = {});

} // namespace opennova::threedi_gp
