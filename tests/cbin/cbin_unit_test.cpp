// CBIN unit tests - no fixtures required.
// Tests encoding/decoding API with programmatically created data.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "cbin/cbin.h"

static int test_count = 0;
static int fail_count = 0;

#define TEST(name) \
    void test_##name(); \
    struct test_##name##_register { \
        test_##name##_register() { \
            test_count++; \
            std::cout << "TEST " << #name << ": "; \
            test_##name(); \
        } \
    } test_##name##_instance; \
    void test_##name()

#define EXPECT_TRUE(cond) do { if (!(cond)) { std::cerr << "FAIL at line " << __LINE__ << ": " #cond << std::endl; fail_count++; return; } } while(0)
#define EXPECT_EQ(a, b) do { if ((a) != (b)) { std::cerr << "FAIL at line " << __LINE__ << ": expected equality" << std::endl; fail_count++; return; } } while(0)
#define EXPECT_FLOAT_EQ(a, b) do { if (std::abs((a) - (b)) > 0.0001f) { std::cerr << "FAIL at line " << __LINE__ << ": " << (a) << " != " << (b) << std::endl; fail_count++; return; } } while(0)
#define PASS() std::cout << "PASS" << std::endl

// Test empty credits encode/decode
TEST(empty_credits) {
    cbin::Credits credits;
    credits.scroll_rate = 1.0f;
    credits.vertical_space = 20;
    credits.center_x = 320;

    std::vector<uint8_t> encoded;
    std::string error;
    EXPECT_TRUE(cbin::encode(credits, encoded, error));
    EXPECT_TRUE(encoded.size() > 20);  // At least header

    // Verify magic
    uint32_t magic;
    std::memcpy(&magic, encoded.data(), 4);
    EXPECT_EQ(magic, cbin::kMagic);

    // Decode
    cbin::Credits decoded;
    EXPECT_TRUE(cbin::decode_credits(encoded.data(), encoded.size(), decoded, error));

    EXPECT_FLOAT_EQ(decoded.scroll_rate, 1.0f);
    EXPECT_EQ(decoded.vertical_space, 20);
    EXPECT_EQ(decoded.center_x, 320);
    EXPECT_EQ(decoded.entries.size(), 0u);

    PASS();
}

// Test text entry roundtrip
TEST(text_entry_roundtrip) {
    cbin::Credits credits;
    credits.entries.push_back(cbin::Entry::make_text("Hello World", ""));

    std::vector<uint8_t> encoded;
    std::string error;
    EXPECT_TRUE(cbin::encode(credits, encoded, error));

    cbin::Credits decoded;
    EXPECT_TRUE(cbin::decode_credits(encoded.data(), encoded.size(), decoded, error));

    EXPECT_EQ(decoded.entries.size(), 1u);
    EXPECT_EQ(decoded.entries[0].type, cbin::EntryType::Text);
    EXPECT_EQ(decoded.entries[0].text, "Hello World");
    EXPECT_EQ(decoded.entries[0].font, "");

    PASS();
}

// Test text with font roundtrip
TEST(text_with_font_roundtrip) {
    cbin::Credits credits;
    credits.entries.push_back(cbin::Entry::make_text("Title", "Arial24"));

    std::vector<uint8_t> encoded;
    std::string error;
    EXPECT_TRUE(cbin::encode(credits, encoded, error));

    cbin::Credits decoded;
    EXPECT_TRUE(cbin::decode_credits(encoded.data(), encoded.size(), decoded, error));

    EXPECT_EQ(decoded.entries.size(), 1u);
    EXPECT_EQ(decoded.entries[0].type, cbin::EntryType::Text);
    EXPECT_EQ(decoded.entries[0].text, "Title");
    EXPECT_EQ(decoded.entries[0].font, "Arial24");

    PASS();
}

// Test color entry roundtrip
TEST(color_entry_roundtrip) {
    cbin::Credits credits;
    credits.entries.push_back(cbin::Entry::make_color(0xFF0000));  // Red
    credits.entries.push_back(cbin::Entry::make_color(0x00FF00));  // Green
    credits.entries.push_back(cbin::Entry::make_color(0x0000FF));  // Blue

    std::vector<uint8_t> encoded;
    std::string error;
    EXPECT_TRUE(cbin::encode(credits, encoded, error));

    cbin::Credits decoded;
    EXPECT_TRUE(cbin::decode_credits(encoded.data(), encoded.size(), decoded, error));

    EXPECT_EQ(decoded.entries.size(), 3u);
    EXPECT_EQ(decoded.entries[0].type, cbin::EntryType::Color);
    EXPECT_EQ(decoded.entries[0].color, 0xFF0000u);
    EXPECT_EQ(decoded.entries[1].color, 0x00FF00u);
    EXPECT_EQ(decoded.entries[2].color, 0x0000FFu);

    PASS();
}

