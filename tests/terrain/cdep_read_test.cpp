// CDEP header parity. Loads a captured Dvxi5.cpt (known to be CDEP
// encoded, terrain_name="Dvxi5", creator="Brophy") and verifies the
// header decode + depth buffer shape.

#include <cpt/cpt.h>
#include "common/test_paths.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

int main() {
    const fs::path repo_root = test_paths_repo_root(__FILE__);
    const fs::path cpt_path =
        repo_root / "fixtures" / "terrain" / "dvxi5" / "Dvxi5.cpt";
    if (!fs::exists(cpt_path)) {
        std::fprintf(stderr, "FAIL: missing fixture %s\n",
                     cpt_path.string().c_str());
        return 1;
    }

    opennova::CptFile cpt;
    try {
        cpt = opennova::CptFile::read(cpt_path.string());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: CptFile::read threw: %s\n", e.what());
        return 1;
    }

    std::string name = cpt.header.terrain_name;
    std::string creator = cpt.header.creator;

    if (name != "Dvxi5") {
        std::fprintf(stderr, "FAIL: terrain_name = \"%s\", expected \"Dvxi5\"\n",
                     name.c_str());
        return 1;
    }
    if (creator != "Brophy") {
        std::fprintf(stderr, "FAIL: creator = \"%s\", expected \"Brophy\"\n",
                     creator.c_str());
        return 1;
    }
    if (cpt.depth_format != opennova::DepthFormat::CDEP) {
        std::fprintf(stderr, "FAIL: depth_format is not CDEP\n");
        return 1;
    }
    if (cpt.depth_buffer.size() != 1024u * 1024u) {
        std::fprintf(stderr,
                     "FAIL: depth_buffer size = %zu, expected %u\n",
                     cpt.depth_buffer.size(), 1024u * 1024u);
        return 1;
    }

    std::printf("OK: CDEP header parity — name=%s creator=%s fmt=CDEP buffer=%zu\n",
                name.c_str(), creator.c_str(), cpt.depth_buffer.size());
    return 0;
}
