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

// A reference corpus folder lists its files of one extension, without case, sorted;
// a folder that is not there lists none.
int test_reference_fixture_files(const fs::path &scratch) {
    set_root("OPENNOVA_JO_ASSETS", scratch.string());
    TEST_EXPECT(retail::reference_fixture_files("bhd/grm", ".grm").empty());
    const fs::path dir = scratch / "fixtures" / "bhd" / "grm";
    std::error_code ec;
    fs::create_directories(dir / "nested.grm", ec);
    std::ofstream((dir / "b.grm").string()) << "b";
    std::ofstream((dir / "A.GRM").string()) << "a";
    std::ofstream((dir / "c.txt").string()) << "c";
    const std::vector<std::string> files = retail::reference_fixture_files("bhd/grm", ".grm");
    TEST_EXPECT(files.size() == 2);
    TEST_EXPECT(files.size() == 2 && fs::path(files[0]).filename() == "A.GRM" &&
                fs::path(files[1]).filename() == "b.grm");
    return 0;
}

} // namespace

int main() {
    std::error_code ec;
    const fs::path scratch =
            fs::temp_directory_path(ec) / test_paths_unique("opennova_retail_paths_test");
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);
    int failed = test_roots_resolve_only_to_directories(scratch);
    failed += test_reference_fixture_files(scratch);
    fs::remove_all(scratch, ec);
    if (failed != 0) return 1;
    std::printf("retail_paths: a root resolves only to an existing directory; a corpus folder lists its files\n");
    return 0;
}
