// A text file's modeled layout and the file generated over it (text_layout.h; the maintainer's ruling of
// 2026-10-04, "model it, generate it", ADR 0003 holding).
#include "text_layout.h"

#include <base/io/strutil.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <map>
#include <set>

namespace opennova::textlayout {
namespace {

bool blank(char c) { return c == ' ' || c == '\t'; }
// What ends a word outside quotes [orig: Terrain_TokenizeConfigLine @ 0x53CB60, the delimiters @
// 0x53CC33..0x53CC4C].
bool separator(char c) { return c == ' ' || c == '\t' || c == ','; }

std::atomic<uint32_t> g_stamp{0};

std::vector<std::string> words_of(Cut cut, const std::string &form) { return cut(form.data(), form.size()).words; }

// A word without the quotes around it, any case.
std::string bare_word(const std::string &word) {
	std::string out = word;
	if (!out.empty() && out.front() == '"') out.erase(out.begin());
	if (!out.empty() && out.back() == '"') out.pop_back();
	return strutil::to_lower(out);
}

bool number_of(const std::string &word, double &out) {
	if (word.empty()) return false;
	char *end = nullptr;
	out = std::strtod(word.c_str(), &end);
	return end == word.c_str() + word.size();
}

// Whether the file's word spells the writer's: the same word in any case, quoted or not, or the same number
// however it is written (`4.50` for `4.5`, `0.0` for `0`).
bool spells(const std::string &file, const std::string &writer) {
	if (bare_word(file) == bare_word(writer)) return true;
	double a = 0.0, b = 0.0;
	return number_of(file, a) && number_of(writer, b) && a == b;
}

// A line of the file's modeled against the writer's words for it as read: each of its words one of the
// writer's in its spelling (the same count: word for word; else each the next writer's word it spells, the
// writer's words passed over left out) or a token the reader reads nothing of.
Shape model_line(const std::vector<std::string> &file, const std::vector<std::string> &writer, size_t set_from) {
	Shape shape;
	shape.modeled = true;
	if (set_from < writer.size() && set_from <= file.size()) {
		// A set's line: its words before the set word for word, then each of the file's a member of the writer's
		// set it spells (the first not taken), or a token the reader reads nothing of.
		shape.set_from = set_from;
		for (size_t i = 0; i < set_from; ++i) shape.words.push_back({true, i, writer[i], file[i]});
		std::vector<bool> taken(writer.size(), false);
		for (size_t i = set_from; i < file.size(); ++i) {
			size_t found = set_from;
			while (found < writer.size() && (taken[found] || !spells(file[i], writer[found]))) ++found;
			if (found == writer.size()) {
				shape.words.push_back({false, 0, std::string(), file[i]});
				continue;
			}
			taken[found] = true;
			shape.words.push_back({true, found, writer[found], file[i]});
		}
		return shape;
	}
	if (file.size() == writer.size()) {
		for (size_t i = 0; i < file.size(); ++i) shape.words.push_back({true, i, writer[i], file[i]});
		return shape;
	}
	size_t next = 0;
	for (const std::string &word : file) {
		size_t found = next;
		while (found < writer.size() && !spells(word, writer[found])) ++found;
		if (found == writer.size()) {
			shape.words.push_back({false, 0, std::string(), word});
			continue;
		}
		for (size_t k = next; k < found; ++k) shape.left_out.emplace_back(k, writer[k]);
		shape.words.push_back({true, found, writer[found], word});
		next = found + 1;
	}
	for (size_t k = next; k < writer.size(); ++k) shape.left_out.emplace_back(k, writer[k]);
	return shape;
}

// A line's words joined by the file's separators (each where it stood, a blank where the line has more words
// than the file's had), between its blanks, its comment and its ending.
std::string joined(const Line &line, const std::vector<std::string> &words) {
	std::string out = line.indent;
	for (size_t i = 0; i < words.size(); ++i) {
		out += words[i];
		if (i + 1 < words.size()) out += i < line.gaps.size() ? line.gaps[i] : std::string(" ");
	}
	return out + line.tail + line.eol;
}

// A modeled line generated from the writer's words now: its words in the file's order, each of the writer's in
// the file's spelling while it is the word as read, else as the writer puts it down; the tokens the reader
// reads nothing of where they stood; a word of the writer's the file left out, left out while it is as read,
// else put down after the words before it. A line kept as its tokens takes the writer's words in its spacing.
std::string generate(const NotedLine &line, const std::vector<std::string> &writer) {
	if (!line.shape.modeled) return joined(line.line, writer);
	std::vector<std::string> out;
	std::vector<bool> placed(writer.size(), false);
	const auto left_out_as_read = [&line, &writer](size_t k) {
		for (const auto &[at, word] : line.shape.left_out)
			if (at == k && writer[k] == word) return true;
		return false;
	};
	const auto put_before = [&](size_t limit) {
		for (size_t k = 0; k < limit && k < writer.size(); ++k) {
			if (placed[k]) continue;
			placed[k] = true;
			if (!left_out_as_read(k)) out.push_back(writer[k]);
		}
	};
	const size_t set_from = line.shape.set_from;
	for (const Word &word : line.shape.words) {
		if (!word.written) {
			out.push_back(word.spelling);
			continue;
		}
		if (word.index >= set_from) {
			// A member of the set: its spelling stands while the writer still puts its word down, wherever.
			for (size_t k = set_from; k < writer.size(); ++k)
				if (!placed[k] && writer[k] == word.as_written) {
					placed[k] = true;
					out.push_back(word.spelling);
					break;
				}
			continue;
		}
		if (word.index >= writer.size() || placed[word.index]) continue; // the writer puts it down no more
		put_before(word.index);
		out.push_back(writer[word.index] == word.as_written ? word.spelling : writer[word.index]);
		placed[word.index] = true;
	}
	put_before(writer.size());
	return joined(line.line, out);
}

// The file's Entry lines of a record a writer's entry lines pair with: each writer's line with the reader's
// line of its entry (the last of several, or the first), a modeled line before one kept as its tokens. -1: none.
std::vector<int> pair_entries(const NotedRecord &record, const OutRecord &out) {
	std::vector<int> paired(out.lines.size(), -1);
	std::vector<bool> used(record.lines.size(), false);
	for (int pass = 0; pass < 2; ++pass) {
		for (size_t i = 0; i < out.lines.size(); ++i) {
			const OutLine &line = out.lines[i];
			if (line.child >= 0 || paired[i] >= 0 || line.entry.empty()) continue;
			int found = -1;
			for (size_t c = 0; c < record.lines.size(); ++c) {
				const NotedLine &file = record.lines[c];
				if (used[c] || file.role != Role::Entry || file.entry != line.entry) continue;
				if ((pass == 0) != file.shape.modeled) continue;
				found = int(c);
				if (!line.last) break;
			}
			if (found >= 0) {
				paired[i] = found;
				used[size_t(found)] = true;
			}
		}
	}
	return paired;
}

class Composer {
public:
	Composer(const Notes *notes, Cut cut, const std::string &eol) : notes_(notes), cut_(cut), eol_(eol) {}

