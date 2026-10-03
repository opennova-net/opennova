// The retail-root seam (common/retail_paths.h): a root resolves only when it
// names an existing directory. A root naming a folder that is not there is as
// good as unset, the rule the GUT resolver keeps (godot/tests/support/
// retail_data.gd _dir), so every gated row reports Skipped instead of walking
// a directory that does not exist (the lwf / cpt asset sweeps died with
// 0xc0000409 on the directory_iterator's throw).

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

void set_root(const char *name, const std::string &value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

int test_roots_resolve_only_to_directories(const fs::path &scratch) {
    const fs::path missing = scratch / "no-such-folder";
    set_root("OPENNOVA_JO_ASSETS", missing.string());
    set_root("OPENNOVA_JO_DIR", missing.string());
    TEST_EXPECT(retail::assets().empty());
    TEST_EXPECT(retail::install().empty());
    TEST_EXPECT(retail::asset_file("items.def").empty());
    TEST_EXPECT(retail::expansions().empty());
    TEST_EXPECT(retail::weapon_sav().empty());

    const fs::path file = scratch / "a-file";
    std::ofstream(file.string()) << "not a directory";
    set_root("OPENNOVA_JO_ASSETS", file.string());
    set_root("OPENNOVA_JO_DIR", file.string());
    TEST_EXPECT(retail::assets().empty());
    TEST_EXPECT(retail::install().empty());

    set_root("OPENNOVA_JO_ASSETS", scratch.string() + "/");
    set_root("OPENNOVA_JO_DIR", scratch.string());
    TEST_EXPECT(retail::assets() == scratch.string());
    TEST_EXPECT(retail::install() == scratch.string());
    return 0;
}

} // namespace

int main() {
    std::error_code ec;
    const fs::path scratch =
            fs::temp_directory_path(ec) / test_paths_unique("opennova_retail_paths_test");
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);
    const int failed = test_roots_resolve_only_to_directories(scratch);
    fs::remove_all(scratch, ec);
    if (failed != 0) return 1;
    std::printf("retail_paths: a root resolves only to an existing directory\n");
    return 0;
}
