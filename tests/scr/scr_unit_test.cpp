#include "scr/scr.h"

#include <cassert>
#include <cstdint>
#include <cstring>

int main() {
    static const uint8_t encrypted[] = {
        0x53, 0x43, 0x52, 0x00, 0xbc, 0xe1, 0xc6, 0x36,
        0xeb, 0x54, 0x4c, 0x16, 0x43, 0x72, 0x55, 0xc4,
        0x09, 0x0f, 0xc4, 0x25, 0x2e, 0x9c,
    };
    static const uint8_t expected[] = "OpenNova SCR test\n";

    assert(scr_is_scr(encrypted, sizeof(encrypted)) == 1);
    assert(scr_get_version(encrypted, sizeof(encrypted)) == 0);
    assert(scr_is_scr(encrypted, 3) == 0);
    assert(scr_get_version(encrypted, 3) == 0);

    uint8_t tiny[4] = {};
    size_t tiny_size = sizeof(tiny);
    assert(scr_decrypt_buf(encrypted, sizeof(encrypted), tiny, &tiny_size, SCR_KEY_DEFAULT) == -2);
    assert(tiny_size == sizeof(expected) - 1);

    uint8_t out[64] = {};
    size_t out_size = sizeof(out);
    assert(scr_decrypt_buf(encrypted, sizeof(encrypted), out, &out_size, SCR_KEY_DEFAULT) == 0);
    assert(out_size == sizeof(expected) - 1);
    assert(std::memcmp(out, expected, out_size) == 0);

    static const uint8_t invalid[] = {'N', 'O', 'P', 'E'};
    out_size = sizeof(out);
    assert(scr_decrypt_buf(invalid, sizeof(invalid), out, &out_size, SCR_KEY_DEFAULT) == -1);

    return 0;
}
