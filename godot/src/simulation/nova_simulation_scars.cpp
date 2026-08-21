// Simulation — the impact-scar draw list: compiles World::scars through the
// portable renderer (engine/runtime/renderer/scar_draw_list.h) into the typed
// packed arrays ScarPresenter uploads. The sim stays render-free; the device
// inputs (camera, fog distance, the terrain light colour) arrive from the shell
// per present frame.
#include "simulation/nova_simulation_internal.h"

#include <renderer/scar_draw_list.h>
#include <world/impact_scar.h>

using namespace novasim;

namespace {

// The owner visibility gate Scar_RenderCache applies (retail: @0x5CD830 — a
// building owner draws while `g_BuildingSectionVisMask[idx] & 0xFFFFFFF` is
// nonzero; any other owner while one of its four containing blink boxes
// (+464..+476) has its section bit set in that building's mask, or outright
// when it sits in none; see docs/world/world-wac-ai-re.md §24.9). Our twins:
// OcclusionWorld::section_mask over the same COBJ-section domain and
// Entity::blink_hits — the packed `((section & 0x1F) | (pool_index << 8)) << 12`
// quads the collision pass stamps.
bool scar_owner_visible(uint16_t p_owner_packed, void *p_user) {
	const Simulation *sim = static_cast<const Simulation *>(p_user);
	return sim->scar_owner_visible(p_owner_packed);
}

inline uint32_t argb_from_color(const Color &p_color) {
	auto byte = [](float v) -> uint32_t {
		const float clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
		return static_cast<uint32_t>(clamped * 255.0f + 0.5f);
	};
	return 0xFF000000u | (byte(p_color.r) << 16) | (byte(p_color.g) << 8) | byte(p_color.b);
}

inline Color color_from_argb(uint32_t p_argb) {
	return Color(static_cast<float>((p_argb >> 16) & 0xFFu) / 255.0f,
			static_cast<float>((p_argb >> 8) & 0xFFu) / 255.0f,
			static_cast<float>(p_argb & 0xFFu) / 255.0f,
			static_cast<float>((p_argb >> 24) & 0xFFu) / 255.0f);
}

} // namespace

bool Simulation::scar_owner_visible(uint16_t p_owner_packed) const {
	if (!world_) {
		return false;
	}
	opennova::world::EntityHandle handle;
	handle.packed = p_owner_packed;
	const opennova::world::Entity *owner = world_->registry.get(handle);
	if (owner == nullptr) {
		return false;
	}
	if (owner->kind == opennova::world::EntityKind::Building) {
		// No occlusion instance = no verdict: the building draws (the same
		// all-visible fold get_building_visibility applies).
		if (!occlusion_world_.has_instance(handle)) {
			return true;
		}
		return (occlusion_world_.section_mask(handle) & 0x0FFFFFFFu) != 0u;
	}
	bool any_hit = false;
	for (const uint32_t hit : owner->blink_hits) {
		if (hit == 0u) {
			continue;
		}
		any_hit = true;
		const int section = static_cast<int>((hit >> 12) & 0x1Fu);
		const int building_slot = static_cast<int>(hit >> 20);
		const opennova::world::EntityHandle building =
				opennova::world::EntityHandle::make(2, building_slot);
		if (!occlusion_world_.has_instance(building)) {
			return true;
		}
		if ((occlusion_world_.section_mask(building) & (1u << section)) != 0u) {
			return true;
		}
	}
	return !any_hit;
}

