// GRM facial texture meshes and gesture offsets.
// [orig: FaceAnimConfig_ParseProperty @0x5886A0; GRM writer @0x588320]
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::grm {

struct Point {
	float x = 0.0f, y = 0.0f;
};

struct Vertex {
	Point uv;
	std::string group;
};

struct Parameter {
	Point offset;
	std::string group;
};

struct Gesture {
	std::string name;
	std::vector<Parameter> parameters;
};

// The writer's three header comments, represented as fields rather than raw
// input. A caller saving an edited document supplies its current local time
// and author here. Runtime loading does not use these fields.
// [orig: GRM writer @0x588320, GetLocalTime/GetUserNameA @0x588375/0x588385]
struct SaveInfo {
	int month = 0, day = 0, year = 0, hour = 0, minute = 0, second = 0;
	std::string author;
};

struct File {
	SaveInfo saved;
	std::string base_texture;
	// File order: first -> original +520, second -> +260.
	std::array<std::string, 2> eye_textures;
	std::vector<Vertex> vertices;
	std::vector<std::array<int32_t, 3>> triangles;
	std::vector<Gesture> gestures;
	// [orig: FaceAnimConfig_InitEyeDefaults @0x588D20]
	Point eye_size{0.04f, 0.06f};
	std::array<Point, 2> eye_centers{{{0.35f, 0.5f}, {0.65f, 0.5f}}};
	Point eye_limits{0.03f, 0.01f};
};

// Retail's CRLF line splitter, case-insensitive keys, comma/space/tab tokens,
// quoted names and ignored unknown statements. A final unterminated line loses
// its last byte, as in the original. Unsafe indices/counts/overlong names fail
// instead of accessing beyond the original arrays (D-GRM-1).
bool parse(const uint8_t *data, size_t size, File &out, std::string &error);

// The texture file the game opens for a name the file writes (the base texture, an eye texture): the name with its
// path stripped and its extension, from the last '.', removed, then `extension` added: kTextureExtension for each
// texture, kTextureTwinExtension for the base texture's twin, each through the stage loader [orig:
// Shadow_DecalLoadTextures @ 0x588040: PathStripPathA, PathRemoveExtensionA, then PathAddExtensionA ".TGA"
// @ 0x5880EA, ".MDT" @ 0x588117, the eyes @ 0x58814A (+520), @ 0x588180 (+260), each through
// Texture_LoadByNameWithChannel].
inline constexpr const char *kTextureExtension = ".TGA";
inline constexpr const char *kTextureTwinExtension = ".MDT";
std::string texture_load_name(const std::string &written, const char *extension);

// The face animation the game opens for a person's model (an item of type 3, when the shadow quality is above 0):
// the model's name with its path stripped and its extension, from the last '.', removed, then ".GRM" added [orig:
// Entity_InitFromModel @ 0x40E211..0x40E22E -> sub_57FDF0 @ 0x57FDF0 -> sub_57FCE0 @ 0x57FCE0: PathStripPathA,
// PathRemoveExtensionA, then PathAddExtensionA ".GRM" @ 0x57FD2E]. No file names one.
inline constexpr const char *kFaceExtension = ".GRM";
std::string face_file_name(const std::string &model);

// Reconstructs the retail writer's CRLF output, field order, spacing and four
// decimal places. Canonical writer output round-trips byte-for-byte; arbitrary
// input comments/layout and excess float precision are not echoed.
// [orig: GRM writer @0x588320]
bool write(const File &file, std::vector<uint8_t> &out, std::string &error);

} // namespace opennova::grm
