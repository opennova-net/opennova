// .mnu extractor: edges against the committed retail fixtures - textures,
// fonts (with %VAR% stylesheet skips), .lwf sound banks (deduped across
// widgets), cross-file menu actions, the screen's string table, string keys,
// table SUBST images, and datasources. Malformed bytes fail soft.
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "common/test_paths.h"
#include "refs/refs.h"

using opennova::refs::Reference;

static int fail_count = 0;

#define EXPECT_TRUE(cond)                                                        \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "FAIL at line " << __LINE__ << ": " #cond << std::endl; \
            fail_count++;                                                        \
        }                                                                        \
    } while (0)

static std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> bytes;
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) return bytes;
    fseek(f, 0, SEEK_END);
    bytes.resize(static_cast<size_t>(ftell(f)));
    fseek(f, 0, SEEK_SET);
    if (!bytes.empty() && fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
    fclose(f);
    return bytes;
}

static std::vector<Reference> extract_fixture(const char* name) {
    const char* repo_root = test_paths_repo_root(__FILE__);
    const std::string path = std::string(repo_root) + "/fixtures/mnu/" + name;
    const std::vector<uint8_t> bytes = read_file(path);
    EXPECT_TRUE(!bytes.empty());
    std::vector<Reference> edges;
    std::string error;
    EXPECT_TRUE(opennova::refs::extract(name, bytes.data(), bytes.size(), edges, error));
    return edges;
}

static int count_kind(const std::vector<Reference>& edges, const std::string& kind) {
    int n = 0;
    for (const Reference& r : edges) {
        if (r.target_kind == kind) n++;
    }
    return n;
}

static const Reference* find_edge(const std::vector<Reference>& edges, const std::string& kind,
                                  const std::string& name) {
    for (const Reference& r : edges) {
        if (r.target_kind == kind && r.target_name == name) return &r;
    }
    return nullptr;
}

static void test_jo_main_edges() {
    const std::vector<Reference> edges = extract_fixture("jo_main.mnu");
    for (const Reference& r : edges) {
        EXPECT_TRUE(r.source_kind == "menu");
    }

    // The screen's string table (lifted root-window TEXT_RSRC), once.
    const Reference* strings = find_edge(edges, "strings", "menutxt.BIN");
    EXPECT_TRUE(strings != nullptr);
    EXPECT_TRUE(count_kind(edges, "strings") == 1);

    // Cross-file screen actions.
    EXPECT_TRUE(find_edge(edges, "menu", "sp.mnu") != nullptr);
    EXPECT_TRUE(find_edge(edges, "menu", "mp.mnu") != nullptr);
    EXPECT_TRUE(find_edge(edges, "menu", "player.mnu") != nullptr);
    EXPECT_TRUE(find_edge(edges, "menu", "options.mnu") != nullptr);
    EXPECT_TRUE(find_edge(edges, "menu", "item_db.mnu") != nullptr);

    // One sound-bank edge despite ~10 SOUND sites (dedupe on kind+name).
    EXPECT_TRUE(find_edge(edges, "sound", "menu.lwf") != nullptr);
    EXPECT_TRUE(count_kind(edges, "sound") == 1);

    // Image appearances + the root cursor.
    EXPECT_TRUE(find_edge(edges, "texture", "Main_hdr.tga") != nullptr);
    EXPECT_TRUE(find_edge(edges, "texture", "Main_ftr.tga") != nullptr);
    EXPECT_TRUE(find_edge(edges, "texture", "newarow1.tga") != nullptr);

    // %DEF_FONTNAME_LG% is a stylesheet variable, never an ASSET edge - it
    // becomes a style_var edge instead; the literal font name elsewhere in the
    // file stays a font edge. Font COLOR slots carry variables too.
    EXPECT_TRUE(find_edge(edges, "font", "%DEF_FONTNAME_LG%") == nullptr);
    EXPECT_TRUE(find_edge(edges, "font", "Arial12b.fnt") != nullptr);
    EXPECT_TRUE(find_edge(edges, "style_var", "DEF_FONTNAME_LG") != nullptr);
    EXPECT_TRUE(find_edge(edges, "style_var", "DEF_TEXT_FG") != nullptr);
    EXPECT_TRUE(find_edge(edges, "style_var", "DEF_TEXT_MOUSEOVER_FG") != nullptr);

    // STRING type="id" keys become string_id edges.
    EXPECT_TRUE(find_edge(edges, "string_id", "MM_Singleplayer") != nullptr);
    EXPECT_TRUE(find_edge(edges, "string_id", "MM_Options") != nullptr);
}

