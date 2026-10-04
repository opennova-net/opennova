// A def file written over its notes (def_notes.h): the noted lines laid out in the file's order, each kept
// while what the writer puts down for it is what it put down for the record as read (its baseline), the
// writer's line in its place where not, keeping the noted blanks, comment and unchanged words.
#include "def_write_record.h"

#include <base/io/strutil.h>

#include <map>
#include <set>

namespace opennova::def {
namespace {

std::string lower_word(const std::string &word) { return strutil::to_lower(word); }

// What tells one line of a step of several lines from another (a weapon's `flags scoped`, `charfilter
// medic`, `classrounds sniper 4`): its key and its first value, any case.
std::string entry_key(const std::vector<std::string> &words) {
	if (words.empty()) return std::string();
	return lower_word(words[0]) + (words.size() > 1 ? " " + lower_word(words[1]) : std::string());
}

// The lines of a text (each with its ending).
std::vector<std::string> lines_of(const std::string &text) {
	std::vector<std::string> out;
	size_t at = 0;
	while (at < text.size()) {
		const size_t end = text.find('\n', at);
		const size_t stop = end == std::string::npos ? text.size() : end + 1;
		out.push_back(text.substr(at, stop - at));
		at = stop;
	}
	return out;
}

// A noted line given the writer's words (`now`) in place of its own, its blanks, its comment and its
// ending kept, and each word the writer puts down as it put it down for the record as read (`base`) in
// the noted spelling: a changed line differs from the file's by the words that changed.
std::string patched(const DefNotedLine &noted, const std::string &base, const std::string &now) {
	const DefNotedLine fresh = def_noted_line(now.data(), now.size());
	const DefNotedLine before = def_noted_line(base.data(), base.size());
	std::vector<std::string> words = fresh.words;
	if (before.words.size() == words.size() && noted.words.size() == words.size()) {
		for (size_t i = 0; i < words.size(); ++i)
			if (words[i] == before.words[i]) words[i] = noted.words[i];
	} else if (!words.empty() && !noted.words.empty() && strutil::iequals(words[0], noted.words[0])) {
		words[0] = noted.words[0]; // the key as the file spells it
	}
	std::string out = noted.indent;
	for (size_t i = 0; i < words.size(); ++i) {
		out += words[i];
		if (i + 1 < words.size()) out += i < noted.gaps.size() ? noted.gaps[i] : std::string(" ");
	}
	// A line of no words (a noted blank) given words keeps nothing of its blanks but its ending.
	return out + noted.tail + noted.eol;
}

class Composer {
public:
	Composer(const DefRecordWriter &writer, const DefTextNotes &notes) : w_(writer), n_(notes), owned_(writer.slots.size()) {
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
		for (const DefNotedLine &line : n_.leading) put_noted(line);
		for (size_t slot = 0; slot < w_.slots.size(); ++slot)
			if (w_.slots[slot].parent < 0) record(int(slot));
		for (const DefNotedLine &line : n_.trailing) put_noted(line);
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
		std::string group(uint8_t step, const Composer &c) const {
			std::string text;
			const auto found = lines.find(step);
			if (found != lines.end())
				for (size_t i : found->second) text += c.text_of(i);
			return text;
		}
	};

	std::string text_of(size_t i) const {
		const Written &x = w_.written[i];
		return w_.result.text.substr(x.begin, x.end - x.begin);
	}
	// A line after a last line with no ending: the file's ending first.
	void open_line() {
		if (!out_.empty() && out_.back() != '\n') out_ += eol_;
	}
	void put_noted(const DefNotedLine &line) {
		open_line();
		out_ += line.text();
	}
	void put_text(const std::string &text) {
		if (text.empty()) return;
		open_line();
		out_ += text;
	}
	// A line of the writer's, with the file's ending, and in a noted record the blanks its own lines start
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

	void canonical(int slot) {
		for (size_t i : owned_[size_t(slot)]) {
			const Written &x = w_.written[i];
			if (x.role == DefNotedRole::Nested) record(x.nested);
			else put_own(text_of(i));
		}
	}

	// A step's lines as the writer put them down (indented as the record's noted lines are), its nested
	// records composed in their place.
	void canonical_step(int slot, uint8_t step, const std::string *indent) {
		for (size_t i : owned_[size_t(slot)]) {
			const Written &x = w_.written[i];
			if (x.step != step || (x.role != DefNotedRole::Line && x.role != DefNotedRole::Nested)) continue;
			if (x.role == DefNotedRole::Nested) record(x.nested);
			else put_own(text_of(i), indent);
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
			case DefNotedRole::Free: break; // the writer's own blank after a record: the notes say what stands there
			}
			if ((x.role == DefNotedRole::Line || x.role == DefNotedRole::Nested) && seen.insert(x.step).second)
				now.order.push_back(x.step);
		}
		return now;
	}