	void emit(const OutRecord &record, std::string &out) {
		const NotedRecord *noted = notes_ && record.note ? notes_->record(record.note) : nullptr;
		if (noted && !emitted_.insert(record.note).second) noted = nullptr; // a second copy of a record: its own form
		if (!noted) {
			for (const OutLine &line : record.lines) {
				if (line.child >= 0) {
					if (size_t(line.child) < record.children.size()) emit(record.children[size_t(line.child)], out);
					continue;
				}
				out += line.form + eol_;
			}
			return;
		}
		emit_noted(record, *noted, out);
	}

private:
	// A unit of the record's output: the lines before its anchor (read for nothing, kept, or of what is gone)
	// and its anchor, a writer's line (an entry generated over its file line, or put down anew; a nested record).
	struct Unit {
		std::vector<size_t> before; // the file's lines emitted as their tokens
		int out_line = -1;          // the writer's line it is (-1: the record's trailing lines alone)
		int file_line = -1;         // the file's line it is generated over (-1: put down anew)
	};

	void emit_noted(const OutRecord &record, const NotedRecord &noted, std::string &out) {
		const std::vector<int> entries = pair_entries(noted, record);
		// The nested records paired with the places they began in the file, by their notes.
		std::vector<int> paired = entries;
		std::vector<int> owner(noted.lines.size(), -1);
		for (size_t i = 0; i < paired.size(); ++i)
			if (paired[i] >= 0) owner[size_t(paired[i])] = int(i);
		for (size_t i = 0; i < record.lines.size(); ++i) {
			const OutLine &line = record.lines[i];
			if (line.child < 0 || size_t(line.child) >= record.children.size()) continue;
			const uint64_t note = record.children[size_t(line.child)].note;
			if (!note) continue;
			for (size_t c = 0; c < noted.lines.size(); ++c)
				if (noted.lines[c].role == Role::Child && noted.lines[c].child == note && owner[c] < 0) {
					paired[i] = int(c);
					owner[c] = int(i);
					break;
				}
		}

		// The file's units in its order: each paired line with the lines before it since the last; a line of what
		// is gone (an entry the writer puts down no more, a record it no longer has) is dropped, the lines before
		// it going to the next unit; a line kept as its tokens stays as one read for nothing.
		std::vector<Unit> units;
		Unit current;
		for (size_t c = 0; c < noted.lines.size(); ++c) {
			const NotedLine &line = noted.lines[c];
			if (owner[c] >= 0) {
				current.out_line = owner[c];
				current.file_line = int(c);
				units.push_back(current);
				current = Unit();
				continue;
			}
			if (line.role == Role::Free || (line.role == Role::Entry && !line.shape.modeled))
				current.before.push_back(c);
		}
		const Unit trailing = current;

		// The nested records of a kind take the file's places of their kind in the writer's order.
		std::map<std::string, std::vector<size_t>> places;      // kind -> the units of its records, the file's order
		std::map<std::string, std::vector<size_t>> by_writer;   // kind -> those units in the writer's order
		for (size_t u = 0; u < units.size(); ++u) {
			const OutLine &line = record.lines[size_t(units[u].out_line)];
			if (line.child >= 0) places[record.children[size_t(line.child)].kind].push_back(u);
		}
		std::vector<int> unit_of(record.lines.size(), -1);
		for (size_t u = 0; u < units.size(); ++u) unit_of[size_t(units[u].out_line)] = int(u);
		for (size_t i = 0; i < record.lines.size(); ++i)
			if (record.lines[i].child >= 0 && unit_of[i] >= 0)
				by_writer[record.children[size_t(record.lines[i].child)].kind].push_back(size_t(unit_of[i]));
		std::vector<Unit> placed = units;
		for (const auto &[kind, slots] : places) {
			const std::vector<size_t> &order = by_writer[kind];
			for (size_t k = 0; k < slots.size() && k < order.size(); ++k) placed[slots[k]] = units[order[k]];
		}

		// The writer's lines the file has none of, each after the line before it in the writer's order (the first
		// before the file's first unit's line, after the lines before it).
		std::vector<std::vector<int>> after(placed.size() + 1); // after[u + 1]: put down after unit u; after[0]: first
		std::vector<int> where(record.lines.size(), -1);       // the unit a writer's line stands in or after
		for (size_t u = 0; u < placed.size(); ++u) where[size_t(placed[u].out_line)] = int(u);
		int last = -1;
		for (size_t i = 0; i < record.lines.size(); ++i) {
			if (where[i] >= 0) {
				last = where[i];
				continue;
			}
			// An entry the writer leaves out as read stays out, and its own separators (an entry of no key) are
			// its form's alone.
			const OutLine &line = record.lines[i];
			if (line.child < 0 && (line.entry.empty() || (paired[i] < 0 && left_out_as_read(noted, line)))) continue;
			after[size_t(last + 1)].push_back(int(i));
			where[i] = last;
		}

		const auto put_anew = [&](int i) {
			const OutLine &line = record.lines[size_t(i)];
			if (line.child >= 0) {
				if (size_t(line.child) < record.children.size()) emit(record.children[size_t(line.child)], out);
				return;
			}
			out += line.form + eol_;
		};
		const auto put_unit = [&](const Unit &unit, bool first) {
			for (size_t c : unit.before) out += noted.lines[c].line.text();
			if (first)
				for (int i : after[0]) put_anew(i);
			if (unit.out_line < 0) return;
			const OutLine &line = record.lines[size_t(unit.out_line)];
			if (line.child >= 0) {
				if (size_t(line.child) < record.children.size()) emit(record.children[size_t(line.child)], out);
				return;
			}
			out += generate(noted.lines[size_t(unit.file_line)], words_of(cut_, line.form));
		};
		for (size_t u = 0; u < placed.size(); ++u) {
			put_unit(placed[u], u == 0);
			for (int i : after[u + 1]) put_anew(i);
		}
		if (placed.empty()) {
			put_unit(trailing, true);
			return;
		}
		put_unit(trailing, false);
	}

