#include "common/test_paths.h"

#include "oed/oed.h"
#include "tdp/tdp.h"
#include "threedi/threedi_3di3.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Fixture {
	const char *dir;
	const char *stem;
};

std::vector<unsigned char> read_binary(const fs::path &path) {
	std::ifstream file(path, std::ios::binary);
	return std::vector<unsigned char>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool expect_material_indices(const Threedi3di3 &model, const char *stem) {
	for (uint32_t i = 0; i < model.material_count; ++i) {
		if (!model.materials || model.materials[i].index != static_cast<int32_t>(i)) {
			std::fprintf(stderr,
			             "%s: material %u has in-memory index %d\n",
			             stem,
			             i,
			             model.materials ? model.materials[i].index : -1);
			return false;
		}
	}
	return true;
}

fs::path resolve_ci(const fs::path &dir, const char *name) {
	const fs::path exact = dir / name;
	if (fs::is_regular_file(exact)) {
		return exact;
	}
	const std::string wanted = std::string(name);
	for (const fs::directory_entry &entry : fs::directory_iterator(dir)) {
		if (!entry.is_regular_file()) {
			continue;
		}
		const std::string candidate = entry.path().filename().string();
		if (candidate.size() != wanted.size()) {
			continue;
		}
		bool match = true;
		for (size_t i = 0; i < candidate.size(); ++i) {
			if (std::tolower(static_cast<unsigned char>(candidate[i])) !=
			    std::tolower(static_cast<unsigned char>(wanted[i]))) {
				match = false;
				break;
			}
		}
		if (match) {
			return entry.path();
		}
	}
	return exact;
}

bool run_fixture(const fs::path &root, const Fixture &fixture) {
	const fs::path fixture_dir = root / "fixtures" / "3dp" / fixture.dir;
	const fs::path output_dir = fs::temp_directory_path() / "opennova_oed_build_model_test";
	fs::create_directories(output_dir);
	const fs::path export_path = output_dir / (std::string(fixture.stem) + "_export.3di");
	const fs::path build_path = output_dir / (std::string(fixture.stem) + "_build.3di");
	fs::remove(export_path);
	fs::remove(build_path);

	TdpProject project = {};
	tdp_init(&project);
	const fs::path tdp_path = fixture_dir / (std::string(fixture.stem) + ".3dp");
	if (tdp_parse(tdp_path.string().c_str(), &project) != 0) {
		std::fprintf(stderr, "failed to parse %s\n", tdp_path.string().c_str());
		return false;
	}

	std::vector<std::string> ase_paths;
	std::vector<const char *> ase_ptrs;
	for (int i = 0; i < TDP_MAX_LODS; ++i) {
		if (project.lods[i].scene_file[0] == '\0') {
			break;
		}
		ase_paths.push_back(resolve_ci(fixture_dir, project.lods[i].scene_file).string());
		ase_ptrs.push_back(ase_paths.back().c_str());
	}

	OedSession *session = nullptr;
	const OedStatus create_rc = oed_session_create(ase_ptrs.data(), static_cast<int>(ase_ptrs.size()), &project, &session);
	if (create_rc != OED_STATUS_OK) {
		std::fprintf(stderr, "oed_session_create failed for %s (%d)\n", fixture.stem, static_cast<int>(create_rc));
		tdp_free(&project);
		return false;
	}

	OedExportRequest request = {};
	// Keep the path string alive for the duration of the request: passing
	// export_path.string().c_str() directly dangles once the temporary
	// std::string dies at the end of the statement (benign on MSVC, garbage
	// on clang/macOS).
	const std::string export_path_str = export_path.string();
	request.project = &project;
	request.output_path = export_path_str.c_str();
	request.update_mask = static_cast<unsigned char>(OED_UPDATE_ALL);
	const OedStatus export_rc = oed_session_export(session, &request);
	if (export_rc != OED_STATUS_OK) {
		const char *msg = oed_session_last_error(session);
		std::fprintf(stderr, "oed_session_export failed for %s (%d): %s\n", fixture.stem,
		             static_cast<int>(export_rc), msg ? msg : "(no detail)");
		oed_session_destroy(session);
		tdp_free(&project);
		return false;
	}

	Threedi3di3 model = {};
	const OedStatus build_rc = oed_session_build_model(
			session, &project, static_cast<unsigned char>(OED_UPDATE_ALL), nullptr, &model);
	oed_session_destroy(session);
	tdp_free(&project);
	if (build_rc != OED_STATUS_OK) {
		std::fprintf(stderr, "oed_session_build_model failed for %s (%d)\n", fixture.stem, static_cast<int>(build_rc));
		return false;
	}
	if (!expect_material_indices(model, fixture.stem)) {
		threedi_3di3_free(&model);
		return false;
	}

	const int write_rc = threedi_3di3_write(build_path.string().c_str(), &model);
	threedi_3di3_free(&model);
	if (write_rc != 0) {
		std::fprintf(stderr, "threedi_3di3_write failed for %s\n", fixture.stem);
		return false;
	}

	const std::vector<unsigned char> exported = read_binary(export_path);
	const std::vector<unsigned char> built = read_binary(build_path);
	fs::remove(export_path);
	fs::remove(build_path);
	if (exported != built) {
		std::fprintf(stderr,
		             "%s: build/export mismatch export=%zu build=%zu\n",
		             fixture.stem,
		             exported.size(),
		             built.size());
		return false;
	}
	std::printf("%s: build/export in-memory PASS\n", fixture.stem);
	return true;
}

} // namespace

int main() {
	const fs::path root = test_paths_repo_root(__FILE__);
	const Fixture fixtures[] = {
		{"armry01", "Armry01"},
		{"Bird1", "Bird1"},
		{"CarierU", "CarierU"},
		{"dapche2", "dapche2"},
		{"dm1a1", "dm1a1"},
		{"dsuv1", "dsuv1"},
		{"m1trret", "M1trret"},
		{"US01_onimport", "US01"},
	};

	bool failed = false;
	for (const Fixture &fixture : fixtures) {
		if (!run_fixture(root, fixture)) {
			failed = true;
		}
	}
	return failed ? 1 : 0;
}
