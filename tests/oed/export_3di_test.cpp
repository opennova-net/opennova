#include "common/test_paths.h"

#include "oed/oed.h"
#include "tdp/tdp.h"
#include "threedi/threedi_compare.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
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

const char *default_compare_chunks() {
	return "INFO,GHDR,USRP,CTRL,MTRL,LGHT,MTRX,OVRT,OCCL,CMDL,BPLN,BVOL,CVRT,CNRM,CFAC,COBJ,CXLT,RDTA,RLOD,RMDL,VERT,INDX,STRP,ROBJ,PANM,CDTA";
}

std::vector<unsigned char> read_binary(const fs::path &path) {
	std::ifstream file(path, std::ios::binary);
	return std::vector<unsigned char>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
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

bool export_fixture(const fs::path &fixture_dir, const char *stem, const fs::path &generated_path) {
	const fs::path tdp_path = fixture_dir / (std::string(stem) + ".3dp");
	TdpProject project = {};
	tdp_init(&project);
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
	if (ase_ptrs.empty()) {
		std::fprintf(stderr, "no LOD ASE files in %s\n", tdp_path.string().c_str());
		tdp_free(&project);
		return false;
	}

	OedSession *session = nullptr;
	const OedStatus create_rc = oed_session_create(ase_ptrs.data(), static_cast<int>(ase_ptrs.size()), &project, &session);
	if (create_rc != OED_STATUS_OK) {
		std::fprintf(stderr, "oed_session_create failed for %s (%d)\n", stem, static_cast<int>(create_rc));
		tdp_free(&project);
		return false;
	}

	OedExportRequest request = {};
	request.project = &project;
	request.output_path = generated_path.string().c_str();
	request.update_mask = static_cast<unsigned char>(OED_UPDATE_ALL);
	const OedStatus export_rc = oed_session_export(session, &request);
	std::string export_err;
	if (export_rc != OED_STATUS_OK) {
		if (const char *msg = oed_session_last_error(session)) {
			export_err = msg;
		}
	}
	oed_session_destroy(session);
	tdp_free(&project);
	if (export_rc != OED_STATUS_OK) {
		std::fprintf(stderr, "oed_session_export failed for %s (%d): %s\n", stem,
		             static_cast<int>(export_rc), export_err.empty() ? "(no detail)" : export_err.c_str());
		return false;
	}
	return true;
}

bool run_fixture(const fs::path &root, const Fixture &fixture) {
	const fs::path fixture_dir = root / "fixtures" / "3dp" / fixture.dir;
	const fs::path reference_path = fixture_dir / (std::string(fixture.stem) + ".3di");
	const fs::path output_dir = fs::temp_directory_path() / "opennova_oed_export_3di_test";
	fs::create_directories(output_dir);
	const fs::path generated_path = output_dir / (std::string(fixture.stem) + ".3di");
	fs::remove(generated_path);

	if (!export_fixture(fixture_dir, fixture.stem, generated_path)) {
		return false;
	}

	const std::vector<unsigned char> reference = read_binary(reference_path);
	const std::vector<unsigned char> generated = read_binary(generated_path);

	if (reference == generated) {
		fs::remove(generated_path);
		std::printf("%s: byte-exact export PASS (%zu bytes)\n", fixture.stem, reference.size());
		return true;
	}

	size_t first_diff = 0;
	const size_t min_size = std::min(reference.size(), generated.size());
	while (first_diff < min_size && reference[first_diff] == generated[first_diff]) {
		++first_diff;
	}
	std::fprintf(stderr,
	             "%s: byte-exact export FAIL ref=%zu gen=%zu first_diff=%zu generated=%s\n",
	             fixture.stem,
	             reference.size(),
	             generated.size(),
	             first_diff,
	             generated_path.string().c_str());
	const char *chunk_filter = std::getenv("OED_EXPORT_CHUNKS");
	if (chunk_filter == nullptr || chunk_filter[0] == '\0') {
		chunk_filter = default_compare_chunks();
	}
	char report[4096] = {};
	const int compare_rc = threedi_3di3_compare_file_chunks(
			reference_path.string().c_str(),
			generated_path.string().c_str(),
			chunk_filter,
			report,
			sizeof(report));
	if (compare_rc == 0) {
		std::fprintf(stderr, "%s: selected chunks match (%s)\n", fixture.stem, chunk_filter);
	} else if (report[0] != '\0') {
		std::fprintf(stderr, "%s: chunk diff: %s\n", fixture.stem, report);
	} else {
		std::fprintf(stderr, "%s: chunk diff unavailable (rc=%d)\n", fixture.stem, compare_rc);
	}
	return false;
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
	};

	bool failed = false;
	const char *filter = std::getenv("OED_EXPORT_FIXTURE");
	for (const Fixture &fixture : fixtures) {
		if (filter != nullptr && std::string(filter) != fixture.stem && std::string(filter) != fixture.dir) {
			continue;
		}
		if (!run_fixture(root, fixture)) {
			failed = true;
		}
	}
	return failed ? 1 : 0;
}