	// The lines a changed step of several lines puts down at each of its noted lines, and after the last.
	struct Plan {
		std::map<size_t, std::string> at;
		std::vector<std::string> late;
	};

	Plan plan(const DefNotedRecord &noted, DefRecordKind kind, uint8_t step, const std::vector<std::string> &now,
	          const std::string *base) {
		Plan out;
		std::vector<size_t> mine;
		for (size_t j = 0; j < noted.lines.size(); ++j)
			if (noted.lines[j].role == DefNotedRole::Line && noted.lines[j].step == step) mine.push_back(j);
		const std::vector<std::string> before = base ? lines_of(*base) : std::vector<std::string>();
		const std::vector<DefProperty> &properties = def_properties(kind);
		if (step < properties.size() && properties[step].encoding == DefEncoding::ItemAttrib) return attributes(noted, mine, now, before);
		if (mine.size() == 1 && now.size() == 1) {
			out.at[mine[0]] = patched(noted.lines[mine[0]], before.size() == 1 ? before[0] : std::string(), now[0]);
			return out;
		}
		// One entry a line: each noted line keeps its place while the writer puts down a line of its entry
		// (noted as it stands where that line is the one put down for the record as read); a line of an
		// entry the writer no longer puts down goes; one of a new entry follows the last.
		std::vector<bool> used(now.size(), false);
		for (size_t j : mine) {
			const std::string key = entry_key(noted.lines[j].words);
			out.at[j] = std::string();
			for (size_t c = 0; c < now.size(); ++c) {
				if (used[c]) continue;
				const DefNotedLine line = def_noted_line(now[c].data(), now[c].size());
				if (entry_key(line.words) != key) continue;
				used[c] = true;
				std::string was;
				for (const std::string &b : before) {
					const DefNotedLine old = def_noted_line(b.data(), b.size());
					if (entry_key(old.words) == key) {
						was = b;
						break;
					}
				}
				out.at[j] = was == now[c] ? noted.lines[j].text() : patched(noted.lines[j], was, now[c]);
				break;
			}
		}
		for (size_t c = 0; c < now.size(); ++c)
			if (!used[c]) out.late.push_back(now[c]);
		return out;
	}

	// An item's attributes: each noted `attrib:` line keeps its words but those of an attribute the item no
	// longer has (a second of one it has twice goes too), the words the game skips (`neutral`, `exp1`)
	// included; the attributes it has anew follow on the last.
	Plan attributes(const DefNotedRecord &noted, const std::vector<size_t> &mine, const std::vector<std::string> &now,
	                const std::vector<std::string> &before) {
		const auto tokens = [](const std::vector<std::string> &lines) {
			std::vector<std::string> out;
			for (const std::string &line : lines) {
				const DefNotedLine words = def_noted_line(line.data(), line.size());
				for (size_t i = 1; i < words.words.size(); ++i) out.push_back(lower_word(words.words[i]));
			}
			return out;
		};
		const std::vector<std::string> has = tokens(now), had = tokens(before);
		const auto in = [](const std::vector<std::string> &list, const std::string &word) {
			return std::find(list.begin(), list.end(), word) != list.end();
		};
		std::set<std::string> placed;
		Plan out;
		for (size_t j : mine) {
			const DefNotedLine &line = noted.lines[j];
			DefNotedLine kept = line;
			kept.words.clear();
			kept.gaps.clear();
			for (size_t i = 0; i < line.words.size(); ++i) {
				const std::string word = lower_word(line.words[i]);
				const bool attribute = i > 0 && (in(has, word) || in(had, word));
				if (attribute && (!in(has, word) || !placed.insert(word).second)) continue;
				if (!kept.words.empty()) kept.gaps.push_back(i > 0 && i - 1 < line.gaps.size() ? line.gaps[i - 1] : " ");
				kept.words.push_back(line.words[i]);
			}
			if (j == mine.back())
				for (const std::string &word : has)
					if (placed.insert(word).second) {
						kept.gaps.push_back(" ");
						kept.words.push_back(word);
					}
			out.at[j] = kept.text();
		}
		return out;
	}

