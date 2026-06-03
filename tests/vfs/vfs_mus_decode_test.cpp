/* vfs_decode_payload MUS handling (regression for the headerless-MUS extraction
   bug). Synthesizes the headerless on-disk ciphertext from the committed
   plaintext fixture via scr_encrypt_mus, then checks that vfs_decode_payload:
     1. decodes the headerless-encrypted form back to plaintext SCR0, and
     2. leaves a plaintext "SCR0" MUS untouched (the SCR0/"SCR" magic collision
        that would otherwise corrupt it via the generic SCR-container path).
   No external assets. */

#include "vfs/vfs_decode.h"
#include "scr/scr.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/test_expect.h"

#ifndef MUS_FIXTURE_DIR
#define MUS_FIXTURE_DIR "fixtures/mus"
#endif

static std::vector<uint8_t> slurp(const char *path) {
    std::vector<uint8_t> v;
    FILE *f = fopen(path, "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    v.resize((size_t)n);
    if (n > 0 && fread(v.data(), 1, (size_t)n, f) != (size_t)n) v.clear();
    fclose(f);
    return v;
}

int main() {
    const std::vector<uint8_t> plain = slurp(MUS_FIXTURE_DIR "/jo_gamemus.bin");
    TEST_EXPECT(!plain.empty());
    TEST_EXPECT(plain.size() >= 4 && plain[0] == 'S' && plain[1] == 'C' && plain[2] == 'R' && plain[3] == '0');

    // Headerless-encrypted form -> vfs_decode_payload must yield plaintext SCR0.
    {
        uint8_t *enc = nullptr;
        size_t enc_size = 0;
        TEST_EXPECT(scr_encrypt_mus(plain.data(), plain.size(), &enc, &enc_size, SCR_KEY_JO_DFX2) == 0);
        std::vector<uint8_t> data(enc, enc + enc_size);
        scr_free_buffer(enc);
        TEST_EXPECT(data.size() < 4 || data[0] != 'S' || data[3] != '0');  // really encrypted

        TEST_EXPECT(opennova::vfs_decode_payload(data));
        TEST_EXPECT(data.size() == plain.size());
        TEST_EXPECT(std::memcmp(data.data(), plain.data(), plain.size()) == 0);
    }

    // Plaintext SCR0 MUS must pass through unchanged (must NOT be mangled by the
    // generic SCR-container decode, whose magic check also matches "SCR0").
    {
        std::vector<uint8_t> data = plain;
        TEST_EXPECT(opennova::vfs_decode_payload(data));
        TEST_EXPECT(data == plain);
    }

    std::printf("vfs_mus_decode_test OK (%zu B fixture)\n", plain.size());
    return 0;
}
