#pragma once

// THE CHAT FLOOD TABLE every chat sender checks a line against before it
// sends: 16 recent lines `[u32 frame][char[64] text]`. A line longer than 59
// characters is cut to 59 first (`message[59] = 0` @0x498f80); an unseen line
// shifts the table down and lands in the newest slot (@0x498fe0..0x498fed); a
// seen line within 0x500 main frames of its entry is refused (@0x499028); an
// older repeat moves to the newest slot, the entries after it shifting down
// (@0x49904a). The clock is the per-main-frame counter the talk debounce reads
// (dword_A8705C), not wall time.
// [orig: Chat_CheckFloodControl @0x498F60 — the table @0xB3B788]

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace opennova::inmatch {

struct ChatFloodEntry {
	uint32_t frame = 0;
	std::string text;
};
using ChatFloodTable = std::array<ChatFloodEntry, 16>;

// True when `text` may be sent (and is recorded); `text` is cut to 59
// characters in place either way.
inline bool chat_flood_check(ChatFloodTable &table, std::string &text, uint32_t frame) {
	if (text.size() > 0x3B) text.resize(59);
	size_t index = 0;
	while (index < table.size() && table[index].text != text) ++index;
	if (index >= table.size()) {
		for (size_t i = 0; i + 1 < table.size(); ++i) table[i] = table[i + 1];
		table.back() = ChatFloodEntry{frame, text};
		return true;
	}
	if (frame - table[index].frame <= 0x500u) return false;
	for (size_t i = index; i + 1 < table.size(); ++i) table[i] = table[i + 1];
	table.back() = ChatFloodEntry{frame, text};
	return true;
}

} // namespace opennova::inmatch
