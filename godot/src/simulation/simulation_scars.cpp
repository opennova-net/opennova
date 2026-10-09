// Simulation — the impact-scar draw list: compiles World::scars through the
// portable renderer (engine/runtime/renderer/scar_draw_list.h) into the typed
// packed arrays ScarPresenter uploads (ScarDrawList::from_compiled packs them). The sim stays render-free; the device
// inputs (camera, fog distance, the terrain light colour) arrive from the shell
// per present frame.
#include "simulation/simulation_internal.h"
#include "util/color_convert.h"
#include "world/scar_draw_list.h"

#include <runtime/renderer/scar_draw_list.h>
#include <runtime/world/collision.h>
#include <runtime/world/impact_scar.h>

using namespace sim_internal;

namespace {

// The compile's user: the sim for the owner gate, its world and collision for
// an entity-ring owner's live section matrix — the one the slot writer stored
// the hit through (world/impact_scar.cpp) — for the world-space form.
struct ScarCompileUser {
	const Simulation *sim = nullptr;
	const opennova::world::World *world = nullptr;
	const opennova::world::CollisionWorld *collision = nullptr;
};

bool scar_owner_visible_cb(uint16_t p_owner_packed, bool p_building, void *p_user) {
	const ScarCompileUser *user = static_cast<const ScarCompileUser *>(p_user);
	return user->sim->scar_owner_visible(p_owner_packed, p_building);
}

bool scar_section_matrix_cb(uint16_t p_owner_packed, int p_section,
		opennova::world::CollisionMatrix &r_matrix, void *p_user) {
	const ScarCompileUser *user = static_cast<const ScarCompileUser *>(p_user);
	opennova::world::EntityHandle handle;
	handle.packed = p_owner_packed;
	return user->collision->entity_section_matrix(*user->world, handle, p_section, r_matrix);
}

} // namespace

bool Simulation::scar_owner_visible(uint16_t p_owner_packed, bool p_building) const {
	if (!kernel_) return false;
	opennova::world::EntityHandle handle;
	handle.packed = p_owner_packed;
	return opennova::renderer::scar_owner_visible(kernel_->world.registry.get(handle), p_building,
			[this](opennova::world::EntityHandle building) -> std::optional<uint32_t> {
				if (!kernel_->occlusion.has_instance(building)) return std::nullopt;
				return kernel_->occlusion.section_mask(building);
			});
}

// The weapon Inset pass compiles the scar caches inside its own collect, so
// its owner gate reads its own section masks and its fog box its own eye
// (engine: world/occlusion.h OcclusionView carries the witness): the same
// compile over the Inset view's frame words, the main view's back after.
Ref<ScarDrawList> Simulation::get_scar_draw_list_inset(const Vector3 &p_camera_godot,
		float p_fog_distance, const Color &p_terrain_light) const {
	if (!kernel_) {
		return get_scar_draw_list(p_camera_godot, p_fog_distance, p_terrain_light);
	}
	kernel_->occlusion.select_view(opennova::world::OcclusionView::kInset);
	const Ref<ScarDrawList> out =
			get_scar_draw_list(p_camera_godot, p_fog_distance, p_terrain_light);
	kernel_->occlusion.select_view(opennova::world::OcclusionView::kMain);
	return out;
}

Ref<ScarDrawList> Simulation::get_scar_draw_list(const Vector3 &p_camera_godot,
		float p_fog_distance, const Color &p_terrain_light) const {
	if (!kernel_) {
		Ref<ScarDrawList> out;
		out.instantiate();
		return out;
	}
	opennova::renderer::ScarViewContext ctx;
	// Godot (x, y, z) -> mission (x, -z, y): the ground axes the fog box tests.
	ctx.cam_x = p_camera_godot.x;
	ctx.cam_y = -p_camera_godot.z;
	ctx.fog_distance = p_fog_distance > 0.0f ? p_fog_distance : 0.0f;
	ctx.terrain_light_argb = opennova::argb_from_color_opaque(p_terrain_light);
	ScarCompileUser user;
	user.sim = this;
	user.world = &kernel_->world;
	user.collision = &kernel_->collision;
	ctx.owner_visible = &scar_owner_visible_cb;
	ctx.section_matrix = &scar_section_matrix_cb;
	ctx.user = &user;
	opennova::renderer::ScarDrawList list;
	opennova::renderer::compile_scar_draws(kernel_->world.out.scars, ctx, list);
	// Mission space, the world's entity rings resolved to their owners' mission
	// identity through the registry (ScarDrawList::from_compiled packs both).
	ScarDrawList::CompiledFrame frame;
	frame.mission_space = true;
	frame.owner_identity = [this](uint16_t p_owner_packed, int32_t &r_bms_id,
									int64_t &r_spawn_origin) {
		opennova::world::EntityHandle handle;
		handle.packed = p_owner_packed;
		if (const opennova::world::Entity *owner = kernel_->world.registry.get(handle)) {
			r_bms_id = owner->bms_id;
			r_spawn_origin = static_cast<int64_t>(owner->spawn_origin);
		}
	};
	frame.rings_leased = kernel_->world.out.scars.leased_count();
	return ScarDrawList::from_compiled(list, frame);
}
