// engine/formats/playersav — player.sav parse/serialize tests.
//
// Every fixture is built in code: no retail file is committed. The optional
// last leg reads the install's own player.sav (OPENNOVA_JO_DIR) read only and
// notes SKIP-LEG otherwise (docs/asset-gated-tests.md).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include <formats/playersav/player_sav.h>

using namespace opennova::playersav;

namespace {

uint32_t u32_at(const std::vector<uint8_t> &b, size_t off) {
    return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
           (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
}

uint16_t u16_at(const std::vector<uint8_t> &b, size_t off) {
    return static_cast<uint16_t>(b[off] | (b[off + 1] << 8));
}

std::string text_at(const std::vector<uint8_t> &b, size_t off) {
    std::string out;
    for (size_t i = off; i < b.size() && b[i] != 0; ++i) out.push_back(static_cast<char>(b[i]));
    return out;
}

BindingEntry sample_binding(uint16_t id, const char *token) {
    BindingEntry e;
    e.id = id;
    e.index = id;
    e.flags = 0x0C000C05u;
    e.modes = 3;
    e.action_class = 1;
    e.help = 1001;
    e.primary = 0x57;
    e.secondary = 0x26;
    e.primary_mod = 17;
    e.secondary_mod = 16;
    e.mouse_mask = 0x400;
    e.mouse_mod = 17;
    e.joy_button = 0x81;
    e.joy_mod = 3;
    e.token = token;
    return e;
}

bool bindings_equal(const BindingEntry &a, const BindingEntry &b) {
    return a.id == b.id && a.index == b.index && a.flags == b.flags && a.modes == b.modes &&
           a.action_class == b.action_class && a.help == b.help && a.primary == b.primary &&
           a.secondary == b.secondary && a.primary_mod == b.primary_mod &&
           a.secondary_mod == b.secondary_mod && a.mouse_mask == b.mouse_mask &&
           a.mouse_mod == b.mouse_mod && a.joy_button == b.joy_button && a.joy_mod == b.joy_mod &&
           a.token == b.token;
}

bool records_equal(const ProfileRecord &a, const ProfileRecord &b) {
    if (a.tag != b.tag || a.name != b.name || a.flags != b.flags) return false;
    size_t count = 0;
    const RecordWord *words = record_words(&count);
    for (size_t i = 0; i < count; ++i)
        if (a.*(words[i].member) != b.*(words[i].member)) return false;
    if (a.campaign_complete != b.campaign_complete || a.macros != b.macros || a.voice != b.voice)
        return false;
    if (a.bindings.size() != b.bindings.size()) return false;
    for (size_t i = 0; i < a.bindings.size(); ++i)
        if (!bindings_equal(a.bindings[i], b.bindings[i])) return false;
    return true;
}

// --- 1. the on-disk layout [orig: PlayerProfile_SaveToFiles @0x54be00] -------

int test_layout() {
    PlayerSav f;
    f.flags = 1;
    f.extra = 0x05000000u;
    ProfileRecord &r = f.slots[2];
    r.name = "Sandman";
    r.flags = 0;
    r.last_campaign = -1;
    r.campaign_complete[3][7] = 1;
    r.macros[0] = "I'm on offense";
    r.macros[9] = std::string(60, 'm');  // cut to the 40-byte cell
    r.campaigns_won = 4;
    r.sp_no_char_abilities = 1;
    r.sp_no_weapon_recoil = 2;
    r.sp_no_scope_drift = 3;
    r.sp_no_crosshair_spread = 4;
    r.sp_difficulty = -1;
    r.mouse_sensitivity = 200;
    r.invert_mouse = 1;
    r.auto_reload = 0;
    r.voice = {5, 9};
    r.auto_medic_off = 1;
    r.bindings = {sample_binding(152, "move_forward"), sample_binding(1, "command")};

    const std::vector<uint8_t> buf = write(f);
    TEST_EXPECT(buf.size() == kPlayerSavBytes);
    TEST_EXPECT(buf.size() == 77464);
    TEST_EXPECT(std::memcmp(buf.data(), "FPBC0211", 8) == 0);
    TEST_EXPECT(u32_at(buf, 8) == 1 && u32_at(buf, 12) == 0x05000000u);
    TEST_EXPECT(std::memcmp(buf.data() + buf.size() - kTrailerBytes, "fsasyaof", 8) == 0);

    const size_t base = kHeaderBytes + 2 * kPlayerRecordBytes;
    TEST_EXPECT(u32_at(buf, base) == kRecordTag);
    TEST_EXPECT(text_at(buf, base + 4) == "Sandman");
    TEST_EXPECT(u32_at(buf, base + 52) == 0);
    TEST_EXPECT(u32_at(buf, base + 60) == 0xFFFFFFFFu);
    TEST_EXPECT(buf[base + 64 + 32 * 3 + 7] == 1);
    TEST_EXPECT(text_at(buf, base + 576) == "I'm on offense");
    // A macro is the cell's first 39 bytes; its 40th stays the NUL the seed puts
    // there [orig: @0x54bccc].
    TEST_EXPECT(text_at(buf, base + 576 + 9 * 40) == std::string(39, 'm'));
    TEST_EXPECT(u32_at(buf, base + 1340) == 4);
    TEST_EXPECT(u32_at(buf, base + 0x548) == 1);
    TEST_EXPECT(u32_at(buf, base + 0x54C) == 2);
    TEST_EXPECT(u32_at(buf, base + 0x550) == 3);
    TEST_EXPECT(u32_at(buf, base + 0x554) == 4);
    TEST_EXPECT(u32_at(buf, base + 0x564) == 0xFFFFFFFFu);
    TEST_EXPECT(u32_at(buf, base + 1424) == 200);
    TEST_EXPECT(u32_at(buf, base + 1428) == 1);
    TEST_EXPECT(u32_at(buf, base + 1524) == 0);
    TEST_EXPECT(buf[base + 1532] == 5 && buf[base + 1533] == 9);
    TEST_EXPECT(u32_at(buf, base + 1660) == 1);
    TEST_EXPECT(u32_at(buf, base + 1804) == 2);
    const size_t e0 = base + 1808;
    TEST_EXPECT(u16_at(buf, e0) == 152 && u16_at(buf, e0 + 2) == 0);
    TEST_EXPECT(u32_at(buf, e0 + 4) == 152 && u32_at(buf, e0 + 8) == 0x0C000C05u);
    TEST_EXPECT(u32_at(buf, e0 + 12) == 3 && u32_at(buf, e0 + 16) == 1 && u32_at(buf, e0 + 20) == 1001);
    TEST_EXPECT(u16_at(buf, e0 + 24) == 0x57 && u16_at(buf, e0 + 26) == 0x26);
    TEST_EXPECT(u16_at(buf, e0 + 28) == 17 && u16_at(buf, e0 + 30) == 16);
    TEST_EXPECT(u16_at(buf, e0 + 32) == 0x400 && u16_at(buf, e0 + 34) == 17);
    TEST_EXPECT(buf[e0 + 36] == 0x81 && buf[e0 + 37] == 3);
    TEST_EXPECT(text_at(buf, e0 + 38) == "move_forward");
    TEST_EXPECT(text_at(buf, e0 + 72 + 38) == "command");
    // The table past its count is zero, as the default table's copy leaves it.
    for (size_t i = e0 + 2 * 72; i < base + kPlayerRecordBytes; ++i) TEST_EXPECT(buf[i] == 0);
    // A default record's seeds.
    const size_t base0 = kHeaderBytes;
    TEST_EXPECT(u32_at(buf, base0 + 52) == kRecordFlagNoName);
    TEST_EXPECT(u32_at(buf, base0 + 1372) == 1);
    TEST_EXPECT(u32_at(buf, base0 + 1412) == 0xFFFFFFFFu);
    TEST_EXPECT(u32_at(buf, base0 + 1424) == 128);
    TEST_EXPECT(u32_at(buf, base0 + 1468) == 127);
    TEST_EXPECT(u32_at(buf, base0 + 1524) == 1);
    return 0;
}

// --- 2. round trips --------------------------------------------------------------

int test_round_trip() {
    PlayerSav f;
    f.slots[0].name = "cdouglass";
    f.slots[0].flags = 0;
    f.slots[1].macros[3] = "Guard the base";
    f.slots[4].bindings = {sample_binding(500, "CycleSpectatorMode")};
    f.slots[4].word_1460 = 77;
    const std::vector<uint8_t> buf = write(f);
    PlayerSav back;
    TEST_EXPECT(read(buf.data(), buf.size(), back));
    for (size_t s = 0; s < kProfileSlots; ++s) TEST_EXPECT(records_equal(f.slots[s], back.slots[s]));
    TEST_EXPECT(write(back) == buf);

    // The word table names every member once, in offset order, the names the
    // tools use [orig: the reads @0x551500 and the seeds @0x54bb40].
    size_t count = 0;
    const RecordWord *words = record_words(&count);
    TEST_EXPECT(count == 40);
    for (size_t i = 1; i < count; ++i) TEST_EXPECT(words[i - 1].offset < words[i].offset);
    TEST_EXPECT(find_record_word("sp_no_char_abilities") != nullptr &&
                find_record_word("sp_no_char_abilities")->offset == 0x548);
    TEST_EXPECT(find_record_word("auto_medic_off")->offset == 1660);
    TEST_EXPECT(find_record_word("nope") == nullptr);
    return 0;
}

// --- 3. the header gate [orig: @0x54f5c6] ------------------------------------------

int test_header_gate() {
    const std::vector<uint8_t> good = write(PlayerSav{});
    PlayerSav out;
    TEST_EXPECT(read(good.data(), good.size(), out));
    // The trailer is never read.
    TEST_EXPECT(read(good.data(), good.size() - kTrailerBytes, out));
    std::vector<uint8_t> bad = good;
    bad[0] = 'X';
    TEST_EXPECT(!read(bad.data(), bad.size(), out));
    bad = good;
    bad[7] = '2';
    TEST_EXPECT(!read(bad.data(), bad.size(), out));
    TEST_EXPECT(!read(good.data(), kHeaderBytes + kPlayerRecordBytes, out));
    TEST_EXPECT(!read(nullptr, 0, out));
    // A count past the table's capacity reads the capacity; a negative one none.
    std::vector<uint8_t> big = good;
    big[kHeaderBytes + kBindingCountOffset] = 0xFF;
    big[kHeaderBytes + kBindingCountOffset + 1] = 0x7F;
    TEST_EXPECT(read(big.data(), big.size(), out));
    TEST_EXPECT(out.slots[0].bindings.size() == kBindingCapacity);
    big[kHeaderBytes + kBindingCountOffset + 3] = 0x80;
    TEST_EXPECT(read(big.data(), big.size(), out));
    TEST_EXPECT(out.slots[0].bindings.empty());
    return 0;
}

// --- 4. optional: the install's own player.sav (SKIP-LEG without it) ---------

int test_retail_file() {
    const std::string sav = retail::player_sav();
    if (sav.empty()) return retail::skip_leg("OPENNOVA_JO_DIR carrying a retail player.sav (corpus leg)");
    std::vector<uint8_t> bytes;
    if (!test_io::read_file(sav.c_str(), bytes)) {
        std::fprintf(stderr, "retail player.sav found but unreadable: %s\n", sav.c_str());
        return 1;
    }
    TEST_EXPECT(bytes.size() == kPlayerSavBytes);
    PlayerSav f;
    TEST_EXPECT(read(bytes.data(), bytes.size(), f));
    for (const ProfileRecord &r : f.slots) {
        TEST_EXPECT(r.tag == kRecordTag);
        TEST_EXPECT(!r.bindings.empty());
        TEST_EXPECT(r.mouse_sensitivity >= 0);
    }
    // Writer parity: the install's file re-serializes byte for byte (every region
    // the writer emits as zero is zero in it).
    const std::vector<uint8_t> rewritten = write(f);
    TEST_EXPECT(rewritten == bytes);
    std::printf("retail player.sav: %zu bytes re-serialized byte-identically; slot 0 \"%s\", %zu bindings\n",
                bytes.size(), f.slots[0].name.c_str(), f.slots[0].bindings.size());
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);
    struct Case {
        const char *name;
        int (*fn)();
    };
    const Case cases[] = {
        {"layout", test_layout},
        {"round_trip", test_round_trip},
        {"header_gate", test_header_gate},
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
        std::fprintf(stderr, "%d player_sav test case(s) failed\n", failures);
        return 1;
    }
    std::printf("player_sav: all cases passed\n");
    return 0;
}
