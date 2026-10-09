// Round-trip + from-scratch writer test for opennova engine/formats/avatars.
//
// The writer creates Avatars.def from scratch (docs/adr/0003, docs/adr/0021);
// it does NOT reproduce the hand-authored retail file byte-for-byte (that file
// carries comment banners and tab-alignment art the editor intentionally
// normalizes). The contract proven here is: (1) the writer is lossless over the
// model and (2) deterministic — parse->write->parse->write yields a
// byte-identical second write. Plus a from-scratch construction case.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include <base/vfs/vfs.h>
#include <formats/avatars/avatars.h>

using namespace opennova::avatars;

#define SETSTR(field, val) std::snprintf((field), sizeof(field), "%s", (val))

namespace {

size_t total_combos(const AvatarsFile &f) {
    size_t n = 0;
    for (size_t i = 0; i < f.nationalities_count; ++i)
        for (size_t j = 0; j < f.nationalities[i].divisions_count; ++j)
            n += f.nationalities[i].divisions[j].combos_count;
    return n;
}

} // namespace

// Lossless + idempotent over a table on disk (the minted synth_avatars.def unconditionally, the
// shipped Avatars.def as the reference-tree leg).
int check_roundtrip(const std::string &path) {
    AvatarsFile m1;
    TEST_EXPECT(avatars_parse(path.c_str(), &m1) == 0);

    char *b1 = nullptr;
    size_t n1 = 0;
    TEST_EXPECT(avatars_write(&m1, &b1, &n1) == 0);
    TEST_EXPECT(b1 != nullptr && n1 > 0);

    AvatarsFile m2;
    TEST_EXPECT(avatars_parse_memory(b1, n1, &m2) == 0);

    // The model survives a write/parse cycle (writer is lossless over the model).
    TEST_EXPECT(m2.parts_count == m1.parts_count);
    TEST_EXPECT(m2.nationalities_count == m1.nationalities_count);
    TEST_EXPECT(total_combos(m2) == total_combos(m1));

    char *b2 = nullptr;
    size_t n2 = 0;
    TEST_EXPECT(avatars_write(&m2, &b2, &n2) == 0);

    // Deterministic: the second write is byte-identical to the first.
    TEST_EXPECT(n2 == n1);
    TEST_EXPECT(std::memcmp(b1, b2, n1) == 0);

    avatars_free(&m1);
    avatars_free(&m2);
    avatars_free_buffer(b1);
    avatars_free_buffer(b2);
    return 0;
}

std::string written(const AvatarsFile &file, const opennova::textlayout::Notes *notes, bool &rewritten) {
    char *data = nullptr;
    size_t size = 0;
    if (avatars_write(&file, notes, &data, &size, &rewritten) != 0) return std::string("<refused>");
    std::string out(data, size);
    avatars_free_buffer(data);
    return out;
}

int lines_changed(const std::string &a, const std::string &b) {
    const auto split = [](const std::string &t) {
        std::vector<std::string> out;
        size_t at = 0;
        while (at < t.size()) {
            const size_t end = t.find("\r\n", at);
            out.push_back(t.substr(at, end == std::string::npos ? std::string::npos : end - at));
            at = end == std::string::npos ? t.size() : end + 2;
        }
        return out;
    };
    const std::vector<std::string> x = split(a), y = split(b);
    if (x.size() != y.size()) return -1;
    int n = 0;
    for (size_t i = 0; i < x.size(); ++i) n += x[i] != y[i];
    return n;
}

