// BAD writer round-trip test.
//   1. parse BINOC.bad -> bad_write -> parse again -> structural equality
//   2. self byte-stability: write(orig) == write(reparsed)
//   3. synthetic translations (flags&2) round-trip (BINOC has none)
#include "bad/bad.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "common/test_expect.h"
#include "common/test_paths.h"

static bool f_eq(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

static bool write_buffer_to_file(const char *path, const uint8_t *data, size_t size) {
    FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    size_t w = (size > 0) ? std::fwrite(data, 1, size, f) : 0;
    std::fclose(f);
    return w == size;
}

// Structural equality of two parsed BadFiles. trans_rows limits the translation
// comparison to the rows the parser actually reads (bone_count * frame_count).
static int compare_badfiles(const BadFile &a, const BadFile &b) {
    TEST_EXPECT(a.version == b.version);
    TEST_EXPECT(a.fps == b.fps);
    TEST_EXPECT(a.frame_count == b.frame_count);
    TEST_EXPECT(a.flags == b.flags);
    TEST_EXPECT(a.bone_count == b.bone_count);
    TEST_EXPECT(a.num_bones == b.num_bones);
    TEST_EXPECT(a.num_channels == b.num_channels);
    TEST_EXPECT(a.num_events == b.num_events);
    TEST_EXPECT(a.num_translations == b.num_translations);

    for (size_t i = 0; i < a.num_bones; ++i) {
        TEST_EXPECT(std::strcmp(a.bones[i].name, b.bones[i].name) == 0);
        TEST_EXPECT(a.bones[i].parent_index == b.bones[i].parent_index);
        TEST_EXPECT(f_eq(a.bones[i].length, b.bones[i].length));
        for (int k = 0; k < 3; ++k) TEST_EXPECT(f_eq(a.bones[i].position[k], b.bones[i].position[k]));
        for (int k = 0; k < 9; ++k) TEST_EXPECT(f_eq(a.bones[i].rotation[k], b.bones[i].rotation[k]));
    }

    for (size_t i = 0; i < a.num_channels; ++i) {
        TEST_EXPECT(a.channels[i].frame_count == b.channels[i].frame_count);
        for (uint32_t j = 0; j < a.channels[i].frame_count; ++j) {
            TEST_EXPECT(a.channels[i].frame_lengths[j] == b.channels[i].frame_lengths[j]);
            TEST_EXPECT(f_eq(a.channels[i].rotations[j].x, b.channels[i].rotations[j].x));
            TEST_EXPECT(f_eq(a.channels[i].rotations[j].y, b.channels[i].rotations[j].y));
            TEST_EXPECT(f_eq(a.channels[i].rotations[j].z, b.channels[i].rotations[j].z));
            TEST_EXPECT(f_eq(a.channels[i].rotations[j].w, b.channels[i].rotations[j].w));
        }
    }

    for (size_t i = 0; i < a.num_events; ++i) {
        for (int k = 0; k < 3; ++k) TEST_EXPECT(f_eq(a.events[i].velocity[k], b.events[i].velocity[k]));
        TEST_EXPECT(f_eq(a.events[i].bottom, b.events[i].bottom));
        TEST_EXPECT(f_eq(a.events[i].top, b.events[i].top));
        TEST_EXPECT(a.events[i].trigger == b.events[i].trigger);
    }

    for (size_t i = 0; i < a.num_translations; ++i) {
        for (int k = 0; k < 3; ++k) TEST_EXPECT(f_eq(a.translations[i][k], b.translations[i][k]));
    }
    return 0;
}

static int test_binoc_roundtrip() {
    char fixture[4096];
    std::snprintf(fixture, sizeof(fixture), "%s%cfixtures%cbad%cBINOC.bad",
                  test_paths_repo_root(__FILE__), TEST_PATHS_SEP, TEST_PATHS_SEP, TEST_PATHS_SEP);

    char out_path[4096];
    std::snprintf(out_path, sizeof(out_path), "%s%cbad_roundtrip_binoc.bad",
                  test_paths_temp_dir(), TEST_PATHS_SEP);

    BadFile orig = {};
    TEST_EXPECT(bad_parse(fixture, &orig) == 0);

    // write(orig) -> bufA
    uint8_t *bufA = nullptr;
    size_t sizeA = 0;
    TEST_EXPECT(bad_write_buffer(&orig, nullptr, &bufA, &sizeA) == 0);
    TEST_EXPECT(bufA != nullptr && sizeA >= 80);
    TEST_EXPECT(write_buffer_to_file(out_path, bufA, sizeA));

    // parse the written file
    BadFile rt = {};
    TEST_EXPECT(bad_parse(out_path, &rt) == 0);
    TEST_EXPECT(compare_badfiles(orig, rt) == 0);

    // self byte-stability: write(rt) must equal write(orig)
    uint8_t *bufB = nullptr;
    size_t sizeB = 0;
    TEST_EXPECT(bad_write_buffer(&rt, nullptr, &bufB, &sizeB) == 0);
    TEST_EXPECT(sizeB == sizeA);
    TEST_EXPECT(std::memcmp(bufA, bufB, sizeA) == 0);

    // rotation block must be 4-byte aligned (frame_lengths padded).
    for (size_t i = 0; i < rt.num_channels; ++i) {
        TEST_EXPECT(rt.channels[i].rotations_offset % 4 == 0);
    }

    bad_write_buffer_free(bufA);
    bad_write_buffer_free(bufB);
    bad_free(&orig);
    bad_free(&rt);
    std::remove(out_path);
    return 0;
}

static int test_synthetic_translations() {
    // Two bones, two frames (channels carry the +1 terminal duplicate), flags&2.
    BadBone bones[2] = {};
    std::strcpy(bones[0].name, "BN01");
    bones[0].parent_index = -1;
    bones[0].length = 1.0f;
    bones[0].position[0] = 0.0f; bones[0].position[1] = 0.0f; bones[0].position[2] = 0.0f;
    bones[0].rotation[0] = 1.0f; bones[0].rotation[4] = 1.0f; bones[0].rotation[8] = 1.0f;
    std::strcpy(bones[1].name, "BN02");
    bones[1].parent_index = 0;
    bones[1].length = 0.5f;
    bones[1].position[0] = 1.0f; bones[1].position[1] = 2.0f; bones[1].position[2] = 3.0f;
    bones[1].rotation[0] = 1.0f; bones[1].rotation[4] = 1.0f; bones[1].rotation[8] = 1.0f;

    uint16_t fl0[3] = {1, 1, 1};
    uint16_t fl1[3] = {1, 1, 1};
    BadQuaternion rot0[3] = {{0, 0, 0, 1}, {0.1f, 0.2f, 0.3f, 0.9f}, {0.1f, 0.2f, 0.3f, 0.9f}};
    BadQuaternion rot1[3] = {{0, 0, 0, 1}, {0.4f, 0.0f, 0.0f, 0.9f}, {0.4f, 0.0f, 0.0f, 0.9f}};
    BadChannel chans[2] = {};
    chans[0].frame_count = 3; chans[0].frame_lengths = fl0; chans[0].rotations = rot0;
    chans[1].frame_count = 3; chans[1].frame_lengths = fl1; chans[1].rotations = rot1;

    // Translations: frame-major [frame][bone], bone_count*frame_count = 2*2 = 4.
    float trans[4][3] = {
        {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f},  // frame 0: bone0, bone1
        {1.5f, -2.5f, 0.25f}, {-3.0f, 4.0f, 5.0f},  // frame 1: bone0, bone1
    };

    BadFile bf = {};
    bf.version = 1;
    bf.header_size = 80;
    bf.fps = 30;
    bf.frame_count = 2;
    bf.flags = 0x02;  // translation bit
    bf.bone_count = 2;
    bf.bones = bones;
    bf.num_bones = 2;
    bf.channels = chans;
    bf.num_channels = 2;
    bf.translations = trans;
    bf.num_translations = 4;

    char out_path[4096];
    std::snprintf(out_path, sizeof(out_path), "%s%cbad_roundtrip_synth.bad",
                  test_paths_temp_dir(), TEST_PATHS_SEP);
    TEST_EXPECT(bad_write(out_path, &bf, nullptr) == 0);

    BadFile rt = {};
    TEST_EXPECT(bad_parse(out_path, &rt) == 0);

    TEST_EXPECT(rt.flags == 0x02);
    TEST_EXPECT(rt.frame_count == 2);
    TEST_EXPECT(rt.bone_count == 2);
    TEST_EXPECT(rt.num_translations == 4);
    TEST_EXPECT(std::strcmp(rt.bones[0].name, "BN01") == 0);
    TEST_EXPECT(rt.bones[0].parent_index == -1);
    TEST_EXPECT(rt.bones[1].parent_index == 0);
    TEST_EXPECT(f_eq(rt.bones[1].position[0], 1.0f));
    TEST_EXPECT(f_eq(rt.bones[1].position[2], 3.0f));
    for (size_t i = 0; i < 4; ++i) {
        for (int k = 0; k < 3; ++k) TEST_EXPECT(f_eq(rt.translations[i][k], trans[i][k]));
    }
    // channel rotations preserved
    TEST_EXPECT(rt.channels[0].frame_count == 3);
    TEST_EXPECT(f_eq(rt.channels[0].rotations[1].x, 0.1f));
    TEST_EXPECT(f_eq(rt.channels[1].rotations[1].x, 0.4f));

    bad_free(&rt);
    std::remove(out_path);
    return 0;
}

int main() {
    if (test_binoc_roundtrip() != 0) return 1;
    if (test_synthetic_translations() != 0) return 1;
    return 0;
}
