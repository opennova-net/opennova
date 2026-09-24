// Exercise the same command handlers the CLI dispatches, including negative
// cases: successful readback alone does not establish semantic equivalence.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <formats/threedi/threedi_3di3.h>

#include "../../apps/threedi_cli/threedi_cli.h"

using namespace opennova::threedi;

namespace {
int failures = 0;
void check(bool ok, const char *what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++failures;
	}
}

const std::string prefix = "o3d 1\nmodel CHECK\nskinned 1\nuv1 1\nmaterial VS_SKBASIC\n"
		"lod 0 gnrc\npart 0 0 0 0\npart 0 0 0 1\npart 0 0 0 0\nstrip 0\nbones 0 1\n";
const std::string verts =
		"v 0 0 0 0 0 1 0 0 0 0 0 1 0 0.75 0.25 0\n"
		"v 1 0 0 0 0 1 1 0 1 0 0 1 0 0.5 0.5 0\n"
		"v 0 1 0 0 0 1 0 1 0 1 0 1 0 0.25 0.75 0\n";
const std::string suffix = "t 0 1 2\npanm 0 0\npanm 1 0\npanm 2 0\n";

std::string replace(std::string s, const std::string &from, const std::string &to) {
	s.replace(s.find(from), from.size(), to);
	return s;
}
} // namespace

