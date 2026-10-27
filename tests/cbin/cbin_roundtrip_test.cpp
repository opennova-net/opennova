// CBIN roundtrip: the form's reader then its writer (formats/cbin/binary_config.h) reproduce a
// credits list byte for byte, and encode -> decode -> encode is a fixed point.
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
#include <formats/cbin/binary_config.h>

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

// A label's entry by its name's string, null when the label holds none.
const opennova::cbin::BinaryConfig::Entry *entry_named(const opennova::cbin::BinaryConfig &config,
                                                       const opennova::cbin::BinaryConfig::Label &label,
                                                       const char *name) {
    for (const auto &entry : label.entries) {
        const std::string *text = config.string_at(entry.name);
        if (text != nullptr && *text == name) return &entry;
    }
    return nullptr;
}

// One list: the CBIN magic, its "env" and "text" labels, the ENV pins, at least one text line with its
// font, the BHD bounds when expected, a byte-for-byte re-encode (left in `re_encoded`), and the internal
// fixed point, all through the form's reader and writer (formats/cbin/binary_config.h).
int check_list(const char *label, const std::string &path, const Expect &expect, std::vector<uint8_t> &re_encoded) {
    using opennova::cbin::BinaryConfig;
    int failures = 0;
    std::vector<uint8_t> original;
    if (!test_io::read_file(path, original)) {
        std::cerr << "FAIL: cannot load " << label << ": " << path << std::endl;
        return 1;
    }
    std::cout << "\n=== " << label << " (" << original.size() << " bytes) ===" << std::endl;
    if (!opennova::cbin::is_cbin(original.data(), original.size())) {
        std::cerr << "FAIL: " << label << " does not open with the CBIN magic" << std::endl;
        return 1;
    }

    BinaryConfig config;
    std::string error;
    if (!opennova::cbin::decode_binary_config(original.data(), original.size(), config, error)) {
        std::cerr << "FAIL: decode failed for " << label << ": " << error << std::endl;
        return 1;
    }
    if (config.labels.size() != 2 || config.string_at(config.labels[0].name) == nullptr ||
        *config.string_at(config.labels[0].name) != "env" || config.string_at(config.labels[1].name) == nullptr ||
        *config.string_at(config.labels[1].name) != "text") {
        std::cerr << "FAIL: " << label << " is not an env label and a text label" << std::endl;
        return 1;
    }
    const BinaryConfig::Label &env = config.labels[0];
    const auto word = [&](const char *name, uint32_t flags, uint32_t &out) {
        const BinaryConfig::Entry *entry = entry_named(config, env, name);
        if (entry == nullptr || entry->values.size() != 1 || entry->values[0].flags != flags) return false;
        out = entry->values[0].raw;
        return true;
    };
    uint32_t scroll_bits = 0, vertical_space = 0, center_x = 0, top_y = 0, bottom_y = 0;
    float scroll_rate = 0.0f;
    const bool env_read = word("scroll_rate", BinaryConfig::kFloat, scroll_bits) &&
                          word("vertical_space", BinaryConfig::kInteger, vertical_space) &&
                          word("center_x", BinaryConfig::kInteger, center_x);
    std::memcpy(&scroll_rate, &scroll_bits, sizeof scroll_rate);
    if (expect.pin_env && (!env_read || scroll_rate != expect.scroll_rate ||
                           int(vertical_space) != expect.vertical_space || int(center_x) != expect.center_x)) {
        std::cerr << "FAIL: " << label << " ENV values differ from the pins" << std::endl;
        failures++;
    }
    const bool has_top = word("top_y", BinaryConfig::kInteger, top_y);
    const bool has_bottom = word("bottom_y", BinaryConfig::kInteger, bottom_y);
    if ((has_top || has_bottom) != expect.bhd_bounds ||
        (expect.bhd_bounds && (!has_top || int(top_y) != expect.top_y || !has_bottom ||
                               int(bottom_y) != expect.bottom_y))) {
        std::cerr << "FAIL: " << label << " BHD bounds differ from the pins" << std::endl;
        failures++;
    }
    size_t text_with_font = 0;
    for (const auto &entry : config.labels[1].entries)
        if (entry.values.size() == 2 && entry.values[1].flags == BinaryConfig::kString) text_with_font++;
    if (text_with_font == 0) {
        std::cerr << "FAIL: " << label << " holds no text line with its font" << std::endl;
        failures++;
    }
    std::cout << "decoded " << config.labels[1].entries.size() << " text entries, " << text_with_font
              << " with fonts" << std::endl;

    if (!opennova::cbin::encode_binary_config(config, re_encoded, error)) {
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
    BinaryConfig again;
    std::vector<uint8_t> re_encoded2;
    if (!opennova::cbin::decode_binary_config(re_encoded.data(), re_encoded.size(), again, error) ||
        !opennova::cbin::encode_binary_config(again, re_encoded2, error) || re_encoded2 != re_encoded) {
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
