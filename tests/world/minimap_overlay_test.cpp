#include <runtime/world/collision.h>
#include <runtime/world/entity.h>
#include <runtime/world/minimap_footprint.h>
#include <runtime/world/minimap_overlay.h>
#include <runtime/world/occlusion.h>

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

	// The map-overlay caller passes a zero color-alpha override, which the
	// wireframe renderer promotes to 0xFF before emitting its fill vertices.
	// The screenshot's large exact 0xA0A0A0 runs are the pixel witness.
	// [orig: MapOverlay_DrawView @0x5a5abc pushes 0; the zero -> 0xFF branch
	//  in render_collision_wireframe @0x596884..0x596891]
	{
		world::Entity e = base_entity();
		e.team = 0;
		CHECK(world::minimap_footprint_fill_argb(e) == 0xFFA0A0A0u,
				"neutral footprints are opaque gray");
		e.team = 1;
		CHECK(world::minimap_footprint_fill_argb(e) == 0xFF4050A0u,
				"team-one footprints are opaque blue");
		e.team = 2;
		CHECK(world::minimap_footprint_fill_argb(e) == 0xFFA05040u,
				"team-two footprints are opaque red");
		e.team = 0;
		e.item_attrib = world::kItemAttribChangeTeam;
		CHECK(world::minimap_footprint_fill_argb(e) == 0xFF609F60u,
				"change-team footprints are opaque green");
	}

	// The footprint mesh comes from the model's OOBJ/OVRT/OPLN/OFAC arena,
	// not CDTA collision. An asymmetric roof (two up-facing tris) keeps its
	// fills and only the outer boundary edges survive the authored edge-word
	// parity toggle. OOBJ position is portal metadata and is not applied.
	// [orig: render_collision_wireframe @0x596800: model +0xDC/+0xE0,
	//  record +24/+32/+36/+40]
	{
		world::OcclusionModel model;
		world::OcclusionPortalFace record;
		record.type = world::kOccRecOccluder;
		record.pos[0] = 100.0f;
		record.pos[1] = 200.0f;
		record.pos[2] = 300.0f;
		record.vert_count = 4;
		record.plane_count = 2;
		record.face_count = 4;
		model.records.push_back(record);
		model.vertices.push_back({{-1.0f, 3.0f, -2.0f}});
		model.vertices.push_back({{1.0f, 3.0f, -2.0f}});
		model.vertices.push_back({{1.0f, 3.0f, 2.0f}});
		model.vertices.push_back({{-1.0f, 3.0f, 2.0f}});
		world::OcclusionPlane up;
		up.normal[1] = 1.0f;
		world::OcclusionPlane side;
		side.normal[2] = 1.0f;
		model.planes.push_back(up);
		model.planes.push_back(side);
		world::OcclusionFaceRec face;
		face.v[0] = 0;
		face.v[1] = 1;
		face.v[2] = 2;
		face.plane = 0;
		face.edge[0] = 0x0100;
		face.edge[1] = 0x0201;
		face.edge[2] = 0x8200;
		model.faces.push_back(face);
		face.v[0] = 0;
		face.v[1] = 2;
		face.v[2] = 3;
		face.edge[0] = 0x0200;
		face.edge[1] = 0x0302;
		face.edge[2] = 0x8300;
		model.faces.push_back(face);
		// Two side faces that must be filtered out entirely.
		face.plane = 1;
		model.faces.push_back(face);
		model.faces.push_back(face);
		const world::MinimapFootprintMesh mesh =
				world::minimap_footprint_from_occlusion(model);
		const int32_t k = 0x10000;
		CHECK(mesh.fill_xy_q16.size() == 12,
				"two up-facing faces emit two footprint triangles");
		CHECK(mesh.fill_xy_q16.size() >= 6 &&
				mesh.fill_xy_q16[0] == -k &&
				mesh.fill_xy_q16[1] == -2 * k &&
				mesh.fill_xy_q16[2] == k &&
				mesh.fill_xy_q16[3] == -2 * k &&
				mesh.fill_xy_q16[4] == k &&
				mesh.fill_xy_q16[5] == 2 * k,
				"footprints project OVRT X/Z without applying OOBJ position");
		CHECK(mesh.edge_xy_q16.size() == 16,
				"the shared diagonal cancels; four boundary edges remain");
	}

	// Retail owns the edge parity list per OOBJ record and suppresses a
	// record with fewer than four survivors. Two independent triangular
	// sections therefore keep their fills but must not combine into a
	// six-edge outline. [orig: render_collision_wireframe @0x596800]
	{
		world::OcclusionModel model;
		world::OcclusionPlane up;
		up.normal[1] = 1.0f;
		model.planes.push_back(up);
		model.planes.push_back(up);
		model.vertices.push_back({{0.0f, 1.0f, 0.0f}});
		model.vertices.push_back({{1.0f, 1.0f, 0.0f}});
		model.vertices.push_back({{0.0f, 1.0f, 1.0f}});
		model.vertices.push_back({{2.0f, 1.0f, 0.0f}});
		model.vertices.push_back({{3.0f, 1.0f, 0.0f}});
		model.vertices.push_back({{2.0f, 1.0f, 1.0f}});

		for (int record_index = 0; record_index < 2; ++record_index) {
			world::OcclusionPortalFace record;
			record.type = world::kOccRecOccluder;
			record.vert_start = record_index * 3;
			record.vert_count = 3;
			record.plane_start = record_index;
			record.plane_count = 1;
			record.face_start = record_index;
			record.face_count = 1;
			model.records.push_back(record);

			world::OcclusionFaceRec face;
			face.v[0] = 0;
			face.v[1] = 1;
			face.v[2] = 2;
			face.plane = 0;
			face.edge[0] = 0x0100;
			face.edge[1] = 0x0201;
			face.edge[2] = 0x8200;
			model.faces.push_back(face);
		}

		const world::MinimapFootprintMesh mesh =
				world::minimap_footprint_from_occlusion(model);
		CHECK(mesh.fill_xy_q16.size() == 12,
				"independent triangular sections retain both fills");
		CHECK(mesh.edge_xy_q16.empty(),
				"independent three-edge sections do not combine into an outline");
	}

	// Retail gives its wireframe the UNFOLDED map-heading/entity-heading delta.
	// OpenNova's map projection applies a separate -90-degree fold, so footprint
	// world placement compensates by subtracting that quarter turn. Together
	// with the projection's mission-Y reflection this is retail's screen matrix.
	// [orig: map heading @0x5a636e; relative angle @0x5be55b;
	//  wireframe @0x596844..0x596bbb]
	{
		const int32_t k = 0x10000;
		world::MinimapFootprintMesh mesh;
		mesh.fill_xy_q16 = {k, 2 * k, 3 * k, 5 * k, -2 * k, 7 * k};
		mesh.edge_xy_q16 = {k, 2 * k, 3 * k, 5 * k};
		world::Entity placed = base_entity();
		placed.position = {10.0f, 20.0f, 0.0f};

		std::vector<int32_t> fill;
		std::vector<int32_t> edges;
		placed.yaw = 90.0f; // entity heading 0, placement heading -90
		world::minimap_footprint_place(mesh, placed, fill, edges);
		CHECK(fill == std::vector<int32_t>({12 * k, 19 * k,
				15 * k, 17 * k, 17 * k, 22 * k}),
				"yaw 90 footprint compensates for the map quarter-turn");
		CHECK(edges == std::vector<int32_t>({12 * k, 19 * k,
				15 * k, 17 * k}),
				"footprint edges use the same entity placement as fills");

		fill.clear();
		edges.clear();
		placed.yaw = 0.0f; // entity heading 90, placement heading 0
		world::minimap_footprint_place(mesh, placed, fill, edges);
		CHECK(fill == std::vector<int32_t>({11 * k, 22 * k,
				13 * k, 25 * k, 8 * k, 27 * k}),
				"yaw 0 footprint uses the compensated entity basis");
	}

	if (failures != 0) return 1;
	std::printf("minimap_overlay_test OK\n");
	return 0;
}
