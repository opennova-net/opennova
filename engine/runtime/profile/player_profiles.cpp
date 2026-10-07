// The player profile's in-memory image, its load and its save — see
// <runtime/profile/player_profiles.h> and docs/playerinfo/player-sav-re.md.

#include <runtime/profile/player_profiles.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include <base/io/le.h>
#include <base/io/strutil.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/controls/controls.h>

namespace opennova::profile {
namespace {

constexpr uint32_t kMagic = 0x43425046u;    // "FPBC"
constexpr uint32_t kVersion = 0x31313230u;  // "0211"
constexpr uint32_t kDefaultTableFlag = 0x4000000u;

// The file's first 16 bytes as the load's File_Read leaves them in its zeroed
// locals: what a short file lacks reads zero [orig: @0x54f502..0x54f5ac].
struct Header {
	uint32_t magic = 0;
	uint32_t version = 0;
	uint32_t flags = 0;
	uint32_t extra = 0;
};

Header read_header(const std::vector<uint8_t> &bytes) {
	uint8_t raw[playersav::kHeaderBytes] = {};
	std::memcpy(raw, bytes.data(), std::min(bytes.size(), sizeof(raw)));
	Header h;
	h.magic = io::read_u32_le(raw);
	h.version = io::read_u32_le(raw + 4);
	h.flags = io::read_u32_le(raw + 8);
	h.extra = io::read_u32_le(raw + 12);
	return h;
}

// The bytes the file holds at [offset, offset + size) over `image`: the
// original reads each record straight into the seeded memory, so a file that
// ends early leaves the seeded bytes past its end.
void overlay(std::vector<uint8_t> &image, size_t image_offset, const std::vector<uint8_t> &file,
		size_t file_offset, size_t size) {
	if (file_offset >= file.size()) return;
	const size_t n = std::min(size, file.size() - file_offset);
	std::memcpy(image.data() + image_offset, file.data() + file_offset, n);
}

}  // namespace

std::vector<playersav::BindingEntry> default_binding_table() {
	size_t count = 0;
	const controls::ActionDef *rows = controls::catalog(&count);
	std::vector<const controls::ActionDef *> filtered;
	for (size_t i = 0; i < count; ++i) {
		// [orig: `(flags & 0x4000000) != 0` @0x54c2e5]
		if ((rows[i].flags & kDefaultTableFlag) != 0 && controls::action_code(rows[i].id) >= 0)
			filtered.push_back(&rows[i]);
	}
	// The walk runs over the table the boot's sort laid out by action code
	// [orig: KeyBinding_SortBySequentialId @0x498260].
	std::sort(filtered.begin(), filtered.end(),
			[](const controls::ActionDef *a, const controls::ActionDef *b) {
				return controls::action_code(a->id) < controls::action_code(b->id);
			});
	std::vector<playersav::BindingEntry> out;
	for (const controls::ActionDef *row : filtered) {
		if (out.size() == playersav::kBindingCapacity) break;  // [orig: @0x54c39b]
		const int code = controls::action_code(row->id);
		playersav::BindingEntry e;
		// [orig: @0x54c2eb..0x54c37a — entry +8 the flags (row +4), +12 the
		//  modes (row +8), +16 the class (row +12), +20 the help (row +16),
		//  +24/+26 the keys (row +20/+22), +28/+30 the key modifiers (row
		//  +28/+30), +32 the mouse mask (row +24), +34 the mouse modifier
		//  (row +32), +36 the joystick binding (row +26), +37 its modifier
		//  (row +34), +4 the row index, +0 the code, +38 the token (row +75)]
		e.id = static_cast<uint16_t>(code);
		e.index = code;
		e.flags = row->flags;
		e.modes = row->modes;
		e.action_class = static_cast<uint32_t>(row->cls);
		e.help = row->help;
		e.primary = static_cast<uint16_t>(row->default_key);
		e.secondary = static_cast<uint16_t>(row->default_key2);
		e.primary_mod = static_cast<uint16_t>(row->default_mod);
		e.secondary_mod = 0;  // row +30, zero on every row
		e.mouse_mask = row->default_mouse;
		e.mouse_mod = row->default_mouse_mod;
		e.joy_button = row->default_joy;
		e.joy_mod = row->default_joy_mod;
		e.token = row->token;
		out.push_back(std::move(e));
	}
	return out;
}

std::array<std::string, playersav::kMacroCount> default_macros(
		const rtxt::File *override_table, const rtxt::File *menu_table) {
	std::array<std::string, playersav::kMacroCount> out;
	for (size_t i = 0; i < playersav::kMacroCount; ++i) {
		// [orig: strcpy(key, "MACRO_0") @0x54bb6e; key[6] = '0' + i @0x54bcb5]
		const std::string key = "MACRO_" + std::to_string(i);
		const rtxt::Entry *entry = nullptr;
		// The override table first, then the menu table's own [orig:
		// TextResource_FindEntryBySectionAndKey @0x75d27b].
		if (override_table != nullptr && override_table != menu_table)
			entry = override_table->find_in_section("Macros", key);
		if (entry == nullptr && menu_table != nullptr)
			entry = menu_table->find_in_section("Macros", key);
		// A miss is the "??%s??" marker of the key [orig: @0x562f0c].
		std::string text = entry != nullptr ? entry->text : "??" + key + "??";
		const size_t nul = text.find('\0');
		if (nul != std::string::npos) text.resize(nul);
		// [orig: strncpy(dest, text, 0x27) @0x54bcc7; dest[39] = 0 @0x54bccc]
		if (text.size() > playersav::kMacroBytes - 1) text.resize(playersav::kMacroBytes - 1);
		out[i] = std::move(text);
	}
	return out;
}

void init_defaults(playersav::ProfileRecord &record, playersav::Record &weapons,
		const ProfileDefaults &defaults) {
	// [orig: memset(profile, 0, 0x3C80) @0x54bb76, then the seeds: the record's
	//  member defaults are the unconditional ones — +0 "0211" @0x54bb94, +52 = 1
	//  @0x54bbb7, +1372 = 1 @0x54bbba, +1424 = 128 @0x54bbc0, +1464 = 1
	//  @0x54bbca, +1468 = 127 @0x54bbd0, +1532/+1533 = 0 @0x54bc1e, +1416 = 1
	//  @0x54bc2a, +1412 = -1 @0x54bc30, +1484 = 1 @0x54bc3a, +1508 = 1
	//  @0x54bc40, +1516 = 1 @0x54bc46, +1520 = 1 @0x54bc4c, +1380 = 0
	//  @0x54bc52, +1524 = 1 @0x54bc58, +1504 = 1 @0x54bc88, +1500 = 1
	//  @0x54bc8e, +1420 = 1 @0x54bc94]
	record = playersav::ProfileRecord{};
	// +1808 the default table, +1804 its count [orig: @0x54bb8c, @0x54bba6]
	record.bindings = defaults.bindings;
	if (record.bindings.size() > playersav::kBindingCapacity)
		record.bindings.resize(playersav::kBindingCapacity);
	// The joystick seeds, the two inner ones nested under the first
	// [orig: @0x54bc64..0x54bc82 — byte_3342F80 (bit 1) -> +1512, then
	//  byte_3342F82 (bit 4) -> +1456 and byte_3342F81 (bit 2) -> +1440]
	if ((defaults.joystick_caps & 1u) != 0) {
		record.joystick_cap_1 = 1;
		if ((defaults.joystick_caps & 4u) != 0) record.joystick_cap_4 = 1;
		if ((defaults.joystick_caps & 2u) != 0) record.joystick_cap_2 = 1;
	}
	record.macros = defaults.macros;  // [orig: @0x54bc9a..0x54bcda]
	// The weapon record: both class bytes 8 [orig: @0x54bbe0, @0x54bbe3], each
	// side's character [orig: @0x54bbea..0x54bc17], the single-player page
	// [orig: @0x54bced] and each side's five class pages [orig:
	// @0x54bcf7..0x54bde4].
	weapons.blue.player_class = 8;
	weapons.red.player_class = 8;
	weapons.blue.avatar_a = defaults.avatars[0].nationality;
	weapons.blue.avatar_b = defaults.avatars[0].division;
	weapons.blue.avatar_packed = defaults.avatars[0].packed;
	weapons.red.avatar_a = defaults.avatars[1].nationality;
	weapons.red.avatar_b = defaults.avatars[1].division;
	weapons.red.avatar_packed = defaults.avatars[1].packed;
	weapons.single_player = playersav::default_single_player_page();
	for (size_t i = 0; i < playersav::kKitPagesPerSide; ++i) {
		const uint8_t klass = static_cast<uint8_t>(playersav::kMinPlayerClass + i);
		weapons.blue.pages[i] = playersav::default_kit_page(playersav::SideId::Blue, klass);
		weapons.red.pages[i] = playersav::default_kit_page(playersav::SideId::Red, klass);
	}
}

std::vector<playersav::BindingEntry> merge_bindings(
		const std::vector<playersav::BindingEntry> &defaults,
		const std::vector<playersav::BindingEntry> &saved) {
	std::vector<playersav::BindingEntry> out = defaults;  // [orig: memcpy @0x54f600]
	for (const playersav::BindingEntry &s : saved) {
		for (playersav::BindingEntry &d : out) {
			// [orig: the code compare @0x54c41a, then _stricmp of the tokens
			//  @0x54c424; the first match takes the eight fields
			//  @0x54c444..0x54c479]
			if (s.id != d.id || !strutil::iequals(s.token, d.token)) continue;
			d.primary = s.primary;
			d.secondary = s.secondary;
			d.primary_mod = s.primary_mod;
			d.secondary_mod = s.secondary_mod;
			d.mouse_mask = s.mouse_mask;
			d.mouse_mod = s.mouse_mod;
			d.joy_button = s.joy_button;
			d.joy_mod = s.joy_mod;
			break;
		}
	}
	return out;
}

bool name_char_allowed(char c) {
	// [orig: @0x55fc0b..0x55fc29 — 'A'..'Z', 'a'..'z', '0'..'9', ' ', '-', '.']
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			c == ' ' || c == '-' || c == '.';
}

PlayerProfiles::PlayerProfiles() = default;

void PlayerProfiles::select_slot(int slot) {
	slot_ = (slot < 0 || slot >= static_cast<int>(playersav::kProfileSlots)) ? 0 : slot;
}

void PlayerProfiles::load(const LoadInput &in, const ProfileDefaults &defaults) {
	const int saved_slot = slot_;  // [orig: @0x54f4e9]
	slot_ = 0;                     // [orig: @0x54f516]
	init_defaults(player_.slots[0], weapons_.slots[0], defaults);  // [orig: @0x54f51b]
	// GetUserNameA(+4, 16) [orig: @0x54f535]: a name that does not fit the
	// buffer with its NUL fails and leaves the cleared field.
	if (in.user_name.size() < playersav::kNameMaxChars) player_.slots[0].name = in.user_name;
	player_.slots[0].flags &= ~playersav::kRecordFlagNoName;  // [orig: @0x54f53b]
	for (size_t s = 1; s < playersav::kProfileSlots; ++s)     // [orig: @0x54f55b]
		init_defaults(player_.slots[s], weapons_.slots[s], defaults);

	first_run_ = false;  // [orig: @0x54f57c]
	uint32_t extra = 0;
	if (in.player_sav != nullptr) {  // [orig: File_CheckExists @0x54f586]
		const Header h = read_header(*in.player_sav);
		extra = h.extra;
		if (h.magic == kMagic && h.version == kVersion) {  // [orig: @0x54f5c6]
			if ((h.flags & playersav::kHeaderFlagLatched) != 0) header_latched_ = true;
			for (size_t s = 0; s < playersav::kProfileSlots; ++s) {
				// [orig: File_Read(record, 0x3C80) @0x54f5ec]
				std::vector<uint8_t> image(playersav::kPlayerRecordBytes, 0);
				playersav::write_record(player_.slots[s], image.data());
				overlay(image, 0, *in.player_sav,
						playersav::kHeaderBytes + s * playersav::kPlayerRecordBytes,
						playersav::kPlayerRecordBytes);
				playersav::ProfileRecord record = playersav::read_record(image.data());
				// [orig: @0x54f600..0x54f62a — the defaults merged with the read
				//  table, stored with the default count]
				record.bindings = merge_bindings(defaults.bindings, record.bindings);
				player_.slots[s] = std::move(record);
			}
		}
	} else {
		first_run_ = true;  // [orig: @0x54f645]
	}
	header_byte_ = static_cast<uint8_t>(header_byte_ + (extra >> 24));  // [orig: @0x54f653]
	select_slot(saved_slot);  // [orig: @0x54f65d..0x54f66b]

	// weapon.sav [orig: @0x54f6c7..0x54f71e]: the header gate, then the five
	// records read over the seeded ones.
	if (in.weapon_sav != nullptr) {
		const Header h = read_header(*in.weapon_sav);
		if (h.magic == kMagic && h.version == kVersion) {
			std::vector<uint8_t> image = playersav::write(weapons_);
			overlay(image, playersav::kHeaderBytes, *in.weapon_sav, playersav::kHeaderBytes,
					playersav::kProfileSlots * playersav::kRecordBytes);
			playersav::File file;
			if (playersav::read(image.data(), image.size(), file)) {
				// The header words are the image's: the weapon.sav load keeps
				// neither.
				file.flags = weapons_.flags;
				file.extra = weapons_.extra;
				weapons_ = std::move(file);
			}
		}
	}
}

std::vector<uint8_t> PlayerProfiles::player_sav_bytes() const {
	// [orig: PlayerProfile_SaveToFiles @0x54be00 — "FPBC0211" @0x54be2e, the
	//  extra word's high byte @0x54be3d, the flags bit @0x54be43]
	playersav::PlayerSav out = player_;
	out.flags = header_latched_ ? playersav::kHeaderFlagLatched : 0u;
	out.extra = static_cast<uint32_t>(header_byte_) << 24;
	return playersav::write(out);
}

std::vector<uint8_t> PlayerProfiles::weapon_sav_bytes() const {
	// The same header, then the five weapon records [orig: @0x54beec..0x54bf1a].
	playersav::File out = weapons_;
	out.flags = header_latched_ ? playersav::kHeaderFlagLatched : 0u;
	out.extra = static_cast<uint32_t>(header_byte_) << 24;
	return playersav::write(out);
}

bool PlayerProfiles::rename(const std::string &text) {
	if (!text.empty() && !name_char_allowed(text.back())) return false;
	// [orig: strncpy(+4, name, 0x10) @0x55fc91; +52 &= ~1 @0x55fc9b]
	current().name = text.substr(0, playersav::kNameMaxChars);
	current().flags &= ~playersav::kRecordFlagNoName;
	return true;
}

void PlayerProfiles::commit_name(const std::string &text) {
	// The edit's text into the 16-byte field [orig: @0x55efc8], then the
	// white-space test [orig: @0x55efe1..0x55f01e; +52 |= 1 @0x55f02a, +4 = 0
	// @0x55f039].
	std::string name = text.substr(0, playersav::kNameMaxChars - 1);
	const size_t nul = name.find('\0');
	if (nul != std::string::npos) name.resize(nul);
	const auto space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
	if (name.empty() || space(name.front()) || space(name.back())) {
		current().flags |= playersav::kRecordFlagNoName;
		current().name.clear();
		return;
	}
	current().name = std::move(name);
}

void PlayerProfiles::reset_current(const ProfileDefaults &defaults) {
	// [orig: sub_561400 — g_curProfileSlot < 5 @0x561412, InitDefaults
	//  @0x561435, +52 |= 1 @0x561442]
	init_defaults(current(), current_weapons(), defaults);
	current().flags |= playersav::kRecordFlagNoName;
}

void PlayerProfiles::record_mission_start(int32_t campaign) {
	current().last_campaign = campaign;  // [orig: @0x561b99]
}

bool PlayerProfiles::record_round_end(bool in_session, bool spawn_gate, int32_t campaign,
		int32_t campaign_mission, bool won) {
	if (!in_session) {
		if (!spawn_gate) return false;  // [orig: @0x54d6c6]
		if (campaign >= 0 && won) {
			// [orig: +64 + 32 * campaign + mission = 1 @0x54d70a; +++1340
			//  @0x54d712]. A place past the table, which the original writes
			//  unchecked, is dropped.
			if (campaign < static_cast<int32_t>(playersav::kCampaigns) && campaign_mission >= 0 &&
					campaign_mission < static_cast<int32_t>(playersav::kCampaignMissions))
				current().campaign_complete[static_cast<size_t>(campaign)]
						[static_cast<size_t>(campaign_mission)] = 1;
			++current().campaigns_won;
		}
	}
	current().word_1460 = session_word_1460_;  // [orig: @0x54d724]
	return true;
}

void PlayerProfiles::clear_intro_pending() {
	for (playersav::ProfileRecord &record : player_.slots) record.intro_pending = 0;
}

}  // namespace opennova::profile
