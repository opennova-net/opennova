// A def file generated over its modeled layout (def_notes.h; the maintainer's ruling of 2026-10-04: "model
// it, generate it", ADR 0003 holding): every line made from the records and the layout data, in the
// file's order. A record's line is the writer's words for it now in the shape the file had (each in the
// file's spelling while it is the word the writer wrote for the record as read, the tokens the game skips
// where they stood, the writer's words the file left out left out while they are as read), with the
// file's blanks, separators, comment and ending; a line the game reads nothing of is its tokens.
#include "def_write_record.h"

#include <base/io/strutil.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>

namespace opennova::def {
namespace {

std::string lower_word(const std::string &word) { return strutil::to_lower(word); }

// A word without the quotes around it, any case.
std::string bare_word(const std::string &word) {
	std::string out = word;
	if (!out.empty() && out.front() == '"') out.erase(out.begin());
	if (!out.empty() && out.back() == '"') out.pop_back();
	return lower_word(out);
}

bool number_of(const std::string &word, double &out) {
	if (word.empty()) return false;
	char *end = nullptr;
	out = std::strtod(word.c_str(), &end);
	return end == word.c_str() + word.size();
}

// Whether the file's word spells the writer's: the same word in any case, quoted or not, or the same
// number however it is written (`4.50` for `4.5`, `0.0` for `0`).
bool spells(const std::string &file, const std::string &writer) {
	if (bare_word(file) == bare_word(writer)) return true;
	double a = 0.0, b = 0.0;
	return number_of(file, a) && number_of(writer, b) && a == b;
}

std::vector<std::string> words_of(const std::string &text) { return def_noted_line(text.data(), text.size()).words; }

// What tells one line of a step of several lines from another (a weapon's `flags scoped`, `charfilter
// medic`, `classrounds sniper 4`): its key and its first value, any case, quoted or not.
std::string entry_of(const std::vector<std::string> &words) {
	if (words.empty()) return std::string();
	return bare_word(words[0]) + (words.size() > 1 ? " " + bare_word(words[1]) : std::string());
}
std::string key_of(const std::string &entry) { return entry.substr(0, entry.find(' ')); }

// The writer's lines of a step paired with the step's lines of the file (their entries; "" for one never
// paired): each with the writer's line of its entry, the first not taken, then each left with the next
// writer's line of its key; a step of one line each, the one with the other. -1: none.
std::vector<int> pair_lines(const std::vector<std::string> &entries, const std::vector<std::string> &lines) {
	std::vector<int> out(entries.size(), -1);
	std::vector<bool> used(lines.size(), false);
	std::vector<std::string> keys;
	for (const std::string &line : lines) keys.push_back(entry_of(words_of(line)));
	if (entries.size() == 1 && lines.size() == 1 && !entries[0].empty()) {
		out[0] = 0;
		return out;
	}
	for (size_t i = 0; i < entries.size(); ++i)
		for (size_t c = 0; c < lines.size() && !entries[i].empty(); ++c)
			if (!used[c] && keys[c] == entries[i]) {
				out[i] = int(c);
				used[c] = true;
				break;
			}
	for (size_t i = 0; i < entries.size(); ++i)
		for (size_t c = 0; c < lines.size() && out[i] < 0 && !entries[i].empty(); ++c)
			if (!used[c] && key_of(keys[c]) == key_of(entries[i])) {
				out[i] = int(c);
				used[c] = true;
			}
	return out;
}

// A line of the file's modeled against the writer's words for it as read: each of its words one of the
// writer's in its spelling (the same count: word for word; else each the next writer's word it spells, the
// writer's words passed over left out) or a token the game skips.
DefNotedShape model_line(const std::vector<std::string> &file, const std::vector<std::string> &writer) {
	DefNotedShape shape;
	shape.modeled = true;
	shape.entry = entry_of(writer);
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

// A line's words joined by the file's separators (each where it stood, a blank where the line has more
// words than the file's had), between its blanks, its comment and its ending.
std::string joined(const DefNotedLine &line, const std::vector<std::string> &words) {
	std::string out = line.indent;
	for (size_t i = 0; i < words.size(); ++i) {
		out += words[i];
		if (i + 1 < words.size()) out += i < line.gaps.size() ? line.gaps[i] : std::string(" ");
	}
	// A line of no words keeps nothing of its blanks but its ending.
	return out + (words.empty() ? std::string() : line.tail) + line.eol;
}

// A modeled line generated from the writer's line now (`now`): its words in the file's order, each of the
// writer's in the file's spelling while it is the word as read, else as the writer puts it down; the
// tokens the game skips where they stood; a word of the writer's the file left out, left out while it is
// as read, else put down after the words before it.
std::string generate(const DefNotedLine &line, const std::string &now) {
	const std::vector<std::string> writer = words_of(now);
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
	for (const DefNotedWord &word : line.shape.words) {
		if (!word.written) {
			out.push_back(word.spelling);
			continue;
		}
		if (word.index >= writer.size() || placed[word.index]) continue; // the writer puts it down no more
		put_before(word.index);
		out.push_back(writer[word.index] == word.as_written ? word.spelling : writer[word.index]);
		placed[word.index] = true;
	}
	put_before(writer.size());
	return joined(line, out);
}

bool attrib_step(DefRecordKind kind, uint8_t step) {
	const std::vector<DefProperty> &properties = def_properties(kind);
	return step < properties.size() && properties[step].encoding == DefEncoding::ItemAttrib;
}

// The attribute words of an item's `attrib:` lines (after each key), any case.
std::vector<std::string> attribute_words(const std::vector<std::string> &lines) {
	std::vector<std::string> out;
	for (const std::string &line : lines) {
		const std::vector<std::string> words = words_of(line);
		for (size_t i = 1; i < words.size(); ++i) out.push_back(lower_word(words[i]));
	}
	return out;
}

bool holds(const std::vector<std::string> &list, const std::string &word) {
	return std::find(list.begin(), list.end(), word) != list.end();
}

// An item's `attrib:` line modeled against the attributes the writer puts down for it as read: its key, each
// word an attribute of the item's in the file's spelling, each other a token the game skips (`neutral`,
// `exp1`, which the attrib chain has no arm for).
DefNotedShape model_attributes(const std::vector<std::string> &file, const std::vector<std::string> &had) {
	DefNotedShape shape;
	shape.modeled = true;
	shape.entry = "attrib:";
	for (size_t i = 0; i < file.size(); ++i) {
		const std::string word = lower_word(file[i]);
		if (i == 0) shape.words.push_back({true, 0, "attrib:", file[i]});
		else if (holds(had, word)) shape.words.push_back({true, 1, word, file[i]});
		else shape.words.push_back({false, 0, std::string(), file[i]});
	}
	return shape;
}

class Composer {
public:
	Composer(const DefRecordWriter &writer, const DefTextNotes &notes, std::vector<std::string> *lost)
	    : w_(writer), n_(notes), lost_(lost), owned_(writer.slots.size()) {
		for (size_t i = 0; i < writer.written.size(); ++i)
			if (writer.written[i].slot >= 0) owned_[size_t(writer.written[i].slot)].push_back(i);
		// The file's own line ending, for the lines put down anew: its first line's that has one.
		const auto first_eol = [this](const std::vector<DefNotedLine> &lines) {
			for (const DefNotedLine &line : lines)
				if (!line.eol.empty()) {
					eol_ = line.eol;
					return true;
				}
			return false;
		};
		bool found = first_eol(n_.leading);
		for (size_t i = 0; !found && i < n_.records.size(); ++i) found = first_eol(n_.records[i].lines);
		if (!found) first_eol(n_.trailing);
	}

	std::string run() {
		for (const DefNotedLine &line : n_.leading) put_tokens(line);
		for (size_t slot = 0; slot < w_.slots.size(); ++slot)
			if (w_.slots[slot].parent < 0) record(int(slot));
		for (const DefNotedLine &line : n_.trailing) put_tokens(line);
		return std::move(out_);
	}

private:
	using Written = DefRecordWriter::Written;

	// What the writer put down for one record: its own lines by step (in the order put down), its header,
	// its end, its nested records by the step they stand at, and its steps in the order first put down.
	struct Now {
		std::map<uint8_t, std::vector<size_t>> lines;
		std::map<uint8_t, std::vector<int>> nested;
		std::vector<uint8_t> order;
		std::string header, end;
	};

	std::string text_of(size_t i) const {
		const Written &x = w_.written[i];
		return w_.result.text.substr(x.begin, x.end - x.begin);
	}
	// A line after a last line with no ending: the file's ending first.
	void open_line() {
		if (!out_.empty() && out_.back() != '\n') out_ += eol_;
	}
	// A line the game reads nothing of (a blank, a comment, a line it skips): its tokens.
	void put_tokens(const DefNotedLine &line) {
		open_line();
		out_ += line.text();
	}
	void put_text(const std::string &text) {
		if (text.empty()) return;
		open_line();
		out_ += text;
	}
	// A line of the writer's, with the file's ending, and in a modeled record the blanks its own lines start
	// with (`indent`; null: the writer's).
	void put_own(const std::string &text, const std::string *indent = nullptr) {
		open_line();
		std::string line = text;
		if (indent) {
			size_t blanks = 0;
			while (blanks < line.size() && (line[blanks] == ' ' || line[blanks] == '\t')) ++blanks;
			line = *indent + line.substr(blanks);
		}
		if (line.size() >= 2 && line.compare(line.size() - 2, 2, "\r\n") == 0) out_ += line.substr(0, line.size() - 2) + eol_;
		else out_ += line;
	}

	// Whether a writer's line of `step` is one the file left out as read (DefNotedBaseline::left_out).
	static bool left_out(const DefNotedRecord &noted, uint8_t step, const std::string &text) {
		for (const auto &[at, line] : noted.baseline.left_out)
			if (at == step && line == text) return true;
		return false;
	}

	// A record in the writer's form, its notes none or set aside (`noted`: a record written so as its own
	// form reads back otherwise). The lines it had that the game reads nothing of stand: those before its
	// header where they were, those within it after its header; a block a later one replaced (which would
	// read again) goes. What it does not keep of the file's form is said (`lost_`).
	void canonical(int slot, const DefNotedRecord *noted = nullptr) {
		std::vector<const DefNotedLine *> within;
		size_t skipped = 0;
		std::vector<std::string> replaced;
		if (noted) {
			bool began = false;
			for (const DefNotedLine &line : noted->lines) {
				if (line.role != DefNotedRole::Free) {
					began = true;
					if (line.role == DefNotedRole::Line)
						for (const DefNotedWord &word : line.shape.words) skipped += word.written ? 0 : 1;
					continue;
				}
				if (!began) put_tokens(line);
				else if (!line.superseded) within.push_back(&line);
				else if (!line.words.empty() && bare_word(line.words[0]) == "action" && line.words.size() > 1)
					replaced.push_back(line.words[1]);
			}
		}
		bool header = false;
		for (size_t i : owned_[size_t(slot)]) {
			const Written &x = w_.written[i];
			if (x.role == DefNotedRole::Nested) record(x.nested);
			else put_own(text_of(i));
			if (x.role == DefNotedRole::Header && !header) {
				header = true;
				for (const DefNotedLine *line : within) put_tokens(*line);
			}
		}
		if (!header)
			for (const DefNotedLine *line : within) put_tokens(*line);
		// Said once, of the file's record (a nested record's lines are its record's).
		if (!noted || !lost_ || w_.slots[size_t(slot)].parent >= 0) return;
		std::string name;
		for (size_t i : owned_[size_t(slot)])
			if (w_.written[i].role == DefNotedRole::Header) {
				const std::vector<std::string> words = words_of(text_of(i));
				if (words.size() > 1) name = words[1];
				break;
			}
		std::string what = "its lines' comments, spacing and spellings";
		if (skipped) what += ", " + std::to_string(skipped) + (skipped == 1 ? " word" : " words") + " the game skips on them";
		for (const std::string &block : replaced) what += ", its earlier action " + block + " block, which a later one replaces";
		lost_->push_back(name + ": written in the editor's form (its edit reads back otherwise in its own): " + what +
		                 " are not kept; its comment lines and the lines the game skips are.");
	}

	// A step's lines as the writer puts them down (indented as the record's own lines are), its nested
	// records composed in their place; a line the file left out as read left out.
	void canonical_step(int slot, uint8_t step, const std::string *indent, const DefNotedRecord &noted) {
		for (size_t i : owned_[size_t(slot)]) {
			const Written &x = w_.written[i];
			if (x.step != step || (x.role != DefNotedRole::Line && x.role != DefNotedRole::Nested)) continue;
			if (x.role == DefNotedRole::Nested) record(x.nested);
			else if (!left_out(noted, step, text_of(i))) put_own(text_of(i), indent);
		}
	}

	Now gather(int slot) const {
		Now now;
		std::set<uint8_t> seen;
		for (size_t i : owned_[size_t(slot)]) {
			const Written &x = w_.written[i];
			switch (x.role) {
			case DefNotedRole::Header: now.header += text_of(i); break;
			case DefNotedRole::End: now.end += text_of(i); break;
			case DefNotedRole::Line: now.lines[x.step].push_back(i); break;
			case DefNotedRole::Nested: now.nested[x.step].push_back(x.nested); break;
			case DefNotedRole::Free: break; // the writer's own blank after a record: the layout says what stands there
			}
			if ((x.role == DefNotedRole::Line || x.role == DefNotedRole::Nested) && seen.insert(x.step).second)
				now.order.push_back(x.step);
		}
		return now;
	}

	// The lines a step puts down at each of its lines in the file, and after the last.
	struct Plan {
		std::map<size_t, std::string> at;
		std::vector<std::string> late;
	};

	Plan plan(const DefNotedRecord &noted, DefRecordKind kind, uint8_t step, const std::vector<std::string> &now) {
		std::vector<size_t> mine;
		for (size_t j = 0; j < noted.lines.size(); ++j)
			if (noted.lines[j].role == DefNotedRole::Line && noted.lines[j].step == step) mine.push_back(j);
		if (attrib_step(kind, step)) return attributes(noted, mine, now);
		Plan out;
		const std::string *base = noted.baseline.step(step);
		std::string group;
		for (const std::string &line : now) group += line;
		const bool as_read = (base ? *base : std::string()) == group;
		std::vector<std::string> entries;
		for (size_t j : mine) entries.push_back(noted.lines[j].shape.modeled ? noted.lines[j].shape.entry : std::string());
		const std::vector<int> pairs = pair_lines(entries, now);
		std::vector<bool> used(now.size(), false);
		for (size_t m = 0; m < mine.size(); ++m) {
			const DefNotedLine &line = noted.lines[mine[m]];
			if (!line.shape.modeled) {
				// A line of the record's the writer puts nothing down for (its words another line's as the writer
				// writes it): its tokens while the step is as read.
				out.at[mine[m]] = as_read ? line.text() : std::string();
				continue;
			}
			if (pairs[m] < 0) {
				out.at[mine[m]] = std::string(); // the writer puts it down no more: it goes
				continue;
			}
			used[size_t(pairs[m])] = true;
			out.at[mine[m]] = generate(line, now[size_t(pairs[m])]);
		}
		for (size_t c = 0; c < now.size(); ++c)
			if (!used[c] && !left_out(noted, step, now[c])) out.late.push_back(now[c]);
		return out;
	}

	// An item's attributes: each `attrib:` line of the file put down with its key, each attribute word of an
	// attribute the item has (every word of it, as the file has it), each token the game skips (`neutral`,
	// `exp1`); the attributes it has anew after the last line's words, as many as a line holds
	// (def_attrib_words_per_line), the rest on lines of their own after it.
	Plan attributes(const DefNotedRecord &noted, const std::vector<size_t> &mine, const std::vector<std::string> &now) {
		const std::vector<std::string> has = attribute_words(now);
		std::set<std::string> placed;
		Plan out;
		for (size_t j : mine) {
			for (const DefNotedWord &word : noted.lines[j].shape.words)
				if (word.written && word.index > 0 && holds(has, word.as_written)) placed.insert(word.as_written);
			// An attribute the file's lines left out as read (the Door a door line raises) stays out.
			for (const auto &[at, word] : noted.lines[j].shape.left_out) placed.insert(word);
		}
		std::vector<std::string> fresh;
		for (const std::string &word : has)
			if (placed.insert(word).second) fresh.push_back(word);
		const size_t cap = def_attrib_words_per_line();
		for (size_t j : mine) {
			const DefNotedLine &line = noted.lines[j];
			std::vector<std::string> words;
			std::vector<std::string> gaps;
			for (size_t i = 0; i < line.shape.words.size(); ++i) {
				const DefNotedWord &word = line.shape.words[i];
				if (word.written && word.index > 0 && !holds(has, word.as_written)) continue; // an attribute it no longer has
				if (!words.empty()) gaps.push_back(i > 0 && i - 1 < line.gaps.size() ? line.gaps[i - 1] : std::string(" "));
				words.push_back(word.spelling);
			}
			if (j == mine.back()) {
				while (!fresh.empty() && words.size() < cap + 1) {
					gaps.push_back(" ");
					words.push_back(fresh.front());
					fresh.erase(fresh.begin());
				}
				for (size_t from = 0; from < fresh.size(); from += cap) {
					std::string extra = "attrib:";
					for (size_t k = from; k < fresh.size() && k < from + cap; ++k) extra += " " + fresh[k];
					out.late.push_back(extra + "\r\n");
				}
			}
			DefNotedLine shaped = line;
			shaped.gaps = gaps;
			out.at[j] = joined(shaped, words);
		}
		return out;
	}

	void record(int slot) {
		const DefRecordWriter::Slot &own = w_.slots[size_t(slot)];
		const DefNotedRecord *noted = n_.record(own.note, own.kind);
		if (noted && !claimed_.insert(own.note).second) noted = nullptr; // two records of one note: the first has it
		if (!noted || own.plain) {
			canonical(slot, noted);
			return;
		}
		const Now now = gather(slot);
		const std::vector<DefNotedLine> &lines = noted->lines;
		// The blanks the record's own lines start with, for the lines it is given anew.
		const std::string *indent = nullptr;
		for (const DefNotedLine &line : lines)
			if (line.role == DefNotedRole::Line) {
				indent = &line.indent;
				break;
			}
		// Where each step's lines end in the file; the steps the writer puts down with none there, each after
		// the step before it that has lines there (or the header).
		std::map<uint8_t, size_t> last, last_nested;
		std::map<uint8_t, size_t> noted_lines;
		for (size_t j = 0; j < lines.size(); ++j) {
			if (lines[j].role != DefNotedRole::Line && lines[j].role != DefNotedRole::Nested) continue;
			last[lines[j].step] = j;
			if (lines[j].role == DefNotedRole::Nested) last_nested[lines[j].step] = j;
			else ++noted_lines[lines[j].step];
		}
		std::map<int, std::vector<uint8_t>> after;
		int anchor = -1;
		for (uint8_t step : now.order) {
			if (last.count(step)) anchor = step;
			else after[anchor].push_back(step);
		}
		std::set<int> anchored; // the anchors whose new steps are put down
		const auto put_new = [&](int at) {
			if (!anchored.insert(at).second) return;
			const auto found = after.find(at);
			if (found == after.end()) return;
			for (uint8_t step : found->second) canonical_step(slot, step, indent, *noted);
		};
		// The nested records in the record's order (a Move's, a Duplicate's): at the place in the file of each
		// one the file had, every record before it in the record's order not yet put down, then it; those
		// left after the last.
		std::set<int> nested_done;
		const auto put_nested = [&](int kid) {
			if (nested_done.insert(kid).second) record(kid);
		};
		const auto flush_nested = [&](uint8_t step, size_t upto) {
			const auto found = now.nested.find(step);
			if (found == now.nested.end()) return;
			for (size_t k = 0; k < found->second.size() && k < upto; ++k) put_nested(found->second[k]);
		};
		const auto put_noted_nested = [&](uint8_t step, uint64_t note) {
			const auto found = now.nested.find(step);
			if (found == now.nested.end()) return;
			for (size_t k = 0; k < found->second.size(); ++k) {
				const int kid = found->second[k];
				if (w_.slots[size_t(kid)].note != note || nested_done.count(kid)) continue;
				for (size_t m = 0; m <= k; ++m) put_nested(found->second[m]);
				return;
			}
		};
		std::map<uint8_t, Plan> plans;
		std::map<uint8_t, size_t> lines_seen;
		for (size_t j = 0; j < lines.size(); ++j) {
			const DefNotedLine &line = lines[j];
			switch (line.role) {
			case DefNotedRole::Free: put_tokens(line); break;
			case DefNotedRole::Header:
				if (!now.header.empty()) put_text(line.shape.modeled ? generate(line, now.header) : line.text());
				put_new(-1);
				break;
			case DefNotedRole::End:
				for (const auto &[at, steps] : after) put_new(at);
				for (const auto &[step, kids] : now.nested) flush_nested(step, kids.size());
				if (!now.end.empty()) put_text(line.shape.modeled ? generate(line, now.end) : line.text());
				break;
			case DefNotedRole::Line: {
				const uint8_t step = line.step;
				const size_t seen = ++lines_seen[step];
				// A step's nested records with none in the file stand before its last line there (an effects
				// table's rows before its `end`), else after its only one.
				const auto kids = now.nested.find(step);
				const bool rows_before = kids != now.nested.end() && !last_nested.count(step) && noted_lines[step] >= 2 &&
				                         seen == noted_lines[step];
				if (rows_before) flush_nested(step, kids->second.size());
				if (!plans.count(step)) {
					std::vector<std::string> texts;
					if (const auto found = now.lines.find(step); found != now.lines.end())
						for (size_t i : found->second) texts.push_back(text_of(i));
					plans[step] = plan(*noted, own.kind, step, texts);
				}
				const Plan &p = plans[step];
				if (const auto at = p.at.find(j); at != p.at.end()) put_text(at->second);
				if (j == last[step] || seen == noted_lines[step])
					for (const std::string &late : p.late) put_own(late, &line.indent);
				if (kids != now.nested.end() && !last_nested.count(step) && noted_lines[step] < 2 && j == last[step])
					flush_nested(step, kids->second.size());
				if (j == last[step]) put_new(step);
				break;
			}
			case DefNotedRole::Nested: {
				const uint8_t step = line.step;
				const auto kids = now.nested.find(step);
				if (kids != now.nested.end()) {
					put_noted_nested(step, line.nested);
					if (j == last_nested[step]) flush_nested(step, kids->second.size());
				}
				if (j == last[step]) put_new(step);
				break;
			}
			}
		}
		// A record of no end (a row of one line): what is left after its lines.
		for (const auto &[at, steps] : after) put_new(at);
		for (const auto &[step, kids] : now.nested) flush_nested(step, kids.size());
	}

	const DefRecordWriter &w_;
	const DefTextNotes &n_;
	std::vector<std::string> *lost_;
	std::vector<std::vector<size_t>> owned_;
	std::set<uint64_t> claimed_;
	std::string eol_ = "\r\n";
	std::string out_;
};

} // namespace

std::string def_compose(const DefRecordWriter &writer, const DefTextNotes &notes, std::vector<std::string> *lost) {
	return Composer(writer, notes, lost).run();
}

void def_note_baseline(const DefRecordWriter &writer, DefTextNotes &notes) {
	// What the writer put down for each record as read: its header, its end, its lines by step.
	struct Made {
		std::string header, end;
		std::map<uint8_t, std::vector<std::string>> steps;
	};
	std::vector<Made> made(writer.slots.size());
	for (const DefRecordWriter::Written &x : writer.written) {
		if (x.slot < 0) continue;
		const std::string text = writer.result.text.substr(x.begin, x.end - x.begin);
		Made &base = made[size_t(x.slot)];
		switch (x.role) {
		case DefNotedRole::Header: base.header += text; break;
		case DefNotedRole::End: base.end += text; break;
		case DefNotedRole::Line: base.steps[x.step].push_back(text); break;
		default: break;
		}
	}
	for (size_t slot = 0; slot < writer.slots.size(); ++slot) {
		const DefRecordWriter::Slot &own = writer.slots[slot];
		if (!notes.record(own.note, own.kind)) continue;
		DefNotedRecord &record = notes.records[size_t(uint32_t(own.note)) - 1];
		const Made &base = made[slot];
		record.baseline = DefNotedBaseline();
		for (const auto &[step, texts] : base.steps) {
			std::string group;
			for (const std::string &text : texts) group += text;
			record.baseline.steps.emplace_back(step, group);
		}
		// Each line modeled against the writer's words for it: the header, the end, each step's lines (an
		// item's attributes by the attributes the writer puts down).
		std::map<uint8_t, std::vector<size_t>> by_step;
		for (size_t j = 0; j < record.lines.size(); ++j) {
			DefNotedLine &line = record.lines[j];
			if (line.role == DefNotedRole::Header || line.role == DefNotedRole::End) {
				const std::string &text = line.role == DefNotedRole::Header ? base.header : base.end;
				if (!text.empty()) {
					line.shape = model_line(line.words, words_of(text));
					line.words.clear();
				}
			} else if (line.role == DefNotedRole::Line) {
				by_step[line.step].push_back(j);
			}
		}
		for (const auto &[step, mine] : by_step) {
			const auto found = base.steps.find(step);
			const std::vector<std::string> texts = found == base.steps.end() ? std::vector<std::string>() : found->second;
			if (attrib_step(record.kind, step)) {
				const std::vector<std::string> had = attribute_words(texts);
				std::vector<std::string> present;
				for (size_t j : mine) {
					DefNotedLine &line = record.lines[j];
					for (const std::string &word : line.words) present.push_back(lower_word(word));
					line.shape = model_attributes(line.words, had);
					line.words.clear();
				}
				// The attributes the writer puts down that the file's lines have no word for (the Door a door
				// line raises): left out while the item has them, on the last line's shape.
				for (const std::string &word : had)
					if (!holds(present, word)) record.lines[mine.back()].shape.left_out.emplace_back(0, word);
				continue;
			}
			std::vector<std::string> entries;
			for (size_t j : mine) entries.push_back(entry_of(record.lines[j].words));
			const std::vector<int> pairs = pair_lines(entries, texts);
			std::vector<bool> used(texts.size(), false);
			for (size_t m = 0; m < mine.size(); ++m) {
				if (pairs[m] < 0) continue; // kept as tokens (DefNotedShape::modeled false)
				DefNotedLine &line = record.lines[mine[m]];
				used[size_t(pairs[m])] = true;
				line.shape = model_line(line.words, words_of(texts[size_t(pairs[m])]));
				line.words.clear();
			}
			for (size_t c = 0; c < texts.size(); ++c)
				if (!used[c]) record.baseline.left_out.emplace_back(step, texts[c]);
		}
		// The steps the writer puts down that the file has no line of: left out as read.
		for (const auto &[step, texts] : base.steps)
			if (!by_step.count(step))
				for (const std::string &text : texts) record.baseline.left_out.emplace_back(step, text);
	}
}

} // namespace opennova::def