static void test_all_widgets_edges() {
    const std::vector<Reference> edges = extract_fixture("all_widgets.mnu");

    // ITEM type="id" keys from list/combo widgets.
    EXPECT_TRUE(find_edge(edges, "string_id", "MM_Alpha") != nullptr);
    EXPECT_TRUE(find_edge(edges, "string_id", "OPTION_LOW") != nullptr);

    // Table SUBST images and scrollbar part textures.
    EXPECT_TRUE(find_edge(edges, "texture", "ping_lan.tga") != nullptr);
    EXPECT_TRUE(find_edge(edges, "texture", "shuttle.tga") != nullptr);

    // Datasources resolve verbatim.
    const Reference* ds = find_edge(edges, "datasource", "credits.txt");
    EXPECT_TRUE(ds != nullptr);

    // %DEF_FONTNAME% stays a stylesheet variable (a style_var edge, never a font).
    EXPECT_TRUE(find_edge(edges, "font", "%DEF_FONTNAME%") == nullptr);
    EXPECT_TRUE(find_edge(edges, "style_var", "DEF_FONTNAME") != nullptr);
}

static void test_style_variables_never_edge_in_any_field() {
    // The original expands %VAR% over the whole raw buffer BEFORE the XML
    // parse [orig: NapiXML_ExpandVariablesInText @ 0x63a000], so a stylesheet
    // variable is legal in ANY field and is never a literal ASSET reference -
    // not just in appearances/fonts. A menu authored entirely with variables
    // must extract style_var edges only, one per variable, whatever the slot.
    const std::string body =
        "<SCREEN><NAME>VARS</NAME>"
        "<WINDOW type=\"window\" name=\"ROOT\">"
        "<TEXT_RSRC>%TXT%</TEXT_RSRC>"
        "<CURSOR><FILE>%CURSOR_ART%</FILE></CURSOR>"
        "<APPEARANCE type=\"image\" state=\"default\">%BG_IMG%</APPEARANCE>"
        "<FRAME><STENCIL>%MENU_FRAME%</STENCIL><BRUSH>%MENU_BRUSH%</BRUSH></FRAME>"
        "<DATASOURCE>%DS%</DATASOURCE>"
        "<WINDOW type=\"button\" name=\"GO\">"
        "<ACTION type=\"screen\" file=\"%NEXT_MENU%\">SUB</ACTION>"
        "<SOUND state=\"selected\" trigger=\"CLICK_SELECT\">%SND_BANK%</SOUND>"
        "<STRING type=\"id\" justify=\"LEFT\">%BTN_KEY%</STRING>"
        "<FONT><NAME>%DEF_FONTNAME%</NAME></FONT>"
        "</WINDOW></WINDOW></SCREEN>";
    std::vector<Reference> edges;
    std::string error;
    EXPECT_TRUE(opennova::refs::extract(
        "vars.mnu", reinterpret_cast<const uint8_t*>(body.data()), body.size(), edges, error));
    for (const Reference& r : edges) {
        if (r.target_kind != "style_var") {
            std::cerr << "phantom asset edge: " << r.target_kind << "|" << r.target_name << std::endl;
        }
        EXPECT_TRUE(r.target_kind == "style_var");
    }
    const char* expected[] = {"TXT", "CURSOR_ART", "BG_IMG", "MENU_FRAME", "MENU_BRUSH",
                              "DS", "NEXT_MENU", "SND_BANK", "BTN_KEY", "DEF_FONTNAME"};
    for (const char* name : expected) {
        EXPECT_TRUE(find_edge(edges, "style_var", name) != nullptr);
    }
    EXPECT_TRUE(count_kind(edges, "style_var") == 10);
}

static void test_malformed_mnu_fails_soft() {
    // The parser is lenient (malformed-XML fixups), so use bytes that cannot
    // parse at all; either a clean failure with an error, or zero edges - never
    // a crash or phantom edges.
    const uint8_t junk[] = {0x00, 0xFF, 0x3C, 0x00, 0x01};
    std::vector<Reference> edges;
    std::string error;
    const bool ok = opennova::refs::extract("broken.mnu", junk, sizeof(junk), edges, error);
    if (!ok) {
        EXPECT_TRUE(!error.empty());
    }
    EXPECT_TRUE(edges.empty());
}

int main() {
    test_jo_main_edges();
    test_all_widgets_edges();
    test_style_variables_never_edge_in_any_field();
    test_malformed_mnu_fails_soft();
    if (fail_count > 0) {
        std::cerr << fail_count << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "refs_mnu_test: all passed" << std::endl;
    return 0;
}
