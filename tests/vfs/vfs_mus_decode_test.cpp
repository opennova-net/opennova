/* vfs_decode_payload generic payload handling around SCR0.

   MUS .bin files in JO_CLIENT/localres.pff are already plaintext SCR0 payloads.
   SCR0 is not an encrypted SCR container and must pass through unchanged. The
   headerless MUS cipher experimented with in PR #53 is intentionally outside
   VFS auto-decode; without an explicit SCR/PFF marker it is just opaque bytes. */

#include <base/vfs/vfs_decode.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#include "common/mus_scr_fixture.h"
#include "common/test_expect.h"

namespace {

using mus_scr_fixture::headerless_mus_ciphertext;
using mus_scr_fixture::minimal_plain_mus;

} // namespace

int main() {
    const std::vector<uint8_t> plain = minimal_plain_mus();

    std::vector<uint8_t> data = plain;
    TEST_EXPECT(opennova::vfs_decode_payload(data));
    TEST_EXPECT(data == plain);

    std::vector<uint8_t> scr0_marker = {'S', 'C', 'R', '0', 'n', 'o', 't', '-', 'm', 'u', 's'};
    const std::vector<uint8_t> scr0_original = scr0_marker;
    TEST_EXPECT(opennova::vfs_decode_payload(scr0_marker));
    TEST_EXPECT(scr0_marker == scr0_original);

    std::vector<uint8_t> headerless = headerless_mus_ciphertext(plain);
    const std::vector<uint8_t> headerless_original = headerless;
    TEST_EXPECT(opennova::vfs_decode_payload(headerless));
    TEST_EXPECT(headerless == headerless_original);

    const std::vector<uint8_t> arbitrary_plain = {'I', 'T', 'E', 'M', 'S', '.', 'D', 'E', 'F', '\n'};
    std::vector<uint8_t> wrapped = mus_scr_fixture::scr_wrapped(arbitrary_plain, 0);
    TEST_EXPECT(opennova::vfs_decode_payload(wrapped));
    TEST_EXPECT(wrapped == arbitrary_plain);

    std::printf("vfs_mus_decode_test OK\n");
    return 0;
}
