/* resource_index MUS .bin classification.

   ResourceIndex should classify the bytes users can actually open through the
   generic VFS payload decoder: plaintext SCR0 and explicit SCR-wrapped SCR0.
   Headerless MUS ciphertext is intentionally not auto-detected. */

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "common/mus_scr_fixture.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include <base/resource_index/resource_index.h>

namespace fs = std::filesystem;

namespace {

using mus_scr_fixture::headerless_mus_ciphertext;
using mus_scr_fixture::minimal_plain_mus;

static void write_bytes(const fs::path &path, const std::vector<uint8_t> &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

static bool has(const std::vector<opennova::ResourceFileEntry> &entries, const std::string &rel) {
    return std::any_of(entries.begin(), entries.end(),
        [&](const opennova::ResourceFileEntry &e) { return e.relative_path == rel; });
}

} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / test_paths_unique("opennova_resource_index_mus_test");
    fs::remove_all(root);
    fs::create_directories(root);

    const std::vector<uint8_t> plain = minimal_plain_mus();
    write_bytes(root / "plainmus.bin", plain);
    write_bytes(root / "wrappedmus.bin", mus_scr_fixture::scr_wrapped(plain, 1));
    write_bytes(root / "headerless.bin", headerless_mus_ciphertext(plain));
    write_bytes(root / "junk.bin", std::vector<uint8_t>(2000, 0x5A));

    opennova::ResourceIndex index;
    TEST_EXPECT(index.scan(root.string()));

    const std::vector<opennova::ResourceFileEntry> music = index.resource_files("music_script");
    TEST_EXPECT(has(music, "plainmus.bin"));
    TEST_EXPECT(has(music, "wrappedmus.bin"));
    TEST_EXPECT(!has(music, "headerless.bin"));
    TEST_EXPECT(!has(music, "junk.bin"));

    return 0;
}