	static bool left_out_as_read(const NotedRecord &noted, const OutLine &line) {
		for (const auto &[entry, form] : noted.left_out)
			if (entry == line.entry && form == line.form) return true;
		return false;
	}

	const Notes *notes_;
	Cut cut_;
	std::string eol_;
	std::set<uint64_t> emitted_;
};

void model_record(Notes &notes, const OutRecord &out, Cut cut, std::set<uint64_t> &seen) {
	for (const OutRecord &child : out.children) model_record(notes, child, cut, seen);
	if (!out.note || uint32_t(out.note >> 32) != notes.stamp || !seen.insert(out.note).second) return;
	const size_t index = size_t(uint32_t(out.note)) - 1;
	if (index >= notes.records.size()) return;
	NotedRecord &record = notes.records[index];
	const std::vector<int> paired = pair_entries(record, out);
	for (size_t i = 0; i < out.lines.size(); ++i) {
		const OutLine &line = out.lines[i];
		if (line.child >= 0 || line.entry.empty()) continue;
		if (paired[i] < 0) {
			record.left_out.emplace_back(line.entry, line.form);
			continue;
		}
		NotedLine &file = record.lines[size_t(paired[i])];
		file.shape = model_line(file.line.words, words_of(cut, line.form), line.set_from);
		file.line.words.clear();
	}
}

} // namespace

std::string Line::text() const {
	std::string out = indent;
	for (size_t i = 0; i < words.size(); ++i) {
		out += words[i];
		if (i < gaps.size()) out += gaps[i];
	}
	return out + tail + eol;
}

Line cut_ascii_walk(const char *text, size_t length) {
	Line out;
	size_t end = length;
	if (end >= 2 && text[end - 2] == '\r' && text[end - 1] == '\n') end -= 2;
	out.eol.assign(text + end, length - end);
	size_t at = 0;
	while (at < end && blank(text[at])) ++at;
	out.indent.assign(text, at);
	bool quoted = false;
	size_t word = SIZE_MAX, comment = end;
	std::string gap;
	for (size_t i = at; i < end; ++i) {
		const char c = text[i];
		if (!quoted && (c == ';' || (c == '/' && i + 1 < end && text[i + 1] == '/'))) {
			comment = i;
			break;
		}
		if (c == '"') quoted = !quoted;
		if (!quoted && separator(c)) {
			if (word != SIZE_MAX) {
				out.words.emplace_back(text + word, i - word);
				word = SIZE_MAX;
			}
			gap += c;
			continue;
		}
		if (word == SIZE_MAX) {
			// What stands before the first word past its blanks (a comma) is the indent's.
			if (!out.words.empty()) out.gaps.push_back(gap);
			else out.indent += gap;
			gap.clear();
			word = i;
		}
	}
	if (word != SIZE_MAX) {
		out.words.emplace_back(text + word, comment - word);
		gap.clear();
	}
	if (out.words.empty()) {
		out.indent += gap;
		gap.clear();
	}
	out.tail = gap + std::string(text + comment, end - comment);
	return out;
}

uint64_t Notes::root() const { return records.empty() ? 0 : note_of(stamp, 0); }

const NotedRecord *Notes::record(uint64_t note) const {
	if (!note || uint32_t(note >> 32) != stamp) return nullptr;
	const size_t index = size_t(uint32_t(note)) - 1;
	return index < records.size() ? &records[index] : nullptr;
}

uint32_t next_stamp() {
	uint32_t stamp = ++g_stamp;
	if (!stamp) stamp = ++g_stamp;
	return stamp;
}

std::string file_eol(const Notes &notes, const std::string &fallback) {
	for (const NotedRecord &record : notes.records)
		for (const NotedLine &line : record.lines)
			if (!line.line.eol.empty()) return line.line.eol;
	return fallback;
}

// --- the recorder ---------------------------------------------------------------------------------------

Noter::Noter(const char *text, size_t size, Notes *notes, Cut cut) : text_(text), size_(size), notes_(notes), cut_(cut) {
	if (!notes_) return;
	*notes_ = Notes();
	notes_->stamp = next_stamp();
	notes_->records.emplace_back();
	open_.push_back(notes_->root());
}

uint64_t Noter::root() const { return notes_ ? notes_->root() : 0; }

void Noter::flush() {
	if (!pending_) return;
	pending_ = false;
	const size_t index = size_t(uint32_t(target_)) - 1;
	if (index >= notes_->records.size()) return;
	line_.line = cut_(text_ + begin_, next_ - begin_);
	notes_->records[index].lines.push_back(std::move(line_));
	line_ = NotedLine();
}

void Noter::line(size_t begin, size_t next) {
	if (!notes_) return;
	flush();
	begin_ = std::min(begin, size_);
	next_ = std::min(std::max(next, begin_), size_);
	pending_ = true;
	target_ = open_.empty() ? notes_->root() : open_.back();
	line_ = NotedLine();
}

uint64_t Noter::open(uint64_t parent) {
	if (!notes_) return 0;
	const size_t parent_index = size_t(uint32_t(parent)) - 1;
	if (!parent || parent_index >= notes_->records.size()) return 0;
	notes_->records.emplace_back();
	const uint64_t note = note_of(notes_->stamp, notes_->records.size() - 1);
	notes_->records.back().parent = parent;
	NotedLine place;
	place.role = Role::Child;
	place.child = note;
	notes_->records[parent_index].lines.push_back(std::move(place));
	open_.push_back(note);
	target_ = note;
	return note;
}

void Noter::entry(uint64_t record, const std::string &key) {
	if (!notes_ || !pending_ || !record) return;
	line_.role = Role::Entry;
	line_.entry = key;
	target_ = record;
}

void Noter::close(uint64_t record) {
	if (!notes_) return;
	const auto at = std::find(open_.begin(), open_.end(), record);
	if (at == open_.end() || at == open_.begin()) return; // the file's own record stays open
	open_.erase(at, open_.end());
}

void Noter::end_before(uint64_t record) {
	if (!notes_) return;
	const auto at = std::find(open_.begin(), open_.end(), record);
	if (at == open_.end() || at == open_.begin()) return;
	const uint64_t parent = *(at - 1);
	open_.erase(at, open_.end());
	const size_t index = size_t(uint32_t(record)) - 1, parent_index = size_t(uint32_t(parent)) - 1;
	if (index >= notes_->records.size() || parent_index >= notes_->records.size()) return;
	std::vector<NotedLine> &lines = notes_->records[index].lines;
	size_t keep = lines.size();
	while (keep > 0 && lines[keep - 1].role == Role::Free) --keep;
	std::vector<NotedLine> &into = notes_->records[parent_index].lines;
	for (size_t i = keep; i < lines.size(); ++i) into.push_back(std::move(lines[i]));
	lines.resize(keep);
	if (target_ == record) target_ = parent;
}

void Noter::finish() {
	if (!notes_) return;
	flush();
}

// --- modeled and generated ------------------------------------------------------------------------------

void model(Notes &notes, const OutRecord &as_read, Cut cut) {
	// A line of an entry the writer put down none of as read stays unmodeled: kept as its tokens.
	std::set<uint64_t> seen;
	model_record(notes, as_read, cut, seen);
}

std::string compose(const Notes *notes, const OutRecord &now, Cut cut, const std::string &eol) {
	std::string out;
	Composer(notes, cut, eol).emit(now, out);
	return out;
}

std::string form_of(const OutRecord &record, const std::string &eol) { return compose(nullptr, record, cut_ascii_walk, eol); }

void collect_notes(const OutRecord &record, std::vector<uint64_t> &out) {
	if (record.note) out.push_back(record.note);
	for (const OutRecord &child : record.children) collect_notes(child, out);
}

} // namespace opennova::textlayout
