#include <array>
#include <runtime/simassets/collision_resolve.h>

#include <runtime/simassets/model_builders.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_panm_pose.h> // the native PANM liveness gate (S3, ADR 0028)

#include <base/io/strutil.h>
#include <formats/mission/mission.h> // kItemIdOffset
#include <runtime/world/player_spawn.h> // kPlayerInfantryTypeId

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

using namespace opennova::def;
using namespace opennova::threedi;

namespace opennova::simassets {

namespace {

bool iends_with_adm(const std::string &name) {
	static const char kExt[] = ".adm";
	if (name.size() < 4) return false;
	return strutil::iequals(name.c_str() + (name.size() - 4), kExt);
}

const char *glass_userpoint_for_graphic(const std::string &graphic) {
	struct GlassSurfaceRow {
		const char *graphic;
		const char *userpoint;
	};
	// Retail's complete Joint Operations table. Projectile_ProcessExplosionQueue
	// requests GLASS1..GLASS4; Terrain_SpawnEffectsAtUserPoint resolves those
	// keys through this exact model-name mapping before walking the USRP bank.
	// GLASS3 and GLASS4 have no shipped rows.
	// [orig: static rows consumed by Terrain_SpawnEffectsAtUserPoint @0x5cee20]
	static constexpr GlassSurfaceRow kRows[] = {
			{"eurhr2", "GLASS"}, {"eurhr2a", "GLASS"},
			{"eurhr2b", "GLASS"}, {"eurhr1", "GLASS02"},
			{"eurhr1b", "GLASS02"}, {"eurhr1c", "GLASS02"},
			{"eurhr3", "GLASS02"}, {"eurhr3b", "GLASS02"},
			{"eurhr3c", "GLASS02"}, {"atrm2a", "GLASS1"},
	};
	for (const GlassSurfaceRow &row : kRows)
		if (strutil::iequals(graphic.c_str(), row.graphic)) return row.userpoint;
	return nullptr;
}

} // namespace

void CollisionResolveState::clear() {
	model_by_graphic.clear();
	occlusion_by_graphic.clear();
	radius_by_graphic.clear();
	radius_q16_by_graphic.clear();
	collision_block_by_graphic.clear();
	half_xy_by_graphic.clear();
	center_by_graphic.clear();
	husk_kz_points_by_graphic.clear();
	husk_dead_points_by_graphic.clear();
	glass_points_by_graphic.clear();
	husk_pieces_by_graphic.clear();
	resolution_attempted.clear();
}

const DefItemDef *find_item_def(const DefItemsFile &items, int item_id) {
	// Last-wins over duplicate definition ids — the same load-order overwrite
	// the id-keyed item map exposed (see simassets item_traits).
	const DefItemDef *found = nullptr;
	for (size_t i = 0; i < items.count; ++i)
		if (items.entries[i].id == item_id) found = &items.entries[i];
	return found;
}

int visual_item_id_for_runtime_type(int item_id, const DefItemsFile &items) {
	// The policy lives in mission/placement_traits.h; this overload only
	// answers the catalog probe against the retained DefItemsFile.
	return mission::visual_item_id_for_runtime_type(item_id,
			find_item_def(items, kPlayerVisualItemId) != nullptr);
}
// The mission-side policy keys on the SAME runtime player type the world
// names; a drift would silently break the visual resolve.
static_assert(mission::kPlayerRuntimeTypeId == world::kPlayerInfantryTypeId);

int32_t collision_model_for_graphic(CollisionResolveState &state,
		const CollisionResolveDeps &deps, const std::string &graphic_key) {
	auto it = state.model_by_graphic.find(graphic_key);
	if (it != state.model_by_graphic.end()) return it->second;
	int32_t model_id = -1;
	int32_t occlusion_id = -1;
	float bound_radius = 0.0f;
	int32_t bound_radius_q16 = 0;
	bool has_collision_block = false;
	std::pair<float, float> half_xy{0.0f, 0.0f};
	std::array<int32_t, 3> center{0, 0, 0};
	if (deps.models.has_index()) {
		// ADR 0028: the sim reads its own parse-once cache. The placer
		// now supplies only the render-side pose sources (live-PANM
		// object data + skeletal sets) — the S3 push-down target.
		if (const Threedi3di3 *m3 = deps.models.model_for(graphic_key)) {
			has_collision_block = m3->collision != nullptr;
			world::CollisionModel model;
			if (collision_model_from_3di(m3->collision, model,
					model_has_collision(*m3))) {
				model_id = deps.collision.add_model(std::move(model));
				// S3 (ADR 0028): the engine-side provider poses this
				// model from the sim's own retained parse. The legacy
				// pose map stays empty in production — the fallback is
				// stub-world-only, so a shadow render-bound copy here
				// could only mask a native decline.
				if (threedi_panm_lod_has_live(*m3, 0))
					deps.pose.register_generic_model(model_id, m3);
				// Every parsed model serves the userpoint leg (PANM or not).
				deps.pose.register_userpoint_model(model_id, m3);
			}
			world::OcclusionModel occ;
			if (occlusion_model_from_3di(*m3, occ))
				occlusion_id = deps.occlusion.add_model(std::move(occ));
			bound_radius_q16 = model_bound_radius_q16_from_3di(*m3);
			bound_radius = static_cast<float>(bound_radius_q16) / 65536.0f;
			// The minimap blip-size source: the CMDL bound-block ground-axis
			// half extents. [orig: draw_minimap_blip @0x5979a2..0x5979b8 —
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
	state.radius_by_graphic.emplace(graphic_key, bound_radius);
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
// the 1-based table index (0 = none). The weapon-def userpoint (+0x333) is
// not carried (weapon.def+856 is unparsed).
// [orig: Entity_InitBoneReferences @0x441470 (@0x4414e0..0x4415aa);
//  Entity_ResolveBoneUserpoints @0x545940; modelgpm_FindUserpointByName
//  @0x5b21ef]
static uint8_t userpoint_index_by_name(const Threedi3di3 &model, const char *name) {
	if (name == nullptr || name[0] == '\0' || model.user_points == nullptr) return 0;
	for (size_t i = 0; i < model.user_point_count && i < 255; ++i) {
		if (strutil::iequals(model.user_points[i].name, name))
			return static_cast<uint8_t>(i + 1);
	}
	return 0;
}

static void resolve_weapon_userpoint_bytes(const DefItemDef &def,
		const Threedi3di3 &model, world::Entity &e) {
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
			e.weapon_userpoint_bytes[slot][field] = bytes[slot * 3 + field];
}

world::ResolvedCollisionShape collision_shape_for_runtime_type(
		int runtime_item_id, const DefItemsFile &items,
		CollisionResolveState &state, const CollisionResolveDeps &deps) {
	world::ResolvedCollisionShape shape;
	const int visual_item_id = visual_item_id_for_runtime_type(runtime_item_id, items);
	const DefItemDef *def = find_item_def(items, visual_item_id);
	if (def == nullptr) return shape;
	shape.pool1_candidate_source_eligible =
			(def->attrib & world::kItemAttribEweap) == 0 || def->type == 1;
	if (def->graphic[0] == '\0') return shape;

	const std::string key(def->graphic);
	shape.model_id = collision_model_for_graphic(state, deps, key);
	shape.uniform_scale_q16 = def->scale_q16;
	shape.has_collision_block = state.collision_block_by_graphic[key];
	if (!shape.has_collision_block) return shape;

	int32_t bound_q16 = state.radius_q16_by_graphic[key];
	if (shape.uniform_scale_q16 != 0)
		bound_q16 = world::retail_q16_mul_rhu(
				bound_q16, shape.uniform_scale_q16);

	// Only the FIRST husk participates in Entity_InitFromModel's max. The
	// huskFinal pointer belongs to the later piece/death chain and is not a
	// compatibility substitute for a missing first husk here.
	// [orig: Entity_InitFromModel @0x40dced..0x40de16]
	if (def->husk[0] != '\0' && deps.models.has_index()) {
		const std::string husk_key(def->husk);
		auto radius_it = state.radius_q16_by_graphic.find(husk_key);
		if (radius_it == state.radius_q16_by_graphic.end()) {
			int32_t husk_bound_q16 = 0;
			if (const Threedi3di3 *husk = deps.models.model_for(husk_key)) {
				husk_bound_q16 = model_bound_radius_q16_from_3di(*husk);
				state.radius_by_graphic.emplace(
						husk_key, static_cast<float>(husk_bound_q16) / 65536.0f);
			}
			radius_it = state.radius_q16_by_graphic.emplace(
					husk_key, husk_bound_q16).first;
		}
		bound_q16 = std::max(bound_q16, radius_it->second);
	}
	shape.bound_radius_q16 = bound_q16 + 0x1000;

	if (!(def->type == 6 && (def->attrib & 0x20u) != 0)) {
		const std::array<int32_t, 3> &center = state.center_by_graphic[key];
		int32_t scaled[3] = {center[0], center[1], center[2]};
		if (shape.uniform_scale_q16 != 0) {
			for (int axis = 0; axis < 3; ++axis)
				scaled[axis] = world::retail_q16_mul_rhu(
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
		const int def_id = is_organic
				? visual_item_id_for_runtime_type(e->item_id, items)
				: static_cast<int>(e->item_id) + mission::kItemIdOffset;
		const DefItemDef *def = find_item_def(items, def_id);
		if (def == nullptr || def->graphic[0] == '\0') continue;
		const std::string key(def->graphic);
		const int32_t resolved_model = collision_model_for_graphic(state, deps, key);
        if (auto *traits = world.tables.item_death_traits.get_mutable(e->item_id);
                traits != nullptr && !traits->model_loaded && deps.models.has_index()) {
            if (const Threedi3di3 *model = deps.models.model_for(key)) {
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
                traits->model_radius_q16 = model_bound_radius_q16_from_3di(*model);
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
                for (size_t i = 0; i < model->user_point_count; ++i) {
                    const auto &point = model->user_points[i];
                    if (!strutil::iequals(point.name, "SOUND")) continue;
                    const uint8_t one_based = static_cast<uint8_t>(i + 1);
                    if (one_based != 0 && one_based <= 127) {
                        float pos[3];
                        threedi_user_point_position(&model->user_points[one_based - 1], pos);
                        traits->has_sound_point = true;
                        traits->sound_point = {pos[2], -pos[0], pos[1]};
                    }
                    break;
                }
            }
        }
		// The blast window path resolves from the INTACT graphic. Cache even a
		// miss so repeated attachment sweeps never reopen or rewalk the model.
		auto glass_it = state.glass_points_by_graphic.find(key);
		if (glass_it == state.glass_points_by_graphic.end()) {
			std::vector<world::GlassPointTrait> points;
			const char *userpoint_name = glass_userpoint_for_graphic(key);
			const Threedi3di3 *glass_model =
					userpoint_name != nullptr && deps.models.has_index()
					? deps.models.model_for(key)
					: nullptr;
			for (size_t up_index = 0;
					glass_model != nullptr && glass_model->user_points != nullptr &&
					up_index < glass_model->user_point_count;
					++up_index) {
				const ThreediUserPoint &point = glass_model->user_points[up_index];
				if (!strutil::iequals(point.name, userpoint_name)) continue;
				float up_pos[3];
				float up_dir[3];
				threedi_user_point_position(&point, up_pos);
				threedi_user_point_direction(&point, up_dir);
				points.push_back(world::GlassPointTrait{
						world::Vec3{up_pos[2], -up_pos[0], up_pos[1]},
						world::Vec3{up_dir[2], -up_dir[0], up_dir[1]}});
				break; // Terrain's exact userpoint lookup returns the first match.
			}
			glass_it = state.glass_points_by_graphic.emplace(
					key, std::move(points)).first;
		}
		if (world::ItemDeathTraits *traits =
					world.tables.item_death_traits.get_mutable(e->item_id);
				traits != nullptr && traits->glass_points.empty() &&
				!glass_it->second.empty())
			traits->glass_points = glass_it->second;
		// The bound-sphere radius (entity+0 boundRadius) comes from the exact
		// GHDR model carrier, but the entire stamp is gated by the model's
		// collision block. Raised to the FIRST husk model's unscaled bound below,
		// then padded +0.0625 [orig: Entity_InitFromModel @ 0x40dc30 — boundRadius =
		// max(scale*gpm[5], husk gpm[5]) + 0x1000. The base model bound is
		// scaled BEFORE the signed compare with the first husk model; the husk
		// operand itself is not multiplied in this initializer.
		// [orig: Entity_InitFromModel @ 0x40dc30]
		const bool has_collision_block = state.collision_block_by_graphic[key];
		int32_t entity_bound_q16 = 0;
		if (has_collision_block) {
			entity_bound_q16 = state.radius_q16_by_graphic[key];
			if (e->uniform_scale_q16 != 0) {
				entity_bound_q16 = world::retail_q16_mul_rhu(
						entity_bound_q16, e->uniform_scale_q16);
			}
		}
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
				if (const Threedi3di3 *model = deps.models.model_for(key)) {
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
				if (const Threedi3di3 *model = deps.models.model_for(key)) {
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
				const Threedi3di3 *vm3 = deps.models.has_index()
						? deps.models.model_for(key)
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
					if (const Threedi3di3 *model = deps.models.model_for(key)) {
						for (size_t i = 0; model->user_points != nullptr &&
								i < model->user_point_count && vt->flare_points.size() < 16;
								++i) {
							const auto &point = model->user_points[i];
							const std::string name(point.name);
							if (name.size() < 5 || !strutil::iequals(name.substr(0, 5), "flare"))
								continue;
							vt->flare_points.push_back({ { point.x, point.y, point.z },
									{ point.rot_x, point.rot_y, point.rot_z } });
						}
					}
				}
				// brain[11] is the CMDL floor's absolute value.
				// [orig: Entity_InitHelicopterAIFromDef @0x4683C0]
				const world::AiEntity *vehicle_ai = world.ai.for_handle(e->handle);
				if (vehicle_ai != nullptr &&
						(vehicle_ai->profile.type == 1 ||
								(vehicle_ai->profile.type == 2 &&
										vehicle_ai->profile.subtype != 1))) {
					e->veh.air_probe_z_off =
							static_cast<int32_t>(vt->box_z_lo < 0 ? 0u - uint32_t(vt->box_z_lo)
																  : uint32_t(vt->box_z_lo));
					if (world::AiEntity *ai = world.ai.for_handle(e->handle))
						ai->brain.f[11] = e->veh.air_probe_z_off;
				}
			}
		}
		if (resolved_model >= 0) {
			deps.collision.assign_entity(h, resolved_model, e->registry_spawn_id);
			++attached;
			// The aim/LOS origin's TARGET userpoint [orig: Entity_InitFromModel
			// @0x40dd04 -> def+1350]; 0 when the model has none.
			if (const Threedi3di3 *m3 = deps.models.model_for(key)) {
				e->target_userpoint_byte = userpoint_index_by_name(*m3, "TARGET");
				e->look_userpoint_byte = userpoint_index_by_name(*m3, "LOOK");
				// [orig: Entity_InitVehicleAI @0x460200, three bounded prefix scans]
				if (world::AiEntity *ai = world.ai.for_handle(e->handle)) {
					if (!ai->inf.active) {
						auto &b = ai->brain;
						b.f[55] = b.f[72] = b.f[89] = 0;
						for (size_t index = 0;
								m3->user_points != nullptr && index < m3->user_point_count;
								++index) {
							const std::string name = strutil::to_lower(m3->user_points[index].name);
							const bool primary = name.compare(0, 4, "prim") == 0 ||
									name.compare(0, 8, "bullet01") == 0 ||
									name.compare(0, 8, "bullet02") == 0;
							const bool secondary = name.compare(0, 3, "sec") == 0 ||
									name.compare(0, 8, "bullet02") == 0;
							const bool flare = name.compare(0, 5, "flare") == 0;
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
				if (const Threedi3di3 *m3 = deps.models.model_for(key))
					resolve_weapon_userpoint_bytes(*def, *m3, *e);
			}
			if (is_organic) {
				deps.pose.remove_entity(h);
				// S3 (ADR 0028): the native skeletal source resolves from
				// the retained def rows + the sim's own model parse; the
				// provider validates rig/FK and declines at query time
				// exactly like the unregistered legacy leg when it cannot.
				if (deps.models.has_index() && def->anim_def[0] != '\0') {
					std::string adm(def->anim_def);
					if (!iends_with_adm(adm)) adm += ".adm";
					deps.pose.register_skeletal_entity(
							h, e->registry_spawn_id, resolved_model,
							strutil::to_lower(adm) + "|" + key,
							adm, deps.models.model_for(key),
							def->launchups_closeattack);
				}
			}
		}
		// The husk-stage collision model: attached beside the graphic instance so
		// every query swaps to the wreck once Flags & 4 sets. The collision pick
		// is the FIRST husk stage (entity+52 huskModel), not huskFinal [orig: the
		// +52 substitution @ 0x538720 / @ 0x413086; D-AI-7 residual closed].
		const std::string first_husk_name(def->husk);
		const std::string final_husk_name(def->huskfinal);
		const std::string &husk_name =
				first_husk_name.empty() ? final_husk_name : first_husk_name;
		if (!husk_name.empty()) {
			// Retail keeps live huskModel and huskFinalModel pointers on the
			// entity. A successfully opened model supplies that pointer even when
			// it has no collision block; missing/corrupt assets leave it null.
			// [orig: Entity_ProcessBuildingDeath @ 0x49442c]
			// S3b full: husk models resolve exclusively through the sim cache's
			// parse-once source.
			const Threedi3di3 *first_husk_m3 = nullptr;
			const Threedi3di3 *final_husk_m3 = nullptr;
			if (deps.models.has_index()) {
				if (!first_husk_name.empty())
					first_husk_m3 = deps.models.model_for(first_husk_name);
				if (!final_husk_name.empty())
					final_husk_m3 = deps.models.model_for(final_husk_name);
			}
			if (world::ItemDeathTraits *t =
						world.tables.item_death_traits.get_mutable(e->item_id)) {
                t->primary_husk_loaded = first_husk_m3 != nullptr;
				t->husk_model_loaded =
						first_husk_m3 != nullptr || final_husk_m3 != nullptr;
				// The death-flash radius source is the PIECE model — huskFinal
				// first [orig: @ 0x4934af huskFinalModel ?: huskModel].
				const Threedi3di3 *piece_m3 =
						final_husk_m3 != nullptr ? final_husk_m3 : first_husk_m3;
				if (piece_m3 != nullptr && t->husk_piece_bound_radius <= 0.0f)
					t->husk_piece_bound_radius =
							model_bound_radius_from_3di(*piece_m3);
				// The interned death masks and all three banks use final-husk first.
				// [orig: resolve_item_materials_and_spawn_bone_trails @0x522EE0]
				if (piece_m3 != nullptr) {
					const char *names[3] = { "Dead", "Fire", "Other" };
					for (int bank = 0; bank < 3; ++bank) {
						auto &out = t->effect_banks[bank];
						out.mask = threedi_3di3_user_point_mask(piece_m3, names[bank]);
						out.points.clear();
						for (size_t i = 0; i < piece_m3->user_point_count; ++i) {
							if ((out.mask & (1u << (i & 31u))) == 0)
								continue;
							float pos[3], dir[3];
							threedi_user_point_position(&piece_m3->user_points[i], pos);
							threedi_user_point_direction(&piece_m3->user_points[i], dir);
							out.points.push_back(
									{ { pos[2], -pos[0], pos[1] }, { dir[2], -dir[0], dir[1] } });
						}
					}
				}
			}
			const std::string &husk_key = husk_name;
			const Threedi3di3 *husk_m3 = first_husk_name.empty()
					? final_husk_m3
					: first_husk_m3;
			// Main and husk graphics share one complete cache entry. A graphic
			// first encountered as a husk must still retain its collision-block,
			// exact GHDR, center, and occlusion metadata if it is later used as an
			// intact model by another definition.
			const int32_t husk_model_id = collision_model_for_graphic(
					state, deps, husk_key);
			if (husk_model_id >= 0 && resolved_model >= 0)
				deps.collision.assign_entity_husk(h, husk_model_id);
			// Retail's death-sound tail walks exact, case-insensitive "KZ"
			// user points on the active FIRST husk, not the huskFinal piece
			// model, and queues a radius-5 blast at every match. Cache this
			// metadata separately from collision registration: the same graphic
			// may already be resident as another entity's main model.
			// Unlike collision's legacy final-only fallback, the retail KZ walker
			// reads entity+52 huskModel. A def with only huskFinal has no KZ source
			// and therefore takes the entity-origin fallback blast.
			if (!first_husk_name.empty()) {
				auto kz_it = state.husk_kz_points_by_graphic.find(husk_key);
				if (kz_it == state.husk_kz_points_by_graphic.end()) {
					std::vector<world::Vec3> kz_points;
					if (husk_m3 != nullptr) {
						const Threedi3di3 &hmodel3di = *husk_m3;
						for (size_t up_index = 0;
								hmodel3di.user_points != nullptr && up_index < hmodel3di.user_point_count;
								++up_index) {
							const ThreediUserPoint &point = hmodel3di.user_points[up_index];
							if (!strutil::iequals(point.name, "KZ"))
								continue;
							// Decoded model space is (-source y, source z, source x);
							// destruction's placement math consumes mission-local (x, y, z).
							float up_pos[3];
							threedi_user_point_position(&point, up_pos);
							kz_points.push_back(world::Vec3{
									up_pos[2],
									-up_pos[0],
									up_pos[1]});
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
			// The PIECE model is huskFINAL first [orig: @ 0x4934af
			// huskFinalModel ?: huskModel] — the opposite preference from the
			// collision husk pick above. Its LOD-0 part table feeds the
			// death-piece loop bound [orig: renderObj[8]+52 @ 0x49361a], the
			// per-section centers [orig: the section-row center @ 0x4938bf],
			// and section 0's z extents (the wreck ground-rest offset
			// [orig: @ 0x461e23-0x461e4b]). Its own cache, independent of the
			// collision cache: a husk graphic can double as some entity's main
			// graphic, which would leave the joint cache without an entry.
			const std::string &piece_key = final_husk_name.empty()
					? first_husk_name
					: final_husk_name;
			auto hs = state.husk_pieces_by_graphic.find(piece_key);
			if (hs == state.husk_pieces_by_graphic.end()) {
				CollisionHuskPieceInfo info;
				const Threedi3di3 *piece_m3 = final_husk_name.empty()
						? first_husk_m3
						: final_husk_m3;
				if (piece_m3 != nullptr && piece_m3->lod_count > 0 &&
				    piece_m3->lods != nullptr) {
					const ThreediLod &lod = piece_m3->lods[0];
					info.sections = static_cast<int32_t>(lod.render_object_count);
					for (size_t pi = 0; lod.render_objects != nullptr && pi < lod.render_object_count;
							++pi) {
						const ThreediRenderObject &part = lod.render_objects[pi];
						info.centers.push_back(world::Vec3{
								part.abs[0] + part.bounding_center[0],
								part.abs[1] + part.bounding_center[1],
								part.abs[2] + part.bounding_center[2]});
					}
					// Section 0 owns the first opaque+alpha strip run (strips are
					// stored sequentially per render object).
					if (lod.render_objects != nullptr && lod.render_object_count > 0 &&
					    lod.strips != nullptr) {
						const ThreediRenderObject &p0 = lod.render_objects[0];
						const int32_t p0_strip_count = p0.num_strips + p0.num_alpha_strips;
						bool any = false;
						for (int32_t pr = 0; pr < p0_strip_count; ++pr) {
							const size_t idx = static_cast<size_t>(pr);
							if (idx >= lod.strip_count) break;
							const ThreediTriangleStrip &prim = lod.strips[idx];
							info.rest_min_z =
									any ? std::min(info.rest_min_z, prim.min[2])
									    : prim.min[2];
							info.rest_max_z =
									any ? std::max(info.rest_max_z, prim.max[2])
									    : prim.max[2];
							any = true;
						}
					}
				}
				hs = state.husk_pieces_by_graphic.emplace(
						piece_key, std::move(info)).first;
				// Piece bound = the husk model's CMDL sphere [orig: Entity_InitFromModel @0x40dced..0x40de16]
				if (piece_m3 != nullptr) {
					const int32_t piece_bound_q16 =
							model_bound_radius_q16_from_3di(*piece_m3);
					state.radius_q16_by_graphic.emplace(
							piece_key, piece_bound_q16);
					state.radius_by_graphic.emplace(
							piece_key,
							static_cast<float>(piece_bound_q16) / 65536.0f);
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
                // retail would dereference a null +0x34.
                // [orig: Entity_BuildDeathSectionTransforms @0x492B46 / @0x492B4D]
                if (t->husk_section_origins_q16.empty()) {
                    const Threedi3di3 *piece = first_husk_m3 != nullptr ? first_husk_m3 : final_husk_m3;
                    if (piece && piece->collision) {
                        for (size_t i = 0; i < piece->collision->object_count; ++i) {
                            const auto &p = piece->collision->objects[i].offset;
                            t->husk_section_origins_q16.push_back({p[0], p[1], p[2]});
                        }
                    }
                }
				if (t->husk_section_centers.empty() && !info.centers.empty())
					t->husk_section_centers = info.centers;
				t->husk_rest_min_z = info.rest_min_z;
				t->husk_rest_max_z = info.rest_max_z;
			}
			// Only entity+52's FIRST husk model joins this signed max. A
			// huskFinal-only definition has no substitute operand here.
			// [orig: Entity_InitFromModel @0x40dced..0x40de16]
			if (first_husk_m3 != nullptr) {
				const int32_t husk_bound_q16 =
						model_bound_radius_q16_from_3di(*first_husk_m3);
				entity_bound_q16 = std::max(entity_bound_q16, husk_bound_q16);
			}
		}
		if (has_collision_block && e->bound_radius <= 0.0f)
			e->bound_radius = static_cast<float>(entity_bound_q16 + 0x1000) /
					65536.0f;
		const int32_t occ_id = state.occlusion_by_graphic[key];
		// Entity_ClassifyForMinimap's ordinary-Building branch checks the
		// live graphic model's +0xE0 portal/occlusion pointer. The parsed .3di
		// and the collision/occlusion resolver are the portable ownership seam
		// for that otherwise renderer-private fact.
		e->has_minimap_model_marker = occ_id >= 0;
		// The blip drawer reads the raw model bound block rather than the entity
		// placement matrix, so authored scale deliberately does not fold here.
		// [orig: draw_minimap_blip @0x5979a2..0x5979b8]
		const std::pair<float, float> &half_xy = state.half_xy_by_graphic[key];
		e->minimap_half_x_q16 = static_cast<int32_t>(half_xy.first * 65536.0f);
		e->minimap_half_y_q16 = static_cast<int32_t>(half_xy.second * 65536.0f);
		// The +0x1FC LOS ray offset: zeroed for type-6 powerups with attrib
		// bit 5; otherwise the raw fixed midpoint is multiplied by the same
		// effective scale as the entity matrix, with retail's +0x8000 rule.
		// [orig: Entity_InitFromModel @0x40defc..0x40df4a]
		const std::array<int32_t, 3> &bc = state.center_by_graphic[key];
		if (!(def->type == 6 && (def->attrib & 0x20u) != 0)) {
			int32_t scaled[3] = {bc[0], bc[1], bc[2]};
			if (e->uniform_scale_q16 != 0) {
				for (int axis = 0; axis < 3; ++axis) {
					scaled[axis] = world::retail_q16_mul_rhu(
							scaled[axis], e->uniform_scale_q16);
				}
			}
			e->bbox_center = world::Vec3{
					static_cast<float>(scaled[0]) / 65536.0f,
					static_cast<float>(scaled[1]) / 65536.0f,
					static_cast<float>(scaled[2]) / 65536.0f};
		}
		if (occ_id >= 0 && e->kind == world::EntityKind::Building) {
			// The def bits the occlusion engine reads: attrib2 bit 6 "weldable"
			// [orig: itemDef+88 >> 6 @ 0x5c5cce], attrib bit 27 recurse-windows
			// [orig: itemDef+84 >> 27 @ 0x5c7456]; the destruction bone-map
			// bytes (+2193/+2194) stay 0 until the destruction system lands
			// (D-COL-2 / D-OCC-9).
			world::OcclusionWorld::EntityDefBits bits;
			bits.weldable = (def->attrib2 & (1u << 6)) != 0;
			bits.recurse_windows = (def->attrib & (1u << 27)) != 0;
			deps.occlusion.assign_entity(h, occ_id, bits);
		}
	}
	return attached;
}

} // namespace opennova::simassets
