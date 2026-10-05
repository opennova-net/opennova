// The `.o3d` parse (threedi_o3d_model.h) lowered to a target: every limit a
// native file or the game imposes lives here, parameterized by the target's
// ThreediTargetLimits, and nowhere in the text's grammar. The lowering splits a
// mesh into the strips a target holds (a skinned strip's palette by OED's
// first-fit, a strip's triangles and vertices by its words), keeps a vertex's
// influences within what the game blends, carries authored tangent frames,
// checks every name, count and fixed-point extent at the record that gives
// it, and adds the notes the engine's tables and loaders call for (a shader
// outside the shader table, a texture that loads only as a .dds, a register
// outside the CTRL catalog, the user point and seat scans). The output is the
// construction API's ThreediBuildModel, which threedi_build_mint writes.
//
// Errors are what the target cannot hold or load, at the record's line.
// Notes are what it loads all the same: a reduction (influences past what it
// blends, vertices a split leaves no triangle for), the engine's notes. A
// mesh's split into strips is silent: it is how a target holds any mesh.
//
// Only the retail target exists (threedi_retail_limits): a limit is a
// target's field, never a constant here, so a target past 3DI3's words (an
// OpenNova-only one) is a new ThreediTarget and a container its writer reads,
// beside this one (ADR 0052). Authoring tooling, not a port: each limit cites
// the field or the engine code that imposes it.
#pragma once

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

#include <formats/threedi/scene_text.h>
#include <formats/threedi/threedi_build.h>
#include <formats/threedi/threedi_o3d_model.h>

namespace opennova::threedi {

// Whether the engine's shader table knows `tag`, and whether that shader reads
// the TANGENT semantic. The table is the renderer's
// (runtime/renderer/material_descriptor.h, material_descriptor_tangent_lookup),
// which a format library may not include, so the target carries it.
using ThreediShaderLookup = bool (*)(const char *tag, bool &reads_tangents);

// Whether the game's texture loader reads a texture row (its name and authored
// type) only as the `.dds` of its stem: true with `opens` the file it opens and
// `loads` that `.dds`; false for a row it decodes itself, one another loader
// reads, or an empty name. The rule is the renderer's
// (runtime/renderer/material_texture.h, material_texture_dds_only), which a
// format library may not include, so the target carries it.
using ThreediTextureLookup = bool (*)(const char *name, uint8_t type, std::string &opens, std::string &loads);

// The most a target holds of one thing; past it the lowering refuses.
struct ThreediLimit {
	long long max = 0;
};

// The bytes one vertex takes in a target's vertex buffer, by layout.
struct ThreediVertexStride {
	int rigid = 0;
	int rigid_tangents = 0;
	int skinned = 0;
	int skinned_tangents = 0;
};

// A target's limits. [field] marks a 3DI3 word's width, [engine] what the
// game's code does with a value the word holds; the retail values and their
// witnesses are threedi_retail_limits'.
struct ThreediTargetLimits {
	// --- render geometry -------------------------------------------------
	ThreediLimit strip_palette;   // [engine] parts a skinned strip's palette holds
	ThreediLimit influences;      // [engine] influences a skinned vertex blends
	ThreediLimit strip_triangles; // [field] triangles a strip's index count holds
	ThreediLimit strip_vertices;  // [field] vertices a strip's indices reach
	ThreediLimit lod_vertex_bytes; // [engine] bytes of one LOD's vertex buffer (its vertices at vertex_stride)
	ThreediLimit lod_index_bytes;  // [engine] bytes of one LOD's index buffer (two an index)
	ThreediVertexStride vertex_stride; // [engine] what one vertex takes in that buffer
	ThreediLimit lods;            // [engine] render LODs
	ThreediLimit parts;           // [field] parts in a LOD (and sections)
	// --- names and tables --------------------------------------------------
	ThreediLimit model_name;      // [engine] characters of the GHDR name
	ThreediLimit user_point_name; // [engine] characters of a USRP name
	ThreediLimit shader;          // [field] bytes of a shader tag
	ThreediLimit register_name;   // [field] bytes of a CTRL register name
	ThreediLimit register_index;  // [field] a register a byte field names
	ThreediLimit lod_type;        // [field] bytes of an RMDL type
	ThreediLimit texture_rows;    // [field] texture rows of a material
	ThreediLimit texture_name;    // [field] bytes of a texture row's name
	ThreediLimit flipbook_frames; // [field] frames of a texture flipbook
	// --- fixed point ------------------------------------------------------
	ThreediLimit collision_extent; // [field] |x| of a CVRT 8.8 corner (exclusive)
	ThreediLimit fixed16_extent;   // [field] |x| of a 16.16 value (exclusive)
	ThreediLimit fixed14_extent;   // [field] |x| of a Q14 normal component (exclusive)
	// --- collision and occlusion ------------------------------------------
	ThreediLimit section_vertices;   // [field] corners a bullet face's indices reach
	ThreediLimit section_normals;    // [field] normals a bullet face's index reaches
	ThreediLimit occlusion_vertices; // [field] vertices of an occlusion record
	ThreediLimit occlusion_planes;   // [engine] planes of an occlusion record
	// --- lights -----------------------------------------------------------
	ThreediLimit light_rate; // [field] a colour generator's rate (exclusive)
	ThreediLimit light_cone; // [field] a spot cone's half-angle in degrees (exclusive)
	// --- what the engine reads of a list it loads whole (notes, not errors) --
	ThreediLimit user_point_scan; // [engine] user points the attach scan reads
	ThreediLimit seat_scan;       // [engine] `sitex` seats the seat scan reads
};

// The retail (Joint Operations) target's limits.
ThreediTargetLimits threedi_retail_limits();

// What a model is built for: the limits, and the engine's tables a format
// library cannot include (each may be null: nothing is noted of it).
struct ThreediTarget {
	ThreediTargetLimits limits;
	ThreediShaderLookup shaders = nullptr;
	ThreediTextureLookup textures = nullptr;
};

// Lower `model` (a clean read) to `target` into `out`. Every error and note
// lands in `findings`, by line; true when none is an error.
bool threedi_o3d_lower(const ThreediO3dModel &model, const ThreediTarget &target, ThreediBuildModel &out,
		std::vector<SceneFinding> &findings);

// Read (threedi_o3d_read), lower, mint (threedi_build_mint) and read the
// minted bytes back through the retail-shape reader. True with `out` the
// model's bytes; false with why in `findings`.
bool threedi_o3d_build(std::istream &text, const ThreediTarget &target, std::vector<uint8_t> &out,
		std::vector<SceneFinding> &findings);

} // namespace opennova::threedi
