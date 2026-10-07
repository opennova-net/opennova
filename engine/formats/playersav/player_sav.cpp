// player.sav reader/writer — see <formats/playersav/player_sav.h> for the
// witnessed layout and the [orig:] citations.

#include <formats/playersav/player_sav.h>

#include <algorithm>
#include <cstring>

#include <base/io/le.h>

namespace opennova::playersav {
namespace {

constexpr uint32_t kMagic = 0x43425046u;    // "FPBC"
constexpr uint32_t kVersion = 0x31313230u;  // "0211"

int32_t get_i32(const uint8_t *rec, size_t offset) {
	return static_cast<int32_t>(io::read_u32_le(rec + offset));
}

void put_i32(uint8_t *rec, size_t offset, int32_t v) {
	io::write_u32_le(rec + offset, static_cast<uint32_t>(v));
}

void put_u32(uint8_t *rec, size_t offset, uint32_t v) {
	io::write_u32_le(rec + offset, v);
}

void put_u16(uint8_t *rec, size_t offset, uint16_t v) {
	io::write_u16_le(rec + offset, v);
}

// A NUL-terminated string in a fixed field: up to the first NUL, or the whole
// field when it carries none.
std::string get_text(const uint8_t *field, size_t bytes) {
	const auto *begin = reinterpret_cast<const char *>(field);
	const auto *end = static_cast<const char *>(std::memchr(begin, 0, bytes));
	return std::string(begin, end != nullptr ? static_cast<size_t>(end - begin) : bytes);
}

// The text into a zeroed field, cut to its size.
void put_text(uint8_t *field, size_t bytes, const std::string &text) {
	std::memcpy(field, text.data(), std::min(text.size(), bytes));
}

// The int32 dwords at their offsets, one table for the reader and the writer.
constexpr RecordWord kWords[] = {
	{"last_campaign", 60, &ProfileRecord::last_campaign},
	{"campaigns_won", 1340, &ProfileRecord::campaigns_won},
	{"sp_no_char_abilities", 1352, &ProfileRecord::sp_no_char_abilities},
	{"sp_no_weapon_recoil", 1356, &ProfileRecord::sp_no_weapon_recoil},
	{"sp_no_scope_drift", 1360, &ProfileRecord::sp_no_scope_drift},
	{"sp_no_crosshair_spread", 1364, &ProfileRecord::sp_no_crosshair_spread},
	{"sp_wind", 1368, &ProfileRecord::sp_wind},
	{"sp_gps_icons", 1372, &ProfileRecord::sp_gps_icons},
	{"sp_no_drop_weapons", 1376, &ProfileRecord::sp_no_drop_weapons},
	{"sp_difficulty", 1380, &ProfileRecord::sp_difficulty},
	{"intro_pending", 1412, &ProfileRecord::intro_pending},
	{"word_1416", 1416, &ProfileRecord::word_1416},
	{"mouse_enabled", 1420, &ProfileRecord::mouse_enabled},
	{"mouse_sensitivity", 1424, &ProfileRecord::mouse_sensitivity},
	{"invert_mouse", 1428, &ProfileRecord::invert_mouse},
	{"joystick_enabled", 1432, &ProfileRecord::joystick_enabled},
	{"invert_joystick", 1436, &ProfileRecord::invert_joystick},
	{"joystick_cap_2", 1440, &ProfileRecord::joystick_cap_2},
	{"force_feedback", 1444, &ProfileRecord::force_feedback},
	{"word_1448", 1448, &ProfileRecord::word_1448},
	{"word_1452", 1452, &ProfileRecord::word_1452},
	{"joystick_cap_4", 1456, &ProfileRecord::joystick_cap_4},
	{"word_1460", 1460, &ProfileRecord::word_1460},
	{"word_1464", 1464, &ProfileRecord::word_1464},
	{"word_1468", 1468, &ProfileRecord::word_1468},
	{"word_1472", 1472, &ProfileRecord::word_1472},
	{"word_1476", 1476, &ProfileRecord::word_1476},
	{"word_1480", 1480, &ProfileRecord::word_1480},
	{"view_mode", 1484, &ProfileRecord::view_mode},
	{"word_1488", 1488, &ProfileRecord::word_1488},
	{"word_1496", 1496, &ProfileRecord::word_1496},
	{"word_1500", 1500, &ProfileRecord::word_1500},
	{"word_1504", 1504, &ProfileRecord::word_1504},
	{"word_1508", 1508, &ProfileRecord::word_1508},
	{"joystick_cap_1", 1512, &ProfileRecord::joystick_cap_1},
	{"word_1516", 1516, &ProfileRecord::word_1516},
	{"word_1520", 1520, &ProfileRecord::word_1520},
	{"auto_reload", 1524, &ProfileRecord::auto_reload},
	{"word_1528", 1528, &ProfileRecord::word_1528},
	{"auto_medic_off", 1660, &ProfileRecord::auto_medic_off},
};

constexpr size_t kFlagsOffset = 52;
constexpr size_t kVoiceOffset = 1532;

BindingEntry read_binding(const uint8_t *e) {
	BindingEntry b;
	b.id = io::read_u16_le(e + 0);
	b.index = static_cast<int32_t>(io::read_u32_le(e + 4));
	b.flags = io::read_u32_le(e + 8);
	b.modes = io::read_u32_le(e + 12);
	b.action_class = io::read_u32_le(e + 16);
	b.help = io::read_u32_le(e + 20);
	b.primary = io::read_u16_le(e + 24);
	b.secondary = io::read_u16_le(e + 26);
	b.primary_mod = io::read_u16_le(e + 28);
	b.secondary_mod = io::read_u16_le(e + 30);
	b.mouse_mask = io::read_u16_le(e + 32);
	b.mouse_mod = io::read_u16_le(e + 34);
	b.joy_button = e[36];
	b.joy_mod = e[37];
	b.token = get_text(e + kBindingTokenOffset, kBindingTokenBytes);
	return b;
}

void write_binding(const BindingEntry &b, uint8_t *e) {
	put_u16(e, 0, b.id);
	put_i32(e, 4, b.index);
	put_u32(e, 8, b.flags);
	put_u32(e, 12, b.modes);
	put_u32(e, 16, b.action_class);
	put_u32(e, 20, b.help);
	put_u16(e, 24, b.primary);
	put_u16(e, 26, b.secondary);
	put_u16(e, 28, b.primary_mod);
	put_u16(e, 30, b.secondary_mod);
	put_u16(e, 32, b.mouse_mask);
	put_u16(e, 34, b.mouse_mod);
	e[36] = b.joy_button;
	e[37] = b.joy_mod;
	put_text(e + kBindingTokenOffset, kBindingTokenBytes, b.token);
}

}  // namespace

const RecordWord *record_words(size_t *count) {
	if (count != nullptr) *count = sizeof(kWords) / sizeof(kWords[0]);
	return kWords;
}

const RecordWord *find_record_word(const std::string &name) {
	for (const RecordWord &w : kWords)
		if (name == w.name) return &w;
	return nullptr;
}

ProfileRecord read_record(const uint8_t *rec) {
	ProfileRecord out;
	out.tag = io::read_u32_le(rec);
	out.name = get_text(rec + kNameOffset, kNameBytes);
	out.flags = io::read_u32_le(rec + kFlagsOffset);
	for (const RecordWord &w : kWords) out.*(w.member) = get_i32(rec, w.offset);
	for (size_t c = 0; c < kCampaigns; ++c)
		std::memcpy(out.campaign_complete[c].data(),
				rec + kCampaignOffset + c * kCampaignMissions, kCampaignMissions);
	for (size_t m = 0; m < kMacroCount; ++m)
		out.macros[m] = get_text(rec + kMacroOffset + m * kMacroBytes, kMacroBytes);
	out.voice[0] = rec[kVoiceOffset];
	out.voice[1] = rec[kVoiceOffset + 1];
	const int32_t count = get_i32(rec, kBindingCountOffset);
	const size_t n = count <= 0 ? 0 : std::min(static_cast<size_t>(count), kBindingCapacity);
	out.bindings.reserve(n);
	for (size_t i = 0; i < n; ++i)
		out.bindings.push_back(read_binding(rec + kBindingOffset + i * kBindingBytes));
	return out;
}

void write_record(const ProfileRecord &record, uint8_t *out) {
	std::memset(out, 0, kPlayerRecordBytes);
	put_u32(out, 0, record.tag);
	put_text(out + kNameOffset, kNameBytes, record.name);
	put_u32(out, kFlagsOffset, record.flags);
	for (const RecordWord &w : kWords) put_i32(out, w.offset, record.*(w.member));
	for (size_t c = 0; c < kCampaigns; ++c)
		std::memcpy(out + kCampaignOffset + c * kCampaignMissions,
				record.campaign_complete[c].data(), kCampaignMissions);
	// A macro is its cell's first 39 bytes, the 40th the NUL the seed writes
	// [orig: PlayerProfile_InitDefaults @0x54bcc7..0x54bccc].
	for (size_t m = 0; m < kMacroCount; ++m)
		put_text(out + kMacroOffset + m * kMacroBytes, kMacroBytes - 1, record.macros[m]);
	out[kVoiceOffset] = record.voice[0];
	out[kVoiceOffset + 1] = record.voice[1];
	const size_t n = std::min(record.bindings.size(), kBindingCapacity);
	put_i32(out, kBindingCountOffset, static_cast<int32_t>(n));
	for (size_t i = 0; i < n; ++i)
		write_binding(record.bindings[i], out + kBindingOffset + i * kBindingBytes);
}

bool read(const uint8_t *data, size_t size, PlayerSav &out) {
	out = PlayerSav{};
	if (data == nullptr || size < kHeaderBytes + kProfileSlots * kPlayerRecordBytes) return false;
	if (io::read_u32_le(data) != kMagic || io::read_u32_le(data + 4) != kVersion) return false;
	out.flags = io::read_u32_le(data + 8);
	out.extra = io::read_u32_le(data + 12);
	for (size_t s = 0; s < kProfileSlots; ++s)
		out.slots[s] = read_record(data + kHeaderBytes + s * kPlayerRecordBytes);
	return true;
}

std::vector<uint8_t> write(const PlayerSav &in) {
	std::vector<uint8_t> out(kPlayerSavBytes, 0);
	put_u32(out.data(), 0, kMagic);
	put_u32(out.data(), 4, kVersion);
	put_u32(out.data(), 8, in.flags);
	put_u32(out.data(), 12, in.extra);
	for (size_t s = 0; s < kProfileSlots; ++s)
		write_record(in.slots[s], out.data() + kHeaderBytes + s * kPlayerRecordBytes);
	std::memcpy(out.data() + kHeaderBytes + kProfileSlots * kPlayerRecordBytes, kTrailer,
			kTrailerBytes);
	return out;
}

}  // namespace opennova::playersav
