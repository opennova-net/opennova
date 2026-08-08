// weapon.sav reader/writer — see include/playersav/weapon_sav.h for the
// witnessed layout and the [orig:] citations.

#include "playersav/weapon_sav.h"

#include <cstdlib>
#include <cstring>

#include <io/byte_reader.h>
#include <io/byte_writer.h>

namespace opennova::playersav {
namespace {

// The 16-byte header's first two dwords, tested for equality by the loader
// [orig: PlayerProfile_LoadAllFromDisk @0x54f4d0]. 1128419398 / 825307696 in
// the original's decimal immediates; little-endian they read as ASCII.
constexpr uint32_t kMagic = 0x43425046u;    // "FPBC"
constexpr uint32_t kVersion = 0x31313230u;  // "0211"

// The witnessed regions of a side block leave a trailing span with no reader in
// the retail image and all-zero content across the retail corpus. We neither
// store nor pass it through — the writer emits zeros.
constexpr size_t kSideUnwitnessedTailBytes =
    kSideBytes - kFirstKitPageOffset - kKitPagesPerSide * kKitPageBytes;  // 22528

// A kit page value token is decimal text consumed by atol in the original. An
// empty or non-numeric token yields -1, the "unset" value every shipped page
// authors.
int32_t parse_value(const std::string &text)
{
    if (text.empty())
        return -1;
    const char *begin = text.c_str();
    char *end = nullptr;
    const long v = std::strtol(begin, &end, 10);
    if (end == begin)
        return -1;
    return static_cast<int32_t>(v);
}

std::string format_value(int32_t v)
{
    return std::to_string(v);
}

KitPage single_name_page(const char *name)
{
    KitPage page;
    KitEntry entry;
    entry.name = name;
    page.entries.push_back(entry);
    return page;
}

void read_side(io::ByteReader &r, Side &side)
{
    const size_t base = r.position();
    side.player_class = r.read_u8();  // +0
    side.avatar_a = r.read_u8();      // +1
    side.avatar_b = r.read_u8();      // +2
    r.skip(1);                        // +3 — unwitnessed pad, zero in the corpus
    side.avatar_packed = r.read_u16();  // +4

    std::array<uint8_t, kKitPageBytes> page{};
    for (size_t i = 0; i < kKitPagesPerSide; ++i) {
        r.read_bytes(page.data(), page.size());
        side.pages[i] = decode_kit_page(page.data(), page.size());
    }

    // Unwitnessed trailing region — skipped, never stored.
    r.skip(base + kSideBytes - r.position());
}

void write_page(io::ByteWriter &w, const KitPage &page)
{
    std::array<uint8_t, kKitPageBytes> buf{};
    encode_kit_page(page, buf.data(), buf.size());
    w.write_bytes(buf.data(), buf.size());
}

void write_side(io::ByteWriter &w, const Side &side)
{
    w.write_u8(side.player_class);
    w.write_u8(side.avatar_a);
    w.write_u8(side.avatar_b);
    w.write_u8(0);  // +3 unwitnessed pad
    w.write_u16(side.avatar_packed);
    for (const KitPage &page : side.pages)
        write_page(w, page);
    for (size_t i = 0; i < kSideUnwitnessedTailBytes; ++i)
        w.write_u8(0);  // unwitnessed trailing region
}

}  // namespace

SideId side_for_team(uint8_t team)
{
    // [orig: Game_StartMission @0x525798-0x5257b4 — `team == 1 || team == 3`
    // selects the blue-side block, everything else the red one].
    return (team == 1 || team == 3) ? SideId::Blue : SideId::Red;
}

const KitPage *Side::page_for_class(uint8_t klass) const
{
    if (klass < kMinPlayerClass || klass > kMaxPlayerClass)
        return nullptr;
    return &pages[static_cast<size_t>(klass) - kMinPlayerClass];
}

const KitPage *Side::selected_page() const
{
    return page_for_class(player_class);
}

const Side &Record::side(SideId s) const
{
    return s == SideId::Blue ? blue : red;
}

Side &Record::side(SideId s)
{
    return s == SideId::Blue ? blue : red;
}

KitPage decode_kit_page(const uint8_t *page, size_t size)
{
    KitPage out;
    if (page == nullptr || size == 0)
        return out;

    // Tokenize: NUL-separated ASCII, terminated by an empty string (the double
    // NUL) or by running off the end of the page.
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < size) {
        const size_t start = i;
        while (i < size && page[i] != 0)
            ++i;
        std::string token(reinterpret_cast<const char *>(page + start), i - start);
        if (i < size)
            ++i;  // consume the NUL
        if (token.empty())
            break;  // double NUL: end of blob
        tokens.push_back(std::move(token));
    }

