// Reads back the .3di opennova-3di minted from fixtures/threedi/o3d/
// spinner.o3d, skinned.o3d or building.o3d (the opennova_3di_build* ctests run
// first) and checks the scene -> model conversions the CLI owns: mission ->
// model axes, the counter-clockwise-in-model render winding and the
// counter-clockwise-about-the-normal collision winding retail uses, the
// register-driven PANM row and its MTRX frame, materials, user points,
// lights, occlusion planes and the skinned layout.
#include <cmath>
#include <cstdio>
#include <cstring>

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_panm.h>

using namespace opennova::threedi;

static int failures = 0;

#define CHECK(cond)                                                                  \
	do {                                                                             \
		if (!(cond)) {                                                               \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                              \
		}                                                                            \
	} while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// cross(e1, e2) . n over one list triangle, in the stored (model) axes.
static float facing(const ThreediLod &lod, const ThreediTriangleStrip &st, int tri) {
	const ThreediVertex *v[3];
	for (int k = 0; k < 3; ++k)
		v[k] = &lod.vertices.items[st.start_vertex + lod.indices.indices[st.index_offset + tri * 3 + k]];
	float e1[3], e2[3];
	for (int k = 0; k < 3; ++k) {
		e1[k] = v[1]->position[k] - v[0]->position[k];
		e2[k] = v[2]->position[k] - v[0]->position[k];
	}
	const float c[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
	return c[0] * v[0]->normal[0] + c[1] * v[0]->normal[1] + c[2] * v[0]->normal[2];
}

// The skinned fixture (skinned.o3d): the retail skinned layout, bone tables,
// weights and bone spheres.
static int check_skinned(const char *path) {
	Threedi3di3 m{};
	if (threedi_3di3_read(path, &m) != 0) {
		std::fprintf(stderr, "cannot read %s\n", path);
		return 1;
	}
	CHECK(m.header.mesh_type == THREEDI_MESH_SKINNED && m.lod_count == 1);
	const ThreediLod &lod = m.lods[0];
	CHECK(lod.render_object_count == 3 && lod.strip_count == 1);
	CHECK((lod.vertices.flags & THREEDI_VERTEX_FLAG_SKINNED) != 0 && (lod.vertices.flags & THREEDI_VERTEX_FLAG_TANGENTS) == 0);
	// Authored on part 2, owned by the root ROBJ; the bounds stay on part 2.
	CHECK(lod.render_objects[0].num_strips == 1 && lod.render_objects[2].num_strips == 0);
	CHECK(lod.render_objects[0].bounding_radius == 0.0f && lod.render_objects[2].bounding_radius > 0.0f);
	// The table lists the parts in the order the triangles first name them.
	CHECK(lod.strips[0].bone_table_length == 2 && lod.strips[0].bone_table[0] == 0 && lod.strips[0].bone_table[1] == 1);
	// The second vertex's pairs (part 0 half, part 1 half): slot 0 its
	// primary, slot 1 the other, the unused slots repeating slot 0 at no
	// weight (slot 3 takes the rest, 1 - (w0 + w1 + w2): none).
	CHECK(lod.vertices.count == 4 && lod.vertices.items[1].bone_indices[0] == 0 &&
			lod.vertices.items[1].bone_indices[1] == 1 && lod.vertices.items[1].bone_indices[3] == 0 &&
			near(lod.vertices.items[1].bone_weights[0], 0.5f) && near(lod.vertices.items[1].bone_weights[1], 0.5f) &&
			lod.vertices.items[1].bone_weights[2] == 0.0f);
	if (lod.vertices.count == 4) {
		ThreediSkinInfluence influences[4];
		threedi_skin_influences(&lod.vertices.items[1], lod.strips[0].bone_table, lod.strips[0].bone_table_length,
				influences);
		CHECK(influences[0].part == 0 && influences[0].weight == 0.5f && influences[1].part == 1 &&
				influences[1].weight == 0.5f && influences[3].part == 0 && influences[3].weight == 0.0f);
		// The third vertex's primary is part 1, lit through slot 1.
		CHECK(lod.vertices.items[2].bone_indices[0] == 1 && lod.vertices.items[2].bone_weights[0] == 1.0f);
	}
	for (int t = 0; t < lod.strips[0].num_triangles; ++t) CHECK(facing(lod, lod.strips[0], t) > 0.0f);
	CHECK(m.collision != nullptr);
	if (m.collision != nullptr) {
		const ThreediCollisionModel &c = *m.collision;
		CHECK(c.object_count == 3 && c.face_count == 1);
		CHECK(c.objects[0].radius == 0x8000 && c.objects[1].radius == 0x4000 && c.objects[1].med[2] == 0x10000);
		CHECK(c.objects[2].num_faces == 1 && c.objects[2].offset[2] == -0x10000);
	}
	threedi_3di3_free(&m);
	if (failures == 0) std::printf("o3d_cli_test --skinned: ok\n");
	return failures == 0 ? 0 : 1;
}

// cross(v1 - v0, v2 - v0) . n for one collision face, in mission axes: retail
// stores collision faces counter-clockwise about their normal.
static double collision_facing(const ThreediCollisionModel &c, size_t vbase, size_t nbase, const ThreediCollisionFace &f) {
	const float *p[3];
	for (int k = 0; k < 3; ++k) p[k] = c.vertices[vbase + f.vert_index[k]].position;
	const double e[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
	const double g[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
	const double x[3] = {e[1] * g[2] - e[2] * g[1], e[2] * g[0] - e[0] * g[2], e[0] * g[1] - e[1] * g[0]};
	const float *n = c.normals[nbase + f.normal_index].normal;
	return x[0] * n[0] + x[1] * n[1] + x[2] * n[2];
}

// The building fixture (building.o3d): LOD types, the empty LOD, the UV1 detail
// stage, an MTRX frame, lights, occlusion planes by the OED rule, a quoted user
// point name, a blink box and the retail collision winding.
static int check_building(const char *path) {
	Threedi3di3 m{};
	if (threedi_3di3_read(path, &m) != 0) {
		std::fprintf(stderr, "cannot read %s\n", path);
		return 1;
	}
	CHECK(m.lod_count == 2 && std::strcmp(m.lods[0].model_type, "bldg") == 0 &&
			std::strcmp(m.lods[1].model_type, "bldg") == 0 && m.lods[1].render_object_count == 0);
	const ThreediLod &lod = m.lods[0];
	// The wall's second vertex tiles the detail texture on UV1.
	CHECK(lod.vertices.count == 8 && near(lod.vertices.items[1].uv0[0], 1.0f) && near(lod.vertices.items[1].uv1[0], 4.0f));
	CHECK(m.material_count == 2 && m.materials[0].texture_count == 2 && m.materials[0].textures[1].slot == 2 &&
			std::strcmp(m.materials[0].textures[1].name, "wall_O.tga") == 0);
	CHECK(m.materials[1].emissive_type == 2 && m.materials[1].rgb_gen.style == 113 && m.materials[1].rgb_gen.reg == 0);
	CHECK(m.ctrl.count == 2 && std::strcmp(m.ctrl.registers[1].name, "DOOR_00") == 0);
	// Part 1 turns in MTRX row 1: the mission quarter turn about up in model axes.
	CHECK(lod.part_animation_count == 2 && lod.part_animations[1].matrix_index == 1 &&
			lod.part_animations[1].rotation_x.control_param == 1);
	CHECK(m.mtrx.count == 2 && near(m.mtrx.matrices[1].m[2], 1.0f) && near(m.mtrx.matrices[1].m[5], 1.0f) &&
			near(m.mtrx.matrices[1].m[8], -1.0f));
	CHECK(m.user_point_count == 1 && std::strcmp(m.user_points[0].name, "ground A") == 0);
	// The omni light keeps the retail default axis and its NaN view_proj; the
	// spot light carries its cone.
	CHECK(m.light_count == 2);
	if (m.light_count == 2) {
		const ThreediLight &omni = m.lights[0], &spot = m.lights[1];
		CHECK(omni.style == 55 && omni.phase == 128 && omni.rate == 76 && omni.subobj_index == 1 && omni.flags == 0x40);
		CHECK(omni.color_start[2] == 255 && omni.color_start[0] == 34 && omni.color_end[2] == 240);
		CHECK(omni.rotation[1] == -1.0f && omni.rotation[3] == 1.0f && std::isnan(omni.view_proj[0]));
		CHECK(spot.flags == 0x48 && spot.falloff_byte == 30 && near(spot.rotation[3], 0.8660254f) &&
				std::isfinite(spot.view_proj[0]));
	}
	// The occluder box's planes are its six bounding planes and each face names
	// its own (Armry01's layout); the window's faces take the +x plane.
	CHECK(m.occlusion_object_count == 2 && m.occlusion_face_count == 14);
	if (m.occlusion_object_count == 2 && m.occlusion_face_count == 14) {
		CHECK(m.occlusion_objects[0].type == 0 && m.occlusion_objects[0].num_planes == 6);
		CHECK((m.occlusion_faces[0].raw_indices >> 24) == 5 && (m.occlusion_faces[2].raw_indices >> 24) == 4 &&
				(m.occlusion_faces[4].raw_indices >> 24) == 0 && (m.occlusion_faces[10].raw_indices >> 24) == 3);
		CHECK(m.occlusion_faces[0].edge_data == 0x03020200u && m.occlusion_faces[0].other_edge_data == 0x8300u);
		CHECK(m.occlusion_objects[1].type == 2 && m.occlusion_objects[1].parent_subobject_index == 1 &&
				(m.occlusion_faces[12].raw_indices >> 24) == 0);
	}
	CHECK(m.collision != nullptr);
	if (m.collision != nullptr) {
		const ThreediCollisionModel &c = *m.collision;
		CHECK(c.object_count == 2 && c.face_count == 2 && c.volume_count == 2);
		for (size_t f = 0; f < c.face_count && c.object_count > 0; ++f) CHECK(collision_facing(c, 0, 0, c.faces[f]) > 0.0);
		// Sections sit at their part's pivot; part 1 carries the blink box.
		CHECK(c.objects[1].offset[0] == 0x10000 && c.objects[1].offset[2] == 0x18000);
		CHECK(c.volume_count == 2 && c.volumes[1].collidable_type == 8 && c.volumes[1].flags == 0x2e);
	}
	threedi_3di3_free(&m);
	if (failures == 0) std::printf("o3d_cli_test --building: ok\n");
	return failures == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
	if (argc == 3 && std::strcmp(argv[1], "--skinned") == 0) return check_skinned(argv[2]);
	if (argc == 3 && std::strcmp(argv[1], "--building") == 0) return check_building(argv[2]);
	if (argc != 2) {
		std::fprintf(stderr, "usage: o3d_cli_test <spinner.3di> | --skinned <skinned.3di> | --building <building.3di>\n");
		return 2;
	}
	Threedi3di3 m{};
	if (threedi_3di3_read(argv[1], &m) != 0) {
		std::fprintf(stderr, "cannot read %s\n", argv[1]);
		return 1;
	}
	CHECK(std::strcmp(m.header.name, "SPINNER") == 0);
	CHECK(m.lod_count == 2);
	CHECK(m.lods[0].lod_threshold == 160 && m.lods[1].lod_threshold == 0);

	const ThreediLod &lod = m.lods[0];
	CHECK(lod.render_object_count == 2);
	// Mission (0, 0, 2) -> model (-y, z, x) = (0, 2, 0).
	CHECK(near(lod.render_objects[1].abs[0], 0.0f) && near(lod.render_objects[1].abs[1], 2.0f) &&
			near(lod.render_objects[1].abs[2], 0.0f));
	// Mission normal +z -> model +y.
	CHECK(near(lod.vertices.items[0].normal[1], 1.0f));
	// Every triangle winds counter-clockwise about its normal in model axes.
	for (size_t s = 0; s < lod.strip_count; ++s)
		for (int t = 0; t < lod.strips[s].num_triangles; ++t) CHECK(facing(lod, lod.strips[s], t) > 0.0f);
	// The rotor strip is the part's alpha strip.
	CHECK(lod.render_objects[1].num_alpha_strips == 1 && lod.render_objects[1].num_strips == 0);

	CHECK(m.ctrl.count == 1 && std::strcmp(m.ctrl.registers[0].name, "HELO_ROTOR") == 0);
	CHECK(lod.part_animation_count == 2);
	const ThreediPartAnimation &spin = lod.part_animations[1];
	CHECK(spin.subobject_index == 1 && spin.parent_subobject == 0);
	CHECK(threedi_panm_rotation_type(spin.flags) == 2);
	// A rotor spins about up: the rotation_x track (Dblkhwk1's HELO_ROTOR).
	CHECK(spin.rotation_x.control == THREEDI_PANM_STYLE_CONTROL_REGISTER && spin.rotation_x.control_param == 0);
	CHECK(spin.rotation_x.end == 16338);
	CHECK(lod.part_animations[0].flags == 0);

	CHECK(m.material_count == 2);
	CHECK(std::strcmp(m.materials[0].textures[0].name, "spinner.tga") == 0);
	CHECK(std::strcmp(m.materials[1].shader_name, "FF_ST_AD_LUM") == 0 && m.materials[1].material_flags == 4);
	CHECK(m.materials[1].alpha_gen.style == 50 && m.materials[1].alpha_gen.reg == -1);
	CHECK(m.materials[1].alpha_gen.start == 120 && m.materials[1].alpha_gen.end == 255);

	CHECK(m.user_point_count == 2);
	CHECK(std::strcmp(m.user_points[0].name, "ground") == 0 && m.user_points[0].z == -65536);
	CHECK(m.user_points[1].subobject_index == -1 && m.user_points[1].userpoint_type == 83);

	CHECK(m.collision != nullptr);
	if (m.collision != nullptr) {
		const ThreediCollisionModel &c = *m.collision;
		CHECK(c.object_count == 2 && c.face_count == 1 && c.volume_count == 2);
		// The 'cvolume' record: type, plane run, outward normal, distance, seam flag.
		CHECK(c.volume_count == 2 && c.volumes[1].collidable_type == 7 && c.volumes[1].plane_count == 7);
		CHECK(c.plane_count == 13 && c.planes[6].flags == 1 && near(c.planes[6].normal[0], 1.0f) &&
				near(c.planes[6].radius, -1.0f));
		CHECK(c.plane_count == 13 && near(c.planes[12].normal[2], 0.70709f) && near(c.planes[12].radius, -1.2f));
		// The scene's counter-clockwise-from-outside order is retail's, stored
		// as given, so the face normal points out of the top (+z).
		CHECK(c.face_count == 1 && near(c.normals[c.faces[0].normal_index].normal[2], 1.0f));
		CHECK(c.faces[0].poly_type == 14);
		CHECK(collision_facing(c, 0, 0, c.faces[0]) > 0.0);
	}
	threedi_3di3_free(&m);
	if (failures == 0) std::printf("o3d_cli_test: ok\n");
	return failures == 0 ? 0 : 1;
}
