// The .bad writer against the reader: our own fixtures write back byte-exact,
// a from-scratch file survives write -> parse field-for-field and re-writes
// to the same bytes, and the shipped BINOC.bad (OPENNOVA_JO_ASSETS, SKIP-LEG)
// parses -> writes -> parses field-equal.
#include <formats/bad/bad.h>
#include <formats/bad/bad_write.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

using namespace opennova::bad;

namespace {

bool same_float(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

bool field_equal(const BadFile &a, const BadFile &b) {
    if (a.version != b.version || a.header_size != b.header_size || a.fps != b.fps ||
        a.frame_count != b.frame_count || a.flags != b.flags || a.bone_count != b.bone_count ||
        a.num_bones != b.num_bones || a.num_channels != b.num_channels || a.num_events != b.num_events ||
        a.num_translations != b.num_translations) {
        return false;
    }
    for (size_t i = 0; i < a.num_bones; ++i) {
        const BadBone &x = a.bones[i];
        const BadBone &y = b.bones[i];
        if (std::strcmp(x.name, y.name) != 0 || x.parent_index != y.parent_index ||
            x.num_children != y.num_children || !same_float(x.length, y.length)) {
            return false;
        }
        for (int k = 0; k < 3; ++k)
            if (!same_float(x.position[k], y.position[k])) return false;
        for (int k = 0; k < 9; ++k)
            if (!same_float(x.rotation[k], y.rotation[k])) return false;
    }
    for (size_t i = 0; i < a.num_channels; ++i) {
        const BadChannel &x = a.channels[i];
        const BadChannel &y = b.channels[i];
        if (x.frame_count != y.frame_count) return false;
        for (uint32_t k = 0; k < x.frame_count; ++k) {
            if (x.frame_lengths[k] != y.frame_lengths[k]) return false;
            if (!same_float(x.rotations[k].x, y.rotations[k].x) || !same_float(x.rotations[k].y, y.rotations[k].y) ||
                !same_float(x.rotations[k].z, y.rotations[k].z) || !same_float(x.rotations[k].w, y.rotations[k].w)) {
                return false;
            }
        }
    }
    for (size_t i = 0; i < a.num_events; ++i) {
        const BadEvent &x = a.events[i];
        const BadEvent &y = b.events[i];
        for (int k = 0; k < 3; ++k)
            if (!same_float(x.velocity[k], y.velocity[k])) return false;
        if (!same_float(x.bottom, y.bottom) || !same_float(x.top, y.top) || x.trigger != y.trigger) return false;
    }
    for (size_t i = 0; i < a.num_translations; ++i)
        for (int k = 0; k < 3; ++k)
            if (!same_float(a.translations[i][k], b.translations[i][k])) return false;
    return true;
}

// A two-bone, three-interval translated clip built from scratch.
struct Scratch {
    BadBone bones[2] = {};
    uint16_t lengths0[4] = {1, 1, 2, 1};
    uint16_t lengths1[2] = {3, 2};
    BadQuaternion rot0[4] = {{0, 0, 0, 1}, {0.1f, 0, 0, 0.995f}, {0, 0.2f, 0, 0.98f}, {0, 0, 0.3f, 0.954f}};
    BadQuaternion rot1[2] = {{0, 0, 0, 1}, {0.5f, 0.5f, 0.5f, 0.5f}};
    BadChannel channels[2] = {};
    BadEvent events[4] = {};
    // Rows 0..frame_count, frame-major: the fence-post row the runtime reads.
    float translations[8][3] = {{0, 0, 0}, {1, 2, 3}, {0.5f, 0, 0}, {1.5f, 2, 3},
                                {1, 0, 0}, {2, 2, 3}, {1.5f, 0, 0}, {2.5f, 2, 3}};
    BadFile file = {};

    Scratch() {
        std::strcpy(bones[0].name, "BN01 Hips");
        bones[0].parent_index = -1;
        bones[0].length = 0.0f;
        bones[0].position[0] = 0.0f;
        bones[0].position[1] = 1.0f;
        bones[0].position[2] = 0.0f;
        for (int k = 0; k < 9; ++k) bones[0].rotation[k] = (k % 4 == 0) ? 1.0f : 0.0f;
        std::strcpy(bones[1].name, "BN02 Lower Spine");
        bones[1].parent_index = 0;
        bones[1].length = 0.25f;
        bones[1].position[0] = -0.1f;
        bones[1].position[1] = 0.2f;
        bones[1].position[2] = 0.3f;
        for (int k = 0; k < 9; ++k) bones[1].rotation[k] = 0.1f * static_cast<float>(k);
        channels[0].frame_count = 4;
        channels[0].frame_lengths = lengths0;
        channels[0].rotations = rot0;
        channels[1].frame_count = 2;
        channels[1].frame_lengths = lengths1;
        channels[1].rotations = rot1;
        for (int i = 0; i < 4; ++i) {
            events[i].velocity[0] = 0.01f * static_cast<float>(i);
            events[i].velocity[1] = 0.0f;
            events[i].velocity[2] = 0.17f;
            events[i].bottom = 0.9f;
            events[i].top = 1.65f;
            events[i].trigger = i == 0 ? 1 : 0;
        }
        file.version = 1;
        file.header_size = 80;
        file.fps = 30;
        file.frame_count = 3;
        file.flags = 1u | 2u;
        file.bone_count = 2;
        file.bones = bones;
        file.num_bones = 2;
        file.channels = channels;
        file.num_channels = 2;
        file.events = events;
        file.num_events = 4;
        file.translations = translations;
        file.num_translations = 8;
    }
};

} // namespace

int main(int argc, char **argv) {
    // `--dump <path>` writes the retail leg's rewritten bytes for a byte diff.
    const char *dump_path = (argc >= 3 && std::strcmp(argv[1], "--dump") == 0) ? argv[2] : nullptr;
    std::vector<uint8_t> bytes;
    TEST_EXPECT(bad_write_buffer(nullptr, bytes) == -1);

    // Our own fixtures: parse -> write reproduces the committed bytes.
    const std::string root = std::string(test_paths_repo_root(__FILE__));
    for (const char *name : {"idle.bad", "walk.bad"}) {
        const std::string path = root + "/fixtures/anim/" + name;
        std::vector<uint8_t> committed;
        TEST_EXPECT(test_io::read_file(path, committed) && !committed.empty());
        BadFile parsed = {};
        TEST_EXPECT(bad_parse_buffer(committed.data(), committed.size(), &parsed) == 0);
        TEST_EXPECT(bad_write_buffer(&parsed, bytes) == 0);
        TEST_EXPECT(bytes == committed);
        bad_free(&parsed);
        std::printf("fixtures/anim/%s: byte-exact through write\n", name);
    }

    // From scratch: write -> parse is field-equal, and the parsed file writes the same bytes.
    {
        Scratch scratch;
        TEST_EXPECT(bad_write_buffer(&scratch.file, bytes) == 0);
        BadFile back = {};
        TEST_EXPECT(bad_parse_buffer(bytes.data(), bytes.size(), &back) == 0);
        TEST_EXPECT(back.num_bones == 2 && back.bones[1].parent_index == 0 && back.bones[0].parent_index == -1);
        TEST_EXPECT(back.bones[0].num_children == 1 && back.bones[1].num_children == 0);
        TEST_EXPECT(back.num_channels == 2 && back.channels[0].frame_count == 4 && back.channels[1].frame_count == 2);
        TEST_EXPECT(back.channels[0].frame_lengths[2] == 2 && same_float(back.channels[1].rotations[1].w, 0.5f));
        TEST_EXPECT(back.num_events == 4 && back.events[0].trigger == 1 && same_float(back.events[3].velocity[0], 0.03f));
        TEST_EXPECT(back.num_translations == 8 && same_float(back.translations[7][0], 2.5f));
        // The block ends on a pad row that repeats row frame_count.
        TEST_EXPECT(bytes.size() >= 24);
        for (int k = 0; k < 6; ++k) {
            float stored;
            std::memcpy(&stored, bytes.data() + bytes.size() - 24 + 4 * k, sizeof(float));
            TEST_EXPECT(same_float(stored, scratch.translations[6 + k / 3][k % 3]));
        }
        TEST_EXPECT(std::strcmp(back.bones[1].name, "BN02 Lower Spine") == 0);
        std::vector<uint8_t> again;
        TEST_EXPECT(bad_write_buffer(&back, again) == 0);
        TEST_EXPECT(again == bytes);
        bad_free(&back);
        std::printf("from-scratch clip: write -> parse field-equal, re-write byte-equal (%zu bytes)\n", bytes.size());

        // A translation block shorter than bone_count x (frame_count + 1) cannot be represented.
        scratch.file.num_translations = 7;
        TEST_EXPECT(bad_write_buffer(&scratch.file, bytes) == -1);
        scratch.file.num_translations = 8;

        // An event count with no events, and a 32-byte name with no NUL (it
        // would read back cut to 31), cannot be represented either.
        scratch.file.events = nullptr;
        TEST_EXPECT(bad_write_buffer(&scratch.file, bytes) == -1);
        scratch.file.events = scratch.events;
        std::memset(scratch.bones[1].name, 'x', 32);
        scratch.bones[1].name[32] = '\0';
        TEST_EXPECT(bad_write_buffer(&scratch.file, bytes) == -1);
    }

    // The retail leg: the shipped 19-bone BINOC.bad parses -> writes -> parses field-equal.
    const std::string fixture = retail::reference_fixture("bad/BINOC.bad");
    if (fixture.empty()) {
        return retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/bad/BINOC.bad (the shipped 19-bone rig)");
    }
    std::vector<uint8_t> retail_bytes;
    TEST_EXPECT(test_io::read_file(fixture, retail_bytes) && !retail_bytes.empty());
    BadFile retail_file = {};
    TEST_EXPECT(bad_parse_buffer(retail_bytes.data(), retail_bytes.size(), &retail_file) == 0);
    TEST_EXPECT(bad_write_buffer(&retail_file, bytes) == 0);
    BadFile rewritten = {};
    TEST_EXPECT(bad_parse_buffer(bytes.data(), bytes.size(), &rewritten) == 0);
    TEST_EXPECT(field_equal(retail_file, rewritten));
    std::printf("retail leg: BINOC.bad parse -> write -> parse field-equal (%zu -> %zu bytes, %s)\n",
                retail_bytes.size(), bytes.size(), bytes == retail_bytes ? "byte-identical" : "retail's post-NUL name bytes are uninitialized junk");
    if (dump_path != nullptr) {
        FILE *f = std::fopen(dump_path, "wb");
        if (f != nullptr) {
            std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
        }
    }
    bad_free(&rewritten);
    bad_free(&retail_file);
    return 0;
}
