// libs/refs extractor tests — fixtures are synthesized in-memory through each
// format's own writer (env::save_env, cbin::encode, plain-text items.def), so
// no committed binaries are needed and the edges asserted are exactly what the
// format libraries themselves produce.
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "cbin/cbin.h"
#include "env/env.h"
#include "refs/refs.h"

using opennova::refs::Reference;

static int fail_count = 0;

#define EXPECT_TRUE(cond)                                                                  \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::cerr << "FAIL at line " << __LINE__ << ": " #cond << std::endl;           \
            fail_count++;                                                                  \
        }                                                                                  \
    } while (0)

#define EXPECT_EQ_STR(a, b)                                                                \
    do {                                                                                   \
        const std::string va = (a), vb = (b);                                              \
        if (va != vb) {                                                                    \
            std::cerr << "FAIL at line " << __LINE__ << ": \"" << va << "\" != \"" << vb   \
                      << "\"" << std::endl;                                                \
            fail_count++;                                                                  \
        }                                                                                  \
    } while (0)

#define EXPECT_EQ_SZ(a, b)                                                                 \
    do {                                                                                   \
        const size_t va = (a), vb = (b);                                                   \
        if (va != vb) {                                                                    \
            std::cerr << "FAIL at line " << __LINE__ << ": " << va << " != " << vb         \
                      << std::endl;                                                        \
            fail_count++;                                                                  \
        }                                                                                  \
    } while (0)

static std::vector<Reference> extract_ok(const std::string& name, const std::string& bytes) {
    std::vector<Reference> out;
    std::string error;
    const bool ok = opennova::refs::extract(
        name, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), out, error);
    if (!ok) {
        std::cerr << "FAIL: extract(" << name << ") errored: " << error << std::endl;
        fail_count++;
    }
    return out;
}

static const Reference* find_edge(const std::vector<Reference>& edges, const std::string& site) {
    for (const Reference& r : edges) {
        if (r.site == site) {
            return &r;
        }
    }
    return nullptr;
}

static void test_env_extracts_sky_and_celestial() {
    opennova::env::Config cfg;
    cfg.sky_map1 = "MyClouds.pcx";
    cfg.sky_map2 = "myclouds_b.pcx";
    cfg.sun_3di = "customsun.3di";
    std::ostringstream body;
    std::string error;
    EXPECT_TRUE(opennova::env::save_env(body, cfg, error));

    const std::vector<Reference> edges = extract_ok("skies/Desert.Env", body.str());
    EXPECT_EQ_SZ(edges.size(), 6u);  // 2 textures + 4 celestial models

    const Reference* sky1 = find_edge(edges, "sky_map1");
    EXPECT_TRUE(sky1 != nullptr);
    if (sky1 != nullptr) {
        EXPECT_EQ_STR(sky1->source_path, "skies/Desert.Env");
        EXPECT_EQ_STR(sky1->source_kind, "environment");
        EXPECT_EQ_STR(sky1->target_name, "MyClouds.pcx");  // verbatim case
        EXPECT_EQ_STR(sky1->target_kind, "texture");
    }
    const Reference* sun = find_edge(edges, "sun_3di");
    EXPECT_TRUE(sun != nullptr);
    if (sun != nullptr) {
        EXPECT_EQ_STR(sun->target_name, "customsun.3di");
        EXPECT_EQ_STR(sun->target_kind, "object_model");
    }
    // Fields the file left alone surface the engine defaults: the engine WILL
    // load them, so they are the file's effective references.
    const Reference* moon = find_edge(edges, "moon_3di");
    EXPECT_TRUE(moon != nullptr);
    if (moon != nullptr) {
        EXPECT_EQ_STR(moon->target_name, "fmoon4.3di");
    }
}

static void test_env_defaults_alone_still_reference() {
    // An empty .env keeps every engine default — all six edges still emit.
    const std::vector<Reference> edges = extract_ok("empty.env", "");
    EXPECT_EQ_SZ(edges.size(), 6u);
    const Reference* sky1 = find_edge(edges, "sky_map1");
    EXPECT_TRUE(sky1 != nullptr);
    if (sky1 != nullptr) {
        EXPECT_EQ_STR(sky1->target_name, "cld_day1.pcx");
    }
}

static void test_credits_fonts_and_images_dedupe() {
    cbin::Credits credits;
    credits.entries.push_back(cbin::Entry::make_text("Lead Designer", "bigfont"));
    credits.entries.push_back(cbin::Entry::make_newline());
    credits.entries.push_back(cbin::Entry::make_text("Someone", "smallfont"));
    credits.entries.push_back(cbin::Entry::make_text("Someone Else", "BIGFONT"));  // dup, case-insensitive
    credits.entries.push_back(cbin::Entry::make_image("logo.tga", 10, 20));
    std::vector<uint8_t> bytes;
    std::string error;
    EXPECT_TRUE(cbin::encode(credits, bytes, error));

    const std::vector<Reference> edges =
        extract_ok("credits.kda", std::string(bytes.begin(), bytes.end()));
    // bigfont (first site wins), smallfont, logo.tga — the BIGFONT repeat dedupes.
    EXPECT_EQ_SZ(edges.size(), 3u);
    const Reference* big = find_edge(edges, "entry[0].font");
    EXPECT_TRUE(big != nullptr);
    if (big != nullptr) {
        EXPECT_EQ_STR(big->source_kind, "credits");
        EXPECT_EQ_STR(big->target_name, "bigfont");
        EXPECT_EQ_STR(big->target_kind, "font");
    }
    const Reference* image = find_edge(edges, "entry[4].image");
    EXPECT_TRUE(image != nullptr);
    if (image != nullptr) {
        EXPECT_EQ_STR(image->target_name, "logo.tga");
        EXPECT_EQ_STR(image->target_kind, "image");
    }
}