    // Groups of four: name, then three decimal-text values. A trailing PARTIAL
    // group still yields an entry (that is exactly what the single-literal
    // PlayerProfile_InitDefaults page looks like); the missing values default
    // to -1.
    for (size_t k = 0; k < tokens.size(); k += 4) {
        if (tokens[k].empty())
            continue;
        KitEntry entry;
        entry.name = tokens[k];
        if (k + 1 < tokens.size())
            entry.ammo_primary = parse_value(tokens[k + 1]);
        if (k + 2 < tokens.size())
            entry.ammo_secondary = parse_value(tokens[k + 2]);
        if (k + 3 < tokens.size())
            entry.flags = parse_value(tokens[k + 3]);
        out.entries.push_back(std::move(entry));
    }
    return out;
}

void encode_kit_page(const KitPage &page, uint8_t *out, size_t size)
{
    if (out == nullptr || size == 0)
        return;
    std::memset(out, 0, size);

    size_t pos = 0;
    for (const KitEntry &entry : page.entries) {
        if (entry.name.empty())
            continue;
        const std::string values[3] = {
            format_value(entry.ammo_primary),
            format_value(entry.ammo_secondary),
            format_value(entry.flags),
        };
        size_t need = entry.name.size() + 1;
        for (const std::string &v : values)
            need += v.size() + 1;
        // Leave room for the terminating NUL that closes the blob; the buffer
        // is already zeroed, so it costs one byte of headroom, not a write.
        if (pos + need + 1 > size)
            break;

        std::memcpy(out + pos, entry.name.data(), entry.name.size());
        pos += entry.name.size();
        out[pos++] = 0;
        for (const std::string &v : values) {
            std::memcpy(out + pos, v.data(), v.size());
            pos += v.size();
            out[pos++] = 0;
        }
    }
    // out[pos] is already 0 from the memset: that byte is the blob terminator.
}

bool read(const uint8_t *data, size_t size, File &out)
{
    out = File{};
    if (data == nullptr || size < kHeaderBytes)
        return false;

    io::ByteReader r(data, size);
    const uint32_t magic = r.read_u32();
    const uint32_t version = r.read_u32();
    if (magic != kMagic || version != kVersion)
        return false;
    out.flags = r.read_u32();
    out.extra = r.read_u32();

    if (size == kHeaderBytes)
        return true;  // header-only file: the slots keep their struct defaults
    if (size < kHeaderBytes + kProfileSlots * kRecordBytes)
        return false;

    for (size_t s = 0; s < kProfileSlots; ++s) {
        const size_t base = r.position();
        Record &rec = out.slots[s];
        read_side(r, rec.blue);
        read_side(r, rec.red);
        std::array<uint8_t, kKitPageBytes> page{};
        r.read_bytes(page.data(), page.size());
        rec.single_player = decode_kit_page(page.data(), page.size());
        r.skip(base + kRecordBytes - r.position());
    }
    return true;
}

std::vector<uint8_t> write(const File &in)
{
    io::ByteWriter w;
    w.write_u32(kMagic);
    w.write_u32(kVersion);
    w.write_u32(in.flags);
    w.write_u32(in.extra);
    for (const Record &rec : in.slots) {
        write_side(w, rec.blue);
        write_side(w, rec.red);
        write_page(w, rec.single_player);
    }
    return w.take();
}

File make_defaults()
{
    // [orig: PlayerProfile_InitDefaults @0x54bb40] — both side class bytes 8,
    // the single-player page "WPN_M4AUTO", and one weapon name per class page
    // written at base-2048 (class 5) .. base+6144 (class 9).
    File f;
    for (Record &rec : f.slots) {
        rec.blue.player_class = 8;
        rec.red.player_class = 8;

        rec.blue.pages[0] = single_name_page("WPN_M4AUTO");    // class 5 medic
        rec.blue.pages[1] = single_name_page("WPN_SR25");      // class 6 sniper
        rec.blue.pages[2] = single_name_page("WPN_M60");       // class 7 gunner
        rec.blue.pages[3] = single_name_page("WPN_M4AUTO");    // class 8 rifleman
        rec.blue.pages[4] = single_name_page("WPN_M16BURST");  // class 9 engineer

        rec.red.pages[0] = single_name_page("WPN_AK47AUTO");
        rec.red.pages[1] = single_name_page("WPN_DRAGUNOV");
        rec.red.pages[2] = single_name_page("WPN_PKM");
        rec.red.pages[3] = single_name_page("WPN_AK47AUTO");
        rec.red.pages[4] = single_name_page("WPN_AK74AUTO");

        rec.single_player = single_name_page("WPN_M4AUTO");
    }
    return f;
}

void clamp_classes(File &f)
{
    // [orig: apply_session_settings_to_globals @0x5516d0-@0x5516ec —
    // `for (side = 0; side < 2; ++side, p += 0x8006) { if (*p < 5) *p = 5;
    //  else if (*p > 9) *p = 9; }`].
    for (Record &rec : f.slots) {
        for (Side *side : {&rec.blue, &rec.red}) {
            if (side->player_class < kMinPlayerClass)
                side->player_class = kMinPlayerClass;
            else if (side->player_class > kMaxPlayerClass)
                side->player_class = kMaxPlayerClass;
        }
    }
}

}  // namespace opennova::playersav
