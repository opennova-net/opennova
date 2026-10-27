#include <array>
#include <runtime/mission/collision_resolve.h>

#include <runtime/renderer/object_lod.h> // the RLOD threshold carrier
#include <runtime/world/model_geometry.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_panm_pose.h> // the native PANM liveness gate (S3, ADR 0028)

#include <base/io/bam.h>
#include <base/io/strutil.h>
#include <base/resource_index/resource_index.h>
#include <formats/mission/mission.h> // kItemIdOffset
#include <runtime/anim/adm_fallback.h>
#include <runtime/mission/seat_spec_extract.h> // the userpoint names
#include <runtime/world/person_overlays.h> // kParachuteItemTypeId
#include <runtime/world/present_passes.h> // final_husk_graphics

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace opennova::def;
using namespace opennova::threedi;

namespace opennova::mission {

void CollisionResolveState::clear() {
	model_by_graphic.clear();
	occlusion_by_graphic.clear();
	radius_q16_by_graphic.clear();
	collision_block_by_graphic.clear();
	half_xy_by_graphic.clear();
	center_by_graphic.clear();
	husk_kz_points_by_graphic.clear();
	husk_dead_points_by_graphic.clear();
	husk_pieces_by_graphic.clear();
	resolution_attempted.clear();
}

const DefItemDef *find_item_def(const DefItemsFile &items, int item_id) {
	// The first row carrying the id, scanning from row 0: a later duplicate is
	// never reached. [orig: ItemList_FindIndexByTypeId @0x49E100 — `cmp
	//  [ecx],esi; jz` @0x49E120..0x49E122 returns on the first hit]
	for (size_t i = 0; i < items.count; ++i)
		if (items.entries[i].id == item_id) return &items.entries[i];
	return nullptr;
}

std::unordered_map<int, const DefItemDef *> item_defs_by_id(const DefItemsFile &items) {
	// emplace keeps the first row of an id: find_item_def's scan from row 0.
	std::unordered_map<int, const DefItemDef *> rows;
	rows.reserve(items.count);
	for (size_t i = 0; i < items.count; ++i) rows.emplace(items.entries[i].id, &items.entries[i]);
	return rows;
}

int32_t collision_model_for_graphic(CollisionResolveState &state,
		const CollisionResolveDeps &deps, const std::string &graphic_key) {
	auto it = state.model_by_graphic.find(graphic_key);
	if (it != state.model_by_graphic.end()) return it->second;
	int32_t model_id = -1;
	int32_t occlusion_id = -1;
	int32_t bound_radius_q16 = 0;
	bool has_collision_block = false;
	std::pair<float, float> half_xy{0.0f, 0.0f};
	std::array<int32_t, 3> center{0, 0, 0};
	if (deps.models.has_source()) {
		// The mounted store supplies the same immutable model to simulation
		// and presentation; collision geometry and instances stay in the world.
		if (const auto m3 = deps.models.model(graphic_key)) {
			has_collision_block = m3->collision != nullptr;
			world::CollisionModel model;
			if (world::collision_model_from_3di(m3->collision, model,
					world::model_has_collision(*m3))) {
				model_id = deps.collision.add_model(std::move(model));
				// The pose provider retains this model independently of the
				// store cache and evaluates it from live entity state.
				if (threedi_panm_lod_has_live(*m3, 0))
					deps.pose.register_generic_model(model_id, m3);
				// Every parsed model serves the userpoint leg (PANM or not).
				deps.pose.register_userpoint_model(model_id, m3);
			}
			world::OcclusionModel occ;
			if (world::occlusion_model_from_3di(*m3, occ))
				occlusion_id = deps.occlusion.add_model(std::move(occ));
			bound_radius_q16 = world::model_bound_radius_q16_from_3di(*m3);
			// The minimap blip-size source: the CMDL bound-block ground-axis
			// half extents. [orig: Minimap_DrawBlip @0x5979a2..0x5979b8 —
			//  model+176: half = (max - min) >> 1 per ground axis]
			if (m3->collision != nullptr) {
				const ThreediCollisionModelData &bd =
						m3->collision->model_data;
				// File ground axes -> mission plane: mission X rides file Y,
				// mission Y rides file X (the threedi position swizzle);
				// extents are sign-agnostic.
				half_xy.first = (bd.bbox[4] - bd.bbox[1]) * 0.5f;
				half_xy.second = (bd.bbox[3] - bd.bbox[0]) * 0.5f;
				if (half_xy.first < 0.0f) half_xy.first = 0.0f;
				if (half_xy.second < 0.0f) half_xy.second = 0.0f;
				// The bbox CENTER stays in model-local axes: retail adds
				// it to the world position unrotated, and its own value is
				// min + (max - min)/2 over the same collision bounds.
				// [orig: Entity_InitFromModel @0x40df1e..0x40df4a]
				const auto fixed = [](float value) {
					return static_cast<int32_t>(
							std::lround(static_cast<double>(value) * 65536.0));
				};
				for (int axis = 0; axis < 3; ++axis) {
					const int32_t lo = fixed(bd.bbox[axis]);
					const int32_t hi = fixed(bd.bbox[axis + 3]);
					center[axis] = lo + ((hi - lo) >> 1);
				}
			}
		}
	}
	state.model_by_graphic.emplace(graphic_key, model_id);
	state.occlusion_by_graphic.emplace(graphic_key, occlusion_id);
	state.radius_q16_by_graphic.emplace(graphic_key, bound_radius_q16);
	state.collision_block_by_graphic.emplace(graphic_key, has_collision_block);
	state.half_xy_by_graphic.emplace(graphic_key, half_xy);
	state.center_by_graphic.emplace(graphic_key, center);
	return model_id;
}

// The def's twelve weapon userpoint names resolved on the entity's model into
// the four slot x three field byte cluster: slot 0 <- weapr?up (def[3..5]),
// 1 <- weapl?up (def[0..2]), 2 <- weapr?up2 (def[9..11]), 3 <- weapl?up2
// (def[6..8]); then the zero-fill [3..5] <- [0..2], [6..8] <- [0..2],
// [9..11] <- [3..5] in that order. Names match case-insensitively; a byte is
// the 1-based table index (0 = none). The thirteenth byte (+0x333) is the
// slot def's heat_effect name (weapon.def+0x358) on the same model, stored
// after the fill: the overheat emitter's anchor.
// [orig: Entity_InitBoneReferences @0x441470 (@0x4414e0..0x4415aa, the
//  heat name @0x4415C2..0x4415CD);
//  Entity_ResolveBoneUserpoints @0x545940; ModelGPM_FindUserpointByName
//  @0x5b21ef]
static uint8_t userpoint_index_by_name(const Threedi3di3 &model, const char *name) {
	if (name == nullptr || name[0] == '\0') return 0;
	const int found = threedi_3di3_find_user_point(&model, name);
	return found >= 0 && found < 255 ? static_cast<uint8_t>(found + 1) : 0;
}

// A loaded model's userpoint records as the runtime model keeps them (every
// record, raw 16.16 position and direction, the name cut at its NUL)
// [orig: the model's +0xBC count and +0xC0 48-byte records the GLASS leg walks,
//  Terrain_SpawnSurfaceEffectsAtUserPoints @0x5CEB43..0x5CEB49].
static void model_user_points(const Threedi3di3 &model, std::vector<world::ModelUserPoint> &out) {
	out.clear();
	if (model.user_points == nullptr) return;
	out.reserve(model.user_point_count);
	for (size_t i = 0; i < model.user_point_count; ++i) {
		const ThreediUserPoint &p = model.user_points[i];
		world::ModelUserPoint point;
		point.name = std::string(threedi_user_point_name(p));
		point.pos_q16[0] = p.x;
		point.pos_q16[1] = p.y;
		point.pos_q16[2] = p.z;
		point.dir_q16[0] = p.rot_x;
		point.dir_q16[1] = p.rot_y;
		point.dir_q16[2] = p.rot_z;
		out.push_back(std::move(point));
	}
}

// Whether the GLASS leg's decal gate refuses a loaded model: its LOD-0 render model (the
// runtime model's +0x20) is absent, which a GHDR LOD count of none leaves, or carries +0x10
// bit 0, which every render LOD of a skinned model (GHDR mesh type 2) carries.
// [orig: ThreediGp_LoadModel @0x5B6160 returns def+4 (@0x5B6273), so +0x20 is def+0x24;
//  ThreediGp_LoadFromFile @0x5B5780 - the GHDR LOD count to def+0x14 @0x5B597B, none skips the
//  loads @0x5B5B82 and zeroes the slots @0x5B5BC6, LOD 0's render model stored to def+0x24 (the
//  lea @0x5B5B84, the store @0x5B5B94); mesh type 2 -> def+0xF0 = 3 @0x5B5903..0x5B5922;
//  GPM_LoadRenderModel @0x5B5000 sets the render model's +0x10 bit 0 under def+0xF0 == 3
//  @0x5B50BA..0x5B50C3. A model with no RLOD does not load here.]
static bool lod0_refuses_decal(const Threedi3di3 &model) {
	return model.header.lod_count_decl <= 0 || model.header.mesh_type == THREEDI_MESH_SKINNED;
}

// Whether a loaded model's scar-cache bone callback returns no matrix array. The model load
// binds the runtime model's +0xAC from LOD 0's RMDL tag (the RMDL chunk's first word, copied to
// def+0x64) through the bone table's third column, row 0's on a miss, so it is never null; of
// the table's 19 rows only the 'pwp0' row's third column returns null, every other one
// g_BoneCallbackScratchMatrix. The row's packed word is stored as the bytes "0pwp".
// [orig: EntityDef_LoadModelsAndCallbacks @0x439F50 - the graphic model's lookup @0x43A03C and
//  +0xAC store @0x43A047, a husk model's @0x43A10D / @0x43A11B; BoneCallbackTable_LookupByTag
//  @0x4E3310 (row 0's on a miss @0x4E3334); GPM_LoadRenderModel @0x5B5000 - the RMDL word to the
//  render model's +0 @0x5B50A9; ThreediGp_LoadFromFile @0x5B5780 - to def+0x64 @0x5B5BDC;
//  g_BoneCallbackTable @0x82CF00, 19 rows (@0x82D030), row 'pwp0' @0x82CF50, its third column
//  @0x82CF5C -> BoneCallback_pwp0_Bone @0x4E29E0 (xor eax, eax)]
static bool scar_bones_null(const Threedi3di3 &model) {
	static constexpr char kPwp0[4] = {'0', 'p', 'w', 'p'};
	return model.lods != nullptr && model.lod_count > 0 &&
			std::memcmp(model.lods[0].model_type, kPwp0, sizeof(kPwp0)) == 0;
}

static void resolve_weapon_userpoint_bytes(const DefItemDef &def,
		const Threedi3di3 &model, const world::WeaponTableEntry *slot_def,
		uint8_t (&cluster)[4][3], uint8_t &heat) {
	static constexpr int kSlotDefBase[4] = {3, 0, 9, 6};
	uint8_t bytes[12] = {};
	for (int slot = 0; slot < 4; ++slot) {
		for (int field = 0; field < 3; ++field) {
			bytes[slot * 3 + field] = userpoint_index_by_name(
					model, def.weapon_userpoints[kSlotDefBase[slot] + field]);
		}
	}
	for (int field = 0; field < 3; ++field) {
		if (bytes[3 + field] == 0) bytes[3 + field] = bytes[field];
		if (bytes[6 + field] == 0) bytes[6 + field] = bytes[field];
		if (bytes[9 + field] == 0) bytes[9 + field] = bytes[3 + field];
	}
	for (int slot = 0; slot < 4; ++slot)
		for (int field = 0; field < 3; ++field)
			cluster[slot][field] = bytes[slot * 3 + field];
	heat = slot_def != nullptr ? userpoint_index_by_name(model, slot_def->heat_effect.c_str()) : 0;
}

// The def's `input_function` row and its `virtualdisplay <model> <userpoint>`
// camera. Retail matches the name over EVERY userpoint of the virtual-display
// model and keeps the LAST hit as the 1-based byte def+0x1C0; the camera
// callback then reads that record's position.
// [orig: EntityDef_LoadModelsAndCallbacks @0x43A5D3..0x43A644 (gate: model
//  def+0x12C && name def+0xE0); input rows @0x829DA8 null / troop / tank]
static void resolve_virtual_display_camera(const DefItemDef &def,
		const CollisionResolveDeps &deps, world::Entity &e) {
	e.input_class = strutil::iequals(def.input_function, "tank") ? 2
			: strutil::iequals(def.input_function, "troop")	  ? 1
															  : 0;
	e.virtual_display_camera = false;
	e.virtual_display_model.clear();
	if (def.virtual_display[0] == '\0' || def.virtual_display_userpoint[0] == '\0') return;
	const std::string display_key = strutil::to_lower(def.virtual_display);
	const Threedi3di3 *display = deps.models.model(display_key).get();
	if (display == nullptr || display->user_points == nullptr) return;
	e.virtual_display_model = display_key;
	for (size_t i = 0; i < display->user_point_count; ++i) {
		const ThreediUserPoint &point = display->user_points[i];
		if (!strutil::iequals(point.name, def.virtual_display_userpoint)) continue;
		e.virtual_display_camera = true;
		e.virtual_display_camera_q16[0] = point.x;
		e.virtual_display_camera_q16[1] = point.y;
		e.virtual_display_camera_q16[2] = point.z;
	}
}

world::ResolvedCollisionShape collision_shape_for_runtime_type(
		int runtime_item_id, const DefItemsFile &items,
		CollisionResolveState &state, const CollisionResolveDeps &deps) {
	world::ResolvedCollisionShape shape;
	const int visual_item_id = mission::visual_item_id_for_runtime_type(runtime_item_id);
	const DefItemDef *def = find_item_def(items, visual_item_id);
	if (def == nullptr) return shape;
	shape.pool1_candidate_source_eligible =
			(def->attrib & world::kItemAttribEweap) == 0 || def->type == 1;
	shape.item_type = static_cast<uint8_t>(def->type);
	if (def->graphic[0] == '\0') return shape;

	const std::string key(def->graphic);
	shape.model_id = collision_model_for_graphic(state, deps, key);
	shape.uniform_scale_q16 = def->scale_q16;
	shape.has_collision_block = state.collision_block_by_graphic[key];
	// The row's render model, as resolve_collision_instances stamps a placed
	// entity's (the same producer, the entity-init form).
	if (const Threedi3di3 *render_model =
				deps.models.has_source() ? deps.models.model(key).get() : nullptr) {
		const renderer::ObjectProjectionSphere sphere =
				world::collision_projection_sphere_from_3di(*render_model, 0, 0,
						world::item_def_zero_bbox_center(def->type, def->attrib));
		shape.has_render_model = true;
		shape.render_sphere_center_q16 = world::FixedVec3{
				sphere.center_q16[0], sphere.center_q16[1], sphere.center_q16[2]};
		shape.render_sphere_radius_q16 = sphere.radius_q16;
	}
	if (!shape.has_collision_block) return shape;

	world::EntityBoundRadiusInputs bound;
	bound.model_radius_q16 = state.radius_q16_by_graphic[key];
	bound.uniform_scale_q16 = shape.uniform_scale_q16;
	bound.has_collision_block = shape.has_collision_block;

	// Only the FIRST husk participates in Entity_InitFromModel's max. The
	// huskFinal pointer belongs to the later piece/death chain and is not a
	// compatibility substitute for a missing first husk here.
	// [orig: Entity_InitFromModel @0x40dc30, first-husk max @0x40e062..0x40e06f]
	if (def->husk[0] != '\0' && deps.models.has_source()) {
		const std::string husk_key(def->husk);
		auto radius_it = state.radius_q16_by_graphic.find(husk_key);
		if (radius_it == state.radius_q16_by_graphic.end()) {
			int32_t husk_bound_q16 = 0;
			if (const Threedi3di3 *husk = deps.models.model(husk_key).get()) {
				husk_bound_q16 = world::model_bound_radius_q16_from_3di(*husk);
			}
			radius_it = state.radius_q16_by_graphic.emplace(
					husk_key, husk_bound_q16).first;
		}
		bound.has_first_husk = true;
		bound.first_husk_radius_q16 = radius_it->second;
	}
	shape.bound_radius_q16 = world::entity_bound_radius_q16(bound);

	if (!world::item_def_zero_bbox_center(def->type, def->attrib)) {
		const std::array<int32_t, 3> &center = state.center_by_graphic[key];
		int32_t scaled[3] = {center[0], center[1], center[2]};
		if (shape.uniform_scale_q16 != 0) {
			for (int axis = 0; axis < 3; ++axis)
				scaled[axis] = io::mul_q16_rhu(
						scaled[axis], shape.uniform_scale_q16);
		}
		shape.bbox_center_q16 = world::FixedVec3{
				scaled[0], scaled[1], scaled[2]};
	}
	return shape;
}

int resolve_collision_instances(world::World &world, const DefItemsFile &items,
		CollisionResolveState &state, const CollisionResolveDeps &deps) {
	std::vector<world::EntityHandle> handles;
	world.registry.for_each(
			[&](const world::Entity &e) { handles.push_back(e.handle); });
	int attached = 0;
	for (const world::EntityHandle h : handles) {
		world::Entity *e = world.registry.get(h);
		if (!e || e->kind == world::EntityKind::Marker)
			continue;
		const auto previous_attempt = state.resolution_attempted.find(h.packed);
		if (previous_attempt != state.resolution_attempted.end() &&
				previous_attempt->second != e->registry_spawn_id) {
			deps.collision.remove_entity_instance(h);
			deps.pose.remove_entity(h);
		}
		state.resolution_attempted[h.packed] = e->registry_spawn_id;
		const bool is_organic = e->kind == world::EntityKind::Organic;
		// Every body's def is the row its type word names, an organic's included.
		const int def_id = mission::visual_item_id_for_runtime_type(e->item_id);
		const DefItemDef *def = find_item_def(items, def_id);
		// The collectors' render model: every entity whose graphic loads carries
		// its CMDL sphere (the entity-init form, radius 0 without a collision
		// block), whatever its collision geometry; an entity without one is
		// never collected. [orig: Entity_InitFromModel @ 0x40df06..0x40dfac;
		// the entity+0x30 gates @ 0x5c6fd8..0x5c6fe1 / @ 0x5c8cf6..0x5c8cff]
		const Threedi3di3 *render_model =
				def != nullptr && def->graphic[0] != '\0' && deps.models.has_source()
				? deps.models.model(std::string(def->graphic)).get()
				: nullptr;
		// entity+0x30 is what that load returned, null when it failed; for an
		// organic it is its VISUAL def's graphic (the player's authored visual
		// item), which the item-traits stamp keyed on the runtime id cannot
		// see [orig: Game_ReloadEntityModelsAndCallbacks @0x522830 - the
		// pool-0 load @0x522CB1 and +0x30 store @0x522CD1, after a player
		// row's type index takes its visual item; EntityDef_LoadModelsAndCallbacks
		// - the store @0x439FDC; the persistent-bank gate
		// MapOverlay_RenderAllByLayer @0x5BE6C4].
		if (def != nullptr) {
			e->has_graphic_model = render_model != nullptr;
			e->graphic_model_userpoints = render_model != nullptr
					? static_cast<uint32_t>(render_model->user_point_count) : 0;
			e->graphic_model_collision_objects = render_model != nullptr &&
							render_model->collision != nullptr &&
							render_model->collision->model_data.num_objects > 0
					? static_cast<uint32_t>(render_model->collision->model_data.num_objects)
					: 0;
		}
		if (render_model != nullptr) {
			deps.occlusion.assign_render_model(h, world::collision_projection_sphere_from_3di(
					*render_model, 0, 0, world::item_def_zero_bbox_center(def->type, def->attrib)));
		} else {
			deps.occlusion.remove_render_model(h);
		}
		if (def == nullptr || def->graphic[0] == '\0') continue;
		const std::string key(def->graphic);
		const int32_t resolved_model = collision_model_for_graphic(state, deps, key);
		if (auto *traits = world.tables.item_death_traits.get_mutable(e->item_id);
				traits != nullptr && !traits->model_loaded && deps.models.has_source()) {
			if (const Threedi3di3 *model = deps.models.model(key).get()) {
				traits->model_loaded = true;
				const uint16_t fx_mask = threedi_3di3_user_point_mask(model, def->particlefx.userpoint);
				for (size_t i = 0; i < model->user_point_count && i < 16; ++i) {
					if ((fx_mask & (1u << i)) == 0) continue;
					const auto &point = model->user_points[i];
					traits->has_particlefx_point = true;
					traits->particlefx_point_q16[0] = point.x;
					traits->particlefx_point_q16[1] = point.y;
					traits->particlefx_point_q16[2] = point.z;
					traits->particlefx_direction_q16[0] = point.rot_x;
					traits->particlefx_direction_q16[1] = point.rot_y;
					traits->particlefx_direction_q16[2] = point.rot_z;
					break;
				}
				traits->graphic_name = model->header.name;
				model_user_points(*model, traits->graphic_userpoints);
				traits->graphic_lod0_refuses_decal = lod0_refuses_decal(*model);
				traits->graphic_no_collision_block = model->collision == nullptr;
				traits->graphic_scar_bones_null = scar_bones_null(*model);
				traits->model_radius_q16 = world::model_bound_radius_q16_from_3di(*model);
				// GPM+24/+28 remain zero in the retail 3DI3 load path. These
				// legacy fields are distinct from CMDL's XY/Z radii.
				// [orig: ThreediGp_LoadFromFile @0x5B5780: zero +4..+E7,
				// GHDR radius -> raw+24 (GPM+20); wrapper @0x5B6160 returns raw+4]
				traits->model_radius_xy_q16 = 0;
				traits->model_radius_z_q16 = 0;
				if (model->collision != nullptr) {
					traits->model_bounds_loaded = true;
					for (size_t i = 0; i < model->collision->translation_count; ++i) {
						const auto &p = model->collision->translations[i].translation;
						traits->model_pivots_q16.push_back({p[0], p[1], p[2]});
					}
					const auto &bounds = model->collision->model_data;
					for (size_t i = 0; i < model->collision->object_count; ++i) {
						const auto &section = model->collision->objects[i];
						traits->model_section_origins_q16.push_back(
								{section.offset[0], section.offset[1], section.offset[2]});
						traits->model_section_heights_q16.push_back(static_cast<int32_t>(
								uint32_t(section.max[2]) - uint32_t(section.min[2])));
					}
					if (model->collision->object_count > 0) {
						traits->model_section0_min_z_q16 = model->collision->objects[0].min[2];
						traits->model_section0_max_z_q16 = model->collision->objects[0].max[2];
					}
					for (int axis = 0; axis < 3; ++axis) {
						traits->model_min_q16[axis] = static_cast<int32_t>(
								std::lround(double(bounds.bbox[axis]) * 65536.0));
						traits->model_max_q16[axis] = static_cast<int32_t>(
								std::lround(double(bounds.bbox[axis + 3]) * 65536.0));
					}
				}
				// [orig: Entity_InitFromModel @0x40DC30: first SOUND point,
				// def+0x54A one-based byte; transform consumer @0x408290]
				const int sound = threedi_3di3_find_user_point(model, "SOUND");
				if (sound >= 0) {
					const uint8_t one_based = static_cast<uint8_t>(sound + 1);
					if (one_based != 0 && one_based <= 127) {
						float pos[3];
						threedi_user_point_position(&model->user_points[one_based - 1], pos);
						traits->has_sound_point = true;
						traits->sound_point = {pos[2], -pos[0], pos[1]};
					}
				}
			}
		}
		// The bound-sphere radius (entity+0 boundRadius) comes from the exact
		// GHDR model carrier, but the entire stamp is gated by the model's
		// collision block. Raised to the FIRST husk model's unscaled bound below,
		// then padded +0.0625 [orig: Entity_InitFromModel @ 0x40dc30 — boundRadius =
		// max(scale*gpm[5], husk gpm[5]) + 0x1000. The base model bound is
		// scaled BEFORE the signed compare with the first husk model; the husk
		// operand itself is not multiplied in this initializer.
		// [orig: Entity_InitFromModel @ 0x40dc30]
		const bool has_collision_block = state.collision_block_by_graphic[key];
		world::EntityBoundRadiusInputs bound;
		bound.model_radius_q16 = state.radius_q16_by_graphic[key];
		bound.uniform_scale_q16 = e->uniform_scale_q16;
		bound.has_collision_block = has_collision_block;
		// Platform probe boxes (vehicle-client-movers-re.md §3, D-VEH-1):
		// retail's load-time derivation, ported as threedi_3di3_collision_probe_boxes —
		// box Z = the CMDL header bbox Z pair, box X/Y = the lower-half
		// type-1 BVOL fold, footprint = the bottom-eighth fold with the
		// q+0x2000 minimum-extent clamps [orig:
		// Threedi_BuildCollisionModelFromChunks @ 0x5b3bf0, tail
		// @ 0x5b4455..0x5b45db]. The CMDL floor sits at the wheel-contact
		// origin (~0 for wheeled hulls), so the solve rests the ORIGIN on the
		// terrain; the earlier per-COBJ AABB union stand-in floated every
		// hull by its below-origin wheel depth. A model the derivation
		// rejects keeps zeroed boxes and therefore the solves'
		// terrain-clamp stand-in, matching retail's observable for a
		// sentinel-boxed hull (its solve-active test also fails).
		if (h.pool() == 1) {
			world::VehicleTraits *vt =
					world.vehicles.traits.get_mutable(e->item_id);
			if (vt != nullptr) {
				if (const Threedi3di3 *model = deps.models.model(key).get()) {
					vt->trail_point_count =
							uint8_t(std::min(size_t(16), size_t(model->user_point_count)));
					for (auto &trail : vt->trails)
						trail.mask = threedi_3di3_user_point_mask(model, trail.userpoint.c_str());
					for (size_t i = 0; i < vt->trail_point_count; ++i) {
						const auto &point = model->user_points[i];
						vt->trail_points[i] = { { point.x, point.y, point.z },
							{ point.rot_x, point.rot_y, point.rot_z } };
					}
				}
			}
			if (vt != nullptr && vt->skid_points.empty() && !vt->skid_userpoint.empty()) {
				if (const Threedi3di3 *model = deps.models.model(key).get()) {
					const uint16_t mask =
							threedi_3di3_user_point_mask(model, vt->skid_userpoint.c_str());
					for (size_t i = 0; i < model->user_point_count && i < 16; ++i) {
						if ((mask & (1u << i)) == 0)
							continue;
						const auto &point = model->user_points[i];
						vt->skid_points.push_back({ { point.x, point.y, point.z },
								{ point.rot_x, point.rot_y, point.rot_z } });
					}
				}
			}
			if (vt != nullptr && vt->box_z_hi == vt->box_z_lo) {
				const Threedi3di3 *vm3 = deps.models.has_source()
						? deps.models.model(key).get()
						: nullptr;
				if (vm3 != nullptr && vm3->collision != nullptr) {
					ThreediCollisionProbeBoxes boxes;
					if (threedi_3di3_collision_probe_boxes(vm3->collision,
							&boxes) != 0) {
						vt->box_x_lo = boxes.box_x_lo;
						vt->box_x_hi = boxes.box_x_hi;
						vt->box_y_lo = boxes.box_y_lo;
						vt->box_y_hi = boxes.box_y_hi;
						vt->box_z_lo = boxes.box_z_lo;
						vt->box_z_hi = boxes.box_z_hi;
						vt->foot_x_lo = boxes.foot_x_lo;
						vt->foot_x_hi = boxes.foot_x_hi;
						vt->foot_y_lo = boxes.foot_y_lo;
						vt->foot_y_hi = boxes.foot_y_hi;
					}
				}
			}
			if (vt != nullptr) {
				// [orig: Entity_InitVehicleAI @0x460200, first 16 FLARE-prefix points]
				if (vt->flare_points.empty()) {
					if (const Threedi3di3 *model = deps.models.model(key).get()) {
						for (size_t i = 0; model->user_points != nullptr &&
								i < model->user_point_count && vt->flare_points.size() < 16;
								++i) {
							const auto &point = model->user_points[i];
							if ((user_point_uses(threedi_user_point_name(point)) & kUserPointFlare) == 0)
								continue;
							vt->flare_points.push_back({ { point.x, point.y, point.z },
									{ point.rot_x, point.rot_y, point.rot_z } });
						}
					}
				}
				// The gunner-attachment points: the first 16 'agun'-prefix userpoint
				// locals (strnicmp 4; model-local 16.16), which the class init's setup
				// collects from the model's 48-byte userpoint rows and
				// VehicleSystem::setup_gunner_attachments transforms per carrier.
				// [orig: Entity_SetupGunnerAttachments @0x4681AA..0x4681D9]
				if (vt->agun_points.empty()) {
					if (const Threedi3di3 *model = deps.models.model(key).get()) {
						for (size_t i = 0; model->user_points != nullptr &&
								i < model->user_point_count && vt->agun_points.size() < 16;
								++i) {
							const auto &point = model->user_points[i];
							if ((user_point_uses(threedi_user_point_name(point)) & kUserPointGunnerAttachment) == 0)
								continue;
							vt->agun_points.push_back({ { point.x, point.y, point.z },
									{ point.rot_x, point.rot_y, point.rot_z } });
						}
					}
				}
				// brain[11] is the CMDL floor's absolute value. The class init
				// writes it: the helicopter family always, the vehicle family
				// unless the loaded profile is a boat (subtype 1), whatever the
				// profile's type. [orig: Entity_InitHelicopterAIFromDef
				//  @0x4684E2..0x4684F7; Entity_InitVehicleAIFromDef `cmp [edi+14h],
				//  ebx` @0x46881C, store @0x468836]
				const world::AiEntity *vehicle_ai = world.ai.for_handle(e->handle);
				if (vehicle_ai != nullptr &&
						(vt->brain_class == world::VehicleBrainClass::Air ||
								(vt->brain_class == world::VehicleBrainClass::Ground &&
										vehicle_ai->profile.subtype != 1))) {
					e->veh.air_probe_z_off =
							static_cast<int32_t>(vt->box_z_lo < 0 ? 0u - uint32_t(vt->box_z_lo)
																  : uint32_t(vt->box_z_lo));
					if (world::AiEntity *ai = world.ai.for_handle(e->handle))
						ai->brain.f[world::AiBrain::kModelFloor] = e->veh.air_probe_z_off;
				}
			}
		}
		if (resolved_model >= 0) {
			deps.collision.assign_entity(h, resolved_model, e->registry_spawn_id);
			++attached;
			// The aim/LOS origin's TARGET userpoint [orig: Entity_InitFromModel
			// @0x40dd04 -> def+1350]; 0 when the model has none.
			if (const Threedi3di3 *m3 = deps.models.model(key).get()) {
				e->target_userpoint_byte = userpoint_index_by_name(*m3, kUserPointTargetName);
				e->look_userpoint_byte = userpoint_index_by_name(*m3, kUserPointLookName);
				// [orig: Entity_InitVehicleAI @0x460200, three bounded prefix scans]
				if (world::AiEntity *ai = world.ai.for_handle(e->handle)) {
					if (!ai->inf.active) {
						auto &b = ai->brain;
						b.f[55] = b.f[72] = b.f[89] = 0;
						for (size_t index = 0;
								m3->user_points != nullptr && index < m3->user_point_count;
								++index) {
							const uint32_t uses = user_point_uses(threedi_user_point_name(m3->user_points[index]));
							const bool primary = (uses & kUserPointPrimaryMuzzle) != 0;
							const bool secondary = (uses & kUserPointSecondaryMuzzle) != 0;
							const bool flare = (uses & kUserPointFlare) != 0;
							for (int bank = 0; bank < 3; ++bank) {
								const int count = 55 + 17 * bank;
								if ((bank == 0					? primary
													: bank == 1 ? secondary
																: flare) &&
										b.f[count] < 16)
									b.f[count + 1 + b.f[count]++] = int32_t(index + 1);
							}
						}
					}
				}
			}
			if (!is_organic && (def->attrib & world::kItemAttribEweap) != 0u) {
				if (const Threedi3di3 *m3 = deps.models.model(key).get()) {
					// The slot def is the one the class init binds from the entity's
					// weapon name [orig: WeaponSlot_InitFromEntityDef @0x5466C0].
					const int slot_index = e->primary_weapon.empty()
							? -1 : world.tables.weapons.index_of(e->primary_weapon.c_str());
					const world::WeaponTableEntry *slot_def = slot_index >= 0 && slot_index <= 0xFF
							? world.tables.weapons.by_index(static_cast<uint8_t>(slot_index))
							: nullptr;
					// Each cluster is one class row's init's: the ewep row's resolves the
					// standard cluster once its slot binds a weapon, the CHel / cpln init the
					// aircraft cluster; any other row resolves neither, and both read 0.
					// [orig: Entity_InitBoneReferences @0x441470 (the slot's weapon test
					//  @0x441495, the cluster @0x4414CD..0x4415B0, the heat byte @0x4415CD);
					//  Entity_InitAircraftWeaponBones @0x490160 (the 0x20 test @0x49016D, the
					//  cluster @0x4901AF..0x490292, the heat byte under the weapon @0x4902B4),
					//  called from Entity_InitHelicopterAIFromDef @0x4686A2]
					std::memset(e->weapon_userpoint_bytes, 0, sizeof(e->weapon_userpoint_bytes));
					std::memset(e->aircraft_weapon_userpoint_bytes, 0,
							sizeof(e->aircraft_weapon_userpoint_bytes));
					e->heat_userpoint_byte = 0;
					e->aircraft_heat_userpoint_byte = 0;
					e->camera_userpoint_byte = 0;
					if (strutil::iequals(def->ai_function, "ewep") && slot_def != nullptr) {
						// The gun's own first-person camera userpoint, ahead of the cluster
						// under the same test; no other class row's init writes it.
						// [orig: Entity_InitBoneReferences @0x4414A9..0x4414B4 -> +0x318]
						e->camera_userpoint_byte = userpoint_index_by_name(*m3, kUserPointCameraName);
						resolve_weapon_userpoint_bytes(*def, *m3, slot_def,
								e->weapon_userpoint_bytes, e->heat_userpoint_byte);
					}
					if (strutil::iequals(def->ai_function, "chel") ||
							strutil::iequals(def->ai_function, "cpln"))
						resolve_weapon_userpoint_bytes(*def, *m3, slot_def,
								e->aircraft_weapon_userpoint_bytes,
								e->aircraft_heat_userpoint_byte);
				}
			}
			resolve_virtual_display_camera(*def, deps, *e);
			if (is_organic) {
				deps.pose.remove_entity(h);
				// S3 (ADR 0028): the native skeletal source resolves from
				// the retained def rows + the shared native model; the
				// provider validates rig/FK and declines at query time
				// exactly like the unregistered legacy leg when it cannot.
				// The rig's map is the spawn's one .adm load: <stem>.adm,
				// default.adm where that file is not mounted
				// [orig: Entity_SpawnFromItemDef @0x45257c -> AnimMap_LoadAdmFile
				//  @0x40cc40; anim/adm_fallback.h].
				if (deps.models.has_source() && def->anim_def[0] != '\0') {
					const ResourceIndex *index = deps.models.index();
					const std::string adm = anim::adm_load_name(def->anim_def,
							[index](const std::string &file) { return index->has_file(file); });
					deps.pose.register_skeletal_entity(
							h, e->registry_spawn_id, resolved_model,
							adm, deps.models.model(key),
							def->launchups_closeattack);
				}
			}
		}
		// The husk-stage collision model: attached beside the graphic instance so
		// every query swaps to the wreck once Flags & 4 sets. The collision pick
		// is the FIRST husk stage (entity+52 huskModel), not huskFinal [orig: the
		// +52 substitution @ 0x538720 / @ 0x413086; D-AI-7 residual closed]. The
		// final husk (+0x38) collides only once the swap stage promotes it into
		// +0x34 (world::update_husk_swap_stage).
		const std::string first_husk_name(def->husk);
		const std::vector<std::string> final_husk_candidates =
				world::final_husk_graphics(def->husk, def->huskfinal);
		e->final_husk_loaded = false;
		if (!first_husk_name.empty() || !final_husk_candidates.empty()) {
			// Retail keeps live huskModel and huskFinalModel pointers on the
			// entity. A successfully opened model supplies that pointer even when
			// it has no collision block; missing/corrupt assets leave it null.
			// The final husk is the first of its candidates that loads.
			// [orig: Entity_ProcessBuildingDeath @ 0x49442c;
			//  EntityDef_LoadModelsAndCallbacks @0x439F50 (@0x43A145..0x43A26E);
			//  Entity_InitFromItemDef @0x49E550 (@0x49E5A1 / @0x49E5AA)]
			// S3b full: husk models resolve exclusively through the shared
			// assets::AssetStore (ADR 0044).
			const Threedi3di3 *first_husk_m3 = nullptr;
			const Threedi3di3 *final_husk_m3 = nullptr;
			std::string final_husk_name;
			if (deps.models.has_source()) {
				if (!first_husk_name.empty())
					first_husk_m3 = deps.models.model(first_husk_name).get();
				for (const std::string &candidate : final_husk_candidates) {
					final_husk_m3 = deps.models.model(candidate).get();
					if (final_husk_m3 != nullptr) {
						final_husk_name = candidate;
						break;
					}
				}
			}
			// A section clone's or a palm fragment's zeroed template copies the
			// parent's +0x34 alone: it holds no final husk [orig:
			//  Entity_SpawnSectionEntity @0x4402D0 (the memset @0x440322, +0x34
			//  @0x440331..0x440343); Palm_SpawnFragment @0x53C1C0 (the memset
			//  @0x53C2BC); Entity_CloneFromTemplateByType @0x4398A0].
			const bool holds_final = !e->section_clone && !e->palm_fragment;
			e->final_husk_loaded = holds_final && final_husk_m3 != nullptr;
			if (world::ItemDeathTraits *t =
						world.tables.item_death_traits.get_mutable(e->item_id)) {
				t->primary_husk_loaded = first_husk_m3 != nullptr;
				// The primary husk's name and userpoints, the GLASS leg's model
				// under Flags & 4 [orig: Terrain_SpawnSurfaceEffectsAtUserPoints
				//  @0x5CEABC..0x5CEAD5 (entity+0x34)].
				t->husk_name = first_husk_m3 != nullptr ? std::string(first_husk_m3->header.name) : std::string();
				t->husk_userpoints.clear();
				if (first_husk_m3 != nullptr) model_user_points(*first_husk_m3, t->husk_userpoints);
				t->husk_lod0_refuses_decal =
						first_husk_m3 != nullptr && lod0_refuses_decal(*first_husk_m3);
				t->husk_no_collision_block =
						first_husk_m3 != nullptr && first_husk_m3->collision == nullptr;
				t->husk_scar_bones_null =
						first_husk_m3 != nullptr && scar_bones_null(*first_husk_m3);
				t->husk_model_loaded =
						first_husk_m3 != nullptr || final_husk_m3 != nullptr;
				// The final husk's facts, which the +0x34 readers take after the
				// promotion, and the swap stage's words.
				t->final_husk_loaded = final_husk_m3 != nullptr;
				t->final_husk_graphic = final_husk_name;
				t->final_husk_name = final_husk_m3 != nullptr
						? std::string(final_husk_m3->header.name) : std::string();
				t->final_husk_userpoints.clear();
				if (final_husk_m3 != nullptr)
					model_user_points(*final_husk_m3, t->final_husk_userpoints);
				t->final_husk_lod0_refuses_decal =
						final_husk_m3 != nullptr && lod0_refuses_decal(*final_husk_m3);
				t->final_husk_no_collision_block =
						final_husk_m3 != nullptr && final_husk_m3->collision == nullptr;
				t->final_husk_scar_bones_null =
						final_husk_m3 != nullptr && scar_bones_null(*final_husk_m3);
				t->husk_swap_at = def->husk_swap_at;
				t->husk_swap_at_sec = def->husk_swap_at_sec;
				// The PIECE model — huskFinal first [orig: @ 0x4934af
				// huskFinalModel ?: huskModel].
				const Threedi3di3 *piece_m3 =
						final_husk_m3 != nullptr ? final_husk_m3 : first_husk_m3;
				// The interned death masks and all three banks use final-husk first.
				// [orig: Game_ResolveItemMaterialsAndSpawnBoneTrails @0x522EE0]
				if (piece_m3 != nullptr)
					world::death_effect_banks_of(*piece_m3, t->effect_banks);
			}
			const std::string &husk_key = first_husk_name;
			const Threedi3di3 *husk_m3 = first_husk_m3;
			// Main and husk graphics share one complete cache entry. A graphic
			// first encountered as a husk must still retain its collision-block,
			// exact GHDR, center, and occlusion metadata if it is later used as an
			// intact model by another definition. The entity's promotion picks
			// between the two (CollisionWorld's held husk).
			const int32_t husk_model_id = husk_key.empty()
					? -1 : collision_model_for_graphic(state, deps, husk_key);
			const int32_t final_husk_model_id = holds_final && final_husk_m3 != nullptr
					? collision_model_for_graphic(state, deps, final_husk_name) : -1;
			if (resolved_model >= 0) {
				if (husk_model_id >= 0) deps.collision.assign_entity_husk(h, husk_model_id);
				if (final_husk_model_id >= 0)
					deps.collision.assign_entity_final_husk(h, final_husk_model_id);
			}
			// Retail's death-sound tail walks exact, case-insensitive "KZ"
			// user points on the active FIRST husk, not the huskFinal piece
			// model, and queues a radius-5 blast at every match. Cache this
			// metadata separately from collision registration: the same graphic
			// may already be resident as another entity's main model.
			// The retail KZ walker reads entity+52 huskModel. A def with only
			// huskFinal has no KZ source and therefore takes the entity-origin
			// fallback blast.
			if (!first_husk_name.empty()) {
				auto kz_it = state.husk_kz_points_by_graphic.find(husk_key);
				if (kz_it == state.husk_kz_points_by_graphic.end()) {
					std::vector<world::FixedVec3> kz_points;
					if (husk_m3 != nullptr) {
						const Threedi3di3 &hmodel3di = *husk_m3;
						for (size_t up_index = 0;
								hmodel3di.user_points != nullptr && up_index < hmodel3di.user_point_count;
								++up_index) {
							const ThreediUserPoint &point = hmodel3di.user_points[up_index];
							if (!strutil::iequals(point.name, "KZ"))
								continue;
							// The user point's own words, which are mission-local (x, y, z)
							// as the blast reads them. [orig: Entity_QueueKzBlastAtUserPoints
							//  @0x4EABF0 - `mov edx, [edi]` @0x4EAC93, `mov eax, [edi+4]`
							//  @0x4EAC99, `mov ecx, [edi+8]` @0x4EACA0]
							kz_points.push_back(world::FixedVec3{point.x, point.y, point.z});
						}
					}
					kz_it = state.husk_kz_points_by_graphic.emplace(
							husk_key, std::move(kz_points)).first;
				}
				if (world::ItemDeathTraits *t =
							world.tables.item_death_traits.get_mutable(e->item_id);
						t != nullptr && t->kz_points.empty() && !kz_it->second.empty())
					t->kz_points = kz_it->second;

				// The bridge callback walks the same active FIRST husk for exact,
				// case-insensitive "DEAD" points. Keep a distinct cache because
				// these anchors produce presentation effects, not KZ blasts.
				// [orig: Entity_SpawnDeathEffectsAtBones @0x4944c0]
				auto dead_it = state.husk_dead_points_by_graphic.find(husk_key);
				if (dead_it == state.husk_dead_points_by_graphic.end()) {
					std::vector<world::Vec3> dead_points;
					if (husk_m3 != nullptr) {
						const Threedi3di3 &hmodel3di = *husk_m3;
						for (size_t up_index = 0;
								hmodel3di.user_points != nullptr &&
								up_index < hmodel3di.user_point_count;
								++up_index) {
							const ThreediUserPoint &point =
									hmodel3di.user_points[up_index];
							if (!strutil::iequals(point.name, "DEAD")) continue;
							float up_pos[3];
							threedi_user_point_position(&point, up_pos);
							dead_points.push_back(world::Vec3{
									up_pos[2], -up_pos[0], up_pos[1]});
						}
					}
					dead_it = state.husk_dead_points_by_graphic.emplace(
							husk_key, std::move(dead_points)).first;
				}
				if (world::ItemDeathTraits *t =
							world.tables.item_death_traits.get_mutable(e->item_id);
						t != nullptr && t->bridge_dead_points.empty() &&
						!dead_it->second.empty())
					t->bridge_dead_points = dead_it->second;
			}
			// The PIECE model is the LOADED huskFinal model, else the husk
			// model [orig: @ 0x4934af..0x4934c3 huskFinalModel ?: huskModel] —
			// the opposite preference from the collision husk pick above. Its
			// LOD-0 part table feeds the death-piece loop bound [orig:
			// renderObj[8]+52 @ 0x49361a], its collision section 0's z words the
			// wreck's ground rest (below); its level table,
			// COBJ centres and bound radius feed the piece spawn and draw
			// (world::DeathPieceModel). Its own cache, independent of the
			// collision cache: a husk graphic can double as some entity's main
			// graphic, which would leave the joint cache without an entry.
			const Threedi3di3 *piece_m3 =
					final_husk_m3 != nullptr ? final_husk_m3 : first_husk_m3;
			const std::string &piece_key =
					final_husk_m3 != nullptr ? final_husk_name : first_husk_name;
			// (The piece model is the final husk the load took: the husk's "xb"
			// variant comes ahead of huskfinal there too.)
			auto hs = state.husk_pieces_by_graphic.find(piece_key);
			if (hs == state.husk_pieces_by_graphic.end()) {
				CollisionHuskPieceInfo info;
				if (piece_m3 != nullptr && piece_m3->lod_count > 0 &&
				    piece_m3->lods != nullptr) {
					const ThreediLod &lod = piece_m3->lods[0];
					info.sections = static_cast<int32_t>(lod.render_object_count);
					// The RLOD table and each level's section count (model+0x40
					// +4*i / the level mesh's +0x34 [orig:
					// DeathPiece_RenderVisible @ 0x57b882..0x57b8ba;
					// DeathPiece_RenderSection @ 0x57b6d1]).
					info.model.lod_threshold_q16 = renderer::model_lod_thresholds_q16(*piece_m3);
					for (size_t li = 0; li < piece_m3->lod_count; ++li)
						info.model.lod_section_count.push_back(static_cast<int32_t>(
								piece_m3->lods[li].render_object_count));
					// The COBJ section centres (the runtime row's +0x38..+0x40)
					// [orig: the +0x6C array @ 0x4938bf, the centre
					// @ 0x4938c2..0x4938d0; @ 0x57b6f6..0x57b70b].
					if (piece_m3->collision != nullptr) {
						for (size_t ci = 0; ci < piece_m3->collision->object_count; ++ci) {
							const auto &offset = piece_m3->collision->objects[ci].offset;
							info.model.section_origin_q16.push_back(
									{offset[0], offset[1], offset[2]});
						}
					}
					info.model.radius_q16 = world::model_bound_radius_q16_from_3di(*piece_m3);
				}
				// The wreck's rest extents are the collision block's section-0 row: the
				// loader copies each COBJ record's min / max words into a 108-byte row,
				// z min at +0x54 and z max at +0x58 (the model's +0xB0 is the block, its
				// +0x6C the row array), which both falling-death legs and the class
				// init's husk floor read. Words in the model's own (mission) axes, z up.
				// [orig: Threedi_BuildCollisionModelFromChunks @0x5B3BF0 - the row
				//  stores @0x5B4021..0x5B4027 / @0x5B403F..0x5B4045, the block at the
				//  loader's +0xB4 = model+0xB0 (ThreediGp_LoadFromFile @0x5B5B65,
				//  ThreediGp_LoadModel returns the def +4 @0x5B6273);
				//  Entity_ProcessFallingDeathPhysics @0x461E25..0x461E4B]
				if (piece_m3 != nullptr && piece_m3->collision != nullptr &&
				    piece_m3->collision->objects != nullptr &&
				    piece_m3->collision->object_count > 0) {
					info.rest_z_lo_q16 = piece_m3->collision->objects[0].min[2];
					info.rest_z_hi_q16 = piece_m3->collision->objects[0].max[2];
				}
				hs = state.husk_pieces_by_graphic.emplace(
						piece_key, std::move(info)).first;
				// Piece bound = the husk model's CMDL sphere [orig: Entity_InitFromModel @0x40dceb..0x40de16]
				if (piece_m3 != nullptr) {
					const int32_t piece_bound_q16 =
							world::model_bound_radius_q16_from_3di(*piece_m3);
					state.radius_q16_by_graphic.emplace(
							piece_key, piece_bound_q16);
				}
			}
			if (world::ItemDeathTraits *t =
						world.tables.item_death_traits.get_mutable(e->item_id)) {
				const CollisionHuskPieceInfo &info = hs->second;
				if (t->husk_section_count == 0 && info.sections > 0)
					t->husk_section_count = info.sections;
				// The section-piece render pivot walks the PRIMARY husk's COBJ
				// list: a spawned section never carries +0x38 huskFinalModel
				// (Entity_SpawnSectionEntity's memset template @0x440322 stores
				// only [13] = +0x34 huskModel @0x440343), so the +0x38 ?: +0x34
				// pick lands on +0x34; the final husk stands in only where
				// retail would dereference a null +0x34. (The clone's +0x34 is its
				// parent's, which the parent's own draw promotes to the final
				// husk: D-ITEMDEF-22.)
				// [orig: Entity_BuildDeathSectionTransforms @0x492B46 / @0x492B4D;
				//  Render_SectorEntity @0x5C423E..0x5C4244]
				if (t->husk_section_origins_q16.empty()) {
					const Threedi3di3 *piece = first_husk_m3 != nullptr ? first_husk_m3 : final_husk_m3;
					if (piece && piece->collision) {
						for (size_t i = 0; i < piece->collision->object_count; ++i) {
							const auto &p = piece->collision->objects[i].offset;
							t->husk_section_origins_q16.push_back({p[0], p[1], p[2]});
						}
					}
				}
				if (!t->piece_model.loaded() && info.model.loaded())
					t->piece_model = info.model;
				t->husk_rest_z_lo_q16 = info.rest_z_lo_q16;
				t->husk_rest_z_hi_q16 = info.rest_z_hi_q16;
				// brain[12] is the husk floor's absolute value, read from the
				// husk (the final one when it loaded, else the first) by the class
				// init under brain[11]'s gate: the helicopter family always, the
				// vehicle family unless the profile is a boat (subtype 1).
				// [orig: Entity_InitHelicopterAIFromDef @0x4684FA..0x468527;
				//  Entity_InitVehicleAIFromDef @0x468839..0x468858]
				const world::VehicleTraits *husk_vt =
						h.pool() == 1 ? world.vehicles.traits.get(e->item_id) : nullptr;
				world::AiEntity *husk_ai = world.ai.for_handle(e->handle);
				if (husk_vt != nullptr && husk_ai != nullptr && !husk_ai->inf.active &&
						t->husk_model_loaded &&
						(husk_vt->brain_class == world::VehicleBrainClass::Air ||
								(husk_vt->brain_class == world::VehicleBrainClass::Ground &&
										husk_ai->profile.subtype != 1)))
					husk_ai->brain.f[world::AiBrain::kHuskFloor] =
							io::bam_abs(t->husk_rest_z_lo_q16);
			}
			// Only entity+52's FIRST husk model joins this signed max. A
			// huskFinal-only definition has no substitute operand here.
			// [orig: Entity_InitFromModel @0x40dc30, first-husk max @0x40e062..0x40e06f]
			if (first_husk_m3 != nullptr) {
				bound.has_first_husk = true;
				bound.first_husk_radius_q16 =
						world::model_bound_radius_q16_from_3di(*first_husk_m3);
			}
		}
		if (has_collision_block && e->bound_radius <= 0.0f)
			e->bound_radius.q16 = world::entity_bound_radius_q16(bound); // the word (D-NET-507)
		const int32_t occ_id = state.occlusion_by_graphic[key];
		// Entity_ClassifyForMinimap's ordinary-Building branch checks the
		// live graphic model's +0xE0 portal/occlusion pointer. The parsed .3di
		// and the collision/occlusion resolver are the portable ownership seam
		// for that otherwise renderer-private fact.
		e->has_minimap_model_marker = occ_id >= 0;
		// The blip drawer reads the raw model bound block rather than the entity
		// placement matrix, so authored scale deliberately does not fold here.
		// [orig: Minimap_DrawBlip @0x5979a2..0x5979b8]
		const std::pair<float, float> &half_xy = state.half_xy_by_graphic[key];
		e->minimap_half_x_q16 = static_cast<int32_t>(half_xy.first * 65536.0f);
		e->minimap_half_y_q16 = static_cast<int32_t>(half_xy.second * 65536.0f);
		// The +0x1FC LOS ray offset: zeroed for type-6 powerups with attrib
		// bit 5; otherwise the raw fixed midpoint is multiplied by the same
		// effective scale as the entity matrix, with retail's +0x8000 rule.
		// [orig: Entity_InitFromModel @0x40defc..0x40df4a]
		const std::array<int32_t, 3> &bc = state.center_by_graphic[key];
		if (!world::item_def_zero_bbox_center(def->type, def->attrib)) {
			int32_t scaled[3] = {bc[0], bc[1], bc[2]};
			if (e->uniform_scale_q16 != 0) {
				for (int axis = 0; axis < 3; ++axis) {
					scaled[axis] = io::mul_q16_rhu(
							scaled[axis], e->uniform_scale_q16);
				}
			}
			e->bbox_center = world::Vec3{
					static_cast<float>(scaled[0]) / 65536.0f,
					static_cast<float>(scaled[1]) / 65536.0f,
					static_cast<float>(scaled[2]) / 65536.0f};
		}
		if (e->kind == world::EntityKind::Building) {
			// Every batched building's parts draw with the def's forced
			// sections ORed in: itemDef +0x891 (first_door - 1) and +0x892
			// (first_subobject - 1), bytes 1 and 2 of the shared +0x890 dword.
			// [orig: Terrain_RenderSectorModels @ 0x5c5d7c..0x5c5da8]
			const uint32_t door_dword = static_cast<uint32_t>(def->deathtime_ticks);
			deps.occlusion.assign_forced_sections(h,
					static_cast<uint8_t>(door_dword >> 8), static_cast<uint8_t>(door_dword >> 16));
		}
		if (occ_id >= 0 && e->kind == world::EntityKind::Building) {
			// The def bits the occlusion engine reads: attrib2 bit 6 "weldable"
			// [orig: itemDef+88 >> 6 @ 0x5c5cce], attrib bit 27 recurse-windows
			// [orig: itemDef+84 >> 27 @ 0x5c7456].
			world::OcclusionWorld::EntityDefBits bits;
			bits.weldable = (def->attrib2 & (1u << 6)) != 0;
			bits.recurse_windows = (def->attrib & (1u << 27)) != 0;
			deps.occlusion.assign_entity(h, occ_id, bits);
		}
	}
	// The person collector's parachute radius: the special item-185 model's
	// GHDR radius, unscaled. [orig: Entity_PreloadSpecialItems @ 0x43C220 loads
	// the model; Terrain_CollectVisibleEntitiesForTerrain reads model+0x14
	// @ 0x5c8e10]
	if (deps.models.has_source()) {
		const DefItemDef *chute = find_item_def(
				items, mission::kItemIdOffset + world::kParachuteItemTypeId);
		const Threedi3di3 *chute_model = chute != nullptr && chute->graphic[0] != '\0'
				? deps.models.model(std::string(chute->graphic)).get()
				: nullptr;
		deps.occlusion.set_parachute_radius_q16(chute_model != nullptr
				? world::model_bound_radius_q16_from_3di(*chute_model)
				: 0);
	}
	return attached;
}

} // namespace opennova::mission
