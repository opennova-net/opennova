/* MUS-style SCR cipher (no leading SCR magic) parity tests.
 *
 * Verifies scr_decrypt_mus() against:
 *   1. fixtures/mus/jo_gamemus.bin (post-decrypt golden, 2521 B = 4 B "SCR0"
 *      magic + 2517 B payload).
 *   2. C:/Users/taylor/Desktop/JO_ASSETS_t/gamemus.bin (retail-disk encrypted
 *      input, 2517 B). Skipped if absent so CI without the asset still passes.
 */
#include "scr/scr.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/test_expect.h"

#ifndef MUS_FIXTURE_DIR
#define MUS_FIXTURE_DIR "fixtures/mus"
#endif

#ifndef RETAIL_GAMEMUS_PATH
#define RETAIL_GAMEMUS_PATH "C:/Users/taylor/Desktop/JO_ASSETS_t/gamemus.bin"
#endif

static uint8_t *slurp(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return nullptr; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return nullptr; }
    fclose(f);
    *out_size = (size_t)n;
    return buf;
}

int main() {
    // Synthetic round-trip: encrypt then decrypt. Verifies the cipher
    // implementation independently of any asset on disk and exercises
    // memory ownership.
    {
        // Build a small "encrypted" buffer by running the algorithm
        // in reverse: take a known plaintext "SCR0..." and produce
        // ciphertext. The forward direction is reverse(plaintext_payload)
        // XOR keystream. We feed that ciphertext back into scr_decrypt_mus
        // and expect the original "SCR0..." back.
        const uint8_t plain[] = {
            'S', 'C', 'R', '0',
            'h', 'e', 'l', 'l', 'o', ' ', 'm', 'u', 's', '!', 0x00, 0x01,
        };
        const size_t plain_size = sizeof(plain);
        const size_t payload_size = plain_size - 4;

        // Reverse the payload, then XOR with keystream(KEY, payload_size).
        // The keystream advances forward starting from `key` exactly as
        // scr_decrypt_mus does.
        uint8_t encrypted[16];
        for (size_t i = 0; i < payload_size; ++i) {
            encrypted[i] = plain[4 + (payload_size - 1 - i)];
        }
        // Apply the same forward keystream that scr_decrypt_mus will:
        // k = rol32(k + rol32(k,11), 4) ^ 1; out[i] = in[i] ^ (k & 0xff)
        uint32_t k = 0xDEADBEEFu;
        for (size_t i = 0; i < payload_size; ++i) {
            k = (k + ((k << 11) | (k >> 21))) & 0xFFFFFFFFu;
            k = ((k << 4) | (k >> 28)) & 0xFFFFFFFFu;
            k ^= 1u;
            encrypted[i] ^= (uint8_t)k;
        }

        uint8_t *out = nullptr;
        size_t out_size = 0;
        TEST_EXPECT(scr_decrypt_mus(encrypted, payload_size, &out, &out_size, 0xDEADBEEFu) == 0);
        TEST_EXPECT(out != nullptr);
        TEST_EXPECT(out_size == plain_size);
        TEST_EXPECT(std::memcmp(out, plain, plain_size) == 0);
        scr_free_buffer(out);
    }

    // Empty input: just the SCR0 magic, no payload.
    {
        uint8_t *out = nullptr;
        size_t out_size = 0;
        TEST_EXPECT(scr_decrypt_mus(nullptr, 0, &out, &out_size, SCR_KEY_JO_DFX2) == 0);
        TEST_EXPECT(out_size == 4);
        TEST_EXPECT(out[0] == 'S' && out[1] == 'C' && out[2] == 'R' && out[3] == '0');
        scr_free_buffer(out);
    }

    // NULL output args rejected.
    {
        uint8_t dummy = 0;
        size_t dummy_size = 0;
        uint8_t *out = nullptr;
        TEST_EXPECT(scr_decrypt_mus(&dummy, 1, nullptr, &dummy_size, 0) == -1);
        TEST_EXPECT(scr_decrypt_mus(&dummy, 1, &out, nullptr, 0) == -1);
    }

    // scr_free_buffer NULL safety.
    scr_free_buffer(nullptr);

    // Golden parity: decrypt retail gamemus.bin and compare against the
    // committed golden. Both paths are required for the strict parity test.
    size_t enc_size = 0;
    uint8_t *enc = slurp(RETAIL_GAMEMUS_PATH, &enc_size);
    size_t gold_size = 0;
    uint8_t *gold = slurp(MUS_FIXTURE_DIR "/jo_gamemus.bin", &gold_size);

    if (enc == nullptr) {
        std::printf("SKIP: retail gamemus.bin not at %s; cipher round-trip + empty checks still ran.\n",
                    RETAIL_GAMEMUS_PATH);
        free(gold);
        return 0;
    }
    TEST_EXPECT(gold != nullptr);
    std::printf("Retail size: %zu, golden size: %zu\n", enc_size, gold_size);
    TEST_EXPECT(gold_size == enc_size + 4);

    uint8_t *out = nullptr;
    size_t out_size = 0;
    TEST_EXPECT(scr_decrypt_mus(enc, enc_size, &out, &out_size, SCR_KEY_JO_DFX2) == 0);
    TEST_EXPECT(out != nullptr);
    TEST_EXPECT(out_size == gold_size);
    TEST_EXPECT(out[0] == 'S' && out[1] == 'C' && out[2] == 'R' && out[3] == '0');
    if (std::memcmp(out, gold, gold_size) != 0) {
        // Find first divergence for diagnostics.
        for (size_t i = 0; i < gold_size; ++i) {
            if (out[i] != gold[i]) {
                std::fprintf(stderr,
                             "byte %zu: decrypted=0x%02x golden=0x%02x\n",
                             i, out[i], gold[i]);
                break;
            }
        }
    }
    TEST_EXPECT(std::memcmp(out, gold, gold_size) == 0);

    scr_free_buffer(out);
    free(enc);
    free(gold);
    return 0;
}
