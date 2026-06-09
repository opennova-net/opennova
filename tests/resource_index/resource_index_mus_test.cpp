/* resource_index MUS .bin classification.

   ResourceIndex should classify the bytes users can actually open through the
   generic VFS payload decoder: plaintext SCR0 and explicit SCR-wrapped SCR0.
   Headerless MUS ciphertext is intentionally not auto-detected. */

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "common/test_expect.h"
#include "resource_index/resource_index.h"

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kScrKeyJoDfx2 = 0x2A5A8EADu;

uint32_t rol32(uint32_t value, int shift) {
    return (value << shift) | (value >> (32 - shift));
}

void xor_scr_keystream(std::vector<uint8_t> &bytes, uint32_t key) {
    for (uint8_t &byte : bytes) {
        key = rol32(key + rol32(key, 11), 4) ^ 1u;
        byte ^= static_cast<uint8_t>(key);
    }
}

std::vector<uint8_t> minimal_plain_mus() {
    std::vector<uint8_t> plain(48, 0);
    plain[0] = 'S';
    plain[1] = 'C';
    plain[2] = 'R';
    plain[3] = '0';
    plain[5] = 0x01; // version 0x00000100
    plain[8] = 0x01; // chunk_count = 1
    return plain;
}

std::vector<uint8_t> scr_wrapped(const std::vector<uint8_t> &plain) {
    std::vector<uint8_t> payload = plain;
    xor_scr_keystream(payload, kScrKeyJoDfx2);
    std::vector<uint8_t> encrypted(payload.rbegin(), payload.rend());

    std::vector<uint8_t> out = {'S', 'C', 'R', 0x01};
    out.insert(out.end(), encrypted.begin(), encrypted.end());
    return out;
}

std::vector<uint8_t> headerless_mus_ciphertext(const std::vector<uint8_t> &plain) {
    std::vector<uint8_t> payload;
    if (plain.size() > 4) {
        payload.assign(plain.begin() + 4, plain.end());
    }
    std::vector<uint8_t> out(payload.rbegin(), payload.rend());
    xor_scr_keystream(out, kScrKeyJoDfx2);
    return out;
}

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
    const fs::path root = fs::temp_directory_path() / "opennova_resource_index_mus_test";
    fs::remove_all(root);
    fs::create_directories(root);

    const std::vector<uint8_t> plain = minimal_plain_mus();
    write_bytes(root / "plainmus.bin", plain);
    write_bytes(root / "wrappedmus.bin", scr_wrapped(plain));
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