// Test newline entry roundtrip
TEST(newline_entry_roundtrip) {
    cbin::Credits credits;
    credits.entries.push_back(cbin::Entry::make_newline());
    credits.entries.push_back(cbin::Entry::make_newline());

    std::vector<uint8_t> encoded;
    std::string error;
    EXPECT_TRUE(cbin::encode(credits, encoded, error));

    cbin::Credits decoded;
    EXPECT_TRUE(cbin::decode_credits(encoded.data(), encoded.size(), decoded, error));

    EXPECT_EQ(decoded.entries.size(), 2u);
    EXPECT_EQ(decoded.entries[0].type, cbin::EntryType::Newline);
    EXPECT_EQ(decoded.entries[1].type, cbin::EntryType::Newline);

    PASS();
}

// Test image entry roundtrip
TEST(image_entry_roundtrip) {
    cbin::Credits credits;
    credits.entries.push_back(cbin::Entry::make_image("logo.pcx", 0));
    credits.entries.push_back(cbin::Entry::make_image("banner.pcx", 1));

    std::vector<uint8_t> encoded;
    std::string error;
    EXPECT_TRUE(cbin::encode(credits, encoded, error));

    cbin::Credits decoded;
    EXPECT_TRUE(cbin::decode_credits(encoded.data(), encoded.size(), decoded, error));

    EXPECT_EQ(decoded.entries.size(), 2u);
    EXPECT_EQ(decoded.entries[0].type, cbin::EntryType::Image);
    EXPECT_EQ(decoded.entries[0].image_path, "logo.pcx");
    EXPECT_EQ(decoded.entries[0].image_display_x, 0);
    EXPECT_EQ(decoded.entries[1].image_path, "banner.pcx");
    EXPECT_EQ(decoded.entries[1].image_display_x, 1);

    PASS();
}

// Test justify entry roundtrip
TEST(justify_entry_roundtrip) {
    cbin::Credits credits;
    credits.entries.push_back(cbin::Entry::make_justify(cbin::Justify::Left));
    credits.entries.push_back(cbin::Entry::make_justify(cbin::Justify::Center));
    credits.entries.push_back(cbin::Entry::make_justify(cbin::Justify::Right));

    std::vector<uint8_t> encoded;
    std::string error;
    EXPECT_TRUE(cbin::encode(credits, encoded, error));

    cbin::Credits decoded;
    EXPECT_TRUE(cbin::decode_credits(encoded.data(), encoded.size(), decoded, error));

    EXPECT_EQ(decoded.entries.size(), 3u);
    EXPECT_EQ(decoded.entries[0].type, cbin::EntryType::Justify);
    EXPECT_EQ(decoded.entries[0].justify, cbin::Justify::Left);
    EXPECT_EQ(decoded.entries[1].justify, cbin::Justify::Center);
    EXPECT_EQ(decoded.entries[2].justify, cbin::Justify::Right);

    PASS();
}

// Test mixed entries roundtrip
TEST(mixed_entries_roundtrip) {
    cbin::Credits credits;
    credits.scroll_rate = 0.75f;
    credits.vertical_space = 16;
    credits.center_x = 400;

    credits.entries.push_back(cbin::Entry::make_justify(cbin::Justify::Center));
    credits.entries.push_back(cbin::Entry::make_color(0xFFFFFF));
    credits.entries.push_back(cbin::Entry::make_text("Game Credits", "Title32"));
    credits.entries.push_back(cbin::Entry::make_newline());
    credits.entries.push_back(cbin::Entry::make_newline());
    credits.entries.push_back(cbin::Entry::make_color(0xAAAAAA));
    credits.entries.push_back(cbin::Entry::make_text("Director", ""));
    credits.entries.push_back(cbin::Entry::make_newline());
    credits.entries.push_back(cbin::Entry::make_text("John Doe", "Name24"));
    credits.entries.push_back(cbin::Entry::make_newline());
    credits.entries.push_back(cbin::Entry::make_image("logo.pcx", 0));

    std::vector<uint8_t> encoded;
    std::string error;
    EXPECT_TRUE(cbin::encode(credits, encoded, error));

    cbin::Credits decoded;
    EXPECT_TRUE(cbin::decode_credits(encoded.data(), encoded.size(), decoded, error));

    EXPECT_FLOAT_EQ(decoded.scroll_rate, 0.75f);
    EXPECT_EQ(decoded.vertical_space, 16);
    EXPECT_EQ(decoded.center_x, 400);
    EXPECT_EQ(decoded.entries.size(), credits.entries.size());

    // Verify each entry
    for (size_t i = 0; i < credits.entries.size(); i++) {
        EXPECT_EQ(decoded.entries[i].type, credits.entries[i].type);
        switch (credits.entries[i].type) {
            case cbin::EntryType::Text:
                EXPECT_EQ(decoded.entries[i].text, credits.entries[i].text);
                EXPECT_EQ(decoded.entries[i].font, credits.entries[i].font);
                break;
            case cbin::EntryType::Color:
                EXPECT_EQ(decoded.entries[i].color, credits.entries[i].color);
                break;
            case cbin::EntryType::Image:
                EXPECT_EQ(decoded.entries[i].image_path, credits.entries[i].image_path);
                EXPECT_EQ(decoded.entries[i].image_display_x, credits.entries[i].image_display_x);
                break;
            case cbin::EntryType::Justify:
                EXPECT_EQ(decoded.entries[i].justify, credits.entries[i].justify);
                break;
            default:
                break;
        }
    }

    PASS();
}

