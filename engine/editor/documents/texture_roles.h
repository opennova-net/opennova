#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <runtime/renderer/texture_roles.h>

namespace opennova::editor {

// The texture roles (ADR 0046 S18) in the editor's words: every way the game uses a texture file
// (runtime/renderer/texture_roles.h: the role ids and each role's loader, formats, size rule and
// whether it reads the alpha, with their witnesses) with its token on the wire, its words, its
// group, what its alpha means, how it is sampled, what the game does when the file is wrong or
// missing, and the witness the findings and the panel quote. The checks, the import defaults, the
// panel's words and the wire read it.

// A loader's token on the wire ("stage", "archive", "hud", "ptl", ...).
const char *texture_loader_token(renderer::TextureLoader loader);

enum class TextureRoleGroup : uint8_t { Model, Terrain, Environment, Effects, Hud, Menus, kCount };
const char *texture_role_group_words(TextureRoleGroup group);

// One role: the engine's row (its loader, formats, size rule and alpha) and its words: its token on
// the wire and its words ("model diffuse"), its group, what the alpha means there, how it is sampled,
// what the game does with a wrong or missing file, and the witness the findings and the panel quote.
struct TextureRoleRow : renderer::TextureRole {
	const char *token = "";
	const char *words = "";
	TextureRoleGroup group = TextureRoleGroup::Model;
	const char *alpha = "";
	const char *sampling = "";
	const char *missing = "";
	const char *witness = "";
};

const TextureRoleRow &texture_role_row(renderer::TextureRoleId id);
// The role a token names; false for none.
bool texture_role_from_token(const std::string &token, renderer::TextureRoleId &out);
// A size rule in words ("1024 x 1024", "powers of two", "64-pixel cells").
std::string texture_size_words(const TextureRoleRow &row);
const char *texture_size_rule_token(renderer::TextureSizeRule rule);
// A role on the wire (the texture_roles query): its token, words, group, loader, formats, size
// {rule, words, width, height}, alpha, sampling, missing, whether its loader reads the alpha, witness.
io::JsonValue texture_role_json(const TextureRoleRow &row);

// What a texture reference gives its loader besides the name (GraphEdge::loader_arg,
// FieldUse::loader_arg, reference_file_candidates): a model texture row its row's type (0 to 255,
// renderer::material_texture_source picks by it); a use of another referrer its role, from
// kTextureRoleArg up, with what the referrer's own content says of it in the bits above: the game
// refuses the mission without the file (kTextureArgGates: a terrain's colour map, its blend map), or the
// name is a mission's tile set, whose extension the game makes TGA
// (kTextureArgTileSet). -1: a use whose loader is not witnessed yet, the name as written.
inline constexpr int32_t kTextureRoleArg = 0x100;
inline constexpr int32_t kTextureArgGates = 0x10000;
inline constexpr int32_t kTextureArgTileSet = 0x20000;
// The name's extension made PCX before its loader reads it (an environment's sky maps [orig:
// TimeOfDay_ParseProperty @ 0x57CC41..0x57CC4B, Path_ReplaceOrAppendExtension @ 0x53C780]).
inline constexpr int32_t kTextureArgPcx = 0x40000;
// The name's path stripped and its extension, from its last '.', made .TGA before its loader reads it (a face
// animation's textures [orig: Shadow_DecalLoadTextures @ 0x588040, PathStripPathA, PathRemoveExtensionA,
// PathAddExtensionA ".TGA"]; formats/grm texture_load_name).
inline constexpr int32_t kTextureArgFaceTga = 0x80000;
int32_t texture_role_arg(renderer::TextureRoleId role, int32_t flags = 0);
// Whether the argument is a model texture row's type.
inline bool texture_arg_is_row_type(int32_t loader_arg) { return loader_arg >= 0 && loader_arg < kTextureRoleArg; }
// The role an argument names; false for a row's type or none.
bool texture_arg_role(int32_t loader_arg, renderer::TextureRoleId &role);
inline bool texture_arg_gates(int32_t loader_arg) { return loader_arg > 0 && (loader_arg & kTextureArgGates) != 0; }

// What a model texture row says of its use beyond its type (FieldUse::use_context, GraphEdge::use_context):
// its slot, its flags (the flipbook bit), its material's flags (alpha test, inverted) and alpha-test
// reference, packed a byte each; a slot of 0 (none is) for no row.
struct TextureRowContext {
	uint8_t slot = 0, row_flags = 0, material_flags = 0, alpha_ref = 0;
};
inline uint32_t pack_texture_row_context(const TextureRowContext &row) {
	return uint32_t(row.slot) | uint32_t(row.row_flags) << 8 | uint32_t(row.material_flags) << 16 |
	       uint32_t(row.alpha_ref) << 24;
}
inline TextureRowContext unpack_texture_row_context(uint32_t packed) {
	return {uint8_t(packed), uint8_t(packed >> 8), uint8_t(packed >> 16), uint8_t(packed >> 24)};
}

const char *texture_alpha_meaning_token(renderer::TextureAlphaMeaning meaning);
// Its words: "the specular brightness VS_PHONGT reads, not transparency".
std::string texture_alpha_meaning_words(renderer::TextureAlphaMeaning meaning, const std::string &shader, uint8_t alpha_ref, bool inverted);

} // namespace opennova::editor
