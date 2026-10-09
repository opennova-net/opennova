#include <formats/scr/scr.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common/test_expect.h"

using namespace opennova::scr;

int main() {
    static const uint8_t encrypted[] = {
        0x53, 0x43, 0x52, 0x00, 0xbc, 0xe1, 0xc6, 0x36,
        0xeb, 0x54, 0x4c, 0x16, 0x43, 0x72, 0x55, 0xc4,
        0x09, 0x0f, 0xc4, 0x25, 0x2e, 0x9c,
    };
    static const uint8_t expected[] = "OpenNova SCR test\n";

    TEST_EXPECT(scr_is_scr(encrypted, sizeof(encrypted)) == 1);
    TEST_EXPECT(scr_get_version(encrypted, sizeof(encrypted)) == 0);
    TEST_EXPECT(scr_is_scr(encrypted, 3) == 0);
    TEST_EXPECT(scr_get_version(encrypted, 3) == 0);

    static const uint8_t scr0_plain[] = {
        'S', 'C', 'R', '0', 'm', 'u', 's', 'i', 'c'
    };
    TEST_EXPECT(scr_is_scr(scr0_plain, sizeof(scr0_plain)) == 0);
    TEST_EXPECT(scr_get_version(scr0_plain, sizeof(scr0_plain)) == 0);

    uint8_t tiny[4] = {};
    size_t tiny_size = sizeof(tiny);
    TEST_EXPECT(scr_decrypt_buf(encrypted, sizeof(encrypted), tiny, &tiny_size, SCR_KEY_DEFAULT) == -2);
    TEST_EXPECT(tiny_size == sizeof(expected) - 1);

    uint8_t out[64] = {};
    size_t out_size = sizeof(out);
    TEST_EXPECT(scr_decrypt_buf(encrypted, sizeof(encrypted), out, &out_size, SCR_KEY_DEFAULT) == 0);
    TEST_EXPECT(out_size == sizeof(expected) - 1);
    TEST_EXPECT(std::memcmp(out, expected, out_size) == 0);

    static const uint8_t invalid[] = {'N', 'O', 'P', 'E'};
    out_size = sizeof(out);
    TEST_EXPECT(scr_decrypt_buf(invalid, sizeof(invalid), out, &out_size, SCR_KEY_DEFAULT) == -1);

    out_size = sizeof(out);
    TEST_EXPECT(scr_decrypt_buf(scr0_plain, sizeof(scr0_plain), out, &out_size, SCR_KEY_DEFAULT) == -1);

    // The shader loader's form [orig: ScriptFile_LoadAndDecrypt @ 0x5AE060]: "SCR", version 1, the text
    // and a NUL under the shaders' key; read back with the NUL dropped, and byte for byte the payload a
    // hand-made file holds.
    const std::string source = "// a shader\r\nfloat4 main() : COLOR { return 0; }\r\n";
    std::string payload = source + std::string(1, '\0');
    scr_encrypt(reinterpret_cast<uint8_t *>(&payload[0]), payload.size(), SCR_KEY_SHADERS);
    const std::string by_hand = std::string("SCR\x01", 4) + payload;
    const std::vector<uint8_t> shader = scr_shader_encode(source);
    TEST_EXPECT(std::string(shader.begin(), shader.end()) == by_hand);
    std::string text;
    bool nul = false;
    TEST_EXPECT(scr_shader_decode(shader.data(), shader.size(), text, &nul) && text == source && nul);
    // Without the NUL: written so, and read back so.
    const std::vector<uint8_t> bare = scr_shader_encode(source, false);
    TEST_EXPECT(bare.size() + 1 == shader.size() && scr_shader_decode(bare.data(), bare.size(), text, &nul) &&
                text == source && !nul);
    // Another version, the text readers' key, or no SCR at all: not the shader loader's form.
    std::vector<uint8_t> two = shader;
    two[3] = 2;
    TEST_EXPECT(!scr_shader_decode(two.data(), two.size(), text) && !scr_shader_decode(encrypted, sizeof(encrypted), text));
    TEST_EXPECT(!scr_shader_decode(reinterpret_cast<const uint8_t *>(source.data()), source.size(), text));
    TEST_EXPECT(!scr_shader_decode(scr0_plain, sizeof(scr0_plain), text));
    TEST_EXPECT(SCR_SHADER_VERSION == 1);

    return 0;
}
