#include <world/collision.h>
#include <world/entity.h>
#include <world/minimap_footprint.h>
#include <world/minimap_overlay.h>

#include <cstdio>

namespace world = opennova::world;

namespace {

int failures = 0;
#define CHECK(c, m) do { if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; } } while (0)

world::Entity base_entity() {
	world::Entity entity;
	entity.handle = world::EntityHandle::make(1, 7);
	entity.has_item_def = true;
	entity.team = 1;
	entity.zone_number = 6;
	return entity;
}

} // namespace

int main() {
	CHECK(world::minimap_team_color(1) == 0x0A &&
			world::minimap_team_color(2) == 0x09 &&
			world::minimap_team_color(0) == 0x0C,
			"team colors use the retail table indices");

	world::Entity entity = base_entity();
	entity.item_type = 1;
	entity.item_unit_type = 6;
	auto row = world::classify_minimap_overlay(entity);
	CHECK(row.visible && row.icon == 15 && row.color == 0x0A &&
			row.handle == entity.handle.packed && row.source == 6,
			"air vehicle classification carries icon, identity, color, and source");

	entity.item_attrib = world::kItemAttribArmory;
	row = world::classify_minimap_overlay(entity);
	CHECK(row.visible && row.icon == 13,
			"armory classification outranks the vehicle branch");

	entity = base_entity();
	entity.item_type = 5;
	entity.team = 0;
	row = world::classify_minimap_overlay(entity);
	CHECK(!row.visible, "ordinary building needs the resolved model marker");
	entity.has_minimap_model_marker = true;
	row = world::classify_minimap_overlay(entity);
	CHECK(row.visible && row.icon == 0 && row.color == 0,
			"marked neutral building uses icon/color zero");

	entity = base_entity();
	entity.item_type = 3;
	row = world::classify_minimap_overlay(entity);
	CHECK(row.visible && row.icon == 3 && row.flags == 0,
			"live Person uses icon 3");
	entity.engine_flags |= world::kEntityFlagDead;
	row = world::classify_minimap_overlay(entity);
	CHECK(row.visible && row.icon == 8 && row.flags == 1,
			"dead Person remains visible with icon 8 and dead flag");

	entity = base_entity();
	entity.item_attrib = world::kItemAttribEweap;
	entity.item_id = 1869;
	row = world::classify_minimap_overlay(entity);
	CHECK(row.visible && row.icon == 12 && row.color == 8,
			"alternate emplacement uses icon 12 and emplacement color");
	entity.item_id = 1902;
	row = world::classify_minimap_overlay(entity);
	CHECK(row.visible && row.icon == 4,
			"ordinary emplacement uses icon 4");

	entity.item_attrib = world::kItemAttribNoHud;
	CHECK(!world::classify_minimap_overlay(entity).visible,
			"NoHud suppresses classification");
	entity.item_attrib = 0;
	entity.engine_flags = 1;
	CHECK(!world::minimap_overlay_entity_enabled(entity),
			"producer disabled bit suppresses an otherwise eligible entity");

	// The draw policy table [orig: draw_minimap_blip @0x597890].
	{
		world::Entity e = base_entity();
		e.item_attrib = world::kItemAttribArmory;
		auto policy = world::minimap_blip_draw_policy(e, 13);
		CHECK(!policy.rotate && policy.half_x_q16 == 0x40000 &&
						policy.half_y_q16 == 0x40000 && policy.floor_px == 6,
				"the armory badge draws upright at 4 wu");

		e = base_entity();
		e.item_type = 3;
		policy = world::minimap_blip_draw_policy(e, 3);
		CHECK(policy.rotate && policy.half_x_q16 == 0x20000,
				"a live person rotates at 2 wu");
		e.engine_flags |= world::kEntityFlagDead;
		policy = world::minimap_blip_draw_policy(e, 8);
		CHECK(!policy.rotate, "the dead-person body draws upright");

		e = base_entity();
		e.item_attrib = world::kItemAttribEweap;
		policy = world::minimap_blip_draw_policy(e, 4);
		CHECK(!policy.rotate && policy.floor_px == 4 &&
						policy.half_x_q16 == 0x40000,
				"non-vehicle emplacements draw upright at 4 wu, floor 4");

		e = base_entity();
		e.item_attrib = world::kItemAttribSpawnPoint;
		e.minimap_half_x_q16 = 0x80000;
		e.minimap_half_y_q16 = 0x80000;
		policy = world::minimap_blip_draw_policy(e, 0);
		CHECK(policy.rotate && policy.floor_px == 16 &&
						policy.half_x_q16 == 0x40000,
				"spawn points floor at 16 px on min(model, 4 wu)");

		e = base_entity();
		e.item_attrib2 = 1;
		policy = world::minimap_blip_draw_policy(e, 0);
		CHECK(!policy.rotate && policy.half_x_q16 == 0x80000 &&
						policy.floor_px == 8,
				"the attrib2-bit0 class draws upright at 8 wu, floor 8");

		e = base_entity();
		policy = world::minimap_blip_draw_policy(e, 6);
		CHECK(!policy.rotate && policy.floor_px == 12,
				"cell 6 draws upright with the 12 px floor");
		policy = world::minimap_blip_draw_policy(e, 2);
		CHECK(!policy.rotate && policy.floor_px == 6,
				"cell 2 draws upright with the 6 px floor");

		e = base_entity();
		e.minimap_half_x_q16 = 0x50000;
		e.minimap_half_y_q16 = 0x30000;
		policy = world::minimap_blip_draw_policy(e, 10);
		CHECK(policy.rotate && policy.half_x_q16 == 0x50000 &&
						policy.half_y_q16 == 0x30000,
				"the default class takes the stamped model halves, rotated");
		policy = world::minimap_blip_draw_policy(e, 9);
		CHECK(policy.half_x_q16 == 0x50000 * 6 / 5,
				"cell 9 scales the halves x1.2");

		e = base_entity();
		e.item_type = 5;
		e.has_minimap_model_marker = true;
		policy = world::minimap_blip_draw_policy(e, 0);
		CHECK(policy.footprint && !policy.rotate,
				"a marked building leaves the icon path for its footprint");
	}

	// The footprint mesh: a unit box roof (two up-facing tris) keeps its
	// fills and only the outer boundary edges survive the parity toggle.
	// [orig: render_collision_wireframe @0x596800]
	{
		world::CollisionModel model;
		world::CollisionSection section;
		section.vertex_start = 0;
		section.vertex_count = 4;
		section.normal_start = 0;
		section.normal_count = 2;
		section.face_start = 0;
		section.face_count = 4;
		model.sections.push_back(section);
		const int32_t k = 0x10000;
		model.vertices.push_back({{-k, -k, k}});
		model.vertices.push_back({{k, -k, k}});
		model.vertices.push_back({{k, k, k}});
		model.vertices.push_back({{-k, k, k}});
		world::CollisionNormal up;
		up.n[0] = 0;
		up.n[1] = 0;
		up.n[2] = 16384; // +1.0 in Q14
		world::CollisionNormal side;
		side.n[0] = 16384;
		side.n[1] = 0;
		side.n[2] = 0;
		model.normals.push_back(up);
		model.normals.push_back(side);
		world::CollisionFace face;
		face.vertex_index[0] = 0;
		face.vertex_index[1] = 1;
		face.vertex_index[2] = 2;
		face.normal_index = 0;
		model.faces.push_back(face);
		face.vertex_index[0] = 0;
		face.vertex_index[1] = 2;
		face.vertex_index[2] = 3;
		model.faces.push_back(face);
		// Two side faces that must be filtered out entirely.
		face.normal_index = 1;
		model.faces.push_back(face);
		model.faces.push_back(face);
		const world::MinimapFootprintMesh mesh =
				world::minimap_footprint_from_collision(model);
		CHECK(mesh.fill_xy_q16.size() == 12,
				"two up-facing faces emit two footprint triangles");
		CHECK(mesh.edge_xy_q16.size() == 16,
				"the shared diagonal cancels; four boundary edges remain");
	}

	if (failures != 0) return 1;
	std::printf("minimap_overlay_test OK\n");
	return 0;
}
