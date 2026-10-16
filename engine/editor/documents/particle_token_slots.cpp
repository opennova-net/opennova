#include "particle_token_slots.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

#include <base/io/strutil.h>
#include <formats/particle/particle.h>

namespace opennova::editor {

namespace {

constexpr size_t kLineMax = 1000; // [orig: @ 0x53CBBB]
constexpr int kSlots = 30;        // [orig: @ 0x53CC8C]
constexpr int kNull = -1;         // a slot no line reached
constexpr int kEmpty = -2;        // the "" each line puts in slots 0 to 2 [orig: @ 0x53CB77..0x53CB81]
constexpr char kStrip[] = ";,=";  // [orig: @ 0x5E897C, 0x3D2C3B]

// The tokenizer's global state, as the game keeps it from one line to the next.
class Tokenizer {
public:
	Tokenizer() { slot_.fill(kNull); }

	// One line, as the walk hands it (its CR cut off) [orig: @ 0x53CB60].
	void line(std::string_view text) {
		count_ = 0;
		slot_[0] = slot_[1] = slot_[2] = kEmpty;
		size_t at = 0;
		while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
		text.remove_prefix(at);
		// strlen: a NUL in the line ends it.
		text = text.substr(0, std::min(text.find('\0'), text.size()));
		if (text.empty()) return;
		const size_t length = std::min(text.size(), kLineMax);
		std::memcpy(buffer_.data(), text.data(), length);
		buffer_[length] = 0;
		bool quoted = false, start = true;
		for (size_t i = 0; i < length; ++i) {
			const char c = buffer_[i];
			if (c == '/' && !quoted && buffer_[i + 1] == '/') break;
			if (c == ';' && !quoted) break;
			if ((c == ' ' || c == ',' || c == '\t') && !quoted) {
				buffer_[i] = 0;
				start = true;
			} else if (c == '"') {
				buffer_[i] = 0;
				start = true;
				quoted = !quoted;
			} else if (start) {
				slot_[size_t(count_++)] = int(i);
				start = false;
			}
			if (count_ >= kSlots) break;
		}
		high_ = std::max(high_, count_);
	}

	int count() const { return count_; }
	int high() const { return high_; }
	int slot(int index) const { return index < kSlots ? slot_[size_t(index)] : kNull; }
	// The C string a slot points at now (its bytes as the buffer holds them).
	std::string text(int at) const { return at == kEmpty ? std::string() : std::string(buffer_.data() + at); }

	// The strip of ';' ',' '=' from the token at `at`, in place, `c` the last it took [orig: sub_5DFF00 @ 0x5DFF00].
	void strip(int at, char &c) {
		if (at == kEmpty) return;
		char *token = buffer_.data() + at;
		for (const char k : std::string_view(kStrip)) {
			const size_t length = std::strlen(token);
			char *found = std::strchr(token, k);
			if (!found) continue;
			const size_t pos = size_t(found - token);
			c = token[pos];
			if (pos < length) std::memmove(token + pos, token + pos + 1, length - pos);
			if (std::strchr(token, k)) strip(at, c);
		}
	}

private:
	std::array<char, kLineMax + 1> buffer_{};
	std::array<int, kSlots> slot_{};
	int count_ = 0;
	int high_ = 0;
};

// The parsers' reads of one statement [orig: CParticleDefEntry_NextToken @ 0x5DFFB0]: the next slot, a lone ';' ',' or
// '=' skipped, the token stripped, the statement ended at a stripped ';'.
class Reads {
public:
	enum class Got { Token, End, Null, Outside };

	explicit Reads(Tokenizer &tokens) : tokens_(tokens) {}

	Got next(std::string &token) {
		if (ended_) return Got::End;
		++index_;
		if (index_ >= kSlots) return Got::Outside;
		const int at = tokens_.slot(index_);
		if (at == kNull) return Got::Null;
		token = tokens_.text(at);
		if (token.size() == 1 && std::strchr(kStrip, token[0])) return next(token);
		char c = '0';
		tokens_.strip(at, c);
		token = tokens_.text(at);
		if (c == ';') ended_ = true;
		return Got::Token;
	}
	int index() const { return index_; }

private:
	Tokenizer &tokens_;
	int index_ = 0; // the key's slot
	bool ended_ = false;
};

bool flag_name(const std::string &token) { return particle::is_particle_flag_name(token); }
bool move_name(const std::string &token) { return particle::is_move_flag_name(token); }

enum class Section { None, Effect, Particle, Table };

} // namespace

std::vector<ParticleSlotFault> particle_token_slot_faults(const std::vector<ParticleLoadFile> &files) {
	std::vector<ParticleSlotFault> faults;
	Tokenizer tokens;
	for (size_t f = 0; f < files.size(); ++f) {
		const std::vector<uint8_t> &bytes = files[f].bytes;
		const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
		Section section = Section::None;
		int rows = 0; // a table's rows read (32 at the most [orig: @ 0x5E4130])
		int line = 0;
		// The walk [orig: File_ParseASCIIFile @ 0x53D8C7..0x53D942]: each line to its CR LF, a last line with none
		// its last byte cut.
		for (size_t at = 0; at < text.size();) {
			++line;
			size_t end = text.find("\r\n", at);
			const bool last = end == std::string_view::npos;
			if (last) end = text.size() - 1;
			tokens.line(text.substr(at, end - at));
			at = last ? text.size() : end + 2;
			if (tokens.count() == 0) continue;
			const std::string key = tokens.text(tokens.slot(0));
			if (key[0] == '/') continue;
			// The section callback [orig: CEffectWorld_ParseSectionCallback @ 0x5ECB40]: a tag opens its section, the
			// edit handles' ends the file's walk.
			if (strutil::iequals(key, "[effectdef]")) section = Section::Effect;
			else if (strutil::iequals(key, "[particledef]")) section = Section::Particle;
			else if (strutil::iequals(key, "[tabledef]")) section = Section::Table;
			else if (strutil::iequals(key, "[tabledef_edithandles]")) break;
			else {
				Reads reads(tokens);
				std::string token;
				Reads::Got got = Reads::Got::End;
				const auto list = [&](bool (*member)(const std::string &)) {
					do got = reads.next(token);
					while (got == Reads::Got::Token && (!member || member(token)));
				};
				if (section == Section::Particle && strutil::iequals(key, "flags")) list(flag_name);
				else if (section == Section::Particle && strutil::iequals(key, "move")) list(move_name);
				else if (section == Section::Effect && strutil::iequals(key, "pdefs")) list(nullptr);
				else if (section == Section::Table && strutil::iequals(key, "id")) rows = 0;
				else if (section == Section::Table && key.find('t') != std::string::npos && rows < 32) {
					for (int value = 0; value < 8; ++value)
						if ((got = reads.next(token)) != Reads::Got::Token) break;
					++rows;
				}
				if (got == Reads::Got::Null || got == Reads::Got::Outside) {
					ParticleSlotFault fault;
					fault.file = f;
					fault.line = line;
					fault.key = key;
					fault.slot = reads.index();
					fault.filled = tokens.high();
					fault.outside = got == Reads::Got::Outside;
					faults.push_back(std::move(fault));
				}
			}
		}
	}
	return faults;
}

} // namespace opennova::editor
