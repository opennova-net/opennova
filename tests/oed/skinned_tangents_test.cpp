// The skinned-extended vertex layout (WriteVertices_SkinnedExtended): a skinned model
// whose material is a tangent-space shader compiles to flags 0x55 / stride 80 with
// tangent + bitangent per vertex; VS_SKBASIC (and the object-space bump shaders) keep
// the basic 0x41 / 56 layout. Pinned on the shipped JO corpus: every skinned LOD with a
// tangent-space shader is 0x55 (457/457) and every first-person arms model in
// Avatars.def ships that layout; the 0x41 skinned LODs use only VS_SKBASIC/OBJ shaders.
#include "common/test_paths.h"

#include "oed/oed.h"
#include "tdp/tdp.h"
#include "threedi/threedi_3di3.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool build_bird(const fs::path &root, const char *shader_tag, Threedi3di3 &out) {
	const fs::path fixture_dir = root / "fixtures" / "3dp" / "Bird1";
	TdpProject project = {};
	tdp_init(&project);
	if (tdp_parse((fixture_dir / "Bird1.3dp").string().c_str(), &project) != 0) {
		std::fprintf(stderr, "failed to parse Bird1.3dp\n");
		return false;
	}
	if (project.material_count < 1) {
		std::fprintf(stderr, "Bird1.3dp has no materials\n");
		tdp_free(&project);
		return false;
	}
	std::snprintf(project.materials[0].shader_tag, sizeof(project.materials[0].shader_tag), "%s", shader_tag);
	std::snprintf(project.materials[0].name, sizeof(project.materials[0].name), "Material_0_%s", shader_tag);
	const std::string ase = (fixture_dir / project.lods[0].scene_file).string();
	const char *ase_ptr = ase.c_str();
	OedSession *session = nullptr;
	if (oed_session_create(&ase_ptr, 1, &project, &session) != OED_STATUS_OK) {
		std::fprintf(stderr, "oed_session_create failed for %s\n", shader_tag);
		tdp_free(&project);
		return false;
	}
	std::memset(&out, 0, sizeof(out));
	const OedStatus rc = oed_session_build_model(session, &project, OED_UPDATE_ALL, "Bird1", &out);
	oed_session_destroy(session);
	tdp_free(&project);
	if (rc != OED_STATUS_OK) {
		std::fprintf(stderr, "oed_session_build_model failed for %s\n", shader_tag);
		return false;
	}
	return true;
}

bool unit(const float *v) {
	const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	return std::isfinite(len) && std::fabs(len - 1.0f) < 1e-3f;
}

}  // namespace

int main() {
	const fs::path root = test_paths_repo_root(__FILE__);
	int failures = 0;

	// 1. VS_SKBASIC (the fixture as shipped): basic skinned layout.
	Threedi3di3 basic = {};
	if (!build_bird(root, "VS_SKBASIC", basic)) return 1;
	const ThreediLod &lb = basic.lods[0];
	if (lb.vertices.flags != (THREEDI_VERTEX_FLAG_SKINNED | 1u) || lb.vertices.stride != 56) {
		std::fprintf(stderr, "VS_SKBASIC: expected flags 0x41 stride 56, got 0x%x stride %u\n",
		             lb.vertices.flags, lb.vertices.stride);
		++failures;
	}

	// 2. A tangent-space skinned shader: skinned-extended layout with unit tangent frames.
	Threedi3di3 ext = {};
	if (!build_bird(root, "VS_SKBUMPPHONGT", ext)) return 1;
	const ThreediLod &le = ext.lods[0];
	if (le.vertices.flags != (THREEDI_VERTEX_FLAG_SKINNED | THREEDI_VERTEX_FLAG_TANGENTS | 1u) ||
	    le.vertices.stride != 80) {
		std::fprintf(stderr, "VS_SKBUMPPHONGT: expected flags 0x55 stride 80, got 0x%x stride %u\n",
		             le.vertices.flags, le.vertices.stride);
		++failures;
	}
	if (le.vertices.count != lb.vertices.count) {
		std::fprintf(stderr, "vertex count changed with the layout: %u vs %u\n", le.vertices.count, lb.vertices.count);
		++failures;
	}
	size_t bad_frames = 0;
	for (uint32_t i = 0; i < le.vertices.count; ++i) {
		const ThreediVertex &v = le.vertices.items[i];
		if (!v.has_tangents || !v.is_skinned || !unit(v.tangent) || !unit(v.bitangent)) ++bad_frames;
		// the skinning payload is untouched by the layout choice
		const ThreediVertex &b = lb.vertices.items[i];
		if (std::memcmp(v.bone_indices, b.bone_indices, sizeof(v.bone_indices)) != 0 ||
		    std::memcmp(v.bone_weights, b.bone_weights, sizeof(v.bone_weights)) != 0 ||
		    std::memcmp(v.position, b.position, sizeof(v.position)) != 0) ++bad_frames;
	}
	if (bad_frames != 0) {
		std::fprintf(stderr, "%zu vertices with a bad tangent frame or altered skinning payload\n", bad_frames);
		++failures;
	}

	// 3. The extended layout survives the 3DI writer/reader.
	const fs::path out_dir = fs::temp_directory_path() / "opennova_oed_skinned_tangents_test";
	fs::create_directories(out_dir);
	const fs::path out_path = out_dir / "Bird1_ext.3di";
	if (threedi_3di3_write(out_path.string().c_str(), &ext) != 0) {
		std::fprintf(stderr, "threedi_3di3_write failed\n");
		++failures;
	} else {
		Threedi3di3 back = {};
		if (threedi_3di3_read(out_path.string().c_str(), &back) != 0) {
			std::fprintf(stderr, "threedi_3di3_read failed on the extended layout\n");
			++failures;
		} else {
			const ThreediLod &lr = back.lods[0];
			if (lr.vertices.flags != le.vertices.flags || lr.vertices.stride != 80 || lr.vertices.count != le.vertices.count) {
				std::fprintf(stderr, "roundtrip changed the vertex buffer: flags 0x%x stride %u count %u\n",
				             lr.vertices.flags, lr.vertices.stride, lr.vertices.count);
				++failures;
			} else {
				size_t diff = 0;
				for (uint32_t i = 0; i < lr.vertices.count; ++i) {
					if (std::memcmp(lr.vertices.items[i].tangent, le.vertices.items[i].tangent, sizeof(float) * 3) != 0 ||
					    std::memcmp(lr.vertices.items[i].bitangent, le.vertices.items[i].bitangent, sizeof(float) * 3) != 0) ++diff;
				}
				if (diff != 0) {
					std::fprintf(stderr, "%zu tangent frames changed across the roundtrip\n", diff);
					++failures;
				}
			}
			threedi_3di3_free(&back);
		}
	}

	threedi_3di3_free(&basic);
	threedi_3di3_free(&ext);
	if (failures == 0) std::printf("skinned tangents: PASS (basic 0x41/56, tangent-space 0x55/80, roundtrip)\n");
	return failures == 0 ? 0 : 1;
}
