// A def file's notes, its modeled layout (def_notes.h): its lines cut into their parts, what the family
// parsers made of each, and the record each belongs to (def_compose.cpp models each record's lines).
#include "def_notes.h"

#include <atomic>

namespace opennova::def {
namespace {

bool blank(char c) { return c == ' ' || c == '\t'; }
// What ends a word outside quotes: a blank, a comma [orig: Terrain_TokenizeConfigLine @ 0x53CB60, the
// delimiters @ 0x53CC33..0x53CC4C], and a line break a file split at CR LF alone keeps inside a line.
bool separator(char c) { return c == ' ' || c == '\t' || c == ',' || c == '\r' || c == '\n'; }

std::atomic<uint32_t> g_stamp{0};

} // namespace

std::string DefNotedLine::text() const {
	std::string out = indent;
	for (size_t i = 0; i < words.size(); ++i) {
		out += words[i];
		if (i < gaps.size()) out += gaps[i];
	}
	return out + tail + eol;
}

DefNotedLine def_noted_line(const char *text, size_t length) {
	DefNotedLine out;
	size_t end = length;
	if (end >= 2 && text[end - 2] == '\r' && text[end - 1] == '\n') end -= 2;
	else if (end >= 1 && text[end - 1] == '\n') end -= 1;
	out.eol.assign(text + end, length - end);
	size_t at = 0;
	while (at < end && blank(text[at])) ++at;
	out.indent.assign(text, at);
	// Words up to the comment: a `//` or a `;` outside quotes (trim_def_line's cut [orig:
	// Terrain_TokenizeConfigLine @ 0x53CB60, the break on `//` / `;` gated on !inQuote]).
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
	// The blanks after the last word go with the comment (with no word, the indent's).
	if (out.words.empty()) {
		out.indent += gap;
		gap.clear();
	}
	out.tail = gap + std::string(text + comment, end - comment);
	return out;
}

const std::string *DefNotedBaseline::step(uint8_t of) const {
	for (const auto &[at, text] : steps)
		if (at == of) return &text;
	return nullptr;
}

const DefNotedRecord *DefTextNotes::record(uint64_t note, DefRecordKind kind) const {
	if (!note || uint32_t(note >> 32) != stamp) return nullptr;
	const size_t index = size_t(uint32_t(note)) - 1;
	if (index >= records.size() || records[index].kind != kind) return nullptr;
	return &records[index];
}

void def_clear_notes(DefRecordKind kind, void *record) {
	switch (kind) {
	case DefRecordKind::Item: {
		auto &item = *static_cast<DefItemDef *>(record);
		item.note = 0;
		for (size_t i = 0; i < item.emplacement_attachments_count; ++i) item.emplacement_attachments[i].note = 0;
		break;
	}
	case DefRecordKind::Weapon: {
		auto &weapon = *static_cast<DefWeaponDef *>(record);
		weapon.note = 0;
		for (size_t i = 0; i < weapon.actions_count; ++i) weapon.actions[i].note = 0;
		for (size_t i = 0; i < weapon.sights_count; ++i) weapon.sights[i].note = 0;
		break;
	}
	case DefRecordKind::Ammo: {
		auto &ammo = *static_cast<DefAmmoDef *>(record);
		ammo.note = 0;
		for (size_t i = 0; i < ammo.effects_table_count; ++i) ammo.effects_table[i].note = 0;
		break;
	}
	case DefRecordKind::Action: static_cast<DefWeaponAction *>(record)->note = 0; break;
	case DefRecordKind::Sight: static_cast<DefSightEntry *>(record)->note = 0; break;
	case DefRecordKind::Attachment: static_cast<DefItemEmplacementAttachment *>(record)->note = 0; break;
	case DefRecordKind::Effect: static_cast<DefEffectTableEntry *>(record)->note = 0; break;
	case DefRecordKind::Carry: static_cast<DefAmmoClassCarry *>(record)->note = 0; break;
	case DefRecordKind::Powerup: {
		auto &row = *static_cast<DefPowerupDef *>(record);
		row.note = 0;
		row.pickup.note = 0;
		row.respawn.note = 0;
		for (size_t i = 0; i < row.ammo_count; ++i) row.ammo[i].note = 0;
		break;
	}
	case DefRecordKind::PowerupAmmo: static_cast<DefPowerupAmmo *>(record)->note = 0; break;
	case DefRecordKind::PowerupAction: static_cast<DefPowerupAction *>(record)->note = 0; break;
	}
}

// --- the noter -------------------------------------------------------------------------------------------

DefTextNoter::DefTextNoter(const char *text, size_t size, DefTextNotes *notes) : text_(text), size_(size), notes_(notes) {
	if (notes_) {
		*notes_ = DefTextNotes();
		notes_->stamp = ++g_stamp;
		if (!notes_->stamp) notes_->stamp = ++g_stamp; // never 0: a note of 0 names none
	}
}

DefNotedRecord &DefTextNoter::record_at(uint64_t note) { return notes_->records[size_t(uint32_t(note)) - 1]; }

void DefTextNoter::line(const char *at) {
	if (!notes_) return;
	const size_t offset = size_t(at - text_);
	if (begin_ != SIZE_MAX) flush_line(offset);
	begin_ = offset;
	role_ = DefNotedRole::Free;
	step_ = 0;
	target_ = 0;
	parent_ = 0;
	closes_ = false;
}

uint64_t DefTextNoter::open(DefRecordKind kind, bool header, uint8_t step) {
	if (!notes_) return 0;
	DefNotedRecord record;
	record.kind = kind;
	notes_->records.push_back(std::move(record));
	const uint64_t note = def_note_of(notes_->stamp, notes_->records.size() - 1);
	role_ = header ? DefNotedRole::Header : DefNotedRole::Line;
	step_ = step;
	target_ = note;
	parent_ = 0;
	closes_ = !header;
	open_.push_back(note);
	return note;
}

uint64_t DefTextNoter::open_nested(DefRecordKind kind, uint8_t step, bool header, uint8_t row_step) {
	if (!notes_) return 0;
	const uint64_t parent = open_.empty() ? 0 : open_.back();
	const uint64_t note = open(kind, header, row_step);
	parent_ = parent;
	parent_step_ = step;
	return note;
}

void DefTextNoter::property(int step) {
	if (!notes_ || open_.empty()) return;
	if (step < 0) return; // read for nothing: a free line, the record's if one comes after it
	role_ = DefNotedRole::Line;
	step_ = uint8_t(step);
	target_ = open_.back();
}

void DefTextNoter::close() {
	if (!notes_ || open_.empty()) return;
	role_ = DefNotedRole::End;
	target_ = open_.back();
	closes_ = true;
}

void DefTextNoter::flush_line(size_t end) {
	DefNotedLine line = def_noted_line(text_ + begin_, end - begin_);
	if (role_ == DefNotedRole::Free || !target_) {
		pending_.push_back(std::move(line));
		return;
	}
	line.role = role_;
	line.step = step_;
	DefNotedRecord &record = record_at(target_);
	const bool opens = record.lines.empty() && (role_ == DefNotedRole::Header || (role_ == DefNotedRole::Line && closes_));
	if (opens) {
		if (parent_) {
			// Where it began in its parent; the lines before it are its own.
			DefNotedLine at;
			at.role = DefNotedRole::Nested;
			at.step = parent_step_;
			at.nested = target_;
			record_at(parent_).lines.push_back(std::move(at));
		} else if (!any_record_) {
			// The lines before the file's first record are the file's.
			notes_->leading = std::move(pending_);
			pending_.clear();
		}
		any_record_ = true;
	}
	for (DefNotedLine &free : pending_) record.lines.push_back(std::move(free));
	pending_.clear();
	record.lines.push_back(std::move(line));
	if (closes_) {
		for (size_t i = open_.size(); i-- > 0;)
			if (open_[i] == target_) {
				open_.erase(open_.begin() + std::ptrdiff_t(i));
				break;
			}
	}
}

void DefTextNoter::drop_nested(uint64_t note) {
	if (!notes_ || !note) return;
	for (DefNotedRecord &parent : notes_->records)
		for (size_t i = 0; i < parent.lines.size(); ++i) {
			if (parent.lines[i].role != DefNotedRole::Nested || parent.lines[i].nested != note) continue;
			std::vector<DefNotedLine> lines = std::move(record_at(note).lines);
			record_at(note).lines.clear();
			for (DefNotedLine &line : lines) {
				// Its own lines would read again as a block (its comments and blanks would not).
				line.superseded = line.role != DefNotedRole::Free;
				line.role = DefNotedRole::Free;
				line.step = 0;
				line.nested = 0;
			}
			parent.lines.erase(parent.lines.begin() + std::ptrdiff_t(i));
			parent.lines.insert(parent.lines.begin() + std::ptrdiff_t(i), std::make_move_iterator(lines.begin()),
			                    std::make_move_iterator(lines.end()));
			return;
		}
}

void DefTextNoter::finish() {
	if (!notes_) return;
	if (begin_ != SIZE_MAX && begin_ < size_) flush_line(size_);
	begin_ = SIZE_MAX;
	// A record the file ends inside keeps its lines; what follows its last is the file's.
	if (!any_record_) notes_->leading = std::move(pending_);
	else notes_->trailing = std::move(pending_);
	pending_.clear();
}

} // namespace opennova::def
