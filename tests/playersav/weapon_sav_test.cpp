// libs/playersav — weapon.sav player-profile parse/serialize tests.
//
// Every fixture is built in code: no retail file is committed. The optional
// last leg reads the maintainer's real weapon.sav when OPENNOVA_WEAPON_SAV
// points at one, and SKIPS AS A PASS otherwise (docs/asset-gated-tests.md).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "common/test_expect.h"
#include "playersav/weapon_sav.h"

using namespace opennova::playersav;

namespace {

std::vector<uint8_t> page_bytes(const std::string &blob, size_t size = kKitPageBytes)
{
    std::vector<uint8_t> out(size, 0);
    const size_t n = blob.size() < size ? blob.size() : size;
    std::memcpy(out.data(), blob.data(), n);
    return out;
}

// A NUL-joined token blob: "a\0b\0c\0" (each token NUL-terminated).
std::string tokens(const std::vector<std::string> &list)
{
    std::string out;
    for (const std::string &t : list) {
        out += t;
        out.push_back('\0');
    }
    return out;
}

bool pages_equal(const KitPage &a, const KitPage &b)
{
    if (a.entries.size() != b.entries.size())
        return false;
    for (size_t i = 0; i < a.entries.size(); ++i) {
        const KitEntry &x = a.entries[i];
        const KitEntry &y = b.entries[i];
        if (x.name != y.name || x.ammo_primary != y.ammo_primary ||
            x.ammo_secondary != y.ammo_secondary || x.flags != y.flags)
            return false;
    }
    return true;
}

bool sides_equal(const Side &a, const Side &b)
{
    if (a.player_class != b.player_class || a.avatar_a != b.avatar_a ||
        a.avatar_b != b.avatar_b || a.avatar_packed != b.avatar_packed)
        return false;
    for (size_t i = 0; i < kKitPagesPerSide; ++i)
        if (!pages_equal(a.pages[i], b.pages[i]))
            return false;
    return true;
}

bool files_equal(const File &a, const File &b)
{
    if (a.flags != b.flags || a.extra != b.extra)
        return false;
    for (size_t s = 0; s < kProfileSlots; ++s) {
        if (!sides_equal(a.slots[s].blue, b.slots[s].blue))
            return false;
        if (!sides_equal(a.slots[s].red, b.slots[s].red))
            return false;
        if (!pages_equal(a.slots[s].single_player, b.slots[s].single_player))
            return false;
    }
    return true;
}

bool is_single_default(const KitPage &page, const char *name)
{
    if (page.entries.size() != 1)
        return false;
    const KitEntry &e = page.entries[0];
    return e.name == name && e.ammo_primary == -1 && e.ammo_secondary == -1 &&
           e.flags == -1;
}

// Read a NUL-terminated token out of a serialized buffer.
std::string token_at(const std::vector<uint8_t> &buf, size_t offset)
{
    std::string out;
    for (size_t i = offset; i < buf.size() && buf[i] != 0; ++i)
        out.push_back(static_cast<char>(buf[i]));
    return out;
}

// --- 1. make_defaults matches PlayerProfile_InitDefaults @0x54bb40 ----------

int test_make_defaults()
{
    const File f = make_defaults();

    const char *blue[kKitPagesPerSide] = {"WPN_M4AUTO", "WPN_SR25", "WPN_M60",
                                          "WPN_M4AUTO", "WPN_M16BURST"};
    const char *red[kKitPagesPerSide] = {"WPN_AK47AUTO", "WPN_DRAGUNOV", "WPN_PKM",
                                         "WPN_AK47AUTO", "WPN_AK74AUTO"};

    for (size_t s = 0; s < kProfileSlots; ++s) {
        const Record &rec = f.slots[s];
        TEST_EXPECT(rec.blue.player_class == 8);
        TEST_EXPECT(rec.red.player_class == 8);
        for (size_t i = 0; i < kKitPagesPerSide; ++i) {
            TEST_EXPECT(is_single_default(rec.blue.pages[i], blue[i]));
            TEST_EXPECT(is_single_default(rec.red.pages[i], red[i]));
        }
        TEST_EXPECT(is_single_default(rec.single_player, "WPN_M4AUTO"));

        // The class byte selects the page: rifleman (8) -> index 3.
        const KitPage *sel = rec.blue.selected_page();
        TEST_EXPECT(sel != nullptr);
        TEST_EXPECT(is_single_default(*sel, "WPN_M4AUTO"));
        TEST_EXPECT(rec.blue.page_for_class(6) != nullptr);
        TEST_EXPECT(is_single_default(*rec.blue.page_for_class(6), "WPN_SR25"));
        TEST_EXPECT(rec.blue.page_for_class(4) == nullptr);
        TEST_EXPECT(rec.blue.page_for_class(10) == nullptr);
        TEST_EXPECT(&rec.side(SideId::Blue) == &rec.blue);
        TEST_EXPECT(&rec.side(SideId::Red) == &rec.red);
    }
    return 0;
}

// --- 2. write/read round-trip and on-disk offsets ---------------------------

int test_write_layout_and_roundtrip()
{
    File f = make_defaults();
    f.flags = 0x11223344u;
    f.extra = 0x55667788u;
    f.slots[2].blue.player_class = 6;
    f.slots[2].blue.avatar_a = 7;
    f.slots[2].blue.avatar_b = 3;
    f.slots[2].blue.avatar_packed = 0x8207;
    f.slots[4].red.pages[1].entries.push_back(KitEntry{"WPN_KNIFE2", 12, -1, 3});

    const std::vector<uint8_t> buf = write(f);
    TEST_EXPECT(buf.size() == kHeaderBytes + kProfileSlots * kRecordBytes);
    TEST_EXPECT(buf.size() == 337996);

    // Header: "FPBC" "0211" then flags/extra.
    const char magic[8] = {'F', 'P', 'B', 'C', '0', '2', '1', '1'};
    TEST_EXPECT(std::memcmp(buf.data(), magic, 8) == 0);
    TEST_EXPECT(buf[8] == 0x44 && buf[9] == 0x33 && buf[10] == 0x22 && buf[11] == 0x11);
    TEST_EXPECT(buf[12] == 0x88 && buf[13] == 0x77 && buf[14] == 0x66 && buf[15] == 0x55);

    // Slot 0 blue block: header bytes then the five class pages at
    // +6 / +2054 / +4102 / +6150 / +8198.
    const size_t slot0 = kHeaderBytes;
    TEST_EXPECT(buf[slot0 + 0] == 8);
    TEST_EXPECT(token_at(buf, slot0 + 6) == "WPN_M4AUTO");
    TEST_EXPECT(token_at(buf, slot0 + 2054) == "WPN_SR25");
    TEST_EXPECT(token_at(buf, slot0 + 4102) == "WPN_M60");
    TEST_EXPECT(token_at(buf, slot0 + 6150) == "WPN_M4AUTO");
    TEST_EXPECT(token_at(buf, slot0 + 8198) == "WPN_M16BURST");
    // The red block starts at +32774 and repeats the page ladder.
    TEST_EXPECT(buf[slot0 + kSideBytes] == 8);
    TEST_EXPECT(token_at(buf, slot0 + kSideBytes + 6) == "WPN_AK47AUTO");
    TEST_EXPECT(token_at(buf, slot0 + kSideBytes + 2054) == "WPN_DRAGUNOV");
    TEST_EXPECT(token_at(buf, slot0 + kSideBytes + 8198) == "WPN_AK74AUTO");
    // The single-player page at +65548.
    TEST_EXPECT(kSinglePlayerPageOffset == 65548);
    TEST_EXPECT(token_at(buf, slot0 + kSinglePlayerPageOffset) == "WPN_M4AUTO");

    // The per-side unwitnessed tail is written as zeros.
    for (size_t i = kFirstKitPageOffset + kKitPagesPerSide * kKitPageBytes;
         i < kSideBytes; ++i)
        TEST_EXPECT(buf[slot0 + i] == 0);

    // Slot 2's edited blue block.
    const size_t slot2 = kHeaderBytes + 2 * kRecordBytes;
    TEST_EXPECT(buf[slot2 + 0] == 6);
    TEST_EXPECT(buf[slot2 + 1] == 7);
    TEST_EXPECT(buf[slot2 + 2] == 3);
    TEST_EXPECT(buf[slot2 + 3] == 0);  // unwitnessed pad byte
    TEST_EXPECT(buf[slot2 + 4] == 0x07 && buf[slot2 + 5] == 0x82);

    File back;
    TEST_EXPECT(read(buf.data(), buf.size(), back));
    TEST_EXPECT(files_equal(f, back));

    // Deterministic: a second write of the parsed file is byte-identical.
    TEST_EXPECT(write(back) == buf);
    return 0;
}

// --- 3. header rejection ----------------------------------------------------

int test_header_rejection()
{
    const std::vector<uint8_t> good = write(make_defaults());

    std::vector<uint8_t> bad_magic = good;
    bad_magic[0] = 'X';
    File out;
    TEST_EXPECT(!read(bad_magic.data(), bad_magic.size(), out));

    std::vector<uint8_t> bad_version = good;
    bad_version[4] = '9';
    TEST_EXPECT(!read(bad_version.data(), bad_version.size(), out));

    // Short buffers.
    TEST_EXPECT(!read(good.data(), 8, out));
    TEST_EXPECT(!read(good.data(), kHeaderBytes + kRecordBytes, out));
    TEST_EXPECT(!read(nullptr, 0, out));

    // A header-only file is tolerated and yields the struct defaults.
    TEST_EXPECT(read(good.data(), kHeaderBytes, out));
    for (size_t s = 0; s < kProfileSlots; ++s) {
        TEST_EXPECT(out.slots[s].blue.player_class == 8);
        TEST_EXPECT(out.slots[s].blue.pages[0].entries.empty());
        TEST_EXPECT(out.slots[s].single_player.entries.empty());
    }
    return 0;
}

// --- 4. decode_kit_page shapes ---------------------------------------------

int test_decode_kit_page()
{
    // A full 7-entry page, the retail shape.
    const char *names[7] = {"WPN_M4AUTO",   "WPN_colt45",    "WPN_SATCHEL_CHARGE",
                            "WPN_GRENADEFB", "WPN_GRENADEHE", "WPN_GRENADESM",
                            "WPN_KNIFE"};
    std::vector<std::string> toks;
    for (const char *n : names) {
        toks.push_back(n);
        toks.push_back("-1");
        toks.push_back("-1");
        toks.push_back("-1");
    }
    std::string blob = tokens(toks);
    blob.push_back('\0');  // the terminating empty string
    std::vector<uint8_t> raw = page_bytes(blob);
    KitPage page = decode_kit_page(raw.data(), raw.size());
    TEST_EXPECT(page.entries.size() == 7);
    for (size_t i = 0; i < 7; ++i) {
        TEST_EXPECT(page.entries[i].name == names[i]);
        TEST_EXPECT(page.entries[i].ammo_primary == -1);
        TEST_EXPECT(page.entries[i].ammo_secondary == -1);
        TEST_EXPECT(page.entries[i].flags == -1);
    }

    // A single bare name with no values — the PlayerProfile_InitDefaults shape.
    raw = page_bytes(std::string("WPN_M4AUTO\0\0", 12));
    page = decode_kit_page(raw.data(), raw.size());
    TEST_EXPECT(page.entries.size() == 1);
    TEST_EXPECT(page.entries[0].name == "WPN_M4AUTO");
    TEST_EXPECT(page.entries[0].ammo_primary == -1);
    TEST_EXPECT(page.entries[0].ammo_secondary == -1);
    TEST_EXPECT(page.entries[0].flags == -1);

    // An empty page (leading double NUL / all zeros).
    raw = page_bytes(std::string());
    page = decode_kit_page(raw.data(), raw.size());
    TEST_EXPECT(page.entries.empty());

    // A page with no double NUL that runs to the 2048-byte end: fill exactly
    // 2048 bytes with NUL-terminated groups and no terminator.
    {
        std::string filled;
        size_t n = 0;
        while (true) {
            const std::string group = tokens({"WPN_AK47AUTO", "1", "2", "3"});
            if (filled.size() + group.size() > kKitPageBytes)
                break;
            filled += group;
            ++n;
        }
        // Pad the remainder with a final unterminated token so no NUL closes it.
        const size_t pad = kKitPageBytes - filled.size();
        std::string tail(pad, 'Z');
        filled += tail;
        TEST_EXPECT(filled.size() == kKitPageBytes);
        raw = std::vector<uint8_t>(filled.begin(), filled.end());
        page = decode_kit_page(raw.data(), raw.size());
        // n full groups plus the trailing unterminated token as a bare name.
        TEST_EXPECT(page.entries.size() == n + 1);
        TEST_EXPECT(page.entries[0].name == "WPN_AK47AUTO");
        TEST_EXPECT(page.entries[0].ammo_primary == 1);
        TEST_EXPECT(page.entries[0].ammo_secondary == 2);
        TEST_EXPECT(page.entries[0].flags == 3);
        TEST_EXPECT(page.entries[n].name == tail);
        TEST_EXPECT(page.entries[n].ammo_primary == -1);
    }

    // A trailing partial group (name + one value) keeps the entry and defaults
    // the rest; a non-numeric value reads as -1.
    {
        std::string b = tokens({"WPN_M60", "abc", "-1", "-1", "WPN_KNIFE", "5"});
        b.push_back('\0');
        raw = page_bytes(b);
        page = decode_kit_page(raw.data(), raw.size());
        TEST_EXPECT(page.entries.size() == 2);
        TEST_EXPECT(page.entries[0].ammo_primary == -1);  // "abc" -> -1
        TEST_EXPECT(page.entries[1].name == "WPN_KNIFE");
        TEST_EXPECT(page.entries[1].ammo_primary == 5);
        TEST_EXPECT(page.entries[1].ammo_secondary == -1);
        TEST_EXPECT(page.entries[1].flags == -1);
    }
    return 0;
}

// --- 5. side_for_team -------------------------------------------------------

int test_side_for_team()
{
    TEST_EXPECT(side_for_team(1) == SideId::Blue);
    TEST_EXPECT(side_for_team(3) == SideId::Blue);
    TEST_EXPECT(side_for_team(2) == SideId::Red);
    TEST_EXPECT(side_for_team(4) == SideId::Red);
    TEST_EXPECT(side_for_team(0) == SideId::Red);
    TEST_EXPECT(side_for_team(9) == SideId::Red);
    return 0;
}

// --- 6. clamp_classes -------------------------------------------------------

int test_clamp_classes()
{
    File f = make_defaults();
    f.slots[0].blue.player_class = 0;
    f.slots[0].red.player_class = 4;
    f.slots[1].blue.player_class = 10;
    f.slots[1].red.player_class = 255;
    f.slots[2].blue.player_class = 5;
    f.slots[2].red.player_class = 9;
    f.slots[3].blue.player_class = 7;
    f.slots[3].red.player_class = 200;

    clamp_classes(f);

    TEST_EXPECT(f.slots[0].blue.player_class == 5);
    TEST_EXPECT(f.slots[0].red.player_class == 5);
    TEST_EXPECT(f.slots[1].blue.player_class == 9);
    TEST_EXPECT(f.slots[1].red.player_class == 9);
    TEST_EXPECT(f.slots[2].blue.player_class == 5);
    TEST_EXPECT(f.slots[2].red.player_class == 9);
    TEST_EXPECT(f.slots[3].blue.player_class == 7);  // in range, untouched
    TEST_EXPECT(f.slots[3].red.player_class == 9);
    return 0;
}

// --- 7. encode -> decode -> encode is byte-stable ---------------------------

int test_page_codec_roundtrip()
{
    KitPage page;
    page.entries.push_back(KitEntry{"WPN_SR25", -1, -1, -1});
    page.entries.push_back(KitEntry{"WPN_M9Beretta", 0, 12, 255});
    page.entries.push_back(KitEntry{"WPN_CLAYMORE", 3, -1, 7});
    page.entries.push_back(KitEntry{"WPN_GRENADEFB", -1, -1, -1});
    page.entries.push_back(KitEntry{"WPN_KNIFE", 1, 2, 3});

    std::vector<uint8_t> a(kKitPageBytes, 0xAB);
    encode_kit_page(page, a.data(), a.size());

    const KitPage decoded = decode_kit_page(a.data(), a.size());
    TEST_EXPECT(pages_equal(page, decoded));

    std::vector<uint8_t> b(kKitPageBytes, 0xCD);
    encode_kit_page(decoded, b.data(), b.size());
    TEST_EXPECT(a == b);

    // Shape check: NUL-separated tokens then a terminating NUL, zero fill after.
    TEST_EXPECT(token_at(a, 0) == "WPN_SR25");
    const std::string expect_head = tokens({"WPN_SR25", "-1", "-1", "-1"});
    TEST_EXPECT(std::memcmp(a.data(), expect_head.data(), expect_head.size()) == 0);
    size_t used = 0;
    for (const KitEntry &e : page.entries) {
        used += e.name.size() + 1;
        used += std::to_string(e.ammo_primary).size() + 1;
        used += std::to_string(e.ammo_secondary).size() + 1;
        used += std::to_string(e.flags).size() + 1;
    }
    for (size_t i = used; i < a.size(); ++i)
        TEST_EXPECT(a[i] == 0);

    // An entry with an empty name is skipped; overflow truncates cleanly and
    // still leaves a terminated blob.
    KitPage over;
    for (int i = 0; i < 400; ++i)
        over.entries.push_back(KitEntry{"WPN_A_RATHER_LONG_WEAPON_NAME", -1, -1, -1});
    over.entries.push_back(KitEntry{"", 1, 2, 3});
    std::vector<uint8_t> c(kKitPageBytes, 0xFF);
    encode_kit_page(over, c.data(), c.size());
    TEST_EXPECT(c[c.size() - 1] == 0);
    const KitPage over_back = decode_kit_page(c.data(), c.size());
    TEST_EXPECT(!over_back.entries.empty());
    TEST_EXPECT(over_back.entries.size() < over.entries.size());
    for (const KitEntry &e : over_back.entries)
        TEST_EXPECT(e.name == "WPN_A_RATHER_LONG_WEAPON_NAME");
    return 0;
}

// --- 8. optional: the maintainer's real weapon.sav (skip-as-pass) -----------

int test_retail_file()
{
    const char *path = std::getenv("OPENNOVA_WEAPON_SAV");
    if (path == nullptr || path[0] == '\0') {
        std::printf(
            "SKIP: set OPENNOVA_WEAPON_SAV to a retail weapon.sav to run the "
            "corpus leg\n");
        return 0;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "OPENNOVA_WEAPON_SAV set but unreadable: %s\n", path);
        return 1;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());

