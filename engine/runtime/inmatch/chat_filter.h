#pragma once

// The chat filter: filter.txt's [WORDS] and [EXCLUSIONS] rows tokenized into rules by a sound-alike
// tokenizer, and the authority's test of a chat line against them. A line the filter scores goes
// back to its sender alone (server_chat.h Server_HandleChatMessage). The rules are retail's flat
// 76-byte records (16 four-byte token slots, the flags dword at +64, a weight float at +68),
// written and read byte for byte as retail does. The IDB's `TTS_*` names for this family are
// misnomers (no speech is made): the tokenizer's 72 patterns and the rules only ever feed these
// counts. [orig: Game_InitSubsystems @0x4A6DEC -> ChatFilter_LoadFromFile @0x4FD640 ->
//  ChatFilter_LoadDictionary @0x68AAF0 ("filter.txt") -> ChatFilter_LoadDictionaryFile @0x68A890;
//  NapiNPServer_HandleChatMessage @0x51382E -> ChatFilter_IsTextFiltered @0x4FD650 ->
//  ChatFilter_ScoreText @0x68A860 (FLIRT misname) -> ChatFilter_ScoreLineForward @0x68A830 ->
//  ChatFilter_ScoreLine @0x68A5F0; dfx2.exe's the same code (NapiNPServer_HandleChatMessage @0x510970 ->
//  sub_4FA730 (dfx2.exe), its token table byte_8430A8 (dfx2.exe) identical to Jointops' @0x84FDC8)]

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::inmatch {

// The counts a line scores: one per word rule matched in a pass, by the rule's filter.txt flag
// letters O (bit 1), R (bit 2) and S (bit 4), summed over the passes.
// [orig: ChatFilter_ScoreWordRules @0x68A080 - @0x68A1D9..0x68A1F3, ctx+8200 / +8204 / +8208]
struct ChatFilterCounts {
	int32_t o = 0;
	int32_t r = 0;
	int32_t s = 0;
};

class ChatFilter {
public:
	// One rule record and the word table's size [orig: ChatFilter_LoadDictionaryFile @0x68A890 -
	// memset(byte_32A6A10, 0, 0x4C000) @0x68A8A3; the 76-byte stride @0x68A3B6].
	static constexpr std::size_t kRuleBytes = 76;
	static constexpr std::size_t kRules = 0x4C000 / kRuleBytes;

	ChatFilter();

	// filter.txt's bytes (a ConfigFile, text or CBIN form) into the two rule tables: the word table
	// cleared first, each [WORDS] ITEM row "word,flags" with a flags token tokenized into rules
	// carrying its O / R / S / F bits, each [EXCLUSIONS] ITEM row's first token into the exclusion
	// table. No file (no bytes) leaves both tables empty, as a missing file does.
	// [orig: ChatFilter_LoadDictionaryFile @0x68A890]
	void load(const uint8_t *data, std::size_t size);

	// The authority's test: the iteration cap set to 4, then the line scored; true when any count
	// is nonzero [orig: ChatFilter_IsTextFiltered @0x4FD650 - ChatFilter_SetMaxIterations(4) @0x4FD655,
	// the three tests @0x4FD676..0x4FD688].
	bool is_text_filtered(const std::string &text) const;

	// The line's counts over the rules [orig: ChatFilter_ScoreLine @0x68A5F0, its reverse flag 0, the
	// only one ChatFilter_ScoreLineForward @0x68A830 passes].
	ChatFilterCounts score(const std::string &text) const;

	// The tables as retail holds them, for the tests.
	const std::vector<uint8_t> &words() const { return words_; }
	const std::vector<uint8_t> &exclusions() const { return exclusions_; }

private:
	std::vector<uint8_t> words_; // byte_32A6A10
	std::vector<uint8_t> exclusions_; // byte_32F2A10
};

} // namespace opennova::inmatch
