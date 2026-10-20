// Round-trip + from-scratch writer test for opennova engine/formats/avatars.
//
// The writer creates Avatars.def from scratch (docs/adr/0003, docs/adr/0021);
// it does NOT reproduce the hand-authored retail file byte-for-byte (that file
// carries comment banners and tab-alignment art the editor intentionally
// normalizes). The contract proven here is: (1) the writer is lossless over the
// model and (2) deterministic — parse->write->parse->write yields a
// byte-identical second write. Plus a from-scratch construction case, and the
// order the game binds a combo's parts by (each nationality written where it
// stood among the parts, D-PLAYERINFO-35).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "common/test_expect.h"
#include "common/test_paths.h"
#include <base/io/hash.h>
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

// Lossless + idempotent over a table on disk (the minted synth_avatars.def and the authored JO-shaped
// jo_shaped.def).
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

std::string written(const AvatarsFile &file) {
    char *data = nullptr;
    size_t size = 0;
    if (avatars_write(&file, &data, &size) != 0) return std::string("<refused>");
    std::string out(data, size);
    avatars_free_buffer(data);
    return out;
}

// Whether a text reads back as the model it was written from.
bool reads_back(const std::string &text, const AvatarsFile &file) {
    AvatarsFile again;
    const bool same = avatars_parse_memory(text.data(), text.size(), &again) == 0 && avatars_equal(again, file);
    avatars_free(&again);
    return same;
}

// A table in the shipped one's shape read through the game's reader, written from its model and read again: the
// same fields; one value changed reads back changed.
int check_written(const std::string &text, const std::string &what) {
    AvatarsFile file;
    TEST_EXPECT(avatars_parse_memory(text.data(), text.size(), &file) == 0);
    TEST_EXPECT(file.parts_count > 0 && file.nationalities_count > 0);
    TEST_EXPECT(reads_back(written(file), file));
    file.parts[0].voice = file.parts[0].voice == 3 ? 4 : 3;
    TEST_EXPECT(reads_back(written(file), file));
    avatars_free(&file);
    std::printf("%s: written from its model, read back as its fields; a voice changed reads back\n", what.c_str());
    return 0;
}

// fixtures/avatars/jo_shaped.def: an authored table in the shipped one's shape (its banners, a comment after a
// header and after a value, quoted names, N00 / D00 / 001 ids, the trailing "skipdemo", parts below the good
// nationalities and the evil one under them).
std::string jo_shaped_path() { return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/avatars/jo_shaped.def"; }

std::string jo_shaped_text() {
    std::string text;
    if (std::FILE *file = std::fopen(jo_shaped_path().c_str(), "rb")) {
        char buffer[4096];
        size_t got = 0;
        while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, got);
        std::fclose(file);
    }
    return text;
}

// The writer's form of the JO-shaped table, pinned (the nw_codec_identity rule: a vector changes only with an
// [orig] witness on the writer or a D-entry; `avatars_roundtrip_test --dump` prints it).
constexpr size_t kJoShapedWrittenBytes = 3415;
constexpr uint64_t kJoShapedWrittenFnv = UINT64_C(0x5bab546e2e04bf75);

uint64_t fnv1a64(const std::string &text) {
    return opennova::io::fnv1a64_bytes(opennova::io::kFnv1a64Offset, text.data(), text.size());
}

int dump() {
    const std::string text = jo_shaped_text();
    AvatarsFile file;
    if (avatars_parse_memory(text.data(), text.size(), &file) != 0) return 1;
    const std::string out = written(file);
    std::printf("constexpr size_t kJoShapedWrittenBytes = %zu;\nconstexpr uint64_t kJoShapedWrittenFnv = UINT64_C(0x%016llx);\n",
                out.size(), static_cast<unsigned long long>(fnv1a64(out)));
    avatars_free(&file);
    return 0;
}

int check_jo_shaped_written() {
    const std::string text = jo_shaped_text();
    TEST_EXPECT(!text.empty());
    AvatarsFile file;
    TEST_EXPECT(avatars_parse_memory(text.data(), text.size(), &file) == 0);
    const std::string out = written(file);
    if (out.size() != kJoShapedWrittenBytes || fnv1a64(out) != kJoShapedWrittenFnv)
        std::printf("FAIL jo_shaped.def: the writer's form is %zu bytes, fnv 0x%016llx\n", out.size(),
                    static_cast<unsigned long long>(fnv1a64(out)));
    TEST_EXPECT(out.size() == kJoShapedWrittenBytes && fnv1a64(out) == kJoShapedWrittenFnv);
    avatars_free(&file);
    return 0;
}

