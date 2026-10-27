#include <formats/bad/bad.h>

#include <cstdio>
#include <cstring>

#include <cstdint>
#include <string>
#include <vector>

#include "common/bad_equal.h"
#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

using namespace opennova::bad;

// The null guard, then the layout pins on the minted clips
// (tests/fixtures/minimal_bad_gen.cpp): fixtures/anim/rig19_idle.bad carries
// the stock 19-bone idle's shape (the shipped BINOC.bad's: the pins below
// held on it), fixtures/anim/vm40_rst.bad the translated 40-bone reset's
// (357_RST.bad's).
int main() {
    BadFile invalid = {};
    TEST_EXPECT(bad_parse(nullptr, &invalid) == -1);

    const std::string fixture = test_paths_fixture("anim/rig19_idle.bad");
    BadFile file = {};
    TEST_EXPECT(bad_parse(fixture.c_str(), &file) == 0);

    TEST_EXPECT(file.version == 1);
    TEST_EXPECT(file.header_size == 80);
    TEST_EXPECT(file.fps == 30);
    TEST_EXPECT(file.frame_count == 3);
    TEST_EXPECT(file.flags == 1);
    TEST_EXPECT(file.bone_count == 19);

    TEST_EXPECT(file.num_bones == 19);
    TEST_EXPECT(file.bones != nullptr);
    TEST_EXPECT(std::strcmp(file.bones[0].name, "BN01 Hips") == 0);
    TEST_EXPECT(file.bones[0].parent_index == -1);

    TEST_EXPECT(file.num_channels == 19);
    TEST_EXPECT(file.channels != nullptr);
    TEST_EXPECT(file.channels[0].frame_count == 4);
    TEST_EXPECT(file.channels[0].frame_lengths != nullptr);
    TEST_EXPECT(file.channels[0].rotations != nullptr);

    TEST_EXPECT(file.num_events == 4);
    TEST_EXPECT(file.events != nullptr);
    TEST_EXPECT(file.num_translations == 0);
    TEST_EXPECT(file.translations == nullptr);

    bad_free(&file);
    TEST_EXPECT(file.bones == nullptr);
    TEST_EXPECT(file.channels == nullptr);
    TEST_EXPECT(file.events == nullptr);

    std::printf("rig19_idle.bad parsed with the stock idle's pins\n");

    // A translated clip carries a row per frame and the fence-post row
    // frame_count, which the runtime's lerp reaches on the last interval
    // [orig: AnimChannel_GetTranslationRowAtTime @0x4102d0 via BoneAnim_TransformBones @0x410360].
    // vm40_rst.bad: 40 bones, one frame, rows 0 and 1 (and the pad row past them).
    const std::string translated = test_paths_fixture("anim/vm40_rst.bad");
    BadFile rst = {};
    TEST_EXPECT(bad_parse(translated.c_str(), &rst) == 0);
    TEST_EXPECT(rst.frame_count == 1 && rst.bone_count == 40 && (rst.flags & 2u) != 0);
    TEST_EXPECT(rst.num_translations == 80);
    bad_free(&rst);
    std::printf("vm40_rst.bad carries rows 0..frame_count\n");

    // The rows are the game's fixed sizes, whatever the header words +0x2C / +0x30 / +0x34
    // hold, and a channel's key count is its row's first 16-bit word [orig: BoneFile_Load
    // @0x40FFF0 - `add edi, 64h` @0x4100FB; BoneAnim_FindKeyframeAtTime @0x410220 - @0x41024F,
    // @0x410284]: the minted idle with those words zeroed and smashed, and garbage over the
    // high half of each channel's count word, parses field for field as it stands.
    {
        std::vector<uint8_t> bytes;
        TEST_EXPECT(test_io::read_file(fixture, bytes) && bytes.size() > 0x50);
        BadFile stock = {};
        TEST_EXPECT(bad_parse_buffer(bytes.data(), bytes.size(), &stock) == 0);
        std::vector<uint8_t> smashed = bytes;
        const auto put_u32 = [&smashed](size_t at, uint32_t v) {
            for (int k = 0; k < 4; ++k) smashed[at + k] = static_cast<uint8_t>(v >> (8 * k));
        };
        put_u32(0x2C, 1);
        put_u32(0x30, 0);
        put_u32(0x34, 3);
        const uint32_t channels_offset = static_cast<uint32_t>(bytes[0x1C]) |
                (static_cast<uint32_t>(bytes[0x1D]) << 8) |
                (static_cast<uint32_t>(bytes[0x1E]) << 16) |
                (static_cast<uint32_t>(bytes[0x1F]) << 24);
        for (uint32_t c = 0; c < stock.bone_count; ++c) {
            smashed[channels_offset + c * kBadChannelRowBytes + 2] = 0xCD;
            smashed[channels_offset + c * kBadChannelRowBytes + 3] = 0xAB;
        }
        BadFile read_back = {};
        TEST_EXPECT(bad_parse_buffer(smashed.data(), smashed.size(), &read_back) == 0);
        TEST_EXPECT(bad_equal::field_equal(stock, read_back));
        bad_free(&read_back);
        bad_free(&stock);
    }

    // A header that names one-byte bone rows must not let a 100-byte row run past the
    // buffer: one bone at 0x5F of a 0x60-byte file does not fit, so the clip is refused
    // (the old stride read passed in_range(0x5F, 1) and read ~99 bytes past the end).
    {
        std::vector<uint8_t> tiny(0x60, 0);
        const auto put_u32 = [&tiny](size_t at, uint32_t v) {
            for (int k = 0; k < 4; ++k) tiny[at + k] = static_cast<uint8_t>(v >> (8 * k));
        };
        put_u32(0x00, 1);    // version
        put_u32(0x04, 0x50); // header size
        put_u32(0x14, 1);    // bone count
        put_u32(0x18, 0x5F); // bone table
        put_u32(0x1C, 0x50); // channel table (one empty channel)
        put_u32(0x2C, 1);    // a bone "stride" the game never reads
        BadFile refused = {};
        TEST_EXPECT(bad_parse_buffer(tiny.data(), tiny.size(), &refused) == -1);
        TEST_EXPECT(refused.bones == nullptr && refused.channels == nullptr);
    }
    std::printf("the fixed rows ignore the header's stride words\n");
    return 0;
}
