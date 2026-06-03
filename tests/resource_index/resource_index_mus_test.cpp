/* resource_index detection of encrypted-on-disk MUS (regression for the
   index<->loader consistency fix). An encrypted MUS .bin has no SCR0 magic in
   its raw bytes, so the old magic-only classifier hid it from quick-open even
   though the loader could open it. We synthesize the headerless ciphertext from
   a minimal valid MUS via scr_encrypt_mus and assert the index classifies it as
   music_script. A junk .bin must NOT be misclassified. No external assets. */

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "common/test_expect.h"
#include "resource_index/resource_index.h"
#include "scr/scr.h"

namespace fs = std::filesystem;

static void write_bytes(const fs::path &path, const std::vector<uint8_t> &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

static bool has(const std::vector<opennova::ResourceFileEntry> &entries, const std::string &rel) {
    return std::any_of(entries.begin(), entries.end(),
        [&](const opennova::ResourceFileEntry &e) { return e.relative_path == rel; });
}

int main() {
    const fs::path root = fs::temp_directory_path() / "opennova_resource_index_mus_test";
    fs::remove_all(root);
    fs::create_directories(root);

    // Minimal valid plaintext MUS (SCR0 + version 0x00000100 + chunk_count 1),
    // padded past the 44-byte header so peek_valid passes.
    std::vector<uint8_t> plain(48, 0);
    plain[0] = 'S'; plain[1] = 'C'; plain[2] = 'R'; plain[3] = '0';
    plain[5] = 0x01;   // version 0x00000100 (LE)
    plain[8] = 0x01;   // chunk_count = 1

    // Encrypted headerless form on disk.
    uint8_t *enc = nullptr;
    size_t enc_size = 0;
    TEST_EXPECT(scr_encrypt_mus(plain.data(), plain.size(), &enc, &enc_size, SCR_KEY_JO_DFX2) == 0);
    write_bytes(root / "encmus.bin", std::vector<uint8_t>(enc, enc + enc_size));
    scr_free_buffer(enc);

    // A plaintext MUS (already SCR0) and a non-MUS .bin for contrast.
    write_bytes(root / "plainmus.bin", plain);
    write_bytes(root / "junk.bin", std::vector<uint8_t>(2000, 0x5A));

    opennova::ResourceIndex index;
    TEST_EXPECT(index.scan(root.string()));

    const std::vector<opennova::ResourceFileEntry> music = index.resource_files("music_script");
    TEST_EXPECT(has(music, "encmus.bin"));     // encrypted-on-disk MUS now detected
    TEST_EXPECT(has(music, "plainmus.bin"));   // plaintext MUS still detected
    TEST_EXPECT(!has(music, "junk.bin"));      // non-MUS .bin not misclassified

    return 0;
}