// An authored table (its comments, its spacing, the `graphic_d` alias, a key the walk reads nothing of, a refused
// nationality, a trailing token after a name key, ids spelled with a letter or leading zeros, and a combo whose
// head is not defined yet): written from its model in the writer's form, none of what the walk reads nothing of,
// every id its decimal, and read back as the model; edits (a value, a swap, a combo added, a part removed) read
// back too (the #1098 review, F2: the id token's spelling and the trailing tokens are not kept).
int check_authored() {
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
    AvatarsFile file;
    TEST_EXPECT(avatars_parse_memory(text.data(), text.size(), &file) == 0);
    TEST_EXPECT(file.parts_count == 3 && file.nationalities_count == 1 &&
                file.nationalities[0].divisions[0].combos_count == 2);
    const std::string out = written(file);
    TEST_EXPECT(out.find("frobnicate") == std::string::npos && out.find("// the first head") == std::string::npos &&
                out.find("AV_BAD") == std::string::npos && out.find("NOHEAD") == std::string::npos &&
                out.find("\tgraphic\t\th1.3di\r\n") != std::string::npos);
    TEST_EXPECT(out.find("skipdemo") == std::string::npos && out.find("N00") == std::string::npos &&
                out.find("D00") == std::string::npos && out.find("\nnationality 0 AV_US\r\n") != std::string::npos &&
                out.find("\tdivision 0 AV_D0\r\n") != std::string::npos &&
                out.find("\t\tcombo 1 H1 B1\r\n\t\tcombo 3 H1 B1\r\n") != std::string::npos);
    TEST_EXPECT(reads_back(out, file));
    // The graphic changed.
    std::snprintf(file.parts[0].graphic, sizeof(file.parts[0].graphic), "%s", "h2.3di");
    TEST_EXPECT(reads_back(written(file), file) && written(file).find("\tgraphic\t\th2.3di\r\n") != std::string::npos);
    // Two combos swapped.
    AvatarDivision &div = file.nationalities[0].divisions[0];
    std::swap(div.combos[0], div.combos[1]);
    TEST_EXPECT(reads_back(written(file), file) &&
                written(file).find("\t\tcombo 3 H1 B1\r\n\t\tcombo 1 H1 B1\r\n") != std::string::npos);
    std::swap(div.combos[0], div.combos[1]);
    // A combo added after the division's last.
    div.combos = static_cast<AvatarCombo *>(std::realloc(div.combos, (div.combos_count + 1) * sizeof(AvatarCombo)));
    div.combos[div.combos_count] = div.combos[0];
    div.combos[div.combos_count].id = 4;
    ++div.combos_count;
    TEST_EXPECT(reads_back(written(file), file) &&
                written(file).find("\t\tcombo 3 H1 B1\r\n\t\tcombo 4 H1 B1\r\n\t}\r\n") != std::string::npos);
    --div.combos_count;
    // A part removed.
    AvatarsFile fewer = file;
    fewer.parts_count = 2; // H1 and B1, A1 gone
    TEST_EXPECT(reads_back(written(fewer), fewer) && written(fewer).find("define arms") == std::string::npos);
    avatars_free(&file);
    std::printf("authored: an authored table written in the writer's form, read back as its model; a value, a swap, "
                "a combo added, a part removed\n");
    return 0;
}

