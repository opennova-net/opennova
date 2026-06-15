#pragma once

// Object-material shader assembler.  Pure C++, no Godot types: it emits the
// per-key `shader_type`/`render_mode` line and the `#define` block that
// specialises the macro über-shader body.  The body itself (the per-family
// shading math, ported faithfully from the original engine's `.fx` effects)
// lives in real, editable Godot shader files under
// `godot/shaders/object/` (`_object_baseinc.gdshaderinc` + `object.gdshaderinc`),
// mirroring how the original factors one shared header + macro-permuted bodies.
//
// The Godot side (godot/engine/object/nova_object_shader_cache.cpp) prepends
// this prelude to a `#include "res://shaders/object/object.gdshaderinc"` line,
// wraps the result in a Godot Shader resource, and caches it per key.
//
// Witness map + divergence catalog: docs/renderer/renderer-re.md (D-RENDER-*).

#include "renderer/material_classify.h"

#include <cstdint>
#include <string>

namespace renderer {

enum ObjectShaderCapBits : uint32_t {
	OSCAP_BLEND_MASK   = 0x00000003u, // ObjectBlendMode value
	OSCAP_FAMILY_MASK  = 0x0000001cu, // ObjectShaderFamily << 2
	OSCAP_FAMILY_SHIFT = 2u,
	OSCAP_TWO_SIDED    = 0x00000020u,
	OSCAP_ALPHA_TEST   = 0x00000040u,
	OSCAP_ALPHA_INVERT = 0x00000080u,
	OSCAP_EMISSIVE     = 0x00000100u,
	OSCAP_LUMINANCE    = 0x00000200u,
	OSCAP_NORMAL_MAP   = 0x00000400u,
	OSCAP_OBJECT_SPACE = 0x00000800u,
	OSCAP_DETAIL       = 0x00001000u,
	OSCAP_SPECULAR     = 0x00002000u,
	OSCAP_GLASS        = 0x00004000u,
	OSCAP_NORMAL_UV2   = 0x00008000u,
};

using ObjectShaderKey = uint32_t;

// Pack a classification into a 32-bit cache key.
ObjectShaderKey build_object_shader_key(const ObjectMaterialClassification &cls);

// Decode the family back out of a key (so callers can branch on it without
// re-classifying).
ObjectShaderFamily decode_object_shader_family(ObjectShaderKey key);
ObjectBlendMode decode_object_shader_blend(ObjectShaderKey key);

// Compose the per-key shader prelude: the `shader_type spatial;` +
// `render_mode …;` line followed by the `#define OBJ_*` block the body switches
// on.  The caller appends the `#include` of the shared body to make a complete
// Godot shader.  `compose_object_shader_defines` returns just the `#define`
// block (exposed for testing the key->define mapping).
std::string compose_object_shader_prelude(ObjectShaderKey key);
std::string compose_object_shader_defines(ObjectShaderKey key);

} // namespace renderer
