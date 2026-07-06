#pragma once

// GLSL source generator for object materials.  Pure C++ - produces shader
// strings the runtime/server can also consume (e.g. for headless texture
// validation, server-side dry-run rendering, or porting to a different
// engine without depending on Godot's Shader resource type).
//
// The Godot side (godot/engine/object/nova_object_shader_cache.cpp) wraps
// these strings into Godot Shader resources and caches them per key.
//
// Ported from the pre-repo prototype's nova_shader_cache compose_* helpers
// helpers, simplified to a single Normal-pass (no per-light passes - that
// belongs to the runtime layer and gets a separate composer when added).

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
	// vsTracer soft edge: unlit color x |dot(eye, normal)|^2
	// [orig: Tracer.fx vsTracer — D-RMAT-2, ported at REN-4].
	// (The glow-copy capability — is_glow_capable — is deliberately NOT a key
	// bit: it selects the Q3/bloom duplicate, not the composed look.)
	OSCAP_VIEW_FADE    = 0x00010000u,
};

using ObjectShaderKey = uint32_t;

// Pack a classification into a 32-bit cache key.
ObjectShaderKey build_object_shader_key(const ObjectMaterialClassification &cls);

// Decode the family back out of a key (so callers can branch on it without
// re-classifying).
ObjectShaderFamily decode_object_shader_family(ObjectShaderKey key);
ObjectBlendMode decode_object_shader_blend(ObjectShaderKey key);

// Compose the Godot-flavoured GLSL source for a key.  The output uses
// `shader_type spatial;` and Godot-specific built-ins (CAMERA_POSITION_WORLD,
// MODEL_MATRIX, ALBEDO, ALPHA, TIME).  The runtime/server uses are expected
// to be either rendering through Godot or syntactically replacing those
// built-ins for their target backend.
std::string compose_object_shader_glsl(ObjectShaderKey key);

} // namespace renderer