static void test_items_def_field_edges() {
    const std::string items_def =
        "begin \"Dune Buggy\"\n"
        "  id 451\n"
        "  type vehicle\n"
        "  graphic Dbuggy1\n"
        "  anim_def buggyanim\n"
        "  husk DbuggyHusk\n"
        "  sound_profile SP_Buggy\n"
        "  soundloop_1 SP_Engine\n"
        "end\n"
        "begin \"Plain Marker\"\n"
        "  id 7\n"
        "  type marker\n"
        "  graphic flag1\n"
        "end\n";

    const std::vector<Reference> edges = extract_ok("ITEMS.DEF", items_def);

    const Reference* graphic = find_edge(edges, "item 451 graphic");
    EXPECT_TRUE(graphic != nullptr);
    if (graphic != nullptr) {
        EXPECT_EQ_STR(graphic->source_kind, "item_defs");
        EXPECT_EQ_STR(graphic->target_name, "Dbuggy1");
        EXPECT_EQ_STR(graphic->target_kind, "object_model");
    }
    const Reference* husk = find_edge(edges, "item 451 husk");
    EXPECT_TRUE(husk != nullptr && husk->target_kind == "object_model");
    const Reference* anim = find_edge(edges, "item 451 anim_def");
    EXPECT_TRUE(anim != nullptr && anim->target_kind == "anim_def");
    const Reference* profile = find_edge(edges, "item 451 sound_profile");
    EXPECT_TRUE(profile != nullptr && profile->target_kind == "sound_profile");
    const Reference* loop = find_edge(edges, "item 451 soundloop[0]");
    EXPECT_TRUE(loop != nullptr && loop->target_name == "SP_Engine");
    // The marker sets only its graphic — no empty-field edges.
    const Reference* marker = find_edge(edges, "item 7 graphic");
    EXPECT_TRUE(marker != nullptr);
    EXPECT_TRUE(find_edge(edges, "item 7 anim_def") == nullptr);
}

static void test_unrecognized_is_success_with_no_edges() {
    std::vector<Reference> out;
    std::string error;
    const uint8_t junk[] = {1, 2, 3};
    EXPECT_TRUE(opennova::refs::extract("texture.tga", junk, sizeof(junk), out, error));
    EXPECT_EQ_SZ(out.size(), 0u);
    // weapon.def has no extractor yet — only items.def dispatches.
    EXPECT_TRUE(opennova::refs::extract("weapon.def", junk, sizeof(junk), out, error));
    EXPECT_EQ_SZ(out.size(), 0u);
}

static void test_can_extract_dispatch() {
    EXPECT_TRUE(opennova::refs::can_extract("foo.env"));
    EXPECT_TRUE(opennova::refs::can_extract("FOO.ENV"));
    EXPECT_TRUE(opennova::refs::can_extract("sub/dir\\credits.KDA"));
    EXPECT_TRUE(opennova::refs::can_extract("C:/game/ITEMS.DEF"));
    EXPECT_TRUE(opennova::refs::can_extract("model.3DI"));
    EXPECT_TRUE(opennova::refs::can_extract("ash_i5b.bms"));
    EXPECT_TRUE(opennova::refs::can_extract("jo_main.MNU"));
    EXPECT_TRUE(!opennova::refs::can_extract("weapon.def"));
    EXPECT_TRUE(!opennova::refs::can_extract("texture.tga"));
    EXPECT_TRUE(!opennova::refs::can_extract("noext"));
}

static void test_malformed_kda_fails_soft() {
    // CBIN magic with a truncated header: a recognized format that cannot parse
    // must return false with an error, never crash — the browser pane sweeps
    // every file blindly.
    const std::string bad("CBIN\x01\x02", 6);
    std::vector<Reference> out;
    std::string error;
    EXPECT_TRUE(!opennova::refs::extract("broken.kda",
                                         reinterpret_cast<const uint8_t*>(bad.data()),
                                         bad.size(), out, error));
    EXPECT_TRUE(!error.empty());
    EXPECT_EQ_SZ(out.size(), 0u);
}

int main() {
    test_env_extracts_sky_and_celestial();
    test_env_defaults_alone_still_reference();
    test_credits_fonts_and_images_dedupe();
    test_items_def_field_edges();
    test_unrecognized_is_success_with_no_edges();
    test_can_extract_dispatch();
    test_malformed_kda_fails_soft();
    if (fail_count > 0) {
        std::cerr << fail_count << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "refs_extract_test: all passed" << std::endl;
    return 0;
}
