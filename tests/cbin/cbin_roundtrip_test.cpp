// CBIN roundtrip integration test.
// Tests byte-for-byte parity: decode -> encode produces identical binary output.
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "cbin/cbin.h"

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be defined"
#endif

static constexpr const char* kFixturePath = OPENNOVA_SOURCE_DIR "/fixtures/cbin/nlist.reference.kda";
static constexpr const char* kJoxFixturePath = OPENNOVA_SOURCE_DIR "/fixtures/cbin/nlist.jox01.reference.kda";
static constexpr const char* kBhdFixturePath = OPENNOVA_SOURCE_DIR "/fixtures/cbin/nlist.bhd.reference.kda";

// Expected values from nlist.reference.kda
static constexpr float kExpectedScrollRate = 0.5f;
static constexpr int kExpectedVerticalSpace = 14;
static constexpr int kExpectedCenterX = 400;

bool load_file(const char* path, std::vector<uint8_t>& out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    out.resize(size);
    file.read(reinterpret_cast<char*>(out.data()), size);
    return file.good();
}

int main() {
    int failures = 0;

    // Load fixture
    std::vector<uint8_t> original;
    if (!load_file(kFixturePath, original)) {
        std::cerr << "FAIL: Cannot load fixture: " << kFixturePath << std::endl;
        return 1;
    }
    std::cout << "Loaded fixture: " << original.size() << " bytes" << std::endl;

    // ========================================
    // Test 1: Byte-for-byte roundtrip
    // decode -> encode must produce identical bytes
    // ========================================
    std::cout << "\n=== Test 1: Byte-for-byte roundtrip ===" << std::endl;

    cbin::Credits credits;
    std::string error;
    if (!cbin::decode_credits(original.data(), original.size(), credits, error)) {
        std::cerr << "FAIL: Decode failed: " << error << std::endl;
        return 1;
    }

    // Verify ENV values
    if (credits.scroll_rate != kExpectedScrollRate) {
        std::cerr << "FAIL: scroll_rate expected " << kExpectedScrollRate
                  << " got " << credits.scroll_rate << std::endl;
        failures++;
    }
    if (credits.vertical_space != kExpectedVerticalSpace) {
        std::cerr << "FAIL: vertical_space expected " << kExpectedVerticalSpace
                  << " got " << credits.vertical_space << std::endl;
        failures++;
    }
    if (credits.center_x != kExpectedCenterX) {
        std::cerr << "FAIL: center_x expected " << kExpectedCenterX
                  << " got " << credits.center_x << std::endl;
        failures++;
    }

    // Log entry count
    std::cout << "Decoded " << credits.entries.size() << " entries" << std::endl;

    // Count entries by type for informational purposes
    size_t text_count = 0, color_count = 0, newline_count = 0;
    size_t image_count = 0, justify_count = 0;
    size_t type2_count = 0;  // Entries with binary_type=2 (text+font pairs)
    size_t with_font_count = 0;  // Text entries with font
    for (const auto& e : credits.entries) {
        if (e.binary_type == 2) type2_count++;
        if (e.type == cbin::EntryType::Text && !e.font.empty()) with_font_count++;
        switch (e.type) {
            case cbin::EntryType::Text: text_count++; break;
            case cbin::EntryType::Color: color_count++; break;
            case cbin::EntryType::Newline: newline_count++; break;
            case cbin::EntryType::Image: image_count++; break;
            case cbin::EntryType::Justify: justify_count++; break;
        }
    }
    std::cout << "Entry counts: text=" << text_count << ", color=" << color_count
              << ", newline=" << newline_count << ", image=" << image_count
              << ", justify=" << justify_count << std::endl;
    std::cout << "Type=2 entries (binary): " << type2_count
              << ", text with font: " << with_font_count << std::endl;

    // Re-encode
    std::vector<uint8_t> re_encoded;
    if (!cbin::encode(credits, re_encoded, error)) {
        std::cerr << "FAIL: Encode failed: " << error << std::endl;
        return 1;
    }
    std::cout << "Re-encoded: " << re_encoded.size() << " bytes (original: "
              << original.size() << ")" << std::endl;

    // Check byte-for-byte parity
    if (re_encoded.size() != original.size()) {
        std::cerr << "FAIL: Size mismatch - expected " << original.size()
                  << " got " << re_encoded.size() << std::endl;
        failures++;
    } else if (std::memcmp(original.data(), re_encoded.data(), original.size()) != 0) {
        // Find all differences
        std::vector<size_t> diff_offsets;
        for (size_t i = 0; i < original.size(); i++) {
            if (original[i] != re_encoded[i]) {
                diff_offsets.push_back(i);
            }
        }

        std::cerr << "FAIL: " << diff_offsets.size() << " byte differences:" << std::endl;
        for (size_t i = 0; i < std::min(diff_offsets.size(), (size_t)10); i++) {
            size_t off = diff_offsets[i];
            std::cerr << "  0x" << std::hex << off << ": orig=0x" << (int)original[off]
                      << " got=0x" << (int)re_encoded[off] << std::dec << std::endl;
        }
        failures++;
    } else {
        std::cout << "PASS: Byte-for-byte identical!" << std::endl;
    }

    // ========================================
    // Test 2: Internal roundtrip consistency
    // encode -> decode -> encode produces identical output
    // ========================================
    std::cout << "\n=== Test 2: Internal roundtrip consistency ===" << std::endl;

    cbin::Credits credits2;
    if (!cbin::decode_credits(re_encoded.data(), re_encoded.size(), credits2, error)) {
        std::cerr << "FAIL: Re-decode failed: " << error << std::endl;
        return 1;
    }

    // Verify entries match
    if (credits2.entries.size() != credits.entries.size()) {
        std::cerr << "FAIL: Roundtrip entry count mismatch: " << credits.entries.size()
                  << " -> " << credits2.entries.size() << std::endl;
        failures++;
    }

    // Re-encode again
    std::vector<uint8_t> re_encoded2;
    if (!cbin::encode(credits2, re_encoded2, error)) {
        std::cerr << "FAIL: Internal re-encode failed: " << error << std::endl;
        return 1;
    }

    // Should be identical
    if (re_encoded.size() != re_encoded2.size()) {
        std::cerr << "FAIL: Internal size mismatch: " << re_encoded.size()
                  << " -> " << re_encoded2.size() << std::endl;
        failures++;
    } else if (std::memcmp(re_encoded.data(), re_encoded2.data(), re_encoded.size()) != 0) {
        std::cerr << "FAIL: Internal roundtrip bytes differ" << std::endl;
        failures++;
    } else {
        std::cout << "PASS: Internal roundtrip byte-identical!" << std::endl;
    }

    auto check_additional_fixture = [&](const char* label, const char* path, bool require_byte_identical) {
        std::cout << "\n=== Additional fixture: " << label << " ===" << std::endl;
        std::vector<uint8_t> fixture;
        if (!load_file(path, fixture)) {
            std::cerr << "FAIL: Cannot load fixture: " << path << std::endl;
            failures++;
            return;
        }

        cbin::Credits parsed;
        if (!cbin::decode_credits(fixture.data(), fixture.size(), parsed, error)) {
            std::cerr << "FAIL: Decode failed for " << label << ": " << error << std::endl;
            failures++;
            return;
        }
        size_t text_with_font_count = 0;
        for (const auto& entry : parsed.entries) {
            if (entry.type == cbin::EntryType::Text && !entry.font.empty()) {
                text_with_font_count++;
            }
        }
        if (text_with_font_count == 0) {
            std::cerr << "FAIL: " << label << " decoded without any text font names" << std::endl;
            failures++;
            return;
        }
        if (std::string(label).find("BHD") != std::string::npos) {
            if (!parsed.has_bhd_bounds() || !parsed.has_top_y || parsed.top_y != 66 ||
                !parsed.has_bottom_y || parsed.bottom_y != 588) {
                std::cerr << "FAIL: " << label << " did not expose expected BHD bounds" << std::endl;
                failures++;
                return;
            }
        }

        std::vector<uint8_t> encoded;
        if (!cbin::encode(parsed, encoded, error)) {
            std::cerr << "FAIL: Encode failed for " << label << ": " << error << std::endl;
            failures++;
            return;
        }

        if (require_byte_identical) {
            if (encoded.size() != fixture.size() ||
                std::memcmp(encoded.data(), fixture.data(), fixture.size()) != 0) {
                std::cerr << "FAIL: " << label << " did not roundtrip byte-for-byte" << std::endl;
                failures++;
                return;
            }
            std::cout << "PASS: " << label << " byte-for-byte roundtrip" << std::endl;
        } else {
            cbin::Credits reparsed;
            if (!cbin::decode_credits(encoded.data(), encoded.size(), reparsed, error)) {
                std::cerr << "FAIL: Re-decode failed for " << label << ": " << error << std::endl;
                failures++;
                return;
            }
            size_t reparsed_text_with_font_count = 0;
            for (const auto& entry : reparsed.entries) {
                if (entry.type == cbin::EntryType::Text && !entry.font.empty()) {
                    reparsed_text_with_font_count++;
                }
            }
            if (reparsed_text_with_font_count != text_with_font_count) {
                std::cerr << "FAIL: " << label << " font-name count changed after re-encode: "
                          << text_with_font_count << " -> " << reparsed_text_with_font_count << std::endl;
                failures++;
                return;
            }
            std::cout << "PASS: " << label << " decode/encode/decode with font names" << std::endl;
        }
    };

    check_additional_fixture("JOX01 nlist", kJoxFixturePath, true);
    check_additional_fixture("BHD nlist", kBhdFixturePath, true);

    // ========================================
    // Summary
    // ========================================
    std::cout << "\n=== Summary ===" << std::endl;
    if (failures == 0) {
        std::cout << "PASS: All roundtrip checks passed" << std::endl;
        return 0;
    } else {
        std::cerr << "FAIL: " << failures << " test(s) failed" << std::endl;
        return 1;
    }
}