    File f;
    TEST_EXPECT(read(bytes.data(), bytes.size(), f));

    // Every side's class byte is in range and every selected page resolves.
    for (size_t s = 0; s < kProfileSlots; ++s) {
        for (SideId id : {SideId::Blue, SideId::Red}) {
            const Side &side = f.slots[s].side(id);
            TEST_EXPECT(side.player_class >= kMinPlayerClass);
            TEST_EXPECT(side.player_class <= kMaxPlayerClass);
            TEST_EXPECT(side.selected_page() != nullptr);
            TEST_EXPECT(!side.selected_page()->entries.empty());
        }
    }

    // Writer parity: a retail-SAVED profile re-serializes byte for byte.
    const std::vector<uint8_t> rewritten = write(f);
    TEST_EXPECT(rewritten.size() == bytes.size());
    TEST_EXPECT(rewritten == bytes);
    std::printf("OPENNOVA_WEAPON_SAV: %zu bytes re-serialized byte-identically\n",
                bytes.size());
    return 0;
}

}  // namespace

int main()
{
    struct Case {
        const char *name;
        int (*fn)();
    };
    const Case cases[] = {
        {"make_defaults", test_make_defaults},
        {"write_layout_and_roundtrip", test_write_layout_and_roundtrip},
        {"header_rejection", test_header_rejection},
        {"decode_kit_page", test_decode_kit_page},
        {"side_for_team", test_side_for_team},
        {"clamp_classes", test_clamp_classes},
        {"page_codec_roundtrip", test_page_codec_roundtrip},
        {"retail_file", test_retail_file},
    };

    int failures = 0;
    for (const Case &c : cases) {
        if (c.fn() != 0) {
            std::fprintf(stderr, "FAILED: %s\n", c.name);
            ++failures;
        } else {
            std::printf("ok: %s\n", c.name);
        }
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d playersav test case(s) failed\n", failures);
        return 1;
    }
    std::printf("playersav: all cases passed\n");
    return 0;
}
