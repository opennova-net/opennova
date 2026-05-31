#include "common/test_paths.h"

#include "oed/oed.h"
#include "tdp/tdp.h"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

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

bool contains(const char *haystack, const char *needle) {
	return haystack != nullptr && std::string(haystack).find(needle) != std::string::npos;
}

} // namespace

int main() {
	const fs::path root = test_paths_repo_root(__FILE__);
	const fs::path fixture_dir = root / "fixtures" / "3dp" / "Bird1";
	const fs::path tdp_path = fixture_dir / "Bird1.3dp";

	TdpProject project = {};
	tdp_init(&project);
	if (tdp_parse(tdp_path.string().c_str(), &project) != 0) {
		std::fprintf(stderr, "failed to parse %s\n", tdp_path.string().c_str());
		return 1;
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
		std::fprintf(stderr, "oed_session_create failed (%d)\n", static_cast<int>(create_rc));
		tdp_free(&project);
		return 1;
	}

	const std::string missing_dir_name = "opennova_oed_missing_dir_" +
	                                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
	const fs::path output_path = fs::temp_directory_path() / missing_dir_name / "Bird1.3di";

	OedExportRequest request = {};
	// Keep the path string alive: output_path.string().c_str() dangles once the
	// temporary std::string dies at the end of the statement.
	const std::string output_path_str = output_path.string();
	request.project = &project;
	request.output_path = output_path_str.c_str();
	request.update_mask = static_cast<unsigned char>(OED_UPDATE_ALL);
	const OedStatus export_rc = oed_session_export(session, &request);
	std::string last_error;
	if (const char *message = oed_session_last_error(session)) {
		last_error = message;
	}
	oed_session_destroy(session);
	tdp_free(&project);

	if (export_rc != OED_STATUS_EXPORT_FAILED) {
		std::fprintf(stderr, "expected export failure, got %d\n", static_cast<int>(export_rc));
		return 1;
	}
	if (!contains(last_error.c_str(), "Failed to copy baseline 3DI")) {
		std::fprintf(stderr, "unexpected last error: %s\n", last_error.c_str());
		return 1;
	}

	std::printf("oed_session_last_error reports export diagnostics\n");
	return 0;
}
