// The chat filter's tokenizer, its rule tables and the line test -- see chat_filter.h.

#include <runtime/inmatch/chat_filter.h>

#include <formats/cbin/binary_config.h> // config_file_sections (ConfigFile_LoadFromFile's dispatch)
#include <formats/configfile/config_file.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace opennova::inmatch {

namespace {

// The tokenizer's patterns: the text matched at the cursor without case, the canonical token the
// rules store (an empty one records nothing), and three flag words. `pre` 4: the pattern needs the
// cursor just past a space; 8: anywhere else; 0x10: just past a space with a consonant before it
// (never both, so such a pattern never matches). `post` 4: a consonant next and a vowel (no u)
// after it; 0x40: a space or the end next; 0x80: neither; 0x10: e / i next; 0x20: no e / i next.
// `extra` 2: skipped in a load's tokenizing; 1: an alternate, taken by the pass's bit (unless 4 in
// a load's), which runs the passes.
// [orig: the table @0x84FDC8, 32-byte entries (the text @+0, the token @+4, pre @+20, post @+24,
//  extra @+28) to the zero entry @0x8506C8; the token strings the scorer and the loaders read
//  @0x84FDCC + 32 * index]
struct Pattern {
	const char *text;
	const char *token;
	uint32_t pre;
	uint32_t post;
	uint32_t extra;
};
constexpr Pattern kPatterns[] = {
	{ "1", "I", 0x0u, 0x4u, 0x0u },
	{ "1e", "E", 0x0u, 0x80u, 0x0u },
	{ "1", "i", 0x0u, 0x0u, 0x0u },
	{ "|", "I", 0x0u, 0x4u, 0x0u },
	{ "|e", "E", 0x0u, 0x80u, 0x0u },
	{ "|", "i", 0x0u, 0x0u, 0x0u },
	{ "0", "O", 0x0u, 0x4u, 0x0u },
	{ "00", "U", 0x0u, 0x0u, 0x0u },
	{ "0u", "U", 0x0u, 0x40u, 0x0u },
	{ "0u", "ow", 0x0u, 0x0u, 0x0u },
	{ "0w", "ow", 0x0u, 0x0u, 0x0u },
	{ "0", "o", 0x0u, 0x0u, 0x0u },
	{ "ai", "A", 0x0u, 0x0u, 0x0u },
	{ "ay", "A", 0x10u, 0x0u, 0x0u },
	{ "ah", "u", 0x0u, 0x0u, 0x1u },
	{ "a", "A", 0x0u, 0x4u, 0x1u },
	{ "a", "a", 0x0u, 0x0u, 0x0u },
	{ "e", "E", 0x0u, 0x4u, 0x0u },
	{ "e", "", 0x10u, 0x40u, 0x0u },
	{ "ey", "A", 0x10u, 0x0u, 0x0u },
	{ "ei", "E", 0x0u, 0x80u, 0x0u },
	{ "ei", "A", 0x0u, 0x40u, 0x0u },
	{ "e", "E", 0x0u, 0x0u, 0x1u },
	{ "e", "e", 0x0u, 0x0u, 0x0u },
	{ "i", "I", 0x0u, 0x4u, 0x1u },
	{ "ie", "E", 0x0u, 0x0u, 0x0u },
	{ "i", "i", 0x0u, 0x0u, 0x0u },
	{ "o", "O", 0x0u, 0x4u, 0x0u },
	{ "oo", "U", 0x0u, 0x0u, 0x0u },
	{ "ou", "U", 0x0u, 0x40u, 0x0u },
	{ "ou", "ow", 0x0u, 0x0u, 0x0u },
	{ "ow", "ow", 0x0u, 0x0u, 0x0u },
	{ "o", "o", 0x0u, 0x0u, 0x0u },
	{ "u", "U", 0x0u, 0x4u, 0x1u },
	{ "u", "u", 0x0u, 0x0u, 0x0u },
	{ "y", "E", 0x8u, 0x0u, 0x1u },
	{ "y", "y", 0x8u, 0x0u, 0x0u },
	{ "b", "b", 0x0u, 0x0u, 0x0u },
	{ "ch", "ch", 0x0u, 0x0u, 0x5u },
	{ "c", "k", 0x0u, 0x20u, 0x0u },
	{ "ck", "k", 0x0u, 0x0u, 0x0u },
	{ "c", "s", 0x0u, 0x10u, 0x1u },
	{ "c", "k", 0x0u, 0x0u, 0x0u },
	{ "d", "d", 0x0u, 0x0u, 0x0u },
	{ "f", "f", 0x0u, 0x0u, 0x0u },
	{ "gg", "g", 0x0u, 0x0u, 0x0u },
	{ "g", "g", 0x0u, 0x0u, 0x0u },
	{ "h", "", 0x0u, 0x0u, 0x3u },
	{ "h", "h", 0x0u, 0x0u, 0x0u },
	{ "j", "j", 0x0u, 0x0u, 0x0u },
	{ "k", "k", 0x0u, 0x0u, 0x0u },
	{ "ll", "l", 0x0u, 0x0u, 0x0u },
	{ "l", "l", 0x0u, 0x0u, 0x0u },
	{ "m", "m", 0x0u, 0x0u, 0x0u },
	{ "ng", "ng", 0x0u, 0x0u, 0x1u },
	{ "n", "n", 0x0u, 0x0u, 0x0u },
	{ "ph", "f", 0x0u, 0x0u, 0x0u },
	{ "p", "p", 0x0u, 0x0u, 0x0u },
	{ "q", "k", 0x0u, 0x0u, 0x0u },
	{ "r", "r", 0x0u, 0x0u, 0x0u },
	{ "sh", "sh", 0x0u, 0x0u, 0x5u },
	{ "sch", "sh", 0x0u, 0x0u, 0x1u },
	{ "s", "s", 0x0u, 0x0u, 0x0u },
	{ "th", "th", 0x0u, 0x0u, 0x1u },
	{ "tch", "ch", 0x0u, 0x0u, 0x1u },
	{ "t", "t", 0x0u, 0x0u, 0x0u },
	{ "v", "v", 0x0u, 0x0u, 0x0u },
	{ "wh", "wh", 0x0u, 0x0u, 0x0u },
	{ "w", "w", 0x0u, 0x0u, 0x0u },
	{ "x", "x", 0x0u, 0x0u, 0x0u },
	{ "y", "y", 0x4u, 0x0u, 0x0u },
	{ "z", "z", 0x0u, 0x0u, 0x0u },
};
constexpr int kPatternCount = static_cast<int>(sizeof(kPatterns) / sizeof(kPatterns[0]));
static_assert(kPatternCount == 72, "the table @0x84FDC8 holds 72 patterns");

// The iteration caps: the global's initial 16, which the boot's dictionary load runs under (it
// loads ahead of any chat), and the 4 every chat test sets ahead of its scoring.
// [orig: dword_8506E8 (16 in the image); ChatFilter_SetMaxIterations @0x68AB10 (the clamp to 16
//  @0x68AB17), its one call ChatFilter_IsTextFiltered @0x4FD655 (4)]
constexpr int32_t kLoadIterations = 16;
constexpr int32_t kChatIterations = 4;

// The token array's 256 entries [orig: memset(ctx, 0, 0x1400) @0x689C67, 20-byte entries].
constexpr int kTokenSlots = 256;

struct Token {
	int32_t pattern = 0; // +0, the pattern index
	int32_t excluded = 0; // +16, an exclusion's mark
};

// One tokenizing pass: the tokens and the alternation's words.
// [orig: ctx+0x1400 the count; ctx+8220 the pass, ctx+8224 the bit's divisor, ctx+8228 the
//  alternates taken, ctx+8232 the alternates met]
struct Pass {
	// The entries and a zero one past the last, which ChatFilter_RemoveDuplicateTokens' shift reads.
	std::array<Token, kTokenSlots + 1> tokens{};
	int count = 0;
	int32_t pass = 0;
	int32_t divisor = 1;
	int32_t taken = 0;
	int32_t met = 0;
	// An entry past the array reads as a zero one (retail reads its own context words there).
	const Token &at(int index) const {
		static const Token kZero{};
		return index >= 0 && index < static_cast<int>(tokens.size())
				? tokens[static_cast<size_t>(index)]
				: kZero;
	}
};

const char *token_of(int32_t pattern) {
	return pattern >= 0 && pattern < kPatternCount ? kPatterns[pattern].token : "";
}

// [orig: ChatFilter_IsConsonant @0x689BF0]: a letter A-Z / a-z other than a vowel; '1' and a space never.
bool is_consonant(char c) {
	if (c == '1' || c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'A' ||
			c == 'E' || c == 'I' || c == 'O' || c == 'U' || c == ' ')
		return false;
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// [orig: ChatFilter_IsVowel @0x689AE0]: '1', '0', '|' and the vowels, either case.
bool is_vowel(char c) {
	return c == '1' || c == '0' || c == '|' || c == 'a' || c == 'e' || c == 'i' || c == 'o' ||
			c == 'u' || c == 'A' || c == 'E' || c == 'I' || c == 'O' || c == 'U';
}

// [orig: ChatFilter_IsVowelNoU @0x689B70]: the same, u and U excepted.
bool is_vowel_no_u(char c) {
	return c == '1' || c == '0' || c == '|' || c == 'a' || c == 'e' || c == 'i' || c == 'o' ||
			c == 'A' || c == 'E' || c == 'I' || c == 'O';
}

char ascii_lower(char c) {
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}
char ascii_upper(char c) {
	return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

// _strnicmp(text, pattern, strlen(pattern)) == 0. The patterns are ASCII, so the locale's fold of a
// non-ASCII byte never meets one.
bool prefix_matches(const char *text, const char *pattern) {
	for (; *pattern != 0; ++text, ++pattern)
		if (ascii_lower(*text) != ascii_lower(*pattern))
			return false;
	return true;
}

// [orig: ChatFilter_TokenizeText @0x689C50]
void tokenize(Pass &p, const char *input, bool load, int32_t max_iterations) {
	p.tokens.fill(Token{}); // @0x689C67
	p.count = 0; // @0x689C69
	const char *cursor = input;
	const char *token_start = input;
	// 1 at the start and past every token, 0 just past a space [orig: var_C @0x689C78, @0x689C93,
	//  @0x689F6A].
	bool past_token = true;
	if (*input == 0)
		return; // @0x689C80
	do {
		if (*cursor == ' ') { // @0x689C8E..0x689C9F
			++cursor;
			past_token = false;
			token_start = cursor;
			continue;
		}
		const Pattern *matched = nullptr;
		for (const Pattern &pattern : kPatterns) {
			if (matched != nullptr)
				break; // the head test @0x689CBB
			if (!prefix_matches(cursor, pattern.text))
				continue; // @0x689CDD
			const size_t length = std::strlen(pattern.text);
			if ((pattern.pre & 4u) != 0 && past_token)
				continue; // @0x689CF8..0x689CFE
			if ((pattern.pre & 8u) != 0 && !past_token)
				continue; // @0x689D04..0x689D0A
			if ((pattern.pre & 0x10u) != 0 && // @0x689D10..0x689D34
					(past_token || cursor == input || !is_consonant(cursor[-1])))
				continue;
			// The next consonant or vowel past the pattern, then the one past that
			// [orig: @0x689D3A..0x689D77, @0x689D79..0x689DB0].
			const char *next = cursor + length;
			while (!is_consonant(*next) && !is_vowel(*next) && *next != 0)
				++next;
			const char *after = *next != 0 ? next + 1 : next;
			while (!is_consonant(*after) && !is_vowel(*after) && *after != 0)
				++after;
			if ((pattern.post & 4u) != 0 && !(is_consonant(*next) && is_vowel_no_u(*after)))
				continue; // @0x689DB6..0x689DD7
			const bool end_next = *next == ' ' || *next == 0;
			if ((pattern.post & 0x40u) != 0 && !end_next)
				continue; // @0x689DE3..0x689DF0
			if ((pattern.post & 0x80u) != 0 && end_next)
				continue; // @0x689DF6..0x689E07
			const bool e_or_i = *next == 'e' || *next == 'i' || *next == 'E' || *next == 'I';
			if ((pattern.post & 0x10u) != 0 && !e_or_i)
				continue; // @0x689E0D..0x689E22
			if ((pattern.post & 0x20u) != 0 && e_or_i)
				continue; // @0x689E28..0x689E49
			if ((pattern.extra & 2u) != 0 && load)
				continue; // @0x689E56..0x689E60
			// An alternate: past the cap taken, else by bit `met` of the pass number, the divisor
			// doubling [orig: @0x689E62..0x689EB3 - the cap @0x689E78..0x689E80, idiv @0x689E96,
			//  the bit's sign fold @0x689E98..0x689EA3, the double @0x689EA8, ++taken @0x689EB3].
			if ((pattern.extra & 1u) != 0 && !((pattern.extra & 4u) != 0 && load) &&
					p.met < max_iterations) {
				++p.met;
				const int32_t bit = (p.pass / p.divisor) % 2;
				p.divisor *= 2;
				if (bit != 0)
					continue;
				++p.taken;
			}
			matched = &pattern; // @0x689EB9..0x689ED2
			token_start += length;
			cursor = token_start;
		}
		if (matched == nullptr) { // @0x689EF0..0x689EF2 -> @0x689F65
			token_start = ++cursor;
			past_token = true;
			continue;
		}
		// A pattern with a token records its index (the start and end offsets have no reader);
		// retail writes past its 256 entries into its own words, which ours stops at.
		// [orig: @0x689EF4..0x689F5D]
		if (matched->token[0] != 0 && p.count < kTokenSlots) {
			p.tokens[static_cast<size_t>(p.count)].pattern =
					static_cast<int32_t>(matched - kPatterns);
			p.tokens[static_cast<size_t>(p.count)].excluded = 0;
			++p.count;
		}
		past_token = true; // @0x689F6A
	} while (*cursor != 0); // @0x689F72
}

// A token next to one of the same token string merges into it: the run keeps its first entry and
// the entries past it move down one [orig: ChatFilter_RemoveDuplicateTokens @0x689F90 - prev -999
// @0x689F98, the compares @0x689FB8..0x68A018, the shift @0x68A02C..0x68A044, the count
// @0x68A04E..0x68A056].
void remove_duplicates(Pass &p) {
	int32_t prev = -999;
	for (int i = 0; i < p.count; ++i) {
		const int32_t current = p.tokens[static_cast<size_t>(i)].pattern;
		const bool same = current == prev ||
				(current >= 0 && prev >= 0 && std::strcmp(token_of(current), token_of(prev)) == 0);
		if (same) {
			for (int k = i; k < p.count; ++k)
				p.tokens[static_cast<size_t>(k)] = p.tokens[static_cast<size_t>(k) + 1];
			--p.count;
			--i;
		}
		prev = current;
	}
}

// strcmp(token, the table's slot string at `at`): the slot runs to its NUL (a table past its end
// reads 0).
bool slot_equals(const std::vector<uint8_t> &table, size_t at, const char *token) {
	for (size_t k = 0;; ++k) {
		const uint8_t stored = at + k < table.size() ? table[at + k] : 0;
		const uint8_t wanted = static_cast<uint8_t>(token[k]);
		if (stored != wanted)
			return false;
		if (wanted == 0)
			return true;
	}
}

// The first exclusion whose token slots match the tokens from `i`, its slots up to its first empty
// one, marks those tokens and ends the walk. (Retail's reverse arm skips the walk; no caller sets
// it.)
// [orig: ChatFilter_MarkExclusions @0x68A260 - the records @0x68A27B..0x68A2EB (stride 0x4C), the slots
//  @0x68A293..0x68A2D9, the marks @0x68A2F3..0x68A314]
void mark_exclusions(Pass &p, const std::vector<uint8_t> &table, int i) {
	for (size_t record = 0; record + ChatFilter::kRuleBytes <= table.size() && table[record] != 0;
			record += ChatFilter::kRuleBytes) {
		int matched = 0;
		size_t slot = record;
		bool whole = true;
		while (slot < table.size() && table[slot] != 0) {
			if (!slot_equals(table, slot, token_of(p.at(i + matched).pattern))) {
				whole = false;
				break;
			}
			++matched;
			slot += 4;
		}
		if (!whole)
			continue;
		for (int k = 0; k < matched; ++k)
			if (i + k < static_cast<int>(p.tokens.size()))
				p.tokens[static_cast<size_t>(i + k)].excluded = 1;
		return;
	}
}

// Every word rule whose token slots match the tokens from `i`, not every one of those tokens
// excluded, counts its O / R / S bits. (The reverse arm's skip of an F rule has no caller; the
// matched rows the game also records have no reader.)
// [orig: ChatFilter_ScoreWordRules @0x68A080 - the records @0x68A0A5..0x68A250, the slots
//  @0x68A10D..0x68A16B, the excluded test @0x68A16D, the counts @0x68A1D9..0x68A1F3]
void score_rules(const Pass &p, const std::vector<uint8_t> &table, int i, ChatFilterCounts &out) {
	for (size_t record = 0; record + ChatFilter::kRuleBytes <= table.size() && table[record] != 0;
			record += ChatFilter::kRuleBytes) {
		int32_t all_excluded = 1;
		int matched = 0;
		size_t slot = record;
		bool mismatch = false;
		do {
			const Token &token = p.at(i + matched);
			if (!slot_equals(table, slot, token_of(token.pattern))) {
				mismatch = true;
				break;
			}
			all_excluded &= token.excluded;
			slot += 4;
			++matched;
		} while (slot < table.size() && table[slot] != 0);
		if (mismatch || all_excluded != 0)
			continue;
		const uint8_t flags = table[record + 64];
		if ((flags & 1u) != 0)
			++out.o;
		if ((flags & 2u) != 0)
			++out.r;
		if ((flags & 4u) != 0)
			++out.s;
	}
}

// One filter.txt row into a table: its trailing CR then LF cut, then a record per alternation pass,
// each the pass's tokens' strings in four-byte slots, a NUL past the last, the flags dword at +64;
// a word's records then take the weight 1.0f at +68. A record past the table, or a slot past it, is
// not written (retail writes on into the memory beyond).
// [orig: ChatFilter_AddWordRecords @0x68A350 (the words) / ChatFilter_AddExclusionRecords
//  @0x68A4B0 (the exclusions) - the CR / LF @0x68A367..0x68A39D, the table's end
//  @0x68A3B3..0x68A3BB, the passes @0x68A3D7..0x68A487, the strcpy into each slot @0x68A435, the
//  NUL @0x68A464, the flags @0x68A468, the weights @0x68A496]
void collect(std::vector<uint8_t> &table, std::string word, int32_t flags, bool weighted) {
	if (!word.empty() && word.back() == '\r')
		word.pop_back();
	if (!word.empty() && word.back() == '\n')
		word.pop_back();
	size_t record = 0;
	if (!table.empty() && table[0] != 0) {
		do
			record += ChatFilter::kRuleBytes;
		while (record < table.size() && table[record] != 0);
	}
	const size_t first = record;
	Pass p;
	p.pass = 0;
	do {
		p.divisor = 1;
		p.taken = 0;
		p.met = 0;
		tokenize(p, word.c_str(), /*load=*/true, kLoadIterations);
		remove_duplicates(p);
		const auto put = [&table](size_t at, uint8_t byte) {
			if (at < table.size())
				table[at] = byte;
		};
		for (int k = 0; k < p.count; ++k) {
			const char *token = token_of(p.tokens[static_cast<size_t>(k)].pattern);
			const size_t slot = record + 4 * static_cast<size_t>(k);
			const size_t length = std::strlen(token);
			for (size_t c = 0; c <= length; ++c)
				put(slot + c, static_cast<uint8_t>(token[c]));
		}
		put(record + 4 * static_cast<size_t>(p.count), 0);
		for (int b = 0; b < 4; ++b)
			put(record + 64 + static_cast<size_t>(b),
					static_cast<uint8_t>(static_cast<uint32_t>(flags) >> (8 * b)));
		record += ChatFilter::kRuleBytes;
		++p.pass;
	} while (p.taken != 0);
	if (!weighted)
		return;
	constexpr uint8_t kOne[4] = { 0x00, 0x00, 0x80, 0x3F }; // 1.0f
	for (size_t at = first; at < record; at += ChatFilter::kRuleBytes)
		for (int b = 0; b < 4; ++b)
			if (at + 68 + static_cast<size_t>(b) < table.size())
				table[at + 68 + static_cast<size_t>(b)] = kOne[b];
}

// strtok(s, ",") over a row: the next run of non-commas from `at`, `at` moved past its delimiter.
// [orig: _strtok @0x76B47B with asc_8506F8 ","]
bool next_token(const std::string &line, size_t &at, std::string &out) {
	while (at < line.size() && line[at] == ',')
		++at;
	if (at >= line.size())
		return false;
	const size_t end = line.find(',', at);
	out = line.substr(at, end == std::string::npos ? std::string::npos : end - at);
	at = end == std::string::npos ? line.size() : end + 1;
	return true;
}

} // namespace

ChatFilter::ChatFilter() : words_(kRules * kRuleBytes, 0), exclusions_(kRules * kRuleBytes, 0) {}

// [orig: ChatFilter_LoadDictionaryFile @0x68A890]
void ChatFilter::load(const uint8_t *data, std::size_t size) {
	std::fill(words_.begin(), words_.end(), uint8_t{ 0 }); // memset @0x68A8A3
	// No file, or no byte, loads nothing.
	// [orig: IniFile_LoadGlobal @0x68A8AD -> ConfigFile_LoadFromFile @0x760A10 (the exists test,
	//  the read)]
	if (data == nullptr || size == 0)
		return;
	std::vector<configfile::ConfigSection> sections;
	if (!cbin::config_file_sections(data, size, sections))
		return;
	// The rows read 127 characters at most.
	// [orig: ConfigFile_ReadStringGlobal(.., 128, ..) @0x68A8E3]
	constexpr size_t kRowBytes = 128;
	// The flags of the last row that had any: a row without a flags token adds nothing and keeps
	// them, and the exclusions are written with them. Retail's first value is an uninitialised
	// stack word; ours 0.
	int32_t flags = 0;
	// The row's word and its flags as the frame holds them: flags_str's 16 bytes and word_lower's
	// 128 after them, so a flags token past 15 characters runs on into the word.
	// [orig: ChatFilter_LoadDictionaryFile - flags_str @esp+8, word_lower @esp+0x18]
	std::array<char, 16 + kRowBytes + 1> frame{};
	char *const flags_str = frame.data();
	char *const word_lower = frame.data() + 16;
	const auto copy_into = [&frame](char *to, const std::string &from) {
		const size_t room = static_cast<size_t>(frame.data() + frame.size() - 1 - to);
		const size_t n = std::min(from.size(), room);
		std::memcpy(to, from.data(), n);
		to[n] = 0;
	};
	std::string row;
	// [orig: ConfigFile_FindSectionGlobal("WORDS") @0x68A8D4, ConfigFile_ReadStringGlobal("ITEM")
	//  @0x68A8E3, the loop @0x68A900..0x68AA11: strtok @0x68A911, strcpy + strlwr @0x68A93B /
	//  @0x68A949, strtok @0x68A955, strcpy + strupr @0x68A97F / @0x68A98D, the O / R / S / F
	//  strchr @0x68A99A..0x68A9EB, ChatFilter_AddWordRecords @0x68A9F5]
	if (configfile::ConfigSection *words = configfile::find_config_section(sections, "WORDS")) {
		while (configfile::read_binary_entry(*words, "ITEM", nullptr, &row, kRowBytes)) {
			size_t at = 0;
			std::string token;
			if (!next_token(row, at, token))
				continue;
			copy_into(word_lower, token);
			for (char *c = word_lower; *c != 0; ++c)
				*c = ascii_lower(*c);
			std::string flags_token;
			if (!next_token(row, at, flags_token))
				continue;
			copy_into(flags_str, flags_token);
			for (char *c = flags_str; *c != 0; ++c)
				*c = ascii_upper(*c);
			flags = std::strchr(flags_str, 'O') != nullptr ? 1 : 0;
			if (std::strchr(flags_str, 'R') != nullptr)
				flags |= 2;
			if (std::strchr(flags_str, 'S') != nullptr)
				flags |= 4;
			if (std::strchr(flags_str, 'F') != nullptr)
				flags |= 8;
			collect(words_, std::string(word_lower), flags, /*weighted=*/true);
		}
	}
	// [orig: ConfigFile_FindSectionGlobal("EXCLUSIONS") @0x68AA34, the loop @0x68AA5C..0x68AAC5:
	//  strtok @0x68AA69, strcpy + strlwr @0x68AA8F / @0x68AA9D, ChatFilter_AddExclusionRecords
	//  @0x68AAA9]
	if (configfile::ConfigSection *exclusions =
					configfile::find_config_section(sections, "EXCLUSIONS")) {
		while (configfile::read_binary_entry(*exclusions, "ITEM", nullptr, &row, kRowBytes)) {
			size_t at = 0;
			std::string token;
			if (!next_token(row, at, token))
				continue;
			copy_into(word_lower, token);
			for (char *c = word_lower; *c != 0; ++c)
				*c = ascii_lower(*c);
			collect(exclusions_, std::string(word_lower), flags, /*weighted=*/false);
		}
	}
}

// [orig: ChatFilter_ScoreLine @0x68A5F0 - the counts cleared @0x68A603..0x68A615, strcpy @0x68A630,
//  the CR / LF @0x68A64D..0x68A684, the pass @0x68A69E, the loop @0x68A6C5..0x68A786 (the
//  divisor 1 @0x68A6C5, the alternates @0x68A6CF / @0x68A6D5, ChatFilter_TokenizeText(.., 0) @0x68A6DB,
//  ChatFilter_RemoveDuplicateTokens @0x68A6E1, ChatFilter_MarkExclusions per token @0x68A6F8, ChatFilter_ScoreWordRules
//  per token @0x68A720, ++pass @0x68A73A, the text copied back unchanged @0x68A749 / @0x68A765, the
//  end on no alternate taken or the cap @0x68A786), the counts out @0x68A7A3..0x68A7CB]
ChatFilterCounts ChatFilter::score(const std::string &text) const {
	ChatFilterCounts out;
	std::string work(text.c_str());
	if (!work.empty() && work.back() == '\r')
		work.pop_back();
	if (!work.empty() && work.back() == '\n')
		work.pop_back();
	Pass p;
	p.pass = 0;
	for (;;) {
		p.divisor = 1;
		p.taken = 0;
		p.met = 0;
		tokenize(p, work.c_str(), /*load=*/false, kChatIterations);
		remove_duplicates(p);
		for (int i = 0; i < p.count; ++i)
			mark_exclusions(p, exclusions_, i);
		for (int i = 0; i < p.count; ++i)
			score_rules(p, words_, i, out);
		++p.pass;
		if (p.taken == 0 || p.pass >= kChatIterations)
			break;
	}
	return out;
}

bool ChatFilter::is_text_filtered(const std::string &text) const {
	const ChatFilterCounts counts = score(text);
	return counts.o != 0 || counts.r != 0 || counts.s != 0;
}

} // namespace opennova::inmatch