int main(int argc, char **argv) {
	if (argc != 2) return 2;
	const auto dir = std::filesystem::path(argv[1]) / "o3d-commands";
	std::filesystem::create_directories(dir);
	const auto build = [&](const char *name, const std::string &text, bool valid = true) {
		const auto scene = (dir / (std::string(name) + ".o3d")).string();
		const auto model = (dir / (std::string(name) + ".3di")).string();
		std::ofstream(scene) << text;
		const int result = threedi_cli::cmd_build(scene.c_str(), model.c_str());
		check(valid ? result == 0 : result != 0, name);
		return model;
	};
	const auto original = build("original", prefix + verts + suffix);
	const auto compare = [&](const char *name, const std::string &text, bool same) {
		const auto path = build(name, text);
		check(threedi_cli::cmd_compare(original.c_str(), path.c_str()) == (same ? 0 : 1), name);
	};
	compare("identical", prefix + verts + suffix, true);
	compare("cyclic-corners", prefix + verts + replace(suffix, "t 0 1 2", "t 1 2 0"), true);
	compare("winding", prefix + verts + replace(suffix, "t 0 1 2", "t 0 2 1"), false);
	compare("weights", prefix + replace(verts, "0.75 0.25 0", "0.25 0.75 0") + suffix, false);
	compare("bone-table", replace(prefix, "bones 0 1", "bones 1 0") + verts + suffix, false);
	// UV swaps preserve each mean, but move the texture to different corners.
	compare("uv0", prefix + replace(replace(verts,
			"v 1 0 0 0 0 1 1 0", "v 1 0 0 0 0 1 0 1"),
			"v 0 1 0 0 0 1 0 1", "v 0 1 0 0 0 1 1 0") + suffix, false);
	compare("uv1", prefix + replace(replace(verts,
			"v 1 0 0 0 0 1 1 0 1 0", "v 1 0 0 0 0 1 1 0 0 1"),
			"v 0 1 0 0 0 1 0 1 0 1", "v 0 1 0 0 0 1 0 1 1 0") + suffix, false);
	compare("remapped-bones", replace(prefix, "bones 0 1", "bones 1 0") +
			replace(replace(replace(verts, "0.75 0.25 0", "0.25 0.75 0"),
					"v 1 0 0 0 0 1 1 0 1 0 0 1 0", "v 1 0 0 0 0 1 1 0 1 0 1 0 0"),
					"v 0 1 0 0 0 1 0 1 0 1 0 1 0", "v 0 1 0 0 0 1 0 1 0 1 1 0 0") + suffix, true);
	build("undeclared-track-register", prefix + verts + suffix + "track rotx 113 0 0 0 100\n", false);
	build("wrapped-panm-part", prefix + verts + "t 0 1 2\npanm 256 0\n", false);
	build("wrapped-panm-parent", prefix + verts + "t 0 1 2\npanm 0 256\n", false);
	build("undeclared-flipbook-register", replace(prefix, "material VS_SKBASIC\n",
			"material VS_SKBASIC\ntexanim 2 1 0\n") + verts + suffix, false);
	const std::string flipbook = replace(replace(prefix, "material VS_SKBASIC\n",
			"register TEX_CAMO1\nregister TEX_CAMO2\nmaterial VS_SKBASIC\ntexanim 2 1 1\n"),
			"model CHECK", "model FLIPBOOK") + verts + suffix;
	const auto flip_a = build("flipbook", flipbook);
	const auto flip_b = build("flipbook-remapped", replace(replace(flipbook,
			"register TEX_CAMO1\nregister TEX_CAMO2", "register TEX_CAMO2\nregister TEX_CAMO1"),
			"texanim 2 1 1", "texanim 2 1 0"));
	check(threedi_cli::cmd_compare(flip_a.c_str(), flip_b.c_str()) == 0, "remapped flipbook register");
	std::string nearby = "o3d 1\nmodel NEARBY\nmaterial FF_ST_OP\nlod 0\npart 0 0 0 0\n";
	for (double x : {0.0, 0.0015, -0.0015})
		nearby += "strip 0\nv " + std::to_string(x) + " 0 0 0 0 1 0 0\nv " + std::to_string(x + 1) +
				" 0 0 0 0 1 1 0\nv " + std::to_string(x) + " 1 0 0 0 1 0 1\nt 0 1 2\n";
	const auto nearby_path = build("nearby", nearby);
	check(threedi_cli::cmd_compare(nearby_path.c_str(), nearby_path.c_str()) == 0, "nearly coincident faces");
	const std::string plain = "o3d 1\nmodel FACES\nlod 0\npart 0 0 0 0\n";
	const std::string collision = "cobj 0\ncv 0 0 0\ncv 1 0 0\ncv 0 1 0\ncf 0 1 2\n";
	const auto collision_a = build("collision", plain + collision);
	const auto collision_b = build("collision-moved", plain +
			"cobj 0\ncv 10 0 0\ncv 11 0 0\ncv 10 1 0\ncf 0 1 2\n");
	check(threedi_cli::cmd_compare(collision_a.c_str(), collision_b.c_str()) == 1, "moved collision faces");
	const auto normal_a = build("stored-normal", plain + replace(collision, "cf 0 1 2", "cf 0 1 2 14 0 0.5 0 0.75"));
	const auto normal_scene = (dir / "stored-normal.rt.o3d").string();
	const auto normal_b = (dir / "stored-normal.rt.3di").string();
	check(threedi_cli::cmd_scene(normal_a.c_str(), normal_scene.c_str()) == 0, "scene with stored normal");
	check(threedi_cli::cmd_build(normal_scene.c_str(), normal_b.c_str()) == 0, "rebuild stored normal");
	check(threedi_cli::cmd_compare(normal_a.c_str(), normal_b.c_str()) == 0, "preserve stored collision normal");
	const std::string occlusion = "occ 0 0 0\nov 0 0 0\nov 1 0 0\nov 0 1 0\nop 0 0 1 0\nof 0 1 2 0\n";
	const auto occ_a = build("occlusion", plain + occlusion);
	const auto occ_b = build("occlusion-reversed", plain + replace(occlusion, "of 0 1 2", "of 0 2 1"));
	check(threedi_cli::cmd_compare(occ_a.c_str(), occ_b.c_str()) == 1, "reversed occlusion faces");
	std::string overflow = plain + "cobj 0\n";
	for (int i = 0; i < 32769; ++i) overflow += "cv 0 0 0\n";
	build("collision-index-overflow", overflow, false);

	// The derived collision rules, read back from the minted bytes.
	const auto read = [&](const std::string &path, Threedi3di3 &m) {
		m = Threedi3di3{};
		const bool ok = threedi_3di3_read(path.c_str(), &m) == 0;
		check(ok, ("read back " + path).c_str());
		return ok;
	};
	Threedi3di3 m{};
	// A bullet face on z = 1 facing +z: the runtime tests n . p + plane_dist.
	if (read(build("plane-distance", plain + "cobj 0\ncv 0 0 1\ncv 1 0 1\ncv 0 1 1\ncf 0 1 2\n"), m)) {
		check(m.collision != nullptr && m.collision->face_count == 1 && m.collision->faces[0].plane_dist_fp16 == -65536,
				"bullet-face plane distance is -(n . v0)");
		threedi_3di3_free(&m);
	}
	// A unit box as triangles, counter-clockwise from outside; `last` names
	// the side whose triangles come last.
	const auto box = [](double x0, double y0, double z0, double x1, double y1, double z1, int last) {
		std::string s;
		for (int i = 0; i < 8; ++i)
			s += "vv " + std::to_string((i & 1) ? x1 : x0) + " " + std::to_string((i & 2) ? y1 : y0) + " " +
					std::to_string((i & 4) ? z1 : z0) + "\n";
		// +x -x +y -y +z -z, each side counter-clockwise from outside.
		const int sides[6][4] = {{1, 3, 7, 5}, {0, 4, 6, 2}, {2, 6, 7, 3}, {0, 1, 5, 4}, {4, 5, 7, 6}, {0, 2, 3, 1}};
		for (int k = 0; k < 6; ++k) {
			const int *q = sides[k == 5 ? last : (k >= last ? k + 1 : k)];
			s += "vf " + std::to_string(q[0]) + " " + std::to_string(q[1]) + " " + std::to_string(q[2]) + "\n";
			s += "vf " + std::to_string(q[0]) + " " + std::to_string(q[2]) + " " + std::to_string(q[3]) + "\n";
		}
		return s;
	};
	// A ladder faces the plane of its last triangle (plane 0 after the swap);
	// a box's faces fold into its six box planes.
	if (read(build("ladder", plain + "cobj 0\ncvmesh 4 0\n" + box(0, 0, 0, 1, 1, 2, 3) + "cvmesh 1 0\n" +
			box(2, 0, 0, 3, 1, 1, 5)), m)) {
		check(m.collision != nullptr && m.collision->volume_count == 2 && m.collision->volumes[0].plane_count == 6 &&
				m.collision->volumes[1].plane_count == 6, "a box volume keeps its six planes");
		check(m.collision != nullptr && m.collision->planes[0].normal[1] == -1.0f && m.collision->planes[3].normal[0] == 1.0f,
				"a ladder's plane 0 is its last triangle's plane");
		check(m.collision != nullptr && m.collision->objects[0].radius == 0, "a volume-only section has radius 0");
		threedi_3di3_free(&m);
	}
	// Seams: a CB inside another CB flags the planes of its faces; the outer
	// one's faces lie inside nothing.
	if (read(build("seams", plain + "cobj 0\ncvmesh 1 0\n" + box(0, 0, 0, 1, 1, 1, 5) + "cvmesh 1 0\n" +
			box(-1, -1, -1, 2, 2, 2, 5)), m)) {
		int inner = 0, outer = 0;
		for (int p = 0; m.collision != nullptr && p < 6; ++p) {
			inner += m.collision->planes[p].flags;
			outer += m.collision->planes[6 + p].flags;
		}
		check(inner == 6 && outer == 0, "seam flags follow the OED overlap rule");
		threedi_3di3_free(&m);
	}
	// No section at all: the CMDL box still bounds LOD 0 while its radii,
	// taken from the collision LOD's faces, stay empty (retail CNet01: 0 0
	// and a height of -20000).
	if (read(build("no-collision", "o3d 1\nmodel NOCOLL\nmaterial FF_ST_OP\nlod 0\npart 0 0 0 0\nstrip 0\n"
			"v 0 0 0 0 0 1 0 0\nv 2 0 0 0 0 1 1 0\nv 0 1 3 0 0 1 0 1\nt 0 1 2\n"), m)) {
		check(m.collision != nullptr && m.collision->object_count == 0 && m.collision->model_data.bbox[3] == 2.0f &&
				m.collision->model_data.radii[0] == 0.0f && m.collision->model_data.radii[2] == -20000.0f,
				"a model without sections bounds LOD 0 in its CMDL box only");
		threedi_3di3_free(&m);
	}
	// A tangent-space shader lays out tangents, derived from the UVs: u runs
	// along mission +x, model (0 0 1); v (down) along mission -y, model (1 0 0).
	if (read(build("tangents", "o3d 1\nmodel TANGENT\nmaterial VS_PHONGT\nlod 0\npart 0 0 0 0\nstrip 0\n"
			"v 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 -1 0 0 0 1 0 1\nt 0 1 2\n"), m)) {
		const ThreediVertex *v = m.lod_count > 0 && m.lods[0].vertices.count > 0 ? &m.lods[0].vertices.items[0] : nullptr;
		check(v != nullptr && (m.lods[0].vertices.flags & THREEDI_VERTEX_FLAG_TANGENTS) == THREEDI_VERTEX_FLAG_TANGENTS,
				"a tangent-space shader lays out tangents");
		check(v != nullptr && v->tangent[2] > 0.999f && v->bitangent[0] > 0.999f, "tangents follow the UVs");
		threedi_3di3_free(&m);
	}
	std::printf("o3d_commands: %d failures\n", failures);
	return failures ? 1 : 0;
}