	void record(int slot) {
		const DefRecordWriter::Slot &own = w_.slots[size_t(slot)];
		const DefNotedRecord *noted = own.plain ? nullptr : n_.record(own.note, own.kind);
		if (noted && !claimed_.insert(own.note).second) noted = nullptr; // a copy: the first record of a note has it
		if (!noted) {
			canonical(slot);
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
		// Where each step's noted lines end; the steps the writer puts down with none noted, each after the
		// step before it that has noted lines (or the header).
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
			for (uint8_t step : found->second) canonical_step(slot, step, indent);
		};
		// The nested records put down: each noted one where its notes put it, a new one (no noted line of this
		// record names it) before the next noted one the writer puts down after it, those left after the last.
		std::set<int> nested_done;
		std::set<uint64_t> nested_noted;
		for (const DefNotedLine &line : lines)
			if (line.role == DefNotedRole::Nested) nested_noted.insert(line.nested);
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
				for (size_t m = 0; m < k; ++m)
					if (!nested_noted.count(w_.slots[size_t(found->second[m])].note)) put_nested(found->second[m]);
				put_nested(kid);
				return;
			}
		};
		std::map<uint8_t, Plan> plans;
		std::set<uint8_t> kept_steps; // steps whose group is as read: their noted lines stand
		std::map<uint8_t, size_t> lines_seen;
		for (size_t j = 0; j < lines.size(); ++j) {
			const DefNotedLine &line = lines[j];
			switch (line.role) {
			case DefNotedRole::Free: put_noted(line); break;
			case DefNotedRole::Header:
				if (now.header == noted->baseline.header) put_noted(line);
				else if (!now.header.empty()) put_text(patched(line, noted->baseline.header, now.header));
				put_new(-1);
				break;
			case DefNotedRole::End:
				for (const auto &[at, steps] : after) put_new(at);
				for (const auto &[step, kids] : now.nested) flush_nested(step, kids.size());
				if (now.end == noted->baseline.end) put_noted(line);
				else if (!now.end.empty()) put_text(patched(line, noted->baseline.end, now.end));
				break;
			case DefNotedRole::Line: {
				const uint8_t step = line.step;
				const size_t seen = ++lines_seen[step];
				// A step's nested records with none noted stand before its last noted line (an effects table's
				// rows before its `end`), else after its only one.
				const auto kids = now.nested.find(step);
				const bool rows_before = kids != now.nested.end() && !last_nested.count(step) && noted_lines[step] >= 2 &&
				                         seen == noted_lines[step];
				if (rows_before) flush_nested(step, kids->second.size());
				// The step's lines as the writer puts them down now, against what it put down for the record as
				// read: the same (none either time included: a line the writer's form has nothing of, such as
				// an `attrib:` line of words the game skips alone), its noted lines stand; else the writer's
				// lines take their places (none: they go).
				if (kept_steps.count(step)) {
					put_noted(line);
				} else if (!plans.count(step)) {
					const std::string group = now.group(step, *this);
					const std::string *base = noted->baseline.step(step);
					if ((base ? *base : std::string()) == group) {
						kept_steps.insert(step);
						put_noted(line);
					} else {
						std::vector<std::string> texts;
						if (const auto found = now.lines.find(step); found != now.lines.end())
							for (size_t i : found->second) texts.push_back(text_of(i));
						plans[step] = plan(*noted, own.kind, step, texts, base);
					}
				}
				if (const auto p = plans.find(step); p != plans.end()) {
					put_text(p->second.at[j]);
					if (j == last[step] || seen == noted_lines[step])
						for (const std::string &late : p->second.late) put_own(late, &line.indent);
				}
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
	std::vector<std::vector<size_t>> owned_;
	std::set<uint64_t> claimed_;
	std::string eol_ = "\r\n";
	std::string out_;
};

} // namespace

std::string def_compose(const DefRecordWriter &writer, const DefTextNotes &notes) {
	return Composer(writer, notes).run();
}

void def_note_baseline(const DefRecordWriter &writer, DefTextNotes &notes) {
	std::vector<DefNotedBaseline> made(writer.slots.size());
	for (const DefRecordWriter::Written &x : writer.written) {
		if (x.slot < 0) continue;
		const std::string text = writer.result.text.substr(x.begin, x.end - x.begin);
		DefNotedBaseline &base = made[size_t(x.slot)];
		switch (x.role) {
		case DefNotedRole::Header: base.header += text; break;
		case DefNotedRole::End: base.end += text; break;
		case DefNotedRole::Line: {
			auto found = std::find_if(base.steps.begin(), base.steps.end(), [&](const auto &step) { return step.first == x.step; });
			if (found == base.steps.end()) base.steps.emplace_back(x.step, text);
			else found->second += text;
			break;
		}
		default: break;
		}
	}
	for (size_t slot = 0; slot < writer.slots.size(); ++slot) {
		const DefRecordWriter::Slot &own = writer.slots[slot];
		if (!notes.record(own.note, own.kind)) continue;
		notes.records[size_t(uint32_t(own.note)) - 1].baseline = std::move(made[slot]);
	}
}

} // namespace opennova::def