// Test is_cbin function
TEST(is_cbin_detection) {
    // Valid CBIN data
    cbin::Credits credits;
    credits.entries.push_back(cbin::Entry::make_text("Test", ""));
    std::vector<uint8_t> encoded;
    std::string error;
    EXPECT_TRUE(cbin::encode(credits, encoded, error));
    EXPECT_TRUE(cbin::is_cbin(encoded.data(), encoded.size()));

    // Invalid magic
    std::vector<uint8_t> invalid = {0x00, 0x00, 0x00, 0x00};
    EXPECT_TRUE(!cbin::is_cbin(invalid.data(), invalid.size()));

    // Too small
    std::vector<uint8_t> small = {0x43, 0x42, 0x49, 0x4E};  // Just "CBIN"
    EXPECT_TRUE(!cbin::is_cbin(small.data(), small.size()));

    PASS();
}

// Test string deduplication in encoder
TEST(string_deduplication) {
    cbin::Credits credits;
    // Add multiple entries with same text/font to test deduplication
    for (int i = 0; i < 10; i++) {
        credits.entries.push_back(cbin::Entry::make_text("Repeated Text", "Font24"));
        credits.entries.push_back(cbin::Entry::make_newline());
    }

    std::vector<uint8_t> encoded;
    std::string error;
    EXPECT_TRUE(cbin::encode(credits, encoded, error));

    cbin::Credits decoded;
    EXPECT_TRUE(cbin::decode_credits(encoded.data(), encoded.size(), decoded, error));

    EXPECT_EQ(decoded.entries.size(), 20u);

    // Verify all text entries have same content
    int text_count = 0;
    for (const auto& e : decoded.entries) {
        if (e.type == cbin::EntryType::Text) {
            EXPECT_EQ(e.text, "Repeated Text");
            EXPECT_EQ(e.font, "Font24");
            text_count++;
        }
    }
    EXPECT_EQ(text_count, 10);

    PASS();
}

// Test env_extra map
TEST(env_extra_roundtrip) {
    cbin::Credits credits;
    credits.env_extra["custom_key"] = "custom_value";
    credits.env_extra["another_key"] = "another_value";

    std::vector<uint8_t> encoded;
    std::string error;
    EXPECT_TRUE(cbin::encode(credits, encoded, error));

    cbin::Credits decoded;
    EXPECT_TRUE(cbin::decode_credits(encoded.data(), encoded.size(), decoded, error));

    EXPECT_EQ(decoded.env_extra.count("custom_key"), 1u);
    EXPECT_EQ(decoded.env_extra["custom_key"], "custom_value");
    EXPECT_EQ(decoded.env_extra.count("another_key"), 1u);
    EXPECT_EQ(decoded.env_extra["another_key"], "another_value");

    PASS();
}

// Test internal roundtrip produces consistent sizes
TEST(internal_roundtrip_consistency) {
    cbin::Credits credits;
    credits.scroll_rate = 0.5f;
    credits.vertical_space = 14;
    credits.center_x = 400;
    credits.entries.push_back(cbin::Entry::make_text("Line 1", "Font24"));
    credits.entries.push_back(cbin::Entry::make_newline());
    credits.entries.push_back(cbin::Entry::make_color(0xFFFF00));
    credits.entries.push_back(cbin::Entry::make_text("Line 2", ""));

    std::string error;

    // Encode once
    std::vector<uint8_t> encoded1;
    EXPECT_TRUE(cbin::encode(credits, encoded1, error));

    // Decode
    cbin::Credits decoded;
    EXPECT_TRUE(cbin::decode_credits(encoded1.data(), encoded1.size(), decoded, error));

    // Encode again
    std::vector<uint8_t> encoded2;
    EXPECT_TRUE(cbin::encode(decoded, encoded2, error));

    // Sizes should match (XOR key differs, but structure is deterministic)
    EXPECT_EQ(encoded1.size(), encoded2.size());

    // Decode again and verify content matches
    cbin::Credits decoded2;
    EXPECT_TRUE(cbin::decode_credits(encoded2.data(), encoded2.size(), decoded2, error));

    EXPECT_EQ(decoded.entries.size(), decoded2.entries.size());
    EXPECT_FLOAT_EQ(decoded.scroll_rate, decoded2.scroll_rate);
    EXPECT_EQ(decoded.vertical_space, decoded2.vertical_space);
    EXPECT_EQ(decoded.center_x, decoded2.center_x);

    PASS();
}

int main() {
    // Tests are registered automatically by constructors above
    std::cout << "\n=== CBIN Unit Test Results ===" << std::endl;
    std::cout << "Total: " << test_count << ", Failed: " << fail_count << std::endl;

    if (fail_count == 0) {
        std::cout << "PASS: All tests passed" << std::endl;
        return 0;
    } else {
        std::cerr << "FAIL: " << fail_count << " test(s) failed" << std::endl;
        return 1;
    }
}
