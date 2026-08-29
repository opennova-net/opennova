// CBIN roundtrip: decode -> encode reproduces a credits list byte for byte,
// and encode -> decode -> encode is a fixed point.
//
// Two legs. The minted fixtures/cbin/synth_nlist*.kda trio (JO, JOX01 and BHD
// shapes; tests/fixtures/minimal_cbin_gen.cpp) runs unconditionally. The
// shipped trio — the retail encoder's own output, redundant control runs and
// all — is read from the reference fixture set behind OPENNOVA_JO_ASSETS and
// must re-encode byte for byte too.
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "common/retail_paths.h"
#include <formats/cbin/cbin.h>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be defined"
#endif

namespace {

bool load_file(const std::string &path, std::vector<uint8_t> &out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    file.read(reinterpret_cast<char *>(out.data()), size);
    return file.good();
}

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
// when expected, a byte-for-byte re-encode, and the internal fixed point.
int check_list(const char *label, const std::string &path, const Expect &expect) {
    int failures = 0;
    std::vector<uint8_t> original;
    if (!load_file(path, original)) {
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

    std::vector<uint8_t> re_encoded;
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

} // namespace

int main() {
    int failures = 0;
    const std::string synth = std::string(OPENNOVA_SOURCE_DIR) + "/fixtures/cbin/";
    failures += check_list("synthetic JO nlist", synth + "synth_nlist.kda", {true, 0.5f, 14, 400, false, 0, 0});
    failures += check_list("synthetic JOX01 nlist", synth + "synth_nlist_jox01.kda", {true, 0.5f, 14, 400, false, 0, 0});
    failures += check_list("synthetic BHD nlist", synth + "synth_nlist_bhd.kda", {true, 0.5f, 14, 400, true, 60, 580});
    if (failures != 0) {
        std::cerr << "FAIL: " << failures << " synthetic check(s) failed" << std::endl;
        return 1;
    }

    // The retail leg: the shipped JO / JOX01 / BHD lists.
    const std::string jo = retail::reference_fixture("cbin/nlist.reference.kda");
    const std::string jox = retail::reference_fixture("cbin/nlist.jox01.reference.kda");
    const std::string bhd = retail::reference_fixture("cbin/nlist.bhd.reference.kda");
    if (jo.empty() || jox.empty() || bhd.empty())
        return retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/cbin/nlist*.reference.kda (the shipped credits lists)");
    failures += check_list("retail JO nlist", jo, {true, 0.5f, 14, 400, false, 0, 0});
    failures += check_list("retail JOX01 nlist", jox, {true, 0.5f, 14, 400, false, 0, 0});
    failures += check_list("retail BHD nlist", bhd, {false, 0.0f, 0, 0, true, 66, 588});
    if (failures != 0) {
        std::cerr << "FAIL: " << failures << " retail check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "\nPASS: all roundtrip checks passed (synthetic + retail)" << std::endl;
    return 0;
}
