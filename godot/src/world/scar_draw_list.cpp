#include "world/scar_draw_list.h"
#include "util/axes.h"
#include "util/color_convert.h"
#include "util/record_bind.h"

#include <runtime/renderer/scar_draw_list.h>
#include <runtime/world/entity.h>
#include <runtime/world/impact_scar.h>

#include <vector>

using namespace godot;

void ScarDrawList::_bind_methods() {
	SCAR_DRAW_LIST_FIELDS(OPENNOVA_RECORD_FIELD)
	BIND_CONSTANT(FLAG_ENTITY_LOCAL);
	BIND_CONSTANT(FLAG_BUILDING);
}

Ref<ScarDrawList> ScarDrawList::from_compiled(const opennova::renderer::ScarDrawList &p_list,
		const CompiledFrame &p_frame) {
	Ref<ScarDrawList> out;
	out.instantiate();
	PackedVector3Array vertices;
	PackedVector2Array uvs;
	PackedColorArray colors;
	vertices.resize(static_cast<int64_t>(p_list.vertices.size()));
	uvs.resize(static_cast<int64_t>(p_list.vertices.size()));
	colors.resize(static_cast<int64_t>(p_list.vertices.size()));
	// Two frames, two swaps. A shared-ring (world) slot is mission space, so it
	// takes the world fold mission (x, y, z) -> Godot (x, z, -y). An entity-ring
	// slot is SECTION-LOCAL: the hit went through the inverse of the live
	// collision section matrix, whose local side is the 3DI SOURCE frame (Z up,
	// the frame the section matrix rotates into mission space), and the
	// section's render-part node carries its mesh as Godot (source y, source z,
	// source x). The entity-local batches therefore take that model fold,
	// (x, y, z) -> (y, z, x). The earlier (-x, y, z) fold read the slot as the
	// already-decoded (-source y, source z, source x) frame and dropped every
	// vehicle/item scar below and beside its struck face. A compile in the
	// device's own space (CompiledFrame::mission_space false) takes neither.
	std::vector<bool> entity_local_vertex(p_list.vertices.size(), false);
	for (const opennova::renderer::ScarDrawBatch &b : p_list.batches) {
		if (!b.entity_local) {
			continue;
		}
		const size_t end = b.first_vertex + b.vertex_count;
		for (size_t k = b.first_vertex; k < end && k < entity_local_vertex.size(); ++k) {
			entity_local_vertex[k] = true;
		}
	}
	// The winding fold that rides the coordinate folds. The witnessed quad
	// order's coordinate cross product points AGAINST the struck face's normal
	// in the engine's frame: the slot writer's handedness fix makes the
	// (tangent, bitangent, normal) triple LEFT-handed [orig: Scar_AddEntry
	// @0x5CCAE6..0x5CCB37 — the tangent flips while n . (b x t) < 0, see
	// docs/world/world-wac-ai-re.md §24.9; pinned by ctest impact_scar].
	// Retail uploads through a REFLECTION
	// (Math_FixedPointToFloat3_YNegated @0x611210, y -> -y) into D3D's
	// left-handed, clockwise-front frame, which makes the quad a front face on
	// the normal side under the drawer's CCW cull — the mark shows on the face
	// you shot and not through the wall behind it. Godot's front face is
	// clockwise too, but its frame is right-handed: a triangle is front-facing
	// when its coordinate cross product points AWAY from the viewer. The two
	// folds below, world (x, y, z) -> (x, z, -y) and entity-local
	// (x, y, z) -> (y, z, x), are both ROTATIONS that preserve the engine-frame
	// relation, so the witnessed order is already front on the struck side and
	// both rings keep it. The scorch shader's cull_back then culls exactly what
	// retail's CCW cull culls (pinned by ctest impact_scar and
	// godot/tests/scar_present_pass_test.gd).
	for (size_t i = 0; i < p_list.vertices.size(); ++i) {
		const opennova::renderer::ScarVertex &v = p_list.vertices[i];
		if (!p_frame.mission_space) {
			vertices[static_cast<int64_t>(i)] = Vector3(v.x, v.y, v.z);
		} else {
			vertices[static_cast<int64_t>(i)] = entity_local_vertex[i]
					? Vector3(v.y, v.z, v.x)
					: mission_to_godot(v);
		}
		uvs[static_cast<int64_t>(i)] = Vector2(v.u, v.v);
		colors[static_cast<int64_t>(i)] = opennova::color_from_argb(v.argb);
	}
	// The entity-ring batches in world space (the slots through the owner's
	// live section matrix): mission space, so the world fold, and the same
	// rotation's winding argument as the shared ring above. A run with no
	// entity rings to resolve leaves its batches, and so this form, out.
	const bool entity_rings = static_cast<bool>(p_frame.owner_identity);
	PackedVector3Array world_vertices;
	if (entity_rings) {
		world_vertices.resize(static_cast<int64_t>(p_list.world_vertices.size()));
		for (size_t i = 0; i < p_list.world_vertices.size(); ++i) {
			const opennova::renderer::ScarVertex &v = p_list.world_vertices[i];
			world_vertices[static_cast<int64_t>(i)] =
					p_frame.mission_space ? mission_to_godot(v) : Vector3(v.x, v.y, v.z);
		}
	}
	PackedInt32Array batch_owner;
	PackedInt32Array batch_texture;
	PackedInt32Array batch_section;
	PackedInt32Array batch_flags;
	PackedInt32Array batch_first;
	PackedInt32Array batch_count;
	PackedInt32Array batch_bms_id;
	PackedInt64Array batch_spawn_origin;
	PackedInt32Array batch_world_first;
	int64_t batches = 0;
	for (const opennova::renderer::ScarDrawBatch &b : p_list.batches) {
		if (entity_rings || !b.entity_local) {
			++batches;
		}
	}
	batch_owner.resize(batches);
	batch_texture.resize(batches);
	batch_section.resize(batches);
	batch_flags.resize(batches);
	batch_first.resize(batches);
	batch_count.resize(batches);
	batch_bms_id.resize(batches);
	batch_spawn_origin.resize(batches);
	batch_world_first.resize(batches);
	int64_t i = 0;
	for (const opennova::renderer::ScarDrawBatch &b : p_list.batches) {
		if (!entity_rings && b.entity_local) {
			continue;
		}
		batch_owner[i] = b.owner_packed;
		batch_texture[i] = b.texture;
		batch_section[i] = b.section;
		batch_flags[i] = (b.entity_local ? FLAG_ENTITY_LOCAL : 0) | (b.building ? FLAG_BUILDING : 0);
		batch_first[i] = static_cast<int32_t>(b.first_vertex);
		batch_count[i] = static_cast<int32_t>(b.vertex_count);
		batch_world_first[i] = b.world_resolved ? static_cast<int32_t>(b.world_first_vertex) : -1;
		// The owner's mission identity for the shell's node resolution (the
		// destruction pass's triple: bms_id + spawn_origin, or the packed
		// handle for runtime-only rows).
		int32_t bms_id = 0;
		int64_t spawn_origin = static_cast<int64_t>(opennova::world::kSpawnOriginNone);
		if (b.entity_local) {
			p_frame.owner_identity(b.owner_packed, bms_id, spawn_origin);
		}
		batch_bms_id[i] = bms_id;
		batch_spawn_origin[i] = spawn_origin;
		++i;
	}
	// The strip table: the TGA name and the GfxShader mode word the loader
	// builds each strip's effect from [orig: Scar_LoadTextures @0x5CC2E0 —
	// modeId 0 -> 0x120651, 1 -> 0x460651, see docs/world/world-wac-ai-re.md
	// §24.9]; the presenter decodes the word into the drawer state
	// (opennova::renderer::decode_scar_strip_mode).
	PackedStringArray strip_names;
	PackedInt32Array strip_mode_words;
	strip_names.resize(opennova::world::kScarTextureStripCount);
	strip_mode_words.resize(opennova::world::kScarTextureStripCount);
	for (int strip = 0; strip < opennova::world::kScarTextureStripCount; ++strip) {
		strip_names[strip] = String(opennova::world::scar_texture_strip_name(strip));
		strip_mode_words[strip] =
				static_cast<int32_t>(opennova::world::scar_texture_strip_mode_word(strip));
	}
	out->set_vertices(vertices);
	out->set_world_vertices(world_vertices);
	out->set_uvs(uvs);
	out->set_colors(colors);
	out->set_batch_owner(batch_owner);
	out->set_batch_texture(batch_texture);
	out->set_batch_section(batch_section);
	out->set_batch_flags(batch_flags);
	out->set_batch_first(batch_first);
	out->set_batch_count(batch_count);
	out->set_batch_bms_id(batch_bms_id);
	out->set_batch_spawn_origin(batch_spawn_origin);
	out->set_batch_world_first(batch_world_first);
	out->set_strip_names(strip_names);
	out->set_strip_mode_words(strip_mode_words);
	out->set_slots_live(static_cast<int>(p_list.slots_live));
	out->set_slots_culled(static_cast<int>(p_list.slots_culled));
	out->set_rings_leased(p_frame.rings_leased);
	return out;
}