Dictionary Simulation::get_scar_draw_list(const Vector3 &p_camera_godot,
		float p_fog_distance, const Color &p_terrain_light) const {
	Dictionary out;
	if (!world_) {
		return out;
	}
	renderer::ScarViewContext ctx;
	// Godot (x, y, z) -> mission (x, -z, y): the ground axes the fog box tests.
	ctx.cam_x = p_camera_godot.x;
	ctx.cam_y = -p_camera_godot.z;
	ctx.fog_distance = p_fog_distance > 0.0f ? p_fog_distance : 0.0f;
	ctx.terrain_light_argb = argb_from_color(p_terrain_light);
	ctx.owner_visible = &scar_owner_visible;
	ctx.user = const_cast<Simulation *>(this);
	renderer::ScarDrawList list;
	renderer::compile_scar_draws(world_->scars, ctx, list);

	PackedVector3Array vertices;
	PackedVector2Array uvs;
	PackedColorArray colors;
	vertices.resize(static_cast<int64_t>(list.vertices.size()));
	uvs.resize(static_cast<int64_t>(list.vertices.size()));
	colors.resize(static_cast<int64_t>(list.vertices.size()));
	for (size_t i = 0; i < list.vertices.size(); ++i) {
		const renderer::ScarVertex &v = list.vertices[i];
		// Mission (x, y, z) -> Godot (x, z, -y) for world-space slots; the
		// section-local slots of an entity ring take the same axis swap into the
		// section node's local frame (the model builder applies it to the
		// model's own vertices).
		vertices[static_cast<int64_t>(i)] = Vector3(v.x, v.z, -v.y);
		uvs[static_cast<int64_t>(i)] = Vector2(v.u, v.v);
		colors[static_cast<int64_t>(i)] = color_from_argb(v.argb);
	}
	PackedInt32Array batch_owner;
	PackedInt32Array batch_texture;
	PackedInt32Array batch_section;
	PackedInt32Array batch_flags;
	PackedInt32Array batch_first;
	PackedInt32Array batch_count;
	PackedInt32Array batch_bms_id;
	PackedInt64Array batch_spawn_origin;
	const int64_t batches = static_cast<int64_t>(list.batches.size());
	batch_owner.resize(batches);
	batch_texture.resize(batches);
	batch_section.resize(batches);
	batch_flags.resize(batches);
	batch_first.resize(batches);
	batch_count.resize(batches);
	batch_bms_id.resize(batches);
	batch_spawn_origin.resize(batches);
	for (int64_t i = 0; i < batches; ++i) {
		const renderer::ScarDrawBatch &b = list.batches[static_cast<size_t>(i)];
		batch_owner[i] = b.owner_packed;
		batch_texture[i] = b.texture;
		batch_section[i] = b.section;
		batch_flags[i] = (b.entity_local ? 1 : 0) | (b.building ? 2 : 0);
		batch_first[i] = static_cast<int32_t>(b.first_vertex);
		batch_count[i] = static_cast<int32_t>(b.vertex_count);
		// The owner's mission identity for the shell's node resolution (the
		// destruction pass's triple: bms_id + spawn_origin, or the packed
		// handle for runtime-only rows).
		int32_t bms_id = 0;
		int64_t spawn_origin = static_cast<int64_t>(opennova::world::kSpawnOriginNone);
		if (b.entity_local) {
			opennova::world::EntityHandle handle;
			handle.packed = b.owner_packed;
			if (const opennova::world::Entity *owner = world_->registry.get(handle)) {
				bms_id = owner->bms_id;
				spawn_origin = static_cast<int64_t>(owner->spawn_origin);
			}
		}
		batch_bms_id[i] = bms_id;
		batch_spawn_origin[i] = spawn_origin;
	}
	PackedStringArray strip_names;
	strip_names.resize(opennova::world::kScarTextureStripCount);
	for (int strip = 0; strip < opennova::world::kScarTextureStripCount; ++strip) {
		strip_names[strip] = String(opennova::world::scar_texture_strip_name(strip));
	}
	out["vertices"] = vertices;
	out["uvs"] = uvs;
	out["colors"] = colors;
	out["batch_owner"] = batch_owner;
	out["batch_texture"] = batch_texture;
	out["batch_section"] = batch_section;
	out["batch_flags"] = batch_flags;
	out["batch_first"] = batch_first;
	out["batch_count"] = batch_count;
	out["batch_bms_id"] = batch_bms_id;
	out["batch_spawn_origin"] = batch_spawn_origin;
	out["strip_names"] = strip_names;
	out["slots_live"] = static_cast<int>(list.slots_live);
	out["slots_culled"] = static_cast<int>(list.slots_culled);
	out["rings_leased"] = world_->scars.leased_count();
	return out;
}