// A table read with its layout (textlayout) and written again is the file, byte for byte; one value changed
// changes its one line, and the text reads back as the model.
int check_noted(const std::string &text, const std::string &what) {
    opennova::textlayout::Notes notes;
    AvatarsFile file;
    TEST_EXPECT(avatars_parse_memory(text.data(), text.size(), &file, notes) == 0);
    bool rewritten = true;
    std::string out = written(file, &notes, rewritten);
    TEST_EXPECT(!rewritten && out == text);
    TEST_EXPECT(file.parts_count > 0 && file.nationalities_count > 0);
    file.parts[0].voice = file.parts[0].voice == 3 ? 4 : 3;
    out = written(file, &notes, rewritten);
    TEST_EXPECT(!rewritten);
    const int voice_lines = lines_changed(text, out);
    // A part with no voice line gains one: the line count moves by one instead.
    TEST_EXPECT(voice_lines == 1 || voice_lines == -1);
    AvatarsFile again;
    TEST_EXPECT(avatars_parse_memory(out.data(), out.size(), &again) == 0 && avatars_equal(again, file));
    avatars_free(&again);
    avatars_free(&file);
    std::printf("%s: written again over its layout byte for byte; a voice changed, its one line\n", what.c_str());
    return 0;
}

// An authored table: its comments, its spacing, the `graphic_d` alias, a key the walk reads nothing of, a refused
// nationality and a combo whose head is not defined yet, all where they stood; a combo added after its division's
// last, two combos swapped, a part removed with its lines.
int check_noted_synthetic() {
    const std::string text =
            "// avatars\r\n"
            "define head  H1   // the first head\r\n"
            "{\r\n"
            "  name AV_H1\r\n"
            "  graphic_d  h1.3di\r\n"
            "  frobnicate 3\r\n"
            "  voice 2\r\n"
            "}\r\n"
            "define body B1\r\n"
            "{\r\n"
            "  graphic b1.3di\r\n"
            "}\r\n"
            "// an unused arms\r\n"
            "define arms A1\r\n"
            "{\r\n"
            "}\r\n"
            "nationality N40 AV_BAD\r\n"
            "{\r\n"
            "}\r\n"
            "nationality N00 AV_US skipdemo\r\n"
            "{\r\n"
            "  alignment good\r\n"
            "  division D00 AV_D0\r\n"
            "  {\r\n"
            "    combo 001 H1 B1\r\n"
            "    combo 002 NOHEAD B1\r\n"
            "    combo 003 H1 B1\r\n"
            "  }\r\n"
            "}\r\n";
    opennova::textlayout::Notes notes;
    AvatarsFile file;
    TEST_EXPECT(avatars_parse_memory(text.data(), text.size(), &file, notes) == 0);
    TEST_EXPECT(file.parts_count == 3 && file.nationalities_count == 1 &&
                file.nationalities[0].divisions[0].combos_count == 2);
    bool rewritten = true;
    std::string out = written(file, &notes, rewritten);
    if (out != text) std::fprintf(stderr, "noted synthetic:\n%s\n", out.c_str());
    TEST_EXPECT(!rewritten && out == text);
    // The graphic changed: the alias stays, the word changes.
    std::snprintf(file.parts[0].graphic, sizeof(file.parts[0].graphic), "%s", "h2.3di");
    out = written(file, &notes, rewritten);
    TEST_EXPECT(!rewritten && out.find("  graphic_d  h2.3di\r\n") != std::string::npos && lines_changed(text, out) == 1);
    std::snprintf(file.parts[0].graphic, sizeof(file.parts[0].graphic), "%s", "h1.3di");
    // Two combos swapped: their lines follow, each with the lines read for nothing before it (the refused combo
    // goes with the one after it).
    AvatarDivision &div = file.nationalities[0].divisions[0];
    std::swap(div.combos[0], div.combos[1]);
    out = written(file, &notes, rewritten);
    TEST_EXPECT(!rewritten && out.find("    combo 002 NOHEAD B1\r\n    combo 003 H1 B1\r\n    combo 001 H1 B1\r\n") !=
                std::string::npos);
    std::swap(div.combos[0], div.combos[1]);
    // A combo added: after the division's last, in the writer's form.
    div.combos = static_cast<AvatarCombo *>(std::realloc(div.combos, (div.combos_count + 1) * sizeof(AvatarCombo)));
    div.combos[div.combos_count] = div.combos[0];
    div.combos[div.combos_count].note = 0;
    SETSTR(div.combos[div.combos_count].raw_id, "004");
    div.combos[div.combos_count].id = 4;
    ++div.combos_count;
    out = written(file, &notes, rewritten);
    TEST_EXPECT(!rewritten && out.find("    combo 003 H1 B1\r\n\t\tcombo 004 H1 B1\r\n  }\r\n") != std::string::npos);
    --div.combos_count;
    // A part removed: its lines go, the comment before it stays.
    AvatarsFile fewer = file;
    fewer.parts_count = 2; // H1 and B1, A1 gone
    out = written(fewer, &notes, rewritten);
    TEST_EXPECT(!rewritten && out.find("define arms") == std::string::npos &&
                out.find("// an unused arms\r\nnationality N40 AV_BAD\r\n") != std::string::npos);
    avatars_free(&file);
    std::printf("noted: an authored table written again as it was; a value, a swap, a combo added, a part removed\n");
    return 0;
}

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);
    const std::string root = test_paths_repo_root(__FILE__);
    if (check_roundtrip(root + "/fixtures/avatars/synth_avatars.def") != 0) return 1;

    // --- From-scratch construction (the writer never depends on parsed input) ---
    AvatarPart parts[2];
    std::memset(parts, 0, sizeof(parts));
    parts[0].kind = AVATAR_PART_HEAD;
    SETSTR(parts[0].name, "TEST_HEAD");
    SETSTR(parts[0].display_name, "Test Face"); // contains a space -> writer must quote it
    SETSTR(parts[0].graphic, "test_head.3di");
    parts[0].camo[0] = 1; parts[0].camo[1] = 2; parts[0].camo[2] = 3;
    parts[0].voice = 7;
    parts[0].sex = AVATAR_SEX_FEMALE;

    parts[1].kind = AVATAR_PART_BODY;
    SETSTR(parts[1].name, "TEST_BODY");
    SETSTR(parts[1].display_name, "Test Body");
    SETSTR(parts[1].graphic, "test_body.3di");
    parts[1].sex = AVATAR_SEX_FEMALE;

    AvatarCombo combo;
    std::memset(&combo, 0, sizeof(combo));
    SETSTR(combo.raw_id, "001");
    combo.id = 1;
    SETSTR(combo.head_name, "TEST_HEAD");
    SETSTR(combo.body_name, "TEST_BODY"); // no arms -> arms_name stays empty

    AvatarDivision div;
    std::memset(&div, 0, sizeof(div));
    SETSTR(div.raw_id, "D00");
    div.id = 0;
    SETSTR(div.name_key, "AV_DIV_TEST");
    SETSTR(div.flags, "skipdemo");
    div.combos = &combo;
    div.combos_count = 1;

    AvatarNationality nat;
    std::memset(&nat, 0, sizeof(nat));
    SETSTR(nat.raw_id, "N00");
    nat.id = 0;
    SETSTR(nat.name_key, "AV_NAT_TEST");
    nat.has_alignment = 1;
    nat.alignment = AVATAR_ALIGN_EVIL;
    nat.divisions = &div;
    nat.divisions_count = 1;

    AvatarsFile fs;
    std::memset(&fs, 0, sizeof(fs));
    fs.parts = parts;
    fs.parts_count = 2;
    fs.nationalities = &nat;
    fs.nationalities_count = 1;

    char *fb = nullptr;
    size_t fn = 0;
    TEST_EXPECT(avatars_write(&fs, &fb, &fn) == 0);

    AvatarsFile fp;
    TEST_EXPECT(avatars_parse_memory(fb, fn, &fp) == 0);
    TEST_EXPECT(fp.parts_count == 2);
    TEST_EXPECT(std::strcmp(fp.parts[0].name, "TEST_HEAD") == 0);
    TEST_EXPECT(std::strcmp(fp.parts[0].display_name, "Test Face") == 0); // quote round-trip
    TEST_EXPECT(fp.parts[0].sex == AVATAR_SEX_FEMALE);
    TEST_EXPECT(fp.parts[0].camo[0] == 1 && fp.parts[0].camo[1] == 2 && fp.parts[0].camo[2] == 3);
    TEST_EXPECT(fp.parts[0].voice == 7);
    TEST_EXPECT(fp.nationalities_count == 1);
    TEST_EXPECT(fp.nationalities[0].alignment == AVATAR_ALIGN_EVIL);
    TEST_EXPECT(fp.nationalities[0].divisions_count == 1);
    TEST_EXPECT(std::strcmp(fp.nationalities[0].divisions[0].flags, "skipdemo") == 0);
    TEST_EXPECT(fp.nationalities[0].divisions[0].combos_count == 1);
    const AvatarCombo *rc = &fp.nationalities[0].divisions[0].combos[0];
    TEST_EXPECT(std::strcmp(rc->head_name, "TEST_HEAD") == 0);
    TEST_EXPECT(std::strcmp(rc->body_name, "TEST_BODY") == 0);
    TEST_EXPECT(rc->arms_name[0] == '\0');
    TEST_EXPECT(std::strcmp(rc->body.graphic, "test_body.3di") == 0);

    avatars_free(&fp);
    avatars_free_buffer(fb);

    // An empty field ahead of a filled one is refused, nothing written: the
    // `""` the writer spells it with is no token to the game's tokenizer, so
    // the flags would read back as the name key and the body as the head.
    // [orig: Terrain_TokenizeConfigLine @0x53CB60, the quote arm
    //  @0x53CC4E..0x53CC70]
    {
        char *rb = nullptr;
        size_t rn = 0;
        SETSTR(div.name_key, "");
        TEST_EXPECT(avatars_write(&fs, &rb, &rn) == 2 && rb == nullptr);
        SETSTR(div.flags, "");
        TEST_EXPECT(avatars_write(&fs, &rb, &rn) == 0); // an empty tail is no shift
        avatars_free_buffer(rb);
        rb = nullptr;
        SETSTR(div.name_key, "AV_DIV_TEST");
        SETSTR(combo.head_name, "");
        TEST_EXPECT(avatars_write(&fs, &rb, &rn) == 2 && rb == nullptr);
        SETSTR(combo.head_name, "TEST_HEAD");
        SETSTR(nat.raw_id, "");
        TEST_EXPECT(avatars_write(&fs, &rb, &rn) == 2 && rb == nullptr);
    }
    if (check_noted_synthetic() != 0) return 1;
    // The retail legs: the install's Avatars.def (its base archives and each expansion's mount) written again over
    // its layout byte for byte, and the reference fixture set's.
    const std::string install = retail::install();
    if (install.empty()) {
        retail::skip_leg("OPENNOVA_JO_DIR (the packed install's Avatars.def over its layout)");
    } else {
        std::vector<std::string> mounts{std::string()};
        for (const std::string &expansion : retail::expansions()) mounts.push_back(expansion);
        for (const std::string &expansion : mounts) {
            opennova::Vfs vfs;
            TEST_EXPECT(vfs.mount_game(install, expansion, opennova::VfsMountMode::Packed));
            std::vector<uint8_t> bytes;
            TEST_EXPECT(vfs.read_file("Avatars.def", bytes) && !bytes.empty());
            if (check_noted(std::string(bytes.begin(), bytes.end()), "the install's Avatars.def " + expansion) != 0) return 1;
        }
    }
    const std::string retail = retail::reference_fixture("avatars/Avatars.def");
    if (retail.empty())
        return retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/avatars/Avatars.def (the shipped avatar table)");
    if (check_roundtrip(retail) != 0) return 1;
    std::printf("retail leg: Avatars.def round-trips losslessly and idempotently\n");
    return 0;
}