// The order the game binds by (D-PLAYERINFO-35): a combo binds the last part of its name and kind defined above it
// [orig: CAvatarDefs_ParseConfigLine @ 0x57a830..0x57a8d3, each scan up to g_AvatarPartCount], so a combo between two
// heads of one name binds the earlier and one after both the later. The reader keeps where each nationality stands
// among the parts (parts_below), the writer puts it there, and the text read again binds every combo as the file did;
// a model whose nationalities stand higher than the one before them is no file's order, refused.
int check_order() {
    const std::string text =
            "define head HEAD_A\r\n{\r\n\tgraphic\ta1.3di\r\n}\r\n"
            "define body BODY_A\r\n{\r\n\tgraphic\tb.3di\r\n}\r\n"
            "nationality 0 AV_US\r\n{\r\n\tdivision 0 AV_D0\r\n\t{\r\n\t\tcombo 1 HEAD_A BODY_A\r\n\t}\r\n}\r\n"
            "define head HEAD_A\r\n{\r\n\tgraphic\ta2.3di\r\n}\r\n"
            "nationality 1 AV_UK\r\n{\r\n\tdivision 0 AV_D0\r\n\t{\r\n\t\tcombo 1 HEAD_A BODY_A\r\n\t}\r\n}\r\n"
            "define arms ARMS_A\r\n{\r\n\tgraphic\tarms.3di\r\n}\r\n";
    const auto bound = [](const AvatarsFile &f, size_t nationality) {
        return std::string(f.nationalities[nationality].divisions[0].combos[0].head.graphic);
    };
    AvatarsFile file;
    TEST_EXPECT(avatars_parse_memory(text.data(), text.size(), &file) == 0);
    TEST_EXPECT(file.parts_count == 4 && file.nationalities_count == 2);
    TEST_EXPECT(file.nationalities[0].parts_below == 2 && file.nationalities[1].parts_below == 1);
    TEST_EXPECT(bound(file, 0) == "a1.3di" && bound(file, 1) == "a2.3di");
    const std::string out = written(file);
    const size_t us = out.find("nationality 0"), uk = out.find("nationality 1"), second = out.find("a2.3di"),
                 arms = out.find("define arms");
    TEST_EXPECT(us != std::string::npos && us < second && second < uk && uk < arms && arms != std::string::npos);
    AvatarsFile again;
    TEST_EXPECT(avatars_parse_memory(out.data(), out.size(), &again) == 0 && avatars_equal(again, file));
    TEST_EXPECT(bound(again, 0) == "a1.3di" && bound(again, 1) == "a2.3di");
    TEST_EXPECT(written(again) == out);
    avatars_free(&again);
    // Every part above both (a model built in code): each combo binds the later head.
    file.nationalities[0].parts_below = file.nationalities[1].parts_below = 0;
    const std::string first = written(file);
    TEST_EXPECT(first.find("define arms") < first.find("nationality 0"));
    TEST_EXPECT(avatars_parse_memory(first.data(), first.size(), &again) == 0 && bound(again, 0) == "a2.3di");
    avatars_free(&again);
    // No file's order: a nationality higher than the one before it, or below more parts than there are.
    file.nationalities[1].parts_below = 1;
    TEST_EXPECT(written(file) == "<refused>");
    file.nationalities[0].parts_below = file.nationalities[1].parts_below = 5;
    TEST_EXPECT(written(file) == "<refused>");
    avatars_free(&file);
    std::printf("order: each nationality written below the parts above it, every combo binding as the file did\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--dump") == 0) return dump();
    const std::string root = test_paths_repo_root(__FILE__);
    if (check_roundtrip(root + "/fixtures/avatars/synth_avatars.def") != 0) return 1;
    if (check_order() != 0) return 1;

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
    combo.id = 1;
    SETSTR(combo.head_name, "TEST_HEAD");
    SETSTR(combo.body_name, "TEST_BODY"); // no arms -> arms_name stays empty

    AvatarDivision div;
    std::memset(&div, 0, sizeof(div));
    div.id = 0;
    SETSTR(div.name_key, "AV_DIV_TEST");
    div.combos = &combo;
    div.combos_count = 1;

    AvatarNationality nat;
    std::memset(&nat, 0, sizeof(nat));
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
    TEST_EXPECT(fp.nationalities[0].divisions[0].combos_count == 1);
    const AvatarCombo *rc = &fp.nationalities[0].divisions[0].combos[0];
    TEST_EXPECT(std::strcmp(rc->head_name, "TEST_HEAD") == 0);
    TEST_EXPECT(std::strcmp(rc->body_name, "TEST_BODY") == 0);
    TEST_EXPECT(rc->arms_name[0] == '\0');
    TEST_EXPECT(std::strcmp(rc->body.graphic, "test_body.3di") == 0);

    avatars_free(&fp);
    avatars_free_buffer(fb);

    // An empty name ahead of a filled one is refused, nothing written: the
    // `""` the writer spells it with is no token to the game's tokenizer, so
    // the body would read back as the head. An empty name key ends its line:
    // no later field shifts. [orig: Config_TokenizeLine @0x53CB60, the quote
    // arm @0x53CC4E..0x53CC70]
    {
        char *rb = nullptr;
        size_t rn = 0;
        SETSTR(div.name_key, "");
        TEST_EXPECT(avatars_write(&fs, &rb, &rn) == 0); // an empty tail is no shift
        avatars_free_buffer(rb);
        rb = nullptr;
        SETSTR(div.name_key, "AV_DIV_TEST");
        SETSTR(combo.head_name, "");
        TEST_EXPECT(avatars_write(&fs, &rb, &rn) == 2 && rb == nullptr);
        SETSTR(combo.head_name, "TEST_HEAD");
    }
    if (check_authored() != 0) return 1;
    // The JO-shaped table: written from its model and read back as its fields, lossless and idempotent, its
    // writer's form pinned.
    if (check_written(jo_shaped_text(), "jo_shaped.def") != 0) return 1;
    if (check_roundtrip(jo_shaped_path()) != 0) return 1;
    if (check_jo_shaped_written() != 0) return 1;
    std::printf("JO-shaped table: round-trips losslessly and idempotently, its written form pinned\n");
    return 0;
}
