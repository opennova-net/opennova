// The CBIN display view: Color/Justify controls collapse into stamped items
// (credits_display_items) and re-emit by diff (credits_entries_from_display).
// The fixture leg proves the display view round-trips a minted credits file
// SEMANTICALLY (the re-emission
// canonicalizes redundant control runs, so byte identity is cbin_roundtrip's
// job, not this test's).
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include <formats/cbin/cbin.h>
#include "common/test_expect.h"

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be defined"
#endif

static constexpr const char* kFixturePath =
    OPENNOVA_SOURCE_DIR "/fixtures/cbin/synth_nlist.kda";

static bool load_file(const char* path, std::vector<uint8_t>& out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    return static_cast<bool>(
        file.read(reinterpret_cast<char*>(out.data()), size));
}

static bool items_equal(const opennova::cbin::CreditsDisplayItem& a,
                        const opennova::cbin::CreditsDisplayItem& b) {
    return a.type == b.type && a.text == b.text && a.font == b.font &&
           a.color == b.color && a.justify == b.justify &&
           a.image_path == b.image_path &&
           a.image_display_x == b.image_display_x &&
           a.image_display_y == b.image_display_y &&
           a.use_simple_image_format == b.use_simple_image_format;
}

int main() {
    using opennova::cbin::Credits;
    using opennova::cbin::Entry;
    using opennova::cbin::EntryType;
    using opennova::cbin::Justify;

    // Seeds: a lone text item is stamped white/center.
    {
        Credits credits;
        credits.entries.push_back(Entry::make_text("HELLO", "MainFont"));
        const auto items = opennova::cbin::credits_display_items(credits);
        TEST_EXPECT(items.size() == 1);
        TEST_EXPECT(items[0].type == EntryType::Text);
        TEST_EXPECT(items[0].color == opennova::cbin::kDefaultDisplayColor);
        TEST_EXPECT(items[0].justify == Justify::Center);
        TEST_EXPECT(items[0].font == "MainFont");
    }

    // Collapse: controls vanish, state stamps every later text, newline/image
    // pass through untouched by the state.
    {
        Credits credits;
        credits.entries.push_back(Entry::make_color(0xFF0000));
        credits.entries.push_back(Entry::make_justify(Justify::Left));
        credits.entries.push_back(Entry::make_text("RED_LEFT"));
        credits.entries.push_back(Entry::make_newline());
        credits.entries.push_back(Entry::make_image("logo.tga", 10, 20));
        credits.entries.push_back(Entry::make_text("STILL_RED_LEFT"));
        const auto items = opennova::cbin::credits_display_items(credits);
        TEST_EXPECT(items.size() == 4);
        TEST_EXPECT(items[0].type == EntryType::Text);
        TEST_EXPECT(items[0].color == 0xFF0000);
        TEST_EXPECT(items[0].justify == Justify::Left);
        TEST_EXPECT(items[1].type == EntryType::Newline);
        TEST_EXPECT(items[2].type == EntryType::Image);
        TEST_EXPECT(items[2].image_path == "logo.tga");
        TEST_EXPECT(items[2].image_display_x == 10);
        TEST_EXPECT(items[2].image_display_y == 20);
        TEST_EXPECT(items[3].color == 0xFF0000);
        TEST_EXPECT(items[3].justify == Justify::Left);
    }

    // Emit-by-diff: color before justify, only immediately before TEXT items;
    // an unchanged state emits nothing; newline/image never emit controls.
    {
        std::vector<opennova::cbin::CreditsDisplayItem> items(4);
        items[0].type = EntryType::Text;
        items[0].text = "A";
        items[0].color = 0x00FF00;
        items[0].justify = Justify::Right;
        items[1].type = EntryType::Newline;
        items[2].type = EntryType::Text;
        items[2].text = "B";
        items[2].color = 0x00FF00;  // unchanged: no controls
        items[2].justify = Justify::Right;
        items[3].type = EntryType::Image;
        items[3].image_path = "pic.tga";
        const auto entries = opennova::cbin::credits_entries_from_display(items);
        TEST_EXPECT(entries.size() == 6);
        TEST_EXPECT(entries[0].type == EntryType::Color);
        TEST_EXPECT(entries[0].color == 0x00FF00);
        TEST_EXPECT(entries[1].type == EntryType::Justify);
        TEST_EXPECT(entries[1].justify == Justify::Right);
        TEST_EXPECT(entries[2].type == EntryType::Text);
        TEST_EXPECT(entries[3].type == EntryType::Newline);
        TEST_EXPECT(entries[4].type == EntryType::Text);
        TEST_EXPECT(entries[5].type == EntryType::Image);
    }

    // Seed state emits nothing: white/center text needs no leading controls.
    {
        std::vector<opennova::cbin::CreditsDisplayItem> items(1);
        items[0].type = EntryType::Text;
        items[0].text = "PLAIN";
        const auto entries = opennova::cbin::credits_entries_from_display(items);
        TEST_EXPECT(entries.size() == 1);
        TEST_EXPECT(entries[0].type == EntryType::Text);
    }

    // Fixture leg: the display view round-trips a minted credits file
    // semantically — decode -> items -> entries -> encode -> decode -> items
    // produces the identical item sequence.
    {
        std::vector<uint8_t> data;
        TEST_EXPECT(load_file(kFixturePath, data));
        Credits credits;
        std::string error;
        TEST_EXPECT(opennova::cbin::decode_credits(data.data(), data.size(), credits, error));
        const auto items = opennova::cbin::credits_display_items(credits);
        TEST_EXPECT(!items.empty());

        Credits rebuilt = credits;  // ENV + string-table policy carried over
        rebuilt.entries = opennova::cbin::credits_entries_from_display(items);
        std::vector<uint8_t> encoded;
        TEST_EXPECT(opennova::cbin::encode(rebuilt, encoded, error));
        Credits reparsed;
        TEST_EXPECT(opennova::cbin::decode_credits(encoded.data(), encoded.size(),
                                         reparsed, error));
        const auto items2 = opennova::cbin::credits_display_items(reparsed);
        TEST_EXPECT(items2.size() == items.size());
        for (size_t i = 0; i < items.size(); ++i) {
            TEST_EXPECT(items_equal(items[i], items2[i]));
        }
    }

    std::printf("OK: cbin display view collapses and re-emits the control codes\n");
    return 0;
}
