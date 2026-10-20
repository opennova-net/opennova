// CBIN roundtrip: decode -> encode reproduces a credits list byte for byte,
// and encode -> decode -> encode is a fixed point.
//
// Over the minted fixtures/cbin/synth_nlist*.kda lists (tests/fixtures/
// minimal_cbin_gen.cpp): the JO, JOX01 and BHD trio, and the shipped lists'
// own shapes, which the retired cbin_roundtrip_retail row read from the
// shipped trio (the retail encoder's output, its key, controls ahead of line
// ends and images, both image forms, BHD's plain lines and ~BINK_LOGO markup).
// Each re-encode is pinned by an FNV-1a64 vector (the nw_codec_identity
// pattern): a vector changes only with an [orig] witness or a D-CBIN entry;
// `cbin_roundtrip_test --dump` prints the replacement table.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "common/file_io.h"
#include <formats/cbin/cbin.h>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be defined"
#endif

namespace {

struct Expect {
    bool pin_env;  // the three ENV values below are asserted (the BHD title authors its own)
    float scroll_rate;
    int vertical_space;
    int center_x;
    bool bhd_bounds;  // top_y/bottom_y present
    int top_y;
    int bottom_y;
};

// One list: the ENV pins, at least one text entry with a font, the BHD bounds
// when expected, a byte-for-byte re-encode (left in `re_encoded`), and the
// internal fixed point.
int check_list(const char *label, const std::string &path, const Expect &expect, std::vector<uint8_t> &re_encoded) {
    int failures = 0;
    std::vector<uint8_t> original;
    if (!test_io::read_file(path, original)) {
        std::cerr << "FAIL: cannot load " << label << ": " << path << std::endl;
        return 1;
    }
    std::cout << "\n=== " << label << " (" << original.size() << " bytes) ===" << std::endl;

    opennova::cbin::Credits credits;
    std::string error;
    if (!opennova::cbin::decode_credits(original.data(), original.size(), credits, error)) {
        std::cerr << "FAIL: decode failed for " << label << ": " << error << std::endl;
        return 1;
    }
    if (expect.pin_env && (credits.scroll_rate != expect.scroll_rate ||
                           credits.vertical_space != expect.vertical_space || credits.center_x != expect.center_x)) {
        std::cerr << "FAIL: " << label << " ENV values differ from the pins" << std::endl;
        failures++;
    }
    size_t text_with_font = 0;
    for (const auto &e : credits.entries)
        if (e.type == opennova::cbin::EntryType::Text && !e.font.empty()) text_with_font++;
    if (text_with_font == 0) {
        std::cerr << "FAIL: " << label << " decoded without any text font names" << std::endl;
        failures++;
    }
    if (credits.has_bhd_bounds() != expect.bhd_bounds ||
        (expect.bhd_bounds && (!credits.has_top_y || credits.top_y != expect.top_y || !credits.has_bottom_y ||
                               credits.bottom_y != expect.bottom_y))) {
        std::cerr << "FAIL: " << label << " BHD bounds differ from the pins" << std::endl;
        failures++;
    }
    std::cout << "decoded " << credits.entries.size() << " entries, " << text_with_font << " with fonts"
              << std::endl;

    if (!opennova::cbin::encode(credits, re_encoded, error)) {
        std::cerr << "FAIL: encode failed for " << label << ": " << error << std::endl;
        return failures + 1;
    }
    if (re_encoded.size() != original.size() ||
        std::memcmp(original.data(), re_encoded.data(), original.size()) != 0) {
        size_t first = 0;
        while (first < original.size() && first < re_encoded.size() && original[first] == re_encoded[first]) ++first;
        std::cerr << "FAIL: " << label << " did not roundtrip byte-for-byte (size " << original.size() << " -> "
                  << re_encoded.size() << ", first difference at 0x" << std::hex << first << std::dec << ")"
                  << std::endl;
        failures++;
    } else {
        std::cout << "PASS: byte-for-byte identical" << std::endl;
    }

    // encode -> decode -> encode is a fixed point.
    opennova::cbin::Credits credits2;
    std::vector<uint8_t> re_encoded2;
    if (!opennova::cbin::decode_credits(re_encoded.data(), re_encoded.size(), credits2, error) ||
        credits2.entries.size() != credits.entries.size() ||
        !opennova::cbin::encode(credits2, re_encoded2, error) || re_encoded2 != re_encoded) {
        std::cerr << "FAIL: " << label << " internal roundtrip is not a fixed point" << std::endl;
        failures++;
    } else {
        std::cout << "PASS: internal roundtrip byte-identical" << std::endl;
    }
    return failures;
}

uint64_t fnv1a64(const std::vector<uint8_t> &bytes) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : bytes) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

struct List {
    const char *name;
    Expect expect;
};

// The minted lists and their pins: the trio (make_credits' shape), JO's and JOX01's controls and images,
// BHD's plain lines with its spacing and bounds.
const List kLists[] = {
    {"synth_nlist.kda", {true, 0.5f, 14, 400, false, 0, 0}},
    {"synth_nlist_jox01.kda", {true, 0.5f, 14, 400, false, 0, 0}},
    {"synth_nlist_bhd.kda", {true, 0.5f, 14, 400, true, 60, 580}},
    {"synth_nlist_controls.kda", {true, 0.5f, 14, 400, false, 0, 0}},
    {"synth_nlist_bhd_plain.kda", {true, 0.5f, 20, 400, true, 66, 588}},
};

struct Vector {
    const char *name;
    size_t len;
    uint64_t hash;
};

const Vector kExpected[] = {
    {"synth_nlist.kda", 1749u, 0x3ed30972e58b0afcull},
    {"synth_nlist_jox01.kda", 2097u, 0x611f10ee876bd71cull},
    {"synth_nlist_bhd.kda", 1454u, 0x5e2e4ab167160252ull},
    {"synth_nlist_controls.kda", 1237u, 0xb66922ab566c34b1ull},
    {"synth_nlist_bhd_plain.kda", 923u, 0xae0fca9182ec3854ull},
};

} // namespace

int main(int argc, char **argv) {
    bool dump = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--dump") == 0) dump = true;
    int failures = 0;
    const std::string synth = std::string(OPENNOVA_SOURCE_DIR) + "/fixtures/cbin/";
    const size_t count = sizeof(kLists) / sizeof(kLists[0]);
    std::string table = "const Vector kExpected[] = {\n";
    for (size_t i = 0; i < count; ++i) {
        std::vector<uint8_t> re_encoded;
        failures += check_list(kLists[i].name, synth + kLists[i].name, kLists[i].expect, re_encoded);
        const uint64_t hash = fnv1a64(re_encoded);
        char row[160];
        std::snprintf(row, sizeof(row), "    {\"%s\", %zuu, 0x%016llxull},\n", kLists[i].name, re_encoded.size(),
                      (unsigned long long)hash);
        table += row;
        if (dump) continue;
        const Vector &want = kExpected[i];
        if (std::strcmp(want.name, kLists[i].name) != 0 || want.len != re_encoded.size() || want.hash != hash) {
            std::cerr << "FAIL: " << kLists[i].name << " re-encodes to " << re_encoded.size() << " bytes, hash 0x"
                      << std::hex << hash << std::dec << "; the pinned vector says " << want.len
                      << " bytes (a change needs a witness; --dump prints the table)" << std::endl;
            failures++;
        }
    }
    if (dump) {
        std::printf("\n%s};\n", table.c_str());
        return 0;
    }
    if (failures != 0) {
        std::cerr << "FAIL: " << failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "\nPASS: all roundtrip checks passed, " << count << " lists pinned" << std::endl;
    return 0;
}
