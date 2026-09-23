// Reads back the .3di opennova-3di minted from tests/fixtures/threedi/o3d/
// spinner.o3d (the threedi_cli_build ctest runs first) and checks the
// scene -> model conversions the CLI owns: mission -> model axes, the
// counter-clockwise-in-model winding retail uses, the register-driven PANM
// row, the material generator, user points and the collision face order.
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

int main(int argc, char **argv) {
	if (argc != 2) {
		std::fprintf(stderr, "usage: o3d_cli_test <spinner.3di>\n");
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
	CHECK(spin.rotation_z.control == THREEDI_PANM_STYLE_CONTROL_REGISTER && spin.rotation_z.control_param == 0);
	CHECK(spin.rotation_z.end == 16338);
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
		// Counter-clockwise from outside in the scene becomes retail's clockwise
		// order, so the stored face normal still points out of the top (+z).
		CHECK(c.face_count == 1 && near(c.normals[c.faces[0].normal_index].normal[2], 1.0f));
		CHECK(c.faces[0].poly_type == 14);
	}
	threedi_3di3_free(&m);
	if (failures == 0) std::printf("o3d_cli_test: ok\n");
	return failures == 0 ? 0 : 1;
}
